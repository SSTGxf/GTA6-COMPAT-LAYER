// hle_registry.cpp — the HLE export registry (Xenia-style) + logging.
// Central table: (Xbox DLL, function) -> host implementation + status tag.
// The loader resolves guest imports through FindExport(); anything missing
// gets the E_NOTIMPL stub. Statuses are the project's tracking surface —
// see docs/STATUS.md.
#include "hle_registry.h"
#include "era_to_gdk.h"  // ERA -> GDK translation (phase 5)
#include "software_rt.h"  // Software ray tracing backend
#include "fixed_timestep.h"  // Fixed-timestep physics decoupling
#include "xbox_shims.h"
#include <mmsystem.h>  // timeBeginPeriod (frame pacing, phase 3)
#include <string.h>
#include <vector>

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

// ---- real-exe xmem additions (grounded 2026-11-23 from the exe's
// import table; XMemVirtualAlloc's 5-arg form from its call site at
// 0x14193e220, which passes type 0x20003000 and attributes 0x2004) ----
extern "C" void* HleXMemAllocDefault(SIZE_T size) {
    return HleXMemAlloc(size, 0);
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

// XMemAllocatePhysicalPages / XMemFreePhysicalPages — S_OK stubs.
// Xbox physical pages (GPU memory) have no desktop equivalent; the
// game allocates them for GPU-side use and the layer has no pool to
// hand out yet. The signatures are GDKX-gated; the stubs ignore args
// (x64 caller cleans the stack, so arg count does not matter).
extern "C" HRESULT HleXMemAllocatePhysicalPages(void) {
    Log("xmem",
        "XMemAllocatePhysicalPages() -> S_OK (STUB: no desktop "
        "physical-page pool)");
    return S_OK;
}

extern "C" HRESULT HleXMemFreePhysicalPages(void) {
    Log("xmem",
        "XMemFreePhysicalPages() -> S_OK (STUB: no desktop "
        "physical-page pool)");
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

extern "C" void* HleXMemMapPhysicalPagesScatter(
    const uint64_t* pPhysicalPages, uint32_t dwPageCount,
    DWORD dwMemoryAttributes, DWORD flProtect) {
    // Same guarded ordinary-memory mapping as XMemMapPhysicalPages
    // (scatter layout is GDKX-gated; the VA returned is one contiguous
    // block the game can write to).
    SIZE_T bytes = (SIZE_T)dwPageCount * 4096;
    if (bytes < 64 * 1024) bytes = 64 * 1024;
    if (bytes > 256 * 1024 * 1024) bytes = 256 * 1024 * 1024;
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    Log("xmem",
        "XMemMapPhysicalPagesScatter(pages %p, count %u, attrs 0x%x, "
        "protect 0x%x) -> %p (%llu bytes, ordinary memory)",
        (const void*)pPhysicalPages, dwPageCount, dwMemoryAttributes,
        flProtect, p, (unsigned long long)bytes);
    return p;
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
extern "C" LPVOID HleVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
                                  DWORD flAllocationType, DWORD flProtect) {
    // Desktop-valid flAllocationType bits. The Xbox bits 0x10000000 and
    // 0x40000000 are dropped here (0x30002000 -> MEM_RESERVE |
    // MEM_LARGE_PAGES; 0x40002000 -> MEM_RESERVE).
    constexpr DWORD kDesktopAllocType =
        MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_RESET_UNDO |
        MEM_LARGE_PAGES | MEM_PHYSICAL | MEM_TOP_DOWN | MEM_WRITE_WATCH;
    DWORD desktopType = flAllocationType & kDesktopAllocType;
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
    // it to the desktop base device.
    static const GUID kXboxDeviceIID = {
        0x5405C344, 0xD457, 0x444E,
        {0xB4, 0xDD, 0x23, 0x66, 0xE4, 0x5A, 0xEE, 0x39}};
    if (memcmp(&riid, &kXboxDeviceIID, sizeof(GUID)) == 0) {
        Log("d3d12xs",
            "D3D12XboxCreateDevice riid is the Xbox device IID "
            "{5405C344-D457-444E-B4DD-2366E45AEE39} (not desktop "
            "IID_ID3D12Device) — will map to the desktop base device");
    }
    HRESULT hr = E_FAIL;
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); ++i) {
        hr = D3D12CreateDevice(adapter, kLevels[i], riid, ppDevice);
        if (SUCCEEDED(hr)) {
            Log("d3d12xs",
                "D3D12XboxCreateDevice -> REAL desktop device %p "
                "(feature level 0x%x; Xbox fixed level is 12_2, best "
                "available on this hardware)",
                ppDevice ? *ppDevice : nullptr, (unsigned)kLevels[i]);
            break;
        }
        if (hr == E_NOINTERFACE) {
            // Unknown IID (the Xbox device IID, or another Xbox
            // interface the desktop driver does not know) — retry
            // with the desktop base IID_ID3D12Device. The Xbox device
            // interface extends the desktop base, so the base methods
            // are at the same vtable offsets; a base device is what
            // the game can use at boot. (The game will QueryInterface
            // for newer Xbox interfaces later — that needs the COM
            // wrapper, which is future work; device creation itself
            // must not fail here.)
            hr = D3D12CreateDevice(adapter, kLevels[i],
                                   __uuidof(ID3D12Device), ppDevice);
            if (SUCCEEDED(hr)) {
                Log("d3d12xs",
                    "D3D12XboxCreateDevice -> REAL desktop device %p "
                    "(feature level 0x%x; requested IID unknown to "
                    "the desktop driver, mapped to IID_ID3D12Device)",
                    ppDevice ? *ppDevice : nullptr,
                    (unsigned)kLevels[i]);
                break;
            }
        }
    }
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        if (g_xsDevice) g_xsDevice->Release();
        g_xsDevice = (ID3D12Device*)*ppDevice;
        g_xsDevice->AddRef();
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
struct StubComVtableInit {
    StubComVtableInit() {
        g_stubComVtable[0] = (void*)StubComQueryInterface;
        g_stubComVtable[1] = (void*)StubComAddRef;
        g_stubComVtable[2] = (void*)StubComRelease;
        for (int i = 3; i < 64; ++i)
            g_stubComVtable[i] = (void*)StubComGeneric;
    }
};
static StubComVtableInit g_stubComVtableInit;
static void* MakeStubComObject(const char* tag) {
    StubComObject* obj = (StubComObject*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StubComObject));
    if (!obj) return nullptr;
    obj->vtable = g_stubComVtable;
    obj->ref = 1;
    Log("stub", "%s stub COM object %p", tag, (void*)obj);
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

extern "C" HRESULT HleDStorageGetFactory(void** ppFactory) {
    Log("dstorage", "DStorageGetFactory() -> S_OK (STUB, no queues yet)");
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

// xg_xs.dll — S_OK stubs (audio DSP / texture computer / FSR loader / audio APU).
SC_DEFINE_SOK_STUB("xgxs", XGCreateTextureComputer)
SC_DEFINE_SOK_STUB("xgxs", XGSuggestSwizzleMode)

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
        if (timeBeginPeriod(1) == TIMERR_NOERROR) timerResRaised = true;
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
// Identifier is synthetic (local-only, no store install); the TitleId
// embedded in it is the GTA VI Scarlett TitleId from the package
// file map, for traceability.

static const char kPackageId[] = "ScarlettCompat-Guest-7952987A";
static const char kPackageDisplayName[] = "ScarlettCompat Guest";
static const char kPackageDescription[] =
    "Synthetic package presented by the ScarlettCompat layer "
    "(local-only, no store install)";
static const char kPackagePublisher[] = "ScarlettCompat";
static const char kPackageTitleId[] = "7952987A";

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
     "IMPLEMENTED (real-exe). Ordinary committed memory returned as the "
     "mapped VA (guarded size)."},
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
    {"xg_xs.dll", "XGCreateTextureComputer", Status::kStub,
     (void*)XGCreateTextureComputer, "STUB (real-exe import, 2026-11-23)."},
    {"xg_xs.dll", "XGSuggestSwizzleMode", Status::kStub,
     (void*)XGSuggestSwizzleMode, "STUB (real-exe import, 2026-11-23)."},

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
