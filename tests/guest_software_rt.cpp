// guest_software_rt.cpp — software ray tracing proof guest.
// A synthetic Game Core title that exercises the layer's software-RT
// entry points (docs/SOFTWARE-RT.md) through the normal loader path:
//   XGameRuntimeInitialize -> D3D12XboxCreateDevice (real desktop
//   device) -> ScSoftwareRT_QueryPath (expects software / tier 0 on the
//   RX 5700 XT, which has no RT cores) -> ScSoftwareRT_Init ->
//   ScSoftwareRT_BuildScene (one red triangle) ->
//   ScSoftwareRT_DispatchRays into a 256x256 UAV -> readback, pixel
//   check: center must be the triangle's red (255,0,0,255), a corner
//   must be the miss clear color (51,102,204,255).
// Every stage reports as srt_<stage> in guest_report.txt; the harness
// prints a dedicated software-rt report. This file contains NO game
// code — it is a test program written for the layer, and it is never
// run through the Windows loader directly (its Xbox imports only
// resolve inside the layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <d3d12.h>
#include "xbox_shims.h"
#include "software_rt.h"

// Xbox build-info section, like the shipped DLLs carry.
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

static FILE* g_report = nullptr;
static int g_stagesTotal = 0;
static int g_stagesPassed = 0;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-16s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
    if (strncmp(name, "srt", 3) == 0) {
        g_stagesTotal++;
        if (pass) g_stagesPassed++;
    }
}

static const uint32_t kW = 256, kH = 256;

