/*
 * Xbox Game runtime Library Tests
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

#include <stdlib.h>
#include <stdarg.h>
#define COBJMACROS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#include <roapi.h>
#include <activation.h>
#include <unknwn.h>
#include <xgameerr.h>

#include "../provider.h"
#include "wine/test.h"
#include "xthread.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Globalization
#include "windows.globalization.h"
#define WIDL_using_Windows_System_Profile
#include "windows.system.profile.h"

// April 2025 Release of GDK
#define GDKC_VERSION 10001L
#define GAMING_SERVICES_VERSION 3181L

static HMODULE xgameruntime = NULL;

typedef HRESULT (*InitializeApiImpl)( ULONG gdkVer, ULONG gsVer );
typedef HRESULT (*QueryApiImpl)( const GUID *runtimeClassId, REFIID interfaceId, void **out );

static InitializeApiImpl InitializeApiImpl_fun = NULL;
static QueryApiImpl QueryApiImpl_fun = NULL;

static const SIZE_T XSystemConsoleIdBytes = 39;
static const SIZE_T XSystemXboxLiveSandboxIdMaxBytes = 16;
static const SIZE_T XSystemXboxLiveSandboxIdBytes = 7;
static const SIZE_T XSystemAppSpecificDeviceIdBytes = 45;

static LPSTR testData = NULL;
static LONG async_do_work_count;
static LONG async_get_result_count;
static LONG async_cleanup_count;
static LONG async_cleanup_before_result;
static LONG async_completion_count;
static LONG user_change_count;

static const GUID test_clsid_xuser_impl =
    {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID test_iid_xuser_base =
    {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID test_iid_xuser_gamertag =
    {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};

static void CALLBACK XAsyncCompletion_testCallback( XAsyncBlock *asyncBlock )
{
    (void)asyncBlock;
    InterlockedIncrement( &async_completion_count );
}

static void CALLBACK user_change_callback( PVOID context, XUserLocalId local_id,
                                           XUserChangeEvent event )
{
    (void)context;
    (void)local_id;
    (void)event;
    InterlockedIncrement( &user_change_count );
}

#define check_interface(obj, iid, supported) _check_interface(__LINE__, obj, iid, supported)
static void _check_interface(unsigned int line, void *obj, const IID *iid, BOOL supported)
{
    IUnknown *iface = obj, *unknown;
    HRESULT hr;

    hr = IUnknown_QueryInterface(iface, iid, (void **)&unknown);
    ok_(__FILE__, line)(hr == S_OK || (!supported && hr == E_NOINTERFACE), "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unknown);
}

static inline HRESULT CALLBACK XAsyncProvider_testCallback( XAsyncOp op, const XAsyncProviderData* data )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;
    SIZE_T testDataSize = 7;

    hr = QueryApiImpl_fun( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    switch ( op )
    {
        case Begin:
            hr = IXThreadingImpl_XAsyncSchedule( xthreading, data->async, 0 );
            IXThreadingImpl_Release( xthreading );
            return hr;

        case DoWork:
            InterlockedIncrement( &async_do_work_count );
            testData = malloc( testDataSize );
            if ( !testData )
            {
                IXThreadingImpl_XAsyncComplete( xthreading, data->async, E_OUTOFMEMORY, 0 );
                IXThreadingImpl_Release( xthreading );
                return E_OUTOFMEMORY;
            }
            strcpy( testData, "foobar" );
            IXThreadingImpl_XAsyncComplete( xthreading, data->async, S_OK, testDataSize );
            IXThreadingImpl_Release( xthreading );
            return S_OK;

        case GetResult:
            InterlockedIncrement( &async_get_result_count );
            if ( async_cleanup_count ) InterlockedIncrement( &async_cleanup_before_result );
            memcpy( data->buffer, (void *)testData, testDataSize);
            IXThreadingImpl_Release( xthreading );
            return S_OK;

        case Cancel:
            IXThreadingImpl_XAsyncComplete( xthreading, data->async, E_ABORT, 0 );
            IXThreadingImpl_Release( xthreading );
            return S_OK;

        case Cleanup:
            InterlockedIncrement( &async_cleanup_count );
            free( testData );
            testData = NULL;
            IXThreadingImpl_Release( xthreading );
            return S_OK;
    }

    IXThreadingImpl_Release( xthreading );
    return S_OK;
}

/**
 *  Testing xgameruntime.dll is a bit difficult to do because the core 
 * library (xgameruntime.lib) is responsible for most of the interfaces 
 * used by applications, and applications don't interact with this library
 * directly themselves.
 * 
 *  These test cases were curated to test whatever's in the library itself 
 * at this moment.
 */

