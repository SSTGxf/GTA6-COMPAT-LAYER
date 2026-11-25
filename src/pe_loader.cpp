// pe_loader.cpp — Xbox Series X PE32+ loader (scaffolding).
// Same ISA as the host (x86-64), so the guest maps straight into the
// process and its imports are intercepted at the DLL boundary (ScarlettX /
// Xbox One shim-DLL model). No CPU emulation, no JIT.
#include "pe_loader.h"
#include "era_to_gdk.h"  // ERA -> GDK translation (phase 5)
#include "winrt_shim.h"  // WinRT CoreApplication shim (WinRT phase)
#include "hle_registry.h"
#include <cstdio>   // _snwprintf_s
#include <cstring>
#include <cwchar>    // wcsrchr
#include <dbghelp.h>  // MiniDumpWriteDump (crash dumps)
#include <intrin.h>  // __readgsqword
#include <string>
#include <tlhelp32.h>  // Build 84: thread enumeration for DR0 watchpoints
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace sc {

namespace {

// Diagnostic-only replacement for one zero-filled game TLS initializer.
// The address is fingerprinted in ApplyScarlettVtablePatch before use.
static void CALLBACK ScarlettNoopThreadInit(PVOID, DWORD, PVOID) {}

// Case-insensitive ASCII compare.
bool CiEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

const IMAGE_SECTION_HEADER* FindSection(const IMAGE_NT_HEADERS64* nt,
                                        const char* name) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp((const char*)sec[i].Name, name, 8) == 0) return &sec[i];
    }
    return nullptr;
}

const uint8_t* RvaToFile(const uint8_t* file, const IMAGE_NT_HEADERS64* nt,
                         uint32_t rva, uint32_t* outSize) {
    if (rva < nt->OptionalHeader.SizeOfHeaders) {
        if (outSize) *outSize = nt->OptionalHeader.SizeOfHeaders - rva;
        return file + rva;
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                              : sec[i].SizeOfRawData;
        if (rva >= va && rva < va + sz) {
            uint32_t off = sec[i].PointerToRawData + (rva - va);
            if (outSize) *outSize = sec[i].SizeOfRawData - (rva - va);
            return file + off;
        }
    }
    return nullptr;
}

// Parse "_xbld_edition_name=..." / "_xbld_edition_full_productbuild=..."
// strings out of the .xbld section (Xbox build-info section).
void ParseXbld(const uint8_t* data, size_t size, LoadedPe* out) {
    const char* kName = "_xbld_edition_name=";
    const char* kBuild = "_xbld_edition_full_productbuild=";
    for (size_t i = 0; i + 8 < size;) {
        const char* s = (const char*)data + i;
        size_t len = strnlen(s, size - i);
        if (len == 0) { ++i; continue; }
        if (strncmp(s, kName, strlen(kName)) == 0) {
            strncpy_s(out->xbldEdition, sizeof(out->xbldEdition),
                      s + strlen(kName), _TRUNCATE);
        } else if (strncmp(s, kBuild, strlen(kBuild)) == 0) {
            strncpy_s(out->xbldBuild, sizeof(out->xbldBuild),
                      s + strlen(kBuild), _TRUNCATE);
        }
        i += len + 1;
    }
    out->hasXbld = out->xbldEdition[0] != 0 || out->xbldBuild[0] != 0;
}

// Translate PE section characteristics to a VirtualProtect protection.
// NOTE (boot #10): a real Xbox title stores MUTABLE globals in .rdata
// — the game passes a .rdata address (RVA 0x62fc7d8) as the out-param
// of DStorageGetFactory, and PAGE_READONLY on .rdata faulted the write
// (0xc0000005, write AV at 0x1462fc7d8, in MakeStubComObject). On Xbox
// the data sections are effectively writable, so every non-executable
// section with READ is mapped READWRITE here. .text stays execute-read
// (or execute-readwrite if it carries WRITE). The synthetic guests
// verify this policy (they expect .rdata RW, not R — updated in
// build 19 alongside this change). This is a deliberate deviation from
// the PE characteristics, grounded in the game's observed behaviour.
DWORD SectionProtection(DWORD characteristics) {
    bool exec = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    DWORD prot;
    if (exec) {
        prot = write ? PAGE_EXECUTE_READWRITE
                     : (read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    } else if (read || write) {
        // Data sections (.rdata, .data, .pdata, .reloc, .tls, .xbld)
        // are writable: the game keeps mutable globals in .rdata.
        prot = PAGE_READWRITE;
    } else {
        prot = PAGE_NOACCESS;
    }
    if (characteristics & IMAGE_SCN_MEM_NOT_CACHED) prot |= PAGE_NOCACHE;
    return prot;
}

// Apply per-section page protections to the mapped image. Must run AFTER
// relocations + import resolution — both write into the image (relocations
// patch .text/.data, imports write the IAT in .rdata) — so the image is
// RWX until this point. Headers become read-only; each section gets the
// protection its characteristics ask for (text XR, rodata R, data RW).
void ApplySectionProtections(void* base, const IMAGE_NT_HEADERS64* nt) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const SIZE_T page = si.dwPageSize;
    SIZE_T headers = nt->OptionalHeader.SizeOfHeaders;
    SIZE_T headersRounded = (headers + page - 1) & ~(page - 1);
    {
        DWORD old = 0;
        if (!VirtualProtect(base, headersRounded, PAGE_READONLY, &old))
            Log("loader", "WARNING: header protection failed (%lu)",
                GetLastError());
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        SIZE_T vsize = sec[i].Misc.VirtualSize;
        if (vsize == 0) vsize = sec[i].SizeOfRawData;
        if (vsize == 0) continue;
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        SIZE_T start = (SIZE_T)base + sec[i].VirtualAddress;
        // Align the start down to a page, but never below the headers —
        // a section VA is page-aligned in a normal PE, this is defensive.
        SIZE_T alignedStart = start & ~(page - 1);
        if (alignedStart < (SIZE_T)base + headersRounded)
            alignedStart = (SIZE_T)base + headersRounded;
        SIZE_T end = start + vsize;
        SIZE_T alignedEnd = (end + page - 1) & ~(page - 1);
        if (alignedEnd <= alignedStart) continue;
        DWORD prot = SectionProtection(sec[i].Characteristics);
        DWORD old = 0;
        if (!VirtualProtect((void*)alignedStart, alignedEnd - alignedStart,
                            prot, &old)) {
            Log("loader", "WARNING: protection failed for %s (%lu)", name,
                GetLastError());
            continue;
        }
        Log("loader", "protect %s -> 0x%02x (%zu bytes)", name, prot,
            alignedEnd - alignedStart);
    }
}

