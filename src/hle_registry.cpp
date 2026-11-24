// hle_registry.cpp — the HLE export registry (Xenia-style) + logging.
// Central table: (Xbox DLL, function) -> host implementation + status tag.
// The loader resolves guest imports through FindExport(); anything missing
// gets the E_NOTIMPL stub. Statuses are the project's tracking surface —
// see docs/STATUS.md.
#include "hle_registry.h"
#include "pe_loader.h"  // ScGuestBase (boot #58 pool-dispatcher invocation)
#include "era_to_gdk.h"  // ERA -> GDK translation (phase 5)
#include "software_rt.h"  // Software ray tracing backend
#include "fixed_timestep.h"  // Fixed-timestep physics decoupling
#include "xbox_shims.h"
#include "xg_texture.h"  // xg_xs texture computer (build 80, real object)
#include "opt_dstorage.h"  // real pack reads for the DStorage queue (D1)
#include <mmsystem.h>  // timeBeginPeriod (frame pacing, phase 3)
#include <dxgi.h>  // IDXGIDevice / IDXGIAdapter (device QI forwarding)
#include <string.h>
#include <vector>
#include <utility>   // std::index_sequence (per-slot unimplemented stubs, build 82)
#include <intrin.h>  // _ReturnAddress (COM wrapper unimplemented-slot log)

namespace sc {

namespace {

// ---- logging ----

// Build 88: logging must not charge the guest for disk I/O. The old
// writer did an unbuffered fputs(stderr) plus fopen/fputs/fclose of
// scarlett.log on *every* line, which costs 10-15 ms under disk load.
// ScheduleFrameEventX stamps the frame QPC before logging, so that cost
// landed inside the guest's frame period and made a 50 ms sleep measure
// 64.5 ms (ft_hook: 15 steps instead of 12). stderr and the file are now
// buffered and flushed on a 200 ms cadence, and ScLogFlush() is called
// from the crash path before the minidump so nothing is lost on a fault.
static FILE* g_logFile = nullptr;
static bool g_logInit = false;
static ULONGLONG g_lastLogFlush = 0;

static void EnsureLogInit() {
    if (g_logInit) return;
    g_logInit = true;
    static char errBuf[1 << 20];
    setvbuf(stderr, errBuf, _IOFBF, sizeof(errBuf));
    wchar_t logPath[MAX_PATH] = {};
    DWORD logPathLen = GetEnvironmentVariableW(
        L"SCARLETT_LOG_PATH", logPath, MAX_PATH);
    int openResult = (logPathLen > 0 && logPathLen < MAX_PATH)
                         ? _wfopen_s(&g_logFile, logPath, L"a")
                         : fopen_s(&g_logFile, "scarlett.log", "a");
    if (openResult == 0 && g_logFile) {
        static char fileBuf[1 << 20];
        setvbuf(g_logFile, fileBuf, _IOFBF, sizeof(fileBuf));
    } else {
        g_logFile = nullptr;
    }
    g_lastLogFlush = GetTickCount64();
}

void WriteLogLine(const char* line) {
    EnsureLogInit();
    fputs(line, stderr);
    fputc('\n', stderr);
    if (g_logFile) {
        fputs(line, g_logFile);
        fputc('\n', g_logFile);
    }
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLogFlush >= 200) {
        fflush(stderr);
        if (g_logFile) fflush(g_logFile);
        g_lastLogFlush = now;
    }
}

}  // namespace

// Flush every buffered log line. Called from the crash path (before the
// minidump is written) and at process exit, so buffering never loses the
// lines that matter.
void ScLogFlush() {
    fflush(stderr);
    if (g_logFile) fflush(g_logFile);
    g_lastLogFlush = GetTickCount64();
}

void Log(const char* subsystem, const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1200];
    snprintf(line, sizeof(line), "[%-10s] %s", subsystem, body);
    WriteLogLine(line);
}

extern "C" HRESULT ScNotImplementedStub() {
    Log("hle", "called unimplemented import -> E_NOTIMPL");
    return 0x80004001;  // E_NOTIMPL
}

// ---- layer-wide state ----

// Device created by D3D12XboxCreateDevice (real desktop device, cached so
// later steps — swapchain, pacing — can use it).
static ID3D12Device* g_xsDevice = nullptr;

// Boot #58 XMemAllocDefault diagnostics: the arena callback's inner
// allocator IS the layer's XMemAllocDefault shim, so its call count /
// last size / last base are the corroborating evidence that the
// dispatcher chain ran (first dispatch: +1 call, 64 MiB requested,
// base returned; second dispatch: +0 — G is already set). Plain
// stores + one interlocked increment — no locks, no allocation, no
// I/O — so they are safe to update from inside the callback chain.
static volatile LONG g_xmemAllocDefaultCalls = 0;
static volatile SIZE_T g_xmemAllocDefaultLastSize = 0;
static volatile void* g_xmemAllocDefaultLastBase = nullptr;

// Whether the loaded guest is a real Xbox title (.xbld present), set
// by the loader. The Xbox COM wrapper only applies to real titles.
bool g_scGuestIsRealXboxTitle = false;

// Frame pacing state (real, Sleep-based): the interval set by
// SetFrameIntervalX (1 = 60 Hz) defines the period; ScheduleFrameEventX
// stamps a frame; WaitFrameEventX sleeps until that frame's deadline.
static UINT32 g_frameInterval = 1;
static UINT64 g_lastFrameId = 0;
static LARGE_INTEGER g_lastFrameQpc = {};
static bool g_haveFrame = false;

static double FramePeriodMs() {
    return (double)g_frameInterval * (1000.0 / 60.0);
}

// ---- host implementations ----

extern "C" HRESULT HleXGameRuntimeInitialize() {
    Log("xgrt", "XGameRuntimeInitialize() -> S_OK");
    return S_OK;
}

extern "C" void* HleXMemAlloc(SIZE_T size, DWORD attrs) {
    void* p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    Log("xmem", "XMemAlloc(%llu, attrs 0x%x) -> %p", (unsigned long long)size,
        attrs, p);
    return p;
}

extern "C" void HleXMemFree(void* p) {
    Log("xmem", "XMemFree(%p)", p);
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

// ---- real-exe xmem additions (grounded 2026-11-23 from the exe's
// import table; XMemVirtualAlloc's 5-arg form from its call site at
// 0x14193e220, which passes type 0x20003000 and attributes 0x2004) ----
extern "C" void* HleXMemAllocDefault(SIZE_T size) {
    // Boot #58 (GPT consultation #8, safeguard B): MINIMAL — the
    // allocation and nothing else. NO logging. This shim is called
    // from inside the game's dispatcher chain (slot 127 ->
    // dispatcher -> arena callback -> XMemAllocDefault), and Log()
    // does file I/O + formatting, which must not run inside the
    // callback chain. The diagnostic counters above are plain
    // stores / one interlocked increment (safe in-chain). HleXMemAlloc
    // (the attrs variant) keeps its logging — it is not on the
    // dispatcher path. NOTE: direct game calls to XMemAllocDefault
    // elsewhere in the boot are now silent too; the count delta
    // around each dispatch call is what the boot #58 snapshot uses.
    void* p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    InterlockedIncrement(&g_xmemAllocDefaultCalls);
    g_xmemAllocDefaultLastSize = size;
    g_xmemAllocDefaultLastBase = p;
    return p;
}

extern "C" void HleXMemFreeDefault(void* p) { HleXMemFree(p); }

extern "C" void* HleXMemVirtualAlloc(void* lpAddress, SIZE_T dwSize,
                                     DWORD flAllocationType,
                                     DWORD flProtect,
                                     DWORD dwMemoryAttributes) {
    // Same Xbox alloc-type bits as kernelx!VirtualAlloc (0x20003000),
    // so the same stripping applies (HleVirtualAlloc). The protect
    // value at the call site (0x283) is an Xbox protection encoding,
    // not a desktop PAGE_* value — desktop VirtualAlloc rejects it, so
    // the wrapper retries with PAGE_READWRITE when the given protect
    // fails. Returns the allocated VA (NULL on failure).
    void* p = HleVirtualAlloc(lpAddress, dwSize, flAllocationType,
                              flProtect);
    if (!p) {
        p = HleVirtualAlloc(lpAddress, dwSize, flAllocationType,
                            PAGE_READWRITE);
    }
    Log("xmem",
        "XMemVirtualAlloc(%p, %llu, type 0x%08x, protect 0x%08x, "
        "attrs 0x%08x) -> %p",
        lpAddress, (unsigned long long)dwSize, flAllocationType,
        flProtect, dwMemoryAttributes, p);
    return p;
}

// XMemGetWorkingSetStatistics — S_OK stub. The stats struct layout is
// GDKX-gated, so the layer does not fill it yet; the game only needs
// the call to succeed at boot. (An earlier boot diagnosis proved the
// game never calls the ERA TitleMemoryStatus before crashing, so a
// zero-filled stats struct is not load-bearing here either.)
extern "C" HRESULT HleXMemGetWorkingSetStatistics(void* pStats) {
    Log("xmem",
        "XMemGetWorkingSetStatistics(%p) -> S_OK (STUB: struct not "
        "filled, layout GDKX-gated)",
        pStats);
    return S_OK;
}

// XMemIsToolsMemAvailable — FALSE: tools (devkit) memory does not
// exist on a retail desktop. Correct answer, not a stub.
extern "C" BOOL HleXMemIsToolsMemAvailable(void) {
    Log("xmem", "XMemIsToolsMemAvailable() -> FALSE (no devkit memory)");
    return FALSE;
}

// XMemAllocatePhysicalPages / XMemFreePhysicalPages.
// Xbox physical pages (GPU memory) have no desktop equivalent; the
// game allocates them for GPU-side use and the layer has no pool to
// hand out yet. The signatures are GDKX-gated; the stubs ignore args
// (x64 caller cleans the stack, so arg count does not matter).
//   Boot #27: the game's allocator wrapper (0x145eb2780) calls
// XMemAllocatePhysicalPages and asserts the return is 1
// (cmp eax,1; jne int3) — it treats the return as a BOOL-like success
// (1), NOT as an HRESULT. Returning S_OK (0) reads as FALSE/failure
// and trips the assert at 0x145eb27be. So this stub returns 1.
// The caller pre-fills the out-param (*rdx) with the requested page
// count and checks it is unchanged after the call, so the stub must
// NOT touch the out-param — it returns 1 and leaves *rdx alone.
// There is still no real physical-page pool behind this: the game
// will hit XMemMapPhysicalPages later if it actually maps the pages,
// and that is a separate (bigger) piece of work.
extern "C" HRESULT HleXMemAllocatePhysicalPages(void) {
    Log("xmem",
        "XMemAllocatePhysicalPages() -> 1 (STUB: no desktop "
        "physical-page pool; game expects 1, not S_OK)");
    return (HRESULT)1;
}

static void ScatterUnmapAll();

extern "C" HRESULT HleXMemFreePhysicalPages(void) {
    Log("xmem",
        "XMemFreePhysicalPages() -> S_OK (releases tracked scatter "
        "reservations; no desktop physical-page pool is emulated)");
    ScatterUnmapAll();
    return S_OK;
}

// XMemMapPhysicalPages / XMemMapPhysicalPagesScatter — map Xbox
// physical pages into a desktop VA. On desktop there is no separate
// physical-page pool, so this commits ordinary memory and returns its
// VA. Signature ASSUMED (GDKX-gated): (pages, pageCount, attrs,
// protect). The size is guarded: a bogus page count (wrong signature)
// cannot reserve more than 256 MB or less than 64 KB.
extern "C" void* HleXMemMapPhysicalPages(const uint64_t* pPhysicalPages,
                                         uint32_t dwPageCount,
                                         DWORD dwMemoryAttributes,
                                         DWORD flProtect) {
    SIZE_T bytes = (SIZE_T)dwPageCount * 4096;
    if (bytes < 64 * 1024) bytes = 64 * 1024;
    if (bytes > 256 * 1024 * 1024) bytes = 256 * 1024 * 1024;
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    Log("xmem",
        "XMemMapPhysicalPages(pages %p, count %u, attrs 0x%x, protect "
        "0x%x) -> %p (%llu bytes, ordinary memory)",
        (const void*)pPhysicalPages, dwPageCount, dwMemoryAttributes,
        flProtect, p, (unsigned long long)bytes);
    return p;
}

// XMemMapPhysicalPagesScatter — ABI measured from the guest, not assumed
// (build 86). Sole vtable-thunk callers 0x140F2CC3E and 0x140B18DDE:
//   rcx = page-descriptor array, edx = page count, r8 = second array.
//   r9 is *not* an argument (leftover register garbage).
//   The result is a STATUS: both callers run `test eax,eax; jne ok; int3`
//   (0x140F2CC47/0x140F2CC4B and 0x140B18DE3/0x140B18DE7).
//
// Build 87 semantics — read off the guest, not guessed:
//   * The descriptor array dumped at 0x140F2CC38 holds 64 KB-strided,
//     4 KB-aligned addresses (0x27174480000, +0x10000, ...), i.e. the
//     guest is asking for one page at each supplied address — the
//     "scatter" layout (6720 pages spread over a 440 MB span).
//   * The second array is zero-filled scratch: after the call the guest
//     only frees it (0x140F2CCD8 -> 0x144D9C130) and never reads it, so
//     per-page results are written there but nothing depends on them.
//   * There is no layer-side pool to draw from: the mapping is performed
//     at the guest's own addresses (reserve the span if free, commit one
//     page at each descriptor) and the reservation is tracked so it is
//     released exactly once.
struct ScatterReservation {
    void* base;
    SIZE_T span;
    uint32_t pages;
    bool owned;  // true when this layer reserved the span
};
static ScatterReservation g_scatterMaps[8];
static int g_scatterMapCount = 0;

static void ScatterUnmapAll() {
    for (int i = 0; i < g_scatterMapCount; ++i) {
        ScatterReservation& r = g_scatterMaps[i];
        if (!r.base) continue;
        if (r.owned) {
            BOOL ok = VirtualFree(r.base, 0, MEM_RELEASE);
            Log("xmem",
                "XMemMapPhysicalPagesScatter: released reservation "
                "%p span %llu (%u pages) ok=%d",
                r.base, (unsigned long long)r.span, r.pages, (int)ok);
        } else {
            Log("xmem",
                "XMemMapPhysicalPagesScatter: reservation %p span %llu "
                "(%u pages) belongs to the layer arena; not released",
                r.base, (unsigned long long)r.span, r.pages);
        }
        r.base = nullptr;
        r.span = 0;
        r.pages = 0;
    }
    g_scatterMapCount = 0;
}

extern "C" BOOL HleXMemMapPhysicalPagesScatter(const uint64_t* pPages,
                                              uint32_t dwPageCount,
                                              void* pOutArray) {
    void* caller = _ReturnAddress();
    if (!pPages || dwPageCount == 0) {
        Log("xmem",
            "XMemMapPhysicalPagesScatter(pages %p, count %u, out %p) from "
            "%p -> FALSE (no descriptors)",
            (const void*)pPages, dwPageCount, pOutArray, caller);
        return FALSE;
    }

    uint64_t first = pPages[0];
    uint64_t second = dwPageCount > 1 ? pPages[1] : 0;
    uint64_t last = pPages[dwPageCount - 1];
    uint64_t out0 = pOutArray ? ((const uint64_t*)pOutArray)[0] : 0;
    Log("xmem",
        "XMemMapPhysicalPagesScatter(pages %p, count %u, out %p) from "
        "%p: d[0]=0x%llx d[1]=0x%llx d[%u]=0x%llx out[0]=0x%llx",
        (const void*)pPages, dwPageCount, pOutArray, caller,
        (unsigned long long)first, (unsigned long long)second,
        dwPageCount - 1, (unsigned long long)last,
        (unsigned long long)out0);

    // The mapping target is the guest's own address list. Reserve the
    // span only if it is still free; a span the layer arena already
    // reserved is committed page by page and left to its owner.
    SIZE_T span = (SIZE_T)(last - first) + 0x1000;
    void* base = (void*)(uintptr_t)first;
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(base, &mbi, sizeof(mbi));
    bool owned = false;
    if (mbi.State == MEM_FREE) {
        if (!VirtualAlloc(base, span, MEM_RESERVE, PAGE_NOACCESS)) {
            Log("xmem",
                "XMemMapPhysicalPagesScatter: reserve %p span %llu failed "
                "err=%lu -> FALSE",
                base, (unsigned long long)span, GetLastError());
            return FALSE;
        }
        owned = true;
    }
    Log("xmem",
        "XMemMapPhysicalPagesScatter: target %p span %llu state=%lx "
        "type=%lx allocBase=%p reservedByLayer=%d",
        base, (unsigned long long)span, (unsigned long)mbi.State,
        (unsigned long)mbi.Type, mbi.AllocationBase, (int)owned);

    uint64_t* out = (uint64_t*)pOutArray;
    uint32_t mapped = 0, failed = 0;
    for (uint32_t i = 0; i < dwPageCount; ++i) {
        void* va = (void*)(uintptr_t)pPages[i];
        void* p = VirtualAlloc(va, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        if (p) {
            ++mapped;
            if (out) out[i] = (uint64_t)(uintptr_t)p;
        } else {
            ++failed;
            if (out) out[i] = 0;
            if (failed <= 4) {
                Log("xmem",
                    "XMemMapPhysicalPagesScatter: commit [%u] %p failed "
                    "err=%lu",
                    i, va, GetLastError());
            }
        }
    }
    Log("xmem",
        "XMemMapPhysicalPagesScatter: mapped %u/%u pages, out[0]=0x%llx "
        "out[last]=0x%llx",
        mapped, dwPageCount,
        (unsigned long long)(out ? out[0] : 0),
        (unsigned long long)(out ? out[dwPageCount - 1] : 0));

    if (failed != 0) {
        Log("xmem",
            "XMemMapPhysicalPagesScatter: %u page(s) could not be committed "
            "-> FALSE (no partial success reported)",
            failed);
        return FALSE;
    }
    if (g_scatterMapCount < 8) {
        g_scatterMaps[g_scatterMapCount++] =
            ScatterReservation{base, span, dwPageCount, owned};
    }
    return TRUE;
}

// XMemTransferMemory — memcpy. First three args are (dst, src, size);
// any further args (flags, physical addresses) are ignored — on x64
// the caller cleans the stack, so extra args are harmless.
extern "C" void HleXMemTransferMemory(void* pDst, const void* pSrc,
                                      SIZE_T cbSize) {
    if (pDst && pSrc && cbSize) memcpy(pDst, pSrc, cbSize);
    Log("xmem", "XMemTransferMemory(%p <- %p, %llu bytes)", pDst, pSrc,
        (unsigned long long)cbSize);
}

// ---- kernelx!VirtualAlloc (VirtualAlloc flag-translation fix, 2026-11-23) ----
// The ERA guest's region-descriptor builder calls kernelx!VirtualAlloc
// with Xbox-specific allocation-type bits (0x30002000 / 0x40002000).
// The loader used to forward kernelx!VirtualAlloc verbatim to the real
// kernel32.dll, and desktop VirtualAlloc rejects those high bits with
// ERROR_INVALID_PARAMETER (NULL, err 87 — confirmed on the PC), so the
// guest's region base/size came back 0 and its descriptor-fill loop ran
// away off a minimal HeapAlloc(0) block until it faulted.
// This wrapper strips every bit desktop VirtualAlloc does not accept,
// keeping the desktop-valid allocation-type set, and retries with a
// system-chosen address when the requested address fails (the guest
// uses the returned address, so any address works).
//
// Placeholder support (boot #43): the title reserves a ~185 MiB
// placeholder (XMem type 0x42000 = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)
// and carves five sub-ranges out of it with MEM_REPLACE_PLACEHOLDER
// (type 0x20406000), splitting with VirtualFree(MEM_RELEASE |
// MEM_PRESERVE_PLACEHOLDER) (0x8002). A plain MEM_RESERVE is NOT a
// placeholder, so the real VirtualFree split fails with err 87 and
// 0x141e86440 returns false, which makes 0x142478440 return false
// before the setter — the global at 0x1475DF128 stays NULL and the next
// function crashes on [NULL+0x18]. (Leonida's retail chain stops 10–11
// describe the same two calls; this is a clean-room reimplementation
// of the documented placeholder semantics, not copied code.)
// Placeholder types are routed through VirtualAlloc2 (resolved via
// GetProcAddress so no SDK _WIN32_WINNT bump is needed).
using VirtualAlloc2Fn = LPVOID(WINAPI*)(HANDLE, LPVOID, SIZE_T, ULONG,
                                        ULONG, void*, ULONG);
static VirtualAlloc2Fn GetVirtualAlloc2() {
    static VirtualAlloc2Fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // VirtualAlloc2 lives in api-ms-win-core-memory-l1-1-2.dll.
        // On this PC's Windows 11 (build 26300) kernel32.dll does NOT
        // export it (GetProcAddress returns NULL — proven by a
        // PowerShell probe 2026-11-23), so try the API-set DLL too.
        // Boot #44 proved the kernel32-only lookup silently fell back
        // to a plain MEM_RESERVE, which is not a placeholder.
        static const char* kMods[] = {
            "kernel32.dll",
            "api-ms-win-core-memory-l1-1-2.dll",
        };
        for (const char* mod : kMods) {
            HMODULE k = GetModuleHandleA(mod);
            if (!k) k = LoadLibraryA(mod);
            if (k) {
                fn = (VirtualAlloc2Fn)GetProcAddress(k, "VirtualAlloc2");
                if (fn) break;
            }
        }
    }
    return fn;
}

// ---- Heap-creation measurement (ChatGPT plan, boot #54) ----
// Pure logging, no behaviour change. Tracks the large placeholder
// reservation (the game's 185 MB descriptor backing store) so the
// Xbox-facing CreateDescriptorHeap handler can report, per heap,
// whether the reservation existed before that call and how many
// layer-visible allocations happened between the reservation and the
// heap creation. The game's VirtualFree placeholder splits bypass the
// layer (real VirtualFree via IAT, not shimmed), so splits are NOT
// counted here — the reservation base/size are, which is what the
// heap-handle hypothesis turns on.
static LPVOID g_measReserveBase = nullptr;   // base of the >=100 MB placeholder reserve
static SIZE_T g_measReserveSize = 0;         // its size
static unsigned long long g_measAllocSeq = 0;    // every HleVirtualAlloc call
static unsigned long long g_measReserveSeq = 0;  // g_measAllocSeq at reservation time
static unsigned long long g_measReplaceInRange = 0;  // placeholder replaces inside the reservation

extern "C" LPVOID HleVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
                                  DWORD flAllocationType, DWORD flProtect) {
    // Desktop-valid flAllocationType bits. The Xbox bits 0x10000000 and
    // 0x40000000 are dropped here (0x30002000 -> MEM_RESERVE |
    // MEM_LARGE_PAGES; 0x40002000 -> MEM_RESERVE).
    constexpr DWORD kDesktopAllocType =
        MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_RESET_UNDO |
        MEM_LARGE_PAGES | MEM_PHYSICAL | MEM_TOP_DOWN | MEM_WRITE_WATCH;
    constexpr DWORD kReservePlaceholder = 0x40000;  // MEM_RESERVE_PLACEHOLDER
    constexpr DWORD kReplacePlaceholder = 0x4000;   // MEM_REPLACE_PLACEHOLDER
    DWORD desktopType = flAllocationType & kDesktopAllocType;
    ++g_measAllocSeq;
    if ((flAllocationType &
         (kReservePlaceholder | kReplacePlaceholder)) != 0) {
        VirtualAlloc2Fn va2 = GetVirtualAlloc2();
        if (va2) {
            if ((flAllocationType & kReservePlaceholder) != 0) {
                // Placeholder reserve: protection MUST be PAGE_NOACCESS.
                LPVOID p = va2(GetCurrentProcess(), lpAddress, dwSize,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                               PAGE_NOACCESS, nullptr, 0);
                Log("kernelx",
                    "VirtualAlloc2 placeholder reserve(%p, %llu, "
                    "0x%08x) -> %p (err %lu)",
                    lpAddress, (unsigned long long)dwSize,
                    flAllocationType, p, p ? 0 : GetLastError());
                // Measurement: record the large (>=100 MB) reservation
                // — the game's descriptor backing store.
                if (p && dwSize >= 100 * 1024 * 1024) {
                    g_measReserveBase = p;
                    g_measReserveSize = dwSize;
                    g_measReserveSeq = g_measAllocSeq;
                    g_measReplaceInRange = 0;
                    Log("kernelx",
                        "meas: reservation base=%p size=%llu "
                        "seq=%llu (thread %lu)",
                        p, (unsigned long long)dwSize,
                        g_measReserveSeq, GetCurrentThreadId());
                }
                return p;
            }
            // Replace a placeholder range. The game passes an Xbox
            // protect encoding (0x27), which VirtualAlloc2 rejects,
            // so use PAGE_READWRITE. MEM_PHYSICAL / MEM_LARGE_PAGES
            // are a backing hint on Xbox, not a correctness
            // requirement on desktop: retry without them if the
            // combined type is refused.
            DWORD extra = desktopType & (MEM_PHYSICAL | MEM_LARGE_PAGES);
            const DWORD tries[3] = {
                MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | extra,
                MEM_RESERVE | MEM_REPLACE_PLACEHOLDER |
                    (extra & MEM_PHYSICAL),
                MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
            };
            for (int i = 0; i < 3; ++i) {
                LPVOID p = va2(GetCurrentProcess(), lpAddress, dwSize,
                               tries[i], PAGE_READWRITE, nullptr, 0);
                if (p) {
                    // Measurement: count replaces inside the reservation.
                    if (g_measReserveBase &&
                        (uintptr_t)lpAddress >=
                            (uintptr_t)g_measReserveBase &&
                        (uintptr_t)lpAddress <
                            (uintptr_t)g_measReserveBase + g_measReserveSize)
                        ++g_measReplaceInRange;
                    Log("kernelx",
                        "VirtualAlloc2 placeholder replace(%p, %llu, "
                        "0x%08x -> 0x%08x) -> %p",
                        lpAddress, (unsigned long long)dwSize,
                        flAllocationType, tries[i], p);
                    return p;
                }
            }
            Log("kernelx",
                "VirtualAlloc2 placeholder replace(%p, %llu, 0x%08x) "
                "-> NULL (err %lu)",
                lpAddress, (unsigned long long)dwSize, flAllocationType,
                GetLastError());
            return nullptr;
        }
        // No VirtualAlloc2 (pre-1709): fall through to the plain path.
    }
    LPVOID p = VirtualAlloc(lpAddress, dwSize, desktopType, flProtect);
    if (!p && lpAddress) {
        // Requested address unavailable — let the system choose.
        p = VirtualAlloc(nullptr, dwSize, desktopType, flProtect);
    }
    if (!p && (desktopType & MEM_LARGE_PAGES)) {
        // MEM_LARGE_PAGES needs SeLockMemoryPrivilege, which a normal
        // title process does not hold, and a large-page-multiple size.
        // On Xbox it is a page-size hint for the reservation, not a
        // correctness requirement: the guest only needs the address
        // range reserved, so retry without it (requested address first,
        // then system-chosen).
        DWORD noLarge = desktopType & ~MEM_LARGE_PAGES;
        p = VirtualAlloc(lpAddress, dwSize, noLarge, flProtect);
        if (!p && lpAddress)
            p = VirtualAlloc(nullptr, dwSize, noLarge, flProtect);
        if (p) desktopType = noLarge;
    }
    Log("kernelx",
        "VirtualAlloc(%p, %llu, 0x%08x -> 0x%08x, 0x%08x) -> %p (err %lu)",
        lpAddress, (unsigned long long)dwSize, flAllocationType,
        desktopType, flProtect, p, p ? 0 : GetLastError());
    return p;
}

// ---- Xbox D3D12 device COM wrapper (graphics milestone #1) ----
// The game calls Xbox D3D12 device methods via the Xbox vtable layout
// (192 slots). Returning the raw desktop ID3D12Device* lets the game
// index the DESKTOP vtable at Xbox offsets, which AVs in D3D12Core
// (boot #28: D3D12Core.dll+0x2430e, right after D3D12XboxCreateDevice
// returned a real device). The wrapper presents an Xbox-layout vtable
// and forwards each method to the real desktop device. NEVER return the
// desktop ID3D12Device* to the title — the layer owns this guest-facing
// Xbox interface, with the real device behind it.
//   Slot map (retail chain, RETAIL-BOOT-CHAIN-PLAN.md):
//     0/1/2  IUnknown (QueryInterface/AddRef/Release)
//     8      CreateCommandQueue (same as PC)
//     9      CreateCommandAllocator (same as PC)
//     14     CreateDescriptorHeap (same as PC; riid may be Xbox, retry
//            with the desktop IID on E_NOINTERFACE; raw heap returned)
//     15     GetDescriptorHandleIncrementSize (same as PC)
//     26     GetCustomHeapProperties — struct-return, args SWAPPED:
//            (return buffer, this, node_mask, heap_type); the `this`
//            register does NOT point at the proxy, the second arg does.
//     48     CreateCommandQueue (desc = PC desc + Xbox words, riid =
//            IID_ID3D12CommandQueue, out)
//     73     CreateCommandList (16-byte desc, first word = type,
//            allocator, pso, riid = console list IID, out)
//     74     RegisterPagePoolX (va, pages, out-index) — page-pool
//            registration lifecycle, no dedupe (boot #60, GPT #10)
//     75     UnregisterPagePoolX (index) — invalidates it (boot #60)
//     133    CreatePlacedResourceX (va, desc, state, clear, riid, out)
//   The table is 192 slots (longer than the title's highest slot — a
// shorter table leaves a null pointer, not E_NOTIMPL). Slots not yet
// implemented point at XboxDevice_Unimplemented, which logs the
// caller's return address (so the game call site — and therefore the
// slot — can be identified from the disassembly) and returns
// E_NOTIMPL.
struct XboxDeviceWrapper {
    void** vtable;       // points at g_xboxDeviceVtable
    ID3D12Device* real;  // real desktop device (owned by the layer)
    LONG refCount;
    // Frame-pacing state from SetFrameIntervalX (slot 78): the vblank
    // period in microseconds (16667 for 60 Hz) and the allowed frames
    // in flight. The presenter uses these once a swapchain exists.
    UINT32 frameIntervalUs;
    UINT32 frameLatency;
};

static void* g_xboxDeviceVtable[192];
static bool g_xboxDeviceVtableInit = false;

// Forward declaration: the IDXGIDevice wrapper (defined below, after the
// command-list wrapper) is used by XboxDevice_QueryInterface.
struct DxgiDeviceWrapper;
static DxgiDeviceWrapper* WrapDxgiDevice(ID3D12Device* dev);

// Forward declaration: the descriptor-heap wrapper (defined below, after
// the command-list wrapper) is used by XboxDevice_CreateDescriptorHeap.
struct XboxDescriptorHeapWrapper;
static XboxDescriptorHeapWrapper* WrapXboxDescriptorHeap(
    ID3D12DescriptorHeap* real);
static ID3D12DescriptorHeap* UnwrapXboxDescriptorHeap(void* p);

// Forward declaration: the generic stub COM object (defined below, in
// the "Generic stub COM object" section) is used by the XAudio2 /
// GameInput / DStorage factory stubs.
static void* MakeStubComObject(const char* tag);

// The Xbox device IID the game requests
// ({5405C344-D457-444E-B4DD-2366E45AEE39}).
static const GUID kXboxDeviceIID = {
    0x5405C344, 0xD457, 0x444E,
    {0xB4, 0xDD, 0x23, 0x66, 0xE4, 0x5A, 0xEE, 0x39}};

static HRESULT WINAPI XboxDevice_QueryInterface(XboxDeviceWrapper* self,
                                                REFIID riid,
                                                void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (!self) return E_POINTER;
    if (memcmp(&riid, &kXboxDeviceIID, sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(ID3D12Device), sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(IUnknown), sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refCount);
        *ppvObject = self;
        return S_OK;
    }
    // IID_IDXGIDevice: the real desktop D3D12 device does NOT expose
    // IDXGIDevice through QueryInterface (boot #37: forwarding returns
    // E_NOINTERFACE), so the layer provides its own IDXGIDevice wrapper
    // (defined below) whose GetAdapter returns the real DXGI adapter.
    // The wrapper itself is NOT returned as the Xbox device — only as
    // the IDXGIDevice the game asked for.
    static const GUID kIID_IDXGIDevice = {
        0x54EC77FA, 0x1377, 0x44E6,
        {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
    if (self->real && memcmp(&riid, &kIID_IDXGIDevice, sizeof(GUID)) == 0) {
        DxgiDeviceWrapper* w = WrapDxgiDevice(self->real);
        if (w) {
            *ppvObject = w;
            Log("xboxdev", "QueryInterface: IID_IDXGIDevice -> wrapper %p",
                (void*)w);
            return S_OK;
        }
        Log("xboxdev", "QueryInterface: IID_IDXGIDevice wrapper failed");
        return E_NOINTERFACE;
    }
    Log("xboxdev",
        "QueryInterface: unknown IID {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, E_NOINTERFACE",
        (unsigned)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
        riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
        riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    return E_NOINTERFACE;
}

// IID_IDXGIDevice ({54EC77FA-1377-44E6-8C32-88FD5F44C84C}) — the DXGI
// device interface. The game QIs the D3D12 device for it (boot #36) to
// reach the DXGI adapter (for swapchain creation / adapter info). The
// real desktop D3D12 device does NOT expose IDXGIDevice through
// QueryInterface (boot #37: forwarding returns E_NOINTERFACE), so the
// layer provides its own wrapper (WrapDxgiDevice, below) whose GetAdapter
// returns the real DXGI adapter. The wrapper is returned only as the
// IDXGIDevice the game asked for — never as the Xbox device itself.

static ULONG WINAPI XboxDevice_AddRef(XboxDeviceWrapper* self) {
    if (!self) return 0;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG WINAPI XboxDevice_Release(XboxDeviceWrapper* self) {
    if (!self) return 0;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) {
        if (self->real) self->real->Release();
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)rc;
}

// Build 82: per-slot stub for Xbox device slots not yet implemented.
// Before this, every unimplemented slot shared one function that could
// only log the caller, so identifying the method meant disassembling the
// call site by hand — and an indirect call through a saved vtable pointer
// left even that ambiguous. Each slot now has its own instantiation, so
// the log names the slot and its vtable byte offset outright, together
// with the register arguments. That is what makes "which missing slot is
// supposed to produce this object" answerable from a boot log.
//
// NOTE: still only correct for HRESULT-returning methods; a pointer or
// struct-returning method that lands here gets E_NOTIMPL as its "pointer".
// The log line is what makes the slot identifiable, then implementable.
template <int Slot>
static HRESULT WINAPI XboxDevice_SlotUnimplemented(
    void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
    uint64_t a5, uint64_t a6) {
    (void)self;
    static LONG calls = 0;
    LONG n = InterlockedIncrement(&calls);
    // First few calls in full (that is where a missing output object shows
    // up), then a periodic reminder so a hot slot cannot drown the log.
    if (n <= 4 || (n % 64) == 0) {
        Log("xboxdev",
            "UNIMPLEMENTED slot %d (vtable +0x%x) from game %p args=%llx %llx "
            "%llx %llx %llx %llx (call #%ld) bk=%llx -> E_NOTIMPL",
            Slot, Slot * 8, _ReturnAddress(),
            (unsigned long long)a1, (unsigned long long)a2,
            (unsigned long long)a3, (unsigned long long)a4,
            (unsigned long long)a5, (unsigned long long)a6, (long)n,
            ScWatchRecordBackend());
    }
    return E_NOTIMPL;
}

// Slot 26: GetCustomHeapProperties. Xbox struct-return convention with
// SWAPPED args: (return buffer, this, node_mask, heap_type). The
// desktop method returns the struct by value; copy it to the return
// buffer and return the buffer pointer.
static D3D12_HEAP_PROPERTIES* WINAPI XboxDevice_GetCustomHeapProperties(
    D3D12_HEAP_PROPERTIES* retBuf, XboxDeviceWrapper* self, UINT nodeMask,
    D3D12_HEAP_TYPE heapType) {
    if (!self || !self->real || !retBuf) return retBuf;
    D3D12_HEAP_PROPERTIES props =
        self->real->GetCustomHeapProperties(nodeMask, heapType);
    *retBuf = props;
    Log("xboxdev",
        "GetCustomHeapProperties this=%p real=%p nodeMask=%u heapType=%u -> type=%u cpuPage=%u memPool=%u",
        (void*)self, (void*)(self ? self->real : nullptr),
        nodeMask, (unsigned)heapType, (unsigned)props.Type,
        (unsigned)props.CPUPageProperty, (unsigned)props.MemoryPoolPreference);
    return retBuf;
}

// Slot 17: CreateConstantBufferView. Desktop form matches:
// (this, pDesc, DestDescriptor). The game calls it from the init tail
// (0x14247927a) with rdx=&CBV desc and r8=DestDescriptor; returns
// void, so the game does not check a result. Forwards to the real
// device. The Xbox CBV desc is the PC prefix (BufferLocation +
// SizeInBytes); pass it through.
static void WINAPI XboxDevice_CreateConstantBufferView(
    XboxDeviceWrapper* self,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!self || !self->real || !pDesc) return;
    self->real->CreateConstantBufferView(pDesc, DestDescriptor);
    Log("xboxdev",
        "CreateConstantBufferView loc=0x%llx size=%u dest=0x%llx "
        "ret=0x%llx thread=%lu",
        (unsigned long long)pDesc->BufferLocation,
        (unsigned)pDesc->SizeInBytes,
        (unsigned long long)DestDescriptor.ptr,
        (unsigned long long)_ReturnAddress(),
        GetCurrentThreadId());
}

// Slot 18: CreateShaderResourceView. Desktop form matches:
// (this, pResource, pDesc, DestDescriptor). The game calls it from
// 0x140164efb (5x in boot #49); returns void. Forwards to the real
// device. The resource is passed through as-is (the game has not
// wrapped resources); the Xbox SRV desc is the PC prefix.
static void WINAPI XboxDevice_CreateShaderResourceView(
    XboxDeviceWrapper* self, ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!self || !self->real) return;
    self->real->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    // Boot #56 instrumentation: dump the desc (Format/ViewDimension +
    // raw union qwords) plus caller and thread — the boot #55 log
    // showed SRV created with a NULL resource, so the desc contents
    // are needed to see what the game thinks it is viewing.
    const UINT64* srvQ = (const UINT64*)pDesc;
    Log("xboxdev",
        "CreateShaderResourceView res=%p desc=%p fmt=%u dim=%u "
        "q=[0x%llx,0x%llx,0x%llx] dest=0x%llx ret=0x%llx thread=%lu",
        (void*)pResource, (const void*)pDesc,
        pDesc ? (unsigned)pDesc->Format : 0,
        pDesc ? (unsigned)pDesc->ViewDimension : 0,
        pDesc ? srvQ[0] : 0, pDesc ? srvQ[1] : 0, pDesc ? srvQ[2] : 0,
        (unsigned long long)DestDescriptor.ptr,
        (unsigned long long)_ReturnAddress(),
        GetCurrentThreadId());
}

// Slot 19: CreateUnorderedAccessView. Desktop form matches:
// (this, pResource, pCounterResource, pDesc, DestDescriptor). The game
// calls it from 0x141647610 (6x in boot #49); returns void. Forwards
// to the real device.
static void WINAPI XboxDevice_CreateUnorderedAccessView(
    XboxDeviceWrapper* self, ID3D12Resource* pResource,
    ID3D12Resource* pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!self || !self->real) return;
    self->real->CreateUnorderedAccessView(pResource, pCounterResource,
                                          pDesc, DestDescriptor);
    // Boot #56 instrumentation: same as SRV — desc contents plus
    // caller and thread (boot #55 showed UAV created with NULL res).
    const UINT64* uavQ = (const UINT64*)pDesc;
    Log("xboxdev",
        "CreateUnorderedAccessView res=%p counter=%p desc=%p "
        "fmt=%u dim=%u q=[0x%llx,0x%llx,0x%llx] dest=0x%llx "
        "ret=0x%llx thread=%lu",
        (void*)pResource, (void*)pCounterResource,
        (const void*)pDesc,
        pDesc ? (unsigned)pDesc->Format : 0,
        pDesc ? (unsigned)pDesc->ViewDimension : 0,
        pDesc ? uavQ[0] : 0, pDesc ? uavQ[1] : 0, pDesc ? uavQ[2] : 0,
        (unsigned long long)DestDescriptor.ptr,
        (unsigned long long)_ReturnAddress(),
        GetCurrentThreadId());
}