static void test_GDKComponentInit(void)
{
    HRESULT hr;
    LPCSTR xgameruntime_libname = "xgameruntime.dll";

    xgameruntime = LoadLibraryA( xgameruntime_libname );
    ok( xgameruntime != NULL, "xgameruntime.dll failed to load! error code: %lu\n", GetLastError() );

    InitializeApiImpl_fun = (InitializeApiImpl)GetProcAddress( xgameruntime, "InitializeApiImpl" );
    ok( InitializeApiImpl_fun != NULL, "couldn't locate function InitializeApiImpl within %p! error code: %lu\n", xgameruntime, GetLastError() );

    hr = InitializeApiImpl_fun( GDKC_VERSION, GAMING_SERVICES_VERSION );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    QueryApiImpl_fun = (QueryApiImpl)GetProcAddress( xgameruntime, "QueryApiImpl" );
    ok( QueryApiImpl_fun != NULL, "couldn't locate function QueryApiImpl within %p! error code: %lu\n", xgameruntime, GetLastError() );
}

static void test_XSystem(void)
{
    IXSystemImpl *xsystem;
    BOOLEAN validHandle;
    HRESULT hr;
    SIZE_T consoleIdUsed;
    SIZE_T sandboxIdUsed;
    LPSTR consoleId;
    LPSTR sandboxId;

    hr = QueryApiImpl_fun( &CLSID_XSystemImpl, &IID_IXSystemImpl, (void **)&xsystem );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( xsystem, &IID_IUnknown, TRUE );
    check_interface( xsystem, &IID_IXSystemImpl, TRUE );

    /**
     * xgameruntime.lib::XSystemGetConsoleId
     */
    consoleId = (LPSTR)malloc( XSystemConsoleIdBytes * sizeof( CHAR ) );
    
    hr = IXSystemImpl_XSystemGetConsoleId( xsystem, XSystemConsoleIdBytes, consoleId, &consoleIdUsed );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( strcmp( consoleId, "00000000.00000000.00000000.00000000.00" ) == 0, "unexpected consoleId. got %s.\n", debugstr_a( consoleId ) );
    ok( consoleIdUsed == XSystemConsoleIdBytes, "unexpected consoleIdUsed. got %lld.\n", consoleIdUsed );

    /**
     * xgameruntime.lib::XSystemGetXboxLiveSandboxId
     */
    sandboxId = (LPSTR)malloc( XSystemXboxLiveSandboxIdMaxBytes * sizeof( CHAR ) );
    
    hr = IXSystemImpl_XSystemGetXboxLiveSandboxId( xsystem, XSystemXboxLiveSandboxIdMaxBytes, sandboxId, &sandboxIdUsed );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( strcmp( sandboxId, "RETAIL" ) == 0, "unexpected sandboxId. got %s.\n", debugstr_a( sandboxId ) );
    ok( sandboxIdUsed == XSystemXboxLiveSandboxIdBytes, "unexpected sandboxIdUsed. got %lld.\n", sandboxIdUsed );

    /**
     * xgameruntime.lib::XSystemGetAppSpecificDeviceId
     */
    hr = IXSystemImpl_XSystemGetAppSpecificDeviceId( xsystem, XSystemAppSpecificDeviceIdBytes, NULL, NULL );
    todo_wine ok( hr == S_OK, "got error %#lx.\n", hr );

    /**
     * xgameruntime.lib::XSystemHandleTrack
     */
    hr = IXSystemImpl_XSystemHandleTrack( xsystem );
    todo_wine ok( hr == S_OK, "got error %#lx.\n", hr );

    /**
     * xgameruntime.lib::XSystemIsHandleValid
     */
    validHandle = IXSystemImpl_XSystemIsHandleValid( xsystem );
    ok( validHandle, "got validHandle %d\n", validHandle );

    /**
     * xgameruntime.lib::XSystemAllowFullDownloadBandwidth
     */
    hr = IXSystemImpl_XSystemAllowFullDownloadBandwidth( xsystem, TRUE );
    todo_wine ok( hr == S_OK, "got error %#lx.\n", hr );

    IXSystemImpl_Release( xsystem );
    free( consoleId );
    free( sandboxId );
}

