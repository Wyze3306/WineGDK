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

/* Whether to apply the in-game sign-in patches that force the signed-in state:
 * the XblInitialize gate, the isLoggedInWithMicrosoftAccount facet, and the
 * online-server join gate. Default ON. The launcher writes
 * HKLM\Software\Wine\WineGDK\ForceMsaFacet=0
 * to turn it OFF for users whose game crashes: forcing that state sends the game
 * down code paths that deref XSAPI account/session objects which never populate
 * under Wine on some setups (issue #17/#18).
 *
 * The passive sign-in lookup null guard remains enabled regardless. */
static BOOLEAN msa_force_enabled( void )
{
    HKEY key;
    DWORD val = 1, sz = sizeof(val), type = REG_DWORD;
    LONG r = RegOpenKeyExA( HKEY_LOCAL_MACHINE, "Software\\Wine\\WineGDK", 0,
                            KEY_READ, &key );
    if (r != ERROR_SUCCESS)
        return TRUE;                         /* key absent → default ON */
    r = RegQueryValueExA( key, "ForceMsaFacet", NULL, &type, (BYTE *)&val, &sz );
    RegCloseKey( key );
    return !(r == ERROR_SUCCESS && type == REG_DWORD && val == 0);
}

typedef HRESULT (WINAPI *InitializeApiImplEx2_ext)( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options );

