/*
 * Copyright 2026 Olivia Ryan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Xbox Game runtime Library
 * GDK Component: System API -> XUser
 */

#include "XUser.h"
#include "DeviceAuth.h"
#include "winhttp.h"
#include <ctype.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/* Same shape as the DeviceAuth.c local helper — the macro isn't exported. */
static inline HRESULT GetJsonStringValue( IJsonObject *object, LPCWSTR key, HSTRING *value )
{
    HSTRING_HEADER key_hdr;
    HSTRING key_hstr;
    HRESULT hr;
    if (FAILED( hr = WindowsCreateStringReference( key, wcslen( key ), &key_hdr, &key_hstr ) ))
        return hr;
    return IJsonObject_GetNamedString( object, key_hstr, value );
}

static inline HRESULT GetOptionalJsonStringValue( IJsonObject *object, LPCWSTR key,
                                                   HSTRING *value, BOOLEAN *present )
{
    IJsonValue *json_value = NULL;
    HSTRING_HEADER key_hdr;
    HSTRING key_hstr;
    HRESULT hr;

    if (!object || !key || !value || !present) return E_POINTER;
    *value = NULL;
    *present = FALSE;
    if (FAILED( hr = WindowsCreateStringReference( key, wcslen( key ),
                                                    &key_hdr, &key_hstr ) ))
        return hr;
    if (FAILED( hr = IJsonObject_GetNamedValue( object, key_hstr,
                                                &json_value ) ))
        return hr;

    /* A value with the wrong JSON type is still present.  GetNamedString will
     * fail below and the caller will consequently treat the claim as empty,
     * rather than falling back to legacy allow-all behaviour. */
    *present = TRUE;
    IJsonValue_Release( json_value );
    return IJsonObject_GetNamedString( object, key_hstr, value );
}

static BOOLEAN XblPrivilegeClaimContains( HSTRING claim,
                                          XUserPrivilege requested_privilege )
{
    BOOLEAN found = FALSE;
    const WCHAR *text;
    UINT32 length, i = 0;
    UINT64 requested = (UINT32)requested_privilege;

    if ((INT32)requested_privilege < 0 || !claim) return FALSE;
    text = WindowsGetStringRawBuffer( claim, &length );
    if (!text || !length) return FALSE;

    /* The launcher stores a canonical sequence of unsigned decimal uint32
     * values separated by one ASCII space.  Validate the complete claim before
     * returning a match so a partially malformed claim always fails closed. */
    while (i < length)
    {
        UINT64 value = 0;

        if (text[i] < L'0' || text[i] > L'9') return FALSE;
        do
        {
            UINT32 digit = text[i] - L'0';
            if (value > (0xffffffffULL - digit) / 10) return FALSE;
            value = value * 10 + digit;
            ++i;
        } while (i < length && text[i] >= L'0' && text[i] <= L'9');

        if (value == requested) found = TRUE;
        if (i == length) break;
        if (text[i] != L' ' || ++i == length) return FALSE;
    }
    return found;
}

static time_t HSTRINGToEpoch( HSTRING value )
{
    unsigned long long seconds;
    UINT32 length;
    LPSTR text, end;

    if (!value || !WindowsGetStringLen( value ) ||
        FAILED( HSTRINGToMultiByte( value, &text, &length ) ))
        return 0;
    seconds = strtoull( text, &end, 10 );
    if (end == text || *end || !seconds || seconds > 0x7fffffffffffffffULL)
        seconds = 0;
    free( text );
    return (time_t)seconds;
}

static const struct IXUserImplVtbl x_user_vtbl;
static const struct IXUserGamertagVtbl x_user_gt_vtbl;

struct change_registration
{
    struct change_registration *next;
    UINT64 token;
};

static SRWLOCK change_registration_lock = SRWLOCK_INIT;
static struct change_registration *change_registrations;
static LONG64 next_change_token;

/* Track last signed-in user for FindUserByLocalId/ById */
static struct x_user *g_signed_in_user;

static struct x_user *signed_in_user_snapshot( void )
{
    return InterlockedCompareExchangePointer(
        (void *volatile *)&g_signed_in_user, NULL, NULL );
}

