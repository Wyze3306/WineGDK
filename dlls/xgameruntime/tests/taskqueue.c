/*
 * XTaskQueue tests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#define COBJMACROS
#include <initguid.h>
#include <windef.h>
#include <winbase.h>

#include "provider.h"
#include "wine/test.h"
#include "xthread.h"

#define GDKC_VERSION 10001L
#define GAMING_SERVICES_VERSION 3181L
#define IMMEDIATE_CALLBACK_COUNT 100000
#define SERIALIZED_CALLBACK_COUNT 2000
#define DELAYED_THREAD_COUNT 4
#define DELAYED_CALLBACKS_PER_THREAD 50

typedef HRESULT (*InitializeApiImpl)( ULONG gdkVer, ULONG gsVer );
typedef HRESULT (*QueryApiImpl)( const GUID *runtimeClassId, REFIID interfaceId, void **out );

struct immediate_context
{
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    LONG calls;
    LONG depth;
    LONG max_depth;
    HRESULT submit_hr;
};

struct serialized_context
{
    HANDLE event;
    LONG calls;
    LONG active;
    LONG max_active;
    LONG canceled;
};

struct submit_context
{
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    UINT32 delay;
    UINT32 count;
    LONG failures;
};

struct wake_context
{
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    HRESULT submit_hr;
};

struct event_context
{
    HANDLE event;
    LONG calls;
    LONG canceled;
};

struct close_context
{
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    HANDLE event;
    LONG calls;
    LONG canceled;
    LONG closed;
};

struct termination_context
{
    LONG calls;
    LONG canceled;
    LONG terminated;
};

static LONG delayed_calls, delayed_canceled;

static void update_max( LONG *target, LONG value )
{
    LONG current, previous;

    current = InterlockedCompareExchange( target, 0, 0 );
    while ( value > current )
    {
        previous = InterlockedCompareExchange( target, value, current );
        if ( previous == current ) break;
        current = previous;
    }
}

static void CALLBACK immediate_callback( void *context, BOOL canceled )
{
    struct immediate_context *impl = context;
    HRESULT hr;
    LONG calls, depth;

    ok( !canceled, "callback was canceled.\n" );

    depth = InterlockedIncrement( &impl->depth );
    update_max( &impl->max_depth, depth );
    calls = InterlockedIncrement( &impl->calls );

    if ( calls < IMMEDIATE_CALLBACK_COUNT )
    {
        hr = IXThreadingImpl_XTaskQueueSubmitCallback( impl->threading, impl->queue,
                Work, impl, immediate_callback );
        if ( FAILED( hr ) ) impl->submit_hr = hr;
    }

    InterlockedDecrement( &impl->depth );
}

static void CALLBACK serialized_callback( void *context, BOOL canceled )
{
    struct serialized_context *impl = context;
    LONG active, calls;

    if ( canceled ) InterlockedIncrement( &impl->canceled );
    active = InterlockedIncrement( &impl->active );
    update_max( &impl->max_active, active );
    SwitchToThread();
    calls = InterlockedIncrement( &impl->calls );
    InterlockedDecrement( &impl->active );
    if ( calls == SERIALIZED_CALLBACK_COUNT ) SetEvent( impl->event );
}

static void CALLBACK delayed_callback( void *context, BOOL canceled )
{
    (void)context;
    if ( canceled ) InterlockedIncrement( &delayed_canceled );
    InterlockedIncrement( &delayed_calls );
}

static DWORD WINAPI delayed_submit_thread( void *context )
{
    struct submit_context *impl = context;
    UINT32 i;
    HRESULT hr;

    for ( i = 0; i < impl->count; ++i )
    {
        hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( impl->threading,
                impl->queue, Work, impl->delay + i % 3, NULL, delayed_callback );
        if ( FAILED( hr ) ) InterlockedIncrement( &impl->failures );
    }
    return 0;
}

static DWORD WINAPI wake_submit_thread( void *context )
{
    struct wake_context *impl = context;

    Sleep( 20 );
    impl->submit_hr = IXThreadingImpl_XTaskQueueSubmitCallback( impl->threading,
            impl->queue, Work, NULL, delayed_callback );
    return 0;
}

static void CALLBACK event_callback( void *context, BOOL canceled )
{
    struct event_context *impl = context;

    if ( canceled ) InterlockedIncrement( &impl->canceled );
    InterlockedIncrement( &impl->calls );
    SetEvent( impl->event );
}

static void CALLBACK delayed_close_callback( void *context, BOOL canceled )
{
    struct close_context *impl = context;

    if ( canceled ) InterlockedIncrement( &impl->canceled );
    InterlockedIncrement( &impl->calls );
    IXThreadingImpl_XTaskQueueCloseHandle( impl->threading, impl->queue );
    InterlockedExchange( &impl->closed, 1 );
    SetEvent( impl->event );
}

static void CALLBACK termination_queue_callback( void *context, BOOL canceled )
{
    struct termination_context *impl = context;

    if ( canceled ) InterlockedIncrement( &impl->canceled );
    InterlockedIncrement( &impl->calls );
}

static void CALLBACK termination_callback( void *context )
{
    struct termination_context *impl = context;
    InterlockedIncrement( &impl->terminated );
}

static void test_immediate_queue( IXThreadingImpl *threading )
{
    struct immediate_context context = {0};
    XTaskQueueHandle queue;
    HRESULT hr;

    hr = IXThreadingImpl_XTaskQueueCreate( threading, Immediate, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    context.threading = threading;
    context.queue = queue;
    context.submit_hr = S_OK;

    hr = IXThreadingImpl_XTaskQueueSubmitCallback( threading, queue, Work,
            &context, immediate_callback );
    ok( hr == S_OK, "XTaskQueueSubmitCallback failed, hr %#lx.\n", hr );
    ok( context.submit_hr == S_OK, "nested submission failed, hr %#lx.\n", context.submit_hr );
    ok( context.calls == IMMEDIATE_CALLBACK_COUNT, "got %ld callbacks.\n", context.calls );
    ok( context.max_depth == 1, "callbacks recursed to depth %ld.\n", context.max_depth );

    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
}

static void test_serialized_queue( IXThreadingImpl *threading )
{
    struct serialized_context context = {0};
    XTaskQueueHandle queue;
    HRESULT hr;
    DWORD i, wait;

    context.event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( context.event != NULL, "CreateEventW failed, error %lu.\n", GetLastError() );

    hr = IXThreadingImpl_XTaskQueueCreate( threading, SerializedThreadPool, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) )
    {
        CloseHandle( context.event );
        return;
    }

    for ( i = 0; i < SERIALIZED_CALLBACK_COUNT; ++i )
    {
        hr = IXThreadingImpl_XTaskQueueSubmitCallback( threading, queue, Work,
                &context, serialized_callback );
        ok( hr == S_OK, "XTaskQueueSubmitCallback failed at %lu, hr %#lx.\n", i, hr );
        if ( FAILED( hr ) ) break;
    }

    wait = WaitForSingleObject( context.event, 10000 );
    ok( wait == WAIT_OBJECT_0, "serialized callback did not run, wait %#lx.\n", wait );
    ok( context.calls == SERIALIZED_CALLBACK_COUNT, "got %ld callbacks.\n", context.calls );
    ok( context.max_active == 1, "serialized callbacks reached concurrency %ld.\n",
            context.max_active );
    ok( !context.canceled, "got %ld canceled callbacks.\n", context.canceled );

    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
    CloseHandle( context.event );
}

static void test_delayed_queue( IXThreadingImpl *threading )
{
    XTaskQueueHandle queue;
    BOOLEAN dispatched;
    DWORD i;
    HRESULT hr;

    delayed_calls = 0;
    delayed_canceled = 0;
    hr = IXThreadingImpl_XTaskQueueCreate( threading, Manual, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    for ( i = 0; i < 3; ++i )
    {
        UINT32 delay = i < 2 ? 40 : 100;
        hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( threading, queue,
                Work, delay, NULL, delayed_callback );
        ok( hr == S_OK, "XTaskQueueSubmitDelayedCallback failed, hr %#lx.\n", hr );
    }

    dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 0 );
    ok( !dispatched, "delayed callback was dispatched immediately.\n" );
    ok( !delayed_calls, "got %ld premature callbacks.\n", delayed_calls );

    for ( i = 0; i < 3; ++i )
    {
        dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 2000 );
        ok( dispatched, "delayed callback %lu was not dispatched.\n", i );
    }
    ok( delayed_calls == 3, "got %ld delayed callbacks.\n", delayed_calls );

    dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 10 );
    ok( !dispatched, "empty queue did not time out.\n" );

    hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( threading, queue, Work,
            30, NULL, delayed_callback );
    ok( hr == S_OK, "post-drain delayed submission failed, hr %#lx.\n", hr );
    dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 2000 );
    ok( dispatched, "post-drain delayed callback was not dispatched.\n" );
    ok( delayed_calls == 4, "got %ld callbacks after post-drain submission.\n",
            delayed_calls );
    ok( !delayed_canceled, "got %ld canceled callbacks.\n", delayed_canceled );

    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
}

static void test_concurrent_delayed_queue( IXThreadingImpl *threading )
{
    struct submit_context contexts[DELAYED_THREAD_COUNT] = {0};
    HANDLE threads[DELAYED_THREAD_COUNT] = {0};
    XTaskQueueHandle queue;
    DWORD i, wait;
    BOOLEAN dispatched;
    HRESULT hr;

    delayed_calls = 0;
    delayed_canceled = 0;
    hr = IXThreadingImpl_XTaskQueueCreate( threading, Manual, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    for ( i = 0; i < DELAYED_THREAD_COUNT; ++i )
    {
        contexts[i].threading = threading;
        contexts[i].queue = queue;
        contexts[i].delay = 20 + i;
        contexts[i].count = DELAYED_CALLBACKS_PER_THREAD;
        threads[i] = CreateThread( NULL, 0, delayed_submit_thread, &contexts[i], 0, NULL );
        ok( threads[i] != NULL, "CreateThread failed, error %lu.\n", GetLastError() );
    }

    wait = WaitForMultipleObjects( DELAYED_THREAD_COUNT, threads, TRUE, 5000 );
    ok( wait == WAIT_OBJECT_0, "submission threads did not finish, wait %#lx.\n", wait );
    for ( i = 0; i < DELAYED_THREAD_COUNT; ++i )
    {
        ok( !contexts[i].failures, "thread %lu had %ld submission failures.\n",
                i, contexts[i].failures );
        if ( threads[i] ) CloseHandle( threads[i] );
    }

    while ( delayed_calls < DELAYED_THREAD_COUNT * DELAYED_CALLBACKS_PER_THREAD )
    {
        dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 2000 );
        if ( !dispatched ) break;
    }
    ok( delayed_calls == DELAYED_THREAD_COUNT * DELAYED_CALLBACKS_PER_THREAD,
            "got %ld concurrent delayed callbacks.\n", delayed_calls );
    ok( !delayed_canceled, "got %ld canceled callbacks.\n", delayed_canceled );

    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
}

static void test_blocking_dispatch( IXThreadingImpl *threading )
{
    struct wake_context context = {0};
    XTaskQueueHandle queue;
    BOOLEAN dispatched;
    HANDLE thread;
    HRESULT hr;

    delayed_calls = 0;
    delayed_canceled = 0;
    hr = IXThreadingImpl_XTaskQueueCreate( threading, Manual, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    context.threading = threading;
    context.queue = queue;
    context.submit_hr = E_FAIL;
    thread = CreateThread( NULL, 0, wake_submit_thread, &context, 0, NULL );
    ok( thread != NULL, "CreateThread failed, error %lu.\n", GetLastError() );

    dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 1000 );
    ok( dispatched, "blocking dispatch was not woken.\n" );
    if ( thread )
    {
        WaitForSingleObject( thread, 1000 );
        CloseHandle( thread );
    }
    ok( context.submit_hr == S_OK, "wake submission failed, hr %#lx.\n",
            context.submit_hr );
    ok( delayed_calls == 1, "got %ld wake callbacks.\n", delayed_calls );

    dispatched = IXThreadingImpl_XTaskQueueDispatch( threading, queue, Work, 10 );
    ok( !dispatched, "empty blocking dispatch did not time out.\n" );
    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
}

static void test_delayed_immediate_close( IXThreadingImpl *threading )
{
    struct event_context context = {0};
    XTaskQueueHandle queue;
    DWORD wait;
    HRESULT hr;

    context.event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( context.event != NULL, "CreateEventW failed, error %lu.\n", GetLastError() );

    hr = IXThreadingImpl_XTaskQueueCreate( threading, Immediate, Manual, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) )
    {
        CloseHandle( context.event );
        return;
    }

    hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( threading, queue, Work,
            20, &context, event_callback );
    ok( hr == S_OK, "immediate delayed submission failed, hr %#lx.\n", hr );
    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );

    wait = WaitForSingleObject( context.event, 2000 );
    ok( wait == WAIT_OBJECT_0, "immediate delayed callback did not run, wait %#lx.\n", wait );
    ok( context.calls == 1, "got %ld immediate delayed callbacks.\n", context.calls );
    ok( !context.canceled, "immediate delayed callback was canceled.\n" );
    CloseHandle( context.event );
}

static void test_delayed_callback_closes_queue( IXThreadingImpl *threading )
{
    struct close_context context = {0};
    DWORD wait;
    HRESULT hr;

    context.threading = threading;
    context.event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( context.event != NULL, "CreateEventW failed, error %lu.\n", GetLastError() );

    hr = IXThreadingImpl_XTaskQueueCreate( threading, Immediate, Manual, &context.queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) )
    {
        CloseHandle( context.event );
        return;
    }

    hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( threading, context.queue,
            Work, 20, &context, delayed_close_callback );
    ok( hr == S_OK, "immediate delayed submission failed, hr %#lx.\n", hr );

    wait = WaitForSingleObject( context.event, 2000 );
    ok( wait == WAIT_OBJECT_0, "delayed close callback did not run, wait %#lx.\n", wait );
    ok( context.calls == 1, "got %ld delayed close callbacks.\n", context.calls );
    ok( !context.canceled, "delayed close callback was canceled.\n" );

    if ( !InterlockedCompareExchange( &context.closed, 0, 0 ) )
        IXThreadingImpl_XTaskQueueCloseHandle( threading, context.queue );
    CloseHandle( context.event );
}

static void test_termination( IXThreadingImpl *threading )
{
    struct termination_context context = {0};
    XTaskQueueHandle queue;
    DWORD i;
    HRESULT hr;

    hr = IXThreadingImpl_XTaskQueueCreate( threading, SerializedThreadPool,
            SerializedThreadPool, &queue );
    ok( hr == S_OK, "XTaskQueueCreate failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    for ( i = 0; i < 100; ++i )
    {
        hr = IXThreadingImpl_XTaskQueueSubmitCallback( threading, queue, Work,
                &context, termination_queue_callback );
        ok( hr == S_OK, "termination submission failed at %lu, hr %#lx.\n", i, hr );
    }

    hr = IXThreadingImpl_XTaskQueueTerminate( threading, queue, TRUE, &context,
            termination_callback );
    ok( hr == S_OK, "XTaskQueueTerminate failed, hr %#lx.\n", hr );
    ok( context.calls == 100, "got %ld callbacks before termination.\n", context.calls );
    ok( context.terminated == 1, "got %ld termination callbacks.\n", context.terminated );

    IXThreadingImpl_XTaskQueueCloseHandle( threading, queue );
}

START_TEST(taskqueue)
{
    InitializeApiImpl initialize;
    QueryApiImpl query;
    IXThreadingImpl *threading;
    HMODULE module;
    HRESULT hr;

    module = LoadLibraryA( "xgameruntime.dll" );
    ok( module != NULL, "xgameruntime.dll failed to load, error %lu.\n", GetLastError() );
    if ( !module ) return;

    initialize = (InitializeApiImpl)GetProcAddress( module, "InitializeApiImpl" );
    query = (QueryApiImpl)GetProcAddress( module, "QueryApiImpl" );
    ok( initialize != NULL, "InitializeApiImpl is missing.\n" );
    ok( query != NULL, "QueryApiImpl is missing.\n" );
    if ( !initialize || !query ) return;

    hr = initialize( GDKC_VERSION, GAMING_SERVICES_VERSION );
    ok( hr == S_OK, "InitializeApiImpl failed, hr %#lx.\n", hr );

    hr = query( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading );
    ok( hr == S_OK, "QueryApiImpl failed, hr %#lx.\n", hr );
    if ( FAILED( hr ) ) return;

    test_immediate_queue( threading );
    test_serialized_queue( threading );
    test_delayed_queue( threading );
    test_concurrent_delayed_queue( threading );
    test_blocking_dispatch( threading );
    test_delayed_immediate_close( threading );
    test_delayed_callback_closes_queue( threading );
    test_termination( threading );

    IXThreadingImpl_Release( threading );
}
