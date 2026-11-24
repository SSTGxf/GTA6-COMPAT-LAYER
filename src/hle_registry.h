// hle_registry.h — the HLE export registry (Xenia-style).
// Central table mapping (Xbox DLL, function name) -> host implementation +
// status tag. The loader resolves guest imports through this table.
#pragma once
#include "scarlett.h"
#include "xbox_shims.h"  // XAsyncBlock / XUser / XGameSave / XPackage types
#include "software_rt.h"  // ScRtPath / ScRtTriangle / ScRtCamera types
#include "fixed_timestep.h"  // ScPhysicsStepFn / ScFixedTimestepStats types
#include "opt_pso_cache.h"  // PSO + root-signature cache types
#include "opt_detile.h"     // de-tiled texture cache types
#include "opt_mesh_fallback.h"  // mesh-shader fallback types
#include "opt_async_compute.h"  // async compute types
#include "opt_desc_heap.h"  // descriptor heap types
#include "opt_dstorage.h"   // DirectStorage + BCPack types
#include "opt_shader_cache.h"  // shader precompilation cache types
#include "opt_texcache.h"  // GPU de-tiled texture cache (VRAM budget + LRU)
#include "opt_batching.h"  // frame batching types (submit / draw / flight)
#include <d3d12.h>  // IUnknown + D3D_FEATURE_LEVEL for the device-creation shim

namespace sc {

// Set by the loader (LoadXboxPe) when the loaded image is a real Xbox
// title (.xbld present AND a large image — the synthetic guests are
// also .xbld-built, so size is the discriminator). The Xbox COM wrapper
// only applies to real Xbox titles — synthetic guests use the desktop
// D3D12 vtable layout and must get the raw desktop device, not the Xbox
// wrapper.
extern bool g_scGuestIsRealXboxTitle;

// Host implementations of the shimmed Xbox functions.
extern "C" HRESULT HleXGameRuntimeInitialize();
extern "C" void* HleXMemAlloc(SIZE_T size, DWORD attrs);
extern "C" void HleXMemFree(void* p);
// kernelx!VirtualAlloc flag translation (VirtualAlloc
// flag-translation fix, 2026-11-23): strips the Xbox-specific allocation-type bits the desktop rejects
// (see HleVirtualAlloc in hle_registry.cpp) and retries with a
// system-chosen address when the requested address fails.
extern "C" LPVOID HleVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize,
                                  DWORD flAllocationType, DWORD flProtect);
extern "C" HRESULT HleD3D12XboxCreateDevice(
    IUnknown* adapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid,
    void** ppDevice);
// ERA d3d12_x!D3D12XCreateDevice — D3D12CreateDevice-shaped (adapter,
// minFL, riid, ppDevice). SEPARATE from the GDK form above (see
// hle_registry.cpp — sharing one implementation crashed era_gfx12).
extern "C" HRESULT HleEraD3D12XCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice);
extern "C" HRESULT HleD3D12ConfigureVirtualMemoryX(const void* pConfig);
extern "C" HRESULT HleD3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);
// Real-exe xmem additions (grounded 2026-11-23 from the exe's import
// table; XMemVirtualAlloc's 5-arg form from its call site at
// 0x14193e220).
extern "C" void* HleXMemAllocDefault(SIZE_T size);
extern "C" void HleXMemFreeDefault(void* p);
extern "C" void* HleXMemVirtualAlloc(void* lpAddress, SIZE_T dwSize,
                                     DWORD flAllocationType, DWORD flProtect,
                                     DWORD dwMemoryAttributes);
extern "C" HRESULT HleXMemGetWorkingSetStatistics(void* pStats);
extern "C" BOOL HleXMemIsToolsMemAvailable(void);
extern "C" HRESULT HleXMemAllocatePhysicalPages(void);
extern "C" HRESULT HleXMemFreePhysicalPages(void);
extern "C" void* HleXMemMapPhysicalPages(const uint64_t* pPhysicalPages,
                                         uint32_t dwPageCount,
                                         DWORD dwMemoryAttributes,
                                         DWORD flProtect);
