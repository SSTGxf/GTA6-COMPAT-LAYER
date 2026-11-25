// era_to_gdk.cpp — ERA (Xbox One) -> GDK translation (phase 5).
// ERA entry points -> GDK equivalents. See docs/ERA-GDK-MAPPING.md.
// The INVENTORY (which ERA names the layer translates) and the
// D3D11XCreateDeviceX / D3DMapEsramMemory / DXGIXPresentArray
// SIGNATURES follow the public Xbox One shim-DLL d3d11.x.cpp
// (XDK-derived); the ORDINALS are the Xbox One shim-DLL exports
// (ERA titles import by name, so their tables give hints, not
// ordinals).
#include "era_to_gdk.h"
#include <roapi.h>     // RoGetActivationFactory (real one, for forwarding)
#include <winstring.h> // WindowsGetStringRawBuffer
#include <string.h>

namespace sc {

namespace {

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

bool StartsWithCi(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (!*s) return false;
        char ca = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
        char cb = (*prefix >= 'A' && *prefix <= 'Z')
                      ? (char)(*prefix + 32)
                      : *prefix;
        if (ca != cb) return false;
        ++s;
        ++prefix;
    }
    return true;
}

}  // namespace

// ERA modules the layer translates. None of these exist on desktop
// ERA modules (phase 5): the Xbox One shim DLLs. A title importing any
// of these is ERA. AcpHal.dll is NOT in this list: it is the audio DSP
// HAL, shared across Xbox One and Series X — the real title (GDK)
// exe imports it (9 imports), so counting it as ERA mis-detected a
// GDK title as ERA (boot #1, 2026-11-23). The _x modules below are
// the ERA-only signal. AcpHal.dll still resolves through the HLE
// registry (kShimModules), never LoadLibrary — that is separate from
// this detection list.
static const char* kEraModules[] = {
    "d3d11_x.dll", "d3d12_x.dll", "kernelx.dll", "xg_x.dll",
};

bool EraIsEraModule(const char* module) {
    if (!module) return false;
    for (size_t i = 0; i < sizeof(kEraModules) / sizeof(kEraModules[0]);
         ++i) {
        if (CiEq(kEraModules[i], module)) return true;
    }
    return false;
}

// ---- ERA graphics: D3D11.x -> desktop D3D11 (adaptation) ----
// GDK on Xbox does not support D3D11.x at all (Microsoft Learn), so a
// D3D11.x title has no d3d12_xs equivalent — the faithful host is a real
// desktop D3D11 device. D3D11XCreateDeviceX's real form follows the
// public Xbox One shim-DLL d3d11.x.cpp (XDK-derived): three args — a
// D3D11X_CREATE_DEVICE_PARAMETERS pointer, a device out-pointer, and a
// context out-pointer. (Phase 5 first assumed an 11-arg
// D3D11CreateDevice-shaped form; that was wrong — the real exe calls
// three args, and an 11-arg callee would read stack garbage for the
// device out-pointer.)

extern "C" HRESULT EraD3D11XCreateDevice(
    IUnknown* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter*)pAdapter, DriverType,
                                   Software, Flags, pFeatureLevels,
                                   FeatureLevels, SDKVersion, ppDevice,
                                   pFeatureLevel, ppImmediateContext);
    Log("era-d3d11x",
        "D3D11XCreateDevice -> desktop D3D11 device %p (hr=0x%08x, "
        "FL 0x%x)",
        ppDevice && *ppDevice ? *ppDevice : nullptr, (unsigned)hr,
        pFeatureLevel ? (unsigned)*pFeatureLevel : 0);
    return hr;
}

extern "C" HRESULT EraD3D11XCreateDeviceAndSwapChain(
    IUnknown* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        (IDXGIAdapter*)pAdapter, DriverType, Software, Flags, pFeatureLevels,
        FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
        pFeatureLevel, ppImmediateContext);
    Log("era-d3d11x",
        "D3D11XCreateDeviceAndSwapChain -> device %p swapchain %p "
        "(hr=0x%08x, FL 0x%x)",
        ppDevice && *ppDevice ? *ppDevice : nullptr,
        ppSwapChain && *ppSwapChain ? *ppSwapChain : nullptr, (unsigned)hr,
        pFeatureLevel ? (unsigned)*pFeatureLevel : 0);
    return hr;
}

// ---- ERA kernel memory (1:1 maps + defaults) ----
// The Default pair are
// the implementations the CRT's alloc hooks point at on console; on
// the host they are the same real HeapAlloc/Free as XMemAlloc/Free.

extern "C" void* EraXMemAllocDefault(SIZE_T size, DWORD attrs) {
    void* p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    Log("era-xmem", "XMemAllocDefault(%llu, attrs 0x%x) -> %p",
        (unsigned long long)size, attrs, p);
    return p;
}

