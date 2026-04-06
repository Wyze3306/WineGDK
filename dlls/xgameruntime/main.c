/*
 * Xbox Game runtime Library
 * 
 * Written by Weather
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

#include "initguid.h"
#include "private.h"

#include "GDKComponent/InitInternalGDKC.h"

WINE_DEFAULT_DEBUG_CHANNEL(xgameruntime);

static HMODULE xgameruntime;
static HMODULE xgameruntime_threading;

static VOID LoadOtherRuntime( DWORD *asked )
{
    HKEY hKey;
    LPCSTR subKey = "Software\\Wine\\WineGDK";
    LPCSTR valueName = "LoadOtherRuntimeAsked";
    DWORD value;
    DWORD dataSize = sizeof(DWORD);
    LONG result;

    *asked = 0;

    result = RegCreateKeyExA(
        HKEY_LOCAL_MACHINE,
        subKey,
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        NULL,
        &hKey,
        NULL
    );

    if (result != ERROR_SUCCESS) {
        return;
    }

    // Try to read the value
    result = RegQueryValueExA(
        hKey,
        valueName,
        NULL,
        NULL,
        (LPBYTE)&value,
        &dataSize
    );

    if ( result == ERROR_FILE_NOT_FOUND ) 
    {
        value = 1;

        result = RegSetValueExA(
            hKey,
            valueName,
            0,
            REG_DWORD,
            (const BYTE*)&value,
            sizeof(DWORD)
        );
    } else if ( result == ERROR_SUCCESS ) 
    {
        *asked = value;

        value = 1;

        result = RegSetValueExA(
            hKey,
            valueName,
            0,
            REG_DWORD,
            (const BYTE*)&value,
            sizeof(DWORD)
        );
    }

    RegCloseKey( hKey );
    return;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return xgameruntime != NULL ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, void *reserved )
{
    TRACE("inst %p, reason %lu, reserved %p.\n", hinst, reason, reserved);

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinst);
            xgameruntime_threading = LoadLibraryA("xgameruntime.dll.threading");
            break;
        case DLL_PROCESS_DETACH:
            if (reserved) break;
            if (xgameruntime) FreeLibrary(xgameruntime);
            if (xgameruntime_threading) FreeLibrary(xgameruntime_threading);
        break;
    }
    return TRUE;
}

typedef HRESULT (WINAPI *InitializeApiImplEx2_ext)( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options );

HRESULT WINAPI InitializeApiImplEx2( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options )
{
    HRESULT hr;

    TRACE("gdkVer %ld, gsVer %ld, mode %d, options %p\n", gdkVer, gsVer, mode, options);

    /* Forward to the native threading DLL to initialize its XAsync/XTaskQueue system */
    TRACE("xgameruntime_threading = %p\n", xgameruntime_threading);
    if (xgameruntime_threading)
    {
        InitializeApiImplEx2_ext native_init = (InitializeApiImplEx2_ext)GetProcAddress( xgameruntime_threading, "InitializeApiImplEx2" );
        if (native_init)
        {
            hr = native_init( gdkVer, gsVer, mode, options );
            TRACE("native InitializeApiImplEx2 returned 0x%08lx\n", hr);
            /* Ignore failures from native init - it may fail without Gaming Services
               but the XAsync/XTaskQueue subsystem should still be usable */
        }
    }

    return GDKC_InitAPI( gdkVer, gsVer, mode, options );
}

HRESULT WINAPI InitializeApiImplEx( ULONG gdkVer, ULONG gsVer, CHAR mode )
{
    TRACE("gdkVer %ld, gsVer %ld, mode %d\n", gdkVer, gsVer, mode);
    return InitializeApiImplEx2( gdkVer, gsVer, mode, NULL );
}

HRESULT WINAPI InitializeApiImpl( ULONG gdkVer, ULONG gsVer )
{
    TRACE("gdkVer %ld, gsVer %ld\n", gdkVer, gsVer);
    return InitializeApiImplEx2( gdkVer, gsVer, 0, NULL );
}

typedef HRESULT (WINAPI *QueryApiImpl_ext)( const GUID *runtimeClassId, REFIID interfaceId, void **out );

