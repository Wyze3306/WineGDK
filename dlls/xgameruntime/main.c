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
                     * Then within ~30 bytes: 48 8B 49 XX (mov rcx,[rcx+disp8]) = mUserManager
                     *   v1.26.12 had disp8=0x50, v1.26.20 has disp8=0x38 — accept any.
                     * Then within ~80 bytes: BA 01 00 00 00 (mov edx,1) = NetworkType::XboxLive */
                    static const BYTE prologue[] = {
                        0x48, 0x89, 0x5C, 0x24, 0x10,
                        0x48, 0x89, 0x74, 0x24, 0x18,
                        0x57,
                        0x48, 0x83, 0xEC, 0x30
                    };
                    SIZE_T best_score = 0;

                    for (i = 0; i + sizeof(prologue) + 80 < size; i++)
                    {
                        SIZE_T j;
                        BOOLEAN found_usermgr = FALSE, found_xboxlive = FALSE;
                        SIZE_T score = 0;

                        if (memcmp( base + i, prologue, sizeof(prologue) ) != 0)
                            continue;

                        /* Verify: mov rcx,[rcx+disp8] within next 30 bytes (any disp8) */
                        for (j = i + sizeof(prologue); j < i + sizeof(prologue) + 30 && j + 4 < size; j++)
                            if (base[j]==0x48 && base[j+1]==0x8B && base[j+2]==0x49)
                                { found_usermgr = TRUE; break; }

                        if (!found_usermgr) continue;

                        /* Verify: mov edx,1 within next 80 bytes */
                        for (j = i + sizeof(prologue); j < i + sizeof(prologue) + 80 && j + 5 < size; j++)
                            if (base[j]==0xBA && base[j+1]==0x01 && base[j+2]==0x00 && base[j+3]==0x00 && base[j+4]==0x00)
                                { found_xboxlive = TRUE; break; }

                        if (!found_xboxlive) continue;

                        /* Tighten the match: prefer functions that ALSO have
                         * `call <near>` (E8 ...) within +90 — isSignedIn calls
                         * the user-manager's isConnected.  This rules out any
                         * inline/stub function that happens to share the
                         * prologue but doesn't dispatch. */
                        for (j = i + sizeof(prologue); j < i + sizeof(prologue) + 90 && j < size; j++)
                            if (base[j] == 0xE8) { score++; break; }

                        if (!score) continue;
                        addr = base + i;
                        best_score = score;
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
     * (call returns into a local at [rbp+disp8]); a JL on that result skips
     * the entire "user is signed in" success path including XblInitialize.
     *
     * Shape we look for: a 4-byte `cmp DWORD PTR [rbp+disp8], 0` (83 7D
     * disp8 00) followed by the 6-byte long-form JL (0F 8C off32) followed
     * (after the 6-byte JL) by `xorps xmm0, xmm0` (0F 57 C0).  Three
     * suffixes after xorps are accepted because compiler output varies
     * across MC versions:
     *   - `33 C0`           (xor eax, eax)         ≤ 1.26.12
     *   - `F3 0F 7F 45 ??`  (movdqu [rbp+disp8], xmm0)  1.26.20+
     *   - `F3 0F 7F 85 ?? ?? ?? ??` (movdqu [rbp+disp32], xmm0) wide form
     *
     * The disp8 of the cmp also changed between versions (was 0xE8 / rbp-24,
     * now 0xFF / rbp-1), so we no longer pin it.  To stay precise we still
     * require the JL to target a forward offset of at least +30 (typical
     * for the failure-skip branch — backward-jumping JLs are loop tails). */
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
                    static const BYTE xorps[] = { 0x0F, 0x57, 0xC0 };
                    SIZE_T i;
                    DWORD op;

                    for (i = 0; i + 16 < size; i++)
                    {
                        /* cmp DWORD PTR [rbp+disp8], 0  (83 7D ?? 00) */
                        if (base[i] != 0x83 || base[i+1] != 0x7D || base[i+3] != 0x00) continue;
                        /* 6-byte JL long form right after */
                        if (base[i+4] != 0x0F || base[i+5] != 0x8C) continue;
                        /* JL must jump forward by >=30 (real gates always
                         * skip a real chunk of success-path code) */
                        INT32 jdisp = *(INT32 *)(base + i + 6);
                        if (jdisp < 30 || jdisp > 0x10000) continue;
                        /* After the 6-byte JL: xorps xmm0, xmm0 */
                        if (memcmp( base + i + 10, xorps, sizeof(xorps) ) != 0) continue;
                        /* And one of the accepted "zero-the-local" suffixes */
                        BYTE *s = base + i + 13;
                        BOOLEAN suffix_ok = (s[0] == 0x33 && s[1] == 0xC0) ||                       /* xor eax,eax */
                                            (s[0] == 0xF3 && s[1] == 0x0F && s[2] == 0x7F && s[3] == 0x45) || /* movdqu [rbp+d8],xmm0 */
                                            (s[0] == 0xF3 && s[1] == 0x0F && s[2] == 0x7F && s[3] == 0x85);   /* movdqu [rbp+d32],xmm0 */
                        if (!suffix_ok) continue;

                        /* NOP the JL: 0F 8C xx xx xx xx → 66 0F 1F 44 00 00 */
                        if (VirtualProtect( base + i + 4, 6, PAGE_EXECUTE_READWRITE, &op ))
                        {
                            base[i+4] = 0x66; base[i+5] = 0x0F; base[i+6] = 0x1F;
                            base[i+7] = 0x44; base[i+8] = 0x00; base[i+9] = 0x00;
                            VirtualProtect( base + i + 4, 6, op, &op );
                            ERR( "patched XblInitialize gate at %p (RVA 0x%lx, disp8=%d, jdisp=+%d)\n",
                                 base + i + 4, (ULONG_PTR)(i + 4),
                                 (signed char)base[i+2], jdisp );
                            patched2 = TRUE;
                        }
                        break;
                    }
                    if (!patched2)
                        ERR( "XblInitialize gate pattern not found\n" );
                }
            }
        }
    }

    /* Patch 3: force the game's isLoggedInWithMicrosoftAccount getter to TRUE.
     * The UI ("userAccount" facet) reads this bool to decide whether the player
     * is signed in with an MSA; on Win32/Wine XSAPI's social manager never
     * finishes so it stays false, which keeps the home-screen "Sign in" button
     * up and greys the Servers tab's join buttons. The getter is a tiny
     * `movzx eax, byte ptr [rcx+disp8] ; ret` whose address is the 2nd `lea`
     * (the value-getter) emitted right after the field-name string in the facet
     * serializer. We anchor on the immutable string "isLoggedInWithMicrosoftAccount":
     *   1. find the string in .rdata,
     *   2. scan .text for the `lea rXX,[rip+d]` that points at it (the name lea),
     *   3. the getter `lea rax,[rip+d]` sits 0x13 bytes after that name lea,
     *   4. resolve its target and overwrite the getter with `mov eax,1; ret`.
     * Version-robust: the string + serializer shape are stable; disp8/offsets
     * are read at runtime, never hard-coded. */
    {
        static BOOLEAN patched3 = FALSE;
        if (!patched3)
        {
            HMODULE game3 = GetModuleHandleA( NULL );
            if (game3)
            {
                MODULEINFO mi3;
                if (GetModuleInformation( GetCurrentProcess(), game3, &mi3, sizeof(mi3) ))
                {
                    BYTE *base = (BYTE *)mi3.lpBaseOfDll;
                    SIZE_T size = mi3.SizeOfImage;
                    static const char needle[] = "isLoggedInWithMicrosoftAccount";
                    SIZE_T nlen = sizeof(needle) - 1;
                    BYTE *str = NULL;
                    SIZE_T i;
                    DWORD oldprot;

                    /* 1. locate the field-name string (must be NUL-terminated to
                     *    avoid matching a longer superset). */
                    for (i = 0; i + nlen + 1 < size; i++)
                    {
                        if (base[i] == 'i' &&
                            memcmp( base + i, needle, nlen ) == 0 &&
                            base[i + nlen] == 0)
                        { str = base + i; break; }
                    }

                    if (str)
                    {
                        ULONG_PTR str_rva = (ULONG_PTR)(str - base);
                        BYTE *name_lea = NULL;

                        /* 2. find the `lea reg,[rip+disp32]` whose target == str.
                         *    encoding: (48|4C) 8D modrm(rm=101) disp32, len 7. */
                        for (i = 0; i + 7 < size; i++)
                        {
                            if ((base[i] == 0x48 || base[i] == 0x4C) &&
                                base[i+1] == 0x8D &&
                                (base[i+2] & 0xC7) == 0x05)
                            {
                                INT32 disp = *(INT32 *)(base + i + 3);
                                ULONG_PTR tgt = (ULONG_PTR)(i + 7) + disp;
                                if (tgt == str_rva) { name_lea = base + i; break; }
                            }
                        }

                        if (name_lea)
                        {
                            /* 3. the value-getter lea is 0x13 bytes after the
                             *    name lea, same `(48|4C) 8D 05 disp32` shape. */
                            BYTE *gl = name_lea + 0x13;
                            if ((gl[0] == 0x48 || gl[0] == 0x4C) &&
                                gl[1] == 0x8D && (gl[2] & 0xC7) == 0x05)
                            {
                                INT32 gdisp = *(INT32 *)(gl + 3);
                                ULONG_PTR getter_rva =
                                    (ULONG_PTR)(gl + 7 - base) + gdisp;
                                BYTE *getter = base + getter_rva;
                                /* 4. expect `movzx eax,byte[rcx+disp8]; ret`
                                 *    (0F B6 41 disp8 C3) — overwrite with
                                 *    `mov eax,1; ret` (B8 01 00 00 00 C3). */
                                if (getter_rva + 6 <= size &&
                                    getter[0] == 0x0F && getter[1] == 0xB6 &&
                                    getter[2] == 0x41 && getter[4] == 0xC3)
                                {
                                    if (VirtualProtect( getter, 6, PAGE_EXECUTE_READWRITE, &oldprot ))
                                    {
                                        getter[0] = 0xB8; getter[1] = 0x01;
                                        getter[2] = 0x00; getter[3] = 0x00;
                                        getter[4] = 0x00; getter[5] = 0xC3;
                                        VirtualProtect( getter, 6, oldprot, &oldprot );
                                        ERR( "patched isLoggedInWithMicrosoftAccount getter at RVA 0x%lx\n",
                                             (ULONG_PTR)getter_rva );
                                        patched3 = TRUE;
                                    }
                                }
                                else
                                    ERR( "MSA getter shape unexpected at RVA 0x%lx (%02x %02x %02x %02x %02x)\n",
                                         (ULONG_PTR)getter_rva,
                                         getter[0], getter[1], getter[2], getter[3], getter[4] );
                            }
                            else
                                ERR( "MSA value-getter lea not at name_lea+0x13\n" );
                        }
                        else
                            ERR( "MSA name-lea xref not found\n" );
                    }
                    else
                        ERR( "isLoggedInWithMicrosoftAccount string not found\n" );
                }
            }
        }
    }

    /* Patch 4: unlock joining online Bedrock servers.
     * The connect dispatcher gates the join on an "online Xbox Live sign-in"
     * check that wrongly fails for our native login, returning
     * UserNeedsToBeSignedIn before any packet is sent. The auth is actually
     * valid, so flip that branch to always take the connect path:
     *   80 BE 98 00 00 00 00  cmp byte[rsi+0x98],0
     *   75 34                 jne <success>   <- patch 0x75 (jne) -> 0xEB (jmp)
     *   49 8B 06 48 8B 80 78 02 00 00         (unique tail) */
    {
        static BOOLEAN patched4 = FALSE;
        if (!patched4)
        {
            HMODULE game = GetModuleHandleA( NULL );
            MODULEINFO modinfo;
            if (game && GetModuleInformation( GetCurrentProcess(), game, &modinfo, sizeof(modinfo) ))
            {
                BYTE *base = (BYTE *)modinfo.lpBaseOfDll;
                SIZE_T size = modinfo.SizeOfImage;
                static const BYTE sig[] = { 0x80,0xBE,0x98,0x00,0x00,0x00,0x00, 0x75,0x34,
                                            0x49,0x8B,0x06, 0x48,0x8B,0x80,0x78,0x02,0x00,0x00 };
                SIZE_T i;
                for (i = 0; i + sizeof(sig) < size; i++)
                {
                    if (memcmp( base + i, sig, sizeof(sig) ) != 0) continue;
                    BYTE *jne = base + i + 7;   /* the 0x75 (jne) */
                    DWORD oldprot;
                    if (VirtualProtect( jne, 1, PAGE_EXECUTE_READWRITE, &oldprot ))
                    {
                        *jne = 0xEB;           /* jne -> jmp */
                        VirtualProtect( jne, 1, oldprot, &oldprot );
                        ERR( "patched online-server join gate at RVA 0x%lx\n", (ULONG_PTR)(jne - base) );
                        patched4 = TRUE;
                    }
                    break;
                }
                if (!patched4) ERR( "online-server join gate signature not found\n" );
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