// One red triangle, same geometry as the proof guest's triangle.
static const ScRtTriangle kTri[1] = {{
    {-0.8f, -0.8f, 0.0f},
    {0.8f, -0.8f, 0.0f},
    {0.0f, 0.8f, 0.0f},
    {1.0f, 0.0f, 0.0f, 1.0f},
}};

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    bool allOk = true;
    char detail[512] = {};
    HRESULT hr = S_OK;

    // 1. Runtime init.
    hr = XGameRuntimeInitialize();
    Report("srt_runtime", SUCCEEDED(hr), "XGameRuntimeInitialize");
    allOk &= SUCCEEDED(hr);

    // 2. Real device via the shim.
    void* dev = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                               __uuidof(ID3D12Device), &dev);
    bool devOk = SUCCEEDED(hr) && dev != nullptr;
    snprintf(detail, sizeof(detail), "hr=0x%08x device=%p (REAL)",
             (unsigned)hr, dev);
    Report("srt_device", devOk, detail);
    allOk &= devOk;
    ID3D12Device* device = (ID3D12Device*)dev;

    // 3. Path query: this card has no RT cores, so the layer must pick
    // the software path (tier 0).
    ScRtPath path = SC_RT_PATH_HARDWARE;
    uint32_t tier = 0xFFFFFFFF;
    ScSoftwareRTSceneHandle scene = nullptr;
    bool tierOk = false;
    if (device) {
        hr = ScSoftwareRT_QueryPath(device, &path, &tier);
        tierOk = SUCCEEDED(hr) && path == SC_RT_PATH_SOFTWARE && tier == 0;
        snprintf(detail, sizeof(detail),
                 "hr=0x%08x rtTier=%u path=%s (want software / tier 0)",
                 (unsigned)hr, tier,
                 path == SC_RT_PATH_SOFTWARE ? "software" : "hardware");
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("srt_tier", tierOk, detail);
    allOk &= tierOk;

    // 4. Init the software backend (compiles the compute PSO).
    bool initOk = false;
    if (device) {
        hr = ScSoftwareRT_Init(device);
        initOk = SUCCEEDED(hr);
        snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("srt_init", initOk, detail);
    allOk &= initOk;

    // 5. Build the one-triangle scene (CPU BVH + GPU upload).
    bool sceneOk = false;
    if (device && initOk) {
        hr = ScSoftwareRT_BuildScene(device, kTri, 1, &scene);
        sceneOk = SUCCEEDED(hr) && scene != nullptr;
        snprintf(detail, sizeof(detail), "hr=0x%08x scene=%p",
                 (unsigned)hr, scene);
    } else {
        snprintf(detail, sizeof(detail), "no device/init");
    }
    Report("srt_scene", sceneOk, detail);
    allOk &= sceneOk;

    // 6-7. Dispatch rays into a UAV, read the pixels back, check them.
    bool dispatchOk = false;
    detail[0] = 0;
    if (device && sceneOk) {
        ID3D12Resource* outTex = nullptr;
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceVal = 0;
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = kW;
            rd.Height = kH;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&outTex));
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                           alloc, nullptr, IID_PPV_ARGS(&list));
        // A fresh command list starts open; close it once so the
        // allocator can be Reset below (phase 4 gotcha).
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
        if (FAILED(hr) || !fenceEv) {
            snprintf(detail, sizeof(detail), "setup hr=0x%08x", (unsigned)hr);
            dispatchOk = false;
        } else {
            // Readback buffer for the output texture.
            D3D12_RESOURCE_DESC texDesc = outTex->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            UINT numRows = 0;
            UINT64 rowSize = 0, totalBytes = 0;
            device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint,
                                          &numRows, &rowSize, &totalBytes);
            ID3D12Resource* readback = nullptr;
            D3D12_HEAP_PROPERTIES hpRb = {};
            hpRb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rbDesc = {};
            rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbDesc.Width = totalBytes;
            rbDesc.Height = 1;
            rbDesc.DepthOrArraySize = 1;
            rbDesc.MipLevels = 1;
            rbDesc.SampleDesc.Count = 1;
            rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device->CreateCommittedResource(
                &hpRb, D3D12_HEAP_FLAG_NONE, &rbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
            if (SUCCEEDED(hr)) hr = alloc->Reset();
            if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
            ScRtCamera cam = {};
            cam.origin[0] = 0.0f; cam.origin[1] = 0.0f; cam.origin[2] = 2.0f;
            cam.lowerLeft[0] = -1.0f; cam.lowerLeft[1] = 1.0f; cam.lowerLeft[2] = 0.0f;
            cam.horizontal[0] = 2.0f; cam.horizontal[1] = 0.0f; cam.horizontal[2] = 0.0f;
            cam.vertical[0] = 0.0f; cam.vertical[1] = -2.0f; cam.vertical[2] = 0.0f;
            if (SUCCEEDED(hr)) {
                hr = ScSoftwareRT_DispatchRays(device, list, scene, &cam,
                                               outTex, kW, kH);
            }
            if (SUCCEEDED(hr)) {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = outTex;
                b.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = outTex;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                hr = list->Close();
            }
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {list};
                q->ExecuteCommandLists(1, lists);
                q->Signal(fence, ++fenceVal);
                fence->SetEventOnCompletion(fenceVal, fenceEv);
                if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
                    hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                void* mapped = nullptr;
                D3D12_RANGE range = {0, totalBytes};
                hr = readback->Map(0, &range, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    const uint8_t* base = (const uint8_t*)mapped;
                    auto pxAt = [&](int x, int y) -> const uint8_t* {
                        return base + footprint.Offset +
                               (uint64_t)y * footprint.Footprint.RowPitch +
                               x * 4;
                    };
                    const uint8_t* c = pxAt(128, 128);
                    const uint8_t* k = pxAt(10, 10);
                    bool centerRed = (abs((int)c[0] - 255) <= 3) &&
                                     (abs((int)c[1] - 0) <= 3) &&
                                     (abs((int)c[2] - 0) <= 3) &&
                                     (abs((int)c[3] - 255) <= 3);
                    bool cornerClear = (abs((int)k[0] - 51) <= 3) &&
                                       (abs((int)k[1] - 102) <= 3) &&
                                       (abs((int)k[2] - 204) <= 3) &&
                                       (abs((int)k[3] - 255) <= 3);
                    dispatchOk = centerRed && cornerClear;
                    snprintf(detail, sizeof(detail),
                             "center (%d,%d,%d,%d) want (255,0,0,255); "
                             "corner (%d,%d,%d,%d) want (51,102,204,255)",
                             c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3]);
                    D3D12_RANGE empty = {0, 0};
                    readback->Unmap(0, &empty);
                } else {
                    snprintf(detail, sizeof(detail), "Map hr=0x%08x",
                             (unsigned)hr);
                }
            } else {
                snprintf(detail, sizeof(detail), "dispatch/copy hr=0x%08x",
                         (unsigned)hr);
            }
            if (readback) readback->Release();
        }
        if (scene) ScSoftwareRT_FreeScene(scene);
        if (outTex) outTex->Release();
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
        if (fenceEv) CloseHandle(fenceEv);
    } else {
        snprintf(detail, sizeof(detail), "no device/scene");
    }
    Report("srt_dispatch", dispatchOk, detail);
    allOk &= dispatchOk;

    if (device) device->Release();
    snprintf(detail, sizeof(detail),
             "%d/%d software-RT stages pass, ray-traced triangle "
             "pixel-verified via the software path",
             g_stagesPassed, g_stagesTotal);
    Report("srt", allOk, detail);
    printf("[guest] software-rt %s\n", allOk ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allOk ? 0 : 1;
}