// real-exe boot #3 fix (game_scarlett_master.exe): the title's global
// memory-pool singleton has a derived vtable whose slot 6 is a deliberate
// int3 trap (VA 0x1422106e0, bytes cc c3) in the shipped binary. During
// static init the allocator's slot 0 returns NULL (empty, capacity 0)
// and the guard calls slot 6 -> EXCEPTION_BREAKPOINT before main, before
// any file I/O. The base class's slot 6 is a real stub (VA 0x140004810:
// xor eax, eax; ret) and the caller ignores slot 6's return value, so
// point the derived slot 6 at the base stub. Single 8-byte patch at a
// fixed RVA; image base is stable (preferred 0x140000000), and all
// addresses are computed base-relative so a relocated image still works
// (relocations have already adjusted the vtable qword by then).
// Fingerprint-gated on all three values, so proof guests (which have
// neither the vtable nor the trap) are untouched. Runs while the image
// is still RWX, before section protections make .rdata read-only.
// Boot #5 (see the local boot #4 diagnosis): the vtable patch moved
// the fault one step — F itself (RVA 0x2219620) is a noreturn path with
// its own int3 at RVA 0x2219650 right after the slot-6 call, so slot 6
// returning is fatal inside F. NOP that int3 (0xcc -> 0x90) so F
// unlocks and returns and the guard returns NULL to its caller.
// Fingerprint-gated on the byte being 0xcc plus the call before it
// (FF 50 30 = call qword ptr [rax+0x30]) and the mov after it
// (4C 89 F1 = mov rcx, r14), all verified against the exe bytes on
// disk before patching.
void ApplyScarlettVtablePatch(void* base, const IMAGE_NT_HEADERS64* nt) {
    // RVAs from the boot #3 diagnosis (see the local boot #3 diagnosis):
    //   slot 6 qword  VA 0x1465f5450 -> RVA 0x65f5450
    //   trap          VA 0x1422106e0 -> RVA 0x22106e0
    //   base stub     VA 0x140004810 -> RVA 0x4810
    // F's int3 (boot #5, see the local boot #4 diagnosis):
    //   F int3        VA 0x142219650 -> RVA 0x2219650
    const uint32_t kSlotRva = 0x65f5450;
    const uint32_t kTrapRva = 0x22106e0;
    const uint32_t kStubRva = 0x4810;
    const uint32_t kFInt3Rva = 0x2219650;
    const SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    if ((SIZE_T)kSlotRva + 8 > imageSize ||
        (SIZE_T)kTrapRva + 2 > imageSize ||
        (SIZE_T)kStubRva + 3 > imageSize ||
        (SIZE_T)kFInt3Rva + 4 > imageSize) {
        return;  // Too small to hold the Scarlett title's vtable/trap.
    }
    uint8_t* img = (uint8_t*)base;
    uint64_t* slot = (uint64_t*)(img + kSlotRva);
    const uint8_t* trap = img + kTrapRva;
    const uint8_t* stub = img + kStubRva;
    // Fingerprint: slot 6 must point at the int3 trap, the trap must be
    // `cc c3`, and the base stub must be `31 c0 c3` (xor eax, eax; ret).
    if (*slot != (uint64_t)img + kTrapRva) return;
    if (trap[0] != 0xcc || trap[1] != 0xc3) return;
    if (stub[0] != 0x31 || stub[1] != 0xc0 || stub[2] != 0xc3) return;
    const uint64_t old = *slot;
    *slot = (uint64_t)img + kStubRva;
    Log("loader",
        "scarlett vtable patch: slot6 0x%llx -> base stub 0x%llx "
        "(was int3 trap)",
        (unsigned long long)old, (unsigned long long)*slot);
    // Boot #5: NOP F's own int3 so the patched slot-6 return does not
    // die inside F. Fingerprint: the byte must be 0xcc, preceded by
    // `call qword ptr [rax+0x30]` (FF 50 30) and followed by
    // `mov rcx, r14` (4C 89 F1) — all verified against the exe bytes
    // on disk (RVA 0x221964d: ff 50 30 cc 4c 89 f1). Runs in the same
    // RWX window as the vtable patch, before section protections.
    uint8_t* fInt3 = img + kFInt3Rva;
    if (fInt3[0] == 0xcc && fInt3[-3] == 0xff && fInt3[-2] == 0x50 &&
        fInt3[-1] == 0x30 && fInt3[1] == 0x4c && fInt3[2] == 0x89 &&
        fInt3[3] == 0xf1) {
        fInt3[0] = 0x90;
        Log("loader",
            "scarlett F int3 patch: 0xcc -> 0x90 at RVA 0x2219650 (NOP)");
    } else {
        Log("loader",
            "scarlett F int3 patch: fingerprint mismatch, skipped "
            "(byte 0x%02x, want 0xcc)",
            fInt3[0]);
    }
    // Diagnostic probe from the local real-exe dump: its dynamic TLS
    // initializer table slot 5 points at RVA 0x28c8ea0, where the image
    // contains 32 zero bytes and no runtime-function entry. Executing those
    // bytes attempts to write into .text and raises 0xc0000005. Redirect only
    // this exact image/table/pointer fingerprint to a no-op in the mapped
    // image, to reveal the next boot gate. This intentionally skips one
    // initializer and is not a semantic fix; the game file is never changed.
    const uint32_t kTlsInitTableSlot5Rva = 0x6791990;
    const uint32_t kTlsInitTargetRva = 0x28c8ea0;
    char tlsProbeEnv[2] = {};
    DWORD tlsProbeEnvLen = GetEnvironmentVariableA(
        "SCARLETT_DIAG_SKIP_TLS_INIT5", tlsProbeEnv,
        (DWORD)sizeof(tlsProbeEnv));
    const bool tlsProbeEnabled =
        tlsProbeEnvLen == 1 && tlsProbeEnv[0] == '1';
    if (imageSize == 0x0b06d000 && !tlsProbeEnabled) {
        Log("loader",
            "diagnostic TLS initializer[5] probe disabled; set "
            "SCARLETT_DIAG_SKIP_TLS_INIT5=1 to enable");
    }
    if (imageSize == 0x0b06d000 && tlsProbeEnabled &&
        (SIZE_T)kTlsInitTableSlot5Rva + sizeof(uint64_t) <= imageSize &&
        (SIZE_T)kTlsInitTargetRva + 0x20 <= imageSize) {
        uint64_t* entry = (uint64_t*)(img + kTlsInitTableSlot5Rva);
        const uint8_t* target = img + kTlsInitTargetRva;
        bool targetIsZero = true;
        for (size_t i = 0; i < 0x20; ++i) {
            if (target[i] != 0) {
                targetIsZero = false;
                break;
            }
        }
        const uint64_t expectedTarget = (uint64_t)img + kTlsInitTargetRva;
        if (*entry == expectedTarget && targetIsZero) {
            const uint64_t replacement =
                reinterpret_cast<uint64_t>(&ScarlettNoopThreadInit);
            *entry = replacement;
            Log("loader",
                "diagnostic TLS initializer[5]: 0x%llx -> no-op 0x%llx "
                "(zero-filled RVA 0x28c8ea0)",
                (unsigned long long)expectedTarget,
                (unsigned long long)replacement);
        } else {
            Log("loader",
                "diagnostic TLS initializer[5]: fingerprint mismatch "
                "(entry 0x%llx, zero-target %u), skipped",
                (unsigned long long)*entry, targetIsZero ? 1u : 0u);
        }
    }
    // A second opt-in probe for the next real-exe dump: initializer-table
    // slot 12 points at a TLS accessor thunk whose relative jump lands at
    // zero-filled RVA 0x4e72fd0 (no .pdata function entry). Let the caller
    // bypass it only when both the thunk bytes and target match this build.
    const uint32_t kTlsInitTableSlot12Rva = 0x67919c8;
    const uint32_t kTlsInitThunkRva = 0x20cf840;
    const uint32_t kTlsInitThunkTargetRva = 0x4e72fd0;
    char tlsProbe12Env[2] = {};
    DWORD tlsProbe12EnvLen = GetEnvironmentVariableA(
        "SCARLETT_DIAG_SKIP_TLS_INIT12", tlsProbe12Env,
        (DWORD)sizeof(tlsProbe12Env));
    const bool tlsProbe12Enabled =
        tlsProbe12EnvLen == 1 && tlsProbe12Env[0] == '1';
    if (imageSize == 0x0b06d000 && tlsProbe12Enabled &&
        (SIZE_T)kTlsInitTableSlot12Rva + sizeof(uint64_t) <= imageSize &&
        (SIZE_T)kTlsInitThunkRva + 24 <= imageSize &&
        (SIZE_T)kTlsInitThunkTargetRva + 0x20 <= imageSize) {
        static const uint8_t kExpectedThunk[24] = {
            0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00,
            0x00, 0x48, 0x8b, 0x00, 0x48, 0x8d, 0x88, 0x48,
            0x11, 0x00, 0x00, 0xe9, 0x78, 0x37, 0xda, 0x02};
        uint64_t* entry = (uint64_t*)(img + kTlsInitTableSlot12Rva);
        const uint64_t expectedThunk = (uint64_t)img + kTlsInitThunkRva;
        const uint8_t* thunk = img + kTlsInitThunkRva;
        const uint8_t* target = img + kTlsInitThunkTargetRva;
        bool targetIsZero = true;
        for (size_t i = 0; i < 0x20; ++i) {
            if (target[i] != 0) {
                targetIsZero = false;
                break;
            }
        }
        if (*entry == expectedThunk &&
            memcmp(thunk, kExpectedThunk, sizeof(kExpectedThunk)) == 0 &&
            targetIsZero) {
            const uint64_t replacement =
                reinterpret_cast<uint64_t>(&ScarlettNoopThreadInit);
            *entry = replacement;
            Log("loader",
                "diagnostic TLS initializer[12]: 0x%llx -> no-op 0x%llx "
                "(thunk RVA 0x20cf840 -> zero-filled RVA 0x4e72fd0)",
                (unsigned long long)expectedThunk,
                (unsigned long long)replacement);
        } else {
            Log("loader",
                "diagnostic TLS initializer[12] fingerprint mismatch "
                "(entry 0x%llx, thunk-match %u, zero-target %u), skipped",
                (unsigned long long)*entry,
                memcmp(thunk, kExpectedThunk, sizeof(kExpectedThunk)) == 0
                    ? 1u : 0u,
                targetIsZero ? 1u : 0u);
        }
    }
    // Boot #6 / H1 Option A (see the local boot #6 diagnosis):
    // grow the pool at construction so slot 0 returns a real block.
    // The pool is built as a 0x20-byte pool over a 0x60-byte BSS buffer;
    // the caller needs a 4-byte bitmap + a 0x5c0-byte entries array, so
    // slot 0 can never serve it as constructed. Patch the size constant
    // (0x20 -> 0x100000) and the buffer lea (BSS global -> a
    // layer-allocated 1 MB region, read through a scratch qword the
    // layer writes before entry runs). Fingerprint-gated on the exact
    // bytes below, verified against the exe bytes on disk first, so
    // proof guests are untouched. Allocation happens only after both
    // fingerprints pass.
    // NOTE (2026-11-23, applied from OPTIONA-IMPLEMENTATION-SPEC.md):
    // the spec wrote the buffer/scratch as RVAs 0x471be600/0x471be688,
    // but the exe bytes prove they are RVAs 0x71be600/0x71be688
    // (VA 0x1471be600/0x1471be688 minus image base 0x140000000;
    // the buffer lea at RVA 0x1cff465 targets RVA 0x71be600, and the
    // singleton lea just before it targets RVA 0x71be660). The spec's
    // own fingerprint (bufTarget == kPoolBufRva) would have failed
    // with the written values and skipped the patch, so the RVAs are
    // corrected here; the patch bytes and disp (0x54bf21c) are
    // unchanged.
    const uint32_t kPoolSizeRva = 0x1cff1e0;    // mov ecx, 0x20
    const uint32_t kPoolBufLeaRva = 0x1cff465;  // lea r8, [rip+...] (buffer)
    const uint32_t kPoolScratchRva = 0x71be688; // [obj+0x28], BSS, 0
    const uint32_t kPoolBufRva = 0x71be600;     // BSS buffer the lea loads
    const uint32_t kPoolSize = 0x100000;
    if ((SIZE_T)kPoolSizeRva + 5 > imageSize ||
        (SIZE_T)kPoolBufLeaRva + 7 > imageSize ||
        (SIZE_T)kPoolScratchRva + 8 > imageSize) {
        Log("loader", "scarlett pool patch: RVAs out of range, skipped");
        return;
    }
    // Fingerprints (all verified against the exe bytes on disk):
    //   size site:  b9 20 00 00 00, preceded by lea rsi,[rsp+0x28]
    //               (48 8d 74 24 28), followed by mov byte [rsi+0x10],0
    //               (c6 46 10 00).
    //   buffer site: 4c 8d 05 94 f1 4b 05, preceded by
    //               lea rcx,[rip+0x54bf1fb] (48 8d 0d fb f1 4b 05),
    //               followed by mov edx,8 (ba 08 00 00 00); the lea's
    //               target must be the BSS buffer (RVA 0x71be600).
    uint8_t* sizeSite = img + kPoolSizeRva;
    uint8_t* bufSite = img + kPoolBufLeaRva;
    const int32_t bufDisp =
        *(const int32_t*)(bufSite + 3);
    const uint32_t bufTarget =
        kPoolBufLeaRva + 7 + (uint32_t)bufDisp;
    const bool sizeOk =
        sizeSite[0] == 0xb9 && sizeSite[1] == 0x20 &&
        sizeSite[2] == 0x00 && sizeSite[3] == 0x00 &&
        sizeSite[4] == 0x00 &&
        sizeSite[-5] == 0x48 && sizeSite[-4] == 0x8d &&
        sizeSite[-3] == 0x74 && sizeSite[-2] == 0x24 &&
        sizeSite[-1] == 0x28 &&
        sizeSite[5] == 0xc6 && sizeSite[6] == 0x46 &&
        sizeSite[7] == 0x10 && sizeSite[8] == 0x00;
    const bool bufOk =
        bufSite[0] == 0x4c && bufSite[1] == 0x8d &&
        bufSite[2] == 0x05 && bufSite[3] == 0x94 &&
        bufSite[4] == 0xf1 && bufSite[5] == 0x4b &&
        bufSite[6] == 0x05 && bufTarget == kPoolBufRva &&
        bufSite[-7] == 0x48 && bufSite[-6] == 0x8d &&
        bufSite[-5] == 0x0d && bufSite[-4] == 0xfb &&
        bufSite[-3] == 0xf1 && bufSite[-2] == 0x4b &&
        bufSite[-1] == 0x05 &&
        bufSite[7] == 0xba && bufSite[8] == 0x08 &&
        bufSite[9] == 0x00 && bufSite[10] == 0x00 &&
        bufSite[11] == 0x00;
    if (!sizeOk || !bufOk) {
        Log("loader",
            "scarlett pool patch: fingerprint mismatch, skipped "
            "(size ok=%d, buffer ok=%d)",
            (int)sizeOk, (int)bufOk);
        return;
    }
    // Scratch qword must still be 0 (BSS); if something already wrote
    // it, do not clobber unknown state.
    uint64_t* scratch = (uint64_t*)(img + kPoolScratchRva);
    if (*scratch != 0) {
        Log("loader",
            "scarlett pool patch: scratch qword already 0x%llx, skipped",
            (unsigned long long)*scratch);
        return;
    }
    void* poolBuf = VirtualAlloc(nullptr, kPoolSize,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!poolBuf) {
        Log("loader", "scarlett pool patch: VirtualAlloc failed (%lu)",
            GetLastError());
        return;
    }
    *scratch = (uint64_t)poolBuf;
    Log("loader",
        "scarlett pool scratch: wrote buffer %p to RVA 0x71be688 "
        "([obj+0x28])",
        poolBuf);
    // Size: mov ecx, 0x20 -> mov ecx, 0x100000.
    sizeSite[0] = 0xb9;
    sizeSite[1] = 0x00;
    sizeSite[2] = 0x00;
    sizeSite[3] = 0x10;
    sizeSite[4] = 0x00;
    Log("loader",
        "scarlett pool size patch: mov ecx,0x20 -> mov ecx,0x100000 "
        "at RVA 0x1cff1e0");
    // Buffer: lea r8, [rip+0x54bf194] -> mov r8, [rip+0x54bf21c]
    // (loads the layer-written buffer address from the scratch qword).
    bufSite[0] = 0x4c;
    bufSite[1] = 0x8b;
    bufSite[2] = 0x05;
    bufSite[3] = 0x1c;
    bufSite[4] = 0xf2;
    bufSite[5] = 0x4b;
    bufSite[6] = 0x05;
    Log("loader",
        "scarlett pool buffer patch: lea r8,[BSS] -> mov r8,[scratch] "
        "at RVA 0x1cff465");
    // Boot #24: the pack-mgr fault fn (0x142236e00) gates its
    // push-back registrations on the first pack lookup
    // (0x14056eb10 for "appdata0.rpf"): `test al,al; je
    // 0x14223716c` at RVA 0x2236e75. The lookup returns 0 even
    // though the mount registry, the table, the dispatcher, the
    // default object and its slot 0x168 (xor eax,eax; ret) all
    // check out — so the push-backs never run, [rsi+0xc] stays 0,
    // and the assert-check at 0x14223716c int3s on the empty
    // count. NOP the 6-byte `je` so the fault fn always proceeds
    // to the push-backs (which register the pack mounts and append
    // to [rsi+0xc]). Fingerprint: 0F 84 <disp32>, disp must target
    // the assert-check RVA 0x223716c.
    const uint32_t kPackGateRva = 0x2236e75;
    const uint32_t kPackGateTargetRva = 0x223716c;
    if ((SIZE_T)kPackGateRva + 6 <= imageSize) {
        uint8_t* gateSite = img + kPackGateRva;
        const int32_t gateDisp = *(const int32_t*)(gateSite + 2);
        const uint32_t gateTarget =
            kPackGateRva + 6 + (uint32_t)gateDisp;
        const bool gateOk =
            gateSite[0] == 0x0f && gateSite[1] == 0x84 &&
            gateTarget == kPackGateTargetRva;
        if (!gateOk) {
            Log("loader",
                "scarlett pack-gate patch: fingerprint mismatch, "
                "skipped (target RVA 0x%x, want 0x%x)",
                gateTarget, kPackGateTargetRva);
        } else {
            for (int i = 0; i < 6; ++i) gateSite[i] = 0x90;
            Log("loader",
                "scarlett pack-gate patch: NOP je at RVA 0x2236e75 "
                "(fault fn proceeds to push-backs)");
        }
    } else {
        Log("loader",
            "scarlett pack-gate patch: RVA out of range, skipped");
    }
    // DIAGNOSTIC (boot #38): the init function 0x142478440 returns false
    // and its caller int3s at RVA 0x19f4f58 (`test al,al; jne +1; int3`).
    // The IDXGIDevice wrapper (build 57) did NOT fix it, and the exact
    // step inside 0x142478440 that returns false is not yet identified
    // (GetCustomHeapProperties is never reached — count 0 in boot #38).
    // NOP the single int3 byte so the game proceeds past the failed init
    // check and reveals the NEXT blocker. This is a diagnostic bypass,
    // not a fix: if the game crashes in a confusing place, the init
    // function was essential and this NOP gets reverted. Fingerprint:
    // bytes at RVA 0x19f4f54 must be 84 C0 75 01 CC
    // (test al,al / jne +1 / int3).
    const uint32_t kInitCheckRva = 0x19f4f58;
    if ((SIZE_T)kInitCheckRva + 1 <= imageSize) {
        uint8_t* int3Site = img + kInitCheckRva;
        const bool int3Ok =
            int3Site[-4] == 0x84 && int3Site[-3] == 0xc0 &&
            int3Site[-2] == 0x75 && int3Site[-1] == 0x01 &&
            int3Site[0] == 0xcc;
        if (!int3Ok) {
            Log("loader",
                "scarlett init-check patch: fingerprint mismatch, "
                "skipped");
        } else {
            int3Site[0] = 0x90;
            Log("loader",
                "scarlett init-check patch: NOP int3 at RVA 0x19f4f58 "
                "(DIAGNOSTIC — game proceeds past failed init check)");
        }
    } else {
        Log("loader",
            "scarlett init-check patch: RVA out of range, skipped");
    }
    // DIAGNOSTIC (boot #48): the descriptor-heap creator 0x141cf3280
    // asserts the CPU handle returned by heap slot 9 equals the base of
    // the game's 185 MB reservation ([0x1475DF188], set by 0x141e86440):
    //   test rbx,rbx; je +6; cmp r14,rbx; je +1; int3   (RVA 0x1cf3362)
    // where r14 = the slot-9 CPU handle and rbx = [0x1475DF188]. On
    // Xbox the heap is backed by that reservation, so the equality
    // holds; on desktop D3D12 the driver allocates the heap's storage
    // itself and the CPU handle is a driver pointer (boot #48:
    // 0x1bfe1beb1a0), so the assert can never hold. NOP the single
    // int3 byte so the game proceeds with the REAL CPU handle (which is
    // what the descriptor writes and the GPU actually need) and reveals
    // the NEXT blocker. This is a diagnostic bypass, not a fix: if the
    // game later depends on the CPU handle being the reservation base,
    // this NOP gets reverted and the heap backing rethought.
    // Fingerprint: bytes at RVA 0x1cf3362 must be
    // 48 85 DB 74 06 49 39 DE 74 01 CC
    // (test rbx,rbx / je +6 / cmp r14,rbx / je +1 / int3).
    const uint32_t kHeapHandleAssertRva = 0x1cf336c;
    if ((SIZE_T)kHeapHandleAssertRva + 1 <= imageSize) {
        uint8_t* heapAssertSite = img + kHeapHandleAssertRva;
        const bool heapAssertOk =
            heapAssertSite[-10] == 0x48 && heapAssertSite[-9] == 0x85 &&
            heapAssertSite[-8] == 0xdb && heapAssertSite[-7] == 0x74 &&
            heapAssertSite[-6] == 0x06 && heapAssertSite[-5] == 0x49 &&
            heapAssertSite[-4] == 0x39 && heapAssertSite[-3] == 0xde &&
            heapAssertSite[-2] == 0x74 && heapAssertSite[-1] == 0x01 &&
            heapAssertSite[0] == 0xcc;
        if (!heapAssertOk) {
            Log("loader",
                "scarlett heap-handle-assert patch: fingerprint mismatch, "
                "skipped");
        } else {
            heapAssertSite[0] = 0x90;
            Log("loader",
                "scarlett heap-handle-assert patch: NOP int3 at RVA "
                "0x1cf336c (DIAGNOSTIC — game proceeds with real CPU "
                "handle)");
        }
    } else {
        Log("loader",
            "scarlett heap-handle-assert patch: RVA out of range, skipped");
    }
    // Boot #17: the VFS initializer (0x1428c2010) zeroes the mount
    // registry (initFlag/count/array) with a single vmovups at RVA
    // 0x28c21b7 — which wipes the layer's pre-entry registry
    // population before pack-init ever runs. NOP that one instruction
    // so the populated registry survives static init. Fingerprint:
    // VEX vmovups [rip+disp32], xmm6 = c5 f8 11 35 <disp32>, and the
    // disp must target the mount init flag (RVA 0x78f5ec0).
    // The lock write just after it (mov [rip+...], rsi at 0x28c21d7)
    // is left alone — the lock still gets initialized.
    const uint32_t kVfsWipeRva = 0x28c21b7;
    const uint32_t kMountInitFlagTargetRva = 0x78f5ec0;
    if ((SIZE_T)kVfsWipeRva + 8 > imageSize) {
        Log("loader",
            "scarlett VFS wipe patch: RVA out of range, skipped");
        return;
    }
    uint8_t* wipeSite = img + kVfsWipeRva;
    const int32_t wipeDisp = *(const int32_t*)(wipeSite + 4);
    const uint32_t wipeTarget =
        kVfsWipeRva + 8 + (uint32_t)wipeDisp;
    const bool wipeOk =
        wipeSite[0] == 0xc5 && wipeSite[1] == 0xf8 &&
        wipeSite[2] == 0x11 && wipeSite[3] == 0x35 &&
        wipeTarget == kMountInitFlagTargetRva;
    if (!wipeOk) {
        Log("loader",
            "scarlett VFS wipe patch: fingerprint mismatch, skipped "
            "(target RVA 0x%x, want 0x%x)",
            wipeTarget, kMountInitFlagTargetRva);
        return;
    }
    for (int i = 0; i < 8; ++i) wipeSite[i] = 0x90;
    Log("loader",
        "scarlett VFS wipe patch: NOP vmovups at RVA 0x28c21b7 "
        "(mount registry survives VFS init)");
}

// Per-thread TLS for guest worker threads (phase 3). SetupTls fills
// this in for the main thread; a guest thread created through the
// guest's own CreateThread/CreateRemoteThread import gets the same
// template copy + .CRT$XLB callbacks via the trampoline below. The
// template/callback pointers live in the mapped image, which stays
// mapped for the guest's lifetime.
struct TlsTemplateInfo {
    const void* templateData = nullptr;
    SIZE_T templateSize = 0;
    DWORD index = 0;
    PIMAGE_TLS_CALLBACK* callbacks = nullptr;
    bool valid = false;
};
static TlsTemplateInfo g_tlsInfo;

// ---- TLS /GA view (Seth's decision 2026-11-23: Leonida's patch+view,
// approach (B), implemented clean-room from the digest description) ----
//
// The game reads its per-thread pool from TLS array[0] + 0x13a8 through
// a hardcoded slot-0 deref (boot #6: 9,084 sites). The real Windows
// loader always gives the process exe TLS index 0, but our loader's
// TlsAlloc() returns a higher index, so the exe block sits elsewhere.
// Approach (A) — put the block at array[0] + save/restore the host
// slot-0 owner — worked on the synthetic guests (build 13 green, era
// 21/21) but boot #7 still died on a worker thread at the same thunk
// (0x140104075, rcx=0x7fffffff), so Seth chose (B):
//
//  - PatchGaTlsReads: scan every executable section for the 9-byte
//    `mov r64, gs:[58h]` (the TLS-array load: 65 48/4C 8B modrm 25
//    58 00 00 00) and rewrite its disp32 from 0x58 to
//    0x1480 + 8*gaSlot, i.e. TEB->TlsSlots[gaSlot]. Same length, so
//    nothing else moves; runs while the image is RWX, before any
//    image code (TLS callbacks included).
//  - The /GA view: a per-thread fake 64-entry array (512 bytes) with
//    view[0] = view[tlsIndex] = the guest block, rest NULL, stored via
//    TlsSetValue(gaSlot, view) (+ a direct TEB TlsSlots write). The
//    exe's patched `mov r64, gs:[58h]` now loads the view, and
//    [view+0] is the exe's block. The real array[0] (the host DLL's
//    block) is never touched — no save/restore of the host slot-0
//    owner is needed.
//  - The guest's __tls_index slot is set to the TlsAlloc index, and
//    the block still goes to tlsArr[tlsIndex] too, so any TLS access
//    the patch misses still lands on the block through __tls_index.
//
// One TlsAlloc index for the /GA view, allocated once (process-wide
// index, per-thread value). Lazily allocated on first use.
static DWORD g_gaSlot = TLS_OUT_OF_INDEXES;
static DWORD GaSlot() {
    if (g_gaSlot == TLS_OUT_OF_INDEXES) g_gaSlot = TlsAlloc();
    return g_gaSlot;
}

// TEB->TlsSlots lives at TEB+0x1480 on x64 (64 slots). The patched
// `mov r64, gs:[58h]` reads gs:[0x1480 + 8*gaSlot] == TlsSlots[gaSlot].
static PVOID* TlsSlots() {
    uint8_t* teb = (uint8_t*)__readgsqword(0x30);  // TEB Self
    return (PVOID*)(teb + 0x1480);
}

// Build the per-thread /GA view for `block`: 64 entries, view[0] and
// view[tlsIndex] both point at the block, the rest NULL. Installs it
// in TlsSlots[gaSlot] (via TlsSetValue and a direct TEB write) and
// returns the view (owned by the caller; free with FreeGaView).
static void* InstallGaView(void* block, DWORD tlsIndex) {
    DWORD slot = GaSlot();
    if (slot == TLS_OUT_OF_INDEXES) return nullptr;
    void** view = (void**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    64 * sizeof(void*));
    if (!view) return nullptr;
    view[0] = block;
    if (tlsIndex < 64) view[tlsIndex] = block;
    TlsSetValue(slot, view);
    TlsSlots()[slot] = view;
    return view;
}

