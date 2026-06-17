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

static const struct IXUserImplVtbl x_user_vtbl;
static const struct IXUserGamertagVtbl x_user_gt_vtbl;

/* Change event callback storage */
static XUserChangeEventCallback g_change_callback;
static PVOID g_change_context;
static XTaskQueueHandle g_change_queue;

/* Track last signed-in user for FindUserByLocalId/ById */
static struct x_user *g_signed_in_user;

static HRESULT LoadDefaultUser( XUserHandle *user, LPCSTR client_id )
{
    struct x_user *impl;
    LSTATUS status;
    LPSTR buffer;
    HRESULT hr;
    DWORD size;

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
    BOOLEAN preauth_done = FALSE;
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
                        HSTRING gtg = NULL, xuid_s = NULL, agg_s = NULL;
                        HSTRING uhs_s = NULL, sisu_uhs_s = NULL, sisu_rp_s = NULL;
                        HSTRING mptok = NULL, mp_uhs_s = NULL, mp_rp_s = NULL;
                        HSTRING lictok = NULL, lic_uhs_s = NULL, lic_rp_s = NULL;
                        (void)GetJsonStringValue( root, L"user_token", &utok );
                        (void)GetJsonStringValue( root, L"xbl_token", &xtok );
                        (void)GetJsonStringValue( root, L"sisu_token", &stok );
                        (void)GetJsonStringValue( root, L"xbl_gamertag", &gtg );
                        (void)GetJsonStringValue( root, L"xbl_xuid", &xuid_s );
                        (void)GetJsonStringValue( root, L"xbl_age_group", &agg_s );
                        (void)GetJsonStringValue( root, L"xbl_uhs", &uhs_s );
                        (void)GetJsonStringValue( root, L"sisu_uhs", &sisu_uhs_s );
                        (void)GetJsonStringValue( root, L"sisu_rp", &sisu_rp_s );
                        (void)GetJsonStringValue( root, L"mp_token", &mptok );
                        (void)GetJsonStringValue( root, L"mp_uhs", &mp_uhs_s );
                        (void)GetJsonStringValue( root, L"mp_rp", &mp_rp_s );
                        (void)GetJsonStringValue( root, L"lic_token", &lictok );
                        (void)GetJsonStringValue( root, L"lic_uhs", &lic_uhs_s );
                        (void)GetJsonStringValue( root, L"lic_rp", &lic_rp_s );
                        if (utok && xtok && xuid_s)
                        {
                            LPSTR mb = NULL; UINT32 ml = 0;
                            WindowsDuplicateString( utok, &impl->user_token );
                            WindowsDuplicateString( xtok, &impl->xsts_token );
                            /* xuid */
                            if (SUCCEEDED( HSTRINGToMultiByte( xuid_s, &mb, &ml ) ) && mb)
                            { impl->xuid = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                            /* gamertag */
                            if (gtg && SUCCEEDED( HSTRINGToMultiByte( gtg, &mb, &ml ) ) && mb)
                            { lstrcpynA( impl->gamertag, mb, sizeof(impl->gamertag) ); free( mb ); mb = NULL; }
                            /* age group: "Adult" → 3, "Teen" → 2, "Child" → 1 */
                            if (agg_s && SUCCEEDED( HSTRINGToMultiByte( agg_s, &mb, &ml ) ) && mb)
                            {
                                if (!lstrcmpiA( mb, "Adult" )) impl->age_group = XUserAgeGroup_Adult;
                                else if (!lstrcmpiA( mb, "Teen" )) impl->age_group = XUserAgeGroup_Teen;
                                else impl->age_group = XUserAgeGroup_Child;
                                free( mb ); mb = NULL;
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
                                impl->sisu_expiry = time( NULL ) + 4 * 3600;
                            }
                            /* SISU cache for the multiplayer RP (external-server join) */
                            if (mptok && mp_uhs_s && mp_rp_s)
                            {
                                WindowsDuplicateString( mptok, &impl->mp_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( mp_uhs_s, &mb, &ml ) ) && mb)
                                { impl->mp_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( mp_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->mp_rp, mb, sizeof(impl->mp_rp) ); free( mb ); mb = NULL; }
                                impl->mp_expiry = time( NULL ) + 4 * 3600;
                            }
                            /* SISU cache for the marketplace/licensing RP (in-game Store) */
                            if (lictok && lic_uhs_s && lic_rp_s)
                            {
                                WindowsDuplicateString( lictok, &impl->lic_token );
                                if (SUCCEEDED( HSTRINGToMultiByte( lic_uhs_s, &mb, &ml ) ) && mb)
                                { impl->lic_uhs = strtoull( mb, NULL, 10 ); free( mb ); mb = NULL; }
                                if (SUCCEEDED( HSTRINGToMultiByte( lic_rp_s, &mb, &ml ) ) && mb)
                                { lstrcpynA( impl->lic_rp, mb, sizeof(impl->lic_rp) ); free( mb ); mb = NULL; }
                                impl->lic_expiry = time( NULL ) + 4 * 3600;
                            }
                            preauth_done = TRUE;
                            ERR( "preauth: loaded user/XSTS tokens for xuid=%llu gtg=%s — skipping Wine HTTP\n",
                                 (unsigned long long)impl->xuid, impl->gamertag );
                        }
                        if (utok) WindowsDeleteString( utok );
                        if (xtok) WindowsDeleteString( xtok );
                        if (stok) WindowsDeleteString( stok );
                        if (gtg) WindowsDeleteString( gtg );
                        if (xuid_s) WindowsDeleteString( xuid_s );
                        if (agg_s) WindowsDeleteString( agg_s );
                        if (uhs_s) WindowsDeleteString( uhs_s );
                        if (sisu_uhs_s) WindowsDeleteString( sisu_uhs_s );
                        if (sisu_rp_s) WindowsDeleteString( sisu_rp_s );
                        if (mptok) WindowsDeleteString( mptok );
                        if (mp_uhs_s) WindowsDeleteString( mp_uhs_s );
                        if (mp_rp_s) WindowsDeleteString( mp_rp_s );
                        if (lictok) WindowsDeleteString( lictok );
                        if (lic_uhs_s) WindowsDeleteString( lic_uhs_s );
                        if (lic_rp_s) WindowsDeleteString( lic_rp_s );
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
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl *iface )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu\n", iface, ref );
    if (!ref)
    {
        WindowsDeleteString( impl->refresh_token );
        WindowsDeleteString( impl->oauth_token );
        WindowsDeleteString( impl->user_token );
        WindowsDeleteString( impl->xsts_token );
        if (impl->sisu_token) WindowsDeleteString( impl->sisu_token );
        if (impl->mp_token) WindowsDeleteString( impl->mp_token );
        if (impl->lic_token) WindowsDeleteString( impl->lic_token );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl *iface, XUserHandle user, XUserHandle *duplicated )
{
    TRACE( "iface %p, user %p, duplicated %p\n", iface, user, duplicated );
    if (!duplicated) return E_POINTER;
    if (!user)
    {
        /* Game may pass NULL when getting user from composite interface */
        if (g_signed_in_user)
        {
            TRACE( "NULL user, returning g_signed_in_user %p\n", g_signed_in_user );
            IXUserImpl_AddRef( &g_signed_in_user->IXUserImpl_iface );
            *duplicated = (XUserHandle)g_signed_in_user;
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
    FIXME( "iface %p, maxUsers %p stub!\n", iface, maxUsers );
    return E_NOTIMPL;
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
    HRESULT hr;

    TRACE( "operation %d, providerData %p\n", operation, providerData );

    if (!providerData) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    context = providerData->context;

    switch (operation)
    {
        case Begin:
            return impl->lpVtbl->XAsyncSchedule( impl, providerData->async, 0 );

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

    return S_OK;
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
    TRACE( "XUserAddResult returning hr=0x%08lx, user=%p\n", hr, user ? *user : NULL );

    /* Track the signed-in user */
    if (SUCCEEDED( hr ) && *user)
    {
        struct x_user *u = (struct x_user *)*user;
        if (!g_signed_in_user)
        {
            g_signed_in_user = u;
            IXUserImpl_AddRef( &u->IXUserImpl_iface );
        }
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
    TRACE( "iface %p, localId %llu, user %p\n", iface, (unsigned long long)localId.value, user );
    if (!user) return E_POINTER;
    if (g_signed_in_user && g_signed_in_user->local_id.value == localId.value)
    {
        IXUserImpl_AddRef( &g_signed_in_user->IXUserImpl_iface );
        *user = (XUserHandle)g_signed_in_user;
        return S_OK;
    }
    return E_GAMEUSER_NO_DEFAULT_USER;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl *iface, XUserHandle user, UINT64 *userId )
{
    TRACE( "iface %p, user %p, userId %p\n", iface, user, userId );
    if (!user || !userId) return E_POINTER;
    *userId = ((struct x_user*)user)->xuid;
    TRACE( "returning xuid=%llu, gamertag=%s\n", (unsigned long long)*userId, ((struct x_user*)user)->gamertag );
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl *iface, UINT64 userId, XUserHandle *user )
{
    TRACE( "iface %p, userId %llu, user %p\n", iface, (unsigned long long)userId, user );
    if (!user) return E_POINTER;
    if (g_signed_in_user && g_signed_in_user->xuid == userId)
    {
        IXUserImpl_AddRef( &g_signed_in_user->IXUserImpl_iface );
        *user = (XUserHandle)g_signed_in_user;
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
    FIXME( "iface %p, asyncBlock %p, size %llu, buffer %p, used %p stub!\n", iface, asyncBlock, size, buffer, used );
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
    TRACE( "iface %p, user %p, options %d, privilege %d, hasPrivilege %p, reason %p\n", iface, user, options, privilege, hasPrivilege, reason );
    if (!user) return E_POINTER;
    if (hasPrivilege) *hasPrivilege = TRUE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
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
static LPCSTR resolve_relying_party_for_url( LPCSTR url )
{
    if (!url) return "http://xboxlive.com";

    if (strstr( url, "collections.mp.microsoft.com" ) ||
        strstr( url, "purchase.mp.microsoft.com" ) ||
        strstr( url, "displaycatalog.mp.microsoft.com" ) ||
        strstr( url, "inventory.xboxlive.com" ) ||
        strstr( url, "licensing.xboxlive.com" ))
        return "http://licensing.xboxlive.com";

    if (strstr( url, "playfab" ))
        return "https://b980a380.minecraft.playfabapi.com/";

    if (strstr( url, "multiplayer.minecraft" ))
        return "https://multiplayer.minecraft.net/";

    /* Friends/Social/Profile and any other Xbox Live edge. */
    return "http://xboxlive.com";
}

struct XUserGetTokenAndSignatureContext
{
    BOOLEAN utf16;
    XUserHandle user;
    XUserGetTokenAndSignatureOptions options;
    LPCSTR method;
    LPCWSTR method_utf16;
    LPCSTR url;
    LPCWSTR url_utf16;
    SIZE_T count;
    XUserGetTokenAndSignatureHttpHeader *headers;
    XUserGetTokenAndSignatureUtf16HttpHeader *headers_utf16;
    SIZE_T size;
    const void *buffer;
    LPSTR result_token;
    SIZE_T result_token_len;
    LPSTR result_signature;
    SIZE_T result_signature_len;
    SIZE_T result_size;
};

static HRESULT CALLBACK XUserGetTokenAndSignatureProvider( XAsyncOp operation, const XAsyncProviderData *providerData )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;

    TRACE( "operation %d, providerData %p\n", operation, providerData );

    if (!providerData) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    context = providerData->context;

    switch (operation)
    {
        case Begin:
            return impl->lpVtbl->XAsyncSchedule( impl, providerData->async, 0 );

        case GetResult:
        {
            XUserGetTokenAndSignatureData *data = (XUserGetTokenAndSignatureData *)providerData->buffer;
            LPSTR strings = (LPSTR)(data + 1);
            if (context->result_token && context->result_token_len > 0)
            {
                memcpy( strings, context->result_token, context->result_token_len );
                strings[context->result_token_len] = '\0';
                data->token = strings;
                data->tokenSize = context->result_token_len;

                if (context->result_signature && context->result_signature_len > 0)
                {
                    LPSTR sig_pos = strings + context->result_token_len + 1;
                    memcpy( sig_pos, context->result_signature, context->result_signature_len );
                    sig_pos[context->result_signature_len] = '\0';
                    data->signature = sig_pos;
                    data->signatureSize = context->result_signature_len;
                }
                else
                {
                    strings[context->result_token_len + 1] = '\0';
                    data->signature = strings + context->result_token_len + 1;
                    data->signatureSize = 0;
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
            LPCSTR url = context->utf16 ? NULL : context->url;
            LPCSTR rp = "http://xboxlive.com";

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

            TRACE( "requesting token for url=%s, rp=%s\n", url ? url : "(utf16)", rp );

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
            UINT64 token_uhs = user_impl->local_id.value;
            xsts_token = NULL;

            if (DeviceAuth_IsInitialized() && user_impl->oauth_token)
            {
                /* Preauth: when MC asks for the http://xboxlive.com RP
                 * (Friends/Social), serve the xbl_token the launcher
                 * pre-fetched and stashed into impl->xsts_token — that
                 * token IS a SISU AuthorizationToken for that exact RP
                 * with the matching uhs in impl->local_id.value. Hitting
                 * SISU from Wine would just TCP-RST against GnuTLS. */
                time_t now = time( NULL );
                if (user_impl->xsts_token && !strcmp( rp, "http://xboxlive.com" ))
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
                if (FAILED( dowork_hr ) &&
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
                /* Reuse the pre-minted licensing-RP SISU token for the in-game
                 * Marketplace (catalog + entitlement calls). Same rationale as
                 * mp_token: a live SISU call for this RP RSTs under Wine. */
                if (FAILED( dowork_hr ) &&
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
                if (FAILED( dowork_hr ) &&
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

            TRACE( "token for %s: %.40s...\n", rp, context->result_token );

            /* Compute request signature if device auth is available */
            if (DeviceAuth_IsInitialized())
            {
                LPCSTR method_str = context->utf16 ? "GET" : context->method;
                /* Extract path from URL */
                LPCSTR path = url ? strstr( url, "://" ) : NULL;
                if (path) path = strchr( path + 3, '/' );
                if (!path) path = "/";

                if (SUCCEEDED( DeviceAuth_SignRequest( method_str, path,
                    context->result_token, NULL, 0, &context->result_signature ) ))
                {
                    context->result_signature_len = strlen( context->result_signature );
                    TRACE( "signature: %.20s...\n", context->result_signature );
                }
            }

            context->result_size = sizeof(XUserGetTokenAndSignatureData)
                + context->result_token_len + 1
                + (context->result_signature_len ? context->result_signature_len + 1 : 1);
            impl->lpVtbl->XAsyncComplete( impl, providerData->async, S_OK, context->result_size );
            break;
        }

        case Cleanup:
            if (context->result_token) free( context->result_token );
            if (context->result_signature) free( context->result_signature );
            if (context->count)
            {
                if (context->utf16) free( context->headers_utf16 );
                else free( context->headers );
            }
            free( context );
            break;

        case Cancel:
            break;
    }

    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, LPCSTR method, LPCSTR url, SIZE_T count, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T size, const void *buffer, XAsyncBlock *asyncBlock )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, count %llu, headers %p, size %llu, buffer %p, asyncBlock %p\n", iface, user, options, method, url, count, headers, size, buffer, asyncBlock );

    if (!user || !method || !url || !asyncBlock) return E_POINTER;
    if (FAILED( hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return hr;
    if (!(context = calloc( 1, sizeof( *context ) )))
    {
        impl->lpVtbl->Release( impl );
        return E_OUTOFMEMORY;
    }

    context->options = options;
    context->buffer = buffer;
    context->method = method;
    context->count = count;
    context->utf16 = FALSE;
    context->size = size;
    context->user = user;
    context->url = url;
    if (count && headers && !(context->headers = calloc( count, sizeof( *headers ) )))
    {
        free( context );
        impl->lpVtbl->Release( impl );
        return E_OUTOFMEMORY;
    }

    for (SIZE_T i = 0; i < count; i++)
        context->headers[i] = headers[i];

    hr = impl->lpVtbl->XAsyncBegin( impl, asyncBlock, context, x_user_XUserGetTokenAndSignatureAsync, "XUserGetTokenAndSignatureAsync", XUserGetTokenAndSignatureProvider );
    impl->lpVtbl->Release( impl );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    IXThreadingImpl *impl;
    TRACE( "iface %p, asyncBlock %p, size %p\n", iface, asyncBlock, size );
    if (!asyncBlock || !size) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    return impl->lpVtbl->XAsyncGetResultSize( impl, asyncBlock, size );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, PVOID buffer, XUserGetTokenAndSignatureData **ptr, SIZE_T *used )
{
    IXThreadingImpl *impl;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, size %llu, buffer %p, ptr %p, used %p\n", iface, asyncBlock, (unsigned long long)size, buffer, ptr, used );
    if (!asyncBlock || !buffer || !ptr) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResult( impl, asyncBlock, x_user_XUserGetTokenAndSignatureAsync, size, buffer, used );
    if (SUCCEEDED( hr )) *ptr = (XUserGetTokenAndSignatureData *)buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, LPCWSTR method, LPCWSTR url, SIZE_T count, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T size, const void *buffer, XAsyncBlock *asyncBlock )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %hs, url %hs, count %llu, headers %p, size %llu, buffer %p, asyncBlock %p\n", iface, user, options, method, url, count, headers, size, buffer, asyncBlock );

    if (!user || !method || !url || !asyncBlock) return E_POINTER;
    if (FAILED( hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return hr;
    if (!(context = calloc( 1, sizeof( *context ) )))
    {
        impl->lpVtbl->Release( impl );
        return E_OUTOFMEMORY;
    }

    context->method_utf16 = method;
    context->options = options;
    context->buffer = buffer;
    context->url_utf16 = url;
    context->count = count;
    context->utf16 = TRUE;
    context->size = size;
    context->user = user;
    if (count && headers && !(context->headers_utf16 = calloc( count, sizeof( *headers ) )))
    {
        free( context );
        impl->lpVtbl->Release( impl );
        return E_OUTOFMEMORY;
    }

    for (SIZE_T i = 0; i < count; i++)
        context->headers_utf16[i] = headers[i];

    hr = impl->lpVtbl->XAsyncBegin( impl, asyncBlock, context, x_user_XUserGetTokenAndSignatureUtf16Async, "XUserGetTokenAndSignatureUtf16Async", XUserGetTokenAndSignatureProvider );
    impl->lpVtbl->Release( impl );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *size )
{
    IXThreadingImpl *impl;
    TRACE( "iface %p, asyncBlock %p, size %p\n", iface, asyncBlock, size );
    if (!asyncBlock || !size) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    return impl->lpVtbl->XAsyncGetResultSize( impl, asyncBlock, size );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl *iface, XAsyncBlock *asyncBlock, SIZE_T size, PVOID buffer, XUserGetTokenAndSignatureUtf16Data **ptr, SIZE_T *used )
{
    IXThreadingImpl *impl;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, size %llu, buffer %p, ptr %p, used %p\n", iface, asyncBlock, (unsigned long long)size, buffer, ptr, used );
    if (!asyncBlock || !buffer || !ptr) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) )) return E_FAIL;
    hr = impl->lpVtbl->XAsyncGetResult( impl, asyncBlock, x_user_XUserGetTokenAndSignatureUtf16Async, size, buffer, used );
    if (SUCCEEDED( hr )) *ptr = (XUserGetTokenAndSignatureUtf16Data *)buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl *iface, XUserHandle user, LPCSTR url, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, url %s, asyncBlock %p stub!\n", iface, user, url, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl *iface, XUserHandle user, LPCWSTR url, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, user %p, url %hs, asyncBlock %p stub!\n", iface, user, url, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static void CALLBACK change_event_taskqueue_cb( void *context, BOOL canceled )
{
    if (canceled || !g_signed_in_user || !g_change_callback) return;

    TRACE( "firing XUserChangeEvent_SignedInAgain via task queue for local_id=%llu\n",
           (unsigned long long)g_signed_in_user->local_id.value );
    g_change_callback( g_change_context, g_signed_in_user->local_id, XUserChangeEvent_SignedInAgain );
    TRACE( "change event callback returned\n" );
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl *iface, XTaskQueueHandle queue, PVOID context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    IXThreadingImpl *impl;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p\n", iface, queue, context, callback, token );
    g_change_callback = (XUserChangeEventCallback)(void*)callback;
    g_change_context = context;
    g_change_queue = queue;
    if (token) token->token = 1;

    /* Fire the change event to notify the game of sign-in */
    if (g_signed_in_user)
    {
        if (queue && SUCCEEDED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&impl ) ))
        {
            TRACE( "submitting change event to task queue %p\n", queue );
            impl->lpVtbl->XTaskQueueSubmitCallback( impl, queue, Completion, NULL, (XTaskQueueCallback*)change_event_taskqueue_cb );
            impl->lpVtbl->Release( impl );
        }
        else
        {
            TRACE( "firing change event directly (no queue)\n" );
            change_event_taskqueue_cb( NULL, FALSE );
        }
    }

    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
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
    FIXME( "iface %p, size %llu, token %p, used %p stub!\n", iface, size, token, used );
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
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_gt_Release( IXUserGamertag *iface )
{
    struct x_user *impl = impl_from_IXUserGamertag( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu\n", iface, ref );
    if (!ref)
    {
        WindowsDeleteString( impl->refresh_token );
        WindowsDeleteString( impl->oauth_token );
        WindowsDeleteString( impl->user_token );
        WindowsDeleteString( impl->xsts_token );
        if (impl->sisu_token) WindowsDeleteString( impl->sisu_token );
        if (impl->mp_token) WindowsDeleteString( impl->mp_token );
        if (impl->lic_token) WindowsDeleteString( impl->lic_token );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI x_user_gt_XUserGetGamertag( IXUserGamertag *iface, XUserHandle user, XUserGamertagComponent component, SIZE_T size, LPSTR gamertag, SIZE_T *used )
{
    struct x_user *impl;
    SIZE_T len;

    TRACE( "iface %p, user %p, component %d, size %llu, gamertag %p, used %p\n", iface, user, component, (unsigned long long)size, gamertag, used );

    if (!user) return E_POINTER;
    impl = (struct x_user *)user;
    len = strlen( impl->gamertag );

    if (used) *used = len + 1;
    if (!gamertag || size == 0) return S_OK;
    if (size < len + 1) return E_NOT_SUFFICIENT_BUFFER;

    memcpy( gamertag, impl->gamertag, len + 1 );
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
    0,
};

IXUserImpl *x_user_impl = &x_user.IXUserImpl_iface;