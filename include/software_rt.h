// software_rt.h — software ray tracing backend for the ScarlettCompat layer.
// Replaces hardware DXR on GPUs with no ray tracing cores (RX 5700 XT /
// RDNA1: D3D12 RaytracingTier == NOT_SUPPORTED). Ray tracing is done in a
// compute shader, Lumen-style: ray generation + BVH walk + Moller-Trumbore
// intersection + closest-hit / miss shading, writing the caller's UAV.
// See docs/SOFTWARE-RT.md for the design, the mapping table, and what is
// NOT implemented (any-hit, procedural primitives, recursion, multi-BLAS
// TLAS, hardware DXR passthrough).
//
// These are ScarlettCompat-defined entry points, NOT Xbox APIs. A real
// title calls ID3D12Device5 / ID3D12GraphicsCommandList4 COM methods;
// routing those to this backend needs the COM wrapper documented in
// docs/SOFTWARE-RT.md ("The remaining integration"). The flat entry
// points below are the backend the wrapper will call into, and the
// surface the synthetic proof guest exercises through the loader.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which path the adapter takes. Software when the device reports no RT
// tier; hardware when it reports tier >= 1_0 (passthrough not yet
// implemented — DispatchRays refuses it with E_NOTIMPL; see the doc).
typedef enum ScRtPath {
    SC_RT_PATH_SOFTWARE = 0,
    SC_RT_PATH_HARDWARE = 1,
} ScRtPath;

// Query the path for a device. rtTier receives the raw
// D3D12_RAYTRACING_TIER value (0 == NOT_SUPPORTED).
HRESULT ScSoftwareRT_QueryPath(ID3D12Device* device, ScRtPath* path,
                               uint32_t* rtTier);

// Compile the compute ray-tracing PSO once and cache it (per process).
// Safe to call more than once; later calls are no-ops returning S_OK.
HRESULT ScSoftwareRT_Init(ID3D12Device* device);

// One triangle of scene geometry (BLAS input). Color is the flat
// closest-hit color for that triangle (linear RGBA, 0..1).
typedef struct ScRtTriangle {
    float v0[3];
    float v1[3];
    float v2[3];
    float color[4];
} ScRtTriangle;

typedef void* ScSoftwareRTSceneHandle;

// Build a scene: CPU BVH build (median-split over centroids, one
// triangle per leaf) + upload to GPU buffers. The handle owns the GPU
// resources; free it with ScSoftwareRT_FreeScene.
HRESULT ScSoftwareRT_BuildScene(ID3D12Device* device,
                                const ScRtTriangle* tris, uint32_t triCount,
                                ScSoftwareRTSceneHandle* scene);
void ScSoftwareRT_FreeScene(ScSoftwareRTSceneHandle scene);

// Pinhole camera (raygen record). Rays are origin + t * dir where
// dir = normalize(lowerLeft + u*horizontal + v*vertical - origin),
// u = x/width, v = y/height.
typedef struct ScRtCamera {
    float origin[3];
    float lowerLeft[3];
    float horizontal[3];
    float vertical[3];
} ScRtCamera;

// Dispatch the ray tracer: one ray per output pixel, closest-hit / miss
// shading, into the caller's UAV texture (R8G8B8A8_UNORM, UAV-capable).
// The caller owns the output resource, transitions it to
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS before calling, and does its own
// readback after the fence. The command list must be open (recording);
// this function does NOT close it, execute it, or wait on a fence.
// Returns E_NOTIMPL when the adapter takes the hardware path (see above).
HRESULT ScSoftwareRT_DispatchRays(ID3D12Device* device,
                                  ID3D12GraphicsCommandList* cmdList,
                                  ScSoftwareRTSceneHandle scene,
                                  const ScRtCamera* camera,
                                  ID3D12Resource* outputUav, uint32_t width,
                                  uint32_t height);

#ifdef __cplusplus
}
#endif