HRESULT WINAPI InitializeApiImplEx2( ULONG gdkVer, ULONG gsVer, CHAR mode, INITIALIZE_OPTIONS *options )
{
    HRESULT hr;
    static BOOLEAN com_initialized = FALSE;
    /* Read once: master gate for the state-forcing sign-in patches. */
    BOOLEAN force = msa_force_enabled();

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

    /* NOP the credential check gate that blocks XblInitialize.
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
        if (force && !patched2)
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

    /* Force the game's isLoggedInWithMicrosoftAccount getter to TRUE.
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
     * are read at runtime, never hard-coded. Gated by ForceMsaFacet (default
     * ON) so users it crashes (issue #17/#18) can turn it off. */
    if (force)
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
                            /* 3. the value-getter lea sits a short distance
                             *    after the name lea — 0x13 in 1.26.20/.21 but
                             *    0x14 in 1.26.30 (the serializer shape shifts
                             *    between versions). Scan a small window for the
                             *    `(48|4C) 8D 05 disp32` whose target is the bool
                             *    getter `movzx eax,byte[rcx+disp8]; ret`
                             *    (0F B6 41 disp8 C3) and overwrite its exact
                             *    five-byte body with `xor eax,eax; inc eax; ret`.
                             *    Scanning by shape (not a
                             *    hard-coded offset) keeps this version-robust. */
                            int off;
                            for (off = 0x10; off <= 0x20 && !patched3; off++)
                            {
                                BYTE *gl = name_lea + off;
                                INT32 gdisp;
                                ULONG_PTR getter_rva;
                                BYTE *getter;
                                if (!((gl[0] == 0x48 || gl[0] == 0x4C) &&
                                      gl[1] == 0x8D && (gl[2] & 0xC7) == 0x05))
                                    continue;
                                gdisp = *(INT32 *)(gl + 3);
                                getter_rva = (ULONG_PTR)(gl + 7 - base) + gdisp;
                                if (getter_rva + 5 > size) continue;
                                getter = base + getter_rva;
                                if (getter[0] == 0x0F && getter[1] == 0xB6 &&
                                    getter[2] == 0x41 && getter[4] == 0xC3)
                                {
                                    if (VirtualProtect( getter, 5, PAGE_EXECUTE_READWRITE, &oldprot ))
                                    {
                                        getter[0] = 0x31; getter[1] = 0xC0;
                                        getter[2] = 0xFF; getter[3] = 0xC0;
                                        getter[4] = 0xC3;
                                        FlushInstructionCache( GetCurrentProcess(), getter, 5 );
                                        VirtualProtect( getter, 5, oldprot, &oldprot );
                                        ERR( "patched isLoggedInWithMicrosoftAccount getter at RVA 0x%lx (name_lea+0x%x)\n",
                                             (ULONG_PTR)getter_rva, off );
                                        patched3 = TRUE;
                                    }
                                }
                            }
                            if (!patched3)
                                ERR( "MSA value-getter not found in window after name_lea\n" );
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

    /* Unlock joining online Bedrock servers.
     * The connect dispatcher gates the join on an "online Xbox Live sign-in"
     * check that wrongly fails for our native login (returns
     * UserNeedsToBeSignedIn before any packet is sent); flip that branch.
     *
     * The gate's exact bytes drift between versions — both the field register
     * and the first vtable offset change (1.26.20-30: cmp byte[rsi+0x98],0 /
     * mov rax,[rax+0x278]; 1.26.40: cmp byte[r13+0x98],0 / +0x270). What stays
     * constant is the SHAPE:
     *     cmp byte[reg+disp32], 0   ; 80 [B8-BF] dd dd dd dd 00
     *     jne  <fail>               ; 75 rel8                 <- flip to EB
     *     mov  rax, [reg]           ; (48|49) 8B (rm 0..3,6,7)
     *     mov  rax, [rax+disp32]    ; 48 8B 80 dd dd dd dd     (1st vtable call)
     *     ... mov edx, 1 ...        ; BA 01 00 00 00           (2nd vtable call)
     * Match that structure with the offsets/registers wildcarded, confirm with
     * the `mov edx,1` tail, and patch only when EXACTLY ONE such site exists
     * (never flip an ambiguous match). Covers the whole 1.26.x line (20->40). */
    {
        static BOOLEAN patched4 = FALSE;
        if (force && !patched4)
        {
            HMODULE game = GetModuleHandleA( NULL );
            MODULEINFO modinfo;
            if (game && GetModuleInformation( GetCurrentProcess(), game, &modinfo, sizeof(modinfo) ))
            {
                BYTE *base = (BYTE *)modinfo.lpBaseOfDll;
                SIZE_T size = modinfo.SizeOfImage, i, j;
                BYTE *gate = NULL;
                int n = 0;
                for (i = 0; i + 19 < size; i++)
                {
                    BYTE rm;
                    BOOLEAN edx1 = FALSE;
                    if (base[i] != 0x80) continue;                       /* cmp byte */
                    if (base[i+1] < 0xB8 || base[i+1] > 0xBF) continue;  /* [reg+disp32] */
                    if (base[i+6] != 0x00) continue;                     /* ,0 */
                    if (base[i+7] != 0x75) continue;                     /* jne rel8 */
                    if (!(base[i+9] == 0x48 || base[i+9] == 0x49) ||
                        base[i+10] != 0x8B) continue;                    /* mov rax,[reg] */
                    rm = base[i+11];
                    if (!(rm <= 0x03 || rm == 0x06 || rm == 0x07)) continue;
                    if (!(base[i+12] == 0x48 && base[i+13] == 0x8B &&
                          base[i+14] == 0x80)) continue;                 /* mov rax,[rax+disp32] */
                    for (j = i + 19; j + 5 <= size && j < i + 0x30; j++)
                        if (base[j] == 0xBA && base[j+1] == 0x01 && base[j+2] == 0x00 &&
                            base[j+3] == 0x00 && base[j+4] == 0x00) { edx1 = TRUE; break; }
                    if (!edx1) continue;                                 /* connect-gate tail */
                    gate = base + i + 7;                                 /* the jne */
                    if (++n > 1) break;                                  /* ambiguous -> bail */
                }
                if (n == 1)
                {
                    DWORD oldprot;
                    if (VirtualProtect( gate, 1, PAGE_EXECUTE_READWRITE, &oldprot ))
                    {
                        *gate = 0xEB;          /* jne -> jmp */
                        VirtualProtect( gate, 1, oldprot, &oldprot );
                        ERR( "patched online-server join gate at RVA 0x%lx\n", (ULONG_PTR)(gate - base) );
                        patched4 = TRUE;
                    }
                }
                else
                    ERR( "online-server join gate: %d candidate(s) (need exactly 1) - not patched\n", n );
            }
        }
    }

    /* Null-guard the sign-in-state lookup used by MSA facet forcing.
     * Where the MSA
     * account object is populated (most installs) that's all the game needs
     * and the Servers tab unlocks. But on some installs (a fresh Steam Deck
     * prefix, issue #17) the backing collection pointer is still NULL when an
     * early lookup runs, so this routine derefs NULL and page-faults seconds
     * after boot:
     *     mov rax,[rax+rdi+0x58]   ; selected collection (NULL there)
     *     mov rdx,[rax+8]          ; <-- #PF read [NULL+8]
     *     cmp byte[rdx+0x19],0 ; mov rcx,rax ; jne ...
     * The routine just looks a key up in one of two collections and returns a
     * byte; an empty (NULL) collection means "not found" = 0. We splice a
     * trampoline in after the collection load: it returns 0 when the pointer
     * is NULL and otherwise runs unchanged - so facet forcing keeps working where
     * the object exists (no behaviour change) and never crashes where it
     * doesn't. Version-robust: the body + `push rsi;push rdi;sub rsp,0x28`
     * prologue are byte-identical across 1.26.20..40 (verified static). We
     * anchor on the unique body signature, confirm the prologue, and patch
     * only when EXACTLY ONE site matches. */
    {
        static BOOLEAN patched5 = FALSE;
        if (!patched5)
        {
            HMODULE game = GetModuleHandleA( NULL );
            MODULEINFO modinfo;
            if (game && GetModuleInformation( GetCurrentProcess(), game, &modinfo, sizeof(modinfo) ))
            {
                BYTE *base = (BYTE *)modinfo.lpBaseOfDll;
                SIZE_T size = modinfo.SizeOfImage, i;
                /* body: mov rax,[rax+rdi+0x58]; mov rdx,[rax+8];
                 *       cmp byte[rdx+0x19],0; mov rcx,rax; jne */
                static const BYTE sig[] = {
                    0x48,0x8b,0x44,0x38,0x58, 0x48,0x8b,0x50,0x08,
                    0x80,0x7a,0x19,0x00, 0x48,0x89,0xc1, 0x75 };
                /* prologue 0x20 earlier: push rsi; push rdi; sub rsp,0x28 */
                static const BYTE prologue[] = { 0x56,0x57,0x48,0x83,0xec,0x28 };
                BYTE *match = NULL;
                int n = 0;
                for (i = 0; i + sizeof(sig) < size; i++)
                {
                    if (base[i] == 0x48 && memcmp( base + i, sig, sizeof(sig) ) == 0)
                    { match = base + i; if (++n > 1) break; }
                }
                if (n == 1 && (ULONG_PTR)(match - base) >= 0x20 &&
                    memcmp( match - 0x20, prologue, sizeof(prologue) ) == 0)
                {
                    /* locate the executable section holding the match, then a
                     * >=28-byte run of 0xCC (inter-function padding) inside it
                     * for the trampoline. */
                    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
                    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
                    BYTE *cave = NULL, *sec_base = NULL;
                    SIZE_T sec_size = 0, s, run = 0, cstart = 0, best = 0, bstart = 0;
                    ULONG_PTR mrva = (ULONG_PTR)(match - base);

                    if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                        nt->Signature == IMAGE_NT_SIGNATURE &&
                        nt->FileHeader.NumberOfSections > 0 &&
                        nt->FileHeader.NumberOfSections <= 96)
                    {
                        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
                        WORD k;
                        for (k = 0; k < nt->FileHeader.NumberOfSections; k++)
                        {
                            ULONG_PTR va = sec[k].VirtualAddress;
                            ULONG_PTR vsz = sec[k].Misc.VirtualSize;
                            if (mrva >= va && mrva < va + vsz &&
                                (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                            { sec_base = base + va; sec_size = vsz; break; }
                        }
                    }
                    for (s = 0; sec_base && s < sec_size; s++)
                    {
                        if (sec_base[s] == 0xCC)
                        {
                            if (run == 0) cstart = s;
                            if (++run > best) { best = run; bstart = cstart; }
                            if (best >= 42) break;       /* enough for the full guard */
                        }
                        else run = 0;
                    }
                    if (best >= 24) cave = sec_base + bstart;

                    if (cave)
                    {
                        ULONG_PTR crva = (ULONG_PTR)(cave - base);
                        INT32 to_cave = (INT32)(crva - (mrva + 5));
                        BOOLEAN full = (best >= 42);
                        DWORD oldprot;
                        /* The lookup walks a CHAIN of pointers:
                         *   mov rax,[rax+rdi+0x58]   ; the collection
                         *   mov rdx,[rax+8]          ; its backing data pointer
                         *   cmp byte[rdx+0x19],0     ; the byte it returns
                         * On a slow Steam Deck it runs before the collection is
                         * populated, so any link can be NULL or a garbage /
                         * non-canonical pointer and the routine page-faults
                         * (issue #21 rdx==NULL, issue #22 rax non-canonical,
                         * issue #25 rdx==-1/non-canonical — the backing data
                         * pointer was -1, which passed the old rdx!=NULL test
                         * and faulted on cmp byte[rdx+0x19]).
                         * "Empty/absent" is just a 0 return, so guard the WHOLE
                         * chain: return 0 unless every link is a sane pointer,
                         * else run the real lookup unchanged.
                         *
                         * Full guard (42 B — needs a big enough cave):
                         *   48 8b 44 38 58  mov rax,[rax+rdi+0x58]  (relocated)
                         *   48 89 c2        mov rdx,rax
                         *   48 c1 ea 2f     shr rdx,47          ; canonical?
                         *   75 13           jnz null_exit       ; non-canonical rax (#22)
                         *   48 85 c0        test rax,rax
                         *   74 0e           jz  null_exit       ; NULL rax
                         *   48 8b 50 08     mov rdx,[rax+8]     (relocated)
                         *   48 85 d2        test rdx,rdx
                         *   7e 05           jle null_exit       ; NULL or -1/non-canonical data ptr (#21,#25)
                         *   e9 rel32        jmp match+9 (the cmp)
                         * null_exit: 31 c0 / 48 83 c4 28 / 5f / 5e / c3
                         * Fallback (no >=42 B cave): the original 24-byte guard
                         * (rax!=NULL only) so a stingy binary never regresses. */
                        if (full && VirtualProtect( cave, 42, PAGE_EXECUTE_READWRITE, &oldprot ))
                        {
                            INT32 back = (INT32)((mrva + 9) - (crva + 33));
                            cave[0]=0x48; cave[1]=0x8b; cave[2]=0x44; cave[3]=0x38; cave[4]=0x58;
                            cave[5]=0x48; cave[6]=0x89; cave[7]=0xc2;
                            cave[8]=0x48; cave[9]=0xc1; cave[10]=0xea; cave[11]=0x2f;
                            cave[12]=0x75; cave[13]=0x13;
                            cave[14]=0x48; cave[15]=0x85; cave[16]=0xc0;
                            cave[17]=0x74; cave[18]=0x0e;
                            cave[19]=0x48; cave[20]=0x8b; cave[21]=0x50; cave[22]=0x08;
                            cave[23]=0x48; cave[24]=0x85; cave[25]=0xd2;  /* test rdx,rdx */
                            cave[26]=0x7e; cave[27]=0x05;  /* jle (was jz): also reject -1/non-canonical rdx (#25) */
                            cave[28]=0xe9;
                            cave[29]=(BYTE)back; cave[30]=(BYTE)(back>>8);
                            cave[31]=(BYTE)(back>>16); cave[32]=(BYTE)(back>>24);
                            cave[33]=0x31; cave[34]=0xc0;
                            cave[35]=0x48; cave[36]=0x83; cave[37]=0xc4; cave[38]=0x28;
                            cave[39]=0x5f; cave[40]=0x5e; cave[41]=0xc3;
                            VirtualProtect( cave, 42, oldprot, &oldprot );
                        }
                        else if (VirtualProtect( cave, 24, PAGE_EXECUTE_READWRITE, &oldprot ))
                        {
                            INT32 back = (INT32)((mrva + 5) - (crva + 0x0f));
                            full = FALSE;
                            cave[0]=0x48; cave[1]=0x8b; cave[2]=0x44; cave[3]=0x38; cave[4]=0x58;
                            cave[5]=0x48; cave[6]=0x85; cave[7]=0xc0;
                            cave[8]=0x74; cave[9]=0x05;
                            cave[10]=0xe9;
                            cave[11]=(BYTE)back; cave[12]=(BYTE)(back>>8);
                            cave[13]=(BYTE)(back>>16); cave[14]=(BYTE)(back>>24);
                            cave[15]=0x31; cave[16]=0xc0;
                            cave[17]=0x48; cave[18]=0x83; cave[19]=0xc4; cave[20]=0x28;
                            cave[21]=0x5f; cave[22]=0x5e; cave[23]=0xc3;
                            VirtualProtect( cave, 24, oldprot, &oldprot );
                        }
                        else cave = NULL;

                        if (cave && VirtualProtect( match, 5, PAGE_EXECUTE_READWRITE, &oldprot ))
                        {
                            match[0]=0xe9;
                            match[1]=(BYTE)to_cave; match[2]=(BYTE)(to_cave>>8);
                            match[3]=(BYTE)(to_cave>>16); match[4]=(BYTE)(to_cave>>24);
                            VirtualProtect( match, 5, oldprot, &oldprot );
                            ERR( "patched sign-in lookup null-guard at RVA 0x%lx (cave 0x%lx, %s)\n",
                                 (ULONG_PTR)mrva, (ULONG_PTR)crva, full ? "full chain" : "rax-only" );
                            patched5 = TRUE;
                        }
                    }
                    else
                        ERR( "sign-in lookup null-guard: no code cave found\n" );
                }
                else
                    ERR( "sign-in lookup null-guard: %d site(s) (need 1) or prologue mismatch - not patched\n", n );
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