static HRESULT LoadDefaultUser( XUserHandle *user, LPCSTR client_id )
{
    struct x_user *impl;
    LSTATUS status;
    LPSTR buffer;
    HRESULT hr;
    DWORD size;
    BOOLEAN preauth_done = FALSE;

    if (!user || !client_id) return E_POINTER;

    if (ERROR_SUCCESS != (status = RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "Software\\Wine\\WineGDK",
        "RefreshToken",
        RRF_RT_REG_SZ,
        NULL,
        NULL,
        &size
    ))) return HRESULT_FROM_WIN32( status );

    if (!(buffer = calloc( 1, size ))) return E_OUTOFMEMORY;

    if (ERROR_SUCCESS != (status = RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "Software\\Wine\\WineGDK",
        "RefreshToken",
        RRF_RT_REG_SZ,
        NULL,
        buffer,
        &size
    )))
    {
        free( buffer );
        return HRESULT_FROM_WIN32( status );
    }

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        free( buffer );
        return E_OUTOFMEMORY;
    }

    impl->IXUserImpl_iface.lpVtbl = &x_user_vtbl;
    impl->IXUserGamertag_iface.lpVtbl = &x_user_gt_vtbl;
    impl->ref = 1;

    hr = RefreshOAuth( client_id, buffer, &impl->oauth_token_expiry, &impl->refresh_token, &impl->oauth_token );
    free( buffer );
    if (FAILED( hr ))
    {
        TRACE( "failed to get oauth token\n" );
        IXUserImpl_Release( &impl->IXUserImpl_iface );
        return hr;
    }

    /* Initialize device auth (generates EC key pair, gets device token) */
    {
        UINT32 oauth_len;
        LPSTR oauth_str;
        if (SUCCEEDED( HSTRINGToMultiByte( impl->oauth_token, &oauth_str, &oauth_len ) ))
        {
            HRESULT da_hr = DeviceAuth_Initialize( oauth_str );
            free( oauth_str );
            if (FAILED( da_hr ))
                WARN( "DeviceAuth_Initialize failed: 0x%08lx (continuing without device auth)\n", da_hr );
        }
    }

    /* Pre-auth bypass for the rest of the Xbox Live chain. Wine 11.1/GnuTLS
     * gets TCP-RSTed by user.auth + xsts.auth + sisu.xboxlive after the body
     * lands, so the launcher pre-fetches user_token, the http://xboxlive.com
     * XSTS token (which carries xuid/gamertag/agegroup), and the PlayFab-RP
     * SISU token, all into the device.json blob whose Wine path is in
     * $WINEGDK_PREAUTH_DEVICE. If the file has those fields we use them
     * straight and skip RequestUserToken + RequestXstsToken entirely. The
     * Wine path still runs as a fallback for completeness. */
    {
        const char *preauth_path = getenv( "WINEGDK_PREAUTH_DEVICE" );
        if (preauth_path && *preauth_path)
        {
            HANDLE fh = CreateFileA( preauth_path, GENERIC_READ, FILE_SHARE_READ,
                                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
            if (fh != INVALID_HANDLE_VALUE)
            {
                DWORD sz = GetFileSize( fh, NULL ), rd = 0;
                LPSTR buf = (sz > 0 && sz < 65536) ? calloc( 1, sz + 1 ) : NULL;
                if (buf && ReadFile( fh, buf, sz, &rd, NULL ) && rd == sz)
                {
                    IJsonObject *root = NULL;
                    if (SUCCEEDED( ParseJsonObject( buf, sz, &root ) ) && root)
                    {
                        HSTRING utok = NULL, xtok = NULL, stok = NULL;
                        HSTRING gtg = NULL, mgt = NULL, mgs = NULL, umg = NULL;
                        HSTRING xuid_s = NULL, agg_s = NULL;
                        HSTRING privileges_s = NULL;
                        HSTRING uhs_s = NULL, sisu_uhs_s = NULL, sisu_rp_s = NULL;
                        HSTRING mptok = NULL, mp_uhs_s = NULL, mp_rp_s = NULL;
                        HSTRING realmstok = NULL, realms_uhs_s = NULL, realms_rp_s = NULL;
                        HSTRING lictok = NULL, lic_uhs_s = NULL, lic_rp_s = NULL;
                        HSTRING user_expiry_s = NULL, xbl_expiry_s = NULL;
                        HSTRING sisu_expiry_s = NULL, mp_expiry_s = NULL;
                        HSTRING realms_expiry_s = NULL, lic_expiry_s = NULL;
                        BOOLEAN privileges_present = FALSE;
                        HRESULT privileges_hr;
                        (void)GetJsonStringValue( root, L"user_token", &utok );
                        (void)GetJsonStringValue( root, L"xbl_token", &xtok );
                        (void)GetJsonStringValue( root, L"sisu_token", &stok );
                        (void)GetJsonStringValue( root, L"xbl_gamertag", &gtg );
                        (void)GetJsonStringValue( root, L"xbl_modern_gamertag", &mgt );
                        (void)GetJsonStringValue( root, L"xbl_modern_gamertag_suffix", &mgs );
                        (void)GetJsonStringValue( root, L"xbl_unique_modern_gamertag", &umg );
                        (void)GetJsonStringValue( root, L"xbl_xuid", &xuid_s );
                        (void)GetJsonStringValue( root, L"xbl_age_group", &agg_s );
                        (void)GetJsonStringValue( root, L"xbl_uhs", &uhs_s );
                        privileges_hr = GetOptionalJsonStringValue(
                            root, L"xbl_privileges", &privileges_s,
                            &privileges_present );
                        (void)GetJsonStringValue( root, L"sisu_uhs", &sisu_uhs_s );
                        (void)GetJsonStringValue( root, L"sisu_rp", &sisu_rp_s );
                        (void)GetJsonStringValue( root, L"mp_token", &mptok );
                        (void)GetJsonStringValue( root, L"mp_uhs", &mp_uhs_s );
                        (void)GetJsonStringValue( root, L"mp_rp", &mp_rp_s );
                        (void)GetJsonStringValue( root, L"realms_token", &realmstok );
                        (void)GetJsonStringValue( root, L"realms_uhs", &realms_uhs_s );
                        (void)GetJsonStringValue( root, L"realms_rp", &realms_rp_s );
                        (void)GetJsonStringValue( root, L"lic_token", &lictok );
                        (void)GetJsonStringValue( root, L"lic_uhs", &lic_uhs_s );
                        (void)GetJsonStringValue( root, L"lic_rp", &lic_rp_s );
                        (void)GetJsonStringValue( root, L"user_token_expiry_epoch", &user_expiry_s );
                        (void)GetJsonStringValue( root, L"xbl_token_expiry_epoch", &xbl_expiry_s );
                        (void)GetJsonStringValue( root, L"sisu_expiry_epoch", &sisu_expiry_s );
                        (void)GetJsonStringValue( root, L"mp_expiry_epoch", &mp_expiry_s );
                        (void)GetJsonStringValue( root, L"realms_expiry_epoch", &realms_expiry_s );
                        (void)GetJsonStringValue( root, L"lic_expiry_epoch", &lic_expiry_s );
                        if (utok && xtok && xuid_s)
                        {
                            LPSTR mb = NULL; UINT32 ml = 0;
                            WindowsDuplicateString( utok, &impl->user_token );
                            WindowsDuplicateString( xtok, &impl->xsts_token );
                            impl->user_token_expiry = HSTRINGToEpoch( user_expiry_s );
                            impl->xsts_token_expiry = HSTRINGToEpoch( xbl_expiry_s );
                            /* xuid */
                            if (SUCCEEDED( HSTRINGToMultiByte( xuid_s, &mb, &ml ) ) && mb)
                            { impl->xuid = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                            /* gamertag */
                            if (gtg && SUCCEEDED( HSTRINGToMultiByte( gtg, &mb, &ml ) ) && mb)
                            {
                                lstrcpynA( impl->gamertag, mb,
                                           sizeof(impl->gamertag) );
                                /* GDK requires an empty suffix for accounts
                                 * without modern components.  Modern and
                                 * unique-modern fall back to the classic tag. */
                                lstrcpynA( impl->modern_gamertag, mb,
                                           sizeof(impl->modern_gamertag) );
                                lstrcpynA( impl->unique_modern_gamertag, mb,
                                           sizeof(impl->unique_modern_gamertag) );
                                free( mb ); mb = NULL;
                            }
                            if (mgt && SUCCEEDED( HSTRINGToMultiByte( mgt, &mb, &ml ) ) && mb)
                            { lstrcpynA( impl->modern_gamertag, mb, sizeof(impl->modern_gamertag) ); free( mb ); mb = NULL; }
                            if (mgs && SUCCEEDED( HSTRINGToMultiByte( mgs, &mb, &ml ) ) && mb)
                            { lstrcpynA( impl->modern_gamertag_suffix, mb, sizeof(impl->modern_gamertag_suffix) ); free( mb ); mb = NULL; }
                            if (umg && SUCCEEDED( HSTRINGToMultiByte( umg, &mb, &ml ) ) && mb)
                            { lstrcpynA( impl->unique_modern_gamertag, mb, sizeof(impl->unique_modern_gamertag) ); free( mb ); mb = NULL; }
                            /* age group: "Adult" → 3, "Teen" → 2, "Child" → 1 */
                            if (agg_s && SUCCEEDED( HSTRINGToMultiByte( agg_s, &mb, &ml ) ) && mb)
                            {
                                if (!lstrcmpiA( mb, "Adult" )) impl->age_group = XUserAgeGroup_Adult;
                                else if (!lstrcmpiA( mb, "Teen" )) impl->age_group = XUserAgeGroup_Teen;
                                else impl->age_group = XUserAgeGroup_Child;
                                free( mb ); mb = NULL;
                            }
                            impl->xbl_privileges_present = privileges_present;
                            if (SUCCEEDED( privileges_hr ) && privileges_s &&
                                FAILED( WindowsDuplicateString(
                                    privileges_s, &impl->xbl_privileges ) ))
                            {
                                /* Keep presence true and fail closed if the
                                 * optional claim cannot be retained. */
                                WARN( "could not retain the Xbox privilege claim\n" );
                            }
                            /* local_id uses the xboxlive-RP uhs */
                            if (uhs_s && SUCCEEDED( HSTRINGToMultiByte( uhs_s, &mb, &ml ) ) && mb)
                            { impl->local_id.value = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                            else impl->local_id.value = impl->xuid;
                            /* SISU cache for the PlayFab RP */
                            if (stok && sisu_uhs_s && sisu_rp_s)
                            {
                                WindowsDuplicateString( stok, &impl->sisu_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( sisu_uhs_s, &mb, &ml ) ) && mb)
                                { impl->sisu_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( sisu_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->sisu_rp, mb, sizeof(impl->sisu_rp) ); free( mb ); mb = NULL; }
                                impl->sisu_expiry = HSTRINGToEpoch( sisu_expiry_s );
                            }
                            /* SISU cache for the multiplayer RP (external-server join) */
                            if (mptok && mp_uhs_s && mp_rp_s)
                            {
                                WindowsDuplicateString( mptok, &impl->mp_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( mp_uhs_s, &mb, &ml ) ) && mb)
                                { impl->mp_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( mp_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->mp_rp, mb, sizeof(impl->mp_rp) ); free( mb ); mb = NULL; }
                                impl->mp_expiry = HSTRINGToEpoch( mp_expiry_s );
                            }
                            /* SISU cache for the Bedrock Realms RP */
                            if (realmstok && realms_uhs_s && realms_rp_s)
                            {
                                WindowsDuplicateString( realmstok, &impl->realms_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( realms_uhs_s, &mb, &ml ) ) && mb)
                                { impl->realms_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( realms_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->realms_rp, mb, sizeof(impl->realms_rp) ); free( mb ); mb = NULL; }
                                impl->realms_expiry = HSTRINGToEpoch( realms_expiry_s );
                            }
                            /* SISU cache for the marketplace/licensing RP (in-game Store) */
                            if (lictok && lic_uhs_s && lic_rp_s)
                            {
                                WindowsDuplicateString( lictok, &impl->lic_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( lic_uhs_s, &mb, &ml ) ) && mb)
                                { impl->lic_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( lic_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->lic_rp, mb, sizeof(impl->lic_rp) ); free( mb ); mb = NULL; }
                                impl->lic_expiry = HSTRINGToEpoch( lic_expiry_s );
                            }
                            preauth_done = TRUE;
                            ERR( "preauth: loaded user/XSTS credentials — skipping Wine HTTP\n" );
                        }
                        if (utok) WindowsDeleteString( utok );
                        if (xtok) WindowsDeleteString( xtok );
                        if (stok) WindowsDeleteString( stok );
                        if (gtg) WindowsDeleteString( gtg );
                        if (mgt) WindowsDeleteString( mgt );
                        if (mgs) WindowsDeleteString( mgs );
                        if (umg) WindowsDeleteString( umg );
                        if (xuid_s) WindowsDeleteString( xuid_s );
                        if (agg_s) WindowsDeleteString( agg_s );
                        if (privileges_s) WindowsDeleteString( privileges_s );
                        if (uhs_s) WindowsDeleteString( uhs_s );
                        if (sisu_uhs_s) WindowsDeleteString( sisu_uhs_s );
                        if (sisu_rp_s) WindowsDeleteString( sisu_rp_s );
                        if (mptok) WindowsDeleteString( mptok );
                        if (mp_uhs_s) WindowsDeleteString( mp_uhs_s );
                        if (mp_rp_s) WindowsDeleteString( mp_rp_s );
                        if (realmstok) WindowsDeleteString( realmstok );
                        if (realms_uhs_s) WindowsDeleteString( realms_uhs_s );
                        if (realms_rp_s) WindowsDeleteString( realms_rp_s );
                        if (lictok) WindowsDeleteString( lictok );
                        if (lic_uhs_s) WindowsDeleteString( lic_uhs_s );
                        if (lic_rp_s) WindowsDeleteString( lic_rp_s );
                        if (user_expiry_s) WindowsDeleteString( user_expiry_s );
                        if (xbl_expiry_s) WindowsDeleteString( xbl_expiry_s );
                        if (sisu_expiry_s) WindowsDeleteString( sisu_expiry_s );
                        if (mp_expiry_s) WindowsDeleteString( mp_expiry_s );
                        if (realms_expiry_s) WindowsDeleteString( realms_expiry_s );
                        if (lic_expiry_s) WindowsDeleteString( lic_expiry_s );
                        IJsonObject_Release( root );
                    }
                }
                free( buf );
                CloseHandle( fh );
            }
        }
    }

    if (!preauth_done)
    {
        if (FAILED( hr = RequestUserToken( impl->oauth_token, &impl->user_token, &impl->local_id ) ))
        {
            TRACE( "failed to get user token\n" );
            IXUserImpl_Release( &impl->IXUserImpl_iface );
            return hr;
        }

        if (FAILED( hr = RequestXstsToken( impl->user_token, &impl->xsts_token, &impl->xuid, &impl->age_group, impl->gamertag, sizeof(impl->gamertag) ) ))
        {
            TRACE( "failed to get xsts token\n" );
            IXUserImpl_Release( &impl->IXUserImpl_iface );
            return hr;
        }
    }

    *user = (XUserHandle)impl;

    return hr;
}