extern "C" void* HleXMemMapPhysicalPagesScatter(const uint64_t* pPhysicalPages,
                                                uint32_t dwPageCount,
                                                DWORD dwMemoryAttributes,
                                                DWORD flProtect);
extern "C" void HleXMemTransferMemory(void* pDst, const void* pSrc,
                                      SIZE_T cbSize);
extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags, DWORD proc);
extern "C" HRESULT HleGameInputCreate(void** ppGameInput);
extern "C" HRESULT HleDStorageGetFactory(const void* riid, void** ppFactory);
extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags);
extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId);
extern "C" HRESULT HleWaitFrameEventX(UINT64 frameId, DWORD timeoutMs);

// Software ray tracing (software RT backend, see docs/SOFTWARE-RT.md).
// ScarlettCompat-defined entry points in d3d12_xs.dll (not Xbox APIs).
extern "C" HRESULT HleScSoftwareRTQueryPath(ID3D12Device* device,
                                            ScRtPath* path, uint32_t* rtTier);
extern "C" ScRtMode HleScSoftwareRTGetMode();
extern "C" HRESULT HleScSoftwareRTInit(ID3D12Device* device);
extern "C" HRESULT HleScSoftwareRTBuildScene(ID3D12Device* device,
                                             const ScRtTriangle* tris,
                                             uint32_t triCount,
                                             ScSoftwareRTSceneHandle* scene);
extern "C" void HleScSoftwareRTFreeScene(ScSoftwareRTSceneHandle scene);
extern "C" HRESULT HleScSoftwareRTDispatchRays(ID3D12Device* device,
                                               ID3D12GraphicsCommandList* cmdList,
                                               ScSoftwareRTSceneHandle scene,
                                               const ScRtCamera* camera,
                                               ID3D12Resource* outputUav,
                                               uint32_t width, uint32_t height);

// Fixed timestep (fixed-timestep backend, see docs/FIXED-TIMESTEP.md).
// ScarlettCompat-defined entry points in d3d12_xs.dll (not Xbox APIs).
extern "C" HRESULT HleScFixedTimestepInit(double rateHz,
                                          uint32_t maxStepsPerFrame);
extern "C" HRESULT HleScFixedTimestepSetStepCallback(
    ScPhysicsStepFn fn, void* ctx);
extern "C" HRESULT HleScFixedTimestepPump(uint32_t* stepsTaken);
extern "C" double HleScFixedTimestepAlpha(void);
extern "C" HRESULT HleScFixedTimestepGetStats(
    ScFixedTimestepStats* out);
extern "C" void HleScFixedTimestepShutdown(void);

// Optimizations (see docs/OPTIMIZATION.md). ScarlettCompat-defined
// entry points in d3d12_xs.dll (not Xbox APIs).
extern "C" HRESULT HleScOptPsoCacheInit(ID3D12Device* device);
extern "C" HRESULT HleScOptPsoCacheGetGraphics(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit);
extern "C" HRESULT HleScOptPsoCacheGetCompute(
    ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit);
extern "C" HRESULT HleScOptPsoCacheGetRootSignature(ID3D12Device* device,
                                                    const void* blob,
                                                    SIZE_T blobLen,
                                                    ID3D12RootSignature** rs,
                                                    BOOL* hit);
extern "C" HRESULT HleScOptPsoCacheStats(uint32_t* created, uint32_t* hits,
                                         uint32_t* misses);
extern "C" void HleScOptPsoCacheDestroy();
extern "C" HRESULT HleScOptTile2D(const uint8_t* linear, uint32_t w,
                                  uint32_t h, uint32_t bpp, uint32_t tileW,
                                  uint32_t tileH, uint8_t* tiled);
extern "C" HRESULT HleScOptDetile2D(const uint8_t* tiled, uint32_t w,
                                    uint32_t h, uint32_t bpp, uint32_t tileW,
                                    uint32_t tileH, uint8_t* linear);