extern "C" void EraXMemFreeDefault(void* p) {
    Log("era-xmem", "XMemFreeDefault(%p)", p);
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

extern "C" HRESULT EraXMemSetAllocationHooks(void* allocHook,
                                             void* freeHook) {
    // Accepted, logged, not routed: the layer's allocations do not
    // call back into title hooks. A title that depends on its hooks
    // being called for every allocation will not see them — documented
    // in docs/ERA-GDK-MAPPING.md.
    Log("era-xmem",
        "XMemSetAllocationHooks(alloc=%p, free=%p) -> S_OK (accepted, "
        "not routed)",
        allocHook, freeHook);
    return S_OK;
}

extern "C" DWORD EraTitleMemoryStatus(void* status) {
    // ASSUMED signature (one pointer, XDK-gated struct layout):
    // zero 64 bytes at the pointer and return 0. Zeros mean "unknown"
    // rather than a fabricated budget — a title that reads a total /
    // available pair out of this struct will see 0, which is the
    // honest value for "the layer does not know the console budget".
    if (status) memset(status, 0, 64);
    Log("era-xmem",
        "TitleMemoryStatus(%p) -> 0 (ASSUMED signature; struct "
        "zero-filled, no fabricated budget)",
        status);
    return 0;
}

// D3D11XCreateDeviceX — the device entry (Xbox One shim-DLL ordinal
// 4). 3-arg form (the public Xbox One shim-DLL d3d11.x.cpp):
// (pParameters, ppDevice, ppImmediateContext). The params struct's
// ESRAM/display/ring fields have no desktop meaning and are logged,
// not acted on; a NULL struct means defaults. Desktop D3D11 manages
// its own command rings, so the ring sizes are informational only.
extern "C" HRESULT EraD3D11XCreateDeviceX(
    const D3D11X_CREATE_DEVICE_PARAMETERS* pParameters,
    ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext) {
    if (pParameters) {
        Log("era-d3d11x",
            "D3D11XCreateDeviceX(params version=%u flags=0x%x "
            "deRing=%u ceRing=%u deSeg=%u ceSeg=%u)",
            pParameters->Version, pParameters->Flags,
            pParameters->ImmediateContextDeRingSizeBytes,
            pParameters->ImmediateContextCeRingSizeBytes,
            pParameters->ImmediateContextDeSegmentSizeBytes,
            pParameters->ImmediateContextCeSegmentSizeBytes);
    } else {
        Log("era-d3d11x",
            "D3D11XCreateDeviceX(params=NULL, defaults)");
    }
    if (!ppDevice || !ppImmediateContext) return E_INVALIDARG;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, 0, fls, 1, D3D11_SDK_VERSION,
                                   ppDevice, &fl, ppImmediateContext);
    Log("era-d3d11x",
        "D3D11XCreateDeviceX -> desktop D3D11 device %p (hr=0x%08x, "
        "FL 0x%x)",
        ppDevice && *ppDevice ? *ppDevice : nullptr, (unsigned)hr,
        (unsigned)fl);
    return hr;
}

// ---- ERA ESRAM (Xbox One, 32 MB on-die SRAM) ----
// NOT the Xbox 360's 10 MB EDRAM. On the console, ESRAM is a separate
// 32 MB high-bandwidth pool the title manages explicitly: render
// targets and depth buffers that fit live there, anything bigger is
// tiled and resolved to DRAM. The ESRAM entry point is
// D3DMapEsramMemory (Xbox One shim-DLL ordinal 9) — signature from
// the public Xbox One shim-DLL d3d11.x.cpp: (Flags, pVirtualAddress,
// NumPages, pPageList). It maps ESRAM pages into the title's virtual
// address space: Flags bit 0 = large pages, bit 1 = 4 MB pages;
// pPageList holds the ESRAM page numbers (4 KB pages) to map at
// pVirtualAddress.
//
// Host translation: there is no ESRAM on a PC, so the layer maps REAL
// committed memory at the requested VA (VirtualAlloc) and enforces the
// 32 MB arena budget with a counter — a mapping that would push the
// total past 32 MB fails with E_OUTOFMEMORY, the same wall the console
// has. Page numbers are validated against the arena (0..8191) and
// logged. Two honest limits, documented in docs/ERA-GDK-MAPPING.md:
// (1) the mapped window is system RAM, not GPU memory — a title that
// writes through the window and then expects the GPU to see the data
// must copy it into a D3D11 resource itself (UpdateSubresource /
// CopyResource), exactly as the era guest proves; (2) two VAs mapping
// the same ESRAM page are independent copies on the host (no VA
// aliasing), where the console aliases them to one physical page.
// Render targets themselves need no ESRAM handling: an ERA D3D11.x
// render target is created as an ordinary desktop D3D11 texture in
// GPU memory, and ResolveSubresource / CopyResource on it has exactly
// the console resolve's semantics — the data is already where the
// resolve reads it, so the resolve is a real copy, not a stub.
static const SIZE_T kEsramArenaBytes = 32ull * 1024 * 1024;  // 32 MB
static const UINT kEsramPageBytes = 4096;
static const UINT kEsramPageCount =
    (UINT)(kEsramArenaBytes / kEsramPageBytes);  // 8192 pages