static inline struct x_user *impl_from_IXUserImpl( IXUserImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl_iface );
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );

    TRACE( "iface %p, iid %s, out %p\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IXUserBase ) ||
        IsEqualGUID( iid, &IID_IXUserAddWithUi ) ||
        IsEqualGUID( iid, &IID_IXUserMsa ) ||
        IsEqualGUID( iid, &IID_IXUserStore ) ||
        IsEqualGUID( iid, &IID_IXUserPlatform ) ||
        IsEqualGUID( iid, &IID_IXUserSignOut ))
    {
        *out = &impl->IXUserImpl_iface;
        IXUserImpl_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertag ))
    {
        *out = &impl->IXUserGamertag_iface;
        IXUserGamertag_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl *iface )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );
    ULONG ref;

    if (impl->is_provider) return 2;
    ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl *iface )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );
    ULONG ref;

    if (impl->is_provider) return 1;
    ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu\n", iface, ref );
    if (!ref)
    {
        WindowsDeleteString( impl->refresh_token );
        WindowsDeleteString( impl->oauth_token );
        WindowsDeleteString( impl->user_token );
        WindowsDeleteString( impl->xsts_token );
        if (impl->xbl_privileges) WindowsDeleteString( impl->xbl_privileges );
        if (impl->sisu_token) WindowsDeleteString( impl->sisu_token );
        if (impl->mp_token) WindowsDeleteString( impl->mp_token );
        if (impl->realms_token) WindowsDeleteString( impl->realms_token );
        if (impl->lic_token) WindowsDeleteString( impl->lic_token );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl *iface, XUserHandle user, XUserHandle *duplicated )
{
    struct x_user *signed_in = signed_in_user_snapshot();

    TRACE( "iface %p, user %p, duplicated %p\n", iface, user, duplicated );
    if (!duplicated) return E_POINTER;
    if (!user)
    {
        /* Game may pass NULL when getting user from composite interface */
        if (signed_in)
        {
            TRACE( "NULL user, returning signed-in user %p\n", signed_in );
            IXUserImpl_AddRef( &signed_in->IXUserImpl_iface );
            *duplicated = (XUserHandle)signed_in;
            return S_OK;
        }
        return E_POINTER;
    }
    IXUserImpl_AddRef( &((struct x_user*)user)->IXUserImpl_iface );
    *duplicated = user;
    return S_OK;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p\n", iface, user );
    if (user) IXUserImpl_Release( &((struct x_user*)user)->IXUserImpl_iface );
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl *iface, XUserHandle user1, XUserHandle user2 )
{
    TRACE( "iface %p, user1 %p, user2 %p\n", iface, user1, user2 );
    if (!user1 || !user2) return 1;
    return ((struct x_user*)user1)->xuid != ((struct x_user*)user2)->xuid;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p\n", iface, maxUsers );
    if (!maxUsers) return E_POINTER;
    *maxUsers = 1;
    return S_OK;
}

struct XUserAddContext
{
    XUserAddOptions options;
    XUserHandle user;
    LPCSTR client_id;
};