extern "C" HRESULT HleScOptDetileCached(const uint8_t* tiled, uint32_t w,
                                        uint32_t h, uint32_t bpp,
                                        uint32_t tileW, uint32_t tileH,
                                        uint8_t* linearOut, BOOL* hit);
extern "C" HRESULT HleScOptDetileStats(uint32_t* detiles, uint32_t* hits,
                                       uint32_t* evictions);
extern "C" void HleScOptDetileDestroy();
extern "C" HRESULT HleScOptMeshFallbackInit(ID3D12Device* device);
extern "C" HRESULT HleScOptMeshFallbackGetShaders(const void** vsBlob,
                                                  SIZE_T* vsLen,
                                                  const void** psBlob,
                                                  SIZE_T* psLen);
extern "C" HRESULT HleScOptMeshFallbackExpand(ID3D12Device* device,
                                              const ScOptMeshlet* meshlet,
                                              ScOptExpandedMesh* expanded);
extern "C" void HleScOptMeshFallbackFreeExpanded(ScOptExpandedMesh* expanded);
extern "C" void HleScOptMeshFallbackDestroy();
extern "C" HRESULT HleScOptAsyncInit(ID3D12Device* device);
extern "C" HRESULT HleScOptAsyncRunTimed(ID3D12Device* device,
                                         ScOptAsyncMode mode, double* avgMs);
extern "C" HRESULT HleScOptAsyncChecksum(ScOptAsyncMode mode,
                                         uint64_t* checksum);
extern "C" void HleScOptAsyncDestroy();
extern "C" HRESULT HleScOptDescHeapInit(ID3D12Device* device,
                                        D3D12_DESCRIPTOR_HEAP_TYPE heapType,
                                        uint32_t capacity,
                                        ScOptDescHeapHandle* heap);
extern "C" HRESULT HleScOptDescAlloc(ScOptDescHeapHandle heap,
                                     D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                                     D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle,
                                     uint32_t* index);
extern "C" HRESULT HleScOptDescFree(ScOptDescHeapHandle heap, uint32_t index);
extern "C" HRESULT HleScOptDescHeapStats(ScOptDescHeapHandle heap,
                                         uint32_t* capacity, uint32_t* live,
                                         uint32_t* heaps);
extern "C" void HleScOptDescHeapDestroy(ScOptDescHeapHandle heap);
extern "C" HRESULT HleScOptDStorageInit(ScOptDsPath* path);
extern "C" HRESULT HleScOptDStorageReadFile(const char* path, uint64_t offset,
                                            uint32_t size, uint8_t** data,
                                            uint32_t* bytesRead);
extern "C" HRESULT HleScOptDStorageReadFileBatch(const char* path,
                                                 const uint64_t* offsets,
                                                 const uint32_t* sizes,
                                                 uint32_t count,
                                                 uint8_t** datas,
                                                 uint32_t* bytesRead);
extern "C" HRESULT HleScOptDStorageDecompress(ScOptDsDecompress mode,
                                              const uint8_t* src, size_t srcLen,
                                              uint8_t* dst, size_t dstLen,
                                              size_t* outLen);
extern "C" HRESULT HleScOptShaderCacheInit(const char* dir);
extern "C" HRESULT HleScOptCompileCached(const char* source, size_t sourceLen,
                                         const char* profile,
                                         const char* entryPoint,
                                         uint8_t** blob, size_t* blobSize);
extern "C" void HleScOptShaderCacheStats(uint64_t* hits, uint64_t* misses);

// GPU de-tiled texture cache (opt 2, memory management).
extern "C" HRESULT HleScOptTexCacheInit(ID3D12Device* device,
                                        uint64_t budgetBytes,
                                        ScOptTexCacheHandle* cache);
extern "C" HRESULT HleScOptTexCacheLookup(ScOptTexCacheHandle cache,
                                          uint64_t key,
                                          ID3D12Resource** resource,
                                          BOOL* hit);
extern "C" HRESULT HleScOptTexCacheStore(ScOptTexCacheHandle cache,
                                         uint64_t key, const uint8_t* tiled,
                                         uint32_t w, uint32_t h, uint32_t bpp,
                                         uint32_t tileW, uint32_t tileH,
                                         ID3D12CommandQueue* queue,
                                         ID3D12Resource** resource);
