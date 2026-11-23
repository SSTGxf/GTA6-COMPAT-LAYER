// scarlett.cpp — ScarlettCompat layer DLL.
// Exports the loader API (ScLoadXboxPe / ScRunXboxPe) and the HLE shim
// functions, so guest titles (and the verification harness) link against
// this one DLL. Shim import libs for the Xbox DLL names are generated from
// defs/*.def; at runtime the loader resolves Xbox imports to these
// functions directly — it never loads a real xgameruntime.dll / d3d12_xs.dll
// (and never lets Windows' own System32 xgameruntime.dll win the search).
#include "scarlett.h"
#include "pe_loader.h"
#include "hle_registry.h"

#define SC_API extern "C" __declspec(dllexport)

namespace sc {

// Loader API (exported for the harness).
SC_API bool ScLoadXboxPe(const wchar_t* path, LoadedPe* out, char* err,
                         size_t errSize) {
    return LoadXboxPe(path, out, err, errSize);
}

SC_API bool ScRunXboxPe(LoadedPe* pe, DWORD* exitCode) {
    return RunXboxPe(pe, exitCode);
}

SC_API void ScFreeXboxPe(LoadedPe* pe) { FreeXboxPe(pe); }

// HLE shim functions, exported under their real Xbox names so the shim
// import libs (defs/*.def) can link guest titles against them.
SC_API HRESULT XGameRuntimeInitialize() { return HleXGameRuntimeInitialize(); }
SC_API void* XMemAlloc(SIZE_T size, DWORD attrs) {
    return HleXMemAlloc(size, attrs);
}
SC_API void XMemFree(void* p) { HleXMemFree(p); }
SC_API void* XMemAllocDefault(SIZE_T size) {
    return HleXMemAllocDefault(size);
}
SC_API void XMemFreeDefault(void* p) { HleXMemFreeDefault(p); }
SC_API void* XMemVirtualAlloc(void* lpAddress, SIZE_T dwSize,
                              DWORD flAllocationType, DWORD flProtect,
                              DWORD dwMemoryAttributes) {
    return HleXMemVirtualAlloc(lpAddress, dwSize, flAllocationType,
                               flProtect, dwMemoryAttributes);
}
SC_API HRESULT XMemGetWorkingSetStatistics(void* pStats) {
    return HleXMemGetWorkingSetStatistics(pStats);
}
SC_API BOOL XMemIsToolsMemAvailable(void) {
    return HleXMemIsToolsMemAvailable();
}
SC_API HRESULT XMemAllocatePhysicalPages(void) {
    return HleXMemAllocatePhysicalPages();
}
SC_API HRESULT XMemFreePhysicalPages(void) {
    return HleXMemFreePhysicalPages();
}
SC_API void* XMemMapPhysicalPages(const uint64_t* pPhysicalPages,
                                  uint32_t dwPageCount,
                                  DWORD dwMemoryAttributes, DWORD flProtect) {
    return HleXMemMapPhysicalPages(pPhysicalPages, dwPageCount,
                                   dwMemoryAttributes, flProtect);
}
SC_API void* XMemMapPhysicalPagesScatter(const uint64_t* pPhysicalPages,
                                         uint32_t dwPageCount,
                                         DWORD dwMemoryAttributes,
                                         DWORD flProtect) {
    return HleXMemMapPhysicalPagesScatter(pPhysicalPages, dwPageCount,
                                          dwMemoryAttributes, flProtect);
}
SC_API void XMemTransferMemory(void* pDst, const void* pSrc, SIZE_T cbSize) {
    HleXMemTransferMemory(pDst, pSrc, cbSize);
}
SC_API HRESULT D3D12XboxCreateDevice(
    IUnknown* adapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid,
    void** ppDevice) {
    return HleD3D12XboxCreateDevice(adapter, params, riid, ppDevice);
}
SC_API HRESULT D3D12ConfigureVirtualMemoryX(const void* pConfig) {
    return HleD3D12ConfigureVirtualMemoryX(pConfig);
}
// NOTE: no scarlett.cpp wrapper for D3D12SerializeVersionedRootSignature —
// that name is already the desktop SDK function (d3d12.h), and defining
// it here would conflict (C2733). The registry maps the Xbox import to
// HleD3D12SerializeVersionedRootSignature, which calls the SDK function.
SC_API HRESULT XAudio2Create(void** ppXAudio2, UINT32 flags, DWORD proc) {
    return HleXAudio2Create(ppXAudio2, flags, proc);
}
SC_API HRESULT GameInputCreate(void** ppGameInput) {
    return HleGameInputCreate(ppGameInput);
}
SC_API HRESULT DStorageGetFactory(void** ppFactory) {
    return HleDStorageGetFactory(ppFactory);
}
SC_API HRESULT SetFrameIntervalX(UINT32 interval, UINT32 flags) {
    return HleSetFrameIntervalX(interval, flags);
}
SC_API HRESULT ScheduleFrameEventX(UINT64 frameId) {
    return HleScheduleFrameEventX(frameId);
}
SC_API HRESULT WaitFrameEventX(UINT64 frameId, DWORD timeoutMs) {
    return HleWaitFrameEventX(frameId, timeoutMs);
}

// NOTE: the software ray tracing entry points (ScSoftwareRT_*) are
// defined in src/software_rt.cpp and exported via defs/d3d12_xs.def —
// no wrappers here (a second definition would collide at link time).
// The HLE registry (hle_registry.cpp) calls them through the Hle
// wrappers.

}  // namespace sc

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        sc::Log("layer", "ScarlettCompat loaded (scaffolding build)");
    }
    return TRUE;
}