static HRESULT CALLBACK XUserAddProvider( XAsyncOp operation, const XAsyncProviderData *providerData )
{
    struct XUserAddContext *context;
    IXThreadingImpl *impl;
    HRESULT hr, result = S_OK;

    TRACE( "operation %d, providerData %p\n", operation, providerData );

    if (!providerData) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    context = providerData->context;

    switch (operation)
    {
        case Begin:
            result = impl->lpVtbl->XAsyncSchedule( impl, providerData->async, 0 );
            break;

        case GetResult:
            memcpy( providerData->buffer, &context->user, sizeof( XUserHandle ) );
            break;

        case DoWork:
            if (context->options & XUserAddOptions_AddDefaultUserAllowingUI)
                hr = LoadDefaultUser( &context->user, context->client_id );
            else if (context->options & XUserAddOptions_AddDefaultUserSilently)
                hr = LoadDefaultUser( &context->user, context->client_id );
            else hr = E_ABORT;

            impl->lpVtbl->XAsyncComplete( impl, providerData->async, hr, sizeof( XUserHandle ) );
            break;

        case Cleanup:
            free( context );
            break;

        case Cancel:
            break;
    }

    impl->lpVtbl->Release( impl );
    return result;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl *iface, XUserAddOptions options, XAsyncBlock *asyncBlock )
{
    struct XUserAddContext *context;
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, options %d, asyncBlock %p, callback %p\n", iface, options, asyncBlock, asyncBlock ? asyncBlock->callback : NULL );

    if (!asyncBlock) return E_POINTER;
    if (FAILED( hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return hr;
    if (!(context = calloc( 1, sizeof( struct XUserAddContext ) )))
    {
        impl->lpVtbl->Release( impl );
        return E_OUTOFMEMORY;
    }

    context->options = options;
    context->client_id = "0000000048183522"; /* MSAAppId matching ProxyPass refresh token */
    hr = impl->lpVtbl->XAsyncBegin( impl, asyncBlock, context, x_user_XUserAddAsync, "XUserAddAsync", XUserAddProvider );
    impl->lpVtbl->Release( impl );
    return hr;
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, XUserHandle *user )
{
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, user %p\n", iface, asyncBlock, user );

    if (!asyncBlock || !user) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_NOTIMPL;
    hr = impl->lpVtbl->XAsyncGetResult( impl, asyncBlock, x_user_XUserAddAsync, sizeof( XUserHandle ), user, NULL );
    impl->lpVtbl->Release( impl );
    TRACE( "XUserAddResult returning hr=0x%08lx, user=%p\n", hr, user ? *user : NULL );

    if (SUCCEEDED( hr ) && *user)
    {
        struct x_user *u = (struct x_user *)*user;

        IXUserImpl_AddRef( &u->IXUserImpl_iface );
        if (InterlockedCompareExchangePointer(
                (void *volatile *)&g_signed_in_user, u, NULL ))
            IXUserImpl_Release( &u->IXUserImpl_iface );
    }

    return hr;
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl *iface, XUserHandle user, XUserLocalId *localId )
{
    TRACE( "iface %p, user %p, localId %p\n", iface, user, localId );
    if (!user || !localId) return E_POINTER;
    *localId = ((struct x_user*)user)->local_id;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl *iface, XUserLocalId localId, XUserHandle *user )
{
    struct x_user *signed_in = signed_in_user_snapshot();

    TRACE( "iface %p, localId %llu, user %p\n", iface, (unsigned long long)localId.value, user );
    if (!user) return E_POINTER;
    if (signed_in && signed_in->local_id.value == localId.value)
    {
        IXUserImpl_AddRef( &signed_in->IXUserImpl_iface );
        *user = (XUserHandle)signed_in;
        return S_OK;
    }
    return E_GAMEUSER_NO_DEFAULT_USER;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl *iface, XUserHandle user, UINT64 *userId )
{
    TRACE( "iface %p, user %p, userId %p\n", iface, user, userId );
    if (!user || !userId) return E_POINTER;
    *userId = ((struct x_user*)user)->xuid;
    TRACE( "returning the signed-in user id\n" );
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl *iface, UINT64 userId, XUserHandle *user )
{
    struct x_user *signed_in = signed_in_user_snapshot();

    TRACE( "iface %p, userId %llu, user %p\n", iface, (unsigned long long)userId, user );
    if (!user) return E_POINTER;
    if (signed_in && signed_in->xuid == userId)
    {
        IXUserImpl_AddRef( &signed_in->IXUserImpl_iface );
        *user = (XUserHandle)signed_in;
        return S_OK;
    }
    return E_GAMEUSER_NO_DEFAULT_USER;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl *iface, XUserHandle user, BOOLEAN *isGuest )
{
    FIXME( "iface %p, user %p, isGuest %p stub!\n", iface, user, isGuest );
    if (!user || !isGuest) return E_POINTER;
    *isGuest = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl *iface, XUserHandle user, XUserState *state )
{
    TRACE( "iface %p, user %p, state %p\n", iface, user, state );
    if (!user || !state) return E_POINTER;
    *state = XUserState_SignedIn;
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXUserImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl *iface, XUserHandle user, XUserGamerPictureSize size, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, size %p, asyncBlock %p stub!\n", iface, user, &size, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    FIXME( "iface %p, asyncBlock %p, size %p stub!\n", iface, asyncBlock, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, PVOID buffer, SIZE_T *used )
{
    FIXME( "iface %p, asyncBlock %p, size %llu, buffer %p, used %p stub!\n",
           iface, asyncBlock, (unsigned long long)size, buffer, used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl *iface, XUserHandle user, XUserAgeGroup *group )
{
    TRACE( "iface %p, user %p, group %p\n", iface, user, group );

    if (!user || !group) return E_POINTER;
    *group = ((struct x_user*)user)->age_group;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    struct x_user *impl = (struct x_user *)user;
    BOOLEAN allowed;

    TRACE( "iface %p, user %p, options %d, privilege %d, hasPrivilege %p, reason %p\n", iface, user, options, privilege, hasPrivilege, reason );
    if (!user || !hasPrivilege) return E_POINTER;
    if ((UINT32)options & ~XUserPrivilegeOptions_AllUsers)
        return E_INVALIDARG;

    /* Caches created before xbl_privileges existed retain the historical
     * permissive behaviour.  Once a claim is present, including an explicit
     * empty claim, its contents are authoritative. */
    allowed = !impl->xbl_privileges_present ||
              XblPrivilegeClaimContains( impl->xbl_privileges, privilege );
    *hasPrivilege = allowed;
    if (reason) *reason = allowed ? XUserPrivilegeDenyReason_None :
                                    XUserPrivilegeDenyReason_Unknown;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, asyncBlock %p stub!\n", iface, user, options, privilege, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

/* Map an outgoing request URL to the XSTS relying party (token audience) the
 * destination service accepts.  Minecraft signs every Xbox Live / commerce
 * request with XUserGetTokenAndSignature; handing back a token minted for the
 * wrong audience makes the service reject it (Friends stay empty, the
 * Marketplace catalog never loads).  Host-based, matching the canonical GDK
 * map:
 *   - Marketplace / entitlements edges -> http://licensing.xboxlive.com
 *   - Friends / Social / Profile / People (*.xboxlive.com) -> http://xboxlive.com
 *   - PlayFab title services -> the PlayFab RP
 *   - Joining an external Bedrock server -> the multiplayer RP
 * Order matters: the licensing hosts end in xboxlive.com, so they are matched
 * before the generic xboxlive.com fallback. */
static BOOLEAN ascii_equal_nocase_n( LPCSTR left, SIZE_T left_len, LPCSTR right )
{
    SIZE_T right_len = strlen( right );

    if (left_len != right_len) return FALSE;
    for (SIZE_T i = 0; i < left_len; ++i)
        if (tolower( (unsigned char)left[i] ) != tolower( (unsigned char)right[i] ))
            return FALSE;
    return TRUE;
}

static BOOLEAN url_host_matches( LPCSTR url, LPCSTR expected, BOOLEAN allow_subdomains )
{
    LPCSTR authority, authority_end, host, host_end, at, colon;
    SIZE_T host_len, expected_len;

    if (!url || !(authority = strstr( url, "://" ))) return FALSE;
    authority += 3;
    authority_end = strpbrk( authority, "/?#" );
    if (!authority_end) authority_end = authority + strlen( authority );
    host = authority;

    /* Credentials are not valid for the GDK endpoints, but excluding them
     * keeps host-based RP selection correct for any syntactically valid URL. */
    at = memchr( authority, '@', authority_end - authority );
    if (at) host = at + 1;

    host_end = authority_end;
    if (host < host_end && *host == '[')
    {
        LPCSTR close = memchr( host, ']', host_end - host );
        if (close) host_end = close + 1;
    }
    else if ((colon = memchr( host, ':', host_end - host )))
    {
        host_end = colon;
    }

    host_len = host_end - host;
    if (ascii_equal_nocase_n( host, host_len, expected )) return TRUE;
    if (!allow_subdomains) return FALSE;

    expected_len = strlen( expected );
    return host_len > expected_len && host[host_len - expected_len - 1] == '.' &&
           ascii_equal_nocase_n( host + host_len - expected_len, expected_len, expected );
}

static LPCSTR resolve_relying_party_for_url( LPCSTR url )
{
    if (!url) return "http://xboxlive.com";

    if (url_host_matches( url, "collections.mp.microsoft.com", FALSE ) ||
        url_host_matches( url, "purchase.mp.microsoft.com", FALSE ) ||
        url_host_matches( url, "displaycatalog.mp.microsoft.com", FALSE ) ||
        url_host_matches( url, "inventory.xboxlive.com", FALSE ) ||
        url_host_matches( url, "licensing.xboxlive.com", FALSE ))
        return "http://licensing.xboxlive.com";

    if (url_host_matches( url, "playfabapi.com", TRUE ))
        return "https://b980a380.minecraft.playfabapi.com/";

    if (url_host_matches( url, "multiplayer.minecraft.net", TRUE ))
        return "https://multiplayer.minecraft.net/";

    if (url_host_matches( url, "pocket.realms.minecraft.net", FALSE ) ||
        url_host_matches( url, "bedrock.frontend.realms.minecraft-services.net", FALSE ) ||
        url_host_matches( url, "bedrock.frontendlegacy.realms.minecraft-services.net", FALSE ))
        return "https://pocket.realms.minecraft.net/";

    /* Friends/Social/Profile and any other Xbox Live edge. */
    return "http://xboxlive.com";
}

#define TOKEN_REQUEST_LOG_LIMIT 64

static LONG token_request_sequence;

static LONG log_token_request( LPCSTR method, LPCSTR url,
                               XUserGetTokenAndSignatureOptions options )
{
    LONG sequence = InterlockedIncrement( &token_request_sequence );
    LPCSTR method_kind = !lstrcmpiA( method, "GET" ) ? "GET" :
                          !lstrcmpiA( method, "POST" ) ? "POST" : "OTHER";

    if (sequence <= TOKEN_REQUEST_LOG_LIMIT)
        TRACE( "native Xbox request auth #%ld queued: method=%s, rp=%s, force_refresh=%u.\n",
               sequence, method_kind, resolve_relying_party_for_url( url ),
               !!(options & XUserGetTokenAndSignatureOptions_ForceRefresh) );
    else if (sequence == TOKEN_REQUEST_LOG_LIMIT + 1)
        TRACE( "native Xbox request auth log limit reached; suppressing later requests.\n" );
    return sequence;
}

static HRESULT duplicate_string( LPCSTR source, LPSTR *destination )
{
    SIZE_T length;

    if (!source || !destination) return E_POINTER;
    *destination = NULL;
    length = strlen( source );
    if (!(*destination = malloc( length + 1 ))) return E_OUTOFMEMORY;
    memcpy( *destination, source, length + 1 );
    return S_OK;
}

static HRESULT utf16_to_utf8( LPCWSTR source, LPSTR *destination )
{
    int length;

    if (!source || !destination) return E_POINTER;
    *destination = NULL;
    if (!(length = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, source, -1,
                                        NULL, 0, NULL, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(*destination = malloc( length ))) return E_OUTOFMEMORY;
    if (!WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, source, -1,
                              *destination, length, NULL, NULL ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( *destination );
        *destination = NULL;
        return hr;
    }
    return S_OK;
}

static HRESULT utf8_to_utf16( LPCSTR source, LPWSTR *destination, SIZE_T *count )
{
    int length;

    if (!source || !destination || !count) return E_POINTER;
    *destination = NULL;
    *count = 0;
    if (!(length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
                                        NULL, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );
    if ((SIZE_T)length > ~(SIZE_T)0 / sizeof(WCHAR))
        return HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW );
    if (!(*destination = malloc( (SIZE_T)length * sizeof(WCHAR) )))
        return E_OUTOFMEMORY;
    if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
                              *destination, length ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( *destination );
        *destination = NULL;
        return hr;
    }
    *count = length;
    return S_OK;
}

static HRESULT request_target_from_url( LPCSTR url, LPSTR *target )
{
    LPCSTR authority, start, end;
    SIZE_T length, prefix = 0;

    if (!url || !target) return E_POINTER;
    *target = NULL;
    if (!(authority = strstr( url, "://" ))) return E_INVALIDARG;
    authority += 3;
    if (!*authority) return E_INVALIDARG;
    start = strpbrk( authority, "/?#" );
    if (!start || *start == '#') return duplicate_string( "/", target );
    if (*start == '?') prefix = 1;
    end = strchr( start, '#' );
    if (!end) end = start + strlen( start );
    length = end - start;
    if (length > ~(SIZE_T)0 - prefix - 1)
        return HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW );
    if (!(*target = malloc( prefix + length + 1 ))) return E_OUTOFMEMORY;
    if (prefix) (*target)[0] = '/';
    memcpy( *target + prefix, start, length );
    (*target)[prefix + length] = 0;
    return S_OK;
}

struct XUserGetTokenAndSignatureContext
{
    BOOLEAN utf16;
    XUserHandle user;
    XUserGetTokenAndSignatureOptions options;
    LPSTR method;
    LPSTR url;
    LPSTR request_target;
    SIZE_T count;
    struct DeviceAuthRequestHeader *headers;
    SIZE_T size;
    BYTE *buffer;
    LPSTR result_token;
    SIZE_T result_token_len;
    LPSTR result_signature;
    SIZE_T result_signature_len;
    LPWSTR result_token_utf16;
    SIZE_T result_token_utf16_count;
    LPWSTR result_signature_utf16;
    SIZE_T result_signature_utf16_count;
    SIZE_T result_size;
    LONG diagnostic_sequence;
};

static void free_token_and_signature_context( struct XUserGetTokenAndSignatureContext *context )
{
    if (!context) return;
    if (context->user)
        IXUserImpl_Release( &((struct x_user *)context->user)->IXUserImpl_iface );
    free( context->method );
    free( context->url );
    free( context->request_target );
    for (SIZE_T i = 0; context->headers && i < context->count; ++i)
    {
        free( (void *)context->headers[i].name );
        free( (void *)context->headers[i].value );
    }
    free( context->headers );
    free( context->buffer );
    free( context->result_token );
    free( context->result_signature );
    free( context->result_token_utf16 );
    free( context->result_signature_utf16 );
    free( context );
}

static HRESULT CALLBACK XUserGetTokenAndSignatureProvider( XAsyncOp operation, const XAsyncProviderData *providerData )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;
    HRESULT result = S_OK;

    TRACE( "operation %d, providerData %p\n", operation, providerData );

    if (!providerData) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    context = providerData->context;

    switch (operation)
    {
        case Begin:
            result = impl->lpVtbl->XAsyncSchedule( impl, providerData->async, 0 );
            break;

        case GetResult:
        {
            if (!context->utf16)
            {
                XUserGetTokenAndSignatureData *data = providerData->buffer;
                LPSTR strings = (LPSTR)(data + 1), signature = NULL;

                memset( data, 0, sizeof(*data) );
                memcpy( strings, context->result_token, context->result_token_len + 1 );
                data->token = strings;
                data->tokenSize = context->result_token_len + 1;
                if (context->result_signature && context->result_signature_len > 0)
                {
                    signature = strings + context->result_token_len + 1;
                    memcpy( signature, context->result_signature,
                            context->result_signature_len + 1 );
                    data->signature = signature;
                    data->signatureSize = context->result_signature_len + 1;
                }
            }
            else
            {
                XUserGetTokenAndSignatureUtf16Data *data = providerData->buffer;
                LPWSTR strings = (LPWSTR)(data + 1), signature = NULL;

                memset( data, 0, sizeof(*data) );
                memcpy( strings, context->result_token_utf16,
                        context->result_token_utf16_count * sizeof(WCHAR) );
                data->token = strings;
                /* The GDK ABI documents these counts as byte sizes, despite
                 * the Count suffix.  Like the ANSI structure, the reported
                 * buffer size includes its terminating NUL. */
                data->tokenCount = context->result_token_utf16_count * sizeof(WCHAR);
                if (context->result_signature_utf16_count)
                {
                    signature = strings + context->result_token_utf16_count;
                    memcpy( signature, context->result_signature_utf16,
                            context->result_signature_utf16_count * sizeof(WCHAR) );
                    data->signature = signature;
                    data->signatureCount = context->result_signature_utf16_count * sizeof(WCHAR);
                }
            }
            break;
        }

        case DoWork:
        {
            struct x_user *user_impl = (struct x_user *)context->user;
            HSTRING xsts_token = NULL;
            UINT32 xsts_len;
            LPSTR xsts_str;
            HRESULT dowork_hr;
            LPCSTR url = context->url;
            LPCSTR rp = "http://xboxlive.com";
            UINT64 token_uhs;
            time_t now;
            BOOLEAN force_refresh = !!(context->options &
                                        XUserGetTokenAndSignatureOptions_ForceRefresh);

            if (!user_impl || !user_impl->user_token)
            {
                WARN( "no user token available\n" );
                impl->lpVtbl->XAsyncComplete( impl, providerData->async, E_FAIL, 0 );
                break;
            }

            /* Determine relying party from the request host (Friends/Social,
             * Marketplace, PlayFab and external-server joins each need a
             * different XSTS audience). */
            rp = resolve_relying_party_for_url( url );

            TRACE( "requesting token: method=%s, rp=%s, headers=%llu, body=%llu bytes\n",
                   context->method, rp, (unsigned long long)context->count,
                   (unsigned long long)context->size );

            /* Try SISU first for PlayFab/multiplayer (title-bound XSTS in a
             * single round-trip with our MSA AppId + device key — the path
             * gophertunnel/ProxyPass use to get past PlayFab's title check).
             * Falls back to the plain user→xsts/authorize exchange if SISU
             * is unavailable (device auth uninitialised, network error,
             * Microsoft returning non-2xx). */
            dowork_hr = E_FAIL;
            /* uhs that goes into the XBL3.0 header.  PlayFab cross-checks
             * it against the uhs claim INSIDE the token: SISU returns a
             * different uhs than the user-only RequestUserToken flow,
             * so when SISU is what minted the token the header must use
             * the SISU one or PlayFab silently rejects → sign-in loops. */
            token_uhs = user_impl->local_id.value;
            xsts_token = NULL;

            if (DeviceAuth_IsInitialized() && user_impl->oauth_token)
            {
                /* Preauth: when MC asks for the http://xboxlive.com RP
                 * (Friends/Social), serve the xbl_token the launcher
                 * pre-fetched and stashed into impl->xsts_token — that
                 * token IS a SISU AuthorizationToken for that exact RP
                 * with the matching uhs in impl->local_id.value. Hitting
                 * SISU from Wine would just TCP-RST against GnuTLS. */
                now = time( NULL );
                if (!force_refresh && user_impl->xsts_token &&
                    user_impl->xsts_token_expiry > now + 30 &&
                    !strcmp( rp, "http://xboxlive.com" ))
                {
                    HSTRING dup = NULL;
                    if (SUCCEEDED( WindowsDuplicateString( user_impl->xsts_token, &dup ) ))
                    {
                        xsts_token = dup;
                        token_uhs = user_impl->local_id.value;
                        dowork_hr = S_OK;
                        TRACE( "reusing preauth xbl_token for %s\n", rp );
                    }
                }
                /* Reuse the pre-minted multiplayer-RP SISU token for a server
                 * join. Without it the live SISU/XSTS call below RSTs under
                 * Wine and the join token comes back empty. */
                if (!force_refresh && FAILED( dowork_hr ) &&
                    user_impl->mp_token && user_impl->mp_expiry > now + 30 &&
                    strcmp( user_impl->mp_rp, rp ) == 0)
                {
                    HSTRING dup = NULL;
                    if (SUCCEEDED( WindowsDuplicateString( user_impl->mp_token, &dup ) ))
                    {
                        xsts_token = dup;
                        token_uhs = user_impl->mp_uhs;
                        dowork_hr = S_OK;
                        TRACE( "reusing preauth multiplayer SISU token for %s\n", rp );
                    }
                }
                /* Realms moved its API traffic to minecraft-services.net,
                 * but still validates the pocket.realms.minecraft.net XSTS
                 * audience.  Reuse the host-minted token because a live SISU
                 * request from Wine is not reliable. */
                if (!force_refresh && FAILED( dowork_hr ) &&
                    user_impl->realms_token && user_impl->realms_expiry > now + 30 &&
                    strcmp( user_impl->realms_rp, rp ) == 0)
                {
                    HSTRING dup = NULL;
                    if (SUCCEEDED( WindowsDuplicateString( user_impl->realms_token, &dup ) ))
                    {
                        xsts_token = dup;
                        token_uhs = user_impl->realms_uhs;
                        dowork_hr = S_OK;
                        TRACE( "reusing preauth Realms SISU token for %s\n", rp );
                    }
                }
                /* Reuse the pre-minted licensing-RP SISU token for the in-game
                 * Marketplace (catalog + entitlement calls). Same rationale as
                 * mp_token: a live SISU call for this RP RSTs under Wine. */
                if (!force_refresh && FAILED( dowork_hr ) &&
                    user_impl->lic_token && user_impl->lic_expiry > now + 30 &&
                    strcmp( user_impl->lic_rp, rp ) == 0)
                {
                    HSTRING dup = NULL;
                    if (SUCCEEDED( WindowsDuplicateString( user_impl->lic_token, &dup ) ))
                    {
                        xsts_token = dup;
                        token_uhs = user_impl->lic_uhs;
                        dowork_hr = S_OK;
                        TRACE( "reusing preauth licensing SISU token for %s\n", rp );
                    }
                }
                /* Reuse the cached SISU token if it's still fresh for
                 * the requested RP — SISU is rate-limited per AppId
                 * (HTTP 4xx after the first call) and the AuthorizationToken
                 * is valid for ~4 h. */
                if (!force_refresh && FAILED( dowork_hr ) &&
                    user_impl->sisu_token && user_impl->sisu_expiry > now + 30 &&
                    strcmp( user_impl->sisu_rp, rp ) == 0)
                {
                    HSTRING dup = NULL;
                    if (SUCCEEDED( WindowsDuplicateString( user_impl->sisu_token, &dup ) ))
                    {
                        xsts_token = dup;
                        token_uhs = user_impl->sisu_uhs;
                        dowork_hr = S_OK;
                        TRACE( "reusing cached SISU token for %s (expires in %lds)\n",
                               rp, (long)(user_impl->sisu_expiry - now) );
                    }
                }

                if (FAILED( dowork_hr ))
                {
                    HSTRING device_token = NULL;
                    /* The launcher's pre-auth oauth_token lasts ~1h; once it
                     * lapses, SISU below rejects it and the Minecraft-Services
                     * session (Realms, signaling, sign-in) can't be renewed.
                     * Now that HttpRequest reaches the auth edges over OpenSSL,
                     * refresh it on demand from the stored refresh token before
                     * minting. RefreshOAuth hits login.live.com; the resulting
                     * fresh token is what the sisu/xsts edges require. */
                    if (user_impl->oauth_token_expiry && user_impl->refresh_token &&
                        time( NULL ) >= user_impl->oauth_token_expiry - 60)
                    {
                        LPSTR rt = NULL; UINT32 rtl = 0;
                        if (SUCCEEDED( HSTRINGToMultiByte( user_impl->refresh_token, &rt, &rtl ) ) && rt)
                        {
                            HSTRING n_oauth = NULL, n_refresh = NULL; time_t n_exp = 0;
                            if (SUCCEEDED( RefreshOAuth( "0000000048183522", rt, &n_exp, &n_refresh, &n_oauth ) ))
                            {
                                if (user_impl->oauth_token) WindowsDeleteString( user_impl->oauth_token );
                                user_impl->oauth_token = n_oauth;
                                if (user_impl->refresh_token) WindowsDeleteString( user_impl->refresh_token );
                                user_impl->refresh_token = n_refresh;
                                user_impl->oauth_token_expiry = n_exp;
                                ERR( "refreshed oauth token in-session (good for %llds)\n",
                                     (long long)(n_exp - time( NULL )) );
                            }
                            else WARN( "in-session oauth refresh failed — mint will likely fail\n" );
                            free( rt );
                        }
                    }
                    if (SUCCEEDED( DeviceAuth_GetDeviceToken( &device_token ) ) && device_token)
                    {
                        /* xal/imLinguin's working Bedrock-PlayFab auth uses
                         * the caller's URL as RP unchanged — what unlocks
                         * PlayFab is SISU's title-binding via AppId
                         * 0000000048183522, not an audience swap. */
                        UINT64 sisu_uhs = 0;
                        dowork_hr = RequestSisuAuthorize(
                            "0000000048183522",
                            user_impl->oauth_token, device_token, rp,
                            &xsts_token, &sisu_uhs );
                        WindowsDeleteString( device_token );
                        if (SUCCEEDED( dowork_hr ) && sisu_uhs)
                        {
                            token_uhs = sisu_uhs;
                            /* Cache for the next ~4 h.  Replace whatever
                             * we had — RP may have changed. */
                            if (user_impl->sisu_token)
                                WindowsDeleteString( user_impl->sisu_token );
                            user_impl->sisu_token = NULL;
                            if (SUCCEEDED( WindowsDuplicateString( xsts_token,
                                                                    &user_impl->sisu_token ) ))
                            {
                                user_impl->sisu_uhs = sisu_uhs;
                                user_impl->sisu_expiry = time( NULL ) + 4 * 3600;
                                lstrcpynA( user_impl->sisu_rp, rp, sizeof(user_impl->sisu_rp) );
                            }
                        }
                        else if (FAILED( dowork_hr ))
                            WARN( "SISU for RP %s failed: 0x%08lx — falling back to user-only XSTS\n",
                                  rp, dowork_hr );
                    }
                }
            }

            if (FAILED( dowork_hr ))
                dowork_hr = RequestXstsTokenForRelyingParty( user_impl->user_token, rp, &xsts_token );
            if (FAILED( dowork_hr ))
            {
                WARN( "XSTS token request for RP %s failed: 0x%08lx\n", rp, dowork_hr );
                impl->lpVtbl->XAsyncComplete( impl, providerData->async, dowork_hr, 0 );
                break;
            }

            dowork_hr = HSTRINGToMultiByte( xsts_token, &xsts_str, &xsts_len );
            WindowsDeleteString( xsts_token );
            if (FAILED( dowork_hr ))
            {
                impl->lpVtbl->XAsyncComplete( impl, providerData->async, dowork_hr, 0 );
                break;
            }

            /* Format: XBL3.0 x=<userHash>;<xstsToken> — userHash MUST match
             * the uhs claim inside the token (set above to sisu_uhs when
             * SISU minted, falls back to local_id.value for user-only). */
            context->result_token_len = snprintf( NULL, 0, "XBL3.0 x=%llu;%.*s",
                (unsigned long long)token_uhs, (int)xsts_len, xsts_str );
            context->result_token = calloc( 1, context->result_token_len + 1 );
            if (!context->result_token)
            {
                free( xsts_str );
                impl->lpVtbl->XAsyncComplete( impl, providerData->async, E_OUTOFMEMORY, 0 );
                break;
            }
            snprintf( context->result_token, context->result_token_len + 1, "XBL3.0 x=%llu;%.*s",
                (unsigned long long)token_uhs, (int)xsts_len, xsts_str );
            free( xsts_str );

            /* Compute request signature if device auth is available */
            if (DeviceAuth_IsInitialized())
            {
                dowork_hr = DeviceAuth_SignRequest(
                    context->method, context->request_target, context->result_token,
                    /* ExtraHeaders is endpoint-policy data, not the complete
                     * caller header list.  The default Xbox Live policy is
                     * empty; keep the normalized headers in the async context
                     * until endpoint-policy discovery is implemented. */
                    0, NULL, context->buffer, context->size,
                    &context->result_signature );
                if (FAILED( dowork_hr ))
                {
                    WARN( "request signing failed: 0x%08lx\n", dowork_hr );
                    impl->lpVtbl->XAsyncComplete( impl, providerData->async,
                                                  dowork_hr, 0 );
                    break;
                }
                context->result_signature_len = strlen( context->result_signature );
            }

            if (!context->utf16)
            {
                SIZE_T strings_size = context->result_token_len + 1;
                if (context->result_signature_len > ~(SIZE_T)0 - strings_size - 1 ||
                    strings_size + context->result_signature_len + 1 >
                        ~(SIZE_T)0 - sizeof(XUserGetTokenAndSignatureData))
                {
                    impl->lpVtbl->XAsyncComplete( impl, providerData->async,
                        HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW ), 0 );
                    break;
                }
                if (context->result_signature_len)
                    strings_size += context->result_signature_len + 1;
                context->result_size = sizeof(XUserGetTokenAndSignatureData) + strings_size;
            }
            else
            {
                SIZE_T char_count, strings_size;

                dowork_hr = utf8_to_utf16( context->result_token,
                    &context->result_token_utf16,
                    &context->result_token_utf16_count );
                if (SUCCEEDED( dowork_hr ) && context->result_signature_len)
                    dowork_hr = utf8_to_utf16( context->result_signature,
                        &context->result_signature_utf16,
                        &context->result_signature_utf16_count );
                if (FAILED( dowork_hr ))
                {
                    impl->lpVtbl->XAsyncComplete( impl, providerData->async,
                                                  dowork_hr, 0 );
                    break;
                }
                if (context->result_signature_utf16_count >
                    ~(SIZE_T)0 - context->result_token_utf16_count)
                {
                    impl->lpVtbl->XAsyncComplete( impl, providerData->async,
                        HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW ), 0 );
                    break;
                }
                char_count = context->result_token_utf16_count +
                             context->result_signature_utf16_count;
                if (char_count > (~(SIZE_T)0 -
                    sizeof(XUserGetTokenAndSignatureUtf16Data)) / sizeof(WCHAR))
                {
                    impl->lpVtbl->XAsyncComplete( impl, providerData->async,
                        HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW ), 0 );
                    break;
                }
                strings_size = char_count * sizeof(WCHAR);
                context->result_size = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                                       strings_size;
            }
            if (context->diagnostic_sequence <= TOKEN_REQUEST_LOG_LIMIT)
                TRACE( "native Xbox request auth #%ld ready: rp=%s, signed=%u.\n",
                       context->diagnostic_sequence, rp,
                       context->result_signature_len != 0 );
            impl->lpVtbl->XAsyncComplete( impl, providerData->async, S_OK, context->result_size );
            break;
        }

        case Cleanup:
            free_token_and_signature_context( context );
            break;

        case Cancel:
            break;
    }

    impl->lpVtbl->Release( impl );
    return result;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, LPCSTR method, LPCSTR url, SIZE_T count, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T size, const void *buffer, XAsyncBlock *asyncBlock )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %s, count %llu, headers %p, size %llu, buffer %p, asyncBlock %p\n",
           iface, user, options, method, (unsigned long long)count,
           headers, (unsigned long long)size, buffer, asyncBlock );

    if (!user || !method || !url || !asyncBlock) return E_POINTER;
    if ((count && !headers) || (size && !buffer)) return E_POINTER;
    if (!*method || !*url) return E_INVALIDARG;
    if ((UINT32)options & ~(XUserGetTokenAndSignatureOptions_ForceRefresh |
                            XUserGetTokenAndSignatureOptions_AllUsers))
        return E_INVALIDARG;
    if (!(context = calloc( 1, sizeof( *context ) )))
        return E_OUTOFMEMORY;

    context->options = options;
    context->count = count;
    context->utf16 = FALSE;
    context->size = size;
    if (FAILED( hr = duplicate_string( method, &context->method ) ) ||
        FAILED( hr = duplicate_string( url, &context->url ) ) ||
        FAILED( hr = request_target_from_url( context->url,
                                               &context->request_target ) ))
        goto failed;
    for (LPSTR ptr = context->method; *ptr; ++ptr)
        *ptr = toupper( (unsigned char)*ptr );
    context->diagnostic_sequence = log_token_request(
        context->method, context->url, context->options );

    if (count && !(context->headers = calloc( count, sizeof(*context->headers) )))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    for (SIZE_T i = 0; i < count; i++)
    {
        LPSTR name, value;

        if (!headers[i].name || !headers[i].value)
        {
            hr = E_POINTER;
            goto failed;
        }
        if (FAILED( hr = duplicate_string( headers[i].name, &name ) ))
            goto failed;
        context->headers[i].name = name;
        if (FAILED( hr = duplicate_string( headers[i].value, &value ) ))
            goto failed;
        context->headers[i].value = value;
    }
    if (size)
    {
        if (!(context->buffer = malloc( size )))
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        memcpy( context->buffer, buffer, size );
    }

    context->user = user;
    IXUserImpl_AddRef( &((struct x_user *)user)->IXUserImpl_iface );
    if (FAILED( hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
                                   (void **)&impl ) ))
        goto failed;

    hr = impl->lpVtbl->XAsyncBegin( impl, asyncBlock, context, x_user_XUserGetTokenAndSignatureAsync, "XUserGetTokenAndSignatureAsync", XUserGetTokenAndSignatureProvider );
    impl->lpVtbl->Release( impl );
    if (FAILED( hr )) goto failed;
    return hr;