static void test_XUserChangeRegistration(void)
{
    XTaskQueueRegistrationToken first = {0}, second = {0}, unknown = {0xdeadbeef};
    XUserChangeEventCallback callback = user_change_callback;
    IXUserBase *user, *again;
    IXUserGamertag *gamertag;
    HRESULT hr;

    hr = QueryApiImpl_fun( &test_clsid_xuser_impl, &test_iid_xuser_base, (void **)&user );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED( hr )) return;

    hr = IXUserBase_XUserRegisterForChangeEvent( user, NULL, NULL, NULL, &first );
    ok( hr == E_POINTER, "NULL callback returned %#lx.\n", hr );
    hr = IXUserBase_XUserRegisterForChangeEvent( user, NULL, NULL, &callback, NULL );
    ok( hr == E_POINTER, "NULL token returned %#lx.\n", hr );

    user_change_count = 0;
    hr = IXUserBase_XUserRegisterForChangeEvent( user, NULL, NULL, &callback, &first );
    ok( hr == S_OK && first.token, "first registration returned %#lx, token %llu.\n",
            hr, (unsigned long long)first.token );
    hr = IXUserBase_XUserRegisterForChangeEvent( user, NULL, NULL, &callback, &second );
    ok( hr == S_OK && second.token && second.token != first.token,
            "second registration returned %#lx, tokens %llu/%llu.\n", hr,
            (unsigned long long)first.token, (unsigned long long)second.token );
    ok( !user_change_count, "registration emitted %ld synthetic events.\n",
            user_change_count );

    ok( IXUserBase_XUserUnregisterForChangeEvent( user, first, FALSE ),
            "first unregister failed.\n" );
    ok( !IXUserBase_XUserUnregisterForChangeEvent( user, first, FALSE ),
            "duplicate unregister succeeded.\n" );
    ok( IXUserBase_XUserUnregisterForChangeEvent( user, second, TRUE ),
            "second unregister failed.\n" );
    ok( !IXUserBase_XUserUnregisterForChangeEvent( user, unknown, TRUE ),
            "unknown unregister succeeded.\n" );

    hr = IXUserBase_QueryInterface( user, &test_iid_xuser_gamertag,
                                    (void **)&gamertag );
    ok( hr == S_OK, "gamertag QueryInterface returned %#lx.\n", hr );
    if (SUCCEEDED( hr ))
    {
        ok( IXUserGamertag_AddRef( gamertag ) == 2,
                "provider gamertag AddRef is not stable.\n" );
        ok( IXUserGamertag_Release( gamertag ) == 1,
                "provider gamertag Release is not stable.\n" );
        IXUserGamertag_Release( gamertag );
    }
    ok( IXUserBase_AddRef( user ) == 2,
            "provider user AddRef is not stable.\n" );
    ok( IXUserBase_Release( user ) == 1,
            "provider user Release is not stable.\n" );

    IXUserBase_Release( user );
    hr = QueryApiImpl_fun( &test_clsid_xuser_impl, &test_iid_xuser_base, (void **)&again );
    ok( hr == S_OK, "provider was not reusable after Release, hr %#lx.\n", hr );
    if (SUCCEEDED( hr )) IXUserBase_Release( again );
}