// Remove the calling thread's /GA view (frees it). No-op if none.
static void FreeGaView() {
    DWORD slot = g_gaSlot;
    if (slot == TLS_OUT_OF_INDEXES) return;
    void* view = TlsGetValue(slot);
    TlsSetValue(slot, nullptr);
    TlsSlots()[slot] = nullptr;
    if (view) HeapFree(GetProcessHeap(), 0, view);
}

// Rewrite every `mov r64, gs:[58h]` in the executable sections so it
// loads TlsSlots[gaSlot] (the /GA view) instead of the TLS array.
// Pattern (9 bytes): 65 (48|4C) 8B modrm 25 58 00 00 00, where modrm
// has mod=00, rm=100 (SIB follows) and reg = the destination register.
// Only the disp32 (bytes 5..8) is rewritten, to 0x1480 + 8*gaSlot;
// the instruction keeps its length. Returns the number of sites
// patched. Runs while the image is RWX, before section protections.
static size_t PatchGaTlsReads(void* base, const IMAGE_NT_HEADERS64* nt) {
    DWORD slot = GaSlot();
    if (slot == TLS_OUT_OF_INDEXES) {
        Log("loader", "WARNING: GA TLS patch skipped (no TlsAlloc slot)");
        return 0;
    }
    const DWORD newDisp = 0x1480 + 8 * slot;
    static const uint8_t kModrm[8] = {0x04, 0x0C, 0x14, 0x1C,
                                      0x24, 0x2C, 0x34, 0x3C};
    size_t patched = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* begin = (uint8_t*)base + sec->VirtualAddress;
        uint8_t* end = begin + sec->Misc.VirtualSize;
        for (uint8_t* p = begin; p + 9 <= end; ++p) {
            if (p[0] != 0x65) continue;
            if (p[1] != 0x48 && p[1] != 0x4C) continue;
            if (p[2] != 0x8B) continue;
            bool modrmOk = false;
            for (uint8_t m : kModrm) {
                if (p[3] == m) { modrmOk = true; break; }
            }
            if (!modrmOk) continue;
            if (p[4] != 0x25) continue;
            if (p[5] != 0x58 || p[6] != 0x00 || p[7] != 0x00 ||
                p[8] != 0x00) continue;
            *(DWORD*)(p + 5) = newDisp;  // rewrite only the disp32
            ++patched;
            p += 8;  // skip past this instruction
        }
    }
    Log("loader",
        "GA TLS patch: %zu mov r64, gs:[58h] sites -> TlsSlots[%lu] "
        "(disp 0x%x)",
        patched, slot, newDisp);
    return patched;
}

// Real thread-creation functions, captured when the guest's IAT is
// hooked (their FT entries hold the real addresses after import
// resolution).
using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T,
                                       LPTHREAD_START_ROUTINE, LPVOID,
                                       DWORD, LPDWORD);
using CreateRemoteThreadFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES,
                                             SIZE_T, LPTHREAD_START_ROUTINE,
                                             LPVOID, DWORD, LPDWORD);
static CreateThreadFn g_realCreateThread = nullptr;
static CreateRemoteThreadFn g_realCreateRemoteThread = nullptr;

// Guest-drive path rewrite (real-exe boot #26): the game builds pack paths
// as "G:/<name>" (the Xbox content drive; the pack table's entry string
// is "G:/") and opens them via CreateFileW / GetFileAttributesW through
// the VFS object's vtable slots 0x28 / 0x168. On Seth's PC, G: is a REAL
// USB drive (ESD-USB), so "G:/appdata0.rpf" does not exist there and the
// open returns -1 — which is why the pack-mgr lookup returns 0 and the
// sibling ctor NULL-derefs (boots #25/#26). DefineDosDevice cannot be
// used (it would shadow the real USB drive). Instead, the layer rewrites
// a leading "G:/" or "G:\" (case-insensitive) to the guest dir in the
// game's own file-API imports, so "G:/appdata0.rpf" opens
// D:\game\game\appdata0.rpf. Only the guest image's IAT is patched —
// the layer's own calls are untouched.
static wchar_t g_scarlettGuestDir[MAX_PATH] = {};
static LONG g_pathRewriteLogged = 0;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR,
                                             GET_FILEEX_INFO_LEVELS, LPVOID);
static CreateFileWFn g_realCreateFileW = nullptr;
static GetFileAttributesWFn g_realGetFileAttributesW = nullptr;
static GetFileAttributesExWFn g_realGetFileAttributesExW = nullptr;

// Rewrite a leading "G:/" or "G:\" to the guest dir. Returns true if
// rewritten (out holds the new path), false if the path is untouched.
static bool RewriteGuestDrivePath(LPCWSTR in, wchar_t* out,
                                  size_t outChars) {
    if (!in || !g_scarlettGuestDir[0]) return false;
    if (!((in[0] == L'G' || in[0] == L'g') && in[1] == L':' &&
          (in[2] == L'/' || in[2] == L'\\')))
        return false;
    // Skip "G:/" (3 chars); the rest is relative to the guest dir.
    const wchar_t* rest = in + 3;
    size_t gdLen = wcslen(g_scarlettGuestDir);
    if (gdLen + 1 + wcslen(rest) + 1 > outChars) return false;
    wcscpy_s(out, outChars, g_scarlettGuestDir);
    if (gdLen > 0 && g_scarlettGuestDir[gdLen - 1] != L'\\')
        wcscat_s(out, outChars, L"\\");
    size_t pos = wcslen(out);
    for (size_t i = 0; rest[i] && pos + 1 < outChars; ++i)
        out[pos++] = (rest[i] == L'/') ? L'\\' : rest[i];
    out[pos] = L'\0';
    if (InterlockedExchange(&g_pathRewriteLogged, 1) == 0)
        Log("loader", "path-rewrite: \"%ls\" -> \"%ls\"", in, out);
    return true;
}

static HANDLE WINAPI ScarlettCreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (!g_realCreateFileW) return INVALID_HANDLE_VALUE;
    wchar_t rewritten[MAX_PATH * 2];
    LPCWSTR name = lpFileName;
    if (RewriteGuestDrivePath(lpFileName, rewritten, _countof(rewritten)))
        name = rewritten;
    return g_realCreateFileW(name, dwDesiredAccess, dwShareMode,
                             lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
}

static DWORD WINAPI ScarlettGetFileAttributesW(LPCWSTR lpFileName) {
    if (!g_realGetFileAttributesW) return INVALID_FILE_ATTRIBUTES;
    wchar_t rewritten[MAX_PATH * 2];
    LPCWSTR name = lpFileName;
    if (RewriteGuestDrivePath(lpFileName, rewritten, _countof(rewritten)))
        name = rewritten;
    return g_realGetFileAttributesW(name);
}

static BOOL WINAPI ScarlettGetFileAttributesExW(
    LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFileInformation) {
    if (!g_realGetFileAttributesExW) return FALSE;
    wchar_t rewritten[MAX_PATH * 2];
    LPCWSTR name = lpFileName;
    if (RewriteGuestDrivePath(lpFileName, rewritten, _countof(rewritten)))
        name = rewritten;
    return g_realGetFileAttributesExW(name, fInfoLevelId, lpFileInformation);
}

// Set up TLS for the calling thread: allocate a block, copy the
// template, write it into the TEB TLS array, and run the .CRT$XLB
// callbacks with DLL_THREAD_ATTACH. Returns the block (owned by the
// caller) or nullptr. No-op when the guest has no TLS directory.
static void* EnsureThreadTls() {
    if (!g_tlsInfo.valid) return nullptr;
    void* block = nullptr;
    if (g_tlsInfo.templateSize > 0) {
        block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                          g_tlsInfo.templateSize);
        if (!block) return nullptr;
        memcpy(block, g_tlsInfo.templateData, g_tlsInfo.templateSize);
    }
    if (!TlsSetValue(g_tlsInfo.index, block)) {
        if (block) HeapFree(GetProcessHeap(), 0, block);
        return nullptr;
    }
    PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
    tlsArr[g_tlsInfo.index] = block;
    // Install this thread's /GA view so the guest's patched
    // `mov r64, gs:[58h]` loads the guest block (via TlsSlots[gaSlot]).
    // The real array[0] is never touched. Must run before the
    // DLL_THREAD_ATTACH callbacks below (guest code).
    InstallGaView(block, g_tlsInfo.index);
    if (g_tlsInfo.callbacks) {
        for (size_t i = 0; g_tlsInfo.callbacks[i]; ++i) {
            __try {
                g_tlsInfo.callbacks[i](nullptr, DLL_THREAD_ATTACH, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("loader",
                    "WARNING: TLS callback %zu faulted on thread attach "
                    "(0x%08x)",
                    i, GetExceptionCode());
                continue;
            }
        }
    }
    return block;
}

// Tear down TLS for the calling thread: DLL_THREAD_DETACH callbacks,
// clear the slot, free the block.
static void CleanupThreadTls() {
    if (!g_tlsInfo.valid) return;
    if (g_tlsInfo.callbacks) {
        for (size_t i = 0; g_tlsInfo.callbacks[i]; ++i) {
            __try {
                g_tlsInfo.callbacks[i](nullptr, DLL_THREAD_DETACH, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
    }
    void* block = TlsGetValue(g_tlsInfo.index);
    TlsSetValue(g_tlsInfo.index, nullptr);
    PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
    tlsArr[g_tlsInfo.index] = nullptr;
    // Remove this thread's /GA view (frees it).
    FreeGaView();
    if (block) HeapFree(GetProcessHeap(), 0, block);
}

struct ThreadStartInfo {
    LPTHREAD_START_ROUTINE realStart;
    LPVOID realParam;
};

static DWORD WINAPI ScarlettThreadStart(LPVOID param) {
    ThreadStartInfo* info = (ThreadStartInfo*)param;
    LPTHREAD_START_ROUTINE realStart = info->realStart;
    LPVOID realParam = info->realParam;
    HeapFree(GetProcessHeap(), 0, info);
    EnsureThreadTls();  // best effort; guest TLS vars work on this thread
    DWORD rc = realStart(realParam);
    CleanupThreadTls();
    return rc;
}

static HANDLE WINAPI ScarlettCreateThread(LPSECURITY_ATTRIBUTES sa,
                                          SIZE_T stack,
                                          LPTHREAD_START_ROUTINE start,
                                          LPVOID param, DWORD flags,
                                          LPDWORD threadId) {
    if (!g_realCreateThread) return nullptr;
    ThreadStartInfo* info = (ThreadStartInfo*)HeapAlloc(
        GetProcessHeap(), 0, sizeof(*info));
    if (!info) return nullptr;
    info->realStart = start;
    info->realParam = param;
    HANDLE h = g_realCreateThread(sa, stack, ScarlettThreadStart, info,
                                  flags, threadId);
    if (!h) HeapFree(GetProcessHeap(), 0, info);
    return h;
}

static HANDLE WINAPI ScarlettCreateRemoteThread(
    HANDLE hProcess, LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
    LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
    LPDWORD threadId) {
    if (!g_realCreateRemoteThread) return nullptr;
    // The trampoline lives in this process's layer DLL — it is only
    // valid for a thread in this process. Remote processes get the
    // real function unwrapped.
    if (hProcess != GetCurrentProcess()) {
        return g_realCreateRemoteThread(hProcess, sa, stack, start, param,
                                        flags, threadId);
    }
    ThreadStartInfo* info = (ThreadStartInfo*)HeapAlloc(
        GetProcessHeap(), 0, sizeof(*info));
    if (!info) return nullptr;
    info->realStart = start;
    info->realParam = param;
    HANDLE h = g_realCreateRemoteThread(hProcess, sa, stack,
                                        ScarlettThreadStart, info, flags,
                                        threadId);
    if (!h) HeapFree(GetProcessHeap(), 0, info);
    return h;
}

// Hook the guest's own CreateThread / CreateRemoteThread imports so
// worker threads get TLS set up. Runs after import resolution (the FT
// entries hold the real addresses) and before section protections
// (.rdata becomes read-only). Only the guest image's IAT is patched —
// the layer's own calls are untouched. Threads created any other way
// (CRT _beginthreadex internals, the OS, etc.) do not get guest TLS;
// that limitation stays documented.
void HookThreadCreation(const uint8_t* file, const IMAGE_NT_HEADERS64* nt,
                        void* base) {
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size) return;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
        file, nt, impDir.VirtualAddress, nullptr);
    for (; desc && desc->Name; ++desc) {
        const char* modName =
            (const char*)RvaToFile(file, nt, desc->Name, nullptr);
        // CreateThread/CreateRemoteThread live in kernel32, but a
        // modern (GDK) title imports them through the processthreads
        // API set (api-ms-win-core-processthreads-l1-1-0.dll), which
        // forwards to kernel32. Boot #8 proved the hook missed the
        // real exe for exactly this reason: its CreateThread import
        // is under the API-set name, so a worker thread was created
        // without ScarlettThreadStart (no guest TLS, no /GA view) and
        // faulted on a patched TLS read with a NULL view. Match the
        // API sets too (the WinRT hook already does this for combase).
        if (!modName) continue;
        bool isKernel32 =
            CiEq(modName, "kernel32.dll") ||
            CiEq(modName, "api-ms-win-core-processthreads-l1-1-0.dll") ||
            CiEq(modName, "api-ms-win-core-processthreads-l1-1-1.dll") ||
            CiEq(modName, "api-ms-win-core-processthreads-l1-1-2.dll") ||
            CiEq(modName, "api-ms-win-core-processthreads-l1-1-3.dll");
        if (!isKernel32) continue;
        uint32_t oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(file, nt, oftRva,
                                                         nullptr);
        auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
        if (!oft || !ft) continue;
        for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
            if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                file, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
            if (!ibn) continue;
            const char* fnName = (const char*)ibn->Name;
            if (CiEq(fnName, "CreateThread") && !g_realCreateThread) {
                g_realCreateThread =
                    (CreateThreadFn)ft[k].u1.Function;
                ft[k].u1.Function = (uint64_t)(void*)ScarlettCreateThread;
                Log("loader", "hooked kernel32!CreateThread for guest TLS");
            } else if (CiEq(fnName, "CreateRemoteThread") &&
                       !g_realCreateRemoteThread) {
                g_realCreateRemoteThread =
                    (CreateRemoteThreadFn)ft[k].u1.Function;
                ft[k].u1.Function =
                    (uint64_t)(void*)ScarlettCreateRemoteThread;
                Log("loader",
                    "hooked kernel32!CreateRemoteThread for guest TLS");
            }
        }
    }
}

// Crash-filter protection (real-exe boot #13): the guest imports
// kernel32!SetUnhandledExceptionFilter and its CRT replaces the
// layer's UnhandledFilter during static init. Boots #12/#13 died on
// an int3 assert (0x80000003) with NO layer dump and NO "guest
// crashed" log line — the unhandled filter never fired — while the
// boot #6 AVs (entry thread, CrashFilter) DID dump. The game's filter
// is a crash reporter/telemetry hook; the layer owns crash handling
// (WriteCrashDump), so the game's replacement is dropped and the
// layer's filter stays installed. Matches the import by NAME under any
// module (kernel32.dll or the errorhandling API set) — the exact
// descriptor the game uses was not pinned down, and the name is
// unambiguous.
// NOTE: this stub deliberately does NOT return the layer's filter
// (UnhandledFilter lives in namespace sc, outside the anonymous
// namespace this hook is in, so naming it here would create a second
// overload). It returns nullptr — "no previous filter" — which is safe:
// even if the game later tries to restore the returned value, the hook
// intercepts that call too and the layer filter stays installed.
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
ScarlettSetUnhandledExceptionFilter(
    LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
    (void)lpTopLevelExceptionFilter;
    Log("loader",
        "crash-filter: guest tried to replace UnhandledFilter; "
        "keeping the layer filter installed");
    return nullptr;
}

// Patches the guest's SetUnhandledExceptionFilter import (any module)
// to ScarlettSetUnhandledExceptionFilter. Runs in the same window as
// HookThreadCreation: after import resolution (FT holds the real
// addresses), before section protections (.rdata -> R). A missing
// import is fine (the guest may not use that path); the VEH installed
// in RunXboxPe is the backstop for the breakpoint case.
void HookCrashFilterProtection(const uint8_t* file,
                               const IMAGE_NT_HEADERS64* nt, void* base) {
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size) return;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
        file, nt, impDir.VirtualAddress, nullptr);
    for (; desc && desc->Name; ++desc) {
        uint32_t oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(file, nt, oftRva,
                                                         nullptr);
        auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
        if (!oft || !ft) continue;
        for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
            if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                file, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
            if (!ibn) continue;
            if (CiEq((const char*)ibn->Name,
                     "SetUnhandledExceptionFilter")) {
                ft[k].u1.Function =
                    (uint64_t)(void*)ScarlettSetUnhandledExceptionFilter;
                Log("loader",
                    "hooked SetUnhandledExceptionFilter for guest "
                    "crash-filter protection");
                return;
            }
        }
    }
}