failed:
    free_token_and_signature_context( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, size %p\n", iface, asyncBlock, size );
    if (!asyncBlock || !size) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResultSize( impl, asyncBlock, size );
    impl->lpVtbl->Release( impl );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, PVOID buffer, XUserGetTokenAndSignatureData **ptr, SIZE_T *used )
{
    IXThreadingImpl *impl;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, size %llu, buffer %p, ptr %p, used %p\n", iface, asyncBlock, (unsigned long long)size, buffer, ptr, used );
    if (!asyncBlock || !buffer || !ptr) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResult( impl, asyncBlock, x_user_XUserGetTokenAndSignatureAsync, size, buffer, used );
    impl->lpVtbl->Release( impl );
    if (SUCCEEDED( hr )) *ptr = (XUserGetTokenAndSignatureData *)buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, LPCWSTR method, LPCWSTR url, SIZE_T count, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T size, const void *buffer, XAsyncBlock *asyncBlock )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %s, count %llu, headers %p, size %llu, buffer %p, asyncBlock %p\n",
           iface, user, options, debugstr_w( method ), (unsigned long long)count,
           headers, (unsigned long long)size, buffer, asyncBlock );

    if (!user || !method || !url || !asyncBlock) return E_POINTER;
    if ((count && !headers) || (size && !buffer)) return E_POINTER;
    if (!*method || !*url) return E_INVALIDARG;
    if ((UINT32)options & ~(XUserGetTokenAndSignatureOptions_ForceRefresh |
                            XUserGetTokenAndSignatureOptions_AllUsers))
        return E_INVALIDARG;
    if (!(context = calloc( 1, sizeof( *context ) )))
        return E_OUTOFMEMORY;

    context->options = options;
    context->count = count;
    context->utf16 = TRUE;
    context->size = size;
    if (FAILED( hr = utf16_to_utf8( method, &context->method ) ) ||
        FAILED( hr = utf16_to_utf8( url, &context->url ) ) ||
        FAILED( hr = request_target_from_url( context->url,
                                               &context->request_target ) ))
        goto failed;
    for (LPSTR ptr = context->method; *ptr; ++ptr)
        *ptr = toupper( (unsigned char)*ptr );
    context->diagnostic_sequence = log_token_request(
        context->method, context->url, context->options );

    if (count && !(context->headers = calloc( count, sizeof(*context->headers) )))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    for (SIZE_T i = 0; i < count; i++)
    {
        LPSTR name, value;

        if (!headers[i].name || !headers[i].value)
        {
            hr = E_POINTER;
            goto failed;
        }
        if (FAILED( hr = utf16_to_utf8( headers[i].name, &name ) ))
            goto failed;
        context->headers[i].name = name;
        if (FAILED( hr = utf16_to_utf8( headers[i].value, &value ) ))
            goto failed;
        context->headers[i].value = value;
    }
    if (size)
    {
        if (!(context->buffer = malloc( size )))
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        memcpy( context->buffer, buffer, size );
    }

    context->user = user;
    IXUserImpl_AddRef( &((struct x_user *)user)->IXUserImpl_iface );
    if (FAILED( hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
                                   (void **)&impl ) ))
        goto failed;

    hr = impl->lpVtbl->XAsyncBegin( impl, asyncBlock, context, x_user_XUserGetTokenAndSignatureUtf16Async, "XUserGetTokenAndSignatureUtf16Async", XUserGetTokenAndSignatureProvider );
    impl->lpVtbl->Release( impl );
    if (FAILED( hr )) goto failed;
    return hr;