static void test_XSystemAnalytics(void)
{
    IXSystemAnalyticsImpl *xsystem_analytics;
    XSystemAnalyticsInfo analyticsInfo;
    RTL_OSVERSIONINFOEXW version_info = {0};
    HRESULT hr;
    DWORD ubr;
    DWORD ubr_size = sizeof(ubr);
    HKEY ubr_registry_key;

    hr = QueryApiImpl_fun( &CLSID_XSystemAnalyticsImpl, &IID_IXSystemAnalyticsImpl, (void **)&xsystem_analytics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( xsystem_analytics, &IID_IUnknown, TRUE );
    check_interface( xsystem_analytics, &IID_IXSystemAnalyticsImpl, TRUE );

    /**
     * xgameruntime.lib::XSystemGetAnalyticsInfo
     */
    version_info.dwOSVersionInfoSize = sizeof( version_info );
    ok( RtlGetVersion( &version_info ) == 0, "RtlGetVersion failed.\n" );
    ok( SUCCEEDED( RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY, &ubr_registry_key ) ),
        "RegOpenKeyExW failed with code %ld.\n", GetLastError() );
    ok( SUCCEEDED( RegQueryValueExW( ubr_registry_key, L"UBR", NULL, NULL, (LPBYTE)&ubr, &ubr_size ) ),
        "RegQueryValueExW failed with code %ld.\n", GetLastError() );
    RegCloseKey( ubr_registry_key );
    ubr = (ubr & 0xFFFF); // <-- lower bits of UBR is the actual update revision.


    analyticsInfo = IXSystemAnalyticsImpl_XSystemGetAnalyticsInfo( xsystem_analytics );
    ok( analyticsInfo.osVersion.major == (UINT16)version_info.dwMajorVersion, 
        "major version %d differs from %d.\n", analyticsInfo.osVersion.major, (UINT16)version_info.dwMajorVersion);
    ok( analyticsInfo.osVersion.minor == (UINT16)version_info.dwMinorVersion, 
        "minor version %d differs from %d.\n", analyticsInfo.osVersion.minor, (UINT16)version_info.dwMinorVersion);
    ok( analyticsInfo.osVersion.build == (UINT16)version_info.dwBuildNumber, 
        "build number %d differs from %d.\n", analyticsInfo.osVersion.build, (UINT16)version_info.dwBuildNumber);
    ok( analyticsInfo.osVersion.revision == (UINT16)ubr, 
        "update revision %d differs from %d.\n", analyticsInfo.hostingOsVersion.revision, (UINT16)ubr);
    ok( analyticsInfo.hostingOsVersion.major == (UINT16)version_info.dwMajorVersion, 
        "host major version %d differs from %d.\n", analyticsInfo.hostingOsVersion.major, (UINT16)version_info.dwMajorVersion);
    ok( analyticsInfo.hostingOsVersion.minor == (UINT16)version_info.dwMinorVersion, 
        "host minor version %d differs from %d.\n", analyticsInfo.hostingOsVersion.minor, (UINT16)version_info.dwMinorVersion);
    ok( analyticsInfo.hostingOsVersion.build == (UINT16)version_info.dwBuildNumber, 
        "host build number %d differs from %d.\n", analyticsInfo.hostingOsVersion.build, (UINT16)version_info.dwBuildNumber);
    ok( analyticsInfo.hostingOsVersion.revision == (UINT16)ubr, 
        "host update revision %d differs from %d.\n", analyticsInfo.osVersion.revision, (UINT16)ubr);
    ok( strcmp( analyticsInfo.family, "Windows" ) == 0, "unexpected family %s.\n", debugstr_a( analyticsInfo.family ) );
    ok( strcmp( analyticsInfo.form, "Desktop" ) == 0, "unexpected form %s.\n", debugstr_a( analyticsInfo.form ) );

    IXSystemAnalyticsImpl_Release( xsystem_analytics );
}

static void test_XGameRuntimeFeature(void)
{
    IXGameRuntimeFeatureImpl *xgame_runtime_feature;
    HRESULT hr;
    BOOLEAN isAvailable;

    hr = QueryApiImpl_fun( &CLSID_XGameRuntimeFeatureImpl, &IID_IXGameRuntimeFeatureImpl, (void **)&xgame_runtime_feature );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( xgame_runtime_feature, &IID_IUnknown, TRUE );
    check_interface( xgame_runtime_feature, &IID_IXGameRuntimeFeatureImpl, TRUE );

    /**
     * xgameruntime.lib::XGameRuntimeIsFeatureAvailable
     */
    isAvailable = IXGameRuntimeFeatureImpl_XGameRuntimeIsFeatureAvailable( xgame_runtime_feature, XGame );
    ok( isAvailable, "got unexpected isAvailable %d.\n", isAvailable );

    IXGameRuntimeFeatureImpl_Release( xgame_runtime_feature );
}