// Patch the guest's file-API imports (CreateFileW, GetFileAttributesW,
// GetFileAttributesExW) to the path-rewrite stubs above (real-exe boot #26).
// Runs in the same window as HookThreadCreation /
// HookCrashFilterProtection: after import resolution (the FT entries hold
// the real addresses) and before section protections (.rdata -> R).
// Matches the import by NAME under any module (kernel32.dll or the file
// API set api-ms-win-core-file-l1-1-0.dll — the real exe imports
// CreateFileW / GetFileAttributesW from the API set, which is why the
// module is not pinned down). A missing import is fine; the guest may
// simply not use that path.
void HookGuestDrivePaths(const uint8_t* file, const IMAGE_NT_HEADERS64* nt,
                         void* base) {
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size) return;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
        file, nt, impDir.VirtualAddress, nullptr);
    for (; desc && desc->Name; ++desc) {
        uint32_t oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(file, nt, oftRva,
                                                         nullptr);
        auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
        if (!oft || !ft) continue;
        for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
            if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                file, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
            if (!ibn) continue;
            const char* fnName = (const char*)ibn->Name;
            if (CiEq(fnName, "CreateFileW") && !g_realCreateFileW) {
                g_realCreateFileW = (CreateFileWFn)ft[k].u1.Function;
                ft[k].u1.Function = (uint64_t)(void*)ScarlettCreateFileW;
                Log("loader",
                    "hooked CreateFileW for guest-drive path rewrite");
            } else if (CiEq(fnName, "GetFileAttributesW") &&
                       !g_realGetFileAttributesW) {
                g_realGetFileAttributesW =
                    (GetFileAttributesWFn)ft[k].u1.Function;
                ft[k].u1.Function =
                    (uint64_t)(void*)ScarlettGetFileAttributesW;
                Log("loader",
                    "hooked GetFileAttributesW for guest-drive path "
                    "rewrite");
            } else if (CiEq(fnName, "GetFileAttributesExW") &&
                       !g_realGetFileAttributesExW) {
                g_realGetFileAttributesExW =
                    (GetFileAttributesExWFn)ft[k].u1.Function;
                ft[k].u1.Function =
                    (uint64_t)(void*)ScarlettGetFileAttributesExW;
                Log("loader",
                    "hooked GetFileAttributesExW for guest-drive path "
                    "rewrite");
            }
        }
    }
}

// Hook the guest's WinRT activation imports so activation routes
// through the layer's CoreApplication shim (WinRT phase; replaces the
// phase 5 RoGet-only hook). Two interception points, because the guest
// reaches activation two ways:
//   - combase!RoGetActivationFactory (direct WinRT call) -> patched to
//     WinRtRoGetActivationFactory, which routes CoreApplication to the
//     shim and delegates everything else to EraRoGetActivationFactory
//     (phase 5 behaviour preserved).
//   - vccorlib110!GetActivationFactoryByPCWSTR — THE path an Xbox One
//     title actually uses (the guest imports it from vccorlib110 and
//     never imports RoGetActivationFactory directly; the old warning
//     "no RoGetActivationFactory import found (WinRT routing not
//     installed)" was therefore misleading — an Xbox One title boot
//     diagnosis proved it). Patched to
//     ScGetActivationFactoryByPCWSTR, with the real address stashed
//     for forwarding non-CoreApplication classes.
// Only installed for ERA titles (see the call site). Runs after import
// resolution (the FT holds the real addresses) and before section
// protections (.rdata -> R). A missing hook is logged, not fatal: the
// guest may simply not use that path.
void HookWinRtActivation(const uint8_t* file, const IMAGE_NT_HEADERS64* nt,
                         void* base) {
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size) return;
    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
        file, nt, impDir.VirtualAddress, nullptr);
    bool hookedRoGet = false;
    bool hookedVccorlib = false;
    for (; desc && desc->Name; ++desc) {
        const char* modName =
            (const char*)RvaToFile(file, nt, desc->Name, nullptr);
        if (!modName) continue;
        // RoGetActivationFactory's import descriptor is recorded under
        // the WinRT API set (api-ms-win-core-winrt-l1-1-0.dll) by
        // runtimeobject.lib — NOT combase.dll. The first build proved
        // it: dumpbin on the guest shows the RoGet import under that
        // API-set name, and the hook missed it until the name was
        // added here (the era guest's phase-5 WinRT stage was a false
        // pass — the real RoGet refused the Xbox class on its own).
        bool isCombase =
            CiEq(modName, "combase.dll") ||
            CiEq(modName, "api-ms-win-core-com-l1-1-0.dll") ||
            CiEq(modName, "api-ms-win-core-winrt-l1-1-0.dll");
        bool isVccorlib = CiEq(modName, "vccorlib110.dll");
        if (!isCombase && !isVccorlib) continue;
        uint32_t oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(file, nt, oftRva,
                                                         nullptr);
        auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
        if (!oft || !ft) continue;
        for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
            if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                file, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
            if (!ibn) continue;
            if (isCombase &&
                CiEq((const char*)ibn->Name, "RoGetActivationFactory") &&
                !hookedRoGet) {
                EraSetRealRoGetActivationFactory(
                    (RoGetActivationFactoryFn)ft[k].u1.Function);
                ft[k].u1.Function =
                    (uint64_t)(void*)WinRtRoGetActivationFactory;
                Log("loader",
                    "hooked combase!RoGetActivationFactory for WinRT "
                    "routing");
                hookedRoGet = true;
            } else if (isVccorlib &&
                       (CiEq((const char*)ibn->Name,
                             "GetActivationFactoryByPCWSTR") ||
                        // The real vccorlib110.dll exports
                        // GetActivationFactoryByPCWSTR ONLY under its
                        // C++ mangled name (undname: long __cdecl
                        // GetActivationFactoryByPCWSTR(void*,
                        // Platform::Guid&, void**)) — that is the name
                        // an Xbox One title imports, so the hook must
                        // match it too. The plain name is kept for
                        // robustness (a title could import it that way
                        // through a different import lib).
                        CiEq((const char*)ibn->Name,
                             "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid"
                             "@Platform@@PEAPEAX@Z")) &&
                       !hookedVccorlib) {
                WinRtSetRealGetActivationFactoryByPCWSTR(
                    (WinRtGetActivationFactoryByPCWSTRFn)ft[k].u1.Function);
                ft[k].u1.Function =
                    (uint64_t)(void*)ScGetActivationFactoryByPCWSTR;
                Log("loader",
                    "hooked vccorlib110!GetActivationFactoryByPCWSTR "
                    "for WinRT routing (the Xbox One title activation "
                    "path)");
                hookedVccorlib = true;
            }
        }
    }
    if (!hookedRoGet)
        Log("loader",
            "note: no RoGetActivationFactory import found — the guest "
            "activates via vccorlib110's GetActivationFactoryByPCWSTR "
            "instead; WinRT routing is installed there when that "
            "import exists");
    if (!hookedVccorlib)
        Log("loader",
            "note: no vccorlib110!GetActivationFactoryByPCWSTR import "
            "found");
    if (!hookedRoGet && !hookedVccorlib)
        Log("loader",
            "WARNING: ERA title but no WinRT activation import found "
            "(WinRT routing not installed)");
}

// Set up TLS for the calling thread: allocate a TLS slot + block, copy
// the template, and run the .CRT$XLB callbacks (DLL_PROCESS_ATTACH).
// The OS normally does this for a loaded module; a manually mapped PE
// gets none of it, so the loader emulates it. Reads the TLS directory
// from the MAPPED image (after relocations — its fields are VAs).
// Runs after protections: the index slot lives in .data (RW), the
// callbacks in .text (X), the template in .tls (R). Best effort: a
// failure logs a warning and the load continues.
bool SetupTls(void* base, const IMAGE_NT_HEADERS64* nt, LoadedPe* out) {
    const IMAGE_DATA_DIRECTORY& tlsDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!tlsDir.VirtualAddress) return true;
    auto* tls = (const IMAGE_TLS_DIRECTORY64*)((uint8_t*)base +
                                               tlsDir.VirtualAddress);
    // The layer keeps a TlsAlloc index for its own bookkeeping
    // (TlsSetValue/TlsGetValue/TlsFree + the direct TEB write at that
    // slot), and the GUEST's __tls_index slot is set to that same
    // index. The guest's hardcoded slot-0 reads (boot #6: the pool at
    // TLS array[0] + 0x13a8, 9,084 sites) are handled by the /GA view
    // instead: PatchGaTlsReads rewrote every `mov r64, gs:[58h]` to
    // load TlsSlots[gaSlot] (the view), and view[0] = view[index] =
    // the block. The real array[0] is never touched.
    DWORD index = TlsAlloc();
    if (index == TLS_OUT_OF_INDEXES) {
        Log("loader", "WARNING: TlsAlloc failed (%lu)", GetLastError());
        return false;
    }
    // The guest's __tls_index slot (a VA into the image, relocated):
    // the TlsAlloc index, so __declspec(thread) access via
    // __tls_index reads the block (through the /GA view, or through
    // tlsArr[index] if a site escaped the patch).
    DWORD* indexSlot = (DWORD*)tls->AddressOfIndex;
    *indexSlot = index;
    Log("loader",
        "TLS dir: rawdata [%p, %p) indexSlot %p (guest index %lu, layer "
        "TlsAlloc index %lu) callbacks %p",
        (void*)tls->StartAddressOfRawData,
        (void*)tls->EndAddressOfRawData, (void*)tls->AddressOfIndex,
        index, index, (void*)tls->AddressOfCallBacks);
    SIZE_T templateSize =
        (SIZE_T)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    void* block = nullptr;
    if (templateSize > 0) {
        block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, templateSize);
        if (!block) {
            Log("loader", "WARNING: TLS block alloc failed (%lu)",
                GetLastError());
            TlsFree(index);
            return false;
        }
        memcpy(block, (void*)tls->StartAddressOfRawData, templateSize);
        const uint8_t* tb = (const uint8_t*)block;
        Log("loader",
            "TLS template %zu bytes: %02x %02x %02x %02x %02x %02x %02x "
            "%02x",
            templateSize, tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6],
            tb[7]);
    }
    if (!TlsSetValue(index, block)) {
        Log("loader", "WARNING: TlsSetValue failed (%lu)", GetLastError());
        if (block) HeapFree(GetProcessHeap(), 0, block);
        TlsFree(index);
        return false;
    }
    // Write the block DIRECTLY into the TEB TLS array as well: the
    // compiler's __declspec(thread) access reads TEB->TLS[index] via
    // gs:[58h], and in this process TlsSetValue's slot and the TEB array
    // slot disagree (TlsGetValue returns the block, the direct TEB read
    // returns a stale pointer), so the direct write is what the guest's
    // TLS access actually sees. TlsSetValue is kept for TlsGetValue.
    {
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        tlsArr[index] = block;
    }
    // Install this thread's /GA view (view[0] = view[index] = block)
    // so the guest's patched `mov r64, gs:[58h]` loads the block. The
    // real array[0] is never touched. Must run before the TLS
    // callbacks below (guest code).
    out->gaView = InstallGaView(block, index);
    // Confirm the slots actually hold the block (diagnostic: TlsGetValue
    // and the direct TEB read have disagreed in testing).
    {
        void* check = TlsGetValue(index);
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        Log("loader",
            "TLS slot check: TlsGetValue=%p TEB->TLS[%lu]=%p "
            "GA view=%p view[0]=%p view[%lu]=%p",
            check, index, tlsArr[index], out->gaView,
            out->gaView ? ((void**)out->gaView)[0] : nullptr, index,
            out->gaView ? ((void**)out->gaView)[index] : nullptr);
    }
    out->tlsBlock = block;
    out->tlsIndex = index;
    out->tlsIndexValid = true;
    // Remember the template for worker threads (phase 3): the template
    // and callback pointers live in the mapped image, which stays
    // mapped for the guest's lifetime.
    g_tlsInfo.templateData = (const void*)tls->StartAddressOfRawData;
    g_tlsInfo.templateSize = templateSize;
    g_tlsInfo.index = index;
    g_tlsInfo.callbacks = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
    g_tlsInfo.valid = true;
    if (tls->AddressOfCallBacks) {
        auto* callbacks = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
        for (size_t i = 0; callbacks[i]; ++i) {
            __try {
                callbacks[i](base, DLL_PROCESS_ATTACH, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("loader", "WARNING: TLS callback %zu faulted (0x%08x)",
                    i, GetExceptionCode());
                continue;
            }
            Log("loader", "TLS callback %zu ran (PROCESS_ATTACH)", i);
        }
    }
    // Hardening (phase 5): the guest's .CRT$XLB callback runs above and
    // may replace or free the block it was given — two full-build runs
    // showed a guest reading garbage for a __declspec(thread) initial
    // value (callback=1, set works, init garbage) while isolated
    // re-runs of the same binary pass, i.e. the block is clobbered
    // after SetupTls, not mis-copied. Re-verify the block still holds
    // the template; if the callback clobbered it, restore the template
    // before the guest reads it. No-op in the common case. If the
    // callback replaced the TEB slot pointer instead, log it (the
    // foreign block is not ours to rewrite).
    if (block && templateSize > 0) {
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        if (tlsArr[index] != block) {
            Log("loader",
                "WARNING: TLS slot %lu changed by callback (%p -> %p)",
                index, block, tlsArr[index]);
        } else if (memcmp(block, (void*)tls->StartAddressOfRawData,
                          templateSize) != 0) {
            Log("loader",
                "WARNING: TLS block clobbered by callback, restoring "
                "template");
            memcpy(block, (void*)tls->StartAddressOfRawData, templateSize);
        }
    }
    Log("loader",
        "TLS: guest index %lu, layer index %lu, template %zu bytes, "
        "block %p, GA view %p (gaSlot %lu)",
        index, index, templateSize, block, out->gaView, g_gaSlot);
    return true;
}

}  // namespace

