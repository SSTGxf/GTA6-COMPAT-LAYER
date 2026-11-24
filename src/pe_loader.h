// pe_loader.h — Xbox Series X PE32+ loader.
// Maps a native x86-64 PE into the host process (same ISA, no CPU
// emulation), validates the Xbox .xbld section, applies relocations, and
// resolves imports: Xbox DLLs (d3d12_xs, xmem, xgameruntime, ...) resolve
// to the HLE registry; api-ms-win-* / system DLLs forward to real Windows.
#pragma once
#include "scarlett.h"

namespace sc {

struct LoadedPe {
    void* base = nullptr;       // Mapped image base.
    void* entry = nullptr;      // Entry point (after relocation).
    size_t imageSize = 0;       // OptionalHeader.SizeOfImage (mapped size).
    wchar_t guestDir[MAX_PATH] = {};  // Directory of the exe (guest data
                                      // dir); used to find AppxManifest.xml.
    uint64_t preferredBase = 0;
    bool relocated = false;     // True if mapped away from preferred base.
    char xbldEdition[128] = {}; // e.g. "GXDK,October 2026 GXDK"
    char xbldBuild[64] = {};    // e.g. "GXDK,10.0.26100.9605"
    bool hasXbld = false;
    bool hasTls = false;        // TLS directory present.
    void* tlsBlock = nullptr;   // Per-thread TLS block (this thread).
    DWORD tlsIndex = 0;         // TlsAlloc index for this module.
    bool tlsIndexValid = false;
    void* gaView = nullptr;     // Per-thread /GA view (this thread).
    bool eraDetected = false;   // True when the guest imports ERA modules
                                // (phase 5): its ERA imports route through
                                // the ERA -> GDK translation.
    uint32_t importModules = 0;
    uint32_t importsResolved = 0;
    uint32_t importsFailed = 0;
};

// Map an Xbox PE32+ into this process. Does NOT run it.
bool LoadXboxPe(const wchar_t* path, LoadedPe* out, char* err, size_t errSize);

// Run the entry point on the calling thread inside an SEH guard.
// NOTE: a normal EXE entry (mainCRTStartup) calls ExitProcess on return,
// so the caller is expected to be a throwaway child process (see
// scarlett_verify --run). Returns false on SEH exception; exitCode is the
// guest's return value or the exception code.
bool RunXboxPe(LoadedPe* pe, DWORD* exitCode);

// Guest image base (set by RunXboxPe before the entry runs). The HLE
// layer reads guest globals / .data dispatch slots through it
// (boot #58 pool-dispatcher invocation).
void* ScGuestBase();

// Build 84: current value of the watched streaming record's backend field
// ([record+0x18], the null sub-object of fault 0x1428C9A08), or 0 while
// no record has been published. Read-only diagnostic for the DR0
// watchpoint; the slot logs print it so the value on both sides of any
// diverted guest branch is on record.
unsigned long long ScWatchRecordBackend();

// Calling thread's game TLS block as the guest's patched code sees
// it (the /GA view's [0] entry), or nullptr if no view is
// installed. Read-only diagnostic for the boot #59 obj-filter
// capture (GPT #9) — the dispatcher's TLS helper reads its context
// from the TLS block, and which block it reads (real array[0] vs
// /GA view[0] vs real array[10]) is what the capture disambiguates.
void* ScGuestGaViewBlock();

void FreeXboxPe(LoadedPe* pe);

}  // namespace sc