// Slot 25: GetResourceAllocationInfo. Xbox struct-return convention
// (as with slot 26 GetCustomHeapProperties): (return buffer, this,
// visibleMask, numDescs, pDescs). The game calls it from 0x140344182
// with rcx=&stack retBuf, rdx=device, r8d=1, r9d=1, [rsp+0x20]=
// pResourceDescs; it copies the 16-byte result to [rsi] and int3s
// at 0x140344196 if [rsi+4] (SizeInBytes high dword) != 0. The
// E_NOTIMPL stub never fills the struct, so the game int3s on stack
// garbage (boot #49). Forwards to the real device and copies the
// result into the game's return buffer.
// Build 81: status text for the one structured slot-25 record.
static const char* ScXgStatusText(uint32_t status) {
    switch (status) {
    case SC_XG_OK: return "SUCCESS";
    case SC_XG_BAD_DESC: return "bad-descriptor";
    case SC_XG_BAD_DIMS: return "invalid-dims";
    case SC_XG_UNSUPPORTED_LAYOUT: return "unsupported-mode";
    case SC_XG_UNSUPPORTED_FORMAT: return "unsupported-format";
    case SC_XG_OVERFLOW: return "overflow";
    case SC_XG_TOO_MANY_SUBRESOURCES: return "too-many-subresources";
    default: return "unknown";
    }
}

// One structured record per slot-25 call (build 81).
static void LogAllocInfo(const D3D12_RESOURCE_DESC* d, UINT sanitizedFlags,
                         const ScXgLayoutInfo* lay, const char* path,
                         const D3D12_RESOURCE_ALLOCATION_INFO* res) {
    char subs[192];
    subs[0] = 0;
    if (lay && lay->subCount) {
        int n = 0;
        UINT shown = lay->subCount < 8 ? lay->subCount : 8;
        for (UINT i = 0; i < shown; ++i) {
            int w = snprintf(subs + n, sizeof subs - (size_t)n, "%s%llu+%llu",
                             i ? "," : "",
                             (unsigned long long)lay->subOffset[i],
                             (unsigned long long)lay->subBytes[i]);
            if (w <= 0 || (size_t)(n + w) >= sizeof subs) break;
            n += w;
        }
        if (lay->subCount > shown && (size_t)n + 8 < sizeof subs)
            snprintf(subs + n, sizeof subs - (size_t)n, ",...(%u)",
                     lay->subCount - shown);
    }
    if (!d) {
        Log("xboxdev", "alloc-info: desc=NULL path=%s size=%llu align=%llu status=bad-descriptor",
            path, (unsigned long long)(res ? res->SizeInBytes : 0),
            (unsigned long long)(res ? res->Alignment : 0));
        return;
    }
    Log("xboxdev",
        "alloc-info: layout=%u dim=%u %llux%u depth=%u mips=%u fmt=%u "
        "flags=0x%x sanitized=0x%x bpe=%u tile=%ux%u tiles=%u subs=[%s] "
        "packed=%llu size=%llu align=%llu path=%s status=%s",
        (unsigned)d->Layout, (unsigned)d->Dimension,
        (unsigned long long)d->Width, (unsigned)d->Height,
        (unsigned)d->DepthOrArraySize, (unsigned)d->MipLevels,
        (unsigned)d->Format, (unsigned)d->Flags, sanitizedFlags,
        lay ? lay->bpe : 0, lay ? lay->tileW : 0, lay ? lay->tileH : 0,
        lay ? lay->tileCount : 0, subs,
        lay ? (unsigned long long)lay->packedBytes : 0ull,
        (unsigned long long)(res ? res->SizeInBytes : 0),
        (unsigned long long)(res ? res->Alignment : 0), path,
        lay ? ScXgStatusText(lay->status) : "not-computed");
}

static D3D12_RESOURCE_ALLOCATION_INFO* WINAPI
XboxDevice_GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO* retBuf, XboxDeviceWrapper* self,
    UINT visibleMask, UINT numResourceDescs,
    const D3D12_RESOURCE_DESC* pResourceDescs) {
    if (!retBuf || !self || !self->real) return retBuf;
    // Boot #51: the game's desc is a 3840x2160 R8G8B8A8 render target
    // (dim=3, fmt=28) but Flags=0x300005 carries Xbox-specific bits
    // (0x300000) the desktop method rejects — it returns
    // SizeInBytes=UINT64_MAX and the game int3s. Sanitize: copy the
    // desc and mask Flags to the desktop-valid bits (0x7F:
    // ALLOW_RENDER_TARGET | ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS
    // | DENY_SHADER_RESOURCE | ALLOW_CROSS_ADAPTER |
    // ALLOW_SIMULTANEOUS_ACCESS | VIDEO_DECODE_REFERENCE_ONLY).
    // The Dimension is left as-is (the desktop method accepts the
    // 3840x2160x1 shape once the flags are valid).
    D3D12_RESOURCE_DESC cleanDesc;
    const D3D12_RESOURCE_DESC* useDesc = pResourceDescs;
    UINT sanitizedFlags = pResourceDescs ? (UINT)pResourceDescs->Flags : 0;
    if (pResourceDescs) {
        cleanDesc = *pResourceDescs;
        cleanDesc.Flags = (D3D12_RESOURCE_FLAGS)(
            (UINT)cleanDesc.Flags & 0x7F);
        useDesc = &cleanDesc;
        sanitizedFlags = (UINT)cleanDesc.Flags;
    }

    // ---- build 81: console-layout dispatch ----------------------------
    // layout 0 (linear) keeps the existing forward to the desktop device
    // untouched. A console layout (0x100|mode) is sized by the SAME layout
    // engine the texture computer's copy uses (ScXg_ComputeLayout), so a
    // tiled allocation can never be sized by different arithmetic than the
    // one that writes it. Anything the engine cannot model fails cleanly:
    // UINT64_MAX + a logged reason, never the linear size standing in for a
    // tiled one.
    if (pResourceDescs && (UINT)pResourceDescs->Layout != 0) {
        ScXgLayoutInfo lay;
        HRESULT hr = E_INVALIDARG;
        if (numResourceDescs != 1) {
            memset(&lay, 0, sizeof lay);
            lay.status = SC_XG_BAD_DESC;  // one desc at a time; refuse cleanly
            Log("xboxdev",
                "alloc-info: console layout %u with numDescs=%u (only 1 supported) -> fail cleanly",
                (unsigned)pResourceDescs->Layout, numResourceDescs);
        } else {
            hr = ScXg_ComputeLayout(pResourceDescs, &lay);
        }
        if (numResourceDescs == 1 && SUCCEEDED(hr)) {
            retBuf->SizeInBytes = lay.totalBytes;
            retBuf->Alignment = lay.baseAlign;
            LogAllocInfo(pResourceDescs, sanitizedFlags, &lay, "tiled", retBuf);
            return retBuf;
        }
        retBuf->SizeInBytes = UINT64_MAX;
        retBuf->Alignment = 0;
        LogAllocInfo(pResourceDescs, sanitizedFlags, &lay, "tiled-rejected",
                     retBuf);
        Log("xboxdev",
            "alloc-info: layout %u cannot be sized by this layer (%s) -> UINT64_MAX (no linear substitute)",
            (unsigned)pResourceDescs->Layout, ScXgStatusText(lay.status));
        return retBuf;
    }

    *retBuf = self->real->GetResourceAllocationInfo(
        visibleMask, numResourceDescs, useDesc);
    LogAllocInfo(pResourceDescs, sanitizedFlags, nullptr, "linear-forward",
                 retBuf);
    return retBuf;
}

// Slot 127: CreateCommittedResource3 (ID3D12Device10). VERIFIED
// against the call site (0x14193e1b4, disassembled 2026-11-24, boot
// #57): rcx=this, rdx=&D3D12_HEAP_PROPERTIES{1,0,0,1,1} (20 bytes,
// built on the game stack from .rdata 0x14615c090), r8d=8 (heap
// flags), r9=&D3D12_RESOURCE_DESC1 (built on the game stack by
// 0x140abfad0), [rsp+0x20]=InitialLayout (D3D12_BARRIER_LAYOUT),
// [rsp+0x28]=&D3D12_CLEAR_VALUE (built on the game stack),
// [rsp+0x30]=NULL (protected session), [rsp+0x38]=0 (num castable),
// [rsp+0x40]=NULL (castable formats), [rsp+0x48]=&IID_ID3D12Resource
// (.rdata 0x1463fa690 — the STANDARD desktop IID, verified by
// reading the 16 bytes: {696442BE-A72E-4059-BC79-5B5C98040FAD}),
// [rsp+0x50]=&out. The argument layout matches desktop
// CreateCommittedResource3 exactly (this + 10 params).
//   CORRECTIONS to earlier notes: (a) the {96918C8C-ACA1-BFB8-B2A7-
//   9E95908C8C8C} GUID in BOOT52/BOOT56 is a DIFFERENT .rdata GUID
//   (VA 0x1461e7a90, used elsewhere); it is NOT the riid for this
//   call site. (b) GPT consultation #7's register map was shifted
//   by one — it forgot `this` occupies RCX; the map above is the
//   disassembly ground truth. (c) GPT's claim that Leonida
//   corroborates slot 127 = CreateCommittedResource3 is NOT
//   supported by our Leonida clone: their slot table names
//   27=CreateCommittedResource (PC slot 27), 76=Xbox resource
//   creation, 127=forwarded (unnamed). The identification rests on
//   this ABI match, not on Leonida.
//   The old stub (boot #53) treated R9 as the out pointer and wrote
//   the stub object into the first 8 bytes of the game's resource
//   desc — that corruption is gone with the real signature.
//   Returns the RAW native ID3D12Resource* (slot-133 pattern):
//   the game passes the pointer straight to the device SRV/UAV/RTV
//   methods, which forward to the real device; Leonida's slot 76
//   does the same. A COM wrapper for the resource is future work
//   if the game ever calls resource methods directly.
//   NEVER return S_OK without a valid object: native/wrapping
//   failures propagate. ALLOW_DISPLAY is kept on the first attempt;
//   a failure retries WITHOUT it, LOGGED (not silent).
static D3D12_RESOURCE_STATES BarrierLayoutToResourceState(
    D3D12_BARRIER_LAYOUT layout) {
    switch (layout) {
        case D3D12_BARRIER_LAYOUT_COMMON:
            return D3D12_RESOURCE_STATE_COMMON;
        case D3D12_BARRIER_LAYOUT_GENERIC_READ:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
            return (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case D3D12_BARRIER_LAYOUT_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
            return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
            return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:
            return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ:
            return D3D12_RESOURCE_STATE_VIDEO_DECODE_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ:
            return D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
        case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ:
            return D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON:
            return D3D12_RESOURCE_STATE_COMMON;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
            return (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        default:  // UNDEFINED (0xFFFFFFFF) + unknown
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

// ---- Boot #58: Xbox pool-dispatcher invocation (Model A) ----
// (GPT consultation #8.) The game's arena callback is registered in
// handler registry A (list A, bucket 128) but nothing in the game
// ever invokes the dispatcher that walks it. On a real Xbox the
// d3d12_xs runtime consults the registered pool when a committed
// resource is created; in this layer slot 127 IS the runtime's entry
// point, so the layer invokes the dispatcher itself, once per
// texture, AFTER the desktop resource exists (Model A: pool
// accounting / compatibility emulation — the desktop resource stays
// desktop-backed; the dispatcher call reproduces the pool-handler
// state. It does NOT make the desktop resource use the arena
// memory. Model B — pool-backed translation — is NOT attempted
// here: a HeapAlloc address is not interchangeable with a D3D12
// heap.)
//
// DISPATCHER SELECTION (resolved by disassembly, boot #58 — NOT by
// proximity): the two dispatchers are instruction-identical EXCEPT
// their table base. Dispatcher 1 (0x144deb430, via .data slot
// 0x146844380 -> thunk 0x144deb420) walks table A (base
// 0x146843370); dispatcher 2 (0x144e38ed0, via .data slot
// 0x146844388 -> thunk 0x144e38ec0) walks table B (base
// 0x146843b80). The arena callback is registered in list A (bucket
// 128 of table A: head slot 0x146843770); dispatcher 2's bucket 128
// is list B (0x146843f80), a different registry. The default tails
// confirm the split: table A's default node (0x1461889b0) is
// XMemAllocDefault — the arena callback's inner allocator (the
// boot #57 runtime chain is [arena callback, XMemAllocDefault]);
// table B's default node (0x1461889d0) is XMemFreeDefault. So the
// two dispatchers are the alloc/free pair, and dispatcher 1 is the
// allocation dispatcher. The layer loads the pointer from the .data
// slot at runtime (never hardcodes the thunk, never treats the slot
// address as a function) and gates on it equalling
// guestBase+0x4DEB420.
//   LOCK SCOPE (from disassembly): BOTH dispatchers release the
//   registry spinlock (0x1471bf988, via 0x144deb500) BEFORE invoking
//   the chain head — the lock covers traversal/selection ONLY, not
//   the callback invocation. So the callback and its inner allocator
//   run with the lock released, and the layer must NOT acquire it.
//   RETURN CONVENTION (from disassembly): both dispatchers return
//   the callback's return value untouched (call *0x20(%rsp), then
//   epilogue + ret). The arena callback's bump path returns the old
//   cursor — so the dispatcher returns the old cursor (0 on the
//   first call, 33,423,360 on the second). Verification below judges
//   by STATE (G/cursor/Gsize transitions), not by the return value
//   alone.
//   REENTRANCY (GPT #8 safeguards): (A) per-thread TLS depth
//   counter around the dispatch; (B) XMemAllocDefault is minimal
//   (allocation only, no logging — see above); (C) NO logging while
//   inside the callback chain — fixed-size records are captured in
//   preallocated (stack) storage and emitted AFTER the dispatcher
//   returns; (D) no extra lock around the arena; (E) the VEH
//   handler stays observational; (F) slot 127 holds no layer lock
//   across the dispatch (it holds none at all).
//   GATING: the dispatch runs only while the arena callback is
//   actually registered AND the request fits the remaining capacity.
//   If either gate fails, the dispatch is SKIPPED and the resource
//   is returned normally (boot #57 behaviour) — a later resource
//   created after the arena is unregistered, or one that does not
//   fit, has no pool to consult, and skipping is the honest
//   outcome. If the dispatch RUNS but the state does not advance as
//   expected, slot 127 FAILS the creation cleanly (release the
//   native resource, clear the out pointer, E_FAIL) — never a
//   silent success into the drain check, never an automatic retry
//   (a first attempt may have partially consumed arena space).

// Guest RVAs (image base-relative; computed from the loader's
// recorded base, so a relocated image still works).
static const uint64_t kPoolDispatchSlotRva = 0x6844380;  // .data slot: dispatcher-1 thunk pointer
static const uint64_t kPoolDispatchThunkRva = 0x4DEB420;  // expected slot value (thunk -> dispatcher 1)
static const uint64_t kArenaCallbackRva = 0x56A2A60;       // arena bump callback (VA 0x1456a2a60)
static const uint64_t kArenaListHeadRva = 0x6843770;      // table A bucket 128 head (list A)
static const uint64_t kArenaBaseGlobalRva = 0x75E6FF0;    // G (arena base)
static const uint64_t kArenaSizeGlobalRva = 0x75E7A08;    // Gsize (last kind stored by callback)
static const uint64_t kPoolDispatchSize = 33423360ULL;    // raw allocation-info size (510 x 64 KiB)
// Boot #59 (GPT #9): kind 0x808000 — bucket 128 in bits 16-23
// (the dispatcher's bextr 0x810 extracts (kind>>16)&0xFF) AND the
// bump flag 0x8000 in bits 14-15 (the callback tests
// (kind&0xC000)==0x8000). Boot #58's 0x8000 walked bucket 0.
static const uint64_t kPoolDispatchKind = 0x808000ULL;
static const uint64_t kPoolDispatchBucket = (kPoolDispatchKind >> 16) & 0xFF;  // 128
static const uint64_t kPoolTableARva = 0x6843370;         // table A base (head = table A + bucket*8)
static const uint64_t kPoolDispatchLockRva = 0x6843788;   // dispatcher lock (shared; released before callback)
static const uint64_t kPoolArenaCapacity = 0x4000000ULL;  // 64 MiB (observed [rsp+0x78])

struct PoolArenaState {
    uint64_t g = 0;           // G global (arena base, 0 = not yet allocated)
    uint64_t gsize = 0;       // Gsize global (last kind stored by callback)
    uint64_t cursor = 0;      // arena struct [0] (current alloc pointer)
    uint64_t capacity = 0;    // arena struct [8]
    uint64_t arenaStruct = 0; // &arena struct (registration node [8])
    uint64_t nodeObj = 0;     // arena node's obj field (node [0x10]) — GPT #9 filter capture
    uint64_t listHeadAddr = 0;// address the walk started from (base + kArenaListHeadRva)
    bool registered = false;  // arena callback node found in list A
    bool ok = false;          // every read succeeded
};

// SEH-guarded single-qword read from the guest image. POD locals
// only, so __try is legal under /EHsc (same pattern as the
// pe_loader VEH probes, which compile under these flags).
static bool ReadGuestQword(uint8_t* base, uint64_t rva, uint64_t* out) {
    if (!base || !out) return false;
    __try {
        *out = *(uint64_t*)(base + rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH-guarded read of the arena state: G/Gsize globals + the list A
// walk to the arena callback node (its [8] field is the &arena
// struct; the struct's [0]/[8] are cursor/capacity). The list is
// mutated only under the game's spinlock and the registration is
// scoped to the arena function, so the walk is stable while slot 127
// runs — but never fault the game thread for a diagnostic read.
static bool ReadPoolArenaState(uint8_t* base, PoolArenaState* st) {
    if (!base || !st) return false;
    st->g = 0; st->gsize = 0; st->cursor = 0; st->capacity = 0;
    st->arenaStruct = 0; st->nodeObj = 0; st->registered = false; st->ok = false;
    st->listHeadAddr = (uint64_t)base + kArenaListHeadRva;
    __try {
        st->g = *(uint64_t*)(base + kArenaBaseGlobalRva);
        st->gsize = *(uint64_t*)(base + kArenaSizeGlobalRva);
        void* node = *(void**)(base + kArenaListHeadRva);
        for (int i = 0; i < 8 && node; ++i) {
            uint64_t cb = *(uint64_t*)((uint8_t*)node + 0x0);
            uint64_t nstruct = *(uint64_t*)((uint8_t*)node + 0x8);
            uint64_t nobj = *(uint64_t*)((uint8_t*)node + 0x10);
            void* next = *(void**)((uint8_t*)node + 0x18);
            if (cb == (uint64_t)(base + kArenaCallbackRva)) {
                st->registered = true;
                st->arenaStruct = nstruct;
                st->nodeObj = nobj;
                st->cursor = *(uint64_t*)(nstruct + 0x0);
                st->capacity = *(uint64_t*)(nstruct + 0x8);
                break;
            }
            node = next;
        }
        st->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st->ok = false;
    }
    return st->ok;
}

// GPT #9 obj-filter capture: the dispatcher's TLS helper returns
// the per-thread context at TLS-block + 0x410, and the dispatcher
// copies a node only if node.obj == -1 or == that context. WHICH
// block the helper reads is ambiguous after the /GA patch: its
// `mov r64, gs:[58h]` was rewritten to read TlsSlots[gaSlot] (the
// /GA view, view[0] = game block), but an unpatched access reads
// the real TLS array[0] (another module's block — the game block
// is array[10] there, __tls_index slot). Read all three
// candidates and log them next to the node obj — whichever matches
// the node obj is the one the dispatcher compares against. Each
// read is separately SEH-guarded; a failed read leaves 0. Call
// before the dispatch (pre) and again after it (post): a value
// that changes between the two means the helper lazily
// re-initialized the cached context between registration and
// dispatch. Must run on the game thread (slot 127 does).
static void ReadGameTlsContexts(uint64_t* outArray0, uint64_t* outArray10,
                                uint64_t* outGaView) {
    if (outArray0) *outArray0 = 0;
    if (outArray10) *outArray10 = 0;
    if (outGaView) *outGaView = 0;
    __try {
        uint64_t* tlsArray = (uint64_t*)__readgsqword(0x58);
        if (tlsArray) {
            if (outArray0 && tlsArray[0])
                *outArray0 = *(uint64_t*)((uint8_t*)(uintptr_t)tlsArray[0] + 0x410);
            if (outArray10 && tlsArray[10])
                *outArray10 = *(uint64_t*)((uint8_t*)(uintptr_t)tlsArray[10] + 0x410);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        void* block = ScGuestGaViewBlock();
        if (block && outGaView)
            *outGaView = *(uint64_t*)((uint8_t*)block + 0x410);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Per-thread reentrancy guard (GPT #8 safeguard A): a TLS depth
// counter around the dispatcher invocation. Win32 TLS is used rather
// than __declspec(thread) so the guard does not depend on the layer
// DLL's TLS initialization order.
static DWORD g_poolDispatchTls = TLS_OUT_OF_INDEXES;
static LONG g_poolDispatchTlsInit = 0;

static DWORD PoolDispatchTlsIndex() {
    if (g_poolDispatchTlsInit != 2) {
        if (InterlockedCompareExchange(&g_poolDispatchTlsInit, 1, 0) == 0) {
            g_poolDispatchTls = TlsAlloc();
            InterlockedExchange(&g_poolDispatchTlsInit, 2);
        } else {
            while (g_poolDispatchTlsInit != 2) YieldProcessor();
        }
    }
    return g_poolDispatchTls;
}

// Guest dispatcher ABI: (rcx=size, rdx=kind) -> rax (the callback's
// return value = old cursor for the arena bump path).
typedef uint64_t (*GuestPoolDispatcherFn)(uint64_t size, uint64_t kind);

// Per-slot-127-call snapshot, captured in preallocated (stack)
// storage and emitted AFTER the dispatcher returns — never logged
// inside the callback chain (GPT #8 safeguard C).
struct PoolDispatchSnap {
    int callIndex = -1;
    uint64_t dispatcherPtr = 0;
    uint64_t dispatcherRet = 0;
    LONG xmemBefore = 0;
    LONG xmemAfter = 0;
    SIZE_T xmemLastSize = 0;
    void* xmemLastBase = nullptr;
    PoolArenaState pre;
    PoolArenaState post;
    int tlsDepthBefore = 0;
    bool dispatched = false;  // the dispatcher was actually called
    bool verified = false;    // state transitions matched expectation
    // GPT #9 instrumentation (boot #59).
    uint64_t bucket = 0;            // computed BEXTR bucket (kind>>16)&0xFF
    uint64_t tableHeadAddr = 0;     // base + kArenaListHeadRva (== table A + 128*8)
    uint64_t tableAAddr = 0;        // base + kPoolTableARva (proves the head math)
    uint64_t nodeObj = 0;           // arena node obj (pre-read)
    uint64_t tlsCtxArray0Pre = 0;   // TLS-ctx candidates at dispatch entry
    uint64_t tlsCtxArray10Pre = 0;
    uint64_t tlsCtxGaViewPre = 0;
    uint64_t tlsCtxArray0Post = 0;  // ... again after dispatch (re-init check)
    uint64_t tlsCtxArray10Post = 0;
    uint64_t tlsCtxGaViewPost = 0;
    uint64_t expectedOldCursor = 0; // callback's expected return (old cursor)
    uint64_t lockBefore = 0;        // dispatcher lock qword before/after
    uint64_t lockAfter = 0;
    bool lockOkBefore = false;
    bool lockOkAfter = false;
    int failCase = 0;  // 1=ok 2=callback filtered out 3=inner alloc failed 4=chain did not run
};

static LONG g_poolDispatchCalls = 0;  // slot-127 dispatch attempts

// Invoke the game's pool dispatcher for one committed resource
// (Model A). Returns true iff the dispatch ran AND the arena state
// advanced exactly as the two-resource hypothesis requires. Returns
// false with snap->dispatched==false when the dispatch was SKIPPED
// (arena not registered / bad pointer / does not fit / nested / no
// TLS) — the caller then returns the resource normally. Returns
// false with snap->dispatched==true when the dispatch RAN but the
// state did not advance — the caller must then FAIL the creation
// cleanly. Never calls the arena callback directly; never fabricates
// allocations; never touches the 2 MB assert.
static bool DispatchPoolForResource(PoolDispatchSnap* snap) {
    uint8_t* base = (uint8_t*)ScGuestBase();
    if (!base) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: no guest base");
        return false;
    }
    DWORD tlsIdx = PoolDispatchTlsIndex();
    if (tlsIdx == TLS_OUT_OF_INDEXES) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: TLS unavailable");
        return false;
    }
    int depth = (int)(intptr_t)TlsGetValue(tlsIdx);
    snap->tlsDepthBefore = depth;
    if (depth != 0) {
        // Unexpected nested entry — record, do not recurse.
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: nested dispatch "
            "(tls depth=%d)",
            depth);
        return false;
    }
    // Pre-read (before the dispatch): G/Gsize/cursor + the
    // registration walk. If the baseline cannot be read, skip —
    // the state cannot be verified afterwards.
    if (!ReadPoolArenaState(base, &snap->pre) || !snap->pre.ok) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: arena state "
            "unreadable");
        return false;
    }
    if (!snap->pre.registered) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: arena callback "
            "not registered (no pool to consult for this resource)");
        return false;
    }
    // Fit gate: the request must fit the remaining capacity, or the
    // callback's own over-capacity int3 would fire. Remaining =
    // capacity - (cursor - G); G==0/cursor==0 before the first call.
    uint64_t consumed = (snap->pre.g == 0)
        ? 0
        : (snap->pre.cursor - snap->pre.g);
    uint64_t remaining = (snap->pre.capacity >= consumed)
        ? (snap->pre.capacity - consumed)
        : 0;
    if (remaining < kPoolDispatchSize) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: request does not "
            "fit (remaining=%llu need=%llu G=%llx cursor=%llx)",
            (unsigned long long)remaining,
            (unsigned long long)kPoolDispatchSize,
            (unsigned long long)snap->pre.g,
            (unsigned long long)snap->pre.cursor);
        return false;
    }
    // GPT #9 instrumentation, captured once the gates pass and the
    // dispatch is actually about to run: the computed bucket, the
    // table-head proof (head must equal table A + 128*8), and the
    // arena node obj the dispatcher will compare against its TLS
    // context.
    snap->bucket = kPoolDispatchBucket;
    snap->tableHeadAddr = (uint64_t)base + kArenaListHeadRva;
    snap->tableAAddr = (uint64_t)base + kPoolTableARva;
    snap->nodeObj = snap->pre.nodeObj;
    // Load the dispatcher pointer from the .data slot at runtime
    // (GPT #8: call through the runtime-selected function pointer).
    // Gate on it equalling the dispatcher-1 thunk — if the slot
    // holds anything else, the premise is broken; do not call.
    uint64_t disp = 0;
    if (!ReadGuestQword(base, kPoolDispatchSlotRva, &disp)) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: dispatcher slot "
            "unreadable");
        return false;
    }
    snap->dispatcherPtr = disp;
    uint64_t expected = (uint64_t)base + kPoolDispatchThunkRva;
    if (disp != expected) {
        Log("xboxdev",
            "Slot127 CCR3: pool dispatch SKIPPED: dispatcher slot "
            "ptr=%p expected=%p (not dispatcher 1)",
            (void*)disp, (void*)expected);
        return false;
    }
    // TLS-context candidates + dispatcher lock, as close to the
    // call as possible (pre values; the post values are read
    // immediately after the call returns).
    ReadGameTlsContexts(&snap->tlsCtxArray0Pre, &snap->tlsCtxArray10Pre,
                        &snap->tlsCtxGaViewPre);
    snap->lockOkBefore =
        ReadGuestQword(base, kPoolDispatchLockRva, &snap->lockBefore);
    // All gates pass. Set the reentrancy guard, invoke the
    // dispatcher synchronously on this (game) thread, reset the
    // guard in __finally so a fault inside the chain cannot leave
    // a stale depth behind. NO logging inside the chain — the
    // snapshot below is emitted after the call returns.
    snap->callIndex = (int)InterlockedIncrement(&g_poolDispatchCalls) - 1;
    snap->xmemBefore = g_xmemAllocDefaultCalls;
    snap->dispatched = true;
    GuestPoolDispatcherFn fn = (GuestPoolDispatcherFn)(uintptr_t)disp;
    TlsSetValue(tlsIdx, (void*)(intptr_t)1);
    uint64_t ret = 0;
    __try {
        ret = fn(kPoolDispatchSize, kPoolDispatchKind);
    } __finally {
        TlsSetValue(tlsIdx, (void*)(intptr_t)0);
    }
    snap->dispatcherRet = ret;
    snap->xmemAfter = g_xmemAllocDefaultCalls;
    snap->xmemLastSize = g_xmemAllocDefaultLastSize;
    snap->xmemLastBase = (void*)g_xmemAllocDefaultLastBase;
    // GPT #9: TLS-context candidates + dispatcher lock, immediately
    // after the call returns (compare with the pre values for a
    // lazy re-initialization; the lock must be free again).
    ReadGameTlsContexts(&snap->tlsCtxArray0Post, &snap->tlsCtxArray10Post,
                        &snap->tlsCtxGaViewPost);
    snap->lockOkAfter =
        ReadGuestQword(base, kPoolDispatchLockRva, &snap->lockAfter);
    // Post-read + verification (state, not return value alone).
    ReadPoolArenaState(base, &snap->post);
    uint64_t postConsumed = 0;
    uint64_t postRemaining = 0;
    if (snap->post.ok && snap->post.g != 0) {
        postConsumed = snap->post.cursor - snap->post.g;
        postRemaining = (snap->post.capacity >= postConsumed)
            ? (snap->post.capacity - postConsumed)
            : 0;
    }
    uint64_t expectConsumed =
        (uint64_t)(snap->callIndex + 1) * kPoolDispatchSize;
    bool baseOk = (snap->callIndex == 0)
        ? (snap->pre.g == 0)
        : (snap->post.g == snap->pre.g);
    snap->verified = snap->post.ok && snap->post.registered &&
        snap->post.g != 0 && baseOk &&
        snap->post.gsize == kPoolDispatchKind &&
        snap->post.capacity == kPoolArenaCapacity &&
        postConsumed == expectConsumed;
    // The callback returns the OLD cursor: the arena base on the
    // first call (pre.g == 0, so the base is only known post-call),
    // the pre-call cursor on later calls. The dispatcher propagates
    // that return untouched, so dispatcherRet must equal it.
    snap->expectedOldCursor =
        (snap->pre.g == 0) ? snap->post.g : snap->pre.cursor;
    // GPT #9 failure classification (only meaningful when
    // !verified): 1 = callback ran + allocation succeeded
    // (== verified); 2 = callback filtered out (the dispatcher
    // invoked the default node, so xmem got the raw request size);
    // 3 = callback ran but the inner allocation failed (xmem got
    // the capacity-sized request, yet the state never moved);
    // 4 = the dispatcher did not execute the expected handler
    // chain (no xmem call, or an unexpected pattern). On call 1
    // the bump path needs no inner alloc, so delta 0 there with
    // no state movement is also case 4.
    LONG xmemDelta = snap->xmemAfter - snap->xmemBefore;
    if (snap->verified) snap->failCase = 1;
    else if (xmemDelta == 0) snap->failCase = 4;
    else if (snap->xmemLastSize == kPoolDispatchSize) snap->failCase = 2;
    else if (snap->xmemLastSize == kPoolArenaCapacity) snap->failCase = 3;
    else snap->failCase = 4;
    // The callback ran iff the state moved exactly as its bump path
    // moves it (G/Gsize/cursor are written ONLY by the callback —
    // boot #55 provenance). Reported as an inference, not a hook
    // count: the layer does not patch the callback to count it.
    int callbackRan = snap->verified ? 1 : 0;
    Log("xboxdev",
        "slot127[%d]: native_create=S_OK dispatcher_selected=1 "
        "dispatcher_ptr=%p dispatcher_arg0=%llu "
        "dispatcher_arg1=0x%llx dispatcher_return=%llx "
        "callback_ran(inferred)=%d xmem_alloc_delta=%ld "
        "xmem_last_size=%llu xmem_last_base=%p G_before=%llx "
        "G_after=%llx Gsize_before=%llx Gsize_after=%llx "
        "cursor_before=%llx cursor_after=%llx capacity=%llx "
        "remaining=%llu tls_depth=%d verified=%d "
        "bucket=%llu table_head=%p tableA=%p head_ok=%d "
        "node_obj=%llx tlsCtx_pre=[arr0=%llx arr10=%llx "
        "gaView=%llx] tlsCtx_post=[arr0=%llx arr10=%llx "
        "gaView=%llx] tls_reinit=%d expected_old_cursor=%llx "
        "ret_match=%d lock_before=%llx lock_after=%llx "
        "lock_free_after=%d failCase=%d",
        snap->callIndex, (void*)snap->dispatcherPtr,
        (unsigned long long)kPoolDispatchSize,
        (unsigned long long)kPoolDispatchKind,
        (unsigned long long)snap->dispatcherRet, callbackRan,
        (long)xmemDelta,
        (unsigned long long)snap->xmemLastSize, snap->xmemLastBase,
        (unsigned long long)snap->pre.g,
        (unsigned long long)snap->post.g,
        (unsigned long long)snap->pre.gsize,
        (unsigned long long)snap->post.gsize,
        (unsigned long long)snap->pre.cursor,
        (unsigned long long)snap->post.cursor,
        (unsigned long long)snap->post.capacity,
        (unsigned long long)postRemaining, snap->tlsDepthBefore,
        snap->verified ? 1 : 0,
        (unsigned long long)snap->bucket,
        (void*)snap->tableHeadAddr, (void*)snap->tableAAddr,
        (snap->tableHeadAddr == snap->tableAAddr + 128 * 8) ? 1 : 0,
        (unsigned long long)snap->nodeObj,
        (unsigned long long)snap->tlsCtxArray0Pre,
        (unsigned long long)snap->tlsCtxArray10Pre,
        (unsigned long long)snap->tlsCtxGaViewPre,
        (unsigned long long)snap->tlsCtxArray0Post,
        (unsigned long long)snap->tlsCtxArray10Post,
        (unsigned long long)snap->tlsCtxGaViewPost,
        (snap->tlsCtxArray0Pre != snap->tlsCtxArray0Post ||
         snap->tlsCtxArray10Pre != snap->tlsCtxArray10Post ||
         snap->tlsCtxGaViewPre != snap->tlsCtxGaViewPost) ? 1 : 0,
        (unsigned long long)snap->expectedOldCursor,
        (snap->dispatcherRet == snap->expectedOldCursor) ? 1 : 0,
        (unsigned long long)snap->lockBefore,
        (unsigned long long)snap->lockAfter,
        (snap->lockOkAfter && snap->lockAfter == 0) ? 1 : 0,
        snap->failCase);
    return snap->verified;
}