static void test_XThreading(void)
{
    IXThreadingImpl *xthreading;
    HRESULT hr;
    SIZE_T receivedBufferSize;
    SIZE_T bufferUsed;
    LPSTR receivedBuffer;

    hr = QueryApiImpl_fun( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( xthreading, &IID_IUnknown, TRUE );
    check_interface( xthreading, &IID_IXThreadingImpl, TRUE );

    {
        XAsyncBlock currentBlock = {0};
        XAsyncBlock foreignQueueBlock = {0};
        XTaskQueueHandle processHandle = NULL;
        XTaskQueueHandle taskHandle;

        async_do_work_count = 0;
        async_get_result_count = 0;
        async_cleanup_count = 0;
        async_cleanup_before_result = 0;
        async_completion_count = 0;

        hr = IXThreadingImpl_XTaskQueueCreate( xthreading, ThreadPool, ThreadPool,
                &taskHandle );
        ok( hr == S_OK, "got hr %#lx.\n", hr );

        IXThreadingImpl_XTaskQueueSetCurrentProcessTaskQueue( xthreading, taskHandle );
        ok( IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( xthreading,
                &processHandle ), "failed to get process task queue.\n" );
        IXThreadingImpl_XTaskQueueCloseHandle( xthreading, processHandle );
        ok( IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( xthreading,
                &processHandle ), "process task queue did not survive closing a returned handle.\n" );
        IXThreadingImpl_XTaskQueueCloseHandle( xthreading, processHandle );
        currentBlock.queue = taskHandle;
        currentBlock.callback = XAsyncCompletion_testCallback;

        hr = IXThreadingImpl_XAsyncBegin( xthreading, &currentBlock, NULL, NULL, NULL, XAsyncProvider_testCallback );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        IXThreadingImpl_XTaskQueueCloseHandle( xthreading, taskHandle );
        taskHandle = NULL;

        hr = IXThreadingImpl_XAsyncGetStatus( xthreading, &currentBlock, TRUE );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( async_do_work_count == 1, "got %ld DoWork calls.\n", async_do_work_count );
        ok( async_completion_count == 1, "got %ld completion callbacks.\n",
                async_completion_count );
        ok( async_cleanup_count == 0, "provider cleaned up before GetResult.\n" );

        hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, &currentBlock, &receivedBufferSize );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( receivedBufferSize == 7, "unexpected receivedBufferSize %llu.\n",
                (unsigned long long)receivedBufferSize );

        receivedBuffer = malloc( receivedBufferSize );
        hr = IXThreadingImpl_XAsyncGetResult( xthreading, &currentBlock, NULL, receivedBufferSize, (PVOID)receivedBuffer, &bufferUsed );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( bufferUsed == 7, "unexpected bufferUsed %llu.\n", (unsigned long long)bufferUsed );
        ok( strcmp( receivedBuffer, "foobar" ) == 0, "unexpected receivedBuffer %s.\n", debugstr_a( receivedBuffer ) );
        ok( async_get_result_count == 1, "got %ld GetResult calls.\n", async_get_result_count );
        ok( async_cleanup_before_result == 0, "provider cleaned up before GetResult.\n" );
        ok( async_cleanup_count == 1, "got %ld Cleanup calls.\n", async_cleanup_count );

        hr = IXThreadingImpl_XAsyncGetResult( xthreading, &currentBlock, NULL,
                receivedBufferSize, receivedBuffer, &bufferUsed );
        ok( hr == E_ILLEGAL_METHOD_CALL, "second GetResult returned %#lx.\n", hr );

        free( receivedBuffer );

        async_do_work_count = 0;
        async_get_result_count = 0;
        async_cleanup_count = 0;
        async_cleanup_before_result = 0;
        foreignQueueBlock.queue = (XTaskQueueHandle)(ULONG_PTR)1;

        hr = IXThreadingImpl_XAsyncBegin( xthreading, &foreignQueueBlock, NULL,
                NULL, NULL, XAsyncProvider_testCallback );
        ok( hr == S_OK, "foreign queue XAsyncBegin returned %#lx.\n", hr );
        hr = IXThreadingImpl_XAsyncGetStatus( xthreading, &foreignQueueBlock, TRUE );
        ok( hr == S_OK, "foreign queue XAsyncGetStatus returned %#lx.\n", hr );
        hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, &foreignQueueBlock,
                &receivedBufferSize );
        ok( hr == S_OK && receivedBufferSize == 7,
                "foreign queue result size returned %#lx, size %llu.\n", hr,
                (unsigned long long)receivedBufferSize );
        receivedBuffer = malloc( receivedBufferSize );
        hr = IXThreadingImpl_XAsyncGetResult( xthreading, &foreignQueueBlock,
                NULL, receivedBufferSize, receivedBuffer, &bufferUsed );
        ok( hr == S_OK, "foreign queue XAsyncGetResult returned %#lx.\n", hr );
        ok( !strcmp( receivedBuffer, "foobar" ), "unexpected foreign queue result %s.\n",
                debugstr_a( receivedBuffer ) );
        ok( async_cleanup_count == 1, "foreign queue got %ld Cleanup calls.\n",
                async_cleanup_count );
        free( receivedBuffer );

        IXThreadingImpl_XTaskQueueSetCurrentProcessTaskQueue( xthreading, NULL );
        memset( &foreignQueueBlock, 0, sizeof(foreignQueueBlock) );
        hr = IXThreadingImpl_XAsyncBegin( xthreading, &foreignQueueBlock, NULL,
                NULL, NULL, XAsyncProvider_testCallback );
        ok( hr == HRESULT_FROM_WIN32( ERROR_NO_TASK_QUEUE ),
                "disabled process queue XAsyncBegin returned %#lx.\n", hr );
    }
}

START_TEST(xgameruntime)
{
    HRESULT hr;

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(hr == S_OK, "RoInitialize failed, hr %#lx\n", hr);

    test_GDKComponentInit();
    test_XSystem();
    test_XSystemAnalytics();
    test_XGameRuntimeFeature();
    test_XThreading();
    test_XUserChangeRegistration();

    RoUninitialize();
}