extern "C" HRESULT HleScOptTexCacheEvict(ScOptTexCacheHandle cache,
                                         uint64_t key);
extern "C" HRESULT HleScOptTexCacheStats(ScOptTexCacheHandle cache,
                                         uint32_t* entries,
                                         uint64_t* bytesUsed,
                                         uint64_t* bytesBudget,
                                         uint32_t* hits, uint32_t* misses,
                                         uint32_t* evictions);
extern "C" void HleScOptTexCacheDestroy(ScOptTexCacheHandle cache);

// Frame batching (opt 9): submission batching, draw/state batching,
// frames in flight.
extern "C" HRESULT HleScOptBatchInit(ID3D12CommandQueue* queue,
                                     uint32_t maxLists,
                                     ScOptBatchHandle* batch);
extern "C" HRESULT HleScOptBatchAdd(ScOptBatchHandle batch,
                                    ID3D12CommandList* list);
extern "C" HRESULT HleScOptBatchFlush(ScOptBatchHandle batch);
extern "C" HRESULT HleScOptBatchStats(ScOptBatchHandle batch,
                                      uint32_t* listsSubmitted,
                                      uint32_t* submitCalls);
extern "C" void HleScOptBatchDestroy(ScOptBatchHandle batch);
extern "C" HRESULT HleScOptDrawBatchBegin(ID3D12GraphicsCommandList* list,
                                          ScOptDrawBatchHandle* batch);
extern "C" HRESULT HleScOptDrawBatchSetPSO(ScOptDrawBatchHandle batch,
                                           ID3D12PipelineState* pso);
extern "C" HRESULT HleScOptDrawBatchSetRootSignature(
    ScOptDrawBatchHandle batch, ID3D12RootSignature* rs);
extern "C" HRESULT HleScOptDrawBatchSetRenderTargets(
    ScOptDrawBatchHandle batch, UINT numRTs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleHandle,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
extern "C" HRESULT HleScOptDrawBatchSetVertexBuffer(
    ScOptDrawBatchHandle batch, UINT slot,
    const D3D12_VERTEX_BUFFER_VIEW* view);
extern "C" HRESULT HleScOptDrawBatchDrawInstanced(ScOptDrawBatchHandle batch,
                                                  UINT vertexCount,
                                                  UINT startVertex,
                                                  UINT instanceCount,
                                                  UINT startInstance);
extern "C" HRESULT HleScOptDrawBatchFlushDraw(ScOptDrawBatchHandle batch);
extern "C" HRESULT HleScOptDrawBatchEnd(ScOptDrawBatchHandle batch);
extern "C" HRESULT HleScOptDrawBatchStats(ScOptDrawBatchHandle batch,
                                          uint32_t* drawsRequested,
                                          uint32_t* drawsRecorded,
                                          uint32_t* setsRequested,
                                          uint32_t* setsIssued);
extern "C" void HleScOptDrawBatchDestroy(ScOptDrawBatchHandle batch);
extern "C" HRESULT HleScOptFlightInit(uint32_t depth,
                                      ScOptFlightHandle* flight);
extern "C" HRESULT HleScOptFlightBeginFrame(ScOptFlightHandle flight,
                                            ID3D12CommandQueue* queue,
                                            ID3D12Fence* fence,
                                            double* waitMs);
extern "C" HRESULT HleScOptFlightEndFrame(ScOptFlightHandle flight,
                                          ID3D12CommandQueue* queue,
                                          ID3D12Fence* fence);
extern "C" HRESULT HleScOptFlightStats(ScOptFlightHandle flight,
                                       uint32_t* frames,
                                       double* totalWaitMs,
                                       double* avgWaitMs);
extern "C" void HleScOptFlightDestroy(ScOptFlightHandle flight);

// XAsync / XUser / XGameSave / XPackage (phase 2, milestone E).
// Types come from xbox_shims.h (shared with the guest).
extern "C" HRESULT HleXAsyncGetStatus(struct XAsyncBlock* async, BOOL wait);
extern "C" HRESULT HleXAsyncGetResultSize(struct XAsyncBlock* async,
                                           size_t* size);
extern "C" HRESULT HleXAsyncCancel(struct XAsyncBlock* async);
extern "C" HRESULT HleXAsyncRun(struct XAsyncBlock* async, XAsyncWork work);
extern "C" HRESULT HleXUserAddAsync(XUserAddOptions options,
                                     struct XAsyncBlock* async);
extern "C" HRESULT HleXUserAddResult(struct XAsyncBlock* async,
                                     XUserHandle* user);
extern "C" HRESULT HleXUserGetId(XUserHandle user, uint64_t* xuid);
extern "C" HRESULT HleXUserGetGamertag(XUserHandle user,
                                        XUserGamertagOptions options,
                                        char* gamertag, size_t* gamertagSize);
extern "C" HRESULT HleXUserGetLocalId(XUserHandle user, XUserLocalId* localId);
extern "C" HRESULT HleXUserCloseHandle(XUserHandle user);
extern "C" HRESULT HleXGameSaveInitializeProvider(XUserHandle user,
                                                   const char* scid,
                                                   BOOL allowSynchronous,
                                                   XGameSaveProviderHandle* provider);
extern "C" HRESULT HleXGameSaveCloseProvider(XGameSaveProviderHandle provider);
extern "C" HRESULT HleXGameSaveCreateContainer(XGameSaveProviderHandle provider,
                                                const char* displayName,
                                                XGameSaveContainerHandle* container);
// XGameSave update flow + XGameSaveFiles (phase 3).
extern "C" HRESULT HleXGameSaveCreateUpdate(XGameSaveContainerHandle container,
                                            const char* containerDisplayName,
                                            XGameSaveUpdateHandle* update);
extern "C" HRESULT HleXGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update,
                                               const char* blobName,
                                               const uint8_t* data,
                                               size_t byteCount);