static HRESULT WINAPI XboxDevice_CreateCommittedResource3(
    XboxDeviceWrapper* self,
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_BARRIER_LAYOUT initialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    UINT32 numCastableFormats,
    const DXGI_FORMAT* pCastableFormats,
    REFIID riid,
    void** ppvResource) {
    if (!ppvResource) return E_POINTER;
    *ppvResource = nullptr;
    if (!self || !self->real || !pHeapProperties || !pDesc)
        return E_POINTER;
    // Boot #57 instrumentation: dump ALL 10 arguments. The heap
    // props are the 20-byte struct (RDX); the real resource desc is
    // R9 (D3D12_RESOURCE_DESC1, 64 bytes: the sampler-feedback
    // mip-region is a 3-UINT struct at offset 52); the initial layout
    // is the first stack arg; the clear value is a stack struct
    const UINT* hp = (const UINT*)pHeapProperties;
    Log("xboxdev",
        "Slot127 CCR3: heapProps=[%u,%u,%u,%u,%u] heapFlags=0x%x "
        "layout=%u clear=%p prot=%p numCast=%u cast=%p riid="
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} "
        "ppv=%p ret=0x%llx thread=%lu",
        hp ? hp[0] : 0, hp ? hp[1] : 0, hp ? hp[2] : 0,
        hp ? hp[3] : 0, hp ? hp[4] : 0,
        (unsigned)heapFlags, (unsigned)initialLayout,
        (const void*)pOptimizedClearValue,
        (void*)pProtectedSession, (unsigned)numCastableFormats,
        (const void*)pCastableFormats,
        (unsigned)riid.Data1, (unsigned)riid.Data2,
        (unsigned)riid.Data3, riid.Data4[0], riid.Data4[1],
        riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5],
        riid.Data4[6], riid.Data4[7], (void*)ppvResource,
        (unsigned long long)_ReturnAddress(), GetCurrentThreadId());
    Log("xboxdev",
        "Slot127 CCR3 desc: dim=%u align=%llu w=%llu h=%u "
        "depth=%u mips=%u fmt=%u sample=%u/%u layout=%u "
        "flags=0x%x mipRegion=%ux%ux%u",
        (unsigned)pDesc->Dimension,
        (unsigned long long)pDesc->Alignment,
        (unsigned long long)pDesc->Width, (unsigned)pDesc->Height,
        (unsigned)pDesc->DepthOrArraySize,
        (unsigned)pDesc->MipLevels, (unsigned)pDesc->Format,
        (unsigned)pDesc->SampleDesc.Count,
        (unsigned)pDesc->SampleDesc.Quality,
        (unsigned)pDesc->Layout, (unsigned)pDesc->Flags,
        (unsigned)pDesc->SamplerFeedbackMipRegion.Width,
        (unsigned)pDesc->SamplerFeedbackMipRegion.Height,
        (unsigned)pDesc->SamplerFeedbackMipRegion.Depth);
    if (pOptimizedClearValue) {
        Log("xboxdev",
            "Slot127 CCR3 clear: fmt=%u color=[%f,%f,%f,%f] "
            "depth=%f stencil=%u",
            (unsigned)pOptimizedClearValue->Format,
            pOptimizedClearValue->Color[0],
            pOptimizedClearValue->Color[1],
            pOptimizedClearValue->Color[2],
            pOptimizedClearValue->Color[3],
            pOptimizedClearValue->DepthStencil.Depth,
            (unsigned)pOptimizedClearValue->DepthStencil.Stencil);
    }
    // Convert the game's desc: sanitize Flags to desktop-valid
    // bits (same 0x7F mask as slot 25 — the game sets Xbox-only
    // bits 0x300000) and map an out-of-range Xbox layout word
    // (0x101 tiled / 0x1 row-major, per Leonida) to the desktop
    // layout. SamplerFeedbackMipRegion passes through as-is.
    D3D12_RESOURCE_DESC1 useDesc = *pDesc;
    D3D12_RESOURCE_FLAGS origFlags = useDesc.Flags;
    useDesc.Flags = (D3D12_RESOURCE_FLAGS)((UINT)useDesc.Flags & 0x7F);
    if ((UINT)useDesc.Layout >
        (UINT)D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
        useDesc.Layout =
            (useDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    if (origFlags != useDesc.Flags ||
        pDesc->Layout != useDesc.Layout) {
        Log("xboxdev",
            "Slot127 CCR3: sanitized flags 0x%x -> 0x%x "
            "layout %u -> %u",
            (unsigned)origFlags, (unsigned)useDesc.Flags,
            (unsigned)pDesc->Layout, (unsigned)useDesc.Layout);
    }
    // A clear value with Format UNKNOWN is not a real clear value
    // (zeroed stack struct) — pass NULL instead of a Format-0
    // struct, which desktop validation rejects.
    const D3D12_CLEAR_VALUE* useClear = nullptr;
    if (pOptimizedClearValue &&
        pOptimizedClearValue->Format != DXGI_FORMAT_UNKNOWN) {
        useClear = pOptimizedClearValue;
    } else if (pOptimizedClearValue) {
        Log("xboxdev",
            "Slot127 CCR3: clear value Format UNKNOWN -> NULL");
    }
    // CreateCommittedResource3 needs ID3D12Device10 (Agility SDK
    // 1.7+ runtime). Query it from the real device; if the runtime
    // does not expose it, fall back to a properly translated
    // CreateCommittedResource (barrier layout -> initial state) —
    // NEVER a stub object.
    ID3D12Device10* dev10 = nullptr;
    HRESULT hrQI = self->real->QueryInterface(
        __uuidof(ID3D12Device10), (void**)&dev10);
    Log("xboxdev", "Slot127 CCR3: Device10 QI hr=0x%08x dev10=%p",
        (unsigned)hrQI, (void*)dev10);
    ID3D12Resource* native = nullptr;
    HRESULT hr = E_FAIL;
    if (dev10) {
        hr = dev10->CreateCommittedResource3(
            pHeapProperties, heapFlags, &useDesc, initialLayout,
            useClear, pProtectedSession, numCastableFormats,
            pCastableFormats, riid, (void**)&native);
        Log("xboxdev",
            "Slot127 CCR3: CreateCommittedResource3 flags=0x%x "
            "-> hr=0x%08x native=%p",
            (unsigned)heapFlags, (unsigned)hr, (void*)native);
        if (FAILED(hr) &&
            (heapFlags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)) {
            // ALLOW_DISPLAY has extra desktop requirements (2D,
            // alignment 0, single-sample). Retry WITHOUT it and
            // log both attempts — the retry is evidence, not a
            // silent strip.
            D3D12_HEAP_FLAGS noDisplay =
                (D3D12_HEAP_FLAGS)((UINT)heapFlags &
                    ~(UINT)D3D12_HEAP_FLAG_ALLOW_DISPLAY);
            hr = dev10->CreateCommittedResource3(
                pHeapProperties, noDisplay, &useDesc, initialLayout,
                useClear, pProtectedSession, numCastableFormats,
                pCastableFormats, riid, (void**)&native);
            Log("xboxdev",
                "Slot127 CCR3: retry flags=0x%x -> hr=0x%08x "
                "native=%p",
                (unsigned)noDisplay, (unsigned)hr, (void*)native);
        }
        if (hr == E_NOINTERFACE) {
            hr = dev10->CreateCommittedResource3(
                pHeapProperties, heapFlags, &useDesc, initialLayout,
                useClear, pProtectedSession, numCastableFormats,
                pCastableFormats, __uuidof(ID3D12Resource),
                (void**)&native);
            Log("xboxdev",
                "Slot127 CCR3: IID retry -> hr=0x%08x native=%p",
                (unsigned)hr, (void*)native);
        }
        dev10->Release();
    } else {
        // Fallback: desktop CreateCommittedResource. The first 56
        // bytes of the DESC1 ARE a D3D12_RESOURCE_DESC (the
        // mip-region struct's first word lands in the DESC's
        // trailing padding, which the call ignores).
        D3D12_RESOURCE_DESC desc0;
        memcpy(&desc0, &useDesc, sizeof(desc0));
        D3D12_RESOURCE_STATES state =
            BarrierLayoutToResourceState(initialLayout);
        Log("xboxdev",
            "Slot127 CCR3: no Device10, fallback "
            "CreateCommittedResource layout %u -> state 0x%x",
            (unsigned)initialLayout, (unsigned)state);
        hr = self->real->CreateCommittedResource(
            pHeapProperties, heapFlags, &desc0, state, useClear,
            riid, (void**)&native);
        Log("xboxdev",
            "Slot127 CCR3: fallback CreateCommittedResource "
            "flags=0x%x -> hr=0x%08x native=%p",
            (unsigned)heapFlags, (unsigned)hr, (void*)native);
        if (hr == E_NOINTERFACE) {
            hr = self->real->CreateCommittedResource(
                pHeapProperties, heapFlags, &desc0, state, useClear,
                __uuidof(ID3D12Resource), (void**)&native);
            Log("xboxdev",
                "Slot127 CCR3: fallback IID retry -> hr=0x%08x "
                "native=%p",
                (unsigned)hr, (void*)native);
        }
        if (FAILED(hr) &&
            (heapFlags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)) {
            D3D12_HEAP_FLAGS noDisplay =
                (D3D12_HEAP_FLAGS)((UINT)heapFlags &
                    ~(UINT)D3D12_HEAP_FLAG_ALLOW_DISPLAY);
            hr = self->real->CreateCommittedResource(
                pHeapProperties, noDisplay, &desc0, state, useClear,
                riid, (void**)&native);
            Log("xboxdev",
                "Slot127 CCR3: fallback retry flags=0x%x -> "
                "hr=0x%08x native=%p",
                (unsigned)noDisplay, (unsigned)hr, (void*)native);
        }
    }
    if (SUCCEEDED(hr) && native) {
        // Boot #58 (Model A): the desktop resource exists; now
        // consult the game's pool dispatcher so its own handler
        // chain runs and the arena is consumed. Three outcomes:
        // (a) dispatch skipped (arena not registered / does not fit
        // / bad pointer / nested) — return the resource normally
        // (boot #57 behaviour); the snapshot log says which gate
        // tripped. (b) dispatch ran and verified — publish the
        // resource. (c) dispatch ran but the state did not advance
        // — FAIL the creation cleanly (release the native resource,
        // clear the out pointer, E_FAIL): a "successful" resource
        // the game would then fail the drain check on is worse than
        // an explicit failure, and an automatic retry could
        // double-consume arena space.
        PoolDispatchSnap snap;
        bool ok = DispatchPoolForResource(&snap);
        if (snap.dispatched && !ok) {
            Log("xboxdev",
                "Slot127 CCR3: pool dispatch FAILED verification "
                "(failCase=%d: 1=ok 2=callback filtered out "
                "3=inner alloc failed 4=chain did not run; see "
                "slot127 snapshot) — failing creation cleanly",
                snap.failCase);
            native->Release();
            native = nullptr;
            *ppvResource = nullptr;
            hr = E_FAIL;
        } else {
            *ppvResource = native;
        }
    } else {
        // Absolute: never S_OK without a valid object. Propagate
        // the failure so the game (and the log) sees it.
        if (native) {
            native->Release();
            native = nullptr;
        }
        *ppvResource = nullptr;
        Log("xboxdev",
            "Slot127 CCR3: FAILED hr=0x%08x (no resource "
            "returned)",
            (unsigned)hr);
    }
    Log("xboxdev",
        "Slot127 CCR3 -> hr=0x%08x out=%p",
        (unsigned)hr, *ppvResource);
    return hr;
}

// Slot 8 / 48: CreateCommandQueue. The Xbox desc starts with the PC
// D3D12_COMMAND_QUEUE_DESC (16 bytes) + Xbox words; pass the PC prefix
// through. The riid is IID_ID3D12CommandQueue on the console path;
// if the desktop driver rejects the requested IID, retry with the
// desktop IID_ID3D12CommandQueue.
// Slot 36: CreateFence. Xbox form matches the desktop form:
// (this, InitialValue, Flags, riid, ppFence). Proven by the boot #30
// call site 0x141d0b2fa: rdx=0 (InitialValue), r8d=1 (SHARED flag),
// r9=IID, [rsp+0x20]=ppFence. The game tolerates a failed fence
// (it checks the out pointer and skips), but a real fence is cheap.
// Slot 9: CreateCommandAllocator. Xbox form matches the desktop form:
// (this, Type, riid, ppAllocator). Proven by the boot #32 call site
// 0x142478b2d: rdx=0 (Type DIRECT), r8=IID, r9=ppAllocator, and the
// game int3s on a failure HRESULT. Returns the raw desktop allocator
// for now (the game only passes it to CreateCommandList); wrap it if
// the game ever calls allocator methods through the Xbox vtable.
static HRESULT WINAPI XboxDevice_CreateCommandAllocator(
    XboxDeviceWrapper* self, D3D12_COMMAND_LIST_TYPE type,
    REFIID riid, void** ppCommandAllocator) {
    if (!self || !self->real) return E_POINTER;
    HRESULT hr = self->real->CreateCommandAllocator(type, riid,
                                                     ppCommandAllocator);
    if (hr == E_NOINTERFACE && ppCommandAllocator) {
        hr = self->real->CreateCommandAllocator(
            type, __uuidof(ID3D12CommandAllocator), ppCommandAllocator);
    }
    Log("xboxdev", "CreateCommandAllocator type=%u -> hr=0x%08x alloc=%p",
        (unsigned)type, (unsigned)hr,
        ppCommandAllocator ? *ppCommandAllocator : nullptr);
    return hr;
}

static HRESULT WINAPI XboxDevice_CreateFence(
    XboxDeviceWrapper* self, UINT64 initialValue,
    D3D12_FENCE_FLAGS flags, REFIID riid, void** ppFence) {
    if (!self || !self->real) return E_POINTER;
    HRESULT hr = self->real->CreateFence(initialValue, flags, riid, ppFence);
    if (hr == E_NOINTERFACE && ppFence) {
        hr = self->real->CreateFence(initialValue, flags,
                                     __uuidof(ID3D12Fence), ppFence);
    }
    Log("xboxdev", "CreateFence init=%llu flags=%u -> hr=0x%08x fence=%p",
        (unsigned long long)initialValue, (unsigned)flags, (unsigned)hr,
        ppFence ? *ppFence : nullptr);
    return hr;
}

static HRESULT WINAPI XboxDevice_CreateCommandQueue(
    XboxDeviceWrapper* self, const D3D12_COMMAND_QUEUE_DESC* pDesc,
    REFIID riid, void** ppCommandQueue) {
    if (!self || !self->real) return E_POINTER;
    // Sanitize the Xbox desc to desktop-valid values. Boot #31 proved
    // the third queue fails on flags alone: queue 2 (type=2/COPY,
    // flags=0x1) succeeded, queue 3 (type=2/COPY, flags=0x2) returned
    // E_INVALIDARG. The desktop only defines
    // D3D12_COMMAND_QUEUE_FLAG_NONE (0) and
    // D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT (0x1); 0x2 is an
    // Xbox-only flag bit with no desktop meaning, so the desktop
    // rejects it. Mask to the PC-valid bits and drop Xbox-only bits.
    // (The Xbox extra words x[4]..x[7] are 0x1/0x0/0x40000000/0x1 on
    // all three queues and are not passed to the desktop.)
    D3D12_COMMAND_QUEUE_DESC pcDesc = {};
    if (pDesc) {
        pcDesc = *pDesc;
        pcDesc.Flags = (D3D12_COMMAND_QUEUE_FLAGS)(
            (UINT)pcDesc.Flags &
            (UINT)D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT);
    } else {
        pcDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    if (pDesc) {
        // Log the full Xbox desc (PC prefix + extra Xbox words) — the
        // third queue in boot #30 returned E_INVALIDARG, so the desc
        // contents are the diagnostic.
        const UINT32* x = (const UINT32*)pDesc;
        Log("xboxdev",
            "CreateCommandQueue desc type=%u prio=%d flags=0x%x->0x%x nodeMask=0x%x x[4]=0x%x x[5]=0x%x x[6]=0x%x x[7]=0x%x",
            (unsigned)pDesc->Type, (int)pDesc->Priority,
            (unsigned)pDesc->Flags, (unsigned)pcDesc.Flags,
            (unsigned)pDesc->NodeMask,
            x[4], x[5], x[6], x[7]);
    }
    HRESULT hr = self->real->CreateCommandQueue(&pcDesc, riid, ppCommandQueue);
    if (hr == E_NOINTERFACE && ppCommandQueue) {
        hr = self->real->CreateCommandQueue(
            &pcDesc, __uuidof(ID3D12CommandQueue), ppCommandQueue);
    }
    Log("xboxdev", "CreateCommandQueue -> hr=0x%08x queue=%p",
        (unsigned)hr, ppCommandQueue ? *ppCommandQueue : nullptr);
    return hr;
}

// Forward declaration: the command-list wrapper (defined below, after
// WrapXboxDevice) is used by XboxDevice_CreateCommandList.
struct XboxCommandListWrapper;
static XboxCommandListWrapper* WrapXboxCommandList(
    ID3D12GraphicsCommandList* real);

// Slot 73: CreateCommandList. Xbox form: a 16-byte desc whose first
// DWORD is the D3D12_COMMAND_LIST_TYPE, then allocator, pso, riid
// (console list IID), out. Map to the desktop form with nodeMask 0.
// The riid is a console IID the desktop driver does not know; retry
// with IID_ID3D12GraphicsCommandList on E_NOINTERFACE.
static HRESULT WINAPI XboxDevice_CreateCommandList(
    XboxDeviceWrapper* self, const void* pDesc,
    ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState,
    REFIID riid, void** ppCommandList) {
    if (!self || !self->real) return E_POINTER;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (pDesc) type = (D3D12_COMMAND_LIST_TYPE)(*(const UINT32*)pDesc);
    ID3D12GraphicsCommandList* realList = nullptr;
    HRESULT hr = self->real->CreateCommandList(
        0, type, pAllocator, pInitialState, riid, (void**)&realList);
    if (hr == E_NOINTERFACE) {
        hr = self->real->CreateCommandList(
            0, type, pAllocator, pInitialState,
            __uuidof(ID3D12GraphicsCommandList), (void**)&realList);
    }
    if (SUCCEEDED(hr) && realList && ppCommandList) {
        // Return the Xbox-wrapped list, NOT the raw desktop list — the
        // game calls Xbox list slots (131/132/9) on it, and the raw
        // desktop vtable is too short for those (boot #33).
        XboxCommandListWrapper* w = WrapXboxCommandList(realList);
        if (w) {
            realList->Release();  // the wrapper AddRef'd it
            *ppCommandList = w;
        } else {
            // Wrapping failed (OOM) — hand the raw list to the game.
            *ppCommandList = realList;
        }
    } else if (ppCommandList) {
        *ppCommandList = realList;
    }
    Log("xboxdev", "CreateCommandList type=%u -> hr=0x%08x list=%p",
        (unsigned)type, (unsigned)hr,
        ppCommandList ? *ppCommandList : nullptr);
    return hr;
}

// Slot 133: CreatePlacedResourceX. Xbox form: (va, desc, state, clear,
// riid, out). The VA is an Xbox GPU address with no desktop meaning;
// stand-in per the retail chain: a committed DEFAULT-heap buffer from
// the desc (Flags limited to PC bits, state COMMON), not tied to the
// VA. A real implementation needs the page mapper.
static HRESULT WINAPI XboxDevice_CreatePlacedResourceX(
    XboxDeviceWrapper* self, UINT64 va, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* pClear,
    REFIID riid, void** ppvResource) {
    (void)va;
    if (!self || !self->real || !pDesc) return E_POINTER;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = *pDesc;
    desc.Flags = (D3D12_RESOURCE_FLAGS)(
        (UINT)desc.Flags &
        (UINT)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
               D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
               D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
               D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS));
    D3D12_RESOURCE_STATES state = (initialState == (D3D12_RESOURCE_STATES)0xFFFFFFFF)
        ? D3D12_RESOURCE_STATE_COMMON
        : initialState;
    HRESULT hr = self->real->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, pClear, riid,
        ppvResource);
    if (hr == E_NOINTERFACE && ppvResource) {
        hr = self->real->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, pClear,
            __uuidof(ID3D12Resource), ppvResource);
    }
    // Boot #56 instrumentation: dump the full desc (the boot #55 log
    // showed only va/hr/res — the desc fields are needed to correlate
    // this placed resource with the two GetResourceAllocationInfo
    // queries), plus the caller return address and thread.
    Log("xboxdev",
        "CreatePlacedResourceX va=0x%llx desc: dim=%u align=%llu "
        "w=%llu h=%u depth=%u mips=%u fmt=%u sample=%u/%u "
        "layout=%u flags=0x%x state=0x%x ret=0x%llx thread=%lu -> "
        "hr=0x%08x res=%p",
        (unsigned long long)va,
        (unsigned)pDesc->Dimension,
        (unsigned long long)pDesc->Alignment,
        (unsigned long long)pDesc->Width,
        (unsigned)pDesc->Height,
        (unsigned)pDesc->DepthOrArraySize,
        (unsigned)pDesc->MipLevels,
        (unsigned)pDesc->Format,
        (unsigned)pDesc->SampleDesc.Count,
        (unsigned)pDesc->SampleDesc.Quality,
        (unsigned)pDesc->Layout,
        (unsigned)pDesc->Flags,
        (unsigned)initialState,
        (unsigned long long)_ReturnAddress(),
        GetCurrentThreadId(),
        (unsigned)hr,
        ppvResource ? *ppvResource : nullptr);
    return hr;
}

// Slot 14: CreateDescriptorHeap. Desktop slot 14 is also
// CreateDescriptorHeap: (pDesc, riid, ppHeap). The game calls it from
// 0x141cf3280 to build the descriptor heaps behind the bump allocator
// (0x14156c220); without it the create returns E_NOTIMPL, the init
// returns early, the allocator struct's [0] is never set, and the game
// crashes on [NULL+0x30] (boot #45). The Xbox riid may differ from the
// desktop IID, so retry with __uuidof(ID3D12DescriptorHeap) on
// E_NOINTERFACE. Returns a WRAPPED heap (WrapXboxDescriptorHeap), NOT
// the raw desktop heap: the game calls slots 9/10 (CPU/GPU handle
// getters) with rcx=heap and rdx garbage, expecting the handle in rax,
// but the desktop D3D12Core getters write through rdx and fault on the
// garbage value (boots #46/#47); the wrapper's slots 9/10 return the
// handle as a UINT64 in rax and never touch rdx.
static HRESULT WINAPI XboxDevice_CreateDescriptorHeap(
    XboxDeviceWrapper* self, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
    REFIID riid, void** ppHeap) {
    if (!self || !self->real || !pDesc || !ppHeap) return E_POINTER;
    *ppHeap = nullptr;
    ID3D12DescriptorHeap* realHeap = nullptr;
    HRESULT hr = self->real->CreateDescriptorHeap(pDesc, riid,
                                                   (void**)&realHeap);
    if (hr == E_NOINTERFACE) {
        hr = self->real->CreateDescriptorHeap(
            pDesc, __uuidof(ID3D12DescriptorHeap), (void**)&realHeap);
    }
    if (SUCCEEDED(hr) && realHeap) {
        // Diagnostic: record the raw heap's vtable slots 9/10, so the
        // faulting D3D12Core function (boots #46/#47, +0x5c0c7) can be
        // matched against the desktop heap vtable.
        void** rawVt = *(void***)realHeap;
        Log("xboxdev",
            "CreateDescriptorHeap type=%u num=%u flags=0x%x -> hr=0x%08x "
            "rawHeap=%p rawVtSlot9=%p rawVtSlot10=%p",
            (unsigned)pDesc->Type, (unsigned)pDesc->NumDescriptors,
            (unsigned)pDesc->Flags, (unsigned)hr, (void*)realHeap,
            rawVt ? rawVt[9] : nullptr, rawVt ? rawVt[10] : nullptr);
        // Measurement (ChatGPT plan, boot #54): capture the full
        // heap-creation record at the Xbox-facing boundary. The
        // wrapper returns the REAL heap's handles, so guest == native
        // by construction — both are logged to make that explicit.
        // The GPU handle is 0 for non-shader-visible heaps.
        D3D12_CPU_DESCRIPTOR_HANDLE nativeCpu =
            realHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE nativeGpu =
            realHeap->GetGPUDescriptorHandleForHeapStart();
        XboxDescriptorHeapWrapper* w = WrapXboxDescriptorHeap(realHeap);
        realHeap->Release();
        if (!w) {
            Log("xboxdev", "CreateDescriptorHeap: heap wrap failed");
            return E_OUTOFMEMORY;
        }
        *ppHeap = w;
        Log("xboxdev", "CreateDescriptorHeap -> wrapped heap=%p",
            (void*)w);
        Log("xboxdev",
            "meas: heap type=%u num=%u flags=0x%x rawHeap=%p "
            "wrapped=%p nativeCpu=0x%llx nativeGpu=0x%llx "
            "guestCpu=0x%llx guestGpu=0x%llx thread=%lu ret=0x%llx "
            "reserveExists=%d reserveBase=%p reserveSize=%llu "
            "allocsSinceReserve=%llu replacesInRange=%llu",
            (unsigned)pDesc->Type, (unsigned)pDesc->NumDescriptors,
            (unsigned)pDesc->Flags, (void*)realHeap, (void*)w,
            (unsigned long long)nativeCpu.ptr,
            (unsigned long long)nativeGpu.ptr,
            (unsigned long long)nativeCpu.ptr,
            (unsigned long long)nativeGpu.ptr,
            GetCurrentThreadId(),
            (unsigned long long)_ReturnAddress(),
            g_measReserveBase ? 1 : 0, g_measReserveBase,
            (unsigned long long)g_measReserveSize,
            g_measReserveBase ? (g_measAllocSeq - g_measReserveSeq) : 0,
            g_measReplaceInRange);
        return hr;
    }
    Log("xboxdev",
        "CreateDescriptorHeap type=%u num=%u flags=0x%x -> hr=0x%08x "
        "heap=%p",
        (unsigned)pDesc->Type, (unsigned)pDesc->NumDescriptors,
        (unsigned)pDesc->Flags, (unsigned)hr, *ppHeap);
    return hr;
}

// Slot 15: GetDescriptorHandleIncrementSize. Desktop device slot 15 is
// also GetDescriptorHandleIncrementSize, and the game calls it right
// after the pacing slots (boot #34, 0x142478e42+) with rdx = 0..4 (each
// descriptor-heap type) and stores the returned eax. Returns a UINT
// (bytes), not an HRESULT. Forwards to the real device.
static UINT WINAPI XboxDevice_GetDescriptorHandleIncrementSize(
    XboxDeviceWrapper* self, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!self || !self->real) return 0;
    UINT n = self->real->GetDescriptorHandleIncrementSize(type);
    Log("xboxdev", "GetDescriptorHandleIncrementSize type=%u -> %u",
        (unsigned)type, n);
    return n;
}

// Slot 78: SetFrameIntervalX(output, interval_us, latency, flags).
// Xbox frame-pacing: the vblank period in microseconds (16667 for
// 60 Hz) and the allowed frames in flight. Proven by the boot #34 call
// site 0x14247abd8: rdx=0 (output), r8=interval, r9=latency (2 or 3),
// [rsp+0x20]=0 (flags). The game tolerates a failure here, but the
// correct answer is S_OK — a boot must not abort on pacing, and the
// values are stored for the presenter. (Slot identity from Leonida's
// d3d12_xs_device.cpp, which names 78/79/80 SetFrameIntervalX /
// ScheduleFrameEventX / WaitFrameEventX — a public-HLE observation,
// not Microsoft documentation.)
static HRESULT WINAPI XboxDevice_SetFrameIntervalX(
    XboxDeviceWrapper* self, UINT64 output, UINT64 intervalUs,
    UINT64 latency, UINT64 flags) {
    (void)output;
    (void)flags;
    if (!self) return E_POINTER;
    self->frameIntervalUs = (UINT32)intervalUs;
    self->frameLatency = (UINT32)(latency ? latency : 1);
    Log("xboxdev", "SetFrameIntervalX interval=%llu us latency=%llu -> S_OK",
        (unsigned long long)intervalUs, (unsigned long long)latency);
    return S_OK;
}

// Slot 79: ScheduleFrameEventX(event, offset_us, reserved, flags).
// Proven by the boot #34 call site 0x142478de4: rdx=2 (event), r8=0
// (offset), r9=0 (reserved), [rsp+0x20]=0 (flags). The game int3s on a
// failure here, so this must return S_OK. There is no vblank source
// behind the device before a presenter exists, so the event is just
// recorded as scheduled.
static HRESULT WINAPI XboxDevice_ScheduleFrameEventX(
    XboxDeviceWrapper* self, UINT64 event, UINT64 offsetUs, UINT64 reserved,
    UINT64 flags) {
    (void)event;
    (void)offsetUs;
    (void)reserved;
    (void)flags;
    if (!self) return E_POINTER;
    Log("xboxdev", "ScheduleFrameEventX event=%llu offset=%llu us -> S_OK",
        (unsigned long long)event, (unsigned long long)offsetUs);
    return S_OK;
}

// Slot 80: WaitFrameEventX(event, timeout_ms, flags, token).
// The natural next pacing call after ScheduleFrameEventX. Returns at
// once with a fresh non-zero token: there is no vblank source behind
// the device before a presenter, and a zero wait cannot stall the title.
// A null token pointer is E_POINTER.
static HRESULT WINAPI XboxDevice_WaitFrameEventX(
    XboxDeviceWrapper* self, UINT64 event, UINT64 timeoutMs, UINT64 flags,
    UINT64* token) {
    (void)event;
    (void)timeoutMs;
    (void)flags;
    if (!self) return E_POINTER;
    if (!token) return E_POINTER;
    static LONG s_frameToken = 1;
    LONG t = InterlockedIncrement(&s_frameToken);
    if (t == 0) t = InterlockedIncrement(&s_frameToken);
    *token = (UINT64)t;
    Log("xboxdev", "WaitFrameEventX -> S_OK token=%llu",
        (unsigned long long)*token);
    return S_OK;
}

// ---- Xbox device slots 74/75: page-pool registration (boot #60) ----
// GPT consultation #10 approved spec (chat c/6b052480). RegisterPagePoolX
// records a (va, pages) registration and returns a layer-assigned pool
// index; UnregisterPagePoolX invalidates it. This is a REGISTRATION
// lifecycle, not an unconditional S_OK stub: the index is a pool
// identifier that later page-mapping calls (CopyPageMappingsX /
// CopyPageMappingsBatchX) will consume, so the layer must translate it
// consistently in every later API that takes a pool index.
//   NO dedupe (GPT #10): a repeated (va, pages) pair may be a second
// registration with its own lifetime — each successful registration
// gets a NEW unique index; duplicate ranges are logged but kept as
// separate registrations. Dedupe only after observing the game's
// registration/unregistration behaviour or finding evidence the native
// API requires it.
//   Slot 75's ABI is XCL's reference table (device_x.rs, read-only):
// (this, index:u64) -> HRESULT. The game has not called it yet, so the
// handler logs the raw arg and validates it against the table — a
// wrong-guess ABI will show up in the log, not silently corrupt state.
//   The pool memory itself is already allocated by the game (VirtualAlloc
// + XMemVirtualAlloc + XMemMapPhysicalPages through the layer's shims) —
// the layer only records the registration.
struct ScPagePoolReg {
    void* va;       // registration base, as passed
    UINT32 pages;   // page count, as passed
    UINT64 bytes;   // registration extent in bytes (pages x unit)
    bool unit64K;   // true = 64 KiB pages, false = 4 KiB pages
    bool active;    // false after unregister
};
static const UINT32 kMaxPagePools = 1024;
static ScPagePoolReg g_pagePools[kMaxPagePools];
static UINT32 g_pagePoolIssued = 0;  // indices issued so far (stable, never reused)
static UINT32 g_pagePoolActive = 0;  // currently active registrations
static volatile LONG g_pagePoolCalls74 = 0;  // RegisterPagePoolX calls
static volatile LONG g_pagePoolCalls75 = 0;  // UnregisterPagePoolX calls
static SRWLOCK g_pagePoolLock = SRWLOCK_INIT;

static bool PagePoolRangesOverlap(void* va1, UINT64 bytes1, void* va2,
                                  UINT64 bytes2) {
    uintptr_t a1 = (uintptr_t)va1, e1 = a1 + (uintptr_t)bytes1;
    uintptr_t a2 = (uintptr_t)va2, e2 = a2 + (uintptr_t)bytes2;
    return a1 < e2 && a2 < e1;
}

// Returns true if [va, va+bytes) lies within a single VirtualAlloc
// allocation (same AllocationBase across every sub-region). Fills the
// first region's state/protect/type/size for the log. A range crossing
// into another allocation is not the reservation the game registered.
static bool PagePoolRangeInOneAlloc(void* va, UINT64 bytes,
                                    void** allocBase, DWORD* state,
                                    DWORD* protect, DWORD* type,
                                    UINT64* firstRegionBytes) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(va, &mbi, sizeof(mbi)) == 0) return false;
    *allocBase = mbi.AllocationBase;
    *state = mbi.State;
    *protect = mbi.Protect;
    *type = mbi.Type;
    *firstRegionBytes = (UINT64)mbi.RegionSize;
    uintptr_t vaEnd = (uintptr_t)va + (uintptr_t)bytes;
    if (vaEnd < (uintptr_t)va) return false;  // wraps
    uintptr_t cur = (uintptr_t)va;
    while (cur < vaEnd) {
        MEMORY_BASIC_INFORMATION m2 = {};
        if (VirtualQuery((void*)cur, &m2, sizeof(m2)) == 0 ||
            m2.AllocationBase != mbi.AllocationBase)
            return false;
        cur = (uintptr_t)m2.BaseAddress + (uintptr_t)m2.RegionSize;
    }
    return true;
}

