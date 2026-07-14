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

static BOOLEAN online_patches_enabled( void )
{
    HKEY key;
    DWORD val = 0, sz = sizeof(val), type = 0;
    LONG r = RegOpenKeyExA( HKEY_LOCAL_MACHINE, "Software\\Wine\\WineGDK", 0,
                            KEY_READ, &key );
    if (r != ERROR_SUCCESS)
        return FALSE;
    r = RegQueryValueExA( key, "ForceMsaFacet", NULL, &type, (BYTE *)&val, &sz );
    RegCloseKey( key );
    return r == ERROR_SUCCESS && type == REG_DWORD && val != 0;
}

struct online_patch_sites
{
    BYTE *xbl_gate;
    BYTE *join_gate;
    SIZE_T xbl_rva;
    SIZE_T join_rva;
    INT32 xbl_jump;
    signed char xbl_local;
};

struct online_patch_layout
{
    DWORD timestamp;
    SIZE_T image_size;
    SIZE_T xbl_rva;
    SIZE_T join_rva;
};

static const struct online_patch_layout online_patch_layouts[] =
{
    {0x6a2af2e1, 0x11a13000, 0x0d7a8d18, 0x0017070c}, /* 1.26.30.5 */
    {0x6a3c31a7, 0x11a13000, 0x0d7a8b48, 0x0017070c}, /* 1.26.32.2 */
    {0x6a4e8d9a, 0x11a13000, 0x0d7a9168, 0x0017070c}, /* 1.26.33.1 */
};

static BOOLEAN executable_image_range( BYTE *base, SIZE_T image_size,
                                        IMAGE_NT_HEADERS64 *nt, SIZE_T rva,
                                        SIZE_T length )
{
    IMAGE_SECTION_HEADER *section;
    MEMORY_BASIC_INFORMATION memory;
    SIZE_T section_table, section_end, region_end;
    WORD i;

    if (!length || rva >= image_size || length > image_size - rva)
        return FALSE;
    section_table = (BYTE *)&nt->OptionalHeader - base +
                    nt->FileHeader.SizeOfOptionalHeader;
    if (!nt->FileHeader.NumberOfSections ||
        nt->FileHeader.NumberOfSections > 96 ||
        section_table > image_size ||
        nt->FileHeader.NumberOfSections >
            (image_size - section_table) / sizeof(*section))
        return FALSE;
    section = (IMAGE_SECTION_HEADER *)(base + section_table);

    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        SIZE_T span = max( section[i].Misc.VirtualSize,
                           section[i].SizeOfRawData );
        SIZE_T start = section[i].VirtualAddress;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            start >= image_size || span > image_size - start)
            continue;
        section_end = start + span;
        if (rva < start || rva + length > section_end)
            continue;
        if (!VirtualQuery( base + rva, &memory, sizeof(memory) ) ||
            memory.State != MEM_COMMIT ||
            memory.AllocationBase != base ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return FALSE;
        region_end = (BYTE *)memory.BaseAddress - base + memory.RegionSize;
        return region_end >= rva + length;
    }
    return FALSE;
}

static BOOLEAN locate_online_patch_sites( BYTE *base, SIZE_T size,
                                           struct online_patch_sites *sites )
{
    static const BYTE xbl_prefix[] = {0x83, 0x7d, 0xff, 0x00, 0x0f, 0x8c};
    static const BYTE xbl_suffix[] = {0x0f, 0x57, 0xc0, 0xf3, 0x0f, 0x7f, 0x45};
    static const BYTE xbl_target[] = {0x48, 0x8d, 0x4d};
    static const BYTE join_target[] = {0x31, 0xf6, 0x40, 0xb7, 0x01};
    const struct online_patch_layout *layout = NULL;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    BYTE *xbl_start, *xbl_destination, *join_start, *join_destination;
    SIZE_T nt_offset, i;
    INT32 xbl_jump;
    signed char join_jump;
    BOOLEAN has_edx_one = FALSE;