HRESULT WINAPI QueryApiImpl( const GUID *runtimeClassId, REFIID interfaceId, void **out )
{
    // Interfaces returned are COM interfaces and inherit IUnknown*
    // 
    //  On MSDN, There's no official documentation on the order of these interfaces and functions.
    // However, we can hook a dummy `xgameruntime.dll` into test environments and individually query
    // each class and what signatures they posses. Once we've pass through an empty IUnknown* interface,
    // we can reconstruct the vtable of each class based on what function gets called.
    //
    //  Example: (e349bd1a-fc20-4e40-b99c-4178cc6b409f) corresponds to part of the `ISystem` class and implements
    // these functions in order:
    //
    //  /*** IUnknown methods ***/
    //  IXSystemImpl_QueryInterface,                    (offset 0)
    //  IXSystemImpl_AddRef,                            (offset 8)
    //  IXSystemImpl_Release,                           (offset 16)
    //  /*** IXSystemImpl methods ***/
    //  IXSystemImpl_XSystemGetConsoleId                (offset 24)
    //  IXSystemImpl_XSystemGetXboxLiveSandboxId        (offset 32)
    //  IXSystemImpl_XSystemGetAppSpecificDeviceId      (offset 40)
    //  IXSystemImpl_XSystemHandleTrack                 (offset 48)
    //  IXSystemImpl_XSystemIsHandleValid               (offset 56)
    //  IXSystemImpl_XSystemAllowFullDownloadBandwidth  (offset 64)
    //

    QueryApiImpl_ext func = (QueryApiImpl_ext)GetProcAddress( xgameruntime_threading, "QueryApiImpl" );
    DWORD asked;

    TRACE("runtimeClassId %s, interfaceId %s, out %p\n", debugstr_guid(runtimeClassId), debugstr_guid(interfaceId), out);

    if ( IsEqualGUID( runtimeClassId, &CLSID_XSystemImpl ) )
    {
        return IXSystemImpl_QueryInterface( x_system_impl, interfaceId, out );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XGameRuntimeFeatureImpl ) )
    {
        return IXGameRuntimeFeatureImpl_QueryInterface( x_game_runtime_feature_impl, interfaceId, out );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XSystemAnalyticsImpl ) )
    {
        return IXSystemAnalyticsImpl_QueryInterface( x_system_analytics_impl, interfaceId, out );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XThreadingImpl ) )
    {
        /* Use native threading DLL for XAsync/XTaskQueue - it has proper
         * thread pool dispatch that the game relies on for its main loop. */
        if ( func )
            return func( runtimeClassId, interfaceId, out );
        return IXThreadingImpl_QueryInterface( x_threading_impl, interfaceId, out );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XNetworkingImpl ) )
    {
        return IXNetworkingImpl_QueryInterface( x_networking_impl, interfaceId, out );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XUserImpl ) )
    {
        return IXUserImpl_QueryInterface( x_user_impl, interfaceId, out );
    }

    /* {0dd112ac} composite XStore service */
    if ( runtimeClassId->Data1 == 0x0dd112ac )
    {
        extern void *x_store_composite_get(void);
        void *store = x_store_composite_get();
        if (store) { *out = store; return S_OK; }
    }

    /* {af406016} composite service broker */
    if ( runtimeClassId->Data1 == 0xaf406016 )
    {
        extern void *x_service_broker_get(void);
        void *broker = x_service_broker_get();
        if (broker) { *out = broker; return S_OK; }
    }

    /* Remaining GDK CLSIDs - return E_NOINTERFACE.
     * Returning fake objects causes worse problems than E_NOINTERFACE
     * because the game calls vtable methods expecting specific interfaces. */

    FIXME( "%s (iid %s) not implemented, returning E_NOINTERFACE.\n", debugstr_guid( runtimeClassId ), debugstr_guid( interfaceId ) );
    if (out) *out = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI UninitializeApiImpl( void )
{
    TRACE("stub!\n");
    return E_NOTIMPL;
}

/* COM class factory for XSAPI Xbox Live context {834366da-2d43-4fe3-8dcd-42ff2274bd0d} */

static HRESULT WINAPI xsapi_cf_QueryInterface( IClassFactory *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IClassFactory ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI xsapi_cf_AddRef( IClassFactory *iface ) { return 2; }
static ULONG WINAPI xsapi_cf_Release( IClassFactory *iface ) { return 1; }

static HRESULT WINAPI xsapi_cf_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out )
{
    FIXME( "CreateInstance iid %s - XSAPI context requested\n", debugstr_guid( iid ) );
    /* The game creates an XSAPI Xbox Live context through COM.
     * Return our service broker which handles all GDK sub-interfaces. */
    if (out)
    {
        extern void *x_service_broker_get(void);
        *out = x_service_broker_get();
        return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT WINAPI xsapi_cf_LockServer( IClassFactory *iface, BOOL lock ) { return S_OK; }

static const IClassFactoryVtbl xsapi_cf_vtbl = {
    xsapi_cf_QueryInterface,
    xsapi_cf_AddRef,
    xsapi_cf_Release,
    xsapi_cf_CreateInstance,
    xsapi_cf_LockServer,
};

static IClassFactory xsapi_class_factory = { &xsapi_cf_vtbl };

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID iid, void **out )
{
    static const GUID CLSID_XsapiContext = {0x834366da, 0x2d43, 0x4fe3, {0x8d,0xcd, 0x42,0xff,0x22,0x74,0xbd,0x0d}};

    TRACE( "clsid %s, iid %s, out %p\n", debugstr_guid( clsid ), debugstr_guid( iid ), out );

    if (IsEqualGUID( clsid, &CLSID_XsapiContext ))
    {
        return IClassFactory_QueryInterface( &xsapi_class_factory, iid, out );
    }

    FIXME( "clsid %s not handled\n", debugstr_guid( clsid ) );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI XGameRuntimeInitialize( void )
{
    ERR("XGameRuntimeInitialize called!\n");
    return S_OK;
}

VOID WINAPI XGameRuntimeUninitialize( void )
{
    TRACE("uninitializing game runtime\n");
}

HRESULT WINAPI XErrorReport( HRESULT status, LPCSTR message )
{
    TRACE("stub!\n");
    return E_NOTIMPL;
}