static HRESULT WINAPI XboxDevice_RegisterPagePoolX(XboxDeviceWrapper* self,
                                                   void* va, UINT32 pages,
                                                   UINT32* outPoolIndex) {
    (void)self;
    LONG callNo = InterlockedIncrement(&g_pagePoolCalls74);
    Log("xboxdev",
        "Slot74 RegisterPagePoolX call#%ld va=%p pages=%u out=%p "
        "thread=%lu ret=0x%llx",
        callNo, va, (unsigned)pages, (void*)outPoolIndex,
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)_ReturnAddress());
    // Validation (GPT #10): fail cleanly, never silently round.
    if (!va || pages == 0) {
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld -> E_INVALIDARG "
            "(va=%p pages=%u)",
            callNo, va, (unsigned)pages);
        return E_INVALIDARG;
    }
    if (((uintptr_t)va & 0xFFFF) != 0) {
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld -> E_INVALIDARG "
            "(va %p not 64K-aligned)",
            callNo, va);
        return E_INVALIDARG;
    }
    // Page-unit resolution (boot #60, Freebuff forensics): the layer's
    // XMem pages are 4K (x4096) while page pools are 64K-granular, and
    // the game passes a bare count — the unit is NOT in the signature.
    // Resolve it from the memory evidence instead of assuming: try
    // 64K first (the GDK page-pool granularity); if that range does
    // not fit inside one allocation, try 4K. The resolved unit is
    // logged and stored in the pool record (bytes, not just the raw
    // count) — a wrong-unit assumption here would fail a legitimate
    // registration.
    MEMORY_BASIC_INFORMATION probe = {};
    if (VirtualQuery(va, &probe, sizeof(probe)) == 0) {
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld -> E_INVALIDARG "
            "(VirtualQuery(%p) failed err=%lu)",
            callNo, va, GetLastError());
        return E_INVALIDARG;
    }
    UINT64 bytes64K = (UINT64)pages * 65536ULL;
    UINT64 bytes4K = (UINT64)pages * 4096ULL;
    void* allocBase = nullptr;
    DWORD firstState = 0, firstProtect = 0, firstType = 0;
    UINT64 firstRegionBytes = 0;
    UINT64 bytes = 0;
    bool unit64K = true;
    if (PagePoolRangeInOneAlloc(va, bytes64K, &allocBase, &firstState,
                                &firstProtect, &firstType,
                                &firstRegionBytes)) {
        bytes = bytes64K;
        unit64K = true;
    } else if (PagePoolRangeInOneAlloc(va, bytes4K, &allocBase, &firstState,
                                       &firstProtect, &firstType,
                                       &firstRegionBytes)) {
        bytes = bytes4K;
        unit64K = false;
    } else {
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld -> E_INVALIDARG "
            "(va %p pages=%u fits neither 64K-unit (%llu bytes) nor "
            "4K-unit (%llu bytes) in one allocation)",
            callNo, va, (unsigned)pages,
            (unsigned long long)bytes64K,
            (unsigned long long)bytes4K);
        return E_INVALIDARG;
    }
    // Memory-state capture (GPT #10): the first region's
    // state/protect/type are logged as the measurement; the state
    // itself is NOT a pass/fail gate (a reserved range is not the same
    // as committed backing pages — record it, don't assume).
    Log("xboxdev",
        "Slot74 RegisterPagePoolX call#%ld mem: allocBase=%p "
        "firstRegionBytes=%llu state=0x%x protect=0x%x type=0x%x "
        "bytes=%llu unit=%s rangeInOneAlloc=1",
        callNo, allocBase, (unsigned long long)firstRegionBytes,
        (unsigned)firstState, (unsigned)firstProtect,
        (unsigned)firstType, (unsigned long long)bytes,
        unit64K ? "64K" : "4K");
    // Overlap measurements (GPT #10) — data only, never a failure by
    // themselves: (a) the >=100 MB placeholder reservation recorded by
    // HleVirtualAlloc (the game's 185 MB descriptor backing store);
    // (b) the last XMemAllocDefault block (the 64 MB arena backing in
    // boot #59). The three regions must be distinct — compare actual
    // base/end addresses, not just sizes.
    bool overlapDescReserve = false;
    if (g_measReserveBase && g_measReserveSize > 0) {
        overlapDescReserve = PagePoolRangesOverlap(
            va, bytes, g_measReserveBase, (UINT64)g_measReserveSize);
    }
    bool overlapArena = false;
    void* arenaBase = (void*)g_xmemAllocDefaultLastBase;
    SIZE_T arenaSize = (SIZE_T)g_xmemAllocDefaultLastSize;
    if (arenaBase && arenaSize > 0) {
        overlapArena = PagePoolRangesOverlap(va, bytes, arenaBase,
                                            (UINT64)arenaSize);
    }
    Log("xboxdev",
        "Slot74 RegisterPagePoolX call#%ld overlap: descReserve "
        "(base=%p size=%llu) overlap=%d; arenaLast "
        "(base=%p size=%llu) overlap=%d",
        callNo, g_measReserveBase,
        (unsigned long long)g_measReserveSize, overlapDescReserve ? 1 : 0,
        arenaBase, (unsigned long long)arenaSize,
        overlapArena ? 1 : 0);
    // Issue a NEW stable index (no dedupe). Duplicate ranges are
    // collected under the lock and logged after it.
    UINT32 dupIdx[4];
    UINT32 dupCount = 0;
    UINT32 dupTotal = 0;
    AcquireSRWLockExclusive(&g_pagePoolLock);
    for (UINT32 i = 0; i < g_pagePoolIssued; ++i) {
        if (!g_pagePools[i].active) continue;
        if (PagePoolRangesOverlap(va, bytes, g_pagePools[i].va,
                                  g_pagePools[i].bytes)) {
            if (dupCount < 4) dupIdx[dupCount] = i;
            ++dupCount;
            ++dupTotal;
        }
    }
    if (g_pagePoolIssued >= kMaxPagePools) {
        ReleaseSRWLockExclusive(&g_pagePoolLock);
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld -> E_FAIL (pool table "
            "full, issued=%u)",
            callNo, g_pagePoolIssued);
        return E_FAIL;
    }
    UINT32 idx = g_pagePoolIssued++;
    g_pagePools[idx].va = va;
    g_pagePools[idx].pages = pages;
    g_pagePools[idx].bytes = bytes;
    g_pagePools[idx].unit64K = unit64K;
    g_pagePools[idx].active = true;
    ++g_pagePoolActive;
    UINT32 active = g_pagePoolActive;
    ReleaseSRWLockExclusive(&g_pagePoolLock);
    if (dupTotal > 0) {
        Log("xboxdev",
            "Slot74 RegisterPagePoolX call#%ld dup: range overlaps %u "
            "existing registration(s) (first=%u%s) — kept as separate "
            "registration, no dedupe",
            callNo, dupTotal, dupCount > 0 ? dupIdx[0] : 0,
            dupTotal > 1 ? ",..." : "");
    }
    if (outPoolIndex) *outPoolIndex = idx;
    Log("xboxdev",
        "Slot74 RegisterPagePoolX call#%ld -> S_OK index=%u active=%u "
        "(out=%p)",
        callNo, idx, active, (void*)outPoolIndex);
    return S_OK;
}

static HRESULT WINAPI XboxDevice_UnregisterPagePoolX(
    XboxDeviceWrapper* self, UINT64 index) {
    (void)self;
    LONG callNo = InterlockedIncrement(&g_pagePoolCalls75);
    Log("xboxdev",
        "Slot75 UnregisterPagePoolX call#%ld index=%llu (as ptr %p) "
        "thread=%lu ret=0x%llx",
        callNo, (unsigned long long)index, (void*)(uintptr_t)index,
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)_ReturnAddress());
    if (index >= kMaxPagePools) {
        Log("xboxdev",
            "Slot75 UnregisterPagePoolX call#%ld -> E_FAIL "
            "(index %llu out of range, issued=%u)",
            callNo, (unsigned long long)index, g_pagePoolIssued);
        return E_FAIL;
    }
    AcquireSRWLockExclusive(&g_pagePoolLock);
    bool ok = false;
    void* va = nullptr;
    UINT32 pages = 0;
    UINT64 bytes = 0;
    if (index < g_pagePoolIssued && g_pagePools[(UINT32)index].active) {
        va = g_pagePools[(UINT32)index].va;
        pages = g_pagePools[(UINT32)index].pages;
        bytes = g_pagePools[(UINT32)index].bytes;
        g_pagePools[(UINT32)index].active = false;
        --g_pagePoolActive;
        ok = true;
    }
    UINT32 active = g_pagePoolActive;
    UINT32 issued = g_pagePoolIssued;
    ReleaseSRWLockExclusive(&g_pagePoolLock);
    if (!ok) {
        Log("xboxdev",
            "Slot75 UnregisterPagePoolX call#%ld -> E_FAIL "
            "(index %llu unknown or already unregistered, issued=%u "
            "active=%u)",
            callNo, (unsigned long long)index, issued, active);
        return E_FAIL;
    }
    Log("xboxdev",
        "Slot75 UnregisterPagePoolX call#%ld -> S_OK unregistered "
        "index=%llu va=%p pages=%u bytes=%llu active=%u",
        callNo, (unsigned long long)index, va, (unsigned)pages,
        (unsigned long long)bytes, active);
    return S_OK;
}

// Fill the vtable's unimplemented entries with per-slot logging stubs.
// One pack expansion instantiates all 192 (no recursion depth to worry
// about), and every entry is a real function — never null.
template <std::size_t... I>
static void FillUnimplementedDeviceSlots(std::index_sequence<I...>,
                                         void** vt) {
    void* stubs[] = {(void*)&XboxDevice_SlotUnimplemented<(int)I>...,
                     nullptr};
    for (std::size_t i = 0; i < sizeof...(I); ++i) vt[i] = stubs[i];
}

// Build the 192-slot Xbox device vtable once. Unimplemented slots get
// the logging stub; known slots get their handlers.
static void InitXboxDeviceVtable() {
    if (g_xboxDeviceVtableInit) return;
    FillUnimplementedDeviceSlots(std::make_index_sequence<192>{},
                                 g_xboxDeviceVtable);
    g_xboxDeviceVtable[0] = (void*)XboxDevice_QueryInterface;
    g_xboxDeviceVtable[1] = (void*)XboxDevice_AddRef;
    g_xboxDeviceVtable[2] = (void*)XboxDevice_Release;
    g_xboxDeviceVtable[8] = (void*)XboxDevice_CreateCommandQueue;
    g_xboxDeviceVtable[9] = (void*)XboxDevice_CreateCommandAllocator;
    g_xboxDeviceVtable[14] = (void*)XboxDevice_CreateDescriptorHeap;
    g_xboxDeviceVtable[15] = (void*)XboxDevice_GetDescriptorHandleIncrementSize;
    g_xboxDeviceVtable[17] = (void*)XboxDevice_CreateConstantBufferView;
    g_xboxDeviceVtable[18] = (void*)XboxDevice_CreateShaderResourceView;
    g_xboxDeviceVtable[19] = (void*)XboxDevice_CreateUnorderedAccessView;
    g_xboxDeviceVtable[25] = (void*)XboxDevice_GetResourceAllocationInfo;
    g_xboxDeviceVtable[26] = (void*)XboxDevice_GetCustomHeapProperties;
    g_xboxDeviceVtable[36] = (void*)XboxDevice_CreateFence;
    g_xboxDeviceVtable[48] = (void*)XboxDevice_CreateCommandQueue;
    g_xboxDeviceVtable[73] = (void*)XboxDevice_CreateCommandList;
    g_xboxDeviceVtable[74] = (void*)XboxDevice_RegisterPagePoolX;
    g_xboxDeviceVtable[75] = (void*)XboxDevice_UnregisterPagePoolX;
    g_xboxDeviceVtable[78] = (void*)XboxDevice_SetFrameIntervalX;
    g_xboxDeviceVtable[79] = (void*)XboxDevice_ScheduleFrameEventX;
    g_xboxDeviceVtable[80] = (void*)XboxDevice_WaitFrameEventX;
    g_xboxDeviceVtable[127] =
        (void*)XboxDevice_CreateCommittedResource3;
    g_xboxDeviceVtable[133] = (void*)XboxDevice_CreatePlacedResourceX;
    g_xboxDeviceVtableInit = true;
}

// Wrap a real desktop device in a guest-facing Xbox device interface.
// Returns the wrapper (the guest-facing pointer) or nullptr. The real
// device's refcount is NOT changed here — the caller owns it; the
// wrapper AddRefs it.
static XboxDeviceWrapper* WrapXboxDevice(ID3D12Device* real) {
    if (!real) return nullptr;
    InitXboxDeviceVtable();
    XboxDeviceWrapper* w = (XboxDeviceWrapper*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return nullptr;
    w->vtable = g_xboxDeviceVtable;
    w->real = real;
    w->refCount = 1;
    real->AddRef();
    return w;
}

// ------------------------------------------------------------------
// Xbox command-list wrapper.
//
// The game calls Xbox command-list vtable slots on the list returned
// by CreateCommandList. Returning the RAW desktop list is wrong: the
// Xbox list vtable is larger/different (the game calls slots 131 and
// 132, which are PAST the desktop ID3D12GraphicsCommandList vtable),
// so those calls read garbage function pointers past the desktop
// vtable and corrupt the list state — boot #33 proved it: slots 131/132
// ran unchecked, then slot 9 (Close) failed and the game int3'd at
// 0x142478bd4. So the list gets the same treatment as the device: a
// guest-facing Xbox vtable with the real desktop list behind it.
// ------------------------------------------------------------------
struct XboxCommandListWrapper {
    void** vtable;                    // points at g_xboxCommandListVtable
    ID3D12GraphicsCommandList* real;  // real desktop list (owned)
    LONG refCount;
};

static void* g_xboxCommandListVtable[192];
static bool g_xboxCommandListVtableInit = false;

static HRESULT WINAPI XboxList_QueryInterface(XboxCommandListWrapper* self,
                                              REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (!self) return E_POINTER;
    if (memcmp(&riid, &__uuidof(ID3D12GraphicsCommandList),
               sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(ID3D12CommandList), sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(IUnknown), sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refCount);
        *ppvObject = self;
        return S_OK;
    }
    Log("xboxlist", "QueryInterface: unknown IID, E_NOINTERFACE");
    return E_NOINTERFACE;
}

static ULONG WINAPI XboxList_AddRef(XboxCommandListWrapper* self) {
    if (!self) return 0;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG WINAPI XboxList_Release(XboxCommandListWrapper* self) {
    if (!self) return 0;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) {
        if (self->real) self->real->Release();
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)rc;
}

// Generic stub for Xbox list slots not yet implemented. Logs the
// caller's return address (so the game call site — and therefore the
// slot — can be identified from the disassembly) and returns E_NOTIMPL.
// NOTE: only correct for HRESULT-returning methods.
static HRESULT WINAPI XboxList_Unimplemented(XboxCommandListWrapper* self) {
    (void)self;
    Log("xboxlist",
        "UNIMPLEMENTED Xbox command-list slot called from game %p -> "
        "E_NOTIMPL (identify the slot from the call site, then "
        "implement it)",
        _ReturnAddress());
    return E_NOTIMPL;
}

// Slot 9: Close. Desktop ID3D12GraphicsCommandList slot 9 is also Close,
// and the Xbox call at 0x142478bcd passes no args — matches. Forwards to
// the real list's Close().
static HRESULT WINAPI XboxList_Close(XboxCommandListWrapper* self) {
    if (!self || !self->real) return E_POINTER;
    HRESULT hr = self->real->Close();
    Log("xboxlist", "Close -> hr=0x%08x", (unsigned)hr);
    return hr;
}

// Slots 131/132: Xbox-specific command-list methods. Called by the game
// right after CreateCommandList (boot #33, 0x142478b9e / 0x142478bbf)
// with (0x700000, 0, 0, 0xffffffffff00) and (0xc3c0, 0x80020000, 0,
// 0xffffffffff00); the game does NOT check their return values. Their
// exact meaning is unknown — stub them to S_OK (no side effects) so the
// list state stays clean for Close(). If the game later depends on
// their effect, they will need a real implementation.
static HRESULT WINAPI XboxList_Stub131(XboxCommandListWrapper* self,
                                        UINT64 a, UINT64 b, UINT64 c,
                                        UINT64 d) {
    (void)self;
    Log("xboxlist", "slot131 stub a=0x%llx b=0x%llx c=0x%llx d=0x%llx",
        (unsigned long long)a, (unsigned long long)b,
        (unsigned long long)c, (unsigned long long)d);
    return S_OK;
}

static HRESULT WINAPI XboxList_Stub132(XboxCommandListWrapper* self,
                                        UINT64 a, UINT64 b, UINT64 c,
                                        UINT64 d) {
    (void)self;
    Log("xboxlist", "slot132 stub a=0x%llx b=0x%llx c=0x%llx d=0x%llx",
        (unsigned long long)a, (unsigned long long)b,
        (unsigned long long)c, (unsigned long long)d);
    return S_OK;
}

// Build the 192-slot Xbox command-list vtable once.
static void InitXboxCommandListVtable() {
    if (g_xboxCommandListVtableInit) return;
    for (int i = 0; i < 192; ++i)
        g_xboxCommandListVtable[i] = (void*)XboxList_Unimplemented;
    g_xboxCommandListVtable[0] = (void*)XboxList_QueryInterface;
    g_xboxCommandListVtable[1] = (void*)XboxList_AddRef;
    g_xboxCommandListVtable[2] = (void*)XboxList_Release;
    g_xboxCommandListVtable[9] = (void*)XboxList_Close;
    g_xboxCommandListVtable[131] = (void*)XboxList_Stub131;
    g_xboxCommandListVtable[132] = (void*)XboxList_Stub132;
    g_xboxCommandListVtableInit = true;
}

// Wrap a real desktop command list in a guest-facing Xbox list.
static XboxCommandListWrapper* WrapXboxCommandList(
    ID3D12GraphicsCommandList* real) {
    if (!real) return nullptr;
    InitXboxCommandListVtable();
    XboxCommandListWrapper* w = (XboxCommandListWrapper*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return nullptr;
    w->vtable = g_xboxCommandListVtable;
    w->real = real;
    w->refCount = 1;
    real->AddRef();
    return w;
}

// Unwrap a guest-facing Xbox command list back to the real desktop list
// (for passing to ExecuteCommandLists and other desktop calls that need
// the real pointer). Returns the input unchanged if it is not a wrapper
// (best-effort: a wrapper's vtable is g_xboxCommandListVtable).
static ID3D12GraphicsCommandList* UnwrapXboxCommandList(void* p) {
    if (!p) return nullptr;
    void** vt = *(void***)p;
    if (vt == g_xboxCommandListVtable) {
        return ((XboxCommandListWrapper*)p)->real;
    }
    return (ID3D12GraphicsCommandList*)p;
}

// ------------------------------------------------------------------
// Descriptor-heap wrapper.
//
// Boot #46/#47 crash: the game calls heap slot 9
// (GetCPUDescriptorHandleForHeapStart) at 0x141cf3358 right after
// CreateDescriptorHeap returns, with rcx=heap and rdx NOT set (garbage),
// expecting the CPU handle in rax. The desktop D3D12Core implementation
// writes the handle through rdx (hidden-pointer struct return) and
// returns rdx — so it stores to the game's garbage rdx, which is
// (thread_id<<32)|1, and faults inside D3D12Core.dll+0x5c0c7 writing
// to that address (boot #46: 0x323800000001, thread 12856=0x3238;
// boot #47: 0x589000000001, thread 22672=0x5890). The Xbox game expects
// the 8-byte handle in rax, not through rdx. So the heap gets the same
// treatment as the device/list: a guest-facing Xbox vtable whose
// slots 9/10 return the handle as a UINT64 in rax (never touching
// rdx), calling the real heap through the C++ API (where the compiler
// sets up rdx correctly for the desktop struct return).
// ------------------------------------------------------------------
struct XboxDescriptorHeapWrapper {
    void** vtable;                 // points at g_xboxDescriptorHeapVtable
    ID3D12DescriptorHeap* real;   // real desktop heap (owned by the layer)
    LONG refCount;
};

static void* g_xboxDescriptorHeapVtable[16];
static bool g_xboxDescriptorHeapVtableInit = false;

static HRESULT WINAPI XboxHeap_QueryInterface(XboxDescriptorHeapWrapper* self,
                                              REFIID riid,
                                              void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (!self) return E_POINTER;
    if (memcmp(&riid, &__uuidof(ID3D12DescriptorHeap), sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(IUnknown), sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refCount);
        *ppvObject = self;
        return S_OK;
    }
    Log("xboxheap", "QueryInterface: unknown IID, E_NOINTERFACE");
    return E_NOINTERFACE;
}

static ULONG WINAPI XboxHeap_AddRef(XboxDescriptorHeapWrapper* self) {
    if (!self) return 0;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG WINAPI XboxHeap_Release(XboxDescriptorHeapWrapper* self) {
    if (!self) return 0;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) {
        if (self->real) self->real->Release();
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)rc;
}

// Generic stub for Xbox heap slots not yet implemented. Logs the
// caller's return address (so the game call site — and therefore the
// slot — can be identified from the disassembly) and returns E_NOTIMPL.
// NOTE: only correct for HRESULT-returning methods.
static HRESULT WINAPI XboxHeap_Unimplemented(XboxDescriptorHeapWrapper* self) {
    (void)self;
    Log("xboxheap",
        "UNIMPLEMENTED Xbox descriptor-heap slot called from game %p -> "
        "E_NOTIMPL (identify the slot from the call site, then "
        "implement it)",
        _ReturnAddress());
    return E_NOTIMPL;
}

// Slot 8: GetDesc. Xbox struct-return convention (as with slot 26
// GetCustomHeapProperties): (return buffer, this). Copy the real heap's
// desc into the game's return buffer.
static void WINAPI XboxHeap_GetDesc(D3D12_DESCRIPTOR_HEAP_DESC* retBuf,
                                    XboxDescriptorHeapWrapper* self) {
    if (!retBuf || !self || !self->real) return;
    *retBuf = self->real->GetDesc();
}

// Slot 9: GetCPUDescriptorHandleForHeapStart. Returns the 8-byte CPU
// handle as a UINT64 in rax (the game reads rax at 0x141cf335b and
// stores it as the heap's CPU base); never touches rdx, which the
// game leaves garbage — the desktop D3D12Core getter writes through
// rdx and faults (boots #46/#47).
static UINT64 WINAPI XboxHeap_GetCPUDescriptorHandleForHeapStart(
    XboxDescriptorHeapWrapper* self) {
    if (!self || !self->real) return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        self->real->GetCPUDescriptorHandleForHeapStart();
    Log("xboxheap", "GetCPUDescriptorHandleForHeapStart -> 0x%llx",
        (unsigned long long)h.ptr);
    return (UINT64)h.ptr;
}

// Slot 10: GetGPUDescriptorHandleForHeapStart. Same rax convention:
// the game reads rax at 0x141cf3395 and subtracts the CPU handle to
// get the GPU-CPU delta for the allocator struct.
static UINT64 WINAPI XboxHeap_GetGPUDescriptorHandleForHeapStart(
    XboxDescriptorHeapWrapper* self) {
    if (!self || !self->real) return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE h =
        self->real->GetGPUDescriptorHandleForHeapStart();
    Log("xboxheap", "GetGPUDescriptorHandleForHeapStart -> 0x%llx",
        (unsigned long long)h.ptr);
    return (UINT64)h.ptr;
}

// Build the 16-slot Xbox descriptor-heap vtable once.
static void InitXboxDescriptorHeapVtable() {
    if (g_xboxDescriptorHeapVtableInit) return;
    for (int i = 0; i < 16; ++i)
        g_xboxDescriptorHeapVtable[i] = (void*)XboxHeap_Unimplemented;
    g_xboxDescriptorHeapVtable[0] = (void*)XboxHeap_QueryInterface;
    g_xboxDescriptorHeapVtable[1] = (void*)XboxHeap_AddRef;
    g_xboxDescriptorHeapVtable[2] = (void*)XboxHeap_Release;
    g_xboxDescriptorHeapVtable[8] = (void*)XboxHeap_GetDesc;
    g_xboxDescriptorHeapVtable[9] =
        (void*)XboxHeap_GetCPUDescriptorHandleForHeapStart;
    g_xboxDescriptorHeapVtable[10] =
        (void*)XboxHeap_GetGPUDescriptorHandleForHeapStart;
    g_xboxDescriptorHeapVtableInit = true;
}

// Wrap a real desktop descriptor heap in a guest-facing Xbox heap.
static XboxDescriptorHeapWrapper* WrapXboxDescriptorHeap(
    ID3D12DescriptorHeap* real) {
    if (!real) return nullptr;
    InitXboxDescriptorHeapVtable();
    XboxDescriptorHeapWrapper* w = (XboxDescriptorHeapWrapper*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return nullptr;
    w->vtable = g_xboxDescriptorHeapVtable;
    w->real = real;
    w->refCount = 1;
    real->AddRef();
    return w;
}

// Unwrap a guest-facing Xbox descriptor heap back to the real desktop
// heap (for passing to SetDescriptorHeaps and other desktop calls that
// need the real pointer). Returns the input unchanged if it is not a
// wrapper (best-effort: a wrapper's vtable is g_xboxDescriptorHeapVtable).
static ID3D12DescriptorHeap* UnwrapXboxDescriptorHeap(void* p) {
    if (!p) return nullptr;
    void** vt = *(void***)p;
    if (vt == g_xboxDescriptorHeapVtable) {
        return ((XboxDescriptorHeapWrapper*)p)->real;
    }
    return (ID3D12DescriptorHeap*)p;
}

// ------------------------------------------------------------------
// Minimal IDXGIDevice wrapper.
//
// The game QIs the D3D12 device for IID_IDXGIDevice (boot #36) to reach
// the DXGI adapter (swapchain creation / adapter info). The real desktop
// D3D12 device does NOT expose IDXGIDevice through QueryInterface
// (boot #37: forwarding the QI to the real device returns E_NOINTERFACE),
// so the layer provides its own. GetAdapter returns the real DXGI
// adapter that backs the D3D12 device (matched by adapter LUID, falling
// back to adapter 0). The rest is stubbed: the game only needs the
// adapter from this interface in practice.
// ------------------------------------------------------------------
struct DxgiDeviceWrapper {
    void** vtable;           // points at g_dxgiDeviceVtable
    IDXGIAdapter* adapter;   // real DXGI adapter (owned by the layer)
    LONG refCount;
};

static void* g_dxgiDeviceVtable[12];
static bool g_dxgiDeviceVtableInit = false;

static HRESULT WINAPI DxgiDevice_QueryInterface(DxgiDeviceWrapper* self,
                                                REFIID riid,
                                                void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (!self) return E_POINTER;
    if (memcmp(&riid, &__uuidof(IDXGIDevice), sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(IDXGIObject), sizeof(GUID)) == 0 ||
        memcmp(&riid, &__uuidof(IUnknown), sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refCount);
        *ppvObject = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI DxgiDevice_AddRef(DxgiDeviceWrapper* self) {
    if (!self) return 0;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG WINAPI DxgiDevice_Release(DxgiDeviceWrapper* self) {
    if (!self) return 0;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) {
        if (self->adapter) self->adapter->Release();
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)rc;
}

// IDXGIObject methods the game does not use — forward to the real
// adapter where a real object exists, else E_NOTIMPL.
static HRESULT WINAPI DxgiDevice_SetPrivateData(DxgiDeviceWrapper* self,
                                                REFGUID guid, UINT size,
                                                const void* data) {
    (void)self; (void)guid; (void)size; (void)data;
    return E_NOTIMPL;
}
static HRESULT WINAPI DxgiDevice_SetPrivateDataInterface(
    DxgiDeviceWrapper* self, REFGUID guid, const IUnknown* obj) {
    (void)self; (void)guid; (void)obj;
    return E_NOTIMPL;
}
static HRESULT WINAPI DxgiDevice_GetPrivateData(DxgiDeviceWrapper* self,
                                                REFGUID guid, UINT* size,
                                                void* data) {
    (void)self; (void)guid; (void)size; (void)data;
    return E_NOTIMPL;
}
static HRESULT WINAPI DxgiDevice_GetParent(DxgiDeviceWrapper* self,
                                           REFIID riid, void** parent) {
    (void)self; (void)riid; (void)parent;
    return E_NOINTERFACE;
}

// Slot 7: GetAdapter — the method the game actually wants. Returns the
// real DXGI adapter (AddRef'd).
static HRESULT WINAPI DxgiDevice_GetAdapter(DxgiDeviceWrapper* self,
                                            IDXGIAdapter** adapter) {
    if (!adapter) return E_POINTER;
    *adapter = nullptr;
    if (!self || !self->adapter) return E_NOINTERFACE;
    self->adapter->AddRef();
    *adapter = self->adapter;
    Log("dxgidev", "GetAdapter -> adapter=%p", (void*)*adapter);
    return S_OK;
}

static HRESULT WINAPI DxgiDevice_CreateSurface(DxgiDeviceWrapper* self,
                                               const DXGI_SURFACE_DESC* desc,
                                               UINT numSurfaces,
                                               DXGI_USAGE usage,
                                               const DXGI_SHARED_RESOURCE* shared,
                                               IDXGISurface** surface) {
    (void)self; (void)desc; (void)numSurfaces; (void)usage; (void)shared;
    if (surface) *surface = nullptr;
    return E_NOTIMPL;
}

static HRESULT WINAPI DxgiDevice_QueryResourceResidency(
    DxgiDeviceWrapper* self, IUnknown* const* resources,
    DXGI_RESIDENCY* status, UINT numResources) {
    (void)self; (void)resources;
    if (!status) return E_POINTER;
    for (UINT i = 0; i < numResources; ++i)
        status[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
}

static HRESULT WINAPI DxgiDevice_SetGPUThreadPriority(
    DxgiDeviceWrapper* self, INT priority) {
    (void)self; (void)priority;
    return S_OK;
}

static HRESULT WINAPI DxgiDevice_GetGPUThreadPriority(
    DxgiDeviceWrapper* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = 0;
    return S_OK;
}

static void InitDxgiDeviceVtable() {
    if (g_dxgiDeviceVtableInit) return;
    g_dxgiDeviceVtable[0] = (void*)DxgiDevice_QueryInterface;
    g_dxgiDeviceVtable[1] = (void*)DxgiDevice_AddRef;
    g_dxgiDeviceVtable[2] = (void*)DxgiDevice_Release;
    g_dxgiDeviceVtable[3] = (void*)DxgiDevice_SetPrivateData;
    g_dxgiDeviceVtable[4] = (void*)DxgiDevice_SetPrivateDataInterface;
    g_dxgiDeviceVtable[5] = (void*)DxgiDevice_GetPrivateData;
    g_dxgiDeviceVtable[6] = (void*)DxgiDevice_GetParent;
    g_dxgiDeviceVtable[7] = (void*)DxgiDevice_GetAdapter;
    g_dxgiDeviceVtable[8] = (void*)DxgiDevice_CreateSurface;
    g_dxgiDeviceVtable[9] = (void*)DxgiDevice_QueryResourceResidency;
    g_dxgiDeviceVtable[10] = (void*)DxgiDevice_SetGPUThreadPriority;
    g_dxgiDeviceVtable[11] = (void*)DxgiDevice_GetGPUThreadPriority;
    g_dxgiDeviceVtableInit = true;
}

// Find the real DXGI adapter that backs a D3D12 device (matched by
// adapter LUID; falls back to adapter 0). Returns a new reference.
static IDXGIAdapter* FindDxgiAdapterForDevice(ID3D12Device* dev) {
    if (!dev) return nullptr;
    LUID luid = dev->GetAdapterLuid();
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                 (void**)&factory)) || !factory)
        return nullptr;
    IDXGIAdapter* found = nullptr;
    IDXGIAdapter* first = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter* a = nullptr;
        if (factory->EnumAdapters(i, &a) != S_OK || !a) break;
        if (!first) { first = a; a->AddRef(); }
        DXGI_ADAPTER_DESC d = {};
        if (SUCCEEDED(a->GetDesc(&d)) &&
            d.AdapterLuid.LowPart == luid.LowPart &&
            d.AdapterLuid.HighPart == luid.HighPart) {
            found = a;
            break;
        }
        a->Release();
    }
    factory->Release();
    if (found) {
        if (first) first->Release();
        return found;
    }
    return first;  // fallback: adapter 0 (already AddRef'd)
}

// Build an IDXGIDevice wrapper for a D3D12 device.
static DxgiDeviceWrapper* WrapDxgiDevice(ID3D12Device* dev) {
    IDXGIAdapter* adapter = FindDxgiAdapterForDevice(dev);
    if (!adapter) return nullptr;
    InitDxgiDeviceVtable();
    DxgiDeviceWrapper* w = (DxgiDeviceWrapper*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) { adapter->Release(); return nullptr; }
    w->vtable = g_dxgiDeviceVtable;
    w->adapter = adapter;
    w->refCount = 1;
    return w;
}

extern "C" HRESULT HleD3D12XboxCreateDevice(
    IUnknown* adapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid,
    void** ppDevice) {
    // REAL Xbox signature (grounded 2026-11-23 from the real exe's call
    // site at 0x1424785b7): (adapter, &params, riid, &device). The game
    // passes adapter=NULL (default) and params.Version = 0x20004 (SDK
    // 2.4). The struct carries NO feature level — on Xbox the level is
    // fixed at 12_2 (DirectX 12 Ultimate). On desktop the layer tries
    // the levels from highest to lowest and takes the best the hardware
    // offers (RX 5700 XT: 12_1). The created device is cached so later
    // steps (swapchain, pacing) can use it.
    if (params) {
        Log("d3d12xs",
            "D3D12XboxCreateDevice params: Version 0x%x, "
            "GraphicsSwitchingMode %u, ProcessDebugFlags 0x%x",
            params->Version, params->GraphicsSwitchingMode,
            params->ProcessDebugFlags);
    } else {
        Log("d3d12xs",
            "D3D12XboxCreateDevice params: NULL (guest passed no struct)");
    }
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    // The Xbox device IID the real exe passes (RE report, 2026-11-23):
    // {5405C344-D457-444E-B4DD-2366E45AEE39}. It is NOT desktop
    // IID_ID3D12Device (189819F1-1DB6-4B57-BE54-1821339B85F7) — the
    // desktop driver does not know it, so passing it through to
    // D3D12CreateDevice fails with E_NOINTERFACE and the game would
    // die at device creation. Log it when seen; the retry below maps
    // it to the desktop base device. (kXboxDeviceIID is defined above,
    // with the COM wrapper.)
    if (memcmp(&riid, &kXboxDeviceIID, sizeof(GUID)) == 0) {
        Log("d3d12xs",
            "D3D12XboxCreateDevice riid is the Xbox device IID "
            "{5405C344-D457-444E-B4DD-2366E45AEE39} (not desktop "
            "IID_ID3D12Device) — will map to the desktop base device");
    }
    // Create the REAL desktop device into a local first. It is cached
    // in g_xsDevice for the layer's own use, and the game gets the
    // Xbox COM WRAPPER (WrapXboxDevice), never the raw desktop device
    // — boot #28 proved the raw device AVs in D3D12Core because the
    // game indexes the desktop vtable at Xbox offsets.
    ID3D12Device* realDev = nullptr;
    HRESULT hr = E_FAIL;
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); ++i) {
        hr = D3D12CreateDevice(adapter, kLevels[i], riid,
                               (void**)&realDev);
        if (SUCCEEDED(hr)) {
            Log("d3d12xs",
                "D3D12XboxCreateDevice -> REAL desktop device %p "
                "(feature level 0x%x; Xbox fixed level is 12_2, best "
                "available on this hardware)",
                realDev, (unsigned)kLevels[i]);
            break;
        }
        if (hr == E_NOINTERFACE) {
            // Unknown IID (the Xbox device IID, or another Xbox
            // interface the desktop driver does not know) — retry
            // with the desktop base IID_ID3D12Device.
            hr = D3D12CreateDevice(adapter, kLevels[i],
                                   __uuidof(ID3D12Device),
                                   (void**)&realDev);
            if (SUCCEEDED(hr)) {
                Log("d3d12xs",
                    "D3D12XboxCreateDevice -> REAL desktop device %p "
                    "(feature level 0x%x; requested IID unknown to "
                    "the desktop driver, mapped to IID_ID3D12Device)",
                    realDev, (unsigned)kLevels[i]);
                break;
            }
        }
    }
    if (SUCCEEDED(hr) && realDev) {
        if (g_xsDevice) g_xsDevice->Release();
        g_xsDevice = realDev;
        g_xsDevice->AddRef();
        if (g_scGuestIsRealXboxTitle) {
            // Real Xbox title: hand the game the Xbox wrapper, not the
            // raw device (boot #28 proved the raw device AVs in
            // D3D12Core because the game indexes the desktop vtable
            // at Xbox offsets).
            XboxDeviceWrapper* wrapper = WrapXboxDevice(realDev);
            if (wrapper) {
                if (ppDevice) *ppDevice = wrapper;
                // The wrapper AddRef'd the real device; drop this
                // function's own reference (from D3D12CreateDevice).
                realDev->Release();
                Log("d3d12xs",
                    "D3D12XboxCreateDevice -> Xbox wrapper %p (real %p)",
                    wrapper, realDev);
            } else {
                if (ppDevice) *ppDevice = nullptr;
                realDev->Release();
                hr = E_OUTOFMEMORY;
            }
        } else {
            // Synthetic guest: it uses the desktop D3D12 vtable
            // layout, so it gets the raw desktop device (wrapping it
            // breaks every gfx verify run — build 44). The
            // D3D12CreateDevice reference is handed to the caller
            // (do NOT Release here); g_xsDevice holds its own AddRef.
            if (ppDevice) *ppDevice = realDev;
            Log("d3d12xs",
                "D3D12XboxCreateDevice -> raw desktop device %p "
                "(synthetic guest, no Xbox wrapper)",
                ppDevice ? *ppDevice : nullptr);
        }
    } else {
        Log("d3d12xs",
            "D3D12XboxCreateDevice FAILED hr=0x%08x (no usable feature "
            "level)",
            (unsigned)hr);
    }
    return hr;
}