failed:
    free_token_and_signature_context( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, size %p\n", iface, asyncBlock, size );
    if (!asyncBlock || !size) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResultSize( impl, asyncBlock, size );
    impl->lpVtbl->Release( impl );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, PVOID buffer, XUserGetTokenAndSignatureUtf16Data **ptr, SIZE_T *used )
{
    IXThreadingImpl *impl;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, size %llu, buffer %p, ptr %p, used %p\n", iface, asyncBlock, (unsigned long long)size, buffer, ptr, used );
    if (!asyncBlock || !buffer || !ptr) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResult( impl, asyncBlock, x_user_XUserGetTokenAndSignatureUtf16Async, size, buffer, used );
    impl->lpVtbl->Release( impl );
    if (SUCCEEDED( hr )) *ptr = (XUserGetTokenAndSignatureUtf16Data *)buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl *iface, XUserHandle user, LPCSTR url, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, url %p, asyncBlock %p stub!\n", iface, user, url, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl *iface, XUserHandle user, LPCWSTR url, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, url %p, asyncBlock %p stub!\n", iface, user, url, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl *iface, XTaskQueueHandle queue, PVOID context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct change_registration *registration;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p\n", iface, queue, context, callback, token );
    /* XUserAddResult reports the initial sign-in. SignedInAgain is only valid
     * after a retained signed-out handle signs in again. */
    if (!callback || !token) return E_POINTER;
    if (!(registration = calloc( 1, sizeof(*registration) )))
        return E_OUTOFMEMORY;