extern "C" HRESULT HleXGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update,
                                                const char* blobName);
extern "C" HRESULT HleXGameSaveSubmitUpdate(XGameSaveUpdateHandle update);
extern "C" HRESULT HleXGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update,
                                                 struct XAsyncBlock* async);
extern "C" HRESULT HleXGameSaveSubmitUpdateResult(struct XAsyncBlock* async);
extern "C" HRESULT HleXGameSaveCloseUpdate(XGameSaveUpdateHandle update);
extern "C" HRESULT HleXGameSaveCloseContainer(XGameSaveContainerHandle container);
extern "C" HRESULT HleXGameSaveReadBlobData(XGameSaveContainerHandle container,
                                            const char* blobName,
                                            size_t* bytesRead,
                                            size_t bufferSize,
                                            uint8_t* buffer);
extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser, const char* configurationId,
    struct XAsyncBlock* async);
extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiResult(
    struct XAsyncBlock* async, size_t bufferSize, char* folder);
extern "C" HRESULT HleXPackageGetMountPathSize(uint32_t* size);
extern "C" HRESULT HleXPackageGetMountPath(wchar_t* path, uint32_t* pathSize);
extern "C" BOOL HleXPackageIsPackagedProcess(void);
extern "C" HRESULT HleXPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize, char* buffer);
extern "C" HRESULT HleXPackageEnumeratePackages(
    XPackageKind kind, XPackageEnumerationScope scope, void* context,
    XPackageEnumerationCallback* callback);
extern "C" HRESULT HleXPackageMountWithUiAsync(const char* packageIdentifier,
                                               struct XAsyncBlock* async);
extern "C" HRESULT HleXPackageMountWithUiResult(struct XAsyncBlock* async,
                                                XPackageMountHandle* mount);
extern "C" HRESULT HleXPackageCloseMountHandle(XPackageMountHandle mount);

// Guest directory for XPackageGetMountPath (set by the loader).
void SetGuestDir(const wchar_t* dir);

}  // namespace sc