bool LoadXboxPe(const wchar_t* path, LoadedPe* out, char* err, size_t errSize) {
    memset(out, 0, sizeof(*out));
    // The guest directory becomes the XPackage mount path.
    {
        std::wstring dir = path;
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) {
            dir = dir.substr(0, p);
            SetGuestDir(dir.c_str());
            wcsncpy_s(out->guestDir, dir.c_str(), _TRUNCATE);
            // The guest-drive path rewrite (real-exe boot #26) needs the
            // guest dir at runtime, after LoadXboxPe returns.
            wcsncpy_s(g_scarlettGuestDir, dir.c_str(), _TRUNCATE);
        }
    }
    auto fail = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(err, errSize, fmt, ap);
        va_end(ap);
        Log("loader", "FAIL %s", err);
        return false;
    };

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail("cannot open %ls (err %lu)", path, GetLastError());
    DWORD fsize = GetFileSize(h, nullptr);
    std::vector<uint8_t> file(fsize);
    DWORD read = 0;
    if (!ReadFile(h, file.data(), fsize, &read, nullptr) || read != fsize) {
        CloseHandle(h);
        return fail("cannot read %ls", path);
    }
    CloseHandle(h);
    const uint8_t* buf = file.data();

    if (fsize < sizeof(IMAGE_DOS_HEADER) ||
        *(const uint16_t*)buf != IMAGE_DOS_SIGNATURE)
        return fail("not a PE (bad MZ)");
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)buf;
    if (dos->e_lfanew < 0 ||
        (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fsize)
        return fail("bad e_lfanew");
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return fail("bad PE signature");
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return fail("not x86-64 (machine 0x%04x)",
                    nt->FileHeader.Machine);
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("not PE32+ (magic 0x%04x)", nt->OptionalHeader.Magic);
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
        return fail("not marked executable");

    out->preferredBase = nt->OptionalHeader.ImageBase;
    Log("loader", "PE ok: machine x64, imagebase 0x%llx, entry 0x%x, "
                  "image size 0x%x, sections %u",
        (unsigned long long)out->preferredBase,
        nt->OptionalHeader.AddressOfEntryPoint,
        nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    // .xbld validation (Xbox build-info section).
    const IMAGE_SECTION_HEADER* xbld = FindSection(nt, ".xbld");
    if (xbld) {
        const uint8_t* data = buf + xbld->PointerToRawData;
        ParseXbld(data, xbld->SizeOfRawData, out);
        Log("loader", ".xbld present: edition='%s' build='%s'",
            out->xbldEdition, out->xbldBuild);
    } else {
        Log("loader", "WARNING: no .xbld section (not an Xbox-built PE?)");
    }
    // The Xbox COM wrapper only applies to real Xbox titles. Synthetic
    // guests are ALSO .xbld-built (GXDK/ERA toolchain) and have a real
    // .xbld section, so hasXbld alone cannot distinguish them — but
    // they are small test programs that use the DESKTOP D3D12 vtable
    // layout and must get the raw device (build 44 proved wrapping
    // them breaks every gfx verify run with E_NOTIMPL). The real game
    // is a 120 MB image (SizeOfImage ~176 MB); every synthetic guest
    // is a few MB at most. Gate the wrapper on a large image as well
    // as .xbld. This is a heuristic, documented as such: if a future
    // real title is ever under 32 MB, or a synthetic guest over it,
    // this gate needs revisiting. NOTE: this must run AFTER
    // out->imageSize is assigned below (it is 0 until then).
    // (Set after the mapping, below.)

    // Map the image.
    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    void* base = VirtualAlloc((void*)out->preferredBase, imageSize,
                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    out->relocated = false;
    if (!base) {
        base = VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT,
                            PAGE_EXECUTE_READWRITE);
        out->relocated = true;
        Log("loader", "preferred base busy, mapped at %p (relocating)", base);
    }
    if (!base) return fail("VirtualAlloc failed (err %lu)", GetLastError());
    out->base = base;
    out->imageSize = imageSize;
    // Now that out->imageSize is assigned, set the COM-wrapper gate.
    g_scGuestIsRealXboxTitle =
        out->hasXbld && out->imageSize >= (32ULL << 20);

    // Copy headers + sections.
    memcpy(base, buf, nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData == 0) continue;
        memcpy((uint8_t*)base + sec[i].VirtualAddress,
               buf + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    }

    // Base relocations (only needed when we could not use the preferred base).
    if (out->relocated) {
        uint64_t delta = (uint64_t)base - out->preferredBase;
        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (dir.VirtualAddress && dir.Size) {
            uint8_t* reloc = (uint8_t*)base + dir.VirtualAddress;
            uint8_t* end = reloc + dir.Size;
            while (reloc < end) {
                auto* block = (IMAGE_BASE_RELOCATION*)reloc;
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                uint32_t count =
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                const uint16_t* entries =
                    (const uint16_t*)(reloc + sizeof(IMAGE_BASE_RELOCATION));
                for (uint32_t j = 0; j < count; ++j) {
                    uint16_t type = entries[j] >> 12;
                    uint16_t off = entries[j] & 0x0FFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        uint64_t* patch =
                            (uint64_t*)((uint8_t*)base + block->VirtualAddress + off);
                        *patch += delta;
                    }
                }
                reloc += block->SizeOfBlock;
            }
            Log("loader", "relocations applied (delta 0x%llx)",
                (unsigned long long)delta);
        }
    }

    // TLS directory: noted here; set up after protections (see below).
    const IMAGE_DATA_DIRECTORY& tlsDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    out->hasTls = tlsDir.VirtualAddress != 0;
    if (out->hasTls)
        Log("loader", "TLS directory present");

    // Import resolution.
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size) {
        auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
            buf, nt, impDir.VirtualAddress, nullptr);
        for (; desc && desc->Name; ++desc) {
            const char* modName =
                (const char*)RvaToFile(buf, nt, desc->Name, nullptr);
            if (!modName) continue;
            out->importModules++;
            bool shim = IsShimModule(modName);
            // ERA detection (phase 5): any ERA module import marks the
            // title as ERA; its ERA imports route through the translation.
            bool eraMod = EraIsEraModule(modName);
            if (eraMod && !out->eraDetected) {
                out->eraDetected = true;
                Log("loader", "ERA title detected (module %s)", modName);
            }
            HMODULE realMod = nullptr;
            if (!shim) {
                realMod = LoadLibraryA(modName);
                if (!realMod)
                    Log("loader", "LoadLibrary failed for %s (err %lu)",
                        modName, GetLastError());
            }
            uint32_t oftRva = desc->OriginalFirstThunk
                                  ? desc->OriginalFirstThunk
                                  : desc->FirstThunk;
            auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(buf, nt, oftRva, nullptr);
            auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
            if (!oft || !ft) continue;
            for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
                void* target = nullptr;
                // Per-import failure logging (WinRT phase): a title
                // boot can leave a failure unidentified when only the
                // count is logged. The name/ordinal is captured here
                // so the failure branch below can name the exact
                // import.
                const char* curFnName = nullptr;
                bool curIsOrdinal = false;
                uint16_t curOrd = 0;
                if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                    // Ordinal import (e.g. d3d12.dll!D3D12CreateDevice is
                    // ordinal 101 in the SDK import lib). Real modules
                    // resolve via GetProcAddress(MAKEINTRESOURCE); shim
                    // modules resolve through the ordinal table
                    // (kOrdinalExports — synthetic ordinals in .def order
                    // for GDK modules; Xbox One shim-DLL ordinals for
                    // ERA modules).
                    uint16_t ord =
                        (uint16_t)(oft[k].u1.Ordinal & 0xFFFF);
                    curIsOrdinal = true;
                    curOrd = ord;
                    if (shim) {
                        const OrdinalExport* oe =
                            FindOrdinalExport(modName, ord);
                        if (oe && oe->fn) {
                            target = oe->fn;
                            Log("loader", "ordinal %s:#%u -> %s", modName,
                                ord, oe->name);
                        }
                    } else if (realMod) {
                        target = (void*)GetProcAddress(
                            realMod, MAKEINTRESOURCEA(ord));
                        if (target)
                            Log("loader", "ordinal %s:#%u resolved", modName,
                                ord);
                    }
                    if (!target)
                        Log("loader",
                            "ordinal import %s:#%u unresolved",
                            modName, ord);
                } else {
                    auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                        buf, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
                    if (ibn) {
                        const char* fnName = (const char*)ibn->Name;
                        curFnName = fnName;
                        if (shim) {
                            const Export* ex = FindExport(modName, fnName);
                            if (ex && ex->fn) {
                                target = ex->fn;
                                if (eraMod) {
                                    Log("loader", "era   %s!%s -> %s",
                                        modName, fnName,
                                        ex->status == Status::kImplemented
                                            ? "implemented"
                                            : "stub");
                                } else {
                                    Log("loader", "shim  %s!%s -> %s",
                                        modName, fnName,
                                        ex->status == Status::kImplemented
                                            ? "implemented"
                                            : "stub");
                                }
                            } else if (eraMod &&
                                       CiEq(modName, "kernelx.dll")) {
                                // kernelx is the ERA kernel shim: its
                                // non-Xbox imports are Win32 API names
                                // (CreateFileW, HeapAlloc, VirtualAlloc,
                                // ...) that the real kernelx.dll
                                // forwards to kernel32.dll on console.
                                // Forward them to the real kernel32.dll
                                // here instead of stubbing them.
                                static HMODULE hKernel32 = nullptr;
                                if (!hKernel32)
                                    hKernel32 =
                                        GetModuleHandleA("kernel32.dll");
                                if (hKernel32)
                                    target = (void*)GetProcAddress(
                                        hKernel32, fnName);
                                if (target) {
                                    Log("loader",
                                        "era   kernelx!%s -> kernel32 "
                                        "forward",
                                        fnName);
                                } else {
                                    Log("loader",
                                        "era   kernelx!%s -> no GDK "
                                        "equivalent (E_NOTIMPL stub)",
                                        fnName);
                                }
                            } else if (eraMod) {
                                Log("loader",
                                    "era   %s!%s -> no GDK equivalent "
                                    "(E_NOTIMPL stub)",
                                    modName, fnName);
                            } else {
                                Log("loader", "shim MISSING %s!%s "
                                              "(no handler, E_NOTIMPL stub)",
                                    modName, fnName);
                            }
                        } else if (realMod) {
                            target = (void*)GetProcAddress(realMod, fnName);
                        }
                    }
                }
                if (target) {
                    ft[k].u1.Function = (uint64_t)target;
                    out->importsResolved++;
                } else {
                    // Name the exact failed import (see the capture
                    // above): 647/648 with an unidentified failure is
                    // not debuggable.
                    if (curIsOrdinal)
                        Log("loader", "IMPORT FAILED: %s:#%u (no handler)",
                            modName, curOrd);
                    else if (curFnName)
                        Log("loader", "IMPORT FAILED: %s!%s (no handler)",
                            modName, curFnName);
                    else
                        Log("loader",
                            "IMPORT FAILED: %s (unreadable import name)",
                            modName);
                    ft[k].u1.Function = (uint64_t)(void*)ScNotImplementedStub;
                    out->importsFailed++;
                }
            }
        }
    }
    Log("loader", "imports: %u modules, %u resolved, %u failed (E_NOTIMPL stub)",
        out->importModules, out->importsResolved, out->importsFailed);
    if (out->eraDetected)
        Log("loader",
            "ERA title: ERA imports routed through the ERA -> GDK "
            "translation");

    // Thread-creation hooks (phase 3): guest CreateThread /
    // CreateRemoteThread imports point at the layer wrappers so worker
    // threads get TLS. Must run after import resolution (FT holds the
    // real addresses) and before section protections (.rdata -> R).
    HookThreadCreation(buf, nt, base);

    // Crash-filter protection (real-exe boot #13): the guest replaces the
    // layer's UnhandledFilter during static init, so int3 asserts never
    // dump. Same window as the thread hooks (after imports, before
    // protections).
    HookCrashFilterProtection(buf, nt, base);

    // Guest-drive path rewrite (real-exe boot #26): the game opens pack
    // files as "G:/<name>" and G: is a real USB drive on this PC, so
    // the opens fail. Rewrite "G:/..." to the guest dir in the game's
    // file-API imports. Same window as the hooks above.
    HookGuestDrivePaths(buf, nt, base);

    // WinRT activation hooks (WinRT phase): only for ERA titles —
    // routes CoreApplication activation through the layer's shim (both
    // the direct RoGet path and the vccorlib110 path an Xbox One title
    // actually uses). Same window as the thread hooks (after imports,
    // before protections).
    if (out->eraDetected) HookWinRtActivation(buf, nt, base);

    // real-exe boot #3/#5 fix: patch the derived vtable's slot 6 and NOP
    // F's int3 (see ApplyScarlettVtablePatch). Must run while the image
    // is still RWX, i.e. before section protections make .rdata
    // read-only.
    ApplyScarlettVtablePatch(base, nt);

    // TLS /GA patch (approach (B), Seth's decision): rewrite every
    // `mov r64, gs:[58h]` to load the /GA view from TlsSlots[gaSlot].
    // Same window as above — while the image is RWX, before section
    // protections and before any image code (TLS callbacks) runs.
    PatchGaTlsReads(base, nt);

    // Final page protections: per-section (text XR, rodata R, data RW).
    // Applied AFTER relocations + imports, which write into the image.
    ApplySectionProtections(base, nt);
    // TLS setup for this thread (slot + block + callbacks), after
    // protections so the index slot (.data, RW) and callbacks (.text, X)
    // are in their final state.
    if (out->hasTls) SetupTls(base, nt, out);
    out->entry = (uint8_t*)base + nt->OptionalHeader.AddressOfEntryPoint;
    Log("loader", "mapped at %p, entry %p", base, out->entry);
    return true;
}

// Shared crash-dump writer: logs the exception record and the faulting
// thread's TLS array state, then writes a full-memory minidump next to
// the harness exe. Used by CrashFilter (entry thread, via __except)
// and by UnhandledFilter (any thread, via SetUnhandledExceptionFilter).
// Boot #7 crashed on a worker thread (0x140104075, rcx=0x7fffffff) and
// the __except never fired, so no dump was written — the worker-thread
// path needs this filter. Runs on the faulting thread, so the TEB read
// below is that thread's TLS array.
static void WriteCrashDump(EXCEPTION_POINTERS* ep, const char* where) {
    if (!ep || !ep->ExceptionRecord) {
        Log("loader", "guest crashed (%s): no exception record", where);
        ScLogFlush();  // buffered logger (build 88): never lose the fault
        return;
    }
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->NumberParameters >= 2) {
        Log("loader",
            "guest crashed (%s): exception 0x%08x at %p "
            "(record %p, params[0]=0x%llx params[1]=0x%llx) thread %lu",
            where, r->ExceptionCode, r->ExceptionAddress, r,
            (unsigned long long)r->ExceptionInformation[0],
            (unsigned long long)r->ExceptionInformation[1],
            (unsigned long)GetCurrentThreadId());
    } else {
        Log("loader",
            "guest crashed (%s): exception 0x%08x at %p (record %p) "
            "thread %lu",
            where, r->ExceptionCode, r->ExceptionAddress, r,
            (unsigned long)GetCurrentThreadId());
    }
    // Faulting thread's TLS state: the /GA view (TlsSlots[gaSlot])
    // and the allocator's pool slot [view[0]+0x13a8]. Guarded — the
    // view pointer may itself be garbage.
    __try {
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        void* view = (g_gaSlot != TLS_OUT_OF_INDEXES)
                         ? TlsSlots()[g_gaSlot]
                         : nullptr;
        void* poolSlot = nullptr;
        if (view && ((void**)view)[0]) {
            poolSlot = *(void**)((uint8_t*)((void**)view)[0] + 0x13a8);
        }
        Log("loader",
            "crash TLS: thread %lu tlsArr[0]=%p tlsArr[%lu]=%p "
            "GA view=%p view[0]=%p [view[0]+0x13a8]=%p",
            (unsigned long)GetCurrentThreadId(), tlsArr ? tlsArr[0] : nullptr,
            g_tlsInfo.valid ? g_tlsInfo.index : 0,
            (tlsArr && g_tlsInfo.valid) ? tlsArr[g_tlsInfo.index] : nullptr,
            view, view ? ((void**)view)[0] : nullptr, poolSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "crash TLS: faulting thread TLS unreadable");
    }
    // Dump path: directory of this exe (verify.exe / ScarlettCompat.dll
    // dir), fixed name so each crash overwrites the last.
    wchar_t exePath[MAX_PATH] = {};
    wchar_t dumpPath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *slash = L'\0';
        _snwprintf_s(dumpPath, MAX_PATH, _TRUNCATE, L"%s\\scarlett_crash.dmp",
                     exePath);
    } else {
        _snwprintf_s(dumpPath, MAX_PATH, _TRUNCATE,
                     L"scarlett_crash.dmp");
    }
    HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("loader", "minidump: cannot create %ls (err %lu)", dumpPath,
            GetLastError());
        return;
    }
    // Flush the buffered logger before the dump is written: the crash
    // lines above must survive even if the dump or the process dies.
    ScLogFlush();
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, MiniDumpWithFullMemory, &mei,
                                nullptr, nullptr);
    if (ok) {
        LARGE_INTEGER size = {};
        GetFileSizeEx(hFile, &size);
        Log("loader", "minidump: wrote %ls (%lld bytes, full memory)",
            dumpPath, size.QuadPart);
    } else {
        Log("loader", "minidump: MiniDumpWriteDump failed (err %lu)",
            GetLastError());
    }
    CloseHandle(hFile);
}

// Unhandled-exception filter: catches a fatal crash on ANY thread
// (the __except in RunXboxPe only covers the entry thread). Writes the
// full-memory dump via WriteCrashDump and lets the process terminate.
static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashDump(ep, "unhandled");
    return EXCEPTION_EXECUTE_HANDLER;
}

// Vectored-handler backstop (real-exe boot #13). The int3 assert that
// killed boots #12/#13 (0x80000003) never reached UnhandledFilter and
// never dumped — the game's CRT replaces the unhandled filter during
// static init (see HookCrashFilterProtection). A VEH fires first-chance
// on ANY thread and cannot be displaced by SetUnhandledExceptionFilter,
// so it guarantees a dump + a mount-registry probe for the breakpoint
// case. It handles ONLY 0x80000003, once per run, and returns
// CONTINUE_SEARCH so normal handling (and the exit code) is unchanged.
// AVs are deliberately left alone: the guest may catch them itself, and
// a first-chance dump there would be noise.
static void* g_guestBaseForVeh = nullptr;
static LONG g_vehDumped = 0;

// Build 84: hardware write watchpoint on the streaming record's backend
// field (the null sub-object of fault 0x1428C9A08). Two stages, both
// observation-only -- no guest byte is patched:
//   stage 1  DR0 = image global 0x79F6D70, catches the publish of a
//            record whose +0x00 is the "sdms" magic
//   stage 2  DR0 = record+0x18, catches every write to the backend
// Every trapping thread logs its RIP (the instruction after the writer),
// the value the field now holds, and the guest return addresses on its
// stack; execution resumes with the watchpoint still armed so later
// writers are named too. DR7 = local enable 0, RW0 = 01 (write),
// LEN0 = 11 (8 bytes) => 0x000D0001.
static volatile LONG g_wpStage = 0;  // 0 idle, 1 global, 2 field, 3 off
static volatile LONG64 g_wpRecord = 0;
static volatile LONG g_wpTrapCount = 0;
static volatile LONG g_wpArmed = 0;
static const uint64_t kWpDr7Write8 = 0x000D0001ull;
// LEN0 = 00, RW0 = 00 (execute), L0 = 1.
static const uint64_t kWpDr7Exec = 0x00000001ull;
// True on the watchdog thread only: its SEH-guarded reads must never be
// mistaken for a guest crash by the VEH (boot #67's verify children died
// that way when the watchdog probed a global the small test images do
// not map).
static thread_local bool t_wpThread = false;

// Arm (or clear, when addr is 0) DR0 on every thread of this process.
static void WpSetAllThreads(uint64_t addr, uint64_t dr7 = kWpDr7Write8) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    const DWORD me = GetCurrentProcessId();
    LONG armed = 0;
    const DWORD self = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != me)
                continue;
            // Never suspend this thread: GetThreadContext on a thread
            // that suspended itself hangs (boot #65 lost the watchdog
            // that way before it logged anything).
            if (te.th32ThreadID == self)
                continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_SET_CONTEXT,
                                   FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &c)) {
                    c.Dr0 = addr;
                    c.Dr1 = 0;
                    c.Dr2 = 0;
                    c.Dr3 = 0;
                    c.Dr6 = 0;
                    c.Dr7 = addr ? dr7 : 0;
                    if (SetThreadContext(th, &c))
                        ++armed;
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_wpArmed = armed;
}