    registration->token = InterlockedIncrement64( &next_change_token );
    if (!registration->token)
        registration->token = InterlockedIncrement64( &next_change_token );
    AcquireSRWLockExclusive( &change_registration_lock );
    registration->next = change_registrations;
    change_registrations = registration;
    ReleaseSRWLockExclusive( &change_registration_lock );
    token->token = registration->token;

    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct change_registration **cursor, *registration = NULL;
    BOOLEAN found;

    TRACE( "iface %p, token %llu, wait %d\n", iface,
           (unsigned long long)token.token, wait );
    if (!token.token) return FALSE;

    AcquireSRWLockExclusive( &change_registration_lock );
    for (cursor = &change_registrations; *cursor; cursor = &(*cursor)->next)
    {
        if ((*cursor)->token != token.token)
            continue;
        registration = *cursor;
        *cursor = registration->next;
        break;
    }
    ReleaseSRWLockExclusive( &change_registration_lock );
    found = registration != NULL;
    free( registration );
    return found;
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl *iface, XUserSignOutDeferralHandle *deferral )
{
    FIXME( "iface %p, deferral %p stub!\n", iface, deferral );
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl *iface, XUserSignOutDeferralHandle deferral )
{
    FIXME( "iface %p, deferral %p stub!\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl *iface, UINT64 userId, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, userId %llu, asyncBlock %p stub!\n", iface, userId, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, XUserHandle *user )
{
    FIXME( "iface %p, asyncBlock %p, user %p stub!\n", iface, asyncBlock, user );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, LPCSTR scope, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, options %u, scope %s, asyncBlock %p stub!\n", iface, options, scope, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, LPSTR token, SIZE_T *used )
{
    FIXME( "iface %p, size %llu, token %p, used %p stub!\n", iface,
           (unsigned long long)size, token, used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl *iface, XUserHandle user )
{
    FIXME( "iface %p, user %p stub!\n", iface, user );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p iface, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl *iface, XUserHandle user, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, asyncBlock %p stub!\n", iface, user, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static const struct IXUserImplVtbl x_user_vtbl =
{
    /* IUnknown methods */
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserBase methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserAddWithUi methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserMsa methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserStore methods */
    x_user_XUserIsStoreUser,
    /* IXUserPlatform methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserSignOut methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult
};

static inline struct x_user *impl_from_IXUserGamertag( IXUserGamertag *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertag_iface );
}

static HRESULT WINAPI x_user_gt_QueryInterface( IXUserGamertag *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertag( iface );

    TRACE( "iface %p, iid %s, out %p\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IXUserBase ) ||
        IsEqualGUID( iid, &IID_IXUserAddWithUi ) ||
        IsEqualGUID( iid, &IID_IXUserMsa ) ||
        IsEqualGUID( iid, &IID_IXUserStore ) ||
        IsEqualGUID( iid, &IID_IXUserPlatform ) ||
        IsEqualGUID( iid, &IID_IXUserSignOut ))
    {
        *out = &impl->IXUserImpl_iface;
        IXUserImpl_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertag ))
    {
        *out = &impl->IXUserGamertag_iface;
        IXUserGamertag_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_gt_AddRef( IXUserGamertag *iface )
{
    struct x_user *impl = impl_from_IXUserGamertag( iface );
    ULONG ref;

    if (impl->is_provider) return 2;
    ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_gt_Release( IXUserGamertag *iface )
{
    struct x_user *impl = impl_from_IXUserGamertag( iface );
    ULONG ref;

    if (impl->is_provider) return 1;
    ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu\n", iface, ref );
    if (!ref)
    {
        WindowsDeleteString( impl->refresh_token );
        WindowsDeleteString( impl->oauth_token );
        WindowsDeleteString( impl->user_token );
        WindowsDeleteString( impl->xsts_token );
        if (impl->xbl_privileges) WindowsDeleteString( impl->xbl_privileges );
        if (impl->sisu_token) WindowsDeleteString( impl->sisu_token );
        if (impl->mp_token) WindowsDeleteString( impl->mp_token );
        if (impl->realms_token) WindowsDeleteString( impl->realms_token );
        if (impl->lic_token) WindowsDeleteString( impl->lic_token );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI x_user_gt_XUserGetGamertag( IXUserGamertag *iface, XUserHandle user, XUserGamertagComponent component, SIZE_T size, LPSTR gamertag, SIZE_T *used )
{
    struct x_user *impl;
    LPCSTR value;
    SIZE_T len;

    TRACE( "iface %p, user %p, component %d, size %llu, gamertag %p, used %p\n", iface, user, component, (unsigned long long)size, gamertag, used );

    if (!user || !gamertag) return E_POINTER;
    impl = (struct x_user *)user;
    switch (component)
    {
        case XUserGamertagComponent_Classic:
            value = impl->gamertag;
            break;
        case XUserGamertagComponent_Modern:
            value = impl->modern_gamertag[0] ? impl->modern_gamertag :
                                               impl->gamertag;
            break;
        case XUserGamertagComponent_ModerSuffix:
            value = impl->modern_gamertag_suffix;
            break;
        case XUserGamertagComponent_UniqueModern:
            value = impl->unique_modern_gamertag[0] ?
                    impl->unique_modern_gamertag : impl->gamertag;
            break;
        default:
            return E_INVALIDARG;
    }
    len = strlen( value );

    if (used) *used = len + 1;
    if (size < len + 1) return E_NOT_SUFFICIENT_BUFFER;

    memcpy( gamertag, value, len + 1 );
    return S_OK;
}

static const struct IXUserGamertagVtbl x_user_gt_vtbl =
{
    /* IUnknown methods */
    x_user_gt_QueryInterface,
    x_user_gt_AddRef,
    x_user_gt_Release,
    /* IXUserGamertag methods */
    x_user_gt_XUserGetGamertag
};

static struct x_user x_user = {
    {&x_user_vtbl},
    {&x_user_gt_vtbl},
    1,
    TRUE,
};

IXUserImpl *x_user_impl = &x_user.IXUserImpl_iface;
