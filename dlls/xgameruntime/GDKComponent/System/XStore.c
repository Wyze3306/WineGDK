/*
 * XStore composite stub for {0dd112ac-7c24-448c-b92b-3960fb5bd30c}
 * The game-license query is answered; the product catalog reports a store error.
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

/* --- XStore queries ---
 *
 * There is no Microsoft Store service behind this composite: it exists so the
 * title finds an XStore object at all, and the reconstructed vtable is the only
 * description we have of its slots.
 *
 * The game-license query is not optional even so.  Minecraft asks for the
 * license of the title itself, in WinGameCoreStore::_initializeLicenseAsync,
 * before the rest of its store comes up, and a store error there is not the
 * neutral answer it looks like: the title reads it as a transient fault, never
 * finishes store startup and asks again, so the query repeats for as long as
 * the session lasts and every screen waiting on the store reports an error.
 *
 * The copy the launcher installed came from the Microsoft Store under the
 * player's own account, so the license being asked after is one this title
 * already holds.  Answer it the way the service would - a full, active,
 * non-trial license with no expiry - on the caller's own async block and task
 * queue, through the threading implementation QueryApiImpl hands out.  That is
 * the route XUser's async calls already take.
 *
 * The associated-products query has no answer available: its result is an
 * opaque product-query handle nothing here can produce.  Completing it with a
 * zeroed one is exactly what an earlier stub did, and a signed-in title read
 * that as a store that answered, marked its offer repository loaded and walked
 * containers the enumeration never filled (issue #171).  It keeps reporting a
 * store error, which store code already has to handle. */

/* The buffer the title hands to XStoreQueryGameLicenseResult is 104 bytes:
 * Minecraft copies exactly that much out of it - six 16-byte moves and a
 * trailing qword - into its per-SKU license cache.  A SKU store id reads
 * <StoreId>/<sku>, seventeen characters, which fixes skuStoreId at 18 bytes
 * and leaves 64 for trialUniqueId.  The stub that used to fabricate a license
 * assumed 64 for both, declared a 144-byte result and so overran the caller's
 * frame by 40 bytes. */
#define STORE_SKU_ID_SIZE          18
#define STORE_TRIAL_UNIQUE_ID_SIZE 64

struct store_game_license
{
    char skuStoreId[STORE_SKU_ID_SIZE];
    BOOLEAN isActive;
    BOOLEAN isTrialOwnedByThisUser;
    BOOLEAN isDiscLicense;
    BOOLEAN isTrial;
    UINT32 trialTimeRemainingInSeconds;
    char trialUniqueId[STORE_TRIAL_UNIQUE_ID_SIZE];
    INT64 expirationDate;
};

C_ASSERT( sizeof(struct store_game_license) == 104 );
C_ASSERT( FIELD_OFFSET(struct store_game_license, isActive) == 18 );
C_ASSERT( FIELD_OFFSET(struct store_game_license, trialTimeRemainingInSeconds) == 24 );
C_ASSERT( FIELD_OFFSET(struct store_game_license, trialUniqueId) == 28 );
C_ASSERT( FIELD_OFFSET(struct store_game_license, expirationDate) == 96 );
C_ASSERT( sizeof(winegdk_game_store_id) == STORE_SKU_ID_SIZE );

static void store_fill_game_license( struct store_game_license *license )
{
    memset( license, 0, sizeof(*license) );

    /* The identity MicrosoftGame.Config declares for this title, when it
     * declares one; an absent store id leaves the field empty rather than
     * naming a SKU that is not ours. */
    if (SUCCEEDED( WineGDKLoadGameConfig() ))
        memcpy( license->skuStoreId, winegdk_game_store_id,
                sizeof(license->skuStoreId) );

    license->isActive = TRUE;
}

static HRESULT CALLBACK store_license_provider( XAsyncOp operation,
        const XAsyncProviderData *providerData )
{
    IXThreadingImpl *impl;
    HRESULT result = S_OK;

    TRACE( "operation %d, providerData %p\n", operation, providerData );

    if (!providerData) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
                              (void **)&impl ) ))
        return E_FAIL;

    switch (operation)
    {
        case Begin:
            result = IXThreadingImpl_XAsyncSchedule( impl, providerData->async, 0 );
            break;

        case DoWork:
            IXThreadingImpl_XAsyncComplete( impl, providerData->async, S_OK,
                                            sizeof(struct store_game_license) );
            break;

        case GetResult:
            if (providerData->bufferSize < sizeof(struct store_game_license))
                result = E_NOT_SUFFICIENT_BUFFER;
            else store_fill_game_license( providerData->buffer );
            break;

        case Cleanup:
        case Cancel:
            break;
    }

    IXThreadingImpl_Release( impl );
    return result;
}

static HRESULT WINAPI store_QueryGameLicenseAsync( void *iface, void *context, void *asyncBlock )
{
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, context %p, asyncBlock %p\n", iface, context, asyncBlock );

    if (!asyncBlock) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
                              (void **)&impl ) ))
    {
        WARN( "no threading implementation, failing the game-license query\n" );
        return E_GAMESTORE_NETWORK_ERROR;
    }

    hr = IXThreadingImpl_XAsyncBegin( impl, asyncBlock, NULL,
            store_QueryGameLicenseAsync, "XStoreQueryGameLicenseAsync",
            store_license_provider );
    IXThreadingImpl_Release( impl );
    /* Every task queue the title owns is one the native implementation made,
     * so this is the implementation that can dispatch the completion.  Say so
     * out loud if it ever refuses one: the title reads the failure as a store
     * outage and asks again for the rest of the session. */
    if (FAILED( hr ))
        WARN( "could not begin the game-license query, hr %#lx\n", hr );
    else TRACE( "XAsyncBegin returned 0x%08lx\n", hr );
    return hr;
}

static HRESULT WINAPI store_QueryGameLicenseResult( void *iface, void *asyncBlock, void *license )
{
    IXThreadingImpl *impl;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, license %p\n", iface, asyncBlock, license );

    if (!asyncBlock || !license) return E_POINTER;
    if (FAILED( QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl,
                              (void **)&impl ) ))
        return E_GAMESTORE_NETWORK_ERROR;

    hr = IXThreadingImpl_XAsyncGetResult( impl, asyncBlock,
            store_QueryGameLicenseAsync, sizeof(struct store_game_license),
            license, NULL );
    IXThreadingImpl_Release( impl );
    TRACE( "XAsyncGetResult returned 0x%08lx\n", hr );
    return hr;
}

/* XStoreQueryAssociatedProductsAsync(this, storeContext, productKinds, maxItems, asyncBlock) */
static HRESULT WINAPI store_QueryAssociatedProductsAsync( void *iface, void *context, UINT32 kinds, UINT32 maxItems, void *asyncBlock )
{
    TRACE( "iface %p, context %p, kinds %u, maxItems %u, asyncBlock %p\n", iface, context, kinds, maxItems, asyncBlock );
    WARN( "no store service available, failing the product query\n" );
    return E_GAMESTORE_NETWORK_ERROR;
}

static HRESULT WINAPI store_QueryAssociatedProductsResult( void *iface, void *asyncBlock, void **result )
{
    TRACE( "iface %p, asyncBlock %p, result %p\n", iface, asyncBlock, result );
    if (result) *result = NULL;
    return E_GAMESTORE_NETWORK_ERROR;
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