// Guest return addresses sitting on the trapping thread's stack.
static void WpLogStack(const CONTEXT* c) {
    __try {
        const uint64_t* sp = (const uint64_t*)(uintptr_t)c->Rsp;
        char buf[320] = {};
        size_t o = 0;
        int found = 0;
        for (int i = 0; i < 40; ++i) {
            const uint64_t v = sp[i];
            if (v < 0x140000000ull || v >= 0x147400000ull)
                continue;
            const int n = _snprintf_s(buf + o, sizeof(buf) - o, _TRUNCATE,
                                      "%s0x%llx", found ? "," : "",
                                      (unsigned long long)v);
            if (n <= 0)
                break;
            o += (size_t)n;
            ++found;
            if (found >= 10)
                break;
        }
        Log("loader", "[watch] stack: %s", found ? buf : "(no guest frames)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "[watch] stack=<unreadable>");
    }
}

// The watchpoint only makes sense for an image that actually maps the
// record global. ERA/guest verify children are tiny (SizeOfImage 0xb000),
// so arming there would only probe unmapped memory.
static bool WpImageCoversGlobal(uint8_t* base) {
    __try {
        const uint32_t lfanew = *(uint32_t*)(base + 0x3C);
        const uint32_t imageSize = *(uint32_t*)(base + lfanew + 0x50);
        return imageSize > 0x79F6D78u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static DWORD WINAPI WpThreadProc(void*) {
    t_wpThread = true;
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base) {
        Log("loader", "[watch] no guest base; watchpoint not armed");
        return 0;
    }
    if (!WpImageCoversGlobal(base)) {
        Log("loader",
            "[watch] image does not map 0x79F6D70; watchpoint skipped");
        g_wpStage = 3;
        return 0;
    }
    const uint64_t gAddr = (uint64_t)(base + 0x79F6D70);
    Log("loader",
        "[watch] watchdog start base=0x%llx global=0x%llx",
        (unsigned long long)(uintptr_t)base, (unsigned long long)gAddr);
    g_wpStage = 1;
    WpSetAllThreads(gAddr);
    Log("loader",
        "[watch] stage1: DR0=0x%llx (record global) write/8 on %ld threads",
        (unsigned long long)gAddr, (long)g_wpArmed);
    // Boot #66 armed stage 1 on the four threads that existed at entry;
    // the publish ran on a thread the game created later, so no trap ever
    // fired. Re-scan continuously: every new thread matters in both
    // stages, and the publish-to-crash window is only milliseconds.
    for (int i = 0; i < 60000; ++i) {  // ~120 s, one tick = 2 ms
        Sleep(2);
        const LONG st = g_wpStage;
        if (st == 3)
            return 0;
        if (st == 2) {
            if ((i % 25) == 0)
                WpSetAllThreads((uint64_t)g_wpRecord + 0x18);
            continue;
        }
        if ((i % 50) == 0)
            WpSetAllThreads(gAddr);
        uint64_t rec = 0, magic = 0;
        __try {
            rec = *(uint64_t*)gAddr;
            magic = rec ? *(uint64_t*)rec : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!rec || magic != 0x6D736473ull)
            continue;
        uint64_t cur = 0, owner = 0;
        __try {
            owner = *(uint64_t*)(rec + 0x10);
            cur = *(uint64_t*)(rec + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_wpRecord = (LONG64)rec;
        g_wpStage = 2;
        WpSetAllThreads(rec + 0x18);
        Log("loader",
            "[watch] stage2: record 0x%llx magic=sdms owner=0x%llx -> "
            "DR0=0x%llx, [+0x18] at arm time 0x%llx, %ld threads",
            (unsigned long long)rec, (unsigned long long)owner,
            (unsigned long long)(rec + 0x18), (unsigned long long)cur,
            (long)g_wpArmed);
    }
    return 0;
}

// Boot #101 instrumentation (env SCARLETT_ARRWP=1): capture the slot bitmap
// at the exact store that fills the empty container array (guest +0xA943A38,
// written by 0x14247B0ED). A DR0 write watchpoint is armed on every thread
// and the bitmap word (guest +0x6891A08, read by the slot allocator at
// 0x145CABD9C) is logged right after the store. Read-only observation: no
// guest code or data is modified, and without the env var nothing runs.
static DWORD WINAPI ArrWpThreadProc(void*) {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base) {
        Log("loader", "[arrwp] no guest base; not armed");
        return 0;
    }
    const uint64_t slot = (uint64_t)(uintptr_t)(base + 0xA943A38);
    g_wpStage = 9;
    Log("loader", "[arrwp] arming DR0 write/8 on 0x%llx",
        (unsigned long long)slot);
    for (int i = 0; i < 20000; ++i) {
        if (g_wpStage != 9)
            return 0;
        WpSetAllThreads(slot);
        Sleep(2);
    }
    Log("loader", "[arrwp] no trap; disarm");
    g_wpStage = 3;
    return 0;
}

static bool ArrWpRequested() {
    char v[8] = {};
    return GetEnvironmentVariableA("SCARLETT_ARRWP", v, sizeof(v)) > 0;
}

// Boot #102 instrumentation (env SCARLETT_SVCPROBE=1): the mode-1 creator
// branches on the status of a virtual call at 0x142481B2B ([vtable+0x138]
// on the service object in global 0x148760D00). Trap on the instruction
// after the call (0x142481B35) and log EAX plus the callee identity. The
// probe is observation only and arms nothing unless the env var is set.
static bool SvcWpRequested() {
    char v[8] = {};
    return GetEnvironmentVariableA("SCARLETT_SVCPROBE", v, sizeof(v)) > 0;
}

static DWORD WINAPI SvcWpThreadProc(void*) {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base) {
        Log("loader", "[svcwp] no guest base; not armed");
        return 0;
    }
    const uint64_t site = (uint64_t)(uintptr_t)(base + 0x2481B2B);
    g_wpStage = 13;
    Log("loader", "[svcwp] arming DR0 exec on 0x%llx",
        (unsigned long long)site);
    for (int i = 0; i < 20000; ++i) {
        if (g_wpStage != 13)
            return 0;
        WpSetAllThreads(site, kWpDr7Exec);
        Sleep(2);
    }
    Log("loader", "[svcwp] no trap; disarm");
    g_wpStage = 3;
    return 0;
}

void StartStreamWatchpoint() {
    if (InterlockedCompareExchange(&g_wpStage, 0, 0) != 0)
        return;
    // The watchdog is diagnostic instrumentation (hardware write watchpoints
    // re-armed on every thread). Opt out with SCARLETT_NOWATCH=1 so a boot
    // can be judged without DR0/DR7 context surgery on the guest threads.
    if (getenv("SCARLETT_NOWATCH")) {
        Log("loader", "[watch] disabled by SCARLETT_NOWATCH");
        return;
    }
    if (SvcWpRequested()) {
        HANDLE sh = CreateThread(nullptr, 0, SvcWpThreadProc, nullptr, 0,
                                 nullptr);
        if (sh)
            CloseHandle(sh);
        else
            Log("loader", "[svcwp] CreateThread failed (%lu)",
                (unsigned long)GetLastError());
        return;
    }
    if (ArrWpRequested()) {
        HANDLE ah = CreateThread(nullptr, 0, ArrWpThreadProc, nullptr, 0,
                                 nullptr);
        if (ah)
            CloseHandle(ah);
        else
            Log("loader", "[arrwp] CreateThread failed (%lu)",
                (unsigned long)GetLastError());
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, WpThreadProc, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
    else
        Log("loader", "[watch] CreateThread failed (%lu)",
            (unsigned long)GetLastError());
}

// Current backend field of the watched record (0 when unknown). Logged
// next to every device-slot stub so the value on both sides of any
// diverted guest branch is recorded.
uint64_t ScWatchRecordBackend() {
    const LONG64 rec = g_wpRecord;
    if (!rec)
        return 0;
    __try {
        return *(uint64_t*)((uint64_t)rec + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Boot #58: the HLE layer needs the guest image base to load the
// Xbox pool-dispatcher pointer from the .data dispatch slot and to
// read the arena globals (G/Gsize/cursor). Set by RunXboxPe before
// the entry runs.
void* ScGuestBase() { return g_guestBaseForVeh; }

// Calling thread's game TLS block via the /GA view (view[0]).
// nullptr when no view is installed. The view pointer itself comes
// from TlsGetValue (safe); the caller SEH-guards the [0] read.
void* ScGuestGaViewBlock() {
    if (g_gaSlot == TLS_OUT_OF_INDEXES) return nullptr;
    void* view = TlsGetValue(g_gaSlot);
    if (!view) return nullptr;
    return ((void**)view)[0];
}

// Boot #13 diagnostic probe: reads the the title VFS mount-registry state
// from the guest image at fixed RVAs (same convention as the existing
// title-specific patches: pool vtable patch, F int3 NOP, Option A).
//   mount count 0x78f5ec4, array 0x78f5ec8, init flag 0x78f5ec0,
//   lookup name pointer 0x74539f6,
//   scheme globals 0x78f6250 / 0x78f6888 / 0x78f6880.
// All reads guarded — the registry may be uninitialized and the name
// pointer may be null. The answers decide the boot #13 fix: a count of
// 0 means no mount was ever registered before pack-init; a non-empty
// registry with no "packroot" entry means the wrong mounts registered.
static void LogMountProbe() {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base) {
        Log("loader", "mount-probe: no guest base");
        return;
    }
    __try {
        uint32_t count = *(uint32_t*)(base + 0x78f5ec4);
        void* array = *(void**)(base + 0x78f5ec8);
        uint32_t initFlag = *(uint32_t*)(base + 0x78f5ec0);
        // The lookup name global at RVA 0x74539f6 is NOT a pointer: the
        // string itself is stored INLINE in a .data char buffer (boot #14
        // showed its first qword = the ASCII bytes "appdata0"). Read the
        // buffer directly instead of dereferencing the qword as a pointer.
        const char* nameStr = (const char*)(base + 0x74539f6);
        void* h1 = *(void**)(base + 0x78f6250);
        void* h2 = *(void**)(base + 0x78f6888);
        void* h3 = *(void**)(base + 0x78f6880);
        static char nameBuf[64];
        const char* name = "<null>";
        __try {
            size_t i = 0;
            for (; i < sizeof(nameBuf) - 1 && nameStr[i]; ++i)
                nameBuf[i] = nameStr[i];
            nameBuf[i] = '\0';
            name = nameBuf;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            name = "<unreadable>";
        }
        Log("loader",
            "mount-probe: count=%lu array=%p initFlag=%lu "
            "nameBuf=\"%s\" schemeH1=%p schemeH2=%p schemeH3=%p",
            (unsigned long)count, array, (unsigned long)initFlag,
            name, h1, h2, h3);
        // The fault fn's 3rd lookup arg (r9) is a runtime-filled
        // buffer at RVA 0x678d1bb; it drives the transform's join
        // logic. Log it, plus the pack-mgr vector count [obj+0xc]
        // (manager object global at RVA 0x7f00510) — the assert is
        // gated on that count, not directly on the lookup result.
        __try {
            static char r9Buf[64];
            const char* r9s = (const char*)(base + 0x678d1bb);
            size_t i = 0;
            for (; i < sizeof(r9Buf) - 1 && r9s[i]; ++i)
                r9Buf[i] = r9s[i];
            r9Buf[i] = '\0';
            Log("loader", "mount-probe: r9buf=\"%s\"", r9Buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: r9buf=<unreadable>");
        }
        __try {
            void* mgr = *(void**)(base + 0x7f00510);
            uint32_t mgrCount =
                mgr ? *(uint32_t*)((uint8_t*)mgr + 0xc) : 0xffffffffu;
            Log("loader", "mount-probe: packMgr=%p count=%lu",
                mgr, (unsigned long)mgrCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: packMgr=<unreadable>");
        }
        // The pack-mgr lookup's table is a TLS singleton: flag at
        // TLS[0]+0x7f8, table at TLS[0]+0x7f0, count at
        // [table+0xc04]. NOTE: the game's patched code reads the
        // /GA view (TlsSlots[gaSlot]), whose [0] is the game block —
        // but the real TLS array's slot 10 IS the game block too
        // (TlsAlloc returned 10, and the loader writes the block
        // there as well). Read the table from slot10, the real game
        // block; slot0 is another module's block and reads 0/NULL.
        __try {
            void** tlsArray = (void**)__readgsqword(0x58);
            void* slot0 = tlsArray ? tlsArray[0] : nullptr;
            void* slot10 = tlsArray ? tlsArray[10] : nullptr;
            void* view = (void*)TlsGetValue(g_gaSlot);
            void* view0 = view ? ((void**)view)[0] : nullptr;
            void* tbl = nullptr;
            uint32_t tblFlag = 0xffffffffu;
            uint32_t tblCount = 0xffffffffu;
            // The game reads the table through the /GA view
            // (view[0]), NOT through the real TLS array[10] —
            // view0 and slot10 are different blocks, so read the
            // table from view0, the block the game actually uses.
            if (view0) {
                tblFlag = *(uint32_t*)((uint8_t*)view0 + 0x7f8);
                tbl = *(void**)((uint8_t*)view0 + 0x7f0);
                if (tbl)
                    tblCount = *(uint32_t*)((uint8_t*)tbl + 0xc04);
            }
            Log("loader",
                "mount-probe: tlsArray=%p slot0=%p slot10=%p "
                "view=%p view0=%p tblFlag=%lu table=%p tblCount=%lu",
                tlsArray, slot0, slot10, view, view0,
                (unsigned long)tblFlag, tbl,
                (unsigned long)tblCount);
            // Boot #25: the table header is valid (tblCount=1) but the
            // lookup still returns 0, so dump the table ENTRY contents
            // the lookup iterates — if the entry region is all zeros,
            // the ctor only wrote the header and never populated an
            // entry. Also confirm the default object's runtime vtable
            // and slot 0x28 (the ctor's open/get-info call, which
            // returned -1 in boot #25).
            if (tbl) {
                uint64_t t0 = *(uint64_t*)((uint8_t*)tbl + 0x0);
                uint64_t t8 = *(uint64_t*)((uint8_t*)tbl + 0x8);
                uint64_t tc00 = *(uint64_t*)((uint8_t*)tbl + 0xc00);
                uint64_t tc08 = *(uint64_t*)((uint8_t*)tbl + 0xc08);
                uint64_t tc10 = *(uint64_t*)((uint8_t*)tbl + 0xc10);
                uint64_t tc18 = *(uint64_t*)((uint8_t*)tbl + 0xc18);
                Log("loader",
                    "mount-probe: table[0]=%llx table[8]=%llx "
                    "table[c00]=%llx table[c08]=%llx "
                    "table[c10]=%llx table[c18]=%llx",
                    (unsigned long long)t0, (unsigned long long)t8,
                    (unsigned long long)tc00,
                    (unsigned long long)tc08,
                    (unsigned long long)tc10,
                    (unsigned long long)tc18);
            }
            {
                void* defObj = *(void**)(base + 0x78f5eb0);
                void* defVt = defObj ? *(void**)defObj : nullptr;
                void* slot28 =
                    defVt ? *(void**)((uint8_t*)defVt + 0x28) : nullptr;
                void* slot168 =
                    defVt ? *(void**)((uint8_t*)defVt + 0x168) : nullptr;
                Log("loader",
                    "mount-probe: defObj=%p vtable=%p slot28=%p "
                    "slot168=%p",
                    defObj, defVt, slot28, slot168);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: tls=<unreadable>");
        }
        if (array && count > 0 && count < 10000) {
            // The array holds 0x20-byte entries INLINE (not pointers
            // to entries): entry i is at (uint8_t*)array + i*0x20.
            uint8_t* entries = (uint8_t*)array;
            uint32_t n = count < 4 ? count : 4;
            for (uint32_t i = 0; i < n; ++i) {
                __try {
                    uint8_t* e = entries + i * 0x20;
                    const char* ename = *(const char**)e;
                    uint16_t elen = *(uint16_t*)(e + 0xa);
                    uint16_t ekind = *(uint16_t*)(e + 0x8);
                    uint32_t ecount = *(uint32_t*)(e + 0x14);
                    void* eobj = *(void**)(e + 0x18);
                    char ebuf[64] = {};
                    if (ename) {
                        size_t j = 0;
                        for (; j < sizeof(ebuf) - 1 && ename[j]; ++j)
                            ebuf[j] = ename[j];
                    }
                    Log("loader",
                        "mount-probe: entry %lu name=\"%s\" len=%u "
                        "kind=%u count=%lu objList=%p",
                        (unsigned long)i, ebuf, (unsigned)elen,
                        (unsigned)ekind, (unsigned long)ecount, eobj);
                    // Also verify the mounted object itself: the
                    // lookup calls vtable slot 0x168 on objList[0],
                    // so a NULL/garbage vtable there would make the
                    // lookup fail even with a valid registry.
                    if (eobj && ecount > 0) {
                        __try {
                            void* obj = *(void**)eobj;
                            void* vt = obj ? *(void**)obj : nullptr;
                            void* slot168 =
                                vt ? *(void**)((uint8_t*)vt + 0x168)
                                   : nullptr;
                            Log("loader",
                                "mount-probe: entry %lu obj=%p "
                                "vtable=%p slot168=%p",
                                (unsigned long)i, obj, vt, slot168);
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            Log("loader",
                                "mount-probe: entry %lu obj unreadable",
                                (unsigned long)i);
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("loader", "mount-probe: entry %lu unreadable",
                        (unsigned long)i);
                }
            }
        }
        // Boot #42: 0x142478440 (D3D12 init) returns false and the
        // global at RVA 0x75df128 is NULL at the crash in 0x145eb2480.
        // The setter (0x145bdb830, called from 0x1424792f4) stores the
        // return of 0x145bdb8c0 into 0x75df128 unconditionally, and
        // 0x145bdb8c0 crashes at [NULL+8] if its factory returns NULL
        // — so if the setter ran, the global is non-NULL. The failure
        // is therefore BEFORE the setter or in the two 0x14161cef0
        // calls after it (0x142479345 / 0x142479366, each `je` to a
        // false return). Dump the globals that distinguish the cases:
        //   setter ran?  -> 0x75df130 non-NULL, flags 0x75df0f6 bit 2
        //   reached 0x142479337? -> 0x75e75d0 non-NULL
        //   0x14161cef0 #1 ok? -> 0x75df220 non-NULL (its out ptr);
        //     its count input is the u32 at 0x75df1c8
        //   0x14161cef0 #2 ok? -> 0x75df240 non-NULL
        //   0x1417f6a30 objects (both calls proved ok by the 512+64
        //     command lists in the boot #41 log) -> 0x75e7360/70
        //   slot-41 `this` object -> 0x8759d00 (should be the device
        //     wrapper; if the slot-41 calls ran, the layer would have
        //     logged UNIMPLEMENTED — none appear, so they never ran)
        __try {
            uint64_t g128 = *(uint64_t*)(base + 0x75df128);
            uint64_t g130 = *(uint64_t*)(base + 0x75df130);
            uint16_t flags = *(uint16_t*)(base + 0x75df0f6);
            uint32_t cnt = *(uint32_t*)(base + 0x75df1c8);
            uint64_t r886 = *(uint64_t*)(base + 0x75e75d0);
            uint64_t out1 = *(uint64_t*)(base + 0x75df220);
            uint64_t out2 = *(uint64_t*)(base + 0x75df240);
            uint64_t objA = *(uint64_t*)(base + 0x75e7360);
            uint64_t objB = *(uint64_t*)(base + 0x75e7370);
            uint64_t ba0 = *(uint64_t*)(base + 0x75df010);
            uint64_t dev = *(uint64_t*)(base + 0x8759d00);
            uint64_t a18 = *(uint64_t*)(base + 0x75df0a8);
            uint32_t tlsIdx = *(uint32_t*)(base + 0x6e08b2c);
            Log("loader",
                "mount-probe: init42 g128=%llx g130=%llx flags=%x "
                "count=%lu r886=%llx out1=%llx out2=%llx",
                (unsigned long long)g128, (unsigned long long)g130,
                (unsigned)flags, (unsigned long)cnt,
                (unsigned long long)r886, (unsigned long long)out1,
                (unsigned long long)out2);
            Log("loader",
                "mount-probe: init42 objA=%llx objB=%llx ba0obj=%llx "
                "devglob=%llx alloc18=%llx tlsIdx=%lu",
                (unsigned long long)objA, (unsigned long long)objB,
                (unsigned long long)ba0, (unsigned long long)dev,
                (unsigned long long)a18, (unsigned long)tlsIdx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: init42=<unreadable>");
        }
        // Boot #43: bracket the stop point inside 0x142478440.
        // Boot #42 proved the setter (0x1424792f4) never runs
        // (g130=0, flags=0, r886=0, alloc18=0) while the two
        // 0x1417f6a30 objects ARE allocated — and the log shows only
        // ONE GetDescriptorHandleIncrementSize (type 0) although the
        // code has five sequential slot-15 calls on the same object.
        // The stores below are written in order by the tail, so their
        // values bracket exactly how far the flow got:
        //   0x75e7000 = descriptor-size type-0 result (expect 32)
        //   0xa943aa0/a8 = slot-9 #1 result (0x142478f74/7b)
        //   0x75e7010 = sub-result (0x142478f9d, after slot-9 #2)
        //   0x75e7540..60 = the 0x14006de60 series stores
        //   0x75e7390/0x75e7420 = the slot-9 `this` globals
        __try {
            uint32_t d0 = *(uint32_t*)(base + 0x75e7000);
            uint64_t s9a = *(uint64_t*)(base + 0xa943aa0);
            uint64_t s9b = *(uint64_t*)(base + 0xa943aa8);
            uint64_t sub = *(uint64_t*)(base + 0x75e7010);
            uint64_t t1 = *(uint64_t*)(base + 0x75e7390);
            uint64_t t2 = *(uint64_t*)(base + 0x75e7420);
            Log("loader",
                "mount-probe: init43 d0=%lu s9a=%llx s9b=%llx "
                "sub=%llx t1=%llx t2=%llx",
                (unsigned long)d0, (unsigned long long)s9a,
                (unsigned long long)s9b, (unsigned long long)sub,
                (unsigned long long)t1, (unsigned long long)t2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: init43a=<unreadable>");
        }
        __try {
            uint64_t q0 = *(uint64_t*)(base + 0x75e7540);
            uint64_t q1 = *(uint64_t*)(base + 0x75e7548);
            uint64_t q2 = *(uint64_t*)(base + 0x75e7550);
            uint64_t q3 = *(uint64_t*)(base + 0x75e7558);
            uint64_t q4 = *(uint64_t*)(base + 0x75e7560);
            Log("loader",
                "mount-probe: init43 q0=%llx q1=%llx q2=%llx "
                "q3=%llx q4=%llx",
                (unsigned long long)q0, (unsigned long long)q1,
                (unsigned long long)q2, (unsigned long long)q3,
                (unsigned long long)q4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("loader", "mount-probe: init43b=<unreadable>");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "mount-probe: registry unreadable");
    }
}

// Boot #55 measurement (GPT follow-up, Seth-approved): operand
// provenance for the 2 MB bounds check in the title's 0x14247AD10
// (int3 at 0x14247AF2D). The check is 64-bit, unsigned (jbe):
//   ([rsp+0x78] - [rsp+0x70] + [0x1475e6ff0]) <= 0x1FFFFF
// [rsp+0x70]/[rsp+0x78] are the arena struct's two fields
// (cursor, capacity) and [0x1475e6ff0] is the arena base written by
// the bump-allocator callback (0x1456a2a60); the callback is the
// ONLY writer of [rsp+0x70] after the zeroing mov, and [rsp+0x78]
// is written ONLY by the capacity mov in 0x14247AD10. Log the
// faulting frame's operands from the VEH context, plus the
// neighbouring UAV-arg globals, the loop count, and the arena
// registration node (its +8 field is the struct address — it must
// equal RSP+0x70 if the callback ran against this frame's arena).
// The faulting frame IS 0x14247AD10 (the int3 is in it), so the
// crash-time slots are exactly the operands; no separate
// first-write capture is needed. LOGGING ONLY: no behaviour change,
// no NOP, the assert stays enabled.
static void LogBoot55Probe(EXCEPTION_POINTERS* ep) {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base || !ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return;
    // Only meaningful when the fault IS the 2 MB assert (unchanged).
    if ((uintptr_t)ep->ExceptionRecord->ExceptionAddress !=
        (uintptr_t)(base + 0x247AF2D)) {
        return;
    }
    uint64_t rsp = (uint64_t)ep->ContextRecord->Rsp;
    __try {
        uint64_t s70 = *(uint64_t*)(rsp + 0x70);
        uint64_t s78 = *(uint64_t*)(rsp + 0x78);
        uint64_t g = *(uint64_t*)(base + 0x75E6FF0);
        uint64_t total = s78 - s70 + g;  // same 64-bit wrap as the game
        Log("loader",
            "boot55-2mb: rsp=%llx [rsp+0x70]=%llx [rsp+0x78]=%llx "
            "G=%llx total=%llx pass=%d",
            (unsigned long long)rsp, (unsigned long long)s70,
            (unsigned long long)s78, (unsigned long long)g,
            (unsigned long long)total, (int)(total <= 0x1FFFFF));
        Log("loader",
            "boot55-2mb: regs rax=%llx rcx=%llx rdx=%llx r8=%llx "
            "r9=%llx r10=%llx r11=%llx",
            (unsigned long long)ep->ContextRecord->Rax,
            (unsigned long long)ep->ContextRecord->Rcx,
            (unsigned long long)ep->ContextRecord->Rdx,
            (unsigned long long)ep->ContextRecord->R8,
            (unsigned long long)ep->ContextRecord->R9,
            (unsigned long long)ep->ContextRecord->R10,
            (unsigned long long)ep->ContextRecord->R11);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "boot55-2mb: stack=<unreadable>");
    }
    __try {
        uint32_t a0 = *(uint32_t*)(base + 0x75E6FE0);
        uint32_t a1 = *(uint32_t*)(base + 0x75E6FE4);
        uint8_t a2 = *(uint8_t*)(base + 0x75E6FE8);
        uint64_t gs = *(uint64_t*)(base + 0x75E7A08);
        uint32_t cnt = *(uint32_t*)(base + 0xA94390C);
        Log("loader",
            "boot55-2mb: arg0=%lu arg1=%lu arg2=%u Gsize=%llx "
            "loopcount=%lu",
            (unsigned long)a0, (unsigned long)a1, (unsigned)a2,
            (unsigned long long)gs, (unsigned long)cnt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "boot55-2mb: globals=<unreadable>");
    }
    __try {
        // Arena registration: 0x145c8f990 pops a node from the free
        // list (head RVA 0x146843360) and pushes it onto the active
        // list (anchor RVA 0x146843758, first node at +0x18 =
        // 0x146843770). Node: +0 callback, +8 &struct, +0x10 obj,
        // +0x18 next. Walk up to 4 nodes.
        void* node = *(void**)(base + 0x146843770);
        for (int i = 0; i < 4 && node; ++i) {
            uint64_t ncb = *(uint64_t*)((uint8_t*)node + 0x0);
            uint64_t nstruct = *(uint64_t*)((uint8_t*)node + 0x8);
            uint64_t nobj = *(uint64_t*)((uint8_t*)node + 0x10);
            void* next = *(void**)((uint8_t*)node + 0x18);
            Log("loader",
                "boot55-2mb: arenanode[%d] node=%p callback=%llx "
                "struct=%llx obj=%llx rsp70=%llx match=%d",
                i, node, (unsigned long long)ncb,
                (unsigned long long)nstruct, (unsigned long long)nobj,
                (unsigned long long)(rsp + 0x70),
                (int)(nstruct == rsp + 0x70));
            node = next;
        }
        if (!node)
            Log("loader", "boot55-2mb: arenanode: end of list");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "boot55-2mb: arenanode=<unreadable>");
    }
}

// Boot #57 arena telemetry (GPT follow-up #7): the 2 MB assert may
// PASS once slot 127 creates real resources, so the boot55 probe
// (gated on the assert fault) may never fire. Log the arena state
// (G, Gsize, UAV args, loop count, registration nodes) on EVERY VEH
// fire, so whatever the next crash is, the log shows whether the
// arena was consumed. LOGGING ONLY.
// NOTE: the boot55 arenanode walk has a probe bug — it adds the full
// VA 0x146843770 to the image base instead of the RVA 0x6843770, so
// it reads garbage. This function uses the correct RVA.
static void LogBoot57Arena() {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base) return;
    __try {
        uint64_t g = *(uint64_t*)(base + 0x75E6FF0);
        uint64_t gs = *(uint64_t*)(base + 0x75E7A08);
        uint32_t a0 = *(uint32_t*)(base + 0x75E6FE0);
        uint32_t a1 = *(uint32_t*)(base + 0x75E6FE4);
        uint8_t a2 = *(uint8_t*)(base + 0x75E6FE8);
        uint32_t cnt = *(uint32_t*)(base + 0xA94390C);
        Log("loader",
            "boot57-arena: G=%llx Gsize=%llx arg0=%lu arg1=%lu "
            "arg2=%u loopcount=%lu",
            (unsigned long long)g, (unsigned long long)gs,
            (unsigned long)a0, (unsigned long)a1, (unsigned)a2,
            (unsigned long)cnt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "boot57-arena: globals=<unreadable>");
    }
    __try {
        // Arena registration: 0x145c8f990 pushes nodes onto list A
        // (anchor RVA 0x146843758, first node at +0x18, i.e. RVA
        // 0x6843770). Node: +0 callback, +8 &struct, +0x10 obj,
        // +0x18 next. Walk up to 4 nodes.
        void* node = *(void**)(base + 0x6843770);
        for (int i = 0; i < 4 && node; ++i) {
            uint64_t ncb = *(uint64_t*)((uint8_t*)node + 0x0);
            uint64_t nstruct = *(uint64_t*)((uint8_t*)node + 0x8);
            uint64_t nobj = *(uint64_t*)((uint8_t*)node + 0x10);
            void* next = *(void**)((uint8_t*)node + 0x18);
            Log("loader",
                "boot57-arena: arenanode[%d] node=%p callback=%llx "
                "struct=%llx obj=%llx",
                i, node, (unsigned long long)ncb,
                (unsigned long long)nstruct,
                (unsigned long long)nobj);
            node = next;
        }
        if (!node)
            Log("loader", "boot57-arena: arenanode: end of list");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "boot57-arena: arenanode=<unreadable>");
    }
}

// Build 83: streaming-context entry snapshot.
//
// Boot #63 (#62 chain) faults at 0x1428C9A08 -- `mov rax,[rcx]` with
// rcx = [rsi+0x18] -- inside the game's streaming submit function
// 0x1428C9960, whose first act is `mov rsi, rcx`, so the faulting rsi
// still holds the streaming context in the exception context record.
// That context is a guest "sdms" record (magic 0x6d736473 at +0x00)
// built by the game constructor at 0x1428C9460 and published to the
// image global 0x79F6D70. This probe names the exact record and dumps
// every field the submit path reads, at fault time, without executing
// or modifying guest code.
static void LogStreamContextProbe(EXCEPTION_POINTERS* ep) {
    uint8_t* base = (uint8_t*)g_guestBaseForVeh;
    if (!base || !ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return;
    uint64_t ctx = (uint64_t)ep->ContextRecord->Rsi;
    uint64_t sub = (uint64_t)ep->ContextRecord->Rcx;
    uint64_t magic = 0, reg = 0;
    __try {
        reg = *(uint64_t*)(base + 0x79F6D70);
        magic = ctx ? *(uint64_t*)ctx : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "ctx-probe: ctx=0x%llx <unreadable>",
            (unsigned long long)ctx);
        return;
    }
    Log("loader",
        "ctx-probe: fault=%p rsi(ctx)=0x%llx rcx(sub-object)=0x%llx "
        "magic=0x%llx registered=0x%llx",
        ep->ExceptionRecord->ExceptionAddress, (unsigned long long)ctx,
        (unsigned long long)sub, (unsigned long long)magic,
        (unsigned long long)reg);
    if (magic != 0x6D736473ull)
        return;
    __try {
        uint32_t v08 = *(uint32_t*)(ctx + 0x08);
        uint32_t v0c = *(uint32_t*)(ctx + 0x0C);
        uint64_t v10 = *(uint64_t*)(ctx + 0x10);
        uint64_t v18 = *(uint64_t*)(ctx + 0x18);
        uint64_t v20 = *(uint64_t*)(ctx + 0x20);
        uint64_t v28 = *(uint64_t*)(ctx + 0x28);
        uint64_t v30 = *(uint64_t*)(ctx + 0x30);
        uint64_t v38 = *(uint64_t*)(ctx + 0x38);
        uint32_t v64 = *(uint32_t*)(ctx + 0x64);
        uint32_t v68 = *(uint32_t*)(ctx + 0x68);
        Log("loader",
            "ctx-probe: ctx=0x%llx owner=0x%llx sub[+0x18]=0x%llx "
            "arg8=%u argc=%u aux[+0x20]=0x%llx aux[+0x28]=0x%llx "
            "out[+0x30]=0x%llx arr[+0x38]=0x%llx used=%u cap=%u",
            (unsigned long long)ctx, (unsigned long long)v10,
            (unsigned long long)v18, v08, v0c, (unsigned long long)v20,
            (unsigned long long)v28, (unsigned long long)v30,
            (unsigned long long)v38, v64, v68);
        // A null sub-object here is the 0xC0000005 read of 0x0 at
        // 0x1428C9A08: the record exists, is registered, and its
        // backend field was never populated.
        Log("loader", "ctx-probe: fault-kind=%s",
            v18 ? "sub-object-present" : "sub-object-null");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("loader", "ctx-probe: ctx=0x%llx fields=<unreadable>",
            (unsigned long long)ctx);
    }
}

static LONG CALLBACK ScarlettVehHandler(EXCEPTION_POINTERS* ep) {    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    // Our own watchdog's guarded probes are not guest faults.
    if (t_wpThread) return EXCEPTION_CONTINUE_SEARCH;
    // Boot #25: the fault moved from the pack assert (0x80000003) to an
    // access violation (0xc0000005, NULL-deref in the sibling pack fn).
    // Catch both so the mount probe + dump fire at the AV too — the
    // 0x80000003-only gate is why boot #25 produced no probe.
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Build 84: hardware write-watchpoint traps (DR0). Logged and resumed
    // in place; never treated as a crash.
    if (code == 0x80000004) {
        CONTEXT* c = ep->ContextRecord;
        // Boot #101 probe stage: DR0 watches the empty container array slot;
        // log the slot bitmap the creator's allocator would have read.
        if (g_wpStage == 13) {
            uint64_t svc = 0, fn = 0;
            __try {
                svc = *(uint64_t*)((uint8_t*)g_guestBaseForVeh + 0x8760D00);
                fn = c && c->Rax ? *(uint64_t*)(c->Rax + 0x138) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            Log("loader",
                "[svcwp] precall rip=0x%llx obj=0x%llx vt=0x%llx fn=0x%llx "
                "rdx=0x%llx r8=0x%llx r9=0x%llx svcGlobal=0x%llx thread=%lu",
                (unsigned long long)(c ? c->Rip : 0),
                (unsigned long long)(c ? c->Rcx : 0),
                (unsigned long long)(c ? c->Rax : 0), (unsigned long long)fn,
                (unsigned long long)(c ? c->Rdx : 0),
                (unsigned long long)(c ? c->R8 : 0),
                (unsigned long long)(c ? c->R9 : 0), (unsigned long long)svc,
                (unsigned long)GetCurrentThreadId());
            g_wpStage = 10;
            if (c) {
                c->Dr0 = 0;
                c->Dr7 = 0;
                c->EFlags |= 0x10000; // RF: execute once, no retrigger
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (g_wpStage == 9) {
            uint64_t stored = 0, bmp = 0;
            __try {
                stored = *(uint64_t*)((uint8_t*)g_guestBaseForVeh + 0xA943A38);
                bmp = *(uint32_t*)((uint8_t*)g_guestBaseForVeh + 0x6891A08);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            Log("loader",
                "[arrwp] trap rip=0x%llx stored=0x%llx bitmap=0x%x thread=%lu "
                "dr6=0x%llx",
                (unsigned long long)(c ? c->Rip : 0),
                (unsigned long long)stored, (unsigned)bmp,
                (unsigned long)GetCurrentThreadId(),
                (unsigned long long)(c ? c->Dr6 : 0));
            g_wpStage = 10;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (!c || g_wpStage == 0 || g_wpStage == 3)
            return EXCEPTION_CONTINUE_SEARCH;
        // Stage 1: this trap is the record publish. Re-arm THIS thread on
        // record+0x18 inside the exception context, so the window between
        // publish and first use cannot be missed by the poller.
        if (g_wpStage == 1) {
            uint64_t rec = 0, magic = 0, owner = 0;
            __try {
                rec = *(uint64_t*)((uint8_t*)g_guestBaseForVeh + 0x79F6D70);
                magic = rec ? *(uint64_t*)rec : 0;
                owner = rec ? *(uint64_t*)(rec + 0x10) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            const long n1 = InterlockedIncrement(&g_wpTrapCount);
            Log("loader",
                "[watch] trap #%ld stage1 rip=0x%llx thread=%lu global "
                "value=0x%llx magic=0x%llx",
                n1, (unsigned long long)c->Rip,
                (unsigned long)GetCurrentThreadId(),
                (unsigned long long)rec, (unsigned long long)magic);
            WpLogStack(c);
            c->Dr6 = 0;
            if (magic == 0x6D736473ull) {
                g_wpRecord = (LONG64)rec;
                g_wpStage = 2;
                c->Dr0 = rec + 0x18;
                c->Dr7 = kWpDr7Write8;
                Log("loader",
                    "[watch] stage2 (from trap): record 0x%llx owner=0x%llx "
                    "-> DR0=0x%llx re-armed in-context",
                    (unsigned long long)rec, (unsigned long long)owner,
                    (unsigned long long)(rec + 0x18));
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const LONG64 rec = g_wpRecord;
        const uint64_t field = rec ? (uint64_t)rec + 0x18 : 0;
        uint64_t val = 0;
        __try {
            val = field ? *(uint64_t*)field : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const long n = InterlockedIncrement(&g_wpTrapCount);
        Log("loader",
            "[watch] trap #%ld dr6=0x%llx rip=0x%llx thread=%lu stage=%ld "
            "field=0x%llx value=0x%llx",
            n, (unsigned long long)c->Dr6, (unsigned long long)c->Rip,
            (unsigned long)GetCurrentThreadId(), (long)g_wpStage,
            (unsigned long long)field, (unsigned long long)val);
        WpLogStack(c);
        c->Dr6 = 0;
        if (n >= 64)
            g_wpStage = 3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != 0x80000003 && code != 0xc0000005)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_vehDumped, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    Log("loader", "veh: exception 0x%08lx at %p thread %lu",
        (unsigned long)code,
        ep->ExceptionRecord->ExceptionAddress,
        (unsigned long)GetCurrentThreadId());
    // Boot #46: the fault IP can be inside a system DLL (not the game
    // image), so resolve it to a module name + offset — otherwise the
    // crashing DLL is unknowable from the log alone.
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod) &&
            mod) {
            char modPath[MAX_PATH] = {};
            GetModuleFileNameA(mod, modPath, MAX_PATH);
            const char* base = strrchr(modPath, '\\');
            Log("loader", "veh: fault module %s +0x%llx (params[0]=0x%llx params[1]=0x%llx)",
                base ? base + 1 : modPath,
                (unsigned long long)((uintptr_t)ep->ExceptionRecord
                                         ->ExceptionAddress -
                                     (uintptr_t)mod),
                (unsigned long long)ep->ExceptionRecord
                    ->ExceptionInformation[0],
                (unsigned long long)ep->ExceptionRecord
                    ->ExceptionInformation[1]);
        }
    }
    LogMountProbe();
    LogBoot55Probe(ep);
    LogBoot57Arena();
    LogStreamContextProbe(ep);
    WriteCrashDump(ep, "veh-breakpoint");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Crash filter for the guest entry point (real-exe boot #6 / H1 harness
// improvement). Runs in the exception
// filter expression, where GetExceptionInformation() is valid — the
// __except handler body cannot see the record. Logs the exception
// record address (so the next boot's fault address is known, not just
// the code) and writes a full-memory minidump so the guest image —
// including the allocator singleton in .data — is readable on the next
// failure. WER's small dumps did not contain it. The dump goes next to
// the harness exe (F:\ScarlettCompat), NEVER next to the guest: the
// child CWD is the guest dir (D:\game\game), which is read-only for us.
// NOTE: MiniDumpWithFullMemory includes all committed memory, so the
// dump is ~5 GB (the committed XMem arenas); the two 24 GB arenas are
// reserved-only and are not included.
LONG CrashFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashDump(ep, "entry");
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- App-manifest mount registration (the title pack-table blocker) ----
// Boot #15 proved the pack filesystem's first lookup is for the bare
// filename "appdata0.rpf", and the VFS mount registry is EMPTY
// (count=0), so the lookup returns 0 and pack-init asserts (int3 at
// 0x142237178). The game does NOT read AppxManifest.xml itself and
// imports NO Package/XPackage APIs, so registering the manifest with
// Windows cannot populate the table — the table is fed by the game's
// own VFS registration path (register fns 0x1412c5670/0x141fb2210),
// which never runs before pack-init. What DOES feed it is the app's
// data directory: the game needs a mount for the app (its manifest
// identity + data dir) so "appdata0.rpf" resolves.
//   So the layer registers the app manifest here: it reads
// AppxManifest.xml from the guest dir (logging the Identity), then
// populates the VFS mount registry DIRECTLY (writes the entries +
// globals). It does NOT call the game's register fn: boot #16 proved
// that fn cannot run pre-entry — its lazy-init allocator needs the
// game's memory pool, which is only constructed during static init,
// so the call faults in ntdll (WER: scarlett_verify.exe,
// ntdll.dll+0x1a12d, 0xc0000005) and the __try does not catch it.
// The registry is just data (count/array/entries), and the lookup
// only reads it, so the layer writes it. The mounts cover every form
// the name transform (0x14128bb70) can produce: "appdata/",
// "appdata0/", "./", "D:/" (drive letter from the guest dir).
// The mount object is a layer-built fake VFS filesystem object (same
// pattern as the stub COM object): a 64-slot vtable where every slot
// returns 0. Slot 45 (byte offset 0x168) is the "open/exists" the VFS
// lookup calls; returning 0 (!= -1) makes the lookup report the name
// as resolvable, so pack-init proceeds past the assert. The pack-mgr
// will then try to actually open/read appdata0.rpf — that is the NEXT
// fault to diagnose (a real file-serving object comes then, if the
// fake one cannot serve it). The "g:" scheme object cannot be used
// here: it lives in BSS and its vtable pointer is only set at runtime
// by the VFS initializer, which has not run yet at this point.
//   Called from RunXboxPe before the entry point (static init has not
// run, but the guest is mapped). No game code is called here, so a
// failure cannot kill the boot — the worst case is the registry
// stays empty and the boot proceeds to the same pack assert as before.
struct FakeVfsObject {
    void** vtable;
};
static void* g_fakeVfsVtable[64];
static FakeVfsObject g_fakeVfsObject;
extern "C" LONG FakeVfsGeneric() { return 0; }
struct FakeVfsVtableInit {
    FakeVfsVtableInit() {
        for (int i = 0; i < 64; ++i)
            g_fakeVfsVtable[i] = (void*)FakeVfsGeneric;
        g_fakeVfsObject.vtable = g_fakeVfsVtable;
    }
};
static FakeVfsVtableInit g_fakeVfsVtableInit;

// Mount-registry entry (0x20 bytes), as built by the game's register
// fn (0x1412c5670) and read by the mount-registry lookup (0x1428c8ec0).
// Layout confirmed from both functions' disassembly:
//   +0x00 name pointer (qword)   +0x08 kind (word)   +0x0a length (word)
//   +0x10 object-list capacity (dword)  +0x14 object count (dword)
//   +0x18 object-list pointer (qword, array of object pointers)
// The lookup returns the object directly when count==1 (no vtable
// call); otherwise it calls vtable slot 0x168 on the objects in turn.
struct MountRegistryEntry {
    const char* name;       // +0x00
    uint16_t kind;          // +0x08
    uint16_t length;        // +0x0a
    uint32_t reserved;      // +0x0c (padding; register fn leaves it zero)
    uint32_t objCapacity;   // +0x10
    uint32_t objCount;      // +0x14
    void** objList;         // +0x18
};
static_assert(sizeof(MountRegistryEntry) == 0x20, "mount entry is 0x20");
static_assert(offsetof(MountRegistryEntry, kind) == 0x08, "kind at +0x08");
static_assert(offsetof(MountRegistryEntry, length) == 0x0a, "len at +0x0a");
static_assert(offsetof(MountRegistryEntry, objCapacity) == 0x10, "cap +0x10");
static_assert(offsetof(MountRegistryEntry, objCount) == 0x14, "cnt +0x14");
static_assert(offsetof(MountRegistryEntry, objList) == 0x18, "list +0x18");
// Registry globals (RVAs, confirmed from the register fn + lookup
// disassembly): init flag 0x78f5ec0, count 0x78f5ec4, array 0x78f5ec8,
// lock 0x78f5ed0 (left 0 = unlocked). The entries array has room for
// 16 mounts so a later in-game registration can append without
// overflowing it.
static const uint32_t kMountInitFlagRva = 0x78f5ec0;
static const uint32_t kMountCountRva = 0x78f5ec4;
static const uint32_t kMountArrayRva = 0x78f5ec8;
static const int kMountCapacity = 16;
static MountRegistryEntry g_mountEntries[kMountCapacity];
static void* g_mountObjLists[4];
static char g_driveMount[8];

static void RegisterAppManifestMount(LoadedPe* pe) {
    // The registry globals live at RVAs 0x78f5ec0..0x78f5ed0; synthetic
    // guests are far smaller, so this skips them (no game code is
    // called here at all).
    if ((uint64_t)kMountArrayRva + 8 > pe->imageSize) {
        Log("loader",
            "app-manifest: mount registry outside image — not the "
            "real title, skipping mount registration");
        return;
    }
    // 1. Read the app manifest (AppxManifest.xml) from the guest dir
    //    and log the package Identity. This is the layer registering
    //    the app by its manifest.
    char manifest[8192] = {};
    bool haveManifest = false;
    {
        std::wstring mpath = pe->guestDir;
        if (!mpath.empty()) {
            mpath += L"\\appxmanifest.xml";
            HANDLE h = CreateFileW(mpath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD read = 0;
                if (ReadFile(h, manifest, sizeof(manifest) - 1, &read,
                             nullptr) &&
                    read > 0) {
                    haveManifest = true;
                }
                CloseHandle(h);
            }
        }
    }
    if (haveManifest) {
        // Minimal parse: <Identity Name="..." ... Version="..." />
        const char* id = strstr(manifest, "<Identity");
        const char* name = id ? strstr(id, "Name=\"") : nullptr;
        const char* ver = id ? strstr(id, "Version=\"") : nullptr;
        char idName[128] = {};
        char idVer[64] = {};
        if (name) {
            name += 6;
            size_t n = 0;
            while (n < sizeof(idName) - 1 && name[n] && name[n] != '"') {
                idName[n] = name[n];
                ++n;
            }
        }
        if (ver) {
            ver += 9;
            size_t n = 0;
            while (n < sizeof(idVer) - 1 && ver[n] && ver[n] != '"') {
                idVer[n] = ver[n];
                ++n;
            }
        }
        Log("loader",
            "app-manifest: registered \"%s\" version %s (from %ls)",
            idName, idVer, pe->guestDir);
    } else {
        Log("loader",
            "app-manifest: WARNING appxmanifest.xml not found in %ls; "
            "registering mounts anyway",
            pe->guestDir);
    }
    // 2. Populate the mount registry DIRECTLY (write the entries +
    //    globals), instead of calling the game's register fn.
    //    Boot #16 proved the register fn cannot run pre-entry: its
    //    lazy-init allocator needs the game's memory pool, which is
    //    only constructed during static init, so the call faults in
    //    ntdll (WER: scarlett_verify.exe, ntdll.dll+0x1a12d,
    //    0xc0000005) and the __try does not catch it (no register-
    //    mount log lines, no crash dump). The registry is just data,
    //    so the layer writes it: the lookup only reads count/array/
    //    entries, never how they got there. The mounts cover every
    //    form the name transform (0x14128bb70) can produce:
    //      "appdata/"  — the bare name "appdata0.rpf" (prefix "appdata")
    //      "appdata0/" — the bare name itself (longest prefix)
    //      "./"        — the transform prepends "." -> "./appdata0.rpf"
    //      "D:/"       — the transform may resolve "." to the absolute
    //                    drive path (drive letter from the guest dir)
    //    Each entry holds one object (count==1), so the lookup returns
    //    the fake VFS object directly; the pack-mgr lookup then calls
    //    its vtable slot 0x168, which returns 0 (!= -1), so the name
    //    resolves and pack-init proceeds past the assert.
    uint8_t* base = (uint8_t*)pe->base;
    g_driveMount[0] = 0;
    if (pe->guestDir[0] && pe->guestDir[1] == L':') {
        // e.g. "D:/" from "D:\game\game".
        g_driveMount[0] = (char)pe->guestDir[0];
        g_driveMount[1] = ':';
        g_driveMount[2] = '/';
        g_driveMount[3] = 0;
    }
    const char* names[4] = {"appdata/", "appdata0/", "./", g_driveMount};
    int mounted = 0;
    for (int i = 0; i < 4; ++i) {
        const char* mname = names[i];
        if (!mname[0]) continue;
        g_mountObjLists[i] = &g_fakeVfsObject;
        MountRegistryEntry& e = g_mountEntries[mounted];
        e.name = mname;
        e.kind = 1;
        e.length = (uint16_t)strlen(mname);
        e.objCapacity = 1;
        e.objCount = 1;
        e.objList = &g_mountObjLists[i];
        Log("loader", "app-manifest: mount \"%s\" -> entry %d (len %u)",
            mname, mounted, e.length);
        ++mounted;
    }
    *(uint32_t*)(base + kMountInitFlagRva) = 1;
    *(uint32_t*)(base + kMountCountRva) = (uint32_t)mounted;
    *(void**)(base + kMountArrayRva) = g_mountEntries;
    Log("loader",
        "app-manifest: mount registry populated: count=%d array=%p "
        "initFlag=1",
        mounted, (void*)g_mountEntries);
}

bool RunXboxPe(LoadedPe* pe, DWORD* exitCode) {
    typedef DWORD(WINAPI* EntryFn)();
    // Catch a fatal crash on ANY thread (boot #7 died on a worker
    // thread and the __except below never fired). Installed here so it
    // is live for the whole guest run.
    SetUnhandledExceptionFilter(UnhandledFilter);
    // Vectored-handler backstop (real-exe boot #13): fires first-chance on
    // ANY thread for the breakpoint code only, so an int3 assert dumps
    // + probes even if the game's own filter handling swallows the
    // unhandled path. Lives until process exit (the harness child exits
    // after the run); g_vehDumped is reset per run.
    g_guestBaseForVeh = pe->base;
    // Build 84: name the writer of the streaming record's backend field.
    // Armed before the guest entry so the publish is caught on whichever
    // thread performs it.
    StartStreamWatchpoint();
    g_vehDumped = 0;
    AddVectoredExceptionHandler(1, ScarlettVehHandler);
    // App-manifest mount registration (the title pack-table blocker): read
    // AppxManifest.xml and populate the VFS mount registry directly
    // so the pack filesystem can resolve "appdata0.rpf". The registry
    // globals live at title-specific RVAs; synthetic guests are
    // skipped by the bounds check inside. Runs before the entry
    // point; no game code is called here.
    if (pe->hasXbld) {
        RegisterAppManifestMount(pe);
    }
    // TLS state just before the entry point (diagnostic).
    if (pe->tlsIndexValid) {
        void* check = TlsGetValue(pe->tlsIndex);
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        Log("loader",
            "TLS pre-entry: TlsGetValue=%p TEB->TLS[%lu]=%p "
            "GA view=%p view[0]=%p (guest block %p)",
            check, pe->tlsIndex, tlsArr[pe->tlsIndex], pe->gaView,
            pe->gaView ? ((void**)pe->gaView)[0] : nullptr,
            pe->tlsBlock);
    }
    __try {
        DWORD rc = ((EntryFn)pe->entry)();
        *exitCode = rc;
        return true;
    } __except (CrashFilter(GetExceptionInformation())) {
        *exitCode = (DWORD)GetExceptionCode();
        Log("loader", "guest crashed in entry: exception 0x%08x",
            *exitCode);
        return false;
    }
}

void FreeXboxPe(LoadedPe* pe) {
    if (pe->tlsIndexValid) {
        TlsSetValue(pe->tlsIndex, nullptr);
        if (pe->tlsBlock) HeapFree(GetProcessHeap(), 0, pe->tlsBlock);
        FreeGaView();  // clears TlsSlots[gaSlot] and frees the view
        TlsFree(pe->tlsIndex);
    }
    if (pe->base) VirtualFree(pe->base, 0, MEM_RELEASE);
    memset(pe, 0, sizeof(*pe));
}

}  // namespace sc
