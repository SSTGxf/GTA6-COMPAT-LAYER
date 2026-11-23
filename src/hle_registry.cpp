// hle_registry.cpp — the HLE export registry (Xenia-style) + logging.
// Central table: (Xbox DLL, function) -> host implementation + status tag.
// The loader resolves guest imports through FindExport(); anything missing
// gets the E_NOTIMPL stub. Statuses are the project's tracking surface —
// see docs/STATUS.md.
#include "hle_registry.h"
#include <string.h>

namespace sc {

namespace {

// ---- logging ----

void WriteLogLine(const char* line) {
    fputs(line, stderr);
    fputc('\n', stderr);
    FILE* f = nullptr;
    if (fopen_s(&f, "scarlett.log", "a") == 0 && f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
}

}  // namespace

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

extern "C" HRESULT HleD3D12XboxCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice) {
    // REAL translation (phase 2): the assumed signature is
    // D3D12CreateDevice-shaped, so this is a thin passthrough to the
    // desktop device. The REAL Xbox signature takes a
    // D3D12XBOX_CREATE_DEVICE_PARAMETERS struct (GDKX-gated) — that form
    // must be confirmed against the exe's import table once it lands.
    // The created device is cached so later steps (swapchain, pacing)
    // can use it.
    HRESULT hr = D3D12CreateDevice(adapter, minFL, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        if (g_xsDevice) g_xsDevice->Release();
        g_xsDevice = (ID3D12Device*)*ppDevice;
        g_xsDevice->AddRef();
        Log("d3d12xs",
            "D3D12XboxCreateDevice -> REAL desktop device %p (minFL 0x%x)",
            *ppDevice, (unsigned)minFL);
    } else {
        Log("d3d12xs",
            "D3D12XboxCreateDevice FAILED hr=0x%08x (minFL 0x%x)",
            (unsigned)hr, (unsigned)minFL);
    }
    return hr;
}

extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags,
                                    DWORD proc) {
    Log("audio", "XAudio2Create(flags 0x%x, proc %lu) -> S_OK (STUB)",
        flags, proc);
    if (ppXAudio2) *ppXAudio2 = (void*)0xA0D10ULL;
    return S_OK;
}

extern "C" HRESULT HleGameInputCreate(void** ppGameInput) {
    Log("input", "GameInputCreate() -> S_OK (STUB)");
    if (ppGameInput) *ppGameInput = (void*)0x6A6E707574ULL;  // "jnput"
    return S_OK;
}

extern "C" HRESULT HleDStorageGetFactory(void** ppFactory) {
    Log("dstorage", "DStorageGetFactory() -> S_OK (STUB, no queues yet)");
    if (ppFactory) *ppFactory = (void*)0xD570A6EULL;
    return S_OK;
}

extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags) {
    if (interval == 0) interval = 1;
    g_frameInterval = interval;
    Log("present",
        "SetFrameIntervalX(%u, 0x%x) -> S_OK (real pacing: %.1f ms/frame)",
        interval, flags, FramePeriodMs());
    return S_OK;
}

extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId) {
    g_lastFrameId = frameId;
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_haveFrame = true;
    Log("present", "ScheduleFrameEventX(%llu) -> S_OK", 
        (unsigned long long)frameId);
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
    if (remainMs > 0) {
        double waitMs = remainMs < (double)timeoutMs ? remainMs
                                                     : (double)timeoutMs;
        // Waitable timer (100ns units) — precise to ~1 ms, unlike Sleep's
        // ~15.6 ms granularity at default timer resolution.
        HANDLE timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, (DWORD)(waitMs + 100) + 100);
            CloseHandle(timer);
        } else {
            DWORD ms = (DWORD)(waitMs + 0.5);
            if (ms > 0) Sleep(ms);
        }
        Log("present",
            "WaitFrameEventX(%llu) waited ~%.1f ms (period %.1f ms, "
            "timeout %lu ms)",
            (unsigned long long)frameId, waitMs, FramePeriodMs(), timeoutMs);
    } else {
        Log("present",
            "WaitFrameEventX(%llu) already past deadline (%.1f ms elapsed)",
            (unsigned long long)frameId, elapsedMs);
    }
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
     "IMPLEMENTED (phase 2). Real passthrough to desktop D3D12CreateDevice; "
     "signature is D3D12CreateDevice-shaped (assumed) — the real Xbox form "
     "takes D3D12XBOX_CREATE_DEVICE_PARAMETERS; confirm vs exe."},
    {"d3d12_xs.dll", "SetFrameIntervalX", Status::kImplemented,
     (void*)HleSetFrameIntervalX,
     "IMPLEMENTED (phase 2). Real pacing: stores the interval."},
    {"d3d12_xs.dll", "ScheduleFrameEventX", Status::kImplemented,
     (void*)HleScheduleFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: stamps the frame."},
    {"d3d12_xs.dll", "WaitFrameEventX", Status::kImplemented,
     (void*)HleWaitFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: sleeps to the frame deadline."},
    // Audio.
    {"XAudio2_9.dll", "XAudio2Create", Status::kStub, (void*)HleXAudio2Create,
     "STUB. XMA2 decode is 0% everywhere; stub only."},
    // Input.
    {"gameinput.dll", "GameInputCreate", Status::kStub,
     (void*)HleGameInputCreate, "STUB."},
    // Storage.
    {"dstorage_xs.dll", "DStorageGetFactory", Status::kStub,
     (void*)HleDStorageGetFactory,
     "STUB. No queues/files yet; world streamer needs real I/O."},
    // Known boot-critical areas with no handler yet — tracked, not hidden.
    // (XGameSave, XPackage, XAsync, XSAPI areas are tracked in
    // docs/STATUS.md; only real import names go in this table.)
    {"xgameruntime.dll", "XUserAddAsync", Status::kNotImplemented, nullptr,
     "User model. Simple User Model default; handler TBD."},
    {"xgameruntime.dll", "XUserGetId", Status::kNotImplemented, nullptr,
     "TBD."},
};

static const char* kShimModules[] = {
    "xgameruntime.dll", "xgameplatform.dll", "d3d12_xs.dll", "d3d12_x.dll",
    "xmem.dll", "XAudio2_9.dll", "dstorage_xs.dll", "gameinput.dll",
    "xgame_save.dll", "xstore.dll",
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

// Ordinal table (synthetic ordinals — see scarlett.h). One entry per
// registered export, numbered in .def order per module.
static const OrdinalExport kOrdinalExports[] = {
    {"xgameruntime.dll", 1, (void*)HleXGameRuntimeInitialize,
     "XGameRuntimeInitialize"},
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