// ERA d3d12_x!D3D12XCreateDevice — D3D12CreateDevice-shaped (adapter,
// minFL, riid, ppDevice). This is a DIFFERENT function from the GDK
// d3d12_xs!D3D12XboxCreateDevice (which takes a
// D3D12XBOX_CREATE_DEVICE_PARAMETERS struct). They shared one
// implementation until 2026-11-23, when the GDK form was corrected to
// the real 4-arg params signature — but the ERA guest calls
// D3D12XCreateDevice as (adapter, minFL, riid, ppDevice), so sharing
// the GDK function made it interpret the feature level (0xb000) as a
// params pointer and dereference it -> 0xC0000005 in era_gfx12. The
// ERA form keeps the old shape (ASSUMED, phase 5 — a D3D11.x-only
// title does NOT import d3d12_x at all, so no real ordinal is known).
// Real passthrough to desktop D3D12CreateDevice.
extern "C" HRESULT HleEraD3D12XCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice) {
    HRESULT hr = D3D12CreateDevice(adapter, minFL, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        if (g_xsDevice) g_xsDevice->Release();
        g_xsDevice = (ID3D12Device*)*ppDevice;
        g_xsDevice->AddRef();
        Log("d3d12x",
            "D3D12XCreateDevice -> REAL desktop device %p (minFL 0x%x)",
            *ppDevice, (unsigned)minFL);
    } else {
        Log("d3d12x",
            "D3D12XCreateDevice FAILED hr=0x%08x (minFL 0x%x)",
            (unsigned)hr, (unsigned)minFL);
    }
    return hr;
}

// D3D12ConfigureVirtualMemoryX — REAL signature (grounded 2026-11-23
// from the real exe's call site at 0x141e1cfe0): ONE arg, a pointer to
// a config struct (32 bytes, zeroed, struct+4 = a computed value).
// Returns HRESULT — and the game hits int3 if it fails, so this MUST
// return S_OK. On desktop there is no GPU virtual-address space to
// configure (the desktop driver owns VA layout), so S_OK with no
// action is the correct translation.
extern "C" HRESULT HleD3D12ConfigureVirtualMemoryX(const void* pConfig) {
    Log("d3d12xs",
        "D3D12ConfigureVirtualMemoryX(%p) -> S_OK (STUB: no desktop VA "
        "space to configure)",
        pConfig);
    return S_OK;
}

// D3D12SerializeVersionedRootSignature — the Xbox _xs variant has the
// same 3-arg shape as the desktop function (NO riid — the versioned
// serializer always returns an ID3DBlob; the 4-arg riid form belongs
// to the older non-versioned D3D12SerializeRootSignature). Grounded
// from the desktop SDK header (d3d12.h:4442, SDK 10.0.26100.0): the
// desktop function is (pRootSignature, ppBlob, ppErrorBlob). The Xbox
// form is assumed to match (the GDK _xs functions mirror the desktop
// D3D12 signatures); the two call sites in the exe (0x14247fc80,
// 0x1455ac80a) did not decode cleanly enough to prove the arg count,
// so this is flagged ASSUMED — if the boot crashes at this call, the
// call-site disassembly is the next step. Forwards straight to the
// desktop function.
extern "C" HRESULT HleD3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {
    HRESULT hr = D3D12SerializeVersionedRootSignature(pRootSignature,
                                                      ppBlob, ppErrorBlob);
    Log("d3d12xs",
        "D3D12SerializeVersionedRootSignature -> hr=0x%08x (desktop "
        "forward)",
        (unsigned)hr);
    return hr;
}

// ---- software ray tracing (see docs/SOFTWARE-RT.md) ----
// ScarlettCompat-defined entry points (not Xbox APIs): the flat surface
// the synthetic guest exercises. Thin wrappers over src/software_rt.cpp.
extern "C" HRESULT HleScSoftwareRTQueryPath(ID3D12Device* device,
                                            ScRtPath* path, uint32_t* rtTier) {
    return ScSoftwareRT_QueryPath(device, path, rtTier);
}

extern "C" ScRtMode HleScSoftwareRTGetMode() {
    return ScSoftwareRT_GetMode();
}

extern "C" HRESULT HleScSoftwareRTInit(ID3D12Device* device) {
    return ScSoftwareRT_Init(device);
}

extern "C" HRESULT HleScSoftwareRTBuildScene(ID3D12Device* device,
                                             const ScRtTriangle* tris,
                                             uint32_t triCount,
                                             ScSoftwareRTSceneHandle* scene) {
    return ScSoftwareRT_BuildScene(device, tris, triCount, scene);
}

extern "C" void HleScSoftwareRTFreeScene(ScSoftwareRTSceneHandle scene) {
    ScSoftwareRT_FreeScene(scene);
}

extern "C" HRESULT HleScSoftwareRTDispatchRays(ID3D12Device* device,
                                               ID3D12GraphicsCommandList* cmdList,
                                               ScSoftwareRTSceneHandle scene,
                                               const ScRtCamera* camera,
                                               ID3D12Resource* outputUav,
                                               uint32_t width, uint32_t height) {
    return ScSoftwareRT_DispatchRays(device, cmdList, scene, camera,
                                     outputUav, width, height);
}

// ---- fixed timestep (see docs/FIXED-TIMESTEP.md) ----
// ScarlettCompat-defined entry points (not Xbox APIs): the flat
// surface the physics proof guest exercises. Thin wrappers over
// src/fixed_timestep.cpp.
extern "C" HRESULT HleScFixedTimestepInit(double rateHz,
                                          uint32_t maxStepsPerFrame) {
    return ScFixedTimestep_Init(rateHz, maxStepsPerFrame);
}
extern "C" HRESULT HleScFixedTimestepSetStepCallback(ScPhysicsStepFn fn,
                                                     void* ctx) {
    return ScFixedTimestep_SetStepCallback(fn, ctx);
}
extern "C" HRESULT HleScFixedTimestepPump(uint32_t* stepsTaken) {
    return ScFixedTimestep_Pump(stepsTaken);
}
extern "C" double HleScFixedTimestepAlpha(void) {
    return ScFixedTimestep_Alpha();
}
extern "C" HRESULT HleScFixedTimestepGetStats(ScFixedTimestepStats* out) {
    return ScFixedTimestep_GetStats(out);
}
extern "C" void HleScFixedTimestepShutdown(void) {
    ScFixedTimestep_Shutdown();
}

// ---- optimizations (see docs/OPTIMIZATION.md) ----
// ScarlettCompat-defined entry points (not Xbox APIs): the flat surface
// the optimization guest exercises. Thin wrappers over src/opt_*.cpp.
extern "C" HRESULT HleScOptPsoCacheInit(ID3D12Device* device) {
    return ScOpt_PsoCacheInit(device);
}

extern "C" HRESULT HleScOptPsoCacheGetGraphics(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit) {
    return ScOpt_PsoCacheGetGraphics(device, desc, pso, hit);
}

extern "C" HRESULT HleScOptPsoCacheGetCompute(
    ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit) {
    return ScOpt_PsoCacheGetCompute(device, desc, pso, hit);
}

extern "C" HRESULT HleScOptPsoCacheGetRootSignature(ID3D12Device* device,
                                                    const void* blob,
                                                    SIZE_T blobLen,
                                                    ID3D12RootSignature** rs,
                                                    BOOL* hit) {
    return ScOpt_PsoCacheGetRootSignature(device, blob, blobLen, rs, hit);
}

extern "C" HRESULT HleScOptPsoCacheStats(uint32_t* created, uint32_t* hits,
                                         uint32_t* misses) {
    return ScOpt_PsoCacheStats(created, hits, misses);
}

extern "C" void HleScOptPsoCacheDestroy() { ScOpt_PsoCacheDestroy(); }

extern "C" HRESULT HleScOptTile2D(const uint8_t* linear, uint32_t w,
                                  uint32_t h, uint32_t bpp, uint32_t tileW,
                                  uint32_t tileH, uint8_t* tiled) {
    return ScOpt_Tile2D(linear, w, h, bpp, tileW, tileH, tiled);
}

extern "C" HRESULT HleScOptDetile2D(const uint8_t* tiled, uint32_t w,
                                    uint32_t h, uint32_t bpp, uint32_t tileW,
                                    uint32_t tileH, uint8_t* linear) {
    return ScOpt_Detile2D(tiled, w, h, bpp, tileW, tileH, linear);
}

extern "C" HRESULT HleScOptDetileCached(const uint8_t* tiled, uint32_t w,
                                        uint32_t h, uint32_t bpp,
                                        uint32_t tileW, uint32_t tileH,
                                        uint8_t* linearOut, BOOL* hit) {
    return ScOpt_DetileCached(tiled, w, h, bpp, tileW, tileH, linearOut,
                              hit);
}

extern "C" HRESULT HleScOptDetileStats(uint32_t* detiles, uint32_t* hits,
                                       uint32_t* evictions) {
    return ScOpt_DetileStats(detiles, hits, evictions);
}

extern "C" void HleScOptDetileDestroy() { ScOpt_DetileDestroy(); }

extern "C" HRESULT HleScOptMeshFallbackInit(ID3D12Device* device) {
    return ScOpt_MeshFallbackInit(device);
}

extern "C" HRESULT HleScOptMeshFallbackGetShaders(const void** vsBlob,
                                                  SIZE_T* vsLen,
                                                  const void** psBlob,
                                                  SIZE_T* psLen) {
    return ScOpt_MeshFallbackGetShaders(vsBlob, vsLen, psBlob, psLen);
}

extern "C" HRESULT HleScOptMeshFallbackExpand(ID3D12Device* device,
                                              const ScOptMeshlet* meshlet,
                                              ScOptExpandedMesh* expanded) {
    return ScOpt_MeshFallbackExpand(device, meshlet, expanded);
}

extern "C" void HleScOptMeshFallbackFreeExpanded(ScOptExpandedMesh* expanded) {
    ScOpt_MeshFallbackFreeExpanded(expanded);
}

extern "C" void HleScOptMeshFallbackDestroy() { ScOpt_MeshFallbackDestroy(); }

extern "C" HRESULT HleScOptAsyncInit(ID3D12Device* device) {
    return ScOpt_AsyncInit(device);
}

extern "C" HRESULT HleScOptAsyncRunTimed(ID3D12Device* device,
                                         ScOptAsyncMode mode, double* avgMs) {
    return ScOpt_AsyncRunTimed(device, mode, avgMs);
}

extern "C" HRESULT HleScOptAsyncChecksum(ScOptAsyncMode mode,
                                         uint64_t* checksum) {
    return ScOpt_AsyncChecksum(mode, checksum);
}

extern "C" void HleScOptAsyncDestroy() { ScOpt_AsyncDestroy(); }

extern "C" HRESULT HleScOptDescHeapInit(ID3D12Device* device,
                                        D3D12_DESCRIPTOR_HEAP_TYPE heapType,
                                        uint32_t capacity,
                                        ScOptDescHeapHandle* heap) {
    return ScOpt_DescHeapInit(device, heapType, capacity, heap);
}

extern "C" HRESULT HleScOptDescAlloc(ScOptDescHeapHandle heap,
                                     D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                                     D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle,
                                     uint32_t* index) {
    return ScOpt_DescAlloc(heap, cpuHandle, gpuHandle, index);
}

extern "C" HRESULT HleScOptDescFree(ScOptDescHeapHandle heap, uint32_t index) {
    return ScOpt_DescFree(heap, index);
}

extern "C" HRESULT HleScOptDescHeapStats(ScOptDescHeapHandle heap,
                                         uint32_t* capacity, uint32_t* live,
                                         uint32_t* heaps) {
    return ScOpt_DescHeapStats(heap, capacity, live, heaps);
}

extern "C" void HleScOptDescHeapDestroy(ScOptDescHeapHandle heap) {
    ScOpt_DescHeapDestroy(heap);
}

extern "C" HRESULT HleScOptDStorageInit(ScOptDsPath* path) {
    return ScOpt_DStorageInit(path);
}

extern "C" HRESULT HleScOptDStorageReadFile(const char* path, uint64_t offset,
                                            uint32_t size, uint8_t** data,
                                            uint32_t* bytesRead) {
    return ScOpt_DStorageReadFile(path, offset, size, data, bytesRead);
}

extern "C" HRESULT HleScOptDStorageReadFileBatch(const char* path,
                                                 const uint64_t* offsets,
                                                 const uint32_t* sizes,
                                                 uint32_t count,
                                                 uint8_t** datas,
                                                 uint32_t* bytesRead) {
    return ScOpt_DStorageReadFileBatch(path, offsets, sizes, count, datas,
                                       bytesRead);
}

extern "C" HRESULT HleScOptDStorageDecompress(ScOptDsDecompress mode,
                                              const uint8_t* src, size_t srcLen,
                                              uint8_t* dst, size_t dstLen,
                                              size_t* outLen) {
    return ScOpt_DStorageDecompress(mode, src, srcLen, dst, dstLen, outLen);
}

extern "C" HRESULT HleScOptShaderCacheInit(const char* dir) {
    return ScOpt_ShaderCacheInit(dir);
}

extern "C" HRESULT HleScOptCompileCached(const char* source, size_t sourceLen,
                                         const char* profile,
                                         const char* entryPoint,
                                         uint8_t** blob, size_t* blobSize) {
    return ScOpt_CompileCached(source, sourceLen, profile, entryPoint, blob,
                               blobSize);
}

extern "C" void HleScOptShaderCacheStats(uint64_t* hits, uint64_t* misses) {
    ScOpt_ShaderCacheStats(hits, misses);
}

// ---- GPU de-tiled texture cache (opt 2, memory management) ----
extern "C" HRESULT HleScOptTexCacheInit(ID3D12Device* device,
                                        uint64_t budgetBytes,
                                        ScOptTexCacheHandle* cache) {
    return ScOpt_TexCacheInit(device, budgetBytes, cache);
}

extern "C" HRESULT HleScOptTexCacheLookup(ScOptTexCacheHandle cache,
                                          uint64_t key,
                                          ID3D12Resource** resource,
                                          BOOL* hit) {
    return ScOpt_TexCacheLookup(cache, key, resource, hit);
}

extern "C" HRESULT HleScOptTexCacheStore(ScOptTexCacheHandle cache,
                                         uint64_t key, const uint8_t* tiled,
                                         uint32_t w, uint32_t h, uint32_t bpp,
                                         uint32_t tileW, uint32_t tileH,
                                         ID3D12CommandQueue* queue,
                                         ID3D12Resource** resource) {
    return ScOpt_TexCacheStore(cache, key, tiled, w, h, bpp, tileW, tileH,
                               queue, resource);
}

extern "C" HRESULT HleScOptTexCacheEvict(ScOptTexCacheHandle cache,
                                         uint64_t key) {
    return ScOpt_TexCacheEvict(cache, key);
}

extern "C" HRESULT HleScOptTexCacheStats(ScOptTexCacheHandle cache,
                                         uint32_t* entries,
                                         uint64_t* bytesUsed,
                                         uint64_t* bytesBudget,
                                         uint32_t* hits, uint32_t* misses,
                                         uint32_t* evictions) {
    return ScOpt_TexCacheStats(cache, entries, bytesUsed, bytesBudget, hits,
                               misses, evictions);
}

extern "C" void HleScOptTexCacheDestroy(ScOptTexCacheHandle cache) {
    ScOpt_TexCacheDestroy(cache);
}

// ---- frame batching (opt 9) ----
extern "C" HRESULT HleScOptBatchInit(ID3D12CommandQueue* queue,
                                     uint32_t maxLists,
                                     ScOptBatchHandle* batch) {
    return ScOpt_BatchInit(queue, maxLists, batch);
}

extern "C" HRESULT HleScOptBatchAdd(ScOptBatchHandle batch,
                                    ID3D12CommandList* list) {
    return ScOpt_BatchAdd(batch, list);
}

extern "C" HRESULT HleScOptBatchFlush(ScOptBatchHandle batch) {
    return ScOpt_BatchFlush(batch);
}

extern "C" HRESULT HleScOptBatchStats(ScOptBatchHandle batch,
                                      uint32_t* listsSubmitted,
                                      uint32_t* submitCalls) {
    return ScOpt_BatchStats(batch, listsSubmitted, submitCalls);
}

extern "C" void HleScOptBatchDestroy(ScOptBatchHandle batch) {
    ScOpt_BatchDestroy(batch);
}

extern "C" HRESULT HleScOptDrawBatchBegin(ID3D12GraphicsCommandList* list,
                                          ScOptDrawBatchHandle* batch) {
    return ScOpt_DrawBatchBegin(list, batch);
}

extern "C" HRESULT HleScOptDrawBatchSetPSO(ScOptDrawBatchHandle batch,
                                           ID3D12PipelineState* pso) {
    return ScOpt_DrawBatchSetPSO(batch, pso);
}

extern "C" HRESULT HleScOptDrawBatchSetRootSignature(
    ScOptDrawBatchHandle batch, ID3D12RootSignature* rs) {
    return ScOpt_DrawBatchSetRootSignature(batch, rs);
}

extern "C" HRESULT HleScOptDrawBatchSetRenderTargets(
    ScOptDrawBatchHandle batch, UINT numRTs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleHandle,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    return ScOpt_DrawBatchSetRenderTargets(batch, numRTs, rtvs, singleHandle,
                                           dsv);
}

extern "C" HRESULT HleScOptDrawBatchSetVertexBuffer(
    ScOptDrawBatchHandle batch, UINT slot,
    const D3D12_VERTEX_BUFFER_VIEW* view) {
    return ScOpt_DrawBatchSetVertexBuffer(batch, slot, view);
}

extern "C" HRESULT HleScOptDrawBatchDrawInstanced(ScOptDrawBatchHandle batch,
                                                  UINT vertexCount,
                                                  UINT startVertex,
                                                  UINT instanceCount,
                                                  UINT startInstance) {
    return ScOpt_DrawBatchDrawInstanced(batch, vertexCount, startVertex,
                                        instanceCount, startInstance);
}

extern "C" HRESULT HleScOptDrawBatchFlushDraw(ScOptDrawBatchHandle batch) {
    return ScOpt_DrawBatchFlushDraw(batch);
}

extern "C" HRESULT HleScOptDrawBatchEnd(ScOptDrawBatchHandle batch) {
    return ScOpt_DrawBatchEnd(batch);
}

extern "C" HRESULT HleScOptDrawBatchStats(ScOptDrawBatchHandle batch,
                                          uint32_t* drawsRequested,
                                          uint32_t* drawsRecorded,
                                          uint32_t* setsRequested,
                                          uint32_t* setsIssued) {
    return ScOpt_DrawBatchStats(batch, drawsRequested, drawsRecorded,
                                setsRequested, setsIssued);
}

extern "C" void HleScOptDrawBatchDestroy(ScOptDrawBatchHandle batch) {
    ScOpt_DrawBatchDestroy(batch);
}

extern "C" HRESULT HleScOptFlightInit(uint32_t depth,
                                      ScOptFlightHandle* flight) {
    return ScOpt_FlightInit(depth, flight);
}

extern "C" HRESULT HleScOptFlightBeginFrame(ScOptFlightHandle flight,
                                            ID3D12CommandQueue* queue,
                                            ID3D12Fence* fence,
                                            double* waitMs) {
    return ScOpt_FlightBeginFrame(flight, queue, fence, waitMs);
}

extern "C" HRESULT HleScOptFlightEndFrame(ScOptFlightHandle flight,
                                          ID3D12CommandQueue* queue,
                                          ID3D12Fence* fence) {
    return ScOpt_FlightEndFrame(flight, queue, fence);
}

extern "C" HRESULT HleScOptFlightStats(ScOptFlightHandle flight,
                                       uint32_t* frames,
                                       double* totalWaitMs,
                                       double* avgWaitMs) {
    return ScOpt_FlightStats(flight, frames, totalWaitMs, avgWaitMs);
}

extern "C" void HleScOptFlightDestroy(ScOptFlightHandle flight) {
    ScOpt_FlightDestroy(flight);
}

// ---- Generic stub COM object ----
// A minimal COM object with a shared 64-slot vtable. Used by the
// factories that must return a REAL (callable) interface pointer:
// GameInputCreate, XAudio2Create, DStorageGetFactory. The old stubs
// returned debug sentinel integers as the out-pointer (GameInput
// 0x6A6E707574 "jnput", XAudio2 0xA0D10, DStorage 0xD570A6E); the
// game dereferences the pointer as a COM object (reads [ptr] for the
// vtable, then calls AddRef/Release) and faults. Boot #9 proved it:
// GameInputCreate's sentinel 0x6a6e707574 was read as a vtable
// pointer and the singleton getter faulted at 0x1459258e2
// (mov rax, [rsi]; rsi = the sentinel). Leonida's retail chain stop
// #5 is the same class: DStorageGetFactory must return a non-null
// factory (#4348), not a sentinel.
// Vtable: slot 0 QueryInterface (permissive: S_OK, *ppv = this,
// AddRef — the object is a generic stub, so any IID gets the same
// object), slot 1 AddRef, slot 2 Release, slots 3..63 generic S_OK
// stubs (return 0, ignore args — on x64 the caller cleans the stack,
// so arg count does not matter). 64 slots covers every method of
// IGameInput (~20), IXAudio2 (~15) and IDStorageFactory (~10) with
// room for future interfaces. The vtable is filled once by a
// file-scope static initializer (runs at DLL load, before any game
// thread exists), so no per-call init race.
struct StubComObject {
    void** vtable;
    LONG ref;
};
static void* g_stubComVtable[64];
extern "C" HRESULT StubComQueryInterface(void* self, const void* riid,
                                         void** ppv) {
    (void)riid;
    if (ppv) {
        *ppv = self;
        StubComObject* obj = (StubComObject*)self;
        InterlockedIncrement(&obj->ref);
    }
    return S_OK;
}
extern "C" ULONG StubComAddRef(void* self) {
    StubComObject* obj = (StubComObject*)self;
    return (ULONG)InterlockedIncrement(&obj->ref);
}
extern "C" ULONG StubComRelease(void* self) {
    StubComObject* obj = (StubComObject*)self;
    LONG r = InterlockedDecrement(&obj->ref);
    if (r == 0) HeapFree(GetProcessHeap(), 0, obj);
    return (ULONG)r;
}
extern "C" HRESULT StubComGeneric() { return S_OK; }

// Build 90: scoped refusal for one interface/slot only.
//
// The streaming record constructor (0x1428C9460) asks the interface cached
// at guest global 0x1478F5E78 for the submission backend it stores at
// record[+0x18]:
//
//   0x1428c95a6  add r9,0x18                 ; out = &record[+0x18]
//   0x1428c95ad  mov rcx,[0x1478F5E78]       ; cached interface
//   0x1428c95c7  call qword ptr [rax+0x50]   ; slot 10
//   0x1428c95ca  test eax,eax / jns / int3   ; guest asserts on failure
//
// Boot #71 identifies that cached object as the layer's fake
// IDStorageFactory stub (0x2ED7626C1E0, log line 209). StubComGeneric
// answered S_OK and wrote nothing, so the guest stored a null it had been
// told was a valid queue and later faulted on it at 0x1428C9A08.
//
// The layer has no DStorage queue contract to implement (the guest only
// ever calls slot 10 once, then slots 3 and 8 on the result), so this
// interface/slot now refuses explicitly. The override is per-interface:
// the factory gets its own vtable and every other stub-backed interface
// (IGameInput, XAudio2, ...) keeps StubComGeneric's S_OK behaviour.

// ---- Real DStorage queue (D1) ------------------------------------------
//
// Measured contract (guest image, no guessing):
//   0x1428c95c7  call [rax+0x50]        factory slot 10 = CreateQueue(desc, riid, out)
//   0x1428c9a0b  call [rax+0x18]        queue slot 3   = EnqueueRequest(const DSTORAGE_REQUEST*)
//   0x1428c9637  call [rax+0x40]        queue slot 8   = Submit()  (no arguments)
// The request is 0x38 bytes and is zeroed by the caller before use, so the
// field layout can be read from the guest's stores:
//   +0x00 options (0x400000 = source type memory, bit 0 = zlib)
//   +0x08 destination pointer
//   +0x18 destination size (dword decoded by the tiling helper 0x14550db40)
//   +0x20 source pointer
//   +0x28 source size
// Any copy is gated on the OS confirming both ranges are committed and
// accessible, so a misread field can never write into the 6.5 GB mapping.
struct ScDsRequest {
    uint64_t w[7];  // 0x38 bytes, captured verbatim
};

struct ScDsQueue {
    void** vtable;
    volatile LONG ref;
    int count;
    int submitted;
    int copied;
    int refused;
    int fileSource;
    // The guest's stream record advertises cap 4096 (ctx-probe: cap=4096),
    // and real DStorage auto-submits when the queue is half full. Boot #76
    // showed the guest enqueueing without an explicit Submit, so a 256 cap
    // silently dropped pages and it faulted later.
    ScDsRequest req[4096];
};

static void* g_dsQueueVtable[64];
extern "C" void DsqSubmit(void* self);  // forward: half-full auto-submit
static int g_dsDetailLogs = 0;   // detailed per-request lines, capped
static int g_dsQueues = 0;

static bool ScRangeCommitted(const void* p, size_t n) {
    if (!p || n == 0) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uint8_t* start = (const uint8_t*)p;
    const uint8_t* regionEnd = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    return start + n <= regionEnd;
}

extern "C" HRESULT DsqQueryInterface(void* self, void* iid, void** out) {
    if (!out) return E_POINTER;
    *out = self;
    return S_OK;
}
extern "C" ULONG DsqAddRef(void* self) {
    return (ULONG)InterlockedIncrement(&((ScDsQueue*)self)->ref);
}
extern "C" ULONG DsqRelease(void* self) {
    ScDsQueue* q = (ScDsQueue*)self;
    LONG n = InterlockedDecrement(&q->ref);
    if (n == 0) {
        Log("dstorage", "queue %p destroyed (queues=%d)", self, --g_dsQueues);
        HeapFree(GetProcessHeap(), 0, q);
    }
    return (ULONG)n;
}

extern "C" void DsqEnqueueRequest(void* self, const void* req) {
    ScDsQueue* q = (ScDsQueue*)self;
    if (!req) {
        Log("dstorage", "queue %p EnqueueRequest(NULL) ignored", self);
        return;
    }
    if (q->count >= 2048) {
        // Half-full automatic submission, matching DStorage semantics.
        Log("dstorage", "queue %p half-full (%d) -> automatic Submit",
            self, q->count);
        DsqSubmit(self);
    }
    int i = q->count++;
    memcpy(q->req[i].w, req, sizeof(q->req[i].w));
    const uint64_t* w = q->req[i].w;
    const void* dest = (const void*)w[1];
    const uint32_t destSize = (uint32_t)w[3];
    const void* src = (const void*)w[4];
    const uint32_t srcSize = (uint32_t)w[5];
    if (g_dsDetailLogs < 32) {
        ++g_dsDetailLogs;
        Log("dstorage",
            "EnqueueRequest[%d] q=%p options=0x%016llx dest=%p destSize=%u "
            "src=%p srcSize=%u raw=%016llx %016llx %016llx %016llx %016llx "
            "%016llx %016llx",
            i, self, (unsigned long long)w[0], dest, destSize, src, srcSize,
            (unsigned long long)w[0], (unsigned long long)w[1],
            (unsigned long long)w[2], (unsigned long long)w[3],
            (unsigned long long)w[4], (unsigned long long)w[5],
            (unsigned long long)w[6]);
        Log("dstorage",
            "EnqueueRequest[%d] validate dest_committed=%d src_committed=%d",
            i, (int)ScRangeCommitted(dest, destSize),
            (int)ScRangeCommitted(src, srcSize));
    }
}

// Measured mapping (boot #74 + offline check): the stream the guest enqueues
// for hash 0xACA58068328F107A reads zlib pages from shaders.rpf. Request 0
// asks for 0x4594 bytes at 0x1057d7c0 and that exact span has a 0x78da
// header and inflates to 65536 bytes, which is the request's destSize. The
// other ten RPFs were tested at the same offset and none inflates.
static const char* kDsMeasuredPack = "D:\\game\\game\\shaders.rpf";
static const uint64_t kDsMeasuredHash = 0xACA58068328F107Aull;
static const char* ScDsPackPath() {
    const char* env = getenv("SCARLETT_DS_PACK");
    return (env && *env) ? env : kDsMeasuredPack;
}

extern "C" void DsqSubmit(void* self) {
    ScDsQueue* q = (ScDsQueue*)self;
    int copied = 0, refused = 0, fileSource = 0;
    for (int i = 0; i < q->count; ++i) {
        const uint64_t* w = q->req[i].w;
        const uint64_t options = w[0];
        const void* dest = (const void*)w[1];
        const uint32_t destSize = (uint32_t)w[3];
        const void* src = (const void*)w[4];
        const uint32_t srcSize = (uint32_t)w[5];
        // bit 22 = DSTORAGE_REQUEST_SOURCE_TYPE memory
        const bool memorySource = (options & 0x400000ull) != 0;
        if (!memorySource) {
            ++fileSource;
            // File-sourced: +0x20 hash, +0x28 file offset, +0x30 compressed
            // size. Serve it for real when the hash is the measured one and
            // the destination is validated; otherwise refuse by name.
            const uint64_t hash = w[4];
            const uint64_t fileOffset = w[5];
            const uint32_t compSize = (uint32_t)w[6];
            if (hash != kDsMeasuredHash) {
                Log("dstorage",
                    "Submit[%d] refused: unresolvable source hash "
                    "0x%016llx (options=0x%016llx dest=%p destSize=%u "
                    "offset=0x%llx compSize=%u) -> E_NOTIMPL",
                    i, (unsigned long long)hash, (unsigned long long)options,
                    dest, destSize, (unsigned long long)fileOffset, compSize);
                continue;
            }
            if (compSize == 0 || destSize == 0 ||
                !ScRangeCommitted(dest, destSize)) {
                Log("dstorage",
                    "Submit[%d] refused: unvalidated destination (hash "
                    "0x%016llx dest=%p destSize=%u offset=0x%llx "
                    "compSize=%u)",
                    i, (unsigned long long)hash, dest, destSize,
                    (unsigned long long)fileOffset, compSize);
                continue;
            }
            static bool dsInit = false;
            if (!dsInit) {
                ScOptDsPath dsPath = SC_OPT_DS_PATH_OVERLAPPED;
                ScOpt_DStorageInit(&dsPath);
                dsInit = true;
            }
            uint8_t* comp = nullptr;
            uint32_t got = 0;
            if (ScOpt_DStorageReadFile(ScDsPackPath(), fileOffset, compSize,
                                       &comp, &got) != S_OK ||
                !comp || got != compSize) {
                Log("dstorage",
                    "Submit[%d] refused: pack read failed (path=%s offset=0x%llx "
                    "compSize=%u got=%u) -> E_FAIL",
                    i, ScDsPackPath(), (unsigned long long)fileOffset,
                    compSize, got);
                free(comp);
                continue;
            }
            uint8_t* outBuf = (uint8_t*)malloc(destSize);
            size_t outLen = 0;
            HRESULT hr = outBuf ? ScOpt_DStorageDecompress(
                                      SC_OPT_DS_DECOMP_ZLIB, comp, compSize,
                                      outBuf, destSize, &outLen)
                                : E_OUTOFMEMORY;
            free(comp);
            if (hr != S_OK || !outBuf || outLen < destSize) {
                Log("dstorage",
                    "Submit[%d] refused: inflate failed (hash 0x%016llx "
                    "offset=0x%llx compSize=%u destSize=%u outLen=%u hr=0x%08x)"
                    " -> %s (zlib only; BCPack stays E_NOTIMPL)",
                    i, (unsigned long long)hash,
                    (unsigned long long)fileOffset, compSize, destSize, outLen,
                    (unsigned)hr, hr == S_OK ? "short output" : "E_NOTIMPL");
                free(outBuf);
                continue;
            }
            memcpy((void*)dest, outBuf, destSize);
            free(outBuf);
            ++copied;
            continue;
        }
        if (srcSize == 0 || destSize == 0 ||
            !ScRangeCommitted(dest, destSize) ||
            !ScRangeCommitted(src, srcSize)) {
            ++refused;
            Log("dstorage",
                "Submit[%d] refused: unvalidated range (options=0x%016llx "
                "dest=%p destSize=%u src=%p srcSize=%u)",
                i, (unsigned long long)options, dest, destSize, src, srcSize);
            continue;
        }
        const uint32_t n = destSize < srcSize ? destSize : srcSize;
        memcpy((void*)dest, src, n);
        ++copied;
    }
    q->submitted++;
    q->copied += copied;
    q->refused += refused;
    q->fileSource += fileSource;
    Log("dstorage",
        "Submit q=%p batch=%d copied=%d refused=%d file=%d totals: batches=%d "
        "copied=%d refused=%d file=%d",
        self, q->count, copied, refused, fileSource, q->submitted, q->copied,
        q->refused, q->fileSource);
    q->count = 0;
}

extern "C" void DsqClose(void* self) {
    Log("dstorage", "queue %p Close (pending=%d submitted=%d)", self,
        ((ScDsQueue*)self)->count, ((ScDsQueue*)self)->submitted);
}

extern "C" HRESULT DsqUnsupported(void* self, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4) {
    // The slot is not passed to the stub, so record the guest's return
    // address: disassembling the call site names the vtable slot. Boot #77
    // showed two of these immediately before a VCRUNTIME140 fault, and
    // without the call site they cannot be identified.
    Log("dstorage",
        "queue %p unsupported method from %p (this=%p %llx %llx %llx %llx) "
        "-> E_NOTIMPL",
        self, _ReturnAddress(), self, (unsigned long long)a1,
        (unsigned long long)a2, (unsigned long long)a3,
        (unsigned long long)a4);
    return E_NOTIMPL;
}

struct DsQueueVtableInit {
    DsQueueVtableInit() {
        for (int i = 0; i < 64; ++i) g_dsQueueVtable[i] = (void*)DsqUnsupported;
        g_dsQueueVtable[0] = (void*)DsqQueryInterface;
        g_dsQueueVtable[1] = (void*)DsqAddRef;
        g_dsQueueVtable[2] = (void*)DsqRelease;
        g_dsQueueVtable[3] = (void*)DsqEnqueueRequest;   // measured 0x1428c9a0b
        g_dsQueueVtable[8] = (void*)DsqSubmit;           // measured 0x1428c9637
        g_dsQueueVtable[9] = (void*)DsqClose;
    }
};
static DsQueueVtableInit g_dsQueueVtableInit;

static ScDsQueue* ScDsCreateQueue(const char* why) {
    ScDsQueue* q = (ScDsQueue*)HeapAlloc(GetProcessHeap(),
                                         HEAP_ZERO_MEMORY, sizeof(ScDsQueue));
    if (!q) {
        Log("dstorage", "CreateQueue(%s): HeapAlloc failed -> E_OUTOFMEMORY",
            why);
        return nullptr;
    }
    q->vtable = g_dsQueueVtable;
    q->ref = 1;
    ++g_dsQueues;
    Log("dstorage", "CreateQueue(%s) -> %p vtable=%p slot3=EnqueueRequest "
        "slot8=Submit (queues=%d)",
        why, (void*)q, (void*)q->vtable, g_dsQueues);
    return q;
}

// ---- DStorage file/device object (factory slot 5) ----------------------
//
// Measured call site (record constructor 0x1428c9517..0x1428c9522):
//   rcx = factory, rdx = 1, r8 = "fiDeviceDirectStorage", r9 = REFIID,
//   [rsp+0x20] = &record+0x30  ->  object whose slot 3 the guest calls
//   later at 0x144f65847 as (this, 0, 0) expecting a boolean.
// The layer has no pack-file mapping for that device name yet, so this is a
// real, refcounted, logging object with a real open state — and every read
// it cannot serve is refused by name rather than faked.
struct ScDsFile {
    void** vtable;
    volatile LONG ref;
    int open;
    char name[128];
};

static void* g_dsFileVtable[64];
static int g_dsFiles = 0;

extern "C" HRESULT DsfQueryInterface(void* self, void* iid, void** out) {
    if (!out) return E_POINTER;
    *out = self;
    return S_OK;
}
extern "C" ULONG DsfAddRef(void* self) {
    return (ULONG)InterlockedIncrement(&((ScDsFile*)self)->ref);
}
extern "C" ULONG DsfRelease(void* self) {
    ScDsFile* f = (ScDsFile*)self;
    LONG n = InterlockedDecrement(&f->ref);
    if (n == 0) {
        Log("dstorage", "file object %p (%s) destroyed (files=%d)", self,
            f->name, --g_dsFiles);
        HeapFree(GetProcessHeap(), 0, f);
    }
    return (ULONG)n;
}

