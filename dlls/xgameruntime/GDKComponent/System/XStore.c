/*
 * XStore composite stub for {0dd112ac-7c24-448c-b92b-3960fb5bd30c}
 * Uses proper XAsync pattern via the native threading DLL.
 */

#include "../../private.h"
#include "Threading/XAsync.h"
#include "Threading/XTaskQueue.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static LONG store_ref = 1;

static HRESULT WINAPI store_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p\n", iface, debugstr_guid( iid ), out );
    if (!out) return E_POINTER;
    *out = iface;
    store_ref++;
    return S_OK;
}

static ULONG WINAPI store_AddRef( void *iface ) { return InterlockedIncrement( &store_ref ); }
static ULONG WINAPI store_Release( void *iface ) { return InterlockedDecrement( &store_ref ); }

/* vtable[3]: XStoreCreateContext */
static HRESULT WINAPI store_CreateContext( void *iface, void *user, void **context )
{
    TRACE( "iface %p, user %p, context %p\n", iface, user, context );
    if (context) *context = iface;
    return S_OK;
}

/* --- XStore license async provider (proper XAsync pattern) --- */

static HRESULT CALLBACK store_license_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    if (!data) return E_POINTER;

    switch (op)
    {
        case Begin:
            return XAsyncSchedule( data->async, 0 );
        case DoWork:
            TRACE( "license DoWork\n" );
            XAsyncComplete( data->async, S_OK, 144 );
            break;
        case GetResult:
        {
            /* XStoreGameLicense: skuStoreId[64], isActive, isTrialOwned, isDiscLicense, isTrial,
               trialTimeRemaining(4), trialUniqueId[64], expirationDate(8) */
            char *p = (char *)data->buffer;
            memcpy( p, "9NBLGGH2JHXJ", 13 );  /* skuStoreId */
            p[64] = 1;  /* isActive = true */
            p[65] = 0;  /* isTrialOwnedByThisUser = false */
            p[66] = 0;  /* isDiscLicense = false */
            p[67] = 0;  /* isTrial = false */
            /* trialTimeRemainingInSeconds at offset 68 = 0 */
            /* trialUniqueId at offset 72 = empty */
            /* expirationDate at offset 136 = 0 (no expiry) */
            break;
        }
        case Cleanup:
        case Cancel:
            break;
    }
    return S_OK;
}

static HRESULT WINAPI store_QueryGameLicenseAsync( void *iface, void *context, void *asyncBlock )
{
    HRESULT hr;
    TRACE( "iface %p, context %p, asyncBlock %p\n", iface, context, asyncBlock );
    hr = XAsyncBegin( asyncBlock, NULL, store_QueryGameLicenseAsync, "XStoreQueryGameLicenseAsync", store_license_provider );
    TRACE( "XAsyncBegin returned 0x%08lx\n", hr );
    return hr;
}

static HRESULT WINAPI store_QueryGameLicenseResult( void *iface, void *asyncBlock, void *license )
{
    TRACE( "iface %p, asyncBlock %p, license %p\n", iface, asyncBlock, license );
    return XAsyncGetResult( asyncBlock, store_QueryGameLicenseAsync, 144, license, NULL );
}

/* --- XStore associated products async provider --- */

static HRESULT CALLBACK store_products_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    if (!data) return E_POINTER;

    switch (op)
    {
        case Begin:
            return XAsyncSchedule( data->async, 0 );
        case DoWork:
            TRACE( "products DoWork\n" );
            XAsyncComplete( data->async, S_OK, sizeof(void*) );
            break;
        case GetResult:
            memset( data->buffer, 0, data->bufferSize );
            break;
        case Cleanup:
        case Cancel:
            break;
    }
    return S_OK;
}

/* XStoreQueryAssociatedProductsAsync(this, storeContext, productKinds, maxItems, asyncBlock) */
static HRESULT WINAPI store_QueryAssociatedProductsAsync( void *iface, void *context, UINT32 kinds, UINT32 maxItems, void *asyncBlock )
{
    HRESULT hr;
    TRACE( "iface %p, context %p, kinds %u, maxItems %u, asyncBlock %p\n", iface, context, kinds, maxItems, asyncBlock );
    hr = XAsyncBegin( asyncBlock, NULL, store_QueryAssociatedProductsAsync, "XStoreQueryAssociatedProductsAsync", store_products_provider );
    TRACE( "XAsyncBegin returned 0x%08lx\n", hr );
    return hr;
}

static HRESULT WINAPI store_QueryAssociatedProductsResult( void *iface, void *asyncBlock, void **result )
{
    TRACE( "asyncBlock %p\n", asyncBlock );
    return XAsyncGetResult( asyncBlock, store_QueryAssociatedProductsAsync, sizeof(void*), result, NULL );
}

/* --- Per-slot stubs --- */

