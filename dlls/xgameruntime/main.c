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
static INIT_ONCE xgameruntime_threading_init_once = INIT_ONCE_STATIC_INIT;
static HRESULT xgameruntime_threading_init_result = E_NOTIMPL;
static SRWLOCK native_process_task_queue_lock = SRWLOCK_INIT;
static LONG native_process_task_queue_ready;

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
            DisableThreadLibraryCalls(hinst);
            xgameruntime_threading = LoadLibraryA("xgameruntime.dll.threading");
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

/* Online services are implemented through the GDK/XUser/XSAPI contracts.
 * This runtime intentionally contains no Minecraft process-memory patcher. */

typedef HRESULT (WINAPI *InitializeApiImplEx2_ext)( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options );
typedef HRESULT (WINAPI *QueryApiImpl_ext)( const GUID *runtimeClassId, REFIID interfaceId, void **out );

struct native_threading_init_params
{
    ULONG gdkVer;
    ULONG gsVer;
    CHAR mode;
    INITIALIZE_OPTIONS *options;
};

static void ensure_native_process_task_queue( IXThreadingImpl *threading )
{
    XTaskQueueHandle process_queue = NULL;
    HRESULT hr;

    if (InterlockedCompareExchange( &native_process_task_queue_ready, 0, 0 ))
        return;

    AcquireSRWLockExclusive( &native_process_task_queue_lock );
    if (native_process_task_queue_ready)
        goto done;

    if (threading->lpVtbl->XTaskQueueGetCurrentProcessTaskQueue(
            threading, &process_queue ))
    {
        TRACE( "native DLL already has process queue %p\n", process_queue );
        InterlockedExchange( &native_process_task_queue_ready, 1 );
    }
    else
    {
        TRACE( "native DLL has no process task queue, creating one\n" );
        hr = threading->lpVtbl->XTaskQueueCreate( threading, ThreadPool,
                ThreadPool, &process_queue );
        if (SUCCEEDED( hr ))
        {
            threading->lpVtbl->XTaskQueueSetCurrentProcessTaskQueue(
                    threading, process_queue );
            TRACE( "set process task queue %p on native DLL\n", process_queue );
            InterlockedExchange( &native_process_task_queue_ready, 1 );
        }
        else
        {
            WARN( "XTaskQueueCreate failed: 0x%08lx\n", hr );
        }
    }

    /* Both GetCurrentProcessTaskQueue and Create return an owned handle.
     * SetCurrentProcessTaskQueue duplicates it, so our local reference must be
     * closed on every successful path. */
    if (process_queue)
        threading->lpVtbl->XTaskQueueCloseHandle( threading, process_queue );
done:
    ReleaseSRWLockExclusive( &native_process_task_queue_lock );
}

static BOOL CALLBACK initialize_native_threading_once( INIT_ONCE *once,
        void *parameter, void **context )
{
    const struct native_threading_init_params *params = parameter;
    InitializeApiImplEx2_ext native_init;
    QueryApiImpl_ext query_api;
    IXThreadingImpl *threading = NULL;
    HRESULT hr;

    UNREFERENCED_PARAMETER( once );
    UNREFERENCED_PARAMETER( context );

    if (!xgameruntime_threading)
    {
        xgameruntime_threading_init_result = HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );
        return TRUE;
    }

    native_init = (InitializeApiImplEx2_ext)GetProcAddress(
            xgameruntime_threading, "InitializeApiImplEx2" );
    if (!native_init)
    {
        xgameruntime_threading_init_result = HRESULT_FROM_WIN32( ERROR_PROC_NOT_FOUND );
    }
    else
    {
        TRACE( "performing native InitializeApiImplEx2 one-time attempt\n" );
        xgameruntime_threading_init_result = native_init( params->gdkVer,
                params->gsVer, params->mode, params->options );
    }

    /* The process queue belongs to the same one-shot bootstrap.  Keeping this
     * inside INIT_ONCE prevents concurrent InitializeApiImplEx2 callers from
     * racing to replace the queue while XAsync completions are in flight. */
    query_api = (QueryApiImpl_ext)GetProcAddress(
            xgameruntime_threading, "QueryApiImpl" );
    hr = query_api ? query_api( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
            (void **)&threading ) : HRESULT_FROM_WIN32( ERROR_PROC_NOT_FOUND );
    TRACE( "native QueryApiImpl for XThreading returned 0x%08lx, threading=%p\n",
           hr, threading );
    if (SUCCEEDED( hr ) && threading)
    {
        ensure_native_process_task_queue( threading );
        threading->lpVtbl->Release( threading );
    }
    return TRUE;
}