    memset( sites, 0, sizeof(*sites) );
    if (size < sizeof(*dos) || (dos = (IMAGE_DOS_HEADER *)base)->e_magic !=
        IMAGE_DOS_SIGNATURE)
        goto unsupported;
    nt_offset = dos->e_lfanew;
    if (nt_offset > size || sizeof(*nt) > size - nt_offset)
        goto unsupported;
    nt = (IMAGE_NT_HEADERS64 *)(base + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader !=
            sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage != size)
        goto unsupported;
    for (i = 0; i < ARRAY_SIZE(online_patch_layouts); ++i)
    {
        if (online_patch_layouts[i].timestamp == nt->FileHeader.TimeDateStamp &&
            online_patch_layouts[i].image_size == size)
        {
            layout = &online_patch_layouts[i];
            break;
        }
    }
    if (!layout)
        goto unsupported;
    if (layout->xbl_rva < 4 || layout->join_rva < 7 ||
        !executable_image_range( base, size, nt, layout->xbl_rva - 4, 18 ) ||
        !executable_image_range( base, size, nt, layout->join_rva - 7, 0x50 ))
        goto fingerprint_rejected;

    xbl_start = base + layout->xbl_rva - 4;
    if (memcmp( xbl_start, xbl_prefix, sizeof(xbl_prefix) ) ||
        memcmp( xbl_start + 10, xbl_suffix, sizeof(xbl_suffix) ) ||
        xbl_start[17] != 0xd7)
        goto fingerprint_rejected;
    memcpy( &xbl_jump, xbl_start + 6, sizeof(xbl_jump) );
    if (xbl_jump < 30 || xbl_jump > 0x10000 ||
        layout->xbl_rva + 6 + xbl_jump > size - 14)
        goto fingerprint_rejected;
    xbl_destination = base + layout->xbl_rva + 6 + xbl_jump;
    if (!executable_image_range( base, size, nt,
                                 xbl_destination - base, 14 ) ||
        memcmp( xbl_destination, xbl_target, sizeof(xbl_target) ) ||
        xbl_destination[4] != 0xe8 || xbl_destination[9] != 0x90 ||
        xbl_destination[10] != 0x48 || xbl_destination[11] != 0x8b ||
        xbl_destination[12] != 0xce || xbl_destination[13] != 0xe8)
        goto fingerprint_rejected;

    join_start = base + layout->join_rva - 7;
    if (join_start[0] != 0x80 || join_start[1] < 0xb8 ||
        join_start[1] > 0xbf || join_start[6] != 0x00 ||
        join_start[7] != 0x75 ||
        (join_start[9] != 0x48 && join_start[9] != 0x49) ||
        join_start[10] != 0x8b ||
        !(join_start[11] <= 0x03 || join_start[11] == 0x06 ||
          join_start[11] == 0x07) ||
        memcmp( join_start + 12, "\x48\x8b\x80", 3 ))
        goto fingerprint_rejected;
    for (i = 19; i + 5 <= 0x30; ++i)
    {
        if (!memcmp( join_start + i, "\xba\x01\x00\x00\x00", 5 ))
        {
            has_edx_one = TRUE;
            break;
        }
    }
    join_jump = (signed char)join_start[8];
    join_destination = join_start + 9 + join_jump;
    if (!has_edx_one || join_destination < join_start + 19 ||
        join_destination > join_start + 0x4b ||
        memcmp( join_destination, join_target, sizeof(join_target) ) ||
        !executable_image_range( base, size, nt,
                                 join_destination - base,
                                 sizeof(join_target) ))
        goto fingerprint_rejected;
    if ((layout->xbl_rva & 7) || (layout->join_rva & 3))
        goto fingerprint_rejected;

    sites->xbl_gate = base + layout->xbl_rva;
    sites->join_gate = base + layout->join_rva;
    sites->xbl_rva = layout->xbl_rva;
    sites->join_rva = layout->join_rva;
    sites->xbl_jump = xbl_jump;
    sites->xbl_local = (signed char)xbl_start[2];
    return TRUE;

unsupported:
    ERR( "unsupported Minecraft PE layout for guarded online patches\n" );
    return FALSE;
fingerprint_rejected:
    ERR( "known Minecraft online-patch fingerprint rejected\n" );
    return FALSE;
}

