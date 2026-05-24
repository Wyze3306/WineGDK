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

            /* Determine relying party from URL */
            if (url && strstr( url, "playfab" ))
                rp = "https://b980a380.minecraft.playfabapi.com/";
            else if (url && strstr( url, "multiplayer.minecraft" ))
                rp = "https://multiplayer.minecraft.net/";

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
            if (DeviceAuth_IsInitialized() && user_impl->oauth_token)
            {
                HSTRING device_token = NULL;
                if (SUCCEEDED( DeviceAuth_GetDeviceToken( &device_token ) ) && device_token)
                {
                    /* Use the caller's actual RP for SISU.  Earlier
                     * attempt pinned it to multiplayer.minecraft.net so
                     * the audience would match what PlayFab "should"
                     * accept, but xal/imLinguin's working Bedrock auth
                     * just uses the per-title playfabapi.com subdomain
                     * directly — what makes PlayFab accept it isn't the
                     * audience, it's the title-binding SISU adds via
                     * AppId 0000000048183522.  Keep the audience the
                     * game requested so PlayFab's pre-validation
                     * cross-check passes. */
                    LPCSTR sisu_rp = rp;
                    UINT64 sisu_uhs = 0;
                    dowork_hr = RequestSisuAuthorize(
                        "0000000048183522",
                        user_impl->oauth_token, device_token, sisu_rp,
                        &xsts_token, &sisu_uhs );
                    WindowsDeleteString( device_token );
                    if (SUCCEEDED( dowork_hr ) && sisu_uhs)
                        token_uhs = sisu_uhs;
                    else if (FAILED( dowork_hr ))
                        WARN( "SISU for RP %s failed: 0x%08lx — falling back to user-only XSTS\n",
                              rp, dowork_hr );
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