HRESULT WINAPI InitializeApiImplEx2( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options )
{
    HRESULT hr;
    static BOOLEAN com_initialized = FALSE;
    struct native_threading_init_params native_params = {
        gdkVer, gsVer, mode, options
    };
    TRACE("gdkVer %ld, gsVer %ld, mode %d, options %p\n", gdkVer, gsVer, mode, options);

    /* Initialize COM for the GDK runtime - needed for DllGetClassObject / CoCreateInstance.
     * Without this, XSAPI's internal COM calls fail with "apartment not initialised". */
    if (!com_initialized)
    {
        hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
            com_initialized = TRUE;
    }

    /* Bootstrap the native XAsync/XTaskQueue sidecar exactly once.  Different
     * game components call InitializeApiImplEx2 with different GDK versions,
     * sometimes while XSAPI completions are in flight.  Retrying sidecar
     * initialization at that point can invalidate or temporarily reject its
     * process queue; native XAsyncComplete then fail-fasts when it cannot
     * enqueue the completion callback.  The sidecar remains queryable after
     * its first (even partially successful) bootstrap, so later calls only
     * initialize our GDK components below. */
    TRACE("xgameruntime_threading = %p\n", xgameruntime_threading);
    if (xgameruntime_threading)
    {
        if (!InitOnceExecuteOnce( &xgameruntime_threading_init_once,
                initialize_native_threading_once, &native_params, NULL ))
            WARN( "native threading one-time initialization failed: %lu\n",
                  GetLastError() );
        TRACE( "native one-time InitializeApiImplEx2 result 0x%08lx\n",
               xgameruntime_threading_init_result );
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

#define UNMAPPED_QUERY_LOG_LIMIT 32

struct unmapped_query_pair
{
    GUID runtime_class_id;
    GUID interface_id;
};

static HRESULT query_api_result( HRESULT status, const GUID *runtime_class_id,
                                 const GUID *interface_id )
{
    static struct unmapped_query_pair seen[UNMAPPED_QUERY_LOG_LIMIT];
    static SRWLOCK lock = SRWLOCK_INIT;
    static UINT count;
    static BOOLEAN limit_reported;
    BOOLEAN report = FALSE, report_limit = FALSE;
    UINT i;

    if (SUCCEEDED( status )) return status;

    AcquireSRWLockExclusive( &lock );
    for (i = 0; i < count; ++i)
    {
        if (IsEqualGUID( runtime_class_id, &seen[i].runtime_class_id ) &&
            IsEqualGUID( interface_id, &seen[i].interface_id ))
            goto done;
    }

    if (count < ARRAY_SIZE(seen))
    {
        seen[count].runtime_class_id = *runtime_class_id;
        seen[count].interface_id = *interface_id;
        ++count;
        report = TRUE;
    }
    else if (!limit_reported)
    {
        limit_reported = TRUE;
        report_limit = TRUE;
    }

done:
    ReleaseSRWLockExclusive( &lock );

    if (report)
        ERR( "unsupported GDK QueryApiImpl class %s, interface %s, hr %#lx.\n",
             debugstr_guid( runtime_class_id ), debugstr_guid( interface_id ),
             status );
    else if (report_limit)
        ERR( "unsupported GDK QueryApiImpl log limit reached; suppressing new pairs.\n" );

    return status;
}

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
    TRACE("runtimeClassId %s, interfaceId %s, out %p\n", debugstr_guid(runtimeClassId), debugstr_guid(interfaceId), out);

    if ( IsEqualGUID( runtimeClassId, &CLSID_XSystemImpl ) )
    {
        return query_api_result( IXSystemImpl_QueryInterface( x_system_impl,
                                 interfaceId, out ), runtimeClassId, interfaceId );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XGameRuntimeFeatureImpl ) )
    {
        return query_api_result( IXGameRuntimeFeatureImpl_QueryInterface(
                                 x_game_runtime_feature_impl, interfaceId, out ),
                                 runtimeClassId, interfaceId );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XGameImpl ) )
    {
        return query_api_result( IXGameImpl_QueryInterface( x_game_impl,
                                 interfaceId, out ), runtimeClassId, interfaceId );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XSystemAnalyticsImpl ) )
    {
        return query_api_result( IXSystemAnalyticsImpl_QueryInterface(
                                 x_system_analytics_impl, interfaceId, out ),
                                 runtimeClassId, interfaceId );
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
                ensure_native_process_task_queue( (IXThreadingImpl *)*out );

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

            return query_api_result( thr, runtimeClassId, interfaceId );
        }
        return query_api_result( IXThreadingImpl_QueryInterface( x_threading_impl,
                                 interfaceId, out ), runtimeClassId, interfaceId );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XNetworkingImpl ) )
    {
        return query_api_result( IXNetworkingImpl_QueryInterface(
                                 x_networking_impl, interfaceId, out ),
                                 runtimeClassId, interfaceId );
    }
    else if ( IsEqualGUID( runtimeClassId, &CLSID_XUserImpl ) )
    {
        return query_api_result( IXUserImpl_QueryInterface( x_user_impl,
                                 interfaceId, out ), runtimeClassId, interfaceId );
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

    /* Unmapped GDK runtime classes: report not-implemented (the RE logger that
     * used to hand back a fake object here was removed — its fake objects could
     * fail-fast/NULL-crash MC at startup, and the 6 startup CLSIDs it captured
     * are not the in-game social path). */
    if (out) *out = NULL;
    return query_api_result( E_NOTIMPL, runtimeClassId, interfaceId );
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