#define STORE_STUB(n) static HRESULT WINAPI store_stub_##n( void ) { FIXME( "XStore vtable[" #n "] called\n" ); return E_NOTIMPL; }
STORE_STUB(4)  STORE_STUB(7)
STORE_STUB(8)  STORE_STUB(9)  STORE_STUB(10) STORE_STUB(11)
STORE_STUB(12) STORE_STUB(13) STORE_STUB(14) STORE_STUB(15)
STORE_STUB(16) STORE_STUB(17) STORE_STUB(18) STORE_STUB(19)
STORE_STUB(20) STORE_STUB(21)

static BOOLEAN WINAPI store_return_true( void ) { return TRUE; }

STORE_STUB(23) STORE_STUB(24) STORE_STUB(25) STORE_STUB(26)
STORE_STUB(27) STORE_STUB(30)
STORE_STUB(31) STORE_STUB(32) STORE_STUB(33) STORE_STUB(34)
STORE_STUB(35) STORE_STUB(36) STORE_STUB(37) STORE_STUB(38)
STORE_STUB(39) STORE_STUB(40) STORE_STUB(41) STORE_STUB(42)
STORE_STUB(43) STORE_STUB(44) STORE_STUB(45) STORE_STUB(46)
STORE_STUB(47) STORE_STUB(48) STORE_STUB(49) STORE_STUB(50)
STORE_STUB(51) STORE_STUB(52) STORE_STUB(53) STORE_STUB(54)
STORE_STUB(55) STORE_STUB(56) STORE_STUB(57) STORE_STUB(58)
STORE_STUB(59) STORE_STUB(60) STORE_STUB(61) STORE_STUB(62)
STORE_STUB(63) STORE_STUB(64) STORE_STUB(65)

static HRESULT WINAPI store_stub_66_real( void *iface, void *monitor, void *progress )
{
    TRACE( "returning installed\n" );
    if (progress) memset( progress, 0, 32 );
    return S_OK;
}

STORE_STUB(67) STORE_STUB(68) STORE_STUB(69)
STORE_STUB(71) STORE_STUB(72) STORE_STUB(73) STORE_STUB(74)
STORE_STUB(75) STORE_STUB(76) STORE_STUB(77) STORE_STUB(78)
STORE_STUB(79) STORE_STUB(80) STORE_STUB(81) STORE_STUB(82)
STORE_STUB(83) STORE_STUB(88) STORE_STUB(89) STORE_STUB(90)
STORE_STUB(91) STORE_STUB(92) STORE_STUB(93) STORE_STUB(94)
STORE_STUB(95) STORE_STUB(96) STORE_STUB(97)

static HRESULT WINAPI store_noop( void ) { return S_OK; }

/* 98-entry vtable */
#define S(n) store_stub_##n
static const void *store_vtable[98] = {
    store_QueryInterface, store_AddRef, store_Release,           /* 0-2 */
    store_CreateContext,                                          /* 3 */
    S(4), store_QueryAssociatedProductsAsync, store_QueryAssociatedProductsResult, S(7),  /* 4-7 */
    store_QueryAssociatedProductsResult, S(9), S(10), S(11), S(12), S(13), S(14), S(15),  /* 8-15, [8]=result alias */
    S(16), S(17), S(18), S(19), S(20), S(21),                    /* 16-21 */
    store_return_true,                                            /* 22: LicenseIsValid */
    S(23), S(24), S(25), S(26), S(27),                           /* 23-27 */
    store_QueryGameLicenseAsync, store_QueryGameLicenseResult,    /* 28-29 */
    S(30), S(31), S(32), S(33), S(34), S(35), S(36), S(37), S(38),  /* 30-38 */
    S(39), S(40), S(41), S(42), S(43), S(44), S(45), S(46),     /* 39-46 */
    S(47), S(48), S(49), S(50), S(51), S(52), S(53), S(54),     /* 47-54 */
    S(55), S(56), S(57), S(58), S(59), S(60), S(61), S(62),     /* 55-62 */
    S(63), S(64), S(65), store_stub_66_real, S(67), S(68), S(69),  /* 63-69 */
    store_return_true,                                            /* 70: IsAvailable */
    S(71), S(72), S(73), S(74), S(75), S(76), S(77), S(78),     /* 71-78 */
    S(79), S(80), S(81), S(82), S(83),                           /* 79-83 */
    store_noop, store_noop, store_noop, store_noop,              /* 84-87 */
    S(88), S(89), S(90), S(91), S(92), S(93), S(94), S(95),     /* 88-95 */
    S(96), S(97),                                                 /* 96-97 */
};
#undef S

static struct { const void **vtbl; } store_instance = { store_vtable };

void *x_store_composite_get(void)
{
    TRACE( "returning store composite stub %p\n", &store_instance );
    return &store_instance;
}