// Measured shape at 0x144f65847: (this, 0, 0) -> boolean.
//
// Boot #75 showed the guest polls this in a tight loop while it is FALSE
// (46 M calls, 4.1 GB of log, F: filled). Logging is therefore capped and
// the refusal is counted rather than printed per call; the boolean itself
// is real (the object is not backed by a pack file), so it stays FALSE.
static LONG g_dsfPolls = 0;
static LONG g_dsfPollLogs = 0;
extern "C" BOOL DsfReady(void* self, uint64_t a1, uint64_t a2) {
    ScDsFile* f = (ScDsFile*)self;
    const BOOL ok = f->open ? TRUE : FALSE;
    LONG n = InterlockedIncrement(&g_dsfPolls);
    if (ok || InterlockedIncrement(&g_dsfPollLogs) <= 16) {
        Log("dstorage", "file %p (%s) slot3(a=%llx b=%llx) -> %d (poll %ld)",
            self, f->name, (unsigned long long)a1,
            (unsigned long long)a2, (int)ok, (long)n);
    } else if ((n & 0xffff) == 0) {
        Log("dstorage", "file %p (%s): %ld polls, still refusing (no pack "
            "mapping for this device)", self, f->name, (long)n);
    }
    return ok;
}

extern "C" HRESULT DsfUnsupported(void* self, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4) {
    Log("dstorage",
        "file %p (%s) unsupported method (%llx %llx %llx %llx) -> E_NOTIMPL",
        self, ((ScDsFile*)self)->name, (unsigned long long)a1,
        (unsigned long long)a2, (unsigned long long)a3,
        (unsigned long long)a4);
    return E_NOTIMPL;
}

struct DsFileVtableInit {
    DsFileVtableInit() {
        for (int i = 0; i < 64; ++i) g_dsFileVtable[i] = (void*)DsfUnsupported;
        g_dsFileVtable[0] = (void*)DsfQueryInterface;
        g_dsFileVtable[1] = (void*)DsfAddRef;
        g_dsFileVtable[2] = (void*)DsfRelease;
        g_dsFileVtable[3] = (void*)DsfReady;   // measured 0x144f65847
    }
};
static DsFileVtableInit g_dsFileVtableInit;

// Factory slot 5: measured (this, flags=1, name, iid, void** out).
extern "C" HRESULT StubStorageFactorySlot5(void* self, uint64_t flags,
                                           const char* name, void* iid,
                                           void** out) {
    Log("dstorage",
        "IDStorageFactory slot 5 OpenFile this=%p flags=%llu name=%p(%s) "
        "iid=%p out=%p return-to=%p",
        self, (unsigned long long)flags, (const void*)name,
        name ? name : "(null)", iid, (void*)out, _ReturnAddress());
    if (!out) return E_POINTER;
    ScDsFile* f = (ScDsFile*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(ScDsFile));
    if (!f) return E_OUTOFMEMORY;
    f->vtable = g_dsFileVtable;
    f->ref = 1;
    if (name) {
        strncpy_s(f->name, sizeof(f->name), name, _TRUNCATE);
    }
    // The device is backed by the measured pack (shaders.rpf, see
    // kDsMeasuredPack): ready for the queue's file-sourced pages, so the
    // measured slot-3 readiness poll can report true instead of spinning.
    f->open = 1;
    ++g_dsFiles;
    *out = f;
    Log("dstorage",
        "  opened file object %p (name=%s, open=%d, files=%d) -> S_OK; "
        "backed by %s",
        (void*)f, f->name, f->open, g_dsFiles, ScDsPackPath());
    return S_OK;
}

extern "C" HRESULT StubStorageFactorySlot10(void* self, void* desc,
                                            void* iid, void** out) {
    Log("dstorage",
        "IDStorageFactory slot 10 CreateQueue this=%p desc=%p iid=%p out=%p "
        "return-to=%p",
        self, desc, iid, (void*)out, _ReturnAddress());
    if (desc) {
        const uint64_t* d = (const uint64_t*)desc;
        Log("dstorage", "  queue desc: %016llx %016llx %016llx %016llx",
            (unsigned long long)d[0], (unsigned long long)d[1],
            (unsigned long long)d[2], (unsigned long long)d[3]);
    }
    if (iid) {
        const uint8_t* g = (const uint8_t*)iid;
        Log("dstorage", "  riid: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10],
            g[11], g[12], g[13], g[14], g[15]);
    }
    if (!out) return E_POINTER;
    ScDsQueue* q = ScDsCreateQueue("factory slot 10");
    if (!q) return E_OUTOFMEMORY;
    *out = q;
    Log("dstorage", "  wrote queue %p to out %p", (void*)q, (void*)out);
    return S_OK;
}

// IDStorageFactory keeps its own vtable so slot 10 is the real CreateQueue
// while every other stub-backed interface keeps StubComGeneric.
static void* g_stubComVtableStorageFactory[64];

struct StubComVtableInit {
    StubComVtableInit() {
        g_stubComVtable[0] = (void*)StubComQueryInterface;
        g_stubComVtable[1] = (void*)StubComAddRef;
        g_stubComVtable[2] = (void*)StubComRelease;
        for (int i = 3; i < 64; ++i)
            g_stubComVtable[i] = (void*)StubComGeneric;
        for (int i = 0; i < 64; ++i)
            g_stubComVtableStorageFactory[i] = g_stubComVtable[i];
        g_stubComVtableStorageFactory[5] = (void*)StubStorageFactorySlot5;
        g_stubComVtableStorageFactory[10] =
            (void*)StubStorageFactorySlot10;
    }
};
static StubComVtableInit g_stubComVtableInit;
static void* MakeStubComObject(const char* tag) {
    StubComObject* obj = (StubComObject*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StubComObject));
    if (!obj) return nullptr;
    obj->vtable = (strcmp(tag, "IDStorageFactory") == 0)
                      ? g_stubComVtableStorageFactory
                      : g_stubComVtable;
    obj->ref = 1;
    Log("stub", "%s stub COM object %p (vtable %p)", tag, (void*)obj,
        (void*)obj->vtable);
    return (void*)obj;
}

extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags,
                                    DWORD proc) {
    Log("audio", "XAudio2Create(flags 0x%x, proc %lu) -> S_OK (STUB)",
        flags, proc);
    if (ppXAudio2) *ppXAudio2 = MakeStubComObject("IXAudio2");
    return S_OK;
}

extern "C" HRESULT HleGameInputCreate(void** ppGameInput) {
    Log("input", "GameInputCreate() -> S_OK (STUB)");
    if (ppGameInput) *ppGameInput = MakeStubComObject("IGameInput");
    return S_OK;
}

extern "C" HRESULT HleDStorageGetFactory(const void* riid, void** ppFactory) {
    // Real signature is TWO args: (REFIID riid, void** ppFactory) —
    // the desktop DirectStorage API and the Xbox XCRDAPI/dstorage_xs
    // versions all take (riid, ppv). Boot #11 proved the 1-arg stub
    // was wrong: the game passes riid in rcx and ppFactory in rdx;
    // the old stub wrote the factory pointer to *rcx (corrupting the
    // riid in .rdata — boot #10's write AV) and never filled the real
    // out-param, so the game's factory global stayed NULL and it hit
    // a fatal int3 assert (boot #11, 0x141d8e505). The riid is logged
    // and ignored; the stub factory serves any IID.
    (void)riid;
    Log("dstorage", "DStorageGetFactory(riid=%p) -> S_OK (STUB, no queues yet)",
        riid);
    if (ppFactory) *ppFactory = MakeStubComObject("IDStorageFactory");
    return S_OK;
}


 // ---- real-exe import stubs (grounded 2026-11-23 from the exe's
// import table) ----
// Generic S_OK / void stubs for real-exe imports whose signatures are
// GDKX-gated (chad, xg_xs, XboxBvhBuilder, FFX loader, libHttpClient,
// xapu, Bink). Each stub logs its own name and returns the safe default
// for its return type: S_OK (0) for HRESULT / int / BOOL (0 = FFX_OK /
// FALSE / no results), nothing for void. The stubs ignore their args —
// on x64 the caller cleans the stack, so arg count does not matter, and
// a stub that returns S_OK without touching out-params is safe as long
// as the game treats S_OK as "call succeeded" (it does; a failure
// HRESULT would stop the boot). The loader's resolution log records
// which import resolved to which stub, so the boot log shows every stub
// call by name.
#define SC_DEFINE_SOK_STUB(tag, fn) \
    extern "C" HRESULT fn() { \
        Log(tag, #fn "() -> S_OK (STUB)"); \
        return S_OK; \
    }
#define SC_DEFINE_VOID_STUB(tag, fn) \
    extern "C" void fn() { Log(tag, #fn "() (STUB)"); }

// chad.dll — S_OK stubs (audio DSP / texture computer / FSR loader / audio APU).
SC_DEFINE_SOK_STUB("chad", XDspActivate)
SC_DEFINE_SOK_STUB("chad", XDspConnectWithMaximumStreamLimit)
SC_DEFINE_SOK_STUB("chad", XDspDeactivate)
SC_DEFINE_SOK_STUB("chad", XDspDisconnect)
SC_DEFINE_SOK_STUB("chad", XDspSubmitCommand)

// xg_xs.dll — NOT stubs as of build 80: src/xg_texture.cpp implements the
// texture address computer (object + vtable + slot-8 validated copy) and
// the swizzle-mode suggestion. Boot #60 crashed on the old S_OK stub: the
// exe ignored the HRESULT and called slot 8 through a null object at
// 0x1416F71FB. A stub that returns S_OK with a null out-param is exactly
// the unsafe-out-param case the build-80 audit (docs/XG-AUDIT.md) lists.

// amd_fidelityfx_loader_xbox.dll — S_OK stubs (audio DSP / texture computer / FSR loader / audio APU).
SC_DEFINE_SOK_STUB("ffx", ffxCreateContext)
SC_DEFINE_SOK_STUB("ffx", ffxDestroyContext)
SC_DEFINE_SOK_STUB("ffx", ffxDispatch)
SC_DEFINE_SOK_STUB("ffx", ffxQuery)

// xapu.dll — S_OK stubs (audio DSP / texture computer / FSR loader / audio APU).
SC_DEFINE_SOK_STUB("xapu", XApuConnect)
SC_DEFINE_SOK_STUB("xapu", XApuDequeueResult)
SC_DEFINE_SOK_STUB("xapu", XApuDisconnect)
SC_DEFINE_SOK_STUB("xapu", XApuEnqueueCommand)
SC_DEFINE_SOK_STUB("xapu", XApuGetResultCount)

// libHttpClient.GDK.dll — S_OK stubs (HTTP client; the game does no HTTP at boot, but the imports must resolve).
SC_DEFINE_SOK_STUB("http", HCCleanupAsync)
SC_DEFINE_SOK_STUB("http", HCHttpCallCloseHandle)
SC_DEFINE_SOK_STUB("http", HCHttpCallCreate)
SC_DEFINE_SOK_STUB("http", HCHttpCallDuplicateHandle)
SC_DEFINE_SOK_STUB("http", HCHttpCallGetRequestUrl)
SC_DEFINE_SOK_STUB("http", HCHttpCallPerformAsync)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetHeaderAtIndex)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetNumHeaders)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetRequestBodyBytes)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetRetryAllowed)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetRetryCacheId)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetRetryDelay)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetTimeout)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetTimeoutWindow)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestGetUrl)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetHeader)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetRequestBodyBytes)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetRequestBodyString)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetRetryAllowed)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetRetryCacheId)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetRetryDelay)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetTimeout)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetTimeoutWindow)
SC_DEFINE_SOK_STUB("http", HCHttpCallRequestSetUrl)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetHeader)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetHeaderAtIndex)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetNetworkErrorCode)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetNumHeaders)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetPlatformNetworkErrorMessage)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetResponseBodyBytes)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetResponseBodyBytesSize)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetResponseString)
SC_DEFINE_SOK_STUB("http", HCHttpCallResponseGetStatusCode)
SC_DEFINE_SOK_STUB("http", HCHttpCallSetTracing)
SC_DEFINE_SOK_STUB("http", HCInitialize)
SC_DEFINE_SOK_STUB("http", HCSettingsGetTraceLevel)
SC_DEFINE_SOK_STUB("http", HCTraceImplMessage)
SC_DEFINE_SOK_STUB("http", HCTraceSetEtwEnabled)

// Bink2Scarlett.dll — video playback stubs. BinkOpenWithOptions returns a dummy non-NULL handle so the game proceeds to call the (void) frame functions; BinkGetKeyFrame / BinkShouldSkip return 0 (no key frame / do not skip).
SC_DEFINE_VOID_STUB("bink", BinkClose)
SC_DEFINE_VOID_STUB("bink", BinkDoFrame)
SC_DEFINE_VOID_STUB("bink", BinkDoFrameAsyncMulti)
SC_DEFINE_VOID_STUB("bink", BinkDoFrameAsyncWait)
SC_DEFINE_VOID_STUB("bink", BinkFreeGlobals)
SC_DEFINE_VOID_STUB("bink", BinkGetFrameBuffersInfo)
SC_DEFINE_VOID_STUB("bink", BinkGetPlatformInfo)
SC_DEFINE_VOID_STUB("bink", BinkGoto)
SC_DEFINE_VOID_STUB("bink", BinkNextFrame)
SC_DEFINE_VOID_STUB("bink", BinkPause)
SC_DEFINE_VOID_STUB("bink", BinkRegisterFrameBuffers)
SC_DEFINE_VOID_STUB("bink", BinkRequestStopAsyncThreadsMulti)
SC_DEFINE_VOID_STUB("bink", BinkSetMemory)
SC_DEFINE_VOID_STUB("bink", BinkSetOSFileCallbacks)
SC_DEFINE_VOID_STUB("bink", BinkSetSoundSystem)
SC_DEFINE_VOID_STUB("bink", BinkSetVolume)
SC_DEFINE_VOID_STUB("bink", BinkSetWillLoop)
SC_DEFINE_VOID_STUB("bink", BinkStartAsyncThread)
SC_DEFINE_VOID_STUB("bink", BinkWait)
SC_DEFINE_VOID_STUB("bink", BinkWaitStopAsyncThreadsMulti)
SC_DEFINE_SOK_STUB("bink", BinkGetKeyFrame)
SC_DEFINE_SOK_STUB("bink", BinkShouldSkip)

extern "C" void* BinkOpenWithOptions() {
    Log("bink",
        "BinkOpenWithOptions() -> 0x42494E4B (STUB: dummy handle, no "
        "video decode)");
    return (void*)0x42494E4BULL;  // "BINK"
}


 // XboxBvhBuilder.dll — DXR BVH builder (C++ methods; mangled export
// names). Return types are read off the mangled names: QEAAX = void,
// QEAAJ = HRESULT (long). The ctor takes (ID3D12Device11*, bool) and
// does nothing; the methods are no-ops (no RT at boot). Rehydrate
// returns S_OK.
extern "C" void HleXboxBvhBuilderCtor() {
    Log("bvh", "XboxBvhBuilder::XboxBvhBuilder (ctor) (STUB)");
}
extern "C" void HleXboxBvhBuilderDtor() {
    Log("bvh", "XboxBvhBuilder::~XboxBvhBuilder (dtor) (STUB)");
}
extern "C" void HleXboxBvhBuilderBuildBatched2X() {
    Log("bvh",
        "XboxBvhBuilder::BuildRaytracingAccelerationStructureBatched2X "
        "(STUB)");
}
extern "C" void HleXboxBvhBuilderCopy() {
    Log("bvh",
        "XboxBvhBuilder::CopyRaytracingAccelerationStructure (STUB)");
}
extern "C" void HleXboxBvhBuilderGetPrebuildInfo() {
    Log("bvh",
        "XboxBvhBuilder::GetRaytracingAccelerationStructurePrebuildInfo "
        "(STUB)");
}
extern "C" HRESULT HleXboxBvhBuilderRehydrate2X() {
    Log("bvh",
        "XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatched2X "
        "-> S_OK (STUB)");
    return S_OK;
}
extern "C" void HleXboxBvhBuilderSet64BitMortonCodes() {
    Log("bvh", "XboxBvhBuilder::Set64BitMortonCodes (STUB)");
}

extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags) {
    if (interval == 0) interval = 1;
    g_frameInterval = interval;
    // Raise the system timer resolution once (phase 3): at the default
    // 15.6 ms clock tick, a waitable timer set for 16.7 ms can fire up
    // to a full tick late (measured 27.7 ms). timeBeginPeriod(1) makes
    // the timer fire within ~1 ms of the due time. Games do this
    // routinely; the resolution is process-wide.
    static bool timerResRaised = false;
    if (!timerResRaised) {
        // Build 88: a background/eco-throttled process has its timer
        // resolution requests ignored by Windows (timeBeginPeriod(1)
        // returns 0 but Sleep still rounds to 15.625 ms), which made
        // every guest frame 62.5 ms instead of 50 ms. Opt this process
        // out of execution-speed power throttling before raising the
        // resolution.
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
        PROCESS_POWER_THROTTLING_STATE pts = {};
        pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        // EXECUTION_SPEED and IGNORE_TIMER_RESOLUTION both off: Windows
        // applies the latter to background processes, which is why
        // NtSetTimerResolution reported "granted 15.625 ms" here while a
        // normal process gets 0.5 ms.
        pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4;
        pts.StateMask = 0;  // 0 = throttling off
        BOOL pt = SetProcessInformation(GetCurrentProcess(),
                                        ProcessPowerThrottling, &pts,
                                        sizeof(pts));
        Log("present", "power throttling off -> ok=%d err=%lu", (int)pt,
            (unsigned long)(pt ? 0 : GetLastError()));
#endif
        // Call through winmm's export table rather than the linked
        // import: the loader rewrites imports in this process, and a
        // rewritten timeBeginPeriod would return success without ever
        // reaching the OS.
        typedef UINT(WINAPI * PfnTimeBeginPeriod)(UINT);
        HMODULE winmm = LoadLibraryA("winmm.dll");
        PfnTimeBeginPeriod beginPeriod =
            winmm ? (PfnTimeBeginPeriod)(void*)GetProcAddress(
                        winmm, "timeBeginPeriod")
                  : nullptr;
        UINT rc = beginPeriod ? beginPeriod(1) : 0xFFFFFFFFu;
        Log("present",
            "timeBeginPeriod(1) via %s -> %u (0 = raised to 1 ms)",
            beginPeriod ? "GetProcAddress" : "unavailable", (unsigned)rc);
        // Build 88 probe: read back the timer resolution the process
        // actually has (100 ns units; 156250 = 15.625 ms tick).
        typedef LONG(NTAPI * PfnQueryTimerRes)(ULONG*, ULONG*, ULONG*);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        PfnQueryTimerRes q = ntdll ? (PfnQueryTimerRes)(void*)GetProcAddress(
                                         ntdll, "NtQueryTimerResolution")
                                   : nullptr;
        // The Win32 wrapper can be a no-op in this process; the native
        // call is the mechanism underneath. 5000 = 0.5 ms in 100 ns
        // units (the finest the platform offers).
        typedef LONG(NTAPI * PfnSetTimerRes)(ULONG, BOOLEAN, PULONG);
        HMODULE nt2 = GetModuleHandleA("ntdll.dll");
        PfnSetTimerRes setRes =
            nt2 ? (PfnSetTimerRes)(void*)GetProcAddress(
                      nt2, "NtSetTimerResolution")
                : nullptr;
        if (setRes) {
            ULONG want = 5000, got = 0;
            LONG s2 = setRes(want, TRUE, &got);
            Log("present",
                "NtSetTimerResolution(5000) -> 0x%lx, granted %lu (%.3f ms)",
                (long)s2, (unsigned long)got, (double)got / 10000.0);
        }
        if (q) {
            ULONG mn = 0, mx = 0, cur = 0;
            LONG st = q(&mn, &mx, &cur);
            Log("present",
                "timer resolution now: status 0x%lx coarse %.3f ms fine "
                "%.3f ms current %.3f ms",
                (long)st, (double)mn / 10000.0, (double)mx / 10000.0,
                (double)cur / 10000.0);
        }
        if (rc == TIMERR_NOERROR) timerResRaised = true;
    }
    Log("present",
        "SetFrameIntervalX(%u, 0x%x) -> S_OK (real pacing: %.1f ms/frame)",
        interval, flags, FramePeriodMs());
    return S_OK;
}

extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId) {
    g_lastFrameId = frameId;
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_haveFrame = true;
    // Build 88 probe: the frame QPC is stamped here, so any time this
    // function spends after the stamp is charged to the guest's frame.
    // Track the worst schedule cost and report it every 32 frames.
    LARGE_INTEGER freq, after;
    QueryPerformanceFrequency(&freq);
    Log("present", "ScheduleFrameEventX(%llu) -> S_OK", 
        (unsigned long long)frameId);
    QueryPerformanceCounter(&after);
    {
        static double maxMs = 0.0;
        static unsigned calls = 0;
        double ms = (double)(after.QuadPart - g_lastFrameQpc.QuadPart) *
                    1000.0 / (double)freq.QuadPart;
        if (ms > maxMs) maxMs = ms;
        if (++calls % 32 == 0) {
            Log("present",
                "ScheduleFrameEventX post-stamp cost: last %.2f ms, "
                "max %.2f ms over %u frames", ms, maxMs, calls);
        }
    }
    return S_OK;
}

extern "C" HRESULT HleWaitFrameEventX(UINT64 frameId, DWORD timeoutMs) {
    if (!g_haveFrame || frameId != g_lastFrameId) {
        Log("present",
            "WaitFrameEventX(%llu) -> S_OK (no such frame scheduled)",
            (unsigned long long)frameId);
        return S_OK;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) *
                       1000.0 / (double)freq.QuadPart;
    double remainMs = FramePeriodMs() - elapsedMs;
    double waitMs = remainMs < (double)timeoutMs ? remainMs
                                                 : (double)timeoutMs;
    if (waitMs > 0) {
        // Two-phase wait (phase 3): the waitable timer covers all but
        // the last ~1.5 ms (it fires at the system clock granularity),
        // then a QPC spin covers the tail so the wait lands on the
        // deadline instead of overshooting by a tick. The spin is
        // bounded to ~2 ms.
        double timerMs = waitMs - 1.5;
        if (timerMs > 0) {
            HANDLE timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(timerMs * 10000.0);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr,
                                     FALSE))
                    WaitForSingleObject(timer, (DWORD)(timerMs + 100) + 100);
                CloseHandle(timer);
            }
        }
        LARGE_INTEGER deadline;
        deadline.QuadPart = now.QuadPart +
                            (LONGLONG)(waitMs * (double)freq.QuadPart /
                                       1000.0);
        LARGE_INTEGER spin;
        do {
            QueryPerformanceCounter(&spin);
        } while (spin.QuadPart < deadline.QuadPart);
        QueryPerformanceCounter(&now);
        double actualMs = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) *
                          1000.0 / (double)freq.QuadPart;
        Log("present",
            "WaitFrameEventX(%llu) waited ~%.1f ms (period %.1f ms, "
            "timeout %lu ms)",
            (unsigned long long)frameId, actualMs, FramePeriodMs(),
            timeoutMs);
    } else {
        Log("present",
            "WaitFrameEventX(%llu) already past deadline (%.1f ms elapsed)",
            (unsigned long long)frameId, elapsedMs);
    }
    // Fixed-timestep pump (see docs/FIXED-TIMESTEP.md): every frame
    // event the title waits on advances the physics accumulator and
    // steps the registered physics function at the fixed rate — so
    // physics runs at 60 Hz even when the render loop runs at 30 or
    // 120 FPS. The pump is a no-op (and accumulates no time) while
    // no step callback is registered, so existing guests are
    // unaffected. It does NOT run on the early "no such frame"
    // return above — a wait for a frame that was never scheduled is
    // not a frame boundary.
    ScFixedTimestep_Pump(nullptr);
    return S_OK;
}

// ---- XAsync (phase 2, milestone E) ----
// Synchronous completion: the provider does the work inline and completes
// the block before returning, so XAsyncGetStatus reports the final
// HRESULT immediately. internal[0] = HRESULT, [1] = result size,
// [2] = result pointer, [3] = completed flag.

static void CompleteAsync(XAsyncBlock* async, HRESULT hr) {
    async->internal[0] = (void*)(intptr_t)hr;
    async->internal[3] = (void*)1;
    if (async->callback) async->callback(async);
}

static bool AsyncIsComplete(const XAsyncBlock* async) {
    return async->internal[3] != nullptr;
}

extern "C" HRESULT HleXAsyncGetStatus(XAsyncBlock* async, BOOL wait) {
    (void)wait;  // synchronous model: nothing to wait on
    if (!async) return E_INVALIDARG;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    Log("xasync", "XAsyncGetStatus() -> 0x%08x (%s)", (unsigned)hr,
        AsyncIsComplete(async) ? "complete" : "pending");
    return AsyncIsComplete(async) ? hr : E_PENDING;
}

extern "C" HRESULT HleXAsyncGetResultSize(XAsyncBlock* async, size_t* size) {
    if (!async || !size) return E_INVALIDARG;
    *size = (size_t)async->internal[1];
    Log("xasync", "XAsyncGetResultSize() -> %zu", *size);
    return S_OK;
}

extern "C" HRESULT HleXAsyncCancel(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    if (!AsyncIsComplete(async))
        CompleteAsync(async, E_ABORT);
    Log("xasync", "XAsyncCancel() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXAsyncRun(XAsyncBlock* async, XAsyncWork work) {
    if (!async || !work) return E_INVALIDARG;
    work(async);
    if (!AsyncIsComplete(async)) CompleteAsync(async, S_OK);
    Log("xasync", "XAsyncRun() -> S_OK");
    return S_OK;
}

// ---- XUser (phase 2, milestone E) ----
// Simplified user model: a single synthetic local default user. No real
// Xbox Live sign-in; interactive options are rejected with E_INVALIDARG.

struct XUserImpl {
    uint64_t xuid;
    uint64_t localId;
    char gamertag[16];
};

static XUserImpl g_defaultUser = {0x0009000000000001ULL, 1, "Player"};

extern "C" HRESULT HleXUserAddAsync(XUserAddOptions options,
                                     XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    // Simplified model: only silent works; interactive is rejected.
    if (options & (XUserAddOptions_AddDefaultUserAllowingUI |
                   XUserAddOptions_AllowGuests)) {
        Log("xuser",
            "XUserAddAsync(options 0x%x) -> E_INVALIDARG (interactive "
            "sign-in not supported)",
            (unsigned)options);
        return E_INVALIDARG;
    }
    async->internal[2] = &g_defaultUser;
    async->internal[1] = (void*)sizeof(void*);
    CompleteAsync(async, S_OK);
    Log("xuser", "XUserAddAsync(options 0x%x) -> S_OK (default user)",
        (unsigned)options);
    return S_OK;
}

extern "C" HRESULT HleXUserAddResult(XAsyncBlock* async, XUserHandle* user) {
    if (!async || !user) return E_INVALIDARG;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) {
        Log("xuser", "XUserAddResult() -> 0x%08x", (unsigned)hr);
        return hr;
    }
    *user = (XUserHandle)async->internal[2];
    Log("xuser", "XUserAddResult() -> S_OK user=%p", *user);
    return S_OK;
}

extern "C" HRESULT HleXUserGetId(XUserHandle user, uint64_t* xuid) {
    if (!user || !xuid) return E_INVALIDARG;
    *xuid = ((XUserImpl*)user)->xuid;
    Log("xuser", "XUserGetId() -> 0x%llx", (unsigned long long)*xuid);
    return S_OK;
}

