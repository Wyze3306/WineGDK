/*
 * Composite service broker for {af406016-e850-4aa8-a88d-2f3dcb9dac7e}
 * 141-entry vtable acting as a master registry for all GDK game services.
 * 12 groups of 8 sub-interface gateways + 21 lifecycle methods.
 */

#include "../../private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static LONG broker_ref = 1;

/* Lifecycle methods */
static HRESULT WINAPI broker_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s\n", iface, debugstr_guid( iid ) );
    if (!out) return E_POINTER;
    *out = iface;
    broker_ref++;
    return S_OK;
}

static ULONG WINAPI broker_AddRef( void *iface ) { return InterlockedIncrement( &broker_ref ); }
static ULONG WINAPI broker_Release( void *iface ) { return InterlockedDecrement( &broker_ref ); }

/* Sub-interface gateway stubs - return E_NOINTERFACE for unimplemented sub-services */
static HRESULT WINAPI broker_stub_qi( void ) { return E_NOINTERFACE; }
static HRESULT WINAPI broker_stub_noop( void ) { return S_OK; }
static HRESULT WINAPI broker_ret_zero( void ) { return 0; }

/* vtbl[136]: IsServiceAvailable(serviceId) -> BOOL
 * Range: 0..21. Returns TRUE for services we implement. */
static BOOLEAN WINAPI broker_IsServiceAvailable( void *iface, DWORD serviceId )
{
    TRACE( "serviceId %lu\n", serviceId );
    /* Report all services as available to prevent the game from
     * disabling features during sign-in */
    return TRUE;
}

/* Build 141-entry vtable.
 * Slots 0-119: 12 groups of 8 sub-interface gateways (mostly stubs)
 * Slots 120-140: lifecycle methods */
static const void *broker_vtable[141] = {
    /* Group 0 (slots 0-7): Outer/XPackage */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 1 (slots 8-15): XPersistentLocalStorage + XGameApplication */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 2 (slots 16-23): XNetworking / XMultiplayerNetworking */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 3 (slots 24-31): XAppCapture + XGameAccessibility */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 4 (slots 32-39): XAppCapture2 + XGameProtocol */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 5 (slots 40-47): XDisplayMode + XPackage2 */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 6 (slots 48-55): XGameConfig */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 7 (slots 56-63): XAnalyticsInfo + XGameStreaming */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 8 (slots 64-71): XStore + XGameEvent */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 9 (slots 72-79): XSystem */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 10 (slots 80-87): XGameInvite + XAppCapture3 */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 11 (slots 88-95): XGameEvent2 + XGameInvite2 */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 12 (slots 96-103): XGameStreaming2 */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 13 (slots 104-111): XGameProtocol2 + XGameUI */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Group 14 (slots 112-119): XQueryForwarder + XAsync */
    broker_stub_qi, broker_ret_zero, broker_stub_noop, broker_ret_zero,
    broker_stub_noop, broker_stub_noop, broker_stub_noop, broker_stub_noop,
    /* Lifecycle methods (slots 120-140) */
    broker_QueryInterface,          /* 120: QI */
    broker_AddRef,                  /* 121: AddRef */
    broker_Release,                 /* 122: Release */
    broker_ret_zero,                /* 123: GetThreadContext */
    broker_stub_noop,               /* 124: Disconnect */
    broker_QueryInterface,          /* 125: QI (dup) */
    broker_AddRef,                  /* 126: AddRef (dup) */
    broker_Release,                 /* 127: Release (dup) */
    broker_ret_zero,                /* 128: GetThreadContext (dup) */
    broker_stub_noop,               /* 129: Constructor */
    broker_ret_zero,                /* 130: stub */
    broker_ret_zero,                /* 131: stub */
    broker_ret_zero,                /* 132: stub */
    broker_QueryInterface,          /* 133: QI (dup) */
    broker_AddRef,                  /* 134: AddRef (dup) */
    broker_Release,                 /* 135: Release (dup) */
    broker_IsServiceAvailable,      /* 136: IsServiceAvailable */
    broker_stub_noop,               /* 137: Constructor (dup) */
    broker_ret_zero,                /* 138: stub */
    broker_ret_zero,                /* 139: stub */
    broker_ret_zero,                /* 140: stub */
};

static struct { const void **vtbl; } broker_instance = { broker_vtable };

void *x_service_broker_get(void)
{
    TRACE( "returning service broker %p\n", &broker_instance );
    return &broker_instance;
}
