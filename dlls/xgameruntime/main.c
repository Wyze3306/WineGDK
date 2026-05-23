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
#include "psapi.h"

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
        {
            HMODULE game;
            DWORD oldprot;

            DisableThreadLibraryCalls(hinst);
            xgameruntime_threading = LoadLibraryA("xgameruntime.dll.threading");

            /* Patch the game's isSignedIn checker to always return TRUE.
             * The function at RVA 0x1433680 checks user->isConnected(XboxLive)
             * which is never set because XSAPI social manager doesn't initialize
             * on Win32/Wine. Patching to 'mov eax,1; ret' bypasses this. */
            break;
        }
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
    static BOOLEAN com_initialized = FALSE;

    TRACE("gdkVer %ld, gsVer %ld, mode %d, options %p\n", gdkVer, gsVer, mode, options);

    /* Initialize COM for the GDK runtime - needed for DllGetClassObject / CoCreateInstance.
     * Without this, XSAPI's internal COM calls fail with "apartment not initialised". */
    if (!com_initialized)
    {
        hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
            com_initialized = TRUE;
    }

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

        /* Set a default process task queue on the native DLL's XThreadingImpl.
         * XSAPI's XblInitialize calls QueryApiImpl({XThreadingImpl}) then checks
         * vtable[25] (XTaskQueueGetCurrentProcessTaskQueue). If it returns FALSE
         * and XblInitArgs->queue is NULL, XblInitialize bails with 0x800701AB
         * and the entire XSAPI/social manager never initializes. */
        {
            HRESULT (WINAPI *qapi)( const GUID *, REFIID, void ** ) = (void*)GetProcAddress( xgameruntime_threading, "QueryApiImpl" );
            IXThreadingImpl *threading = NULL;
            HRESULT qhr = qapi ? qapi( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void**)&threading ) : E_FAIL;
            ERR( "native QueryApiImpl for XThreading returned 0x%08lx, threading=%p\n", qhr, threading );
            if (SUCCEEDED( qhr ) && threading)
            {
                XTaskQueueHandle processQueue = NULL;
                if (!threading->lpVtbl->XTaskQueueGetCurrentProcessTaskQueue( threading, &processQueue ))
                {
                    ERR( "native DLL has no process task queue, creating one\n" );
                    if (SUCCEEDED( threading->lpVtbl->XTaskQueueCreate( threading, ThreadPool, ThreadPool, &processQueue ) ))
                    {
                        threading->lpVtbl->XTaskQueueSetCurrentProcessTaskQueue( threading, processQueue );
                        ERR( "set process task queue %p on native DLL\n", processQueue );
                    }
                    else
                    {
                        ERR( "XTaskQueueCreate failed!\n" );
                    }
                }
                else
                {
                    ERR( "native DLL already has process queue %p\n", processQueue );
                }
                threading->lpVtbl->Release( threading );
            }
        }
    }

    /* Patch the game's isSignedIn checker to always return TRUE.
     * Scans for the function's unique byte pattern so it works across versions.
     * Called here (not DLL_PROCESS_ATTACH) to ensure the game binary is loaded.
     * Retries on each InitializeApiImplEx2 call until successful. */
    {
        static BOOLEAN patched = FALSE;
        if (!patched)
        {
            HMODULE game = GetModuleHandleA( NULL );
            if (game)
            {
                MODULEINFO modinfo;
                if (GetModuleInformation( GetCurrentProcess(), game, &modinfo, sizeof(modinfo) ))
                {
                    BYTE *base = (BYTE *)modinfo.lpBaseOfDll;
                    SIZE_T size = modinfo.SizeOfImage;
                    BYTE *addr = NULL;
                    SIZE_T i;
                    DWORD oldprot;

                    /* Function prologue pattern:
                     *   48 89 5C 24 10   mov [rsp+10h], rbx
                     *   48 89 74 24 18   mov [rsp+18h], rsi
                     *   57               push rdi
                     *   48 83 EC 30      sub rsp, 30h
                     * Then within ~20 bytes: 48 8B 49 50 (mov rcx,[rcx+50h]) = mUserManager
                     * Then within ~80 bytes: BA 01 00 00 00 (mov edx,1) = NetworkType::XboxLive */
                    static const BYTE prologue[] = {
                        0x48, 0x89, 0x5C, 0x24, 0x10,
                        0x48, 0x89, 0x74, 0x24, 0x18,
                        0x57,
                        0x48, 0x83, 0xEC, 0x30
                    };

                    for (i = 0; i + sizeof(prologue) + 80 < size; i++)
                    {
                        SIZE_T j;
                        BOOLEAN found_usermgr = FALSE, found_xboxlive = FALSE;

                        if (memcmp( base + i, prologue, sizeof(prologue) ) != 0)
                            continue;

                        /* Verify: mov rcx,[rcx+50h] within next 20 bytes */
                        for (j = i + sizeof(prologue); j < i + sizeof(prologue) + 20 && j + 4 < size; j++)
                            if (base[j]==0x48 && base[j+1]==0x8B && base[j+2]==0x49 && base[j+3]==0x50)
                                { found_usermgr = TRUE; break; }

                        if (!found_usermgr) continue;

                        /* Verify: mov edx,1 within next 80 bytes */
                        for (j = i + sizeof(prologue); j < i + sizeof(prologue) + 80 && j + 5 < size; j++)
                            if (base[j]==0xBA && base[j+1]==0x01 && base[j+2]==0x00 && base[j+3]==0x00 && base[j+4]==0x00)
                                { found_xboxlive = TRUE; break; }

                        if (!found_xboxlive) continue;

                        addr = base + i;
                        break;
                    }

                    if (addr && VirtualProtect( addr, 6, PAGE_EXECUTE_READWRITE, &oldprot ))
                    {
                        addr[0] = 0xB8; /* mov eax, 1 */
                        addr[1] = 0x01;
                        addr[2] = 0x00;
                        addr[3] = 0x00;
                        addr[4] = 0x00;
                        addr[5] = 0xC3; /* ret */
                        VirtualProtect( addr, 6, oldprot, &oldprot );
                        ERR( "patched isSignedIn at %p (RVA 0x%lx)\n", addr, (ULONG_PTR)(addr - base) );
                        patched = TRUE;
                    }
                    else if (!addr)
                    {
                        ERR( "isSignedIn pattern not found in %zu bytes\n", size );
                    }
                }
            }
        }
    }

    /* Patch 2: NOP the credential check gate that blocks XblInitialize.
     * The game's XboxLiveServices::signIn checks a credential provider
     * which returns E_FAIL without Gaming Services. This JL skips XblInitialize.
     * Pattern: cmp [rbp-0x18], 0; JL (83 7D E8 00 0F 8C)
     * followed by xorps xmm0,xmm0; xor eax,eax (0F 57 C0 33 C0) */
    {
        static BOOLEAN patched2 = FALSE;
        if (!patched2)
        {
            HMODULE game2 = GetModuleHandleA( NULL );
            if (game2)
            {
                MODULEINFO mi2;
                if (GetModuleInformation( GetCurrentProcess(), game2, &mi2, sizeof(mi2) ))
                {
                    BYTE *base = (BYTE *)mi2.lpBaseOfDll;
                    SIZE_T size = mi2.SizeOfImage;
                    static const BYTE pat2[] = { 0x83, 0x7D, 0xE8, 0x00, 0x0F, 0x8C };
                    static const BYTE verify[] = { 0x0F, 0x57, 0xC0, 0x33, 0xC0 };
                    SIZE_T i;
                    DWORD op;

                    for (i = 0; i + sizeof(pat2) + 10 < size; i++)
                    {
                        if (memcmp( base + i, pat2, sizeof(pat2) ) != 0) continue;
                        /* Verify: after the 6-byte JL (at +4), check for xorps pattern */
                        if (i + 10 + sizeof(verify) < size &&
                            memcmp( base + i + 10, verify, sizeof(verify) ) == 0)
                        {
                            /* NOP the JL: 0F 8C xx xx xx xx → 66 0F 1F 44 00 00 */
                            if (VirtualProtect( base + i + 4, 6, PAGE_EXECUTE_READWRITE, &op ))
                            {
                                base[i+4] = 0x66; base[i+5] = 0x0F; base[i+6] = 0x1F;
                                base[i+7] = 0x44; base[i+8] = 0x00; base[i+9] = 0x00;
                                VirtualProtect( base + i + 4, 6, op, &op );
                                ERR( "patched XblInitialize gate at %p (RVA 0x%lx)\n", base + i + 4, (ULONG_PTR)(i + 4) );
                                patched2 = TRUE;
                            }
                            break;
                        }
                    }
                    if (!patched2)
                        ERR( "XblInitialize gate pattern not found\n" );
                }
            }
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
        /* Use native threading DLL for XAsync/XTaskQueue. But ensure the
         * process task queue is set - XSAPI's XblInitialize checks vtable[25]
         * (GetCurrentProcessTaskQueue) and bails if it returns FALSE. */
        if ( func )
        {
            HRESULT thr = func( runtimeClassId, interfaceId, out );
            if (SUCCEEDED( thr ) && *out)
            {
                /* Ensure process task queue exists on the native impl */
                IXThreadingImpl *ti = (IXThreadingImpl *)*out;
                XTaskQueueHandle pq = NULL;
                if (!ti->lpVtbl->XTaskQueueGetCurrentProcessTaskQueue( ti, &pq ))
                {
                    /* Create and set a default process task queue */
                    if (SUCCEEDED( ti->lpVtbl->XTaskQueueCreate( ti, ThreadPool, ThreadPool, &pq ) ))
                        ti->lpVtbl->XTaskQueueSetCurrentProcessTaskQueue( ti, pq );
                }
            }

            /* DO NOT touch vtable[11].  Per xthread.h's IXThreadingImplVtbl
             * layout (QueryInterface, AddRef, Release, XAsyncGetStatus,
             * XAsyncGetResultSize, XAsyncCancel, XAsyncRun, XAsyncBegin,
             * __PADDING__, XAsyncSchedule, XAsyncComplete, XAsyncGetResult,
             * ...) slot 11 is XAsyncGetResult, NOT a hidden "user sign-in
             * slot".  Stubbing it to `xor eax,eax; ret` makes every async
             * result come back as S_OK with the caller's output buffer
             * untouched: XUserAddAsync's DoWork populates context->user,
             * but XAsyncGetResult never invokes the provider's GetResult
             * branch, so XUserAddResult returns user=NULL → XUserGetId
             * dereferences NULL → Minecraft's GDK auth path bubbles back
             * as "Llama (0x80004003)" on the title screen.  Whatever
             * XblInitialize needed before has to be solved elsewhere
             * (a real per-purpose hook, not blanketing a busy vtable
             * slot).  See bedrockonlinux-native-login-contract memory. */

            return thr;
        }
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
    HRESULT hr;
    ERR("XGameRuntimeInitialize called - initializing COM\n");
    hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        WARN("CoInitializeEx failed: 0x%08lx\n", hr);
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