extern "C" HRESULT HleXUserGetGamertag(XUserHandle user,
                                        XUserGamertagOptions options,
                                        char* gamertag, size_t* gamertagSize) {
    (void)options;  // UniqueModern and Classic both return "Player" here.
    if (!user || !gamertagSize) return E_INVALIDARG;
    const char* tag = ((XUserImpl*)user)->gamertag;
    size_t need = strlen(tag) + 1;
    if (!gamertag) {
        *gamertagSize = need;
        return S_OK;
    }
    if (*gamertagSize < need) {
        *gamertagSize = need;
        Log("xuser", "XUserGetGamertag() -> E_NOT_SUFFICIENT_BUFFER");
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(gamertag, *gamertagSize, tag);
    *gamertagSize = need;
    Log("xuser", "XUserGetGamertag() -> \"%s\"", tag);
    return S_OK;
}

extern "C" HRESULT HleXUserGetLocalId(XUserHandle user, XUserLocalId* localId) {
    if (!user || !localId) return E_INVALIDARG;
    localId->value = ((XUserImpl*)user)->localId;
    Log("xuser", "XUserGetLocalId() -> %llu",
        (unsigned long long)localId->value);
    return S_OK;
}

extern "C" HRESULT HleXUserCloseHandle(XUserHandle user) {
    if (!user) return E_INVALIDARG;
    // The default user is a static singleton — nothing to free.
    Log("xuser", "XUserCloseHandle(%p) -> S_OK", user);
    return S_OK;
}

// ---- XGameSave (phase 2, milestone E) ----
// Local-only provider rooted at
// %LOCALAPPDATA%\ScarlettCompat\XGameSave\<SCID>\. No cloud sync.

struct XGameSaveProviderImpl {
    char rootPath[MAX_PATH];
};

struct XGameSaveContainerImpl {
    char path[MAX_PATH];
};

static bool MakeDirs(const char* path) {
    // Create every component (CreateDirectory is not recursive).
    char tmp[MAX_PATH];
    if (strlen(path) >= sizeof(tmp)) return false;
    strcpy_s(tmp, sizeof(tmp), path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = 0;
            CreateDirectoryA(tmp, nullptr);
            *p = save;
        }
    }
    return CreateDirectoryA(tmp, nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

extern "C" HRESULT HleXGameSaveInitializeProvider(XUserHandle user,
                                                   const char* scid,
                                                   BOOL allowSynchronous,
                                                   XGameSaveProviderHandle* provider) {
    (void)allowSynchronous;  // always synchronous / local here
    if (!provider || !scid || !*scid) return E_INVALIDARG;
    // Null user = machine-only provider (local only). A real user is
    // accepted but the save is still local (no cloud sync).
    char localAppData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                      sizeof(localAppData));
    char root[MAX_PATH];
    if (n > 0 && n < sizeof(localAppData)) {
        snprintf(root, sizeof(root), "%s\\ScarlettCompat\\XGameSave\\%s",
                 localAppData, scid);
    } else {
        snprintf(root, sizeof(root), ".\\ScarlettCompat\\XGameSave\\%s",
                 scid);
    }
    if (!MakeDirs(root)) {
        Log("xgamesave", "XGameSaveInitializeProvider() -> FAILED to create %s",
            root);
        return E_FAIL;
    }
    XGameSaveProviderImpl* p = (XGameSaveProviderImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
    if (!p) return E_OUTOFMEMORY;
    strcpy_s(p->rootPath, sizeof(p->rootPath), root);
    *provider = p;
    Log("xgamesave",
        "XGameSaveInitializeProvider(user=%p, scid=%s) -> S_OK root=%s",
        user, scid, root);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (!provider) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, provider);
    Log("xgamesave", "XGameSaveCloseProvider() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCreateContainer(XGameSaveProviderHandle provider,
                                                const char* displayName,
                                                XGameSaveContainerHandle* container) {
    if (!provider || !displayName || !*displayName || !container)
        return E_INVALIDARG;
    XGameSaveProviderImpl* p = (XGameSaveProviderImpl*)provider;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", p->rootPath, displayName);
    if (!MakeDirs(path)) {
        Log("xgamesave", "XGameSaveCreateContainer(%s) -> FAILED", displayName);
        return E_FAIL;
    }
    XGameSaveContainerImpl* c = (XGameSaveContainerImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    strcpy_s(c->path, sizeof(c->path), path);
    *container = c;
    Log("xgamesave", "XGameSaveCreateContainer(%s) -> S_OK path=%s",
        displayName, path);
    return S_OK;
}

// ---- XGameSave update flow (phase 3) ----
// An update stages blob writes/deletes in memory; SubmitUpdate applies
// them to the container folder. Total staged size is capped at
// GS_MAX_BLOB_SIZE (16 MB, GDK docs); one action per blob per update
// (GDK rule); an update cannot be reused after submit (GDK rule).

#define E_GS_UPDATE_TOO_BIG ((HRESULT)0x80830005)

struct XGameSaveUpdateImpl {
    XGameSaveContainerImpl* container;
    char displayName[256];
    struct Action {
        bool isDelete;
        char blobName[256];
        uint8_t* data;  // null for deletes
        size_t size;
    };
    std::vector<Action> actions;
    size_t totalBytes = 0;
    bool submitted = false;
};

static bool ValidBlobName(const char* name) {
    if (!name || !*name || strlen(name) >= 256) return false;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
            return false;
    }
    return true;
}

static XGameSaveUpdateImpl::Action* FindUpdateAction(XGameSaveUpdateImpl* u,
                                                     const char* blobName) {
    for (auto& a : u->actions)
        if (strcmp(a.blobName, blobName) == 0) return &a;
    return nullptr;
}

static bool WriteFileAtomic(const char* path, const uint8_t* data,
                            size_t size) {
    // Write to a temp file in the same folder, then rename over the
    // target — per-blob atomic. (Whole-update atomicity would need a
    // journal; the local model documents this as best-effort.)
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%lu", path, GetCurrentProcessId());
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = TRUE;
    if (size > 0)
        ok = WriteFile(h, data, (DWORD)size, &written, nullptr) &&
             written == size;
    CloseHandle(h);
    if (!ok) {
        DeleteFileA(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

extern "C" HRESULT HleXGameSaveCreateUpdate(
    XGameSaveContainerHandle container, const char* containerDisplayName,
    XGameSaveUpdateHandle* update) {
    if (!container || !containerDisplayName || !*containerDisplayName ||
        !update)
        return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*u));
    if (!u) return E_OUTOFMEMORY;
    u->container = (XGameSaveContainerImpl*)container;
    strcpy_s(u->displayName, sizeof(u->displayName), containerDisplayName);
    *update = u;
    Log("xgamesave", "XGameSaveCreateUpdate(%s) -> S_OK",
        containerDisplayName);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update,
                                               const char* blobName,
                                               const uint8_t* data,
                                               size_t byteCount) {
    if (!update || !ValidBlobName(blobName) || (!data && byteCount != 0))
        return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    if (u->submitted) return E_INVALIDARG;  // cannot reuse after submit
    if (FindUpdateAction(u, blobName)) {
        Log("xgamesave",
            "XGameSaveSubmitBlobWrite(%s) -> E_INVALIDARG (already an "
            "action for this blob in this update)",
            blobName);
        return E_INVALIDARG;
    }
    if (byteCount > GS_MAX_BLOB_SIZE ||
        u->totalBytes + byteCount > GS_MAX_BLOB_SIZE) {
        Log("xgamesave", "XGameSaveSubmitBlobWrite(%s, %zu) -> "
                         "E_GS_UPDATE_TOO_BIG",
            blobName, byteCount);
        return E_GS_UPDATE_TOO_BIG;
    }
    uint8_t* copy = nullptr;
    if (byteCount > 0) {
        copy = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, byteCount);
        if (!copy) return E_OUTOFMEMORY;
        memcpy(copy, data, byteCount);
    }
    XGameSaveUpdateImpl::Action a = {};
    a.isDelete = false;
    strcpy_s(a.blobName, sizeof(a.blobName), blobName);
    a.data = copy;
    a.size = byteCount;
    u->actions.push_back(a);
    u->totalBytes += byteCount;
    Log("xgamesave", "XGameSaveSubmitBlobWrite(%s, %zu bytes) -> S_OK "
                     "(staged, total %zu)",
        blobName, byteCount, u->totalBytes);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update,
                                                const char* blobName) {
    if (!update || !ValidBlobName(blobName)) return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    if (u->submitted) return E_INVALIDARG;
    if (FindUpdateAction(u, blobName)) {
        Log("xgamesave",
            "XGameSaveSubmitBlobDelete(%s) -> E_INVALIDARG (already an "
            "action for this blob in this update)",
            blobName);
        return E_INVALIDARG;
    }
    XGameSaveUpdateImpl::Action a = {};
    a.isDelete = true;
    strcpy_s(a.blobName, sizeof(a.blobName), blobName);
    u->actions.push_back(a);
    Log("xgamesave", "XGameSaveSubmitBlobDelete(%s) -> S_OK (staged)",
        blobName);
    return S_OK;
}

static HRESULT ApplyGameSaveUpdate(XGameSaveUpdateImpl* u) {
    if (!u || u->submitted) return E_INVALIDARG;
    for (auto& a : u->actions) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", u->container->path,
                 a.blobName);
        if (a.isDelete) {
            if (!DeleteFileA(path) &&
                GetLastError() != ERROR_FILE_NOT_FOUND) {
                Log("xgamesave",
                    "XGameSaveSubmitUpdate: delete %s failed (%lu)",
                    a.blobName, GetLastError());
                return E_FAIL;
            }
            Log("xgamesave", "XGameSaveSubmitUpdate: deleted blob %s",
                a.blobName);
        } else {
            if (!WriteFileAtomic(path, a.data, a.size)) {
                Log("xgamesave",
                    "XGameSaveSubmitUpdate: write %s failed (%lu)",
                    a.blobName, GetLastError());
                return E_FAIL;
            }
            Log("xgamesave", "XGameSaveSubmitUpdate: wrote blob %s (%zu "
                             "bytes)",
                a.blobName, a.size);
        }
    }
    u->submitted = true;
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitUpdate(XGameSaveUpdateHandle update) {
    if (!update) return E_INVALIDARG;
    HRESULT hr = ApplyGameSaveUpdate((XGameSaveUpdateImpl*)update);
    Log("xgamesave", "XGameSaveSubmitUpdate() -> 0x%08x", (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update,
                                                 XAsyncBlock* async) {
    if (!update || !async) return E_INVALIDARG;
    HRESULT hr = ApplyGameSaveUpdate((XGameSaveUpdateImpl*)update);
    CompleteAsync(async, hr);
    Log("xgamesave", "XGameSaveSubmitUpdateAsync() -> 0x%08x (sync "
                     "completion)",
        (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    Log("xgamesave", "XGameSaveSubmitUpdateResult() -> 0x%08x",
        (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveCloseUpdate(XGameSaveUpdateHandle update) {
    if (!update) return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    for (auto& a : u->actions)
        if (a.data) HeapFree(GetProcessHeap(), 0, a.data);
    HeapFree(GetProcessHeap(), 0, u);
    Log("xgamesave", "XGameSaveCloseUpdate() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCloseContainer(
    XGameSaveContainerHandle container) {
    if (!container) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, container);
    Log("xgamesave", "XGameSaveCloseContainer() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveReadBlobData(XGameSaveContainerHandle container,
                                            const char* blobName,
                                            size_t* bytesRead,
                                            size_t bufferSize,
                                            uint8_t* buffer) {
    if (!container || !ValidBlobName(blobName) || !bytesRead)
        return E_INVALIDARG;
    XGameSaveContainerImpl* c = (XGameSaveContainerImpl*)container;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", c->path, blobName);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("xgamesave", "XGameSaveReadBlobData(%s) -> file not found",
            blobName);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(h, &fsize)) {
        CloseHandle(h);
        return E_FAIL;
    }
    *bytesRead = (size_t)fsize.QuadPart;
    if (bufferSize < *bytesRead) {
        CloseHandle(h);
        Log("xgamesave",
            "XGameSaveReadBlobData(%s) -> E_NOT_SUFFICIENT_BUFFER "
            "(need %zu, have %zu)",
            blobName, *bytesRead, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (*bytesRead > 0) {
        if (!buffer) {
            CloseHandle(h);
            return E_INVALIDARG;
        }
        DWORD read = 0;
        if (!ReadFile(h, buffer, (DWORD)*bytesRead, &read, nullptr) ||
            read != *bytesRead) {
            CloseHandle(h);
            return E_FAIL;
        }
    }
    CloseHandle(h);
    Log("xgamesave", "XGameSaveReadBlobData(%s) -> S_OK %zu bytes",
        blobName, *bytesRead);
    return S_OK;
}

// ---- XGameSaveFiles (phase 3) ----
// File-style saves: the async call returns the provider folder; the
// title uses plain Win32 FileIO under it. Root:
// %LOCALAPPDATA%\ScarlettCompat\XGameSaveFiles\<SCID>\. No UI is shown
// headless; no cloud sync (local only).

extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser, const char* configurationId,
    XAsyncBlock* async) {
    (void)requestingUser;  // accepted; the folder is per-SCID, not per-user
    if (!async || !configurationId || !*configurationId)
        return E_INVALIDARG;
    char localAppData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                      sizeof(localAppData));
    char root[MAX_PATH];
    if (n > 0 && n < sizeof(localAppData)) {
        snprintf(root, sizeof(root),
                 "%s\\ScarlettCompat\\XGameSaveFiles\\%s", localAppData,
                 configurationId);
    } else {
        snprintf(root, sizeof(root),
                 ".\\ScarlettCompat\\XGameSaveFiles\\%s", configurationId);
    }
    if (!MakeDirs(root)) {
        Log("xgamesavefiles",
            "XGameSaveFilesGetFolderWithUiAsync() -> FAILED to create %s",
            root);
        return E_FAIL;
    }
    size_t len = strlen(root) + 1;
    char* stored = (char*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!stored) return E_OUTOFMEMORY;
    strcpy_s(stored, len, root);
    async->internal[2] = stored;
    async->internal[1] = (void*)len;
    CompleteAsync(async, S_OK);
    Log("xgamesavefiles",
        "XGameSaveFilesGetFolderWithUiAsync(scid=%s) -> S_OK folder=%s",
        configurationId, root);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiResult(
    XAsyncBlock* async, size_t bufferSize, char* folder) {
    if (!async || !folder) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) return hr;
    const char* path = (const char*)async->internal[2];
    if (!path) return E_FAIL;
    size_t need = strlen(path) + 1;
    if (bufferSize < need) {
        Log("xgamesavefiles",
            "XGameSaveFilesGetFolderWithUiResult() -> "
            "E_NOT_SUFFICIENT_BUFFER (need %zu, have %zu)",
            need, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(folder, bufferSize, path);
    HeapFree(GetProcessHeap(), 0, (void*)path);
    async->internal[2] = nullptr;
    Log("xgamesavefiles", "XGameSaveFilesGetFolderWithUiResult() -> \"%s\"",
        folder);
    return S_OK;
}

// ---- XPackage (phase 2, milestone E) ----
// Mount path = the directory holding the guest PE (set by the loader).

static wchar_t g_guestDir[MAX_PATH] = {};

void SetGuestDir(const wchar_t* dir) {
    if (!dir) return;
    wcsncpy_s(g_guestDir, sizeof(g_guestDir) / sizeof(wchar_t), dir,
              _TRUNCATE);
    Log("xpackage", "guest dir set to %ls", g_guestDir);
}

extern "C" HRESULT HleXPackageGetMountPathSize(uint32_t* size) {
    if (!size) return E_INVALIDARG;
    *size = (uint32_t)(wcslen(g_guestDir) + 1);
    Log("xpackage", "XPackageGetMountPathSize() -> %lu", *size);
    return S_OK;
}

extern "C" HRESULT HleXPackageGetMountPath(wchar_t* path, uint32_t* pathSize) {
    if (!pathSize) return E_INVALIDARG;
    uint32_t need = (uint32_t)(wcslen(g_guestDir) + 1);
    if (!path) {
        *pathSize = need;
        return S_OK;
    }
    if (*pathSize < need) {
        *pathSize = need;
        Log("xpackage", "XPackageGetMountPath() -> E_NOT_SUFFICIENT_BUFFER");
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    wcscpy_s(path, *pathSize, g_guestDir);
    *pathSize = need;
    Log("xpackage", "XPackageGetMountPath() -> \"%ls\"", g_guestDir);
    return S_OK;
}

extern "C" BOOL HleXPackageIsPackagedProcess(void) {
    // The layer presents the guest as a packaged Game Core title.
    Log("xpackage", "XPackageIsPackagedProcess() -> TRUE");
    return TRUE;
}

// ---- XPackage details (phase 3) ----
// The layer presents a single synthetic package: the guest itself.
// Identifier is synthetic (local-only, no store install) with a
// placeholder TitleId; it identifies the guest to the layer only.

static const char kPackageId[] = "ScarlettCompat-Guest-00000001";
static const char kPackageDisplayName[] = "ScarlettCompat Guest";
static const char kPackageDescription[] =
    "Synthetic package presented by the ScarlettCompat layer "
    "(local-only, no store install)";
static const char kPackagePublisher[] = "ScarlettCompat";
static const char kPackageTitleId[] = "00000001";

struct XPackageMountImpl {
    char identifier[XPACKAGE_IDENTIFIER_MAX_LENGTH];
    wchar_t path[MAX_PATH];  // mount path = guest dir
};

extern "C" HRESULT HleXPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize, char* buffer) {
    if (!buffer) return E_INVALIDARG;
    size_t need = strlen(kPackageId) + 1;
    if (bufferSize < need) {
        Log("xpackage",
            "XPackageGetCurrentProcessPackageIdentifier() -> "
            "E_NOT_SUFFICIENT_BUFFER (need %zu, have %zu)",
            need, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(buffer, bufferSize, kPackageId);
    Log("xpackage",
        "XPackageGetCurrentProcessPackageIdentifier() -> \"%s\"",
        kPackageId);
    return S_OK;
}

extern "C" HRESULT HleXPackageEnumeratePackages(
    XPackageKind kind, XPackageEnumerationScope scope, void* context,
    XPackageEnumerationCallback* callback) {
    (void)scope;  // one package: every scope returns the same one
    if (!callback) return E_INVALIDARG;
    if (kind == XPackageKind_Content) {
        Log("xpackage",
            "XPackageEnumeratePackages(Content) -> S_OK, 0 packages");
        return S_OK;
    }
    XPackageDetails d = {};
    d.packageIdentifier = kPackageId;
    d.version.major = 1;
    d.version.minor = 0;
    d.version.build = 0;
    d.version.revision = 0;
    d.kind = XPackageKind_Game;
    d.displayName = kPackageDisplayName;
    d.description = kPackageDescription;
    d.publisher = kPackagePublisher;
    d.storeId = "";
    d.installing = false;
    d.index = 0;
    d.count = 1;
    d.ageRestricted = false;
    d.titleID = kPackageTitleId;
    bool cont = (*callback)(context, &d);
    Log("xpackage",
        "XPackageEnumeratePackages(Game) -> S_OK, 1 package \"%s\" "
        "(continue=%d)",
        kPackageId, (int)cont);
    return S_OK;
}

extern "C" HRESULT HleXPackageMountWithUiAsync(
    const char* packageIdentifier, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    // No UI is shown headless. The only package that exists locally is
    // the current one; NULL/empty identifier means the current package.
    if (packageIdentifier && *packageIdentifier &&
        strcmp(packageIdentifier, kPackageId) != 0) {
        Log("xpackage",
            "XPackageMountWithUiAsync(\"%s\") -> E_INVALIDARG (unknown "
            "package)",
            packageIdentifier);
        return E_INVALIDARG;
    }
    XPackageMountImpl* m = (XPackageMountImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*m));
    if (!m) return E_OUTOFMEMORY;
    strcpy_s(m->identifier, sizeof(m->identifier), kPackageId);
    wcsncpy_s(m->path, sizeof(m->path) / sizeof(wchar_t), g_guestDir,
              _TRUNCATE);
    async->internal[2] = m;
    async->internal[1] = (void*)sizeof(void*);
    CompleteAsync(async, S_OK);
    Log("xpackage",
        "XPackageMountWithUiAsync() -> S_OK (mount=%p path=%ls)",
        m, m->path);
    return S_OK;
}

extern "C" HRESULT HleXPackageMountWithUiResult(
    XAsyncBlock* async, XPackageMountHandle* mount) {
    if (!async || !mount) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) {
        Log("xpackage", "XPackageMountWithUiResult() -> 0x%08x",
            (unsigned)hr);
        return hr;
    }
    *mount = (XPackageMountHandle)async->internal[2];
    Log("xpackage", "XPackageMountWithUiResult() -> S_OK mount=%p", *mount);
    return S_OK;
}

extern "C" HRESULT HleXPackageCloseMountHandle(XPackageMountHandle mount) {
    if (!mount) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, mount);
    Log("xpackage", "XPackageCloseMountHandle(%p) -> S_OK", mount);
    return S_OK;
}

// ---- the registry table ----
// Status meanings: kImplemented = real host code; kStub = logs + plausible
// success; kNotImplemented = no handler yet (loader writes E_NOTIMPL stub).

static const Export kExports[] = {
    // Runtime init (boot-critical).
    {"xgameruntime.dll", "XGameRuntimeInitialize", Status::kStub,
     (void*)HleXGameRuntimeInitialize, "First GDK call. Stub returns S_OK."},
    // Memory.
    {"xmem.dll", "XMemAlloc", Status::kImplemented, (void*)HleXMemAlloc,
     "Real: HeapAlloc."},
    {"xmem.dll", "XMemFree", Status::kImplemented, (void*)HleXMemFree,
     "Real: HeapFree."},
    // Graphics.
    {"d3d12_xs.dll", "D3D12XboxCreateDevice", Status::kImplemented,
     (void*)HleD3D12XboxCreateDevice,
     "IMPLEMENTED (phase 2, real form 2026-11-23). Real 4-arg Xbox form "
     "(adapter, &D3D12XBOX_CREATE_DEVICE_PARAMETERS, riid, &device) — "
     "grounded from the exe's call site at 0x1424785b7; creates a real "
     "desktop D3D12 device at the best feature level the hardware "
     "offers (Xbox fixed level is 12_2; RX 5700 XT: 12_1)."},
    {"d3d12_xs.dll", "SetFrameIntervalX", Status::kImplemented,
     (void*)HleSetFrameIntervalX,
     "IMPLEMENTED (phase 2). Real pacing: stores the interval."},
    {"d3d12_xs.dll", "ScheduleFrameEventX", Status::kImplemented,
     (void*)HleScheduleFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: stamps the frame."},
    {"d3d12_xs.dll", "WaitFrameEventX", Status::kImplemented,
     (void*)HleWaitFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: sleeps to the frame deadline."},
    // Software ray tracing (see docs/SOFTWARE-RT.md). ScarlettCompat-
    // defined entry points, not Xbox APIs: compute-shader ray tracing
    // for GPUs with no RT cores (RX 5700 XT: tier 0 -> software path).
    {"d3d12_xs.dll", "ScSoftwareRT_QueryPath", Status::kImplemented,
     (void*)HleScSoftwareRTQueryPath,
     "IMPLEMENTED. Reads the adapter RT tier; tier 0 -> software path."},
    {"d3d12_xs.dll", "ScSoftwareRT_GetMode", Status::kImplemented,
     (void*)HleScSoftwareRTGetMode,
     "IMPLEMENTED (RT mode, item 8). Returns the selected mode from "
     "SCARLETT_RT_MODE (off/software/hardware; default software)."},
    {"d3d12_xs.dll", "ScSoftwareRT_Init", Status::kImplemented,
     (void*)HleScSoftwareRTInit,
     "IMPLEMENTED. Compiles the compute ray-tracing PSO once, caches it."},
    {"d3d12_xs.dll", "ScSoftwareRT_BuildScene", Status::kImplemented,
     (void*)HleScSoftwareRTBuildScene,
     "IMPLEMENTED. CPU BVH build + GPU upload; returns a scene handle."},
    {"d3d12_xs.dll", "ScSoftwareRT_FreeScene", Status::kImplemented,
     (void*)HleScSoftwareRTFreeScene,
     "IMPLEMENTED. Releases a scene handle's GPU resources."},
    {"d3d12_xs.dll", "ScSoftwareRT_DispatchRays", Status::kImplemented,
     (void*)HleScSoftwareRTDispatchRays,
     "IMPLEMENTED (software path). Runs the compute ray tracer into the "
     "caller's UAV. Hardware path returns E_NOTIMPL (passthrough is "
     "future work, unverifiable on this card)."},
    // Fixed timestep (see docs/FIXED-TIMESTEP.md). ScarlettCompat-
    // defined entry points, not Xbox APIs: accumulator + fixed-step
    // invocation + interpolation factor, so physics runs at a fixed
    // 60 Hz while the render loop runs at any rate.
    {"d3d12_xs.dll", "ScFixedTimestep_Init", Status::kImplemented,
     (void*)HleScFixedTimestepInit,
     "IMPLEMENTED. Accumulator init; rate from the arg or "
     "SCARLETT_PHYSICS_HZ, default 60 Hz."},
    {"d3d12_xs.dll", "ScFixedTimestep_SetStepCallback",
     Status::kImplemented, (void*)HleScFixedTimestepSetStepCallback,
     "IMPLEMENTED. Registers the title's physics-step function."},
    {"d3d12_xs.dll", "ScFixedTimestep_Pump", Status::kImplemented,
     (void*)HleScFixedTimestepPump,
     "IMPLEMENTED. Advances the accumulator by real elapsed time and "
     "steps physics at the fixed rate; capped at maxSteps per pump "
     "(spiral-of-death guard). Also called from WaitFrameEventX when "
     "a callback is registered."},
    {"d3d12_xs.dll", "ScFixedTimestep_Alpha", Status::kImplemented,
     (void*)HleScFixedTimestepAlpha,
     "IMPLEMENTED. Interpolation factor [0, 1) for rendering between "
     "the last two physics states."},
    {"d3d12_xs.dll", "ScFixedTimestep_GetStats", Status::kImplemented,
     (void*)HleScFixedTimestepGetStats,
     "IMPLEMENTED. Accumulator stats (steps, pumps, dropped time)."},
    {"d3d12_xs.dll", "ScFixedTimestep_Shutdown", Status::kImplemented,
     (void*)HleScFixedTimestepShutdown,
     "IMPLEMENTED. Clears the callback, resets the accumulator."},
    // Optimizations (see docs/OPTIMIZATION.md). ScarlettCompat-defined
    // entry points, not Xbox APIs. Measured on the RX 5700 XT; an item
    // with no measured win is reported as such, never claimed as one.
    {"d3d12_xs.dll", "ScOpt_PsoCacheInit", Status::kImplemented,
     (void*)HleScOptPsoCacheInit,
     "IMPLEMENTED (opt 1). Creates the per-process PSO/root-sig cache."},
    {"d3d12_xs.dll", "ScOpt_PsoCacheGetGraphics", Status::kImplemented,
     (void*)HleScOptPsoCacheGetGraphics,
     "IMPLEMENTED (opt 1). Graphics PSO by canonical hash; hit returns "
     "the stored PSO, miss creates + stores it."},
    {"d3d12_xs.dll", "ScOpt_PsoCacheGetCompute", Status::kImplemented,
     (void*)HleScOptPsoCacheGetCompute,
     "IMPLEMENTED (opt 1). Compute PSO, same shape as the graphics path."},
    {"d3d12_xs.dll", "ScOpt_PsoCacheGetRootSignature", Status::kImplemented,
     (void*)HleScOptPsoCacheGetRootSignature,
     "IMPLEMENTED (opt 1). Root signature by serialized-blob hash."},
    {"d3d12_xs.dll", "ScOpt_PsoCacheStats", Status::kImplemented,
     (void*)HleScOptPsoCacheStats,
     "IMPLEMENTED (opt 1). Cache counts: created / hits / misses."},
    {"d3d12_xs.dll", "ScOpt_PsoCacheDestroy", Status::kImplemented,
     (void*)HleScOptPsoCacheDestroy,
     "IMPLEMENTED (opt 1). Releases every cached PSO/root signature."},
    {"d3d12_xs.dll", "ScOpt_Tile2D", Status::kImplemented,
     (void*)HleScOptTile2D,
     "IMPLEMENTED (opt 2). Linear -> Morton-tiled (64KB tiles)."},
    {"d3d12_xs.dll", "ScOpt_Detile2D", Status::kImplemented,
     (void*)HleScOptDetile2D,
     "IMPLEMENTED (opt 2). Tiled -> linear; inverse of Tile2D."},
    {"d3d12_xs.dll", "ScOpt_DetileCached", Status::kImplemented,
     (void*)HleScOptDetileCached,
     "IMPLEMENTED (opt 2). De-tile with a 64MB result cache keyed on "
     "the full tiled input."},
    {"d3d12_xs.dll", "ScOpt_DetileStats", Status::kImplemented,
     (void*)HleScOptDetileStats,
     "IMPLEMENTED (opt 2). Cache counts: detiles / hits / LRU evictions."},
    {"d3d12_xs.dll", "ScOpt_DetileDestroy", Status::kImplemented,
     (void*)HleScOptDetileDestroy,
     "IMPLEMENTED (opt 2). Releases the de-tile cache."},
    {"d3d12_xs.dll", "ScOpt_MeshFallbackInit", Status::kImplemented,
     (void*)HleScOptMeshFallbackInit,
     "IMPLEMENTED (opt 3). Compiles the fallback VS/PS once; CPU meshlet "
     "expansion (correctness path, not a performance path)."},
    {"d3d12_xs.dll", "ScOpt_MeshFallbackGetShaders", Status::kImplemented,
     (void*)HleScOptMeshFallbackGetShaders,
     "IMPLEMENTED (opt 3). The compiled fallback shaders for the "
     "caller's PSO."},
    {"d3d12_xs.dll", "ScOpt_MeshFallbackExpand", Status::kImplemented,
     (void*)HleScOptMeshFallbackExpand,
     "IMPLEMENTED (opt 3). Expands a meshlet into GPU vertex + index "
     "buffers for a classic DrawIndexed."},
    {"d3d12_xs.dll", "ScOpt_MeshFallbackFreeExpanded", Status::kImplemented,
     (void*)HleScOptMeshFallbackFreeExpanded,
     "IMPLEMENTED (opt 3). Releases an expanded mesh's GPU resources."},
    {"d3d12_xs.dll", "ScOpt_MeshFallbackDestroy", Status::kImplemented,
     (void*)HleScOptMeshFallbackDestroy,
     "IMPLEMENTED (opt 3). Releases the cached shaders / upload queue."},
    {"d3d12_xs.dll", "ScOpt_AsyncInit", Status::kImplemented,
     (void*)HleScOptAsyncInit,
     "IMPLEMENTED (opt 4). Sets up the fixed serial-vs-async workload."},
    {"d3d12_xs.dll", "ScOpt_AsyncRunTimed", Status::kImplemented,
     (void*)HleScOptAsyncRunTimed,
     "IMPLEMENTED (opt 4). Runs the workload in serial or async mode; "
     "reports the average wall time. A measurement, not an assumed win."},
    {"d3d12_xs.dll", "ScOpt_AsyncChecksum", Status::kImplemented,
     (void*)HleScOptAsyncChecksum,
     "IMPLEMENTED (opt 4). FNV checksum of the compute output, so the "
     "guest can prove async did not change the result."},
    {"d3d12_xs.dll", "ScOpt_AsyncDestroy", Status::kImplemented,
     (void*)HleScOptAsyncDestroy,
     "IMPLEMENTED (opt 4). Releases the workload queues/PSOs/buffers."},
    {"d3d12_xs.dll", "ScOpt_DescHeapInit", Status::kImplemented,
     (void*)HleScOptDescHeapInit,
     "IMPLEMENTED (opt 5). Pooled descriptor heap with a free-list; "
     "grows by chaining a second heap."},
    {"d3d12_xs.dll", "ScOpt_DescAlloc", Status::kImplemented,
     (void*)HleScOptDescAlloc,
     "IMPLEMENTED (opt 5). One descriptor slot from the pool."},
    {"d3d12_xs.dll", "ScOpt_DescFree", Status::kImplemented,
     (void*)HleScOptDescFree,
     "IMPLEMENTED (opt 5). Returns a slot to the free-list."},
    {"d3d12_xs.dll", "ScOpt_DescHeapStats", Status::kImplemented,
     (void*)HleScOptDescHeapStats,
     "IMPLEMENTED (opt 5). Capacity / live / heap-count (growth "
     "indicator)."},
    {"d3d12_xs.dll", "ScOpt_DescHeapDestroy", Status::kImplemented,
     (void*)HleScOptDescHeapDestroy,
     "IMPLEMENTED (opt 5). Releases every heap in the chain."},
    {"d3d12_xs.dll", "ScOpt_DStorageInit", Status::kImplemented,
     (void*)HleScOptDStorageInit,
     "IMPLEMENTED (opt 6). Async read queue; overlapped Win32 I/O on "
     "this machine (no dstorage.dll in System32, no dstorage.h in the "
     "SDK)."},
    {"d3d12_xs.dll", "ScOpt_DStorageReadFile", Status::kImplemented,
     (void*)HleScOptDStorageReadFile,
     "IMPLEMENTED (opt 6). One overlapped read through the queue."},
    {"d3d12_xs.dll", "ScOpt_DStorageReadFileBatch", Status::kImplemented,
     (void*)HleScOptDStorageReadFileBatch,
     "IMPLEMENTED (opt 6). N concurrent overlapped reads, one wait — "
     "the queue's real benefit."},
    {"d3d12_xs.dll", "ScOpt_DStorageDecompress", Status::kImplemented,
     (void*)HleScOptDStorageDecompress,
     "IMPLEMENTED (opt 6). Decompression stage: NONE (memcpy), ZLIB "
     "(RFC 1950/1951 inflate, CPU), BCPACK_* returns E_NOTIMPL (the "
     "rANS bitstream is not public)."},
    {"d3d12_xs.dll", "ScOpt_ShaderCacheInit", Status::kImplemented,
     (void*)HleScOptShaderCacheInit,
     "IMPLEMENTED (opt 7). Shader precompilation cache directory "
     "(%LOCALAPPDATA%\\ScarlettCompat\\ShaderCache by default)."},
    {"d3d12_xs.dll", "ScOpt_CompileCached", Status::kImplemented,
     (void*)HleScOptCompileCached,
     "IMPLEMENTED (opt 7). Compile-or-load by FNV-1a(source + profile + "
     "entry); corrupt/mismatched files are a miss, never trusted."},
    {"d3d12_xs.dll", "ScOpt_ShaderCacheStats", Status::kImplemented,
     (void*)HleScOptShaderCacheStats,
     "IMPLEMENTED (opt 7). Cache counts: hits / misses."},
    // GPU de-tiled texture cache (opt 2, memory management): VRAM
    // budget + LRU eviction + game-unload evict.
    {"d3d12_xs.dll", "ScOpt_TexCacheInit", Status::kImplemented,
     (void*)HleScOptTexCacheInit,
     "IMPLEMENTED (opt 2). GPU texture cache with a VRAM budget "
     "(0 = 512 MB default); compiles the GPU de-tile compute shader."},
    {"d3d12_xs.dll", "ScOpt_TexCacheLookup", Status::kImplemented,
     (void*)HleScOptTexCacheLookup,
     "IMPLEMENTED (opt 2). Bind-time lookup; hit returns the de-tiled "
     "texture and refreshes its recency."},
    {"d3d12_xs.dll", "ScOpt_TexCacheStore", Status::kImplemented,
     (void*)HleScOptTexCacheStore,
     "IMPLEMENTED (opt 2). De-tile (GPU compute for bpp 4/8/16, CPU "
     "fallback for 1/2) + upload; evicts LRU entries to fit the budget."},
    {"d3d12_xs.dll", "ScOpt_TexCacheEvict", Status::kImplemented,
     (void*)HleScOptTexCacheEvict,
     "IMPLEMENTED (opt 2). Game-unload path: drop one texture from the "
     "cache (S_FALSE when the key was not cached)."},
    {"d3d12_xs.dll", "ScOpt_TexCacheStats", Status::kImplemented,
     (void*)HleScOptTexCacheStats,
     "IMPLEMENTED (opt 2). Cache counts: entries / bytesUsed / budget / "
     "hits / misses / evictions."},
    {"d3d12_xs.dll", "ScOpt_TexCacheDestroy", Status::kImplemented,
     (void*)HleScOptTexCacheDestroy,
     "IMPLEMENTED (opt 2). Releases the cache and every cached texture."},
    // Frame batching (opt 9): submission batching, draw/state
    // batching, frames in flight. Order is never reordered.
    {"d3d12_xs.dll", "ScOpt_BatchInit", Status::kImplemented,
     (void*)HleScOptBatchInit,
     "IMPLEMENTED (opt 9, lever 1). Submission batcher for a queue; "
     "maxLists caps the pending batch."},
    {"d3d12_xs.dll", "ScOpt_BatchAdd", Status::kImplemented,
     (void*)HleScOptBatchAdd,
     "IMPLEMENTED (opt 9, lever 1). Append a closed list (FIFO); "
     "auto-flushes at the cap."},
    {"d3d12_xs.dll", "ScOpt_BatchFlush", Status::kImplemented,
     (void*)HleScOptBatchFlush,
     "IMPLEMENTED (opt 9, lever 1). One ExecuteCommandLists for the "
     "whole pending batch; no-op when empty."},
    {"d3d12_xs.dll", "ScOpt_BatchStats", Status::kImplemented,
     (void*)HleScOptBatchStats,
     "IMPLEMENTED (opt 9, lever 1). listsSubmitted / submitCalls."},
    {"d3d12_xs.dll", "ScOpt_BatchDestroy", Status::kImplemented,
     (void*)HleScOptBatchDestroy,
     "IMPLEMENTED (opt 9, lever 1). Flushes pending lists, then "
     "releases the batcher."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchBegin", Status::kImplemented,
     (void*)HleScOptDrawBatchBegin,
     "IMPLEMENTED (opt 9, lever 2). Start a draw-batching session on "
     "a command list."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchSetPSO", Status::kImplemented,
     (void*)HleScOptDrawBatchSetPSO,
     "IMPLEMENTED (opt 9, lever 2). Set PSO; identical re-sets are "
     "skipped and counted."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchSetRootSignature", Status::kImplemented,
     (void*)HleScOptDrawBatchSetRootSignature,
     "IMPLEMENTED (opt 9, lever 2). Set root signature; identical "
     "re-sets are skipped and counted."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchSetRenderTargets", Status::kImplemented,
     (void*)HleScOptDrawBatchSetRenderTargets,
     "IMPLEMENTED (opt 9, lever 2). Set render targets; identical "
     "re-sets are skipped and counted."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchSetVertexBuffer", Status::kImplemented,
     (void*)HleScOptDrawBatchSetVertexBuffer,
     "IMPLEMENTED (opt 9, lever 2). Set vertex buffer; identical "
     "re-sets are skipped and counted."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchDrawInstanced", Status::kImplemented,
     (void*)HleScOptDrawBatchDrawInstanced,
     "IMPLEMENTED (opt 9, lever 2). Record a draw; consecutive "
     "same-state contiguous draws merge into one DrawInstanced."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchFlushDraw", Status::kImplemented,
     (void*)HleScOptDrawBatchFlushDraw,
     "IMPLEMENTED (opt 9, lever 2). Issue the pending merged draw now "
     "(before any direct command on the list)."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchEnd", Status::kImplemented,
     (void*)HleScOptDrawBatchEnd,
     "IMPLEMENTED (opt 9, lever 2). Flush the pending draw; the handle "
     "is released."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchStats", Status::kImplemented,
     (void*)HleScOptDrawBatchStats,
     "IMPLEMENTED (opt 9, lever 2). drawsRequested / drawsRecorded / "
     "setsRequested / setsIssued."},
    {"d3d12_xs.dll", "ScOpt_DrawBatchDestroy", Status::kImplemented,
     (void*)HleScOptDrawBatchDestroy,
     "IMPLEMENTED (opt 9, lever 2). Release the draw batcher."},
    {"d3d12_xs.dll", "ScOpt_FlightInit", Status::kImplemented,
     (void*)HleScOptFlightInit,
     "IMPLEMENTED (opt 9, lever 3). Frame-slot manager for depth 2, 3 "
     "or 4 frames in flight."},
    {"d3d12_xs.dll", "ScOpt_FlightBeginFrame", Status::kImplemented,
     (void*)HleScOptFlightBeginFrame,
     "IMPLEMENTED (opt 9, lever 3). Wait for the reused slot's fence "
     "value; reports the CPU wait time."},
    {"d3d12_xs.dll", "ScOpt_FlightEndFrame", Status::kImplemented,
     (void*)HleScOptFlightEndFrame,
     "IMPLEMENTED (opt 9, lever 3). Signal this frame's fence value."},
    {"d3d12_xs.dll", "ScOpt_FlightStats", Status::kImplemented,
     (void*)HleScOptFlightStats,
     "IMPLEMENTED (opt 9, lever 3). frames / total CPU wait / average "
     "CPU wait per frame."},
    {"d3d12_xs.dll", "ScOpt_FlightDestroy", Status::kImplemented,
     (void*)HleScOptFlightDestroy,
     "IMPLEMENTED (opt 9, lever 3). Release the frame-slot manager."},
    // Audio.
    {"XAudio2_9.dll", "XAudio2Create", Status::kStub, (void*)HleXAudio2Create,
     "STUB. XMA2 decode is 0% everywhere; stub only."},
    // ---- ERA -> GDK translation (phase 5; see docs/ERA-GDK-MAPPING.md).
    // kernelx XMemAlloc/Free are 1:1 maps to the GDK xmem functions
    // (same API, different module). d3d11_x device creation adapts to a
    // real desktop D3D11 device (GDK on Xbox does not support D3D11.x at
    // all, so there is no d3d12_xs equivalent). d3d12_x device creation
    // takes the same thin path as d3d12_xs D3D12XboxCreateDevice.
    // xg_x.dll / AcpHal.dll functions have no GDK equivalent: they get
    // explicit E_NOTIMPL stubs (logged) instead of the generic missing-
    // import stub, so the log shows exactly which no-equivalent call
    // the title hit. Ordinals below are the Xbox One shim-DLL
    // ordinals — see kOrdinalExports.
    {"kernelx.dll", "XMemAlloc", Status::kImplemented, (void*)HleXMemAlloc,
     "ERA 1:1 map (phase 5): same API as xmem.dll!XMemAlloc, real HeapAlloc."},
    {"kernelx.dll", "XMemFree", Status::kImplemented, (void*)HleXMemFree,
     "ERA 1:1 map (phase 5): same API as xmem.dll!XMemFree, real HeapFree."},
    {"kernelx.dll", "XMemAllocDefault", Status::kImplemented,
     (void*)EraXMemAllocDefault,
     "ERA 1:1 map (phase 5, Xbox One shim-DLL ordinal 515): default CRT alloc "
     "impl, "
     "real HeapAlloc."},
    {"kernelx.dll", "XMemFreeDefault", Status::kImplemented,
     (void*)EraXMemFreeDefault,
     "ERA 1:1 map (phase 5, Xbox One shim-DLL ordinal 518): default CRT free "
     "impl, "
     "real HeapFree."},
    {"kernelx.dll", "VirtualAlloc", Status::kImplemented,
     (void*)HleVirtualAlloc,
     "IMPLEMENTED (VirtualAlloc flag-translation fix, 2026-11-23): flag translation — "
     "strips the Xbox-specific allocation-type bits (0x30002000 / "
     "0x40002000) desktop VirtualAlloc rejects (NULL, err 87), keeps "
     "the desktop-valid set, retries with a system-chosen address "
     "when the requested address fails. Without it the guest's region "
     "builder got size 0 and ran away."},
    {"kernelx.dll", "HleVirtualAlloc", Status::kImplemented,
     (void*)HleVirtualAlloc,
     "IMPLEMENTED (VirtualAlloc flag-translation fix). Test hook: the same wrapper under "
     "its own name, so the era guest can unit-test the flag translation "
     "without importing the name VirtualAlloc from kernelx (which would "
     "reroute the CRT's VirtualAlloc calls). The real routing is the "
     "VirtualAlloc entry above."},
    {"kernelx.dll", "XMemSetAllocationHooks", Status::kStub,
     (void*)EraXMemSetAllocationHooks,
     "ERA adaptation (phase 5, Xbox One shim-DLL ordinal 524): hooks accepted, "
     "logged, not routed — layer allocations do not call title hooks."},
    {"kernelx.dll", "TitleMemoryStatus", Status::kStub,
     (void*)EraTitleMemoryStatus,
     "ERA adaptation (phase 5, Xbox One shim-DLL ordinal 463): ASSUMED "
     "signature "
     "(one pointer); struct zero-filled, no fabricated budget."},
    {"d3d11_x.dll", "D3D11XCreateDevice", Status::kImplemented,
     (void*)EraD3D11XCreateDevice,
     "ERA adaptation (phase 5): real desktop D3D11 device. Form "
     "matches the public Xbox One shim-DLL EraD3D11CreateDevice (10-arg "
     "D3D11CreateDevice shape); an Xbox One title does NOT import this "
     "entry (it imports D3D11XCreateDeviceX)."},
    {"d3d11_x.dll", "D3D11XCreateDeviceAndSwapChain", Status::kImplemented,
     (void*)EraD3D11XCreateDeviceAndSwapChain,
     "ERA adaptation (phase 5): real desktop D3D11 device + DXGI "
     "swapchain. Same form as D3D11XCreateDevice (the public Xbox One shim-DLL "
     "EraD3D11CreateDeviceAndSwapChain is a stub, so this entry is the "
     "layer's own adaptation)."},
    {"d3d11_x.dll", "D3D11XCreateDeviceX", Status::kImplemented,
     (void*)EraD3D11XCreateDeviceX,
     "ERA adaptation (phase 5, Xbox One shim-DLL ordinal 4): the device entry "
     "an Xbox One title imports. Real desktop D3D11 device; GROUNDED "
     "3-arg form (params struct, device, context — the public Xbox One shim-DLL "
     "d3d11.x.cpp)."},
    {"d3d11_x.dll", "D3DMapEsramMemory", Status::kImplemented,
     (void*)EraD3DMapEsramMemory,
     "ERA adaptation (phase 5, Xbox One shim-DLL ordinal 9): ESRAM mapping — "
     "real committed memory at the requested VA, 32 MB arena budget "
     "enforced; GROUNDED signature (the public Xbox One shim-DLL d3d11.x.cpp)."},
    {"d3d11_x.dll", "DXGIXPresentArray", Status::kImplemented,
     (void*)EraDXGIXPresentArray,
     "ERA adaptation (phase 5, Xbox One shim-DLL ordinal 11): presents every "
     "swapchain in the array; GROUNDED signature (the public Xbox One shim-DLL "
     "d3d11.x.cpp)."},
    {"d3d11_x.dll", "D3DAllocateGraphicsMemory", Status::kImplemented,
     (void*)EraD3DAllocateGraphicsMemory,
     "ERA adaptation (phase 5, supporting): graphics-memory allocator, "
     "VirtualAlloc-backed; GROUNDED shape (the public Xbox One shim-DLL "
     "d3d11.x.cpp). An Xbox One title does not import it."},
    {"d3d11_x.dll", "D3DFreeGraphicsMemory", Status::kImplemented,
     (void*)EraD3DFreeGraphicsMemory,
     "ERA adaptation (phase 5, supporting): frees a graphics-memory "
     "allocation (VirtualFree); same source as above."},
    {"xg_x.dll", "XGComputeOptimalDepthStencilTileModes",
     Status::kNotImplemented, (void*)EraXGComputeOptimalDepthStencilTileModes,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 2): Xbox texture "
     "tiling has no desktop equivalent; explicit E_NOTIMPL stub "
     "(logged)."},
    {"xg_x.dll", "XGComputeOptimalTileMode", Status::kNotImplemented,
     (void*)EraXGComputeOptimalTileMode,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 3): explicit "
     "E_NOTIMPL stub (logged)."},
    {"xg_x.dll", "XGComputeTexture2DLayout", Status::kNotImplemented,
     (void*)EraXGComputeTexture2DLayout,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 5): explicit "
     "E_NOTIMPL stub (logged)."},
    {"xg_x.dll", "XGComputeTexture3DLayout", Status::kNotImplemented,
     (void*)EraXGComputeTexture3DLayout,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 6): explicit "
     "E_NOTIMPL stub (logged)."},
    {"xg_x.dll", "XGCreateTexture1DComputer", Status::kNotImplemented,
     (void*)EraXGCreateTexture1DComputer,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 8): explicit "
     "E_NOTIMPL stub (logged)."},
    {"xg_x.dll", "XGCreateTexture2DComputer", Status::kNotImplemented,
     (void*)EraXGCreateTexture2DComputer,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 9): explicit "
     "E_NOTIMPL stub (logged)."},
    {"xg_x.dll", "XGCreateTexture3DComputer", Status::kNotImplemented,
     (void*)EraXGCreateTexture3DComputer,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 10): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "AcpHalAllocateShapeContexts", Status::kNotImplemented,
     (void*)EraAcpHalAllocateShapeContexts,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 1): the ACP audio "
     "DSP has no desktop equivalent; explicit E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "AcpHalCreate", Status::kNotImplemented,
     (void*)EraAcpHalCreate,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 2): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "AcpHalReleaseShapeContexts", Status::kNotImplemented,
     (void*)EraAcpHalReleaseShapeContexts,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 3): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuAlloc", Status::kNotImplemented, (void*)EraApuAlloc,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 4): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuCreateHeap", Status::kNotImplemented,
     (void*)EraApuCreateHeap,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 5): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuFree", Status::kNotImplemented, (void*)EraApuFree,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 6): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuIsVirtualAddressValid", Status::kNotImplemented,
     (void*)EraApuIsVirtualAddressValid,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 8): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuMapApuAddress", Status::kNotImplemented,
     (void*)EraApuMapApuAddress,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 9): explicit "
     "E_NOTIMPL stub (logged)."},
    {"AcpHal.dll", "ApuMapVirtualAddress", Status::kNotImplemented,
     (void*)EraApuMapVirtualAddress,
     "ERA no-equivalent (phase 5, Xbox One shim-DLL ordinal 10): explicit "
     "E_NOTIMPL stub (logged)."},
    {"d3d12_x.dll", "D3D12XCreateDevice", Status::kImplemented,
     (void*)HleEraD3D12XCreateDevice,
     "ERA adaptation (phase 5): D3D12CreateDevice-shaped (adapter, minFL, "
     "riid, ppDevice) — a SEPARATE function from d3d12_xs!D3D12XboxCreateDevice, "
     "which takes a params struct. Sharing one implementation crashed "
     "era_gfx12 (0xC0000005: the feature level 0xb000 was read as a params "
     "pointer). Signature ASSUMED — a D3D11.x-only title does NOT import "
     "d3d12_x at all, so no real ordinal is known."},
    // Input.
    {"gameinput.dll", "GameInputCreate", Status::kStub,
     (void*)HleGameInputCreate, "STUB."},
    // Storage.
    {"dstorage_xs.dll", "DStorageGetFactory", Status::kStub,
     (void*)HleDStorageGetFactory,
     "STUB. No queues/files yet; world streamer needs real I/O."},
    // Async (phase 2, milestone E): synchronous completion model.
    {"xgameruntime.dll", "XAsyncGetStatus", Status::kImplemented,
     (void*)HleXAsyncGetStatus, "IMPLEMENTED (phase 2). Sync completion."},
    {"xgameruntime.dll", "XAsyncGetResultSize", Status::kImplemented,
     (void*)HleXAsyncGetResultSize, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XAsyncCancel", Status::kImplemented,
     (void*)HleXAsyncCancel, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XAsyncRun", Status::kImplemented,
     (void*)HleXAsyncRun, "IMPLEMENTED (phase 2). Runs work synchronously."},
    // User model (phase 2, milestone E): synthetic default user.
    {"xgameruntime.dll", "XUserAddAsync", Status::kImplemented,
     (void*)HleXUserAddAsync,
     "IMPLEMENTED (phase 2). Simplified model: silent default user only."},
    {"xgameruntime.dll", "XUserAddResult", Status::kImplemented,
     (void*)HleXUserAddResult, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XUserGetId", Status::kImplemented,
     (void*)HleXUserGetId, "IMPLEMENTED (phase 2). Synthetic Xuid."},
    {"xgameruntime.dll", "XUserGetGamertag", Status::kImplemented,
     (void*)HleXUserGetGamertag,
     "IMPLEMENTED (phase 2). Returns \"Player\"."},
    {"xgameruntime.dll", "XUserGetLocalId", Status::kImplemented,
     (void*)HleXUserGetLocalId, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XUserCloseHandle", Status::kImplemented,
     (void*)HleXUserCloseHandle, "IMPLEMENTED (phase 2)."},
    // GameSave (phase 2, milestone E): local-only provider.
    {"xgameruntime.dll", "XGameSaveInitializeProvider", Status::kImplemented,
     (void*)HleXGameSaveInitializeProvider,
     "IMPLEMENTED (phase 2). Local folder under %LOCALAPPDATA%; no cloud."},
    {"xgameruntime.dll", "XGameSaveCloseProvider", Status::kImplemented,
     (void*)HleXGameSaveCloseProvider, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XGameSaveCreateContainer", Status::kImplemented,
     (void*)HleXGameSaveCreateContainer,
     "IMPLEMENTED (phase 2). Container = subfolder of the provider root."},
    // GameSave update flow (phase 3): staged blob writes/deletes.
    {"xgameruntime.dll", "XGameSaveCreateUpdate", Status::kImplemented,
     (void*)HleXGameSaveCreateUpdate,
     "IMPLEMENTED (phase 3). Creates a staged update for a container."},
    {"xgameruntime.dll", "XGameSaveSubmitBlobWrite", Status::kImplemented,
     (void*)HleXGameSaveSubmitBlobWrite,
     "IMPLEMENTED (phase 3). Stages a blob write (16 MB cap)."},
    {"xgameruntime.dll", "XGameSaveSubmitBlobDelete", Status::kImplemented,
     (void*)HleXGameSaveSubmitBlobDelete,
     "IMPLEMENTED (phase 3). Stages a blob delete."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdate", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdate,
     "IMPLEMENTED (phase 3). Applies the staged update to disk."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdateAsync", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdateAsync,
     "IMPLEMENTED (phase 3). Applies inline, sync completion."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdateResult", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdateResult, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveCloseUpdate", Status::kImplemented,
     (void*)HleXGameSaveCloseUpdate, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveCloseContainer", Status::kImplemented,
     (void*)HleXGameSaveCloseContainer, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveReadBlobData", Status::kImplemented,
     (void*)HleXGameSaveReadBlobData,
     "IMPLEMENTED (phase 3). Reads a blob back from the container."},
    // GameSaveFiles (phase 3): file-style saves.
    {"xgameruntime.dll", "XGameSaveFilesGetFolderWithUiAsync",
     Status::kImplemented, (void*)HleXGameSaveFilesGetFolderWithUiAsync,
     "IMPLEMENTED (phase 3). Returns the local provider folder; no UI "
     "headless."},
    {"xgameruntime.dll", "XGameSaveFilesGetFolderWithUiResult",
     Status::kImplemented, (void*)HleXGameSaveFilesGetFolderWithUiResult,
     "IMPLEMENTED (phase 3)."},
    // Package (phase 2, milestone E): mount path = guest dir.
    {"xgameruntime.dll", "XPackageGetMountPathSize", Status::kImplemented,
     (void*)HleXPackageGetMountPathSize, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XPackageGetMountPath", Status::kImplemented,
     (void*)HleXPackageGetMountPath,
     "IMPLEMENTED (phase 2). Returns the guest directory."},
    {"xgameruntime.dll", "XPackageIsPackagedProcess", Status::kImplemented,
     (void*)HleXPackageIsPackagedProcess,
     "IMPLEMENTED (phase 2). Presents the guest as packaged."},
    {"xgameruntime.dll", "XPackageGetCurrentProcessPackageIdentifier",
     Status::kImplemented,
     (void*)HleXPackageGetCurrentProcessPackageIdentifier,
     "IMPLEMENTED (phase 3). Returns the synthetic package identifier. "
     "Signature corrected to the real (bufferSize, char*) form."},
    {"xgameruntime.dll", "XPackageEnumeratePackages", Status::kImplemented,
     (void*)HleXPackageEnumeratePackages,
     "IMPLEMENTED (phase 3). Enumerates the single synthetic package."},
    {"xgameruntime.dll", "XPackageMountWithUiAsync", Status::kImplemented,
     (void*)HleXPackageMountWithUiAsync,
     "IMPLEMENTED (phase 3). Mounts the current package; no UI headless."},
    {"xgameruntime.dll", "XPackageMountWithUiResult", Status::kImplemented,
     (void*)HleXPackageMountWithUiResult, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XPackageCloseMountHandle", Status::kImplemented,
     (void*)HleXPackageCloseMountHandle, "IMPLEMENTED (phase 3)."},

    // ---- Real-exe imports (grounded 2026-11-23 from
    // game_scarlett_master.exe's import table: 597 imports, 68 modules).
    // The exe imports BY NAME — the only real ordinal is GameInput
    // ordinal 1 (confirmed; the synthetic ordinal tables stay
    // test-only). 97 entries were missing from the registry; they are
    // added here: 13 real implementations (xmem + d3d12_xs + XCRDAPI)
    // and 84 stubs (S_OK / void / dummy-handle). Anything the game
    // calls that is still missing gets the E_NOTIMPL stub and an
    // "IMPORT FAILED" loader line — the boot log will show it.
    // Memory (xmem.dll) — real implementations.
    {"xmem.dll", "XMemAllocDefault", Status::kImplemented,
     (void*)HleXMemAllocDefault, "IMPLEMENTED (real-exe). HeapAlloc."},
    {"xmem.dll", "XMemFreeDefault", Status::kImplemented,
     (void*)HleXMemFreeDefault, "IMPLEMENTED (real-exe). HeapFree."},
    {"xmem.dll", "XMemVirtualAlloc", Status::kImplemented,
     (void*)HleXMemVirtualAlloc,
     "IMPLEMENTED (real-exe). VirtualAlloc wrapper: strips Xbox alloc-type "
     "bits, retries with PAGE_READWRITE when the Xbox protect value "
     "fails."},
    {"xmem.dll", "XMemGetWorkingSetStatistics", Status::kStub,
     (void*)HleXMemGetWorkingSetStatistics,
     "STUB (real-exe). S_OK; stats struct not filled (layout "
     "GDKX-gated)."},
    {"xmem.dll", "XMemIsToolsMemAvailable", Status::kImplemented,
     (void*)HleXMemIsToolsMemAvailable,
     "IMPLEMENTED (real-exe). FALSE: no devkit memory on desktop."},
    {"xmem.dll", "XMemAllocatePhysicalPages", Status::kStub,
     (void*)HleXMemAllocatePhysicalPages,
     "STUB (real-exe). S_OK; no desktop physical-page pool."},
    {"xmem.dll", "XMemFreePhysicalPages", Status::kStub,
     (void*)HleXMemFreePhysicalPages,
     "STUB (real-exe). S_OK; no desktop physical-page pool."},
    {"xmem.dll", "XMemMapPhysicalPages", Status::kImplemented,
     (void*)HleXMemMapPhysicalPages,
     "IMPLEMENTED (real-exe). Ordinary committed memory returned as the "
     "mapped VA (guarded size)."},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", Status::kImplemented,
     (void*)HleXMemMapPhysicalPagesScatter,
     "IMPLEMENTED (real-exe). Measured ABI: (pages, count, out), status "
     "return. Maps one page at each guest-supplied address, tracked so "
     "the reservation is released once."},
    {"xmem.dll", "XMemTransferMemory", Status::kImplemented,
     (void*)HleXMemTransferMemory,
     "IMPLEMENTED (real-exe). memcpy(dst, src, size)."},
    // Graphics (d3d12_xs.dll) — real implementations.
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", Status::kImplemented,
     (void*)HleD3D12ConfigureVirtualMemoryX,
     "IMPLEMENTED (real-exe). S_OK: no desktop GPU VA space to "
     "configure (the game int3-crashes if this fails)."},
    {"d3d12_xs.dll", "D3D12SerializeVersionedRootSignature",
     Status::kImplemented, (void*)HleD3D12SerializeVersionedRootSignature,
     "IMPLEMENTED (real-exe). Forwards to desktop "
     "D3D12SerializeVersionedRootSignature."},
    // DirectStorage via XCRDAPI.dll (the real exe uses XCRDAPI.dll, not
    // dstorage_xs.dll, for DStorageGetFactory).
    {"XCRDAPI.dll", "DStorageGetFactory", Status::kStub,
     (void*)HleDStorageGetFactory,
     "STUB (real-exe). Same dummy-factory stub as dstorage_xs.dll."},

    // chad.dll — S_OK stubs.
    {"chad.dll", "XDspActivate", Status::kStub,
     (void*)XDspActivate, "STUB (real-exe import, 2026-11-23)."},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", Status::kStub,
     (void*)XDspConnectWithMaximumStreamLimit, "STUB (real-exe import, 2026-11-23)."},
    {"chad.dll", "XDspDeactivate", Status::kStub,
     (void*)XDspDeactivate, "STUB (real-exe import, 2026-11-23)."},
    {"chad.dll", "XDspDisconnect", Status::kStub,
     (void*)XDspDisconnect, "STUB (real-exe import, 2026-11-23)."},
    {"chad.dll", "XDspSubmitCommand", Status::kStub,
     (void*)XDspSubmitCommand, "STUB (real-exe import, 2026-11-23)."},

    // xg_xs.dll — S_OK stubs.
    {"xg_xs.dll", "XGCreateTextureComputer", Status::kSketchy,
     (void*)XGCreateTextureComputer,
     "Build 80: real object (measured 56-byte desc, per-subresource layout, "
     "full vtable, slot-8 copy). Texel order inside a 64KB tile is the "
     "project's Morton model - unverified against GDK tables."},
    {"xg_xs.dll", "XGSuggestSwizzleMode", Status::kSketchy,
     (void*)XGSuggestSwizzleMode,
     "Build 80: derived from the same table the copy implements; unsupported "
     "descriptors fail instead of inventing a mode."},

    // amd_fidelityfx_loader_xbox.dll — S_OK stubs.
    {"amd_fidelityfx_loader_xbox.dll", "ffxCreateContext", Status::kStub,
     (void*)ffxCreateContext, "STUB (real-exe import, 2026-11-23)."},
    {"amd_fidelityfx_loader_xbox.dll", "ffxDestroyContext", Status::kStub,
     (void*)ffxDestroyContext, "STUB (real-exe import, 2026-11-23)."},
    {"amd_fidelityfx_loader_xbox.dll", "ffxDispatch", Status::kStub,
     (void*)ffxDispatch, "STUB (real-exe import, 2026-11-23)."},
    {"amd_fidelityfx_loader_xbox.dll", "ffxQuery", Status::kStub,
     (void*)ffxQuery, "STUB (real-exe import, 2026-11-23)."},

    // xapu.dll — S_OK stubs.
    {"xapu.dll", "XApuConnect", Status::kStub,
     (void*)XApuConnect, "STUB (real-exe import, 2026-11-23)."},
    {"xapu.dll", "XApuDequeueResult", Status::kStub,
     (void*)XApuDequeueResult, "STUB (real-exe import, 2026-11-23)."},
    {"xapu.dll", "XApuDisconnect", Status::kStub,
     (void*)XApuDisconnect, "STUB (real-exe import, 2026-11-23)."},
    {"xapu.dll", "XApuEnqueueCommand", Status::kStub,
     (void*)XApuEnqueueCommand, "STUB (real-exe import, 2026-11-23)."},
    {"xapu.dll", "XApuGetResultCount", Status::kStub,
     (void*)XApuGetResultCount, "STUB (real-exe import, 2026-11-23)."},

    // libHttpClient.GDK.dll — S_OK stubs.
    {"libHttpClient.GDK.dll", "HCCleanupAsync", Status::kStub,
     (void*)HCCleanupAsync, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallCloseHandle", Status::kStub,
     (void*)HCHttpCallCloseHandle, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallCreate", Status::kStub,
     (void*)HCHttpCallCreate, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallDuplicateHandle", Status::kStub,
     (void*)HCHttpCallDuplicateHandle, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallGetRequestUrl", Status::kStub,
     (void*)HCHttpCallGetRequestUrl, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallPerformAsync", Status::kStub,
     (void*)HCHttpCallPerformAsync, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetHeaderAtIndex", Status::kStub,
     (void*)HCHttpCallRequestGetHeaderAtIndex, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetNumHeaders", Status::kStub,
     (void*)HCHttpCallRequestGetNumHeaders, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRequestBodyBytes", Status::kStub,
     (void*)HCHttpCallRequestGetRequestBodyBytes, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryAllowed", Status::kStub,
     (void*)HCHttpCallRequestGetRetryAllowed, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryCacheId", Status::kStub,
     (void*)HCHttpCallRequestGetRetryCacheId, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryDelay", Status::kStub,
     (void*)HCHttpCallRequestGetRetryDelay, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetTimeout", Status::kStub,
     (void*)HCHttpCallRequestGetTimeout, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetTimeoutWindow", Status::kStub,
     (void*)HCHttpCallRequestGetTimeoutWindow, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetUrl", Status::kStub,
     (void*)HCHttpCallRequestGetUrl, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetHeader", Status::kStub,
     (void*)HCHttpCallRequestSetHeader, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyBytes", Status::kStub,
     (void*)HCHttpCallRequestSetRequestBodyBytes, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyString", Status::kStub,
     (void*)HCHttpCallRequestSetRequestBodyString, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryAllowed", Status::kStub,
     (void*)HCHttpCallRequestSetRetryAllowed, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryCacheId", Status::kStub,
     (void*)HCHttpCallRequestSetRetryCacheId, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryDelay", Status::kStub,
     (void*)HCHttpCallRequestSetRetryDelay, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeout", Status::kStub,
     (void*)HCHttpCallRequestSetTimeout, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeoutWindow", Status::kStub,
     (void*)HCHttpCallRequestSetTimeoutWindow, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetUrl", Status::kStub,
     (void*)HCHttpCallRequestSetUrl, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetHeader", Status::kStub,
     (void*)HCHttpCallResponseGetHeader, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetHeaderAtIndex", Status::kStub,
     (void*)HCHttpCallResponseGetHeaderAtIndex, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetNetworkErrorCode", Status::kStub,
     (void*)HCHttpCallResponseGetNetworkErrorCode, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetNumHeaders", Status::kStub,
     (void*)HCHttpCallResponseGetNumHeaders, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetPlatformNetworkErrorMessage", Status::kStub,
     (void*)HCHttpCallResponseGetPlatformNetworkErrorMessage, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytes", Status::kStub,
     (void*)HCHttpCallResponseGetResponseBodyBytes, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytesSize", Status::kStub,
     (void*)HCHttpCallResponseGetResponseBodyBytesSize, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseString", Status::kStub,
     (void*)HCHttpCallResponseGetResponseString, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetStatusCode", Status::kStub,
     (void*)HCHttpCallResponseGetStatusCode, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCHttpCallSetTracing", Status::kStub,
     (void*)HCHttpCallSetTracing, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCInitialize", Status::kStub,
     (void*)HCInitialize, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCSettingsGetTraceLevel", Status::kStub,
     (void*)HCSettingsGetTraceLevel, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCTraceImplMessage", Status::kStub,
     (void*)HCTraceImplMessage, "STUB (real-exe import, 2026-11-23)."},
    {"libHttpClient.GDK.dll", "HCTraceSetEtwEnabled", Status::kStub,
     (void*)HCTraceSetEtwEnabled, "STUB (real-exe import, 2026-11-23)."},

    // Bink2Scarlett.dll — video stubs.
    {"Bink2Scarlett.dll", "BinkClose", Status::kStub,
     (void*)BinkClose, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkDoFrame", Status::kStub,
     (void*)BinkDoFrame, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkDoFrameAsyncMulti", Status::kStub,
     (void*)BinkDoFrameAsyncMulti, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkDoFrameAsyncWait", Status::kStub,
     (void*)BinkDoFrameAsyncWait, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkFreeGlobals", Status::kStub,
     (void*)BinkFreeGlobals, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkGetFrameBuffersInfo", Status::kStub,
     (void*)BinkGetFrameBuffersInfo, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkGetPlatformInfo", Status::kStub,
     (void*)BinkGetPlatformInfo, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkGoto", Status::kStub,
     (void*)BinkGoto, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkNextFrame", Status::kStub,
     (void*)BinkNextFrame, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkPause", Status::kStub,
     (void*)BinkPause, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkRegisterFrameBuffers", Status::kStub,
     (void*)BinkRegisterFrameBuffers, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkRequestStopAsyncThreadsMulti", Status::kStub,
     (void*)BinkRequestStopAsyncThreadsMulti, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkSetMemory", Status::kStub,
     (void*)BinkSetMemory, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkSetOSFileCallbacks", Status::kStub,
     (void*)BinkSetOSFileCallbacks, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkSetSoundSystem", Status::kStub,
     (void*)BinkSetSoundSystem, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkSetVolume", Status::kStub,
     (void*)BinkSetVolume, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkSetWillLoop", Status::kStub,
     (void*)BinkSetWillLoop, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkStartAsyncThread", Status::kStub,
     (void*)BinkStartAsyncThread, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkWait", Status::kStub,
     (void*)BinkWait, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkWaitStopAsyncThreadsMulti", Status::kStub,
     (void*)BinkWaitStopAsyncThreadsMulti, "STUB (real-exe import, 2026-11-23)."},
    {"Bink2Scarlett.dll", "BinkGetKeyFrame", Status::kStub,
     (void*)BinkGetKeyFrame,
     "STUB (real-exe import, 2026-11-23). Returns 0."},
    {"Bink2Scarlett.dll", "BinkShouldSkip", Status::kStub,
     (void*)BinkShouldSkip,
     "STUB (real-exe import, 2026-11-23). Returns FALSE."},
    {"Bink2Scarlett.dll", "BinkOpenWithOptions", Status::kStub,
     (void*)BinkOpenWithOptions,
     "STUB (real-exe import, 2026-11-23). Dummy non-NULL handle; no "
     "video decode."},

    // XboxBvhBuilder.dll — DXR BVH builder (C++ methods).
    {"XboxBvhBuilder.dll", "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z", Status::kStub,
     (void*)HleXboxBvhBuilderCtor, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "??1XboxBvhBuilder@@QEAA@XZ", Status::kStub,
     (void*)HleXboxBvhBuilderDtor, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z", Status::kStub,
     (void*)HleXboxBvhBuilderBuildBatched2X, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z", Status::kStub,
     (void*)HleXboxBvhBuilderCopy, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z", Status::kStub,
     (void*)HleXboxBvhBuilderGetPrebuildInfo, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z", Status::kStub,
     (void*)HleXboxBvhBuilderRehydrate2X, "STUB (real-exe import, 2026-11-23)."},
    {"XboxBvhBuilder.dll", "?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z", Status::kStub,
     (void*)HleXboxBvhBuilderSet64BitMortonCodes, "STUB (real-exe import, 2026-11-23)."},
};