static SIZE_T g_esramMappedBytes = 0;
static CRITICAL_SECTION g_esramCs;
static bool g_esramCsInit = false;

static void EsramLock() {
    if (!g_esramCsInit) {
        InitializeCriticalSection(&g_esramCs);
        g_esramCsInit = true;
    }
    EnterCriticalSection(&g_esramCs);
}

extern "C" HRESULT EraD3DMapEsramMemory(UINT Flags, void* pVirtualAddress,
                                        UINT NumPages,
                                        const UINT* pPageList) {
    if (NumPages == 0) return E_INVALIDARG;
    if (!pPageList) return E_INVALIDARG;
    SIZE_T bytes = (SIZE_T)NumPages * kEsramPageBytes;
    if (bytes > kEsramArenaBytes) return E_OUTOFMEMORY;
    // Validate every page number against the 32 MB arena.
    UINT firstPage = pPageList[0], lastPage = pPageList[0];
    for (UINT i = 0; i < NumPages; ++i) {
        if (pPageList[i] >= kEsramPageCount) {
            Log("era-esram",
                "D3DMapEsramMemory: page %u out of range (arena %u "
                "pages) -> E_INVALIDARG",
                pPageList[i], kEsramPageCount);
            return E_INVALIDARG;
        }
        if (pPageList[i] < firstPage) firstPage = pPageList[i];
        if (pPageList[i] > lastPage) lastPage = pPageList[i];
    }
    if (Flags & ~0x3u) {
        Log("era-esram",
            "D3DMapEsramMemory: unknown flag bits 0x%x (ignored)",
            Flags & ~0x3u);
    }
    EsramLock();
    if (g_esramMappedBytes + bytes > kEsramArenaBytes) {
        SIZE_T used = g_esramMappedBytes;
        LeaveCriticalSection(&g_esramCs);
        Log("era-esram",
            "D3DMapEsramMemory: %u pages (%llu bytes) would exceed the "
            "32 MB arena (%llu mapped) -> E_OUTOFMEMORY",
            NumPages, (unsigned long long)bytes,
            (unsigned long long)used);
        return E_OUTOFMEMORY;
    }
    // Map real memory at the requested VA. NULL means "pick an
    // address". A non-NULL VA is expected to be reserved by the title
    // (VirtualAlloc MEM_RESERVE) — commit into it; if that fails, fall
    // back to a fresh reservation at the requested address.
    void* mapped = nullptr;
    if (pVirtualAddress) {
        mapped = VirtualAlloc(pVirtualAddress, bytes, MEM_COMMIT,
                              PAGE_READWRITE);
        if (!mapped) {
            mapped = VirtualAlloc(pVirtualAddress, bytes,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
    } else {
        mapped = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    }
    if (!mapped) {
        LeaveCriticalSection(&g_esramCs);
        Log("era-esram",
            "D3DMapEsramMemory: VirtualAlloc(%p, %llu) failed (%lu) -> "
            "E_OUTOFMEMORY",
            pVirtualAddress, (unsigned long long)bytes, GetLastError());
        return E_OUTOFMEMORY;
    }
    g_esramMappedBytes += bytes;
    SIZE_T used = g_esramMappedBytes;
    LeaveCriticalSection(&g_esramCs);
    Log("era-esram",
        "D3DMapEsramMemory(flags=0x%x, va=%p, pages=%u [%u..%u], "
        "mapped=%p, arena %llu/33554432 bytes) -> S_OK",
        Flags, pVirtualAddress, NumPages, firstPage, lastPage, mapped,
        (unsigned long long)used);
    return S_OK;
}

// ---- ERA multi-swapchain present (DXGIXPresentArray) ----
// Signature from the public Xbox One shim-DLL d3d11.x.cpp:
// (SyncInterval, PresentImmediateThreshold, Flags, NumSwapChains,
// ppSwapChains, pPresentParameters). The translation presents every
// swapchain in the array with the given SyncInterval through the real
// desktop swapchains (created by D3D11XCreateDeviceAndSwapChain). The
// per-swapchain present parameters (source rect, scale factor) are
// accepted and ignored — the full back buffer is presented. A failure
// on one swapchain does not stop the rest; the last failure is
// returned.
extern "C" HRESULT EraDXGIXPresentArray(
    UINT SyncInterval, UINT PresentImmediateThreshold, UINT Flags,
    UINT NumSwapChains, void** ppSwapChains, const void* pPresentParameters) {
    (void)PresentImmediateThreshold;
    (void)Flags;
    (void)pPresentParameters;
    if (NumSwapChains == 0 || !ppSwapChains) return E_INVALIDARG;
    HRESULT hr = S_OK;
    UINT presented = 0;
    for (UINT i = 0; i < NumSwapChains; ++i) {
        IDXGISwapChain* sc = (IDXGISwapChain*)ppSwapChains[i];
        if (!sc) {
            hr = E_INVALIDARG;
            continue;
        }
        HRESULT h = sc->Present(SyncInterval, 0);
        if (SUCCEEDED(h)) {
            ++presented;
        } else {
            hr = h;
        }
    }
    Log("era-d3d11x",
        "DXGIXPresentArray(sync=%u, chains=%u, presented=%u) -> "
        "hr=0x%08x",
        SyncInterval, NumSwapChains, presented, (unsigned)hr);
    return hr;
}

// ---- ERA graphics memory (D3DAllocateGraphicsMemory /
// D3DFreeGraphicsMemory) ----
// Shape from the public Xbox One shim-DLL d3d11.x.cpp: allocate GPU
// memory at a desired GPU virtual address. On the host the allocation
// is plain VirtualAlloc (PAGE_READWRITE) — CPU-visible, like the
// console's graphics memory from the CPU's side. Alignment over 128 KB
// is rejected (same limit as the Xbox One shim-DLL's). The Flags
// coherency hints are accepted and logged; the host protection is
// uniform PAGE_READWRITE. They are supporting infrastructure for the
// ESRAM work, tested by the era guest.
extern "C" HRESULT EraD3DAllocateGraphicsMemory(
    SIZE_T SizeInBytes, UINT64 AlignmentBytes,
    UINT64 DesiredGpuVirtualAddress, UINT Flags, void** ppAddress) {
    if (!ppAddress || AlignmentBytes > 0x20000) return E_INVALIDARG;
    if (SizeInBytes == 0) return E_INVALIDARG;
    void* p = VirtualAlloc((LPVOID)DesiredGpuVirtualAddress, SizeInBytes,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return E_OUTOFMEMORY;
    *ppAddress = p;
    Log("era-d3d11x",
        "D3DAllocateGraphicsMemory(%llu bytes, align %llu, va 0x%llx, "
        "flags 0x%x) -> %p",
        (unsigned long long)SizeInBytes, (unsigned long long)AlignmentBytes,
        (unsigned long long)DesiredGpuVirtualAddress, Flags, p);
    return S_OK;
}

extern "C" HRESULT EraD3DFreeGraphicsMemory(void* pAddress) {
    if (!pAddress) return E_INVALIDARG;
    if (!VirtualFree(pAddress, 0, MEM_RELEASE)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Log("era-d3d11x", "D3DFreeGraphicsMemory(%p) -> hr=0x%08x",
            pAddress, (unsigned)hr);
        return hr;
    }
    Log("era-d3d11x", "D3DFreeGraphicsMemory(%p) -> S_OK", pAddress);
    return S_OK;
}

// ---- no-GDK-equivalent stubs (explicit E_NOTIMPL, logged) ----
// Each stub logs its first four raw register args and returns
// E_NOTIMPL. The declared 4-arg form is for logging only: on x64 the
// caller owns stack cleanup, so a callee that reads fewer args than
// the caller passed is safe. The real signatures are XDK-gated.

#define ERA_NOTIMPL_STUB(fnName, tag)                                        \
    extern "C" HRESULT fnName(uintptr_t a0, uintptr_t a1, uintptr_t a2,        \
                              uintptr_t a3) {                                  \
        Log(tag, #fnName "(%p, %p, %p, %p) -> E_NOTIMPL (no GDK "            \
                   "equivalent; XDK-gated signature)",                        \
            (void*)a0, (void*)a1, (void*)a2, (void*)a3);                      \
        return E_NOTIMPL;                                                      \
    }

ERA_NOTIMPL_STUB(EraXGComputeOptimalDepthStencilTileModes, "era-xg")
ERA_NOTIMPL_STUB(EraXGComputeOptimalTileMode, "era-xg")
ERA_NOTIMPL_STUB(EraXGComputeTexture2DLayout, "era-xg")
ERA_NOTIMPL_STUB(EraXGComputeTexture3DLayout, "era-xg")
ERA_NOTIMPL_STUB(EraXGCreateTexture1DComputer, "era-xg")
ERA_NOTIMPL_STUB(EraXGCreateTexture2DComputer, "era-xg")
ERA_NOTIMPL_STUB(EraXGCreateTexture3DComputer, "era-xg")
ERA_NOTIMPL_STUB(EraAcpHalReleaseShapeContexts, "era-acphal")
ERA_NOTIMPL_STUB(EraApuFree, "era-acphal")
ERA_NOTIMPL_STUB(EraApuIsVirtualAddressValid, "era-acphal")
ERA_NOTIMPL_STUB(EraApuMapApuAddress, "era-acphal")
ERA_NOTIMPL_STUB(EraApuMapVirtualAddress, "era-acphal")

#undef ERA_NOTIMPL_STUB

// ---- APU audio heap + AcpHal context (boots #184/#185) ----
// The title's audio bring-up lives at guest 0x1443AD8B0 and runs, in order:
//
//   0x1443AD933  call ApuCreateHeap(0x1C700000, 0x226E00, -1, -1)
//   0x1443AD938  test eax,eax / js 0x1443AD94E      <- int3 on ANY failure
//   0x1443AD93C  call 0x1443AEC40 -> AcpHalAllocateShapeContexts(&ctx)
//                                   0x1443AEC73, js -> return 0 -> int3
//                                   then AcpHalCreate(&ctx) 0x1443AED91
//   0x1443AD945  call 0x1443AEE50 -> XApuConnect (already an S_OK stub)
//
// Boot #184 stopped here: ApuCreateHeap returned E_NOTIMPL, the sign test
// failed, and the guest hit its own int3 at 0x1443AD94E. The heap is a plain
// window the title owns (base 0x1C700000 = 0x1C7 << 20, 0x226E00 bytes), so it
// is modelled as committed host memory at the requested base — the same
// treatment EraD3DAllocateGraphicsMemory already gives the guest's GPU VAs.
// The two shape-context calls have no desktop equivalent (there are no DSP
// shapes); they answer S_OK and say so, like the xapu.dll stubs.
static uintptr_t g_apuHeapBase = 0;
static uintptr_t g_apuHeapSize = 0;

extern "C" HRESULT EraApuCreateHeap(uintptr_t base, uintptr_t size,
                                    uintptr_t a2, uintptr_t a3) {
    if (base == 0 || size == 0) {
        Log("era-acphal",
            "ApuCreateHeap(base=0x%llx size=0x%llx a2=0x%llx a3=0x%llx) "
            "-> E_INVALIDARG (empty window)",
            (unsigned long long)base, (unsigned long long)size,
            (unsigned long long)a2, (unsigned long long)a3);
        return E_INVALIDARG;
    }
    void* p = VirtualAlloc((LPVOID)base, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
    if (!p) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Log("era-acphal",
            "ApuCreateHeap(base=0x%llx size=0x%llx a2=0x%llx a3=0x%llx) "
            "-> VirtualAlloc failed hr=0x%08x",
            (unsigned long long)base, (unsigned long long)size,
            (unsigned long long)a2, (unsigned long long)a3, (unsigned)hr);
        return hr;
    }
    g_apuHeapBase = base;
    g_apuHeapSize = size;
    Log("era-acphal",
        "ApuCreateHeap(base=0x%llx size=0x%llx a2=0x%llx a3=0x%llx) "
        "-> S_OK heap=%p (committed host memory, no APU on desktop)",
        (unsigned long long)base, (unsigned long long)size,
        (unsigned long long)a2, (unsigned long long)a3, p);
    return S_OK;
}

// ApuAlloc(out, flags, size, align, kind) — measured from the guest's own
// code (boots #187/#188 dumped the call), not from XDK documentation:
//   0x1443A6110 is a 7-instruction wrapper around the AcpHal thunk:
//     rcx = caller's rcx        -> ApuAlloc arg1, forwarded untouched (&out)
//     edx = 0                   -> ApuAlloc arg2 (flags)
//     eax = caller's edx        -> ApuAlloc r8d (size)
//     r9d = 0x800               -> ApuAlloc arg4 (alignment)
//     caller's r8d              -> [rsp+0x20] (5th argument, a 32-bit kind)
//   Its one real caller 0x1443AED13 (the shape-context builder) does
//     lea rsi,[rsp+0x30] / mov qword [rsi],0 / call / mov r15,[rsi]
//   and then bumps that slot by 0x1700 then 0x200 for each of its 350 table
//   entries, zeroing both ranges through the published pointer. The return
//   value (eax) is never inspected: the out slot is the whole contract.
// Boots #187/#188 answered E_NOTIMPL and never wrote the slot, so the guest
// kept *out = 0 and died writing through it (0xc0000005 write at 0x20 in
// VCRUNTIME140, one line after the call).
static uintptr_t g_apuAllocUsed = 0;  // bump offset inside the APU heap
static uintptr_t g_apuArenaEnd = 0;   // first free VA above the APU heap

extern "C" HRESULT EraApuAlloc(uintptr_t outPtr, uintptr_t flags,
                               uintptr_t size, uintptr_t align) {
    if (outPtr == 0) {
        Log("era-acphal",
            "ApuAlloc(out=null flags=0x%llx size=0x%llx align=0x%llx) -> "
            "E_INVALIDARG (no out slot)",
            (unsigned long long)flags, (unsigned long long)size,
            (unsigned long long)align);
        return E_INVALIDARG;
    }
    if (size == 0) {
        Log("era-acphal",
            "ApuAlloc(out=%p flags=0x%llx size=0 align=0x%llx) -> "
            "E_INVALIDARG (empty request)",
            (void*)outPtr, (unsigned long long)flags,
            (unsigned long long)align);
        return E_INVALIDARG;
    }
    if (g_apuHeapBase == 0 || g_apuHeapSize == 0) {
        Log("era-acphal",
            "ApuAlloc(out=%p size=0x%llx) -> E_FAIL (ApuCreateHeap has not "
            "created the APU window)",
            (void*)outPtr, (unsigned long long)size);
        return E_FAIL;
    }
    const uintptr_t gran = align ? align : 8;
    if ((gran & (gran - 1)) != 0) {
        Log("era-acphal",
            "ApuAlloc(out=%p size=0x%llx align=0x%llx) -> E_INVALIDARG "
            "(alignment is not a power of two)",
            (void*)outPtr, (unsigned long long)size,
            (unsigned long long)align);
        return E_INVALIDARG;
    }
    const uintptr_t heapEnd = g_apuHeapBase + g_apuHeapSize;
    uintptr_t start = (g_apuHeapBase + g_apuAllocUsed + (gran - 1)) & ~(gran - 1);
    if (size > g_apuHeapSize || start < g_apuHeapBase || start + size < start ||
        start + size > heapEnd) {
        // Boot #191: the title makes a second request far larger than the
        // 0x226E00 heap it declared (0x1913AFA0 = 421 MB) and memsets through
        // whatever comes back, so an exhausted window cannot just fail. The
        // APU arena rises from the heap end, 64 KiB-aligned, so every pointer
        // the title gets is still inside its own APU VA neighbourhood rather
        // than some unrelated host allocation.
        uintptr_t cur = g_apuArenaEnd ? g_apuArenaEnd
                                      : (heapEnd + 0xFFFF) & ~(uintptr_t)0xFFFF;
        uintptr_t want = (cur + (gran - 1)) & ~(gran - 1);
        SIZE_T bytes = (SIZE_T)((size + 0xFFFF) & ~(uintptr_t)0xFFFF);
        void* p = VirtualAlloc((LPVOID)want, bytes, MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
        if (!p) {
            p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
        }
        if (!p) {
            Log("era-acphal",
                "ApuAlloc(out=%p size=0x%llx align=0x%llx) -> E_OUTOFMEMORY "
                "(heap used 0x%llx of 0x%llx, arena request failed)",
                (void*)outPtr, (unsigned long long)size,
                (unsigned long long)align, (unsigned long long)g_apuAllocUsed,
                (unsigned long long)g_apuHeapSize);
            return E_OUTOFMEMORY;
        }
        g_apuArenaEnd = (uintptr_t)p + bytes;
        *(void**)outPtr = p;
        Log("era-acphal",
            "ApuAlloc(out=%p size=0x%llx align=0x%llx) -> S_OK arena block=%p "
            "(0x%llx bytes, too big for the 0x%llx heap at 0x%llx; committed "
            "above it inside the APU VA space)",
            (void*)outPtr, (unsigned long long)size, (unsigned long long)align,
            p, (unsigned long long)bytes,
            (unsigned long long)g_apuHeapSize,
            (unsigned long long)g_apuHeapBase);
        return S_OK;
    }
    // The guest zeroes both sub-ranges itself, but the window is recycled host
    // memory, so hand out a clean block like an APU allocation would be.
    ZeroMemory((void*)start, (SIZE_T)size);
    g_apuAllocUsed = (start - g_apuHeapBase) + size;
    *(void**)outPtr = (void*)start;
    Log("era-acphal",
        "ApuAlloc(out=%p flags=0x%llx size=0x%llx align=0x%llx) -> S_OK "
        "block=%p used=0x%llx/0x%llx of the APU heap",
        (void*)outPtr, (unsigned long long)flags, (unsigned long long)size,
        (unsigned long long)align, (void*)start,
        (unsigned long long)g_apuAllocUsed, (unsigned long long)g_apuHeapSize);
    return S_OK;
}

// AcpHalAllocateShapeContexts(ctx, allocator, 0x7FFF...FC, 0). The guest's
// own code (0x1443AEC40) shows the contract without any XDK documentation:
//   +0x10  u64 count = 0x15E, set by the guest BEFORE the call
//   +0x38  void*  array, read right AFTER the call and memset to count<<6
// so the callee must allocate count * 64 bytes and publish it at +0x38.
// Boot #185 returned S_OK without filling +0x38 and the guest then wrote
// through the stale pointer (0xc0000005, write, address 0x20).
extern "C" HRESULT EraAcpHalAllocateShapeContexts(uintptr_t ctx,
                                                  uintptr_t a1,
                                                  uintptr_t a2,
                                                  uintptr_t a3) {
    if (ctx == 0) return E_INVALIDARG;
    uint8_t* base = (uint8_t*)ctx;
    uint64_t count = *(const uint64_t*)(base + 0x10);
    if (count == 0 || count > 0x10000) {
        Log("era-acphal",
            "AcpHalAllocateShapeContexts(ctx=%p, %p, %p, %p) -> E_INVALIDARG "
            "(count %llu out of range)",
            (void*)ctx, (void*)a1, (void*)a2, (void*)a3,
            (unsigned long long)count);
        return E_INVALIDARG;
    }
    const SIZE_T bytes = (SIZE_T)count * 64;
    void* array = calloc(1, bytes);
    if (!array) return E_OUTOFMEMORY;
    *(void**)(base + 0x38) = array;
    Log("era-acphal",
        "AcpHalAllocateShapeContexts(ctx=%p, %p, %p, %p) -> S_OK "
        "count=%llu array=%p bytes=%llu (shape records are 64 bytes; "
        "host memory, no DSP on desktop)",
        (void*)ctx, (void*)a1, (void*)a2, (void*)a3,
        (unsigned long long)count, array, (unsigned long long)bytes);
    return S_OK;
}

// ---- the ACP HAL object the guest keeps at ctx+0 (boots #189/#190) ----
// The audio init calls AcpHalCreate(&g_acpContext, 0x20, 0xE0, 0xE0) at
// 0x1443AED91 and then, with no call in between, dereferences the slot it is
// supposed to fill:
//
//   0x1443AEDA5  mov  rcx, [0x149AE98B8]   ; the slot AcpHalCreate must fill
//   0x1443AEDB7  mov  rax, [rcx]           ; object[0] = vtable pointer
//   0x1443AEDBA  call [rax]                ; slot 0: edx=0x1000, r8d=0x1000
//
// and later on the same object:
//
//   0x1443AEDFC  mov  rcx, [0x149AE98B8]
//   0x1443AEE1F  mov  r10, [rcx]
//   0x1443AEE2F  call [r10+0x10]           ; slot 2: edx=2, r8d=0xF0F0F0F0,
//                                          ; r9d=0xFFFFFFFF,
//                                          ; [rsp+0x20]=&{0xC0}, [rsp+0x28]=0
//
// Both returns are only sign-checked (`test eax,eax; js 0x1443AEE3A` -> the
// init fails), so the contract at ctx+0 is exactly: a pointer to an object
// whose first word is a vtable, whose slots 0 and 2 answer a non-negative
// HRESULT. There is no DSP on desktop, so the methods are logged stubs; the
// vtable covers 16 slots so any further slot the title calls answers honestly
// instead of dereferencing a wild pointer.
struct EraAcpHalObject;
typedef HRESULT (*EraAcpHalMethod)(EraAcpHalObject* self, uintptr_t a1,
                                  uintptr_t a2, uintptr_t a3, uintptr_t a4,
                                  uintptr_t a5);
struct EraAcpHalObject {
    EraAcpHalMethod* vtbl;
};

static HRESULT EraAcpHalObjectDispatch(int slot, EraAcpHalObject* self,
                                       uintptr_t a1, uintptr_t a2,
                                       uintptr_t a3, uintptr_t a4,
                                       uintptr_t a5) {
    if (slot == 0) {
        Log("era-acphal",
            "AcpHalObject::slot0(this=%p, 0x%llx, 0x%llx) -> S_OK "
            "(stub: no DSP command ring on desktop)",
            (void*)self, (unsigned long long)a1, (unsigned long long)a2);
        return S_OK;
    }
    if (slot == 2) {
        Log("era-acphal",
            "AcpHalObject::slot2(this=%p, 0x%llx, 0x%llx, 0x%llx, out=%p, 0x%llx) "
            "-> S_OK (stub: no DSP shapes; out not written)",
            (void*)self, (unsigned long long)a1, (unsigned long long)a2,
            (unsigned long long)a3, (void*)a4, (unsigned long long)a5);
        return S_OK;
    }
    Log("era-acphal",
        "AcpHalObject::slot%d(this=%p, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) "
        "-> S_OK (stub)",
        slot, (void*)self, (unsigned long long)a1, (unsigned long long)a2,
        (unsigned long long)a3, (unsigned long long)a4,
        (unsigned long long)a5);
    return S_OK;
}

#define ERA_ACP_SLOT(n)                                                     \
    extern "C" HRESULT EraAcpHalSlot##n(EraAcpHalObject* self, uintptr_t a1, \
                                         uintptr_t a2, uintptr_t a3,       \
                                         uintptr_t a4, uintptr_t a5) {     \
        return EraAcpHalObjectDispatch(n, self, a1, a2, a3, a4, a5);         \
    }
ERA_ACP_SLOT(0)
ERA_ACP_SLOT(1)
ERA_ACP_SLOT(2)
ERA_ACP_SLOT(3)
ERA_ACP_SLOT(4)
ERA_ACP_SLOT(5)
ERA_ACP_SLOT(6)
ERA_ACP_SLOT(7)
ERA_ACP_SLOT(8)
ERA_ACP_SLOT(9)
ERA_ACP_SLOT(10)
ERA_ACP_SLOT(11)
ERA_ACP_SLOT(12)
ERA_ACP_SLOT(13)
ERA_ACP_SLOT(14)
ERA_ACP_SLOT(15)
#undef ERA_ACP_SLOT

static EraAcpHalMethod g_acpHalVtbl[16] = {
    EraAcpHalSlot0,  EraAcpHalSlot1,  EraAcpHalSlot2,  EraAcpHalSlot3,
    EraAcpHalSlot4,  EraAcpHalSlot5,  EraAcpHalSlot6,  EraAcpHalSlot7,
    EraAcpHalSlot8,  EraAcpHalSlot9,  EraAcpHalSlot10, EraAcpHalSlot11,
    EraAcpHalSlot12, EraAcpHalSlot13, EraAcpHalSlot14, EraAcpHalSlot15};
static EraAcpHalObject g_acpHalObject = {g_acpHalVtbl};

extern "C" HRESULT EraAcpHalCreate(uintptr_t ctx, uintptr_t a1, uintptr_t a2,
                                   uintptr_t a3) {
    if (ctx == 0) {
        Log("era-acphal",
            "AcpHalCreate(ctx=null, %p, %p, %p) -> E_INVALIDARG",
            (void*)a1, (void*)a2, (void*)a3);
        return E_INVALIDARG;
    }
    *(void**)ctx = &g_acpHalObject;
    Log("era-acphal",
        "AcpHalCreate(ctx=%p, %p, %p, %p) -> S_OK object=%p vtable=%p "
        "(slot0(this,0x1000,0x1000) and slot2(this,2,0xF0F0F0F0,0xFFFFFFFF,out,0) "
        "are the two the guest calls; both only sign-checked; S_OK stubs)",
        (void*)ctx, (void*)a1, (void*)a2, (void*)a3, (void*)&g_acpHalObject,
        (void*)g_acpHalVtbl);
    return S_OK;
}

// ---- WinRT activation routing ----
// The real RoGetActivationFactory, stashed by the loader when it
// installs the hook. Forwarding target for every non-Xbox class name.
static RoGetActivationFactoryFn g_realRoGetActivationFactory = nullptr;

void EraSetRealRoGetActivationFactory(RoGetActivationFactoryFn realFn) {
    g_realRoGetActivationFactory = realFn;
}

// Xbox-namespace WinRT classes have no layer COM object yet (their
// interface definitions are XDK-gated), so activation is refused
// explicitly instead of faked. Everything else forwards to the real
// RoGetActivationFactory, so normal WinRT keeps working.
extern "C" HRESULT EraRoGetActivationFactory(HSTRING activatableClassId,
                                             REFIID iid, void** factory) {
    char className[256] = {};
    UINT32 len = 0;
    PCWSTR raw = nullptr;
    if (activatableClassId)
        raw = WindowsGetStringRawBuffer(activatableClassId, &len);
    if (raw && len > 0 && len < sizeof(className)) {
        // HSTRING is UTF-16; Xbox class names are ASCII, so narrow them.
        for (UINT32 i = 0; i < len; ++i)
            className[i] = (char)(raw[i] & 0x7F);
    }
    bool isXbox = StartsWithCi(className, "Windows.Xbox.") ||
                  CiEq(className, "Windows.ApplicationModel.Package");
    if (isXbox) {
        Log("era-winrt",
            "RoGetActivationFactory(\"%s\") -> REGDB_E_CLASSNOTREG (no "
            "layer COM object yet; XDK-gated interface definitions)",
            className);
        return REGDB_E_CLASSNOTREG;  // 0x80040154
    }
    if (!g_realRoGetActivationFactory) {
        Log("era-winrt",
            "RoGetActivationFactory(\"%s\") -> E_FAIL (no real function "
            "stashed)",
            className);
        return E_FAIL;
    }
    HRESULT hr = g_realRoGetActivationFactory(activatableClassId, iid,
                                              factory);
    Log("era-winrt",
        "RoGetActivationFactory(\"%s\") -> forwarded hr=0x%08x",
        className, (unsigned)hr);
    return hr;
}

}  // namespace sc