void WineGDKApplyOnlinePatches( BOOLEAN user_ready )
{
    static const BYTE xbl_replacement[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    static SRWLOCK lock = SRWLOCK_INIT;
    static BOOLEAN applied;
    struct online_patch_sites sites;
    MODULEINFO module;
    HMODULE game;
    DWORD xbl_protection, join_protection, ignored;
    LONG expected_join, replacement_join, previous_join;
    LONG64 expected_xbl, replacement_xbl, previous_xbl;
    BOOLEAN xbl_writable = FALSE;

    if (!online_patches_enabled())
        return;
    if (!user_ready)
    {
        ERR( "online patches skipped: XUser multiplayer credentials are incomplete\n" );
        return;
    }

    AcquireSRWLockExclusive( &lock );
    if (applied)
        goto done;

    game = GetModuleHandleA( NULL );
    if (!game || !GetModuleInformation( GetCurrentProcess(), game, &module,
                                         sizeof(module) ) ||
        !locate_online_patch_sites( module.lpBaseOfDll, module.SizeOfImage,
                                    &sites ))
        goto done;

    if (!VirtualProtect( sites.xbl_gate, 8, PAGE_EXECUTE_READWRITE,
                         &xbl_protection ))
    {
        ERR( "could not make XblInitialize gate writable, error %lu\n",
             GetLastError() );
        goto done;
    }
    xbl_writable = TRUE;
    if (!VirtualProtect( sites.join_gate, 4, PAGE_EXECUTE_READWRITE,
                         &join_protection ))
    {
        ERR( "could not make online-server gate writable, error %lu\n",
             GetLastError() );
        VirtualProtect( sites.xbl_gate, 8, xbl_protection, &ignored );
        goto done;
    }

    memcpy( &expected_join, sites.join_gate, sizeof(expected_join) );
    memcpy( &expected_xbl, sites.xbl_gate, sizeof(expected_xbl) );
    replacement_join = expected_join;
    replacement_xbl = expected_xbl;
    ((BYTE *)&replacement_join)[0] = 0xeb;
    memcpy( &replacement_xbl, xbl_replacement, sizeof(xbl_replacement) );
    if (((BYTE *)&expected_join)[0] != 0x75 ||
        ((BYTE *)&expected_join)[2] != 0x48 ||
        ((BYTE *)&expected_join)[3] != 0x8b ||
        ((BYTE *)&expected_xbl)[0] != 0x0f ||
        ((BYTE *)&expected_xbl)[1] != 0x8c)
    {
        ERR( "online patch sites changed before commit\n" );
        goto restore;
    }

    previous_join = InterlockedCompareExchange(
        (LONG volatile *)sites.join_gate, replacement_join, expected_join );
    if (previous_join != expected_join)
    {
        ERR( "online-server gate changed during atomic commit\n" );
        goto restore;
    }
    previous_xbl = InterlockedCompareExchange64(
        (LONG64 volatile *)sites.xbl_gate, replacement_xbl, expected_xbl );
    if (previous_xbl != expected_xbl)
    {
        ERR( "XblInitialize gate changed during atomic commit\n" );
        previous_join = InterlockedCompareExchange(
            (LONG volatile *)sites.join_gate, expected_join, replacement_join );
        if (previous_join != replacement_join)
            ERR( "online-server gate rollback failed\n" );
        goto restore;
    }
    if (!FlushInstructionCache( GetCurrentProcess(), sites.join_gate, 4 ) ||
        !FlushInstructionCache( GetCurrentProcess(), sites.xbl_gate, 8 ))
    {
        ERR( "could not flush committed online patch instructions, error %lu\n",
             GetLastError() );
    }
    applied = TRUE;

restore:
    if (!VirtualProtect( sites.join_gate, 4, join_protection, &ignored ))
        ERR( "could not restore online-server gate protection, error %lu\n",
             GetLastError() );
    if (xbl_writable &&
        !VirtualProtect( sites.xbl_gate, 8, xbl_protection, &ignored ))
        ERR( "could not restore XblInitialize gate protection, error %lu\n",
             GetLastError() );
    if (applied)
        ERR( "user ready: committed XblInitialize gate at RVA 0x%llx and "
             "online-server gate at RVA 0x%llx (local=%d, jump=+%d)\n",
             (unsigned long long)sites.xbl_rva,
             (unsigned long long)sites.join_rva,
             sites.xbl_local, sites.xbl_jump );

done:
    ReleaseSRWLockExclusive( &lock );
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

    /* Unmapped GDK runtime classes: report not-implemented (the RE logger that
     * used to hand back a fake object here was removed — its fake objects could
     * fail-fast/NULL-crash MC at startup, and the 6 startup CLSIDs it captured
     * are not the in-game social path). */
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