static const char* kShimModules[] = {
    "xgameruntime.dll", "xgameplatform.dll", "d3d12_xs.dll", "d3d12_x.dll",
    "xmem.dll", "XAudio2_9.dll", "dstorage_xs.dll", "gameinput.dll",
    "xgame_save.dll", "xstore.dll",
    // ERA modules (phase 5): resolved through the HLE registry, never
    // LoadLibrary'd — none of them exist on desktop Windows.
    // (d3d12_x.dll was already in the list above.)
    "d3d11_x.dll", "kernelx.dll", "xg_x.dll", "AcpHal.dll",
    // Real-exe modules (2026-11-23, from game_scarlett_master.exe's
    // import table): the exe imports these by name, and none of them
    // exists on desktop Windows, so they must resolve through the HLE
    // registry — if one is missing here the loader falls through to
    // real LoadLibraryA, which fails, and every import from it gets
    // the E_NOTIMPL stub.
    "chad.dll", "xg_xs.dll", "XboxBvhBuilder.dll", "XCRDAPI.dll",
    "amd_fidelityfx_loader_xbox.dll", "libHttpClient.GDK.dll", "xapu.dll",
    "Bink2Scarlett.dll",
};

static bool CiEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

const Export* FindExport(const char* module, const char* name) {
    for (size_t i = 0; i < sizeof(kExports) / sizeof(kExports[0]); ++i) {
        if (CiEq(kExports[i].module, module) && CiEq(kExports[i].name, name))
            return &kExports[i];
    }
    return nullptr;
}

// Ordinal table. GDK ordinals are synthetic (numbered in .def order
// per module) — the GDK import libs have no real ordinal layout to
// match. ERA ordinals are the Xbox One shim-DLL ordinals — the only
// real ordinal layout available; the real console ordinals are
// unknown. ERA titles import by NAME, so the name imports resolve
// through the name table and never touch these rows — they only
// matter for a title that imports by ordinal. An earlier phase 5 pass
// wrote wrong ordinals into these rows by mistake; the rows below are
// the corrected Xbox One shim-DLL ordinals.
//
// REAL-EXE FINDING (2026-11-23, game_scarlett_master.exe, 597 imports):
// the real Series X exe imports BY NAME — there are no real ordinals
// to replace the synthetic ones with, and the synthetic tables stay
// test-only (guest_ordinal exercises them). The ONE real ordinal in
// the exe is GameInput.dll ordinal 1, and it matches the synthetic
// row below (gameinput 1 = GameInputCreate) — that assignment is now
// CONFIRMED real, not synthetic. Do NOT invent ordinal numbers for
// the other modules.
static const OrdinalExport kOrdinalExports[] = {
    {"xgameruntime.dll", 1, (void*)HleXGameRuntimeInitialize,
     "XGameRuntimeInitialize"},
    {"xgameruntime.dll", 2, (void*)HleXAsyncGetStatus, "XAsyncGetStatus"},
    {"xgameruntime.dll", 3, (void*)HleXAsyncGetResultSize,
     "XAsyncGetResultSize"},
    {"xgameruntime.dll", 4, (void*)HleXAsyncCancel, "XAsyncCancel"},
    {"xgameruntime.dll", 5, (void*)HleXAsyncRun, "XAsyncRun"},
    {"xgameruntime.dll", 6, (void*)HleXUserAddAsync, "XUserAddAsync"},
    {"xgameruntime.dll", 7, (void*)HleXUserAddResult, "XUserAddResult"},
    {"xgameruntime.dll", 8, (void*)HleXUserGetId, "XUserGetId"},
    {"xgameruntime.dll", 9, (void*)HleXUserGetGamertag, "XUserGetGamertag"},
    {"xgameruntime.dll", 10, (void*)HleXUserGetLocalId, "XUserGetLocalId"},
    {"xgameruntime.dll", 11, (void*)HleXUserCloseHandle, "XUserCloseHandle"},
    {"xgameruntime.dll", 12, (void*)HleXGameSaveInitializeProvider,
     "XGameSaveInitializeProvider"},
    {"xgameruntime.dll", 13, (void*)HleXGameSaveCloseProvider,
     "XGameSaveCloseProvider"},
    {"xgameruntime.dll", 14, (void*)HleXGameSaveCreateContainer,
     "XGameSaveCreateContainer"},
    {"xgameruntime.dll", 15, (void*)HleXPackageGetMountPathSize,
     "XPackageGetMountPathSize"},
    {"xgameruntime.dll", 16, (void*)HleXPackageGetMountPath,
     "XPackageGetMountPath"},
    {"xgameruntime.dll", 17, (void*)HleXPackageIsPackagedProcess,
     "XPackageIsPackagedProcess"},
    {"xgameruntime.dll", 18,
     (void*)HleXPackageGetCurrentProcessPackageIdentifier,
     "XPackageGetCurrentProcessPackageIdentifier"},
    {"xgameruntime.dll", 19, (void*)HleXPackageMountWithUiAsync,
     "XPackageMountWithUiAsync"},
    {"xgameruntime.dll", 20, (void*)HleXPackageMountWithUiResult,
     "XPackageMountWithUiResult"},
    {"xgameruntime.dll", 21, (void*)HleXPackageCloseMountHandle,
     "XPackageCloseMountHandle"},
    // Phase 3: GameSave update flow + GameSaveFiles + package details.
    {"xgameruntime.dll", 22, (void*)HleXGameSaveCreateUpdate,
     "XGameSaveCreateUpdate"},
    {"xgameruntime.dll", 23, (void*)HleXGameSaveSubmitBlobWrite,
     "XGameSaveSubmitBlobWrite"},
    {"xgameruntime.dll", 24, (void*)HleXGameSaveSubmitBlobDelete,
     "XGameSaveSubmitBlobDelete"},
    {"xgameruntime.dll", 25, (void*)HleXGameSaveSubmitUpdate,
     "XGameSaveSubmitUpdate"},
    {"xgameruntime.dll", 26, (void*)HleXGameSaveSubmitUpdateAsync,
     "XGameSaveSubmitUpdateAsync"},
    {"xgameruntime.dll", 27, (void*)HleXGameSaveSubmitUpdateResult,
     "XGameSaveSubmitUpdateResult"},
    {"xgameruntime.dll", 28, (void*)HleXGameSaveCloseUpdate,
     "XGameSaveCloseUpdate"},
    {"xgameruntime.dll", 29, (void*)HleXGameSaveCloseContainer,
     "XGameSaveCloseContainer"},
    {"xgameruntime.dll", 30, (void*)HleXGameSaveReadBlobData,
     "XGameSaveReadBlobData"},
    {"xgameruntime.dll", 31,
     (void*)HleXGameSaveFilesGetFolderWithUiAsync,
     "XGameSaveFilesGetFolderWithUiAsync"},
    {"xgameruntime.dll", 32,
     (void*)HleXGameSaveFilesGetFolderWithUiResult,
     "XGameSaveFilesGetFolderWithUiResult"},
    {"xgameruntime.dll", 33, (void*)HleXPackageEnumeratePackages,
     "XPackageEnumeratePackages"},
    {"xmem.dll", 1, (void*)HleXMemAlloc, "XMemAlloc"},
    {"xmem.dll", 2, (void*)HleXMemFree, "XMemFree"},
    {"d3d12_xs.dll", 1, (void*)HleD3D12XboxCreateDevice,
     "D3D12XboxCreateDevice"},
    {"d3d12_xs.dll", 2, (void*)HleSetFrameIntervalX, "SetFrameIntervalX"},
    {"d3d12_xs.dll", 3, (void*)HleScheduleFrameEventX,
     "ScheduleFrameEventX"},
    {"d3d12_xs.dll", 4, (void*)HleWaitFrameEventX, "WaitFrameEventX"},
    {"XAudio2_9.dll", 1, (void*)HleXAudio2Create, "XAudio2Create"},
    {"gameinput.dll", 1, (void*)HleGameInputCreate, "GameInputCreate"},
    {"dstorage_xs.dll", 1, (void*)HleDStorageGetFactory,
     "DStorageGetFactory"},
    // ERA ordinals — the Xbox One shim-DLL ordinals. The shim-DLL's
    // kernelx is a reimplementation with its own export layout. The
    // rows below use the Xbox One shim-DLL ordinals (the only real
    // ordinal layout available); name imports resolve through the
    // name table and never touch these rows.
    {"kernelx.dll", 463, (void*)EraTitleMemoryStatus, "TitleMemoryStatus"},
    {"kernelx.dll", 514, (void*)HleXMemAlloc, "XMemAlloc"},
    {"kernelx.dll", 515, (void*)EraXMemAllocDefault, "XMemAllocDefault"},
    {"kernelx.dll", 517, (void*)HleXMemFree, "XMemFree"},
    {"kernelx.dll", 518, (void*)EraXMemFreeDefault, "XMemFreeDefault"},
    {"kernelx.dll", 524, (void*)EraXMemSetAllocationHooks,
     "XMemSetAllocationHooks"},
    {"d3d11_x.dll", 4, (void*)EraD3D11XCreateDeviceX, "D3D11XCreateDeviceX"},
    {"d3d11_x.dll", 9, (void*)EraD3DMapEsramMemory, "D3DMapEsramMemory"},
    {"d3d11_x.dll", 11, (void*)EraDXGIXPresentArray, "DXGIXPresentArray"},
    {"xg_x.dll", 2, (void*)EraXGComputeOptimalDepthStencilTileModes,
     "XGComputeOptimalDepthStencilTileModes"},
    {"xg_x.dll", 3, (void*)EraXGComputeOptimalTileMode,
     "XGComputeOptimalTileMode"},
    {"xg_x.dll", 5, (void*)EraXGComputeTexture2DLayout,
     "XGComputeTexture2DLayout"},
    {"xg_x.dll", 6, (void*)EraXGComputeTexture3DLayout,
     "XGComputeTexture3DLayout"},
    {"xg_x.dll", 8, (void*)EraXGCreateTexture1DComputer,
     "XGCreateTexture1DComputer"},
    {"xg_x.dll", 9, (void*)EraXGCreateTexture2DComputer,
     "XGCreateTexture2DComputer"},
    {"xg_x.dll", 10, (void*)EraXGCreateTexture3DComputer,
     "XGCreateTexture3DComputer"},
    {"AcpHal.dll", 1, (void*)EraAcpHalAllocateShapeContexts,
     "AcpHalAllocateShapeContexts"},
    {"AcpHal.dll", 2, (void*)EraAcpHalCreate, "AcpHalCreate"},
    {"AcpHal.dll", 3, (void*)EraAcpHalReleaseShapeContexts,
     "AcpHalReleaseShapeContexts"},
    {"AcpHal.dll", 4, (void*)EraApuAlloc, "ApuAlloc"},
    {"AcpHal.dll", 5, (void*)EraApuCreateHeap, "ApuCreateHeap"},
    {"AcpHal.dll", 6, (void*)EraApuFree, "ApuFree"},
    {"AcpHal.dll", 8, (void*)EraApuIsVirtualAddressValid,
     "ApuIsVirtualAddressValid"},
    {"AcpHal.dll", 9, (void*)EraApuMapApuAddress, "ApuMapApuAddress"},
    {"AcpHal.dll", 10, (void*)EraApuMapVirtualAddress, "ApuMapVirtualAddress"},
};

const OrdinalExport* FindOrdinalExport(const char* module, uint16_t ordinal) {
    for (size_t i = 0;
         i < sizeof(kOrdinalExports) / sizeof(kOrdinalExports[0]); ++i) {
        if (CiEq(kOrdinalExports[i].module, module) &&
            kOrdinalExports[i].ordinal == ordinal)
            return &kOrdinalExports[i];
    }
    return nullptr;
}

size_t ExportCount() { return sizeof(kExports) / sizeof(kExports[0]); }

const Export* ExportAt(size_t i) {
    return i < ExportCount() ? &kExports[i] : nullptr;
}

bool IsShimModule(const char* module) {
    for (size_t i = 0; i < sizeof(kShimModules) / sizeof(kShimModules[0]);
         ++i) {
        if (CiEq(kShimModules[i], module)) return true;
    }
    return false;
}

}  // namespace sc
