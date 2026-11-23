// guest_minimal.cpp — synthetic guest title for the ScarlettCompat harness.
// A minimal Xbox-style PE: image base 0x180000000, a .xbld section with
// GXDK build strings, and imports from the Xbox DLLs (xgameruntime,
// xmem, d3d12_xs, XAudio2_9, dstorage_xs, gameinput) that the layer shims.
// It exercises the boot sequence from the research brief §1:
//   XGameRuntimeInitialize -> XMemAlloc -> D3D12XboxCreateDevice ->
//   frame-pacing calls -> XAudio2Create -> GameInputCreate ->
//   DStorageGetFactory -> real desktop D3D12 device + clear + fence.
// The desktop D3D12 check (gfx_real) proves the translation target works;
// the d3d12_xs call (gfx_xs) proves the boot path reaches the shim.
// Results go to guest_report.txt (read by scarlett_verify) and stdout.
// This file contains NO game code — it is a test program written for the
// layer, and it is never run through the Windows loader directly (its
// Xbox imports only resolve inside the layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d12.h>
#include <comdef.h>
#include "xbox_shims.h"

// Xbox build-info section, like the shipped DLLs carry.
// (Matches the October 2026 GXDK stamp on amd_fidelityfx_upscaler_xbox.dll.)
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

static FILE* g_report = nullptr;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-12s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
}

// Real desktop D3D12 device + clear + fence. This is the translation
// target for the d3d12_xs shim: if the desktop device path works, the shim
// has something real to translate to. Guarded so a failure here does not
// mask the boot-path result.
static bool TestRealD3D12(char* detail, size_t n) {
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        snprintf(detail, n, "D3D12CreateDevice hr=0x%08x", (unsigned)hr);
        return false;
    }
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommandQueue hr=0x%08x", (unsigned)hr);
        dev->Release();
        return false;
    }
    ID3D12CommandAllocator* alloc = nullptr;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    nullptr, IID_PPV_ARGS(&list));
    ID3D12Resource* rt = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    if (SUCCEEDED(hr))
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 64;
        rd.Height = 64;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE h =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt, nullptr, h);
        FLOAT color[4] = {0.1f, 0.2f, 0.4f, 1.0f};
        list->ClearRenderTargetView(h, color, 0, nullptr);
        hr = list->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* lists[] = {list};
            q->ExecuteCommandLists(1, lists);
            ID3D12Fence* fence = nullptr;
            hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&fence));
            if (SUCCEEDED(hr)) {
                HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                q->Signal(fence, 1);
                fence->SetEventOnCompletion(1, ev);
                DWORD w = WaitForSingleObject(ev, 10000);
                ok = (w == WAIT_OBJECT_0);
                if (!ok)
                    snprintf(detail, n, "fence wait timed out");
                else
                    snprintf(detail, n,
                             "device+queue+clear+fence ok (FL 11_0)");
                CloseHandle(ev);
                fence->Release();
            } else {
                snprintf(detail, n, "CreateFence hr=0x%08x", (unsigned)hr);
            }
        } else {
            snprintf(detail, n, "Close hr=0x%08x", (unsigned)hr);
        }
    } else {
        snprintf(detail, n, "setup hr=0x%08x", (unsigned)hr);
    }
    if (rt) rt->Release();
    if (rtvHeap) rtvHeap->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (q) q->Release();
    dev->Release();
    return ok;
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    printf("[guest] synthetic Xbox guest starting\n");
    bool allBoot = true;
    char detail[256];

    HRESULT hr = XGameRuntimeInitialize();
    bool ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    Report("runtime", ok, detail);
    allBoot &= ok;

    void* mem = XMemAlloc(1024, 0);
    ok = mem != nullptr;
    Report("memory", ok, ok ? "1024 bytes allocated+freed" : "alloc NULL");
    if (mem) XMemFree(mem);
    allBoot &= ok;

    void* dev = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                               __uuidof(ID3D12Device), &dev);
    ok = SUCCEEDED(hr) && dev != nullptr;
    snprintf(detail, sizeof(detail), "hr=0x%08x device=%p", (unsigned)hr,
             dev);
    Report("gfx_xs", ok, detail);
    allBoot &= ok;

    hr = SetFrameIntervalX(1, 0);
    ok = SUCCEEDED(hr);
    Report("present", ok, ok ? "frame interval set" : "SetFrameIntervalX failed");
    allBoot &= ok;
    hr = ScheduleFrameEventX(1);
    ok = SUCCEEDED(hr);
    Report("frame_evt", ok, nullptr);
    allBoot &= ok;
    hr = WaitFrameEventX(1, 1000);
    ok = SUCCEEDED(hr);
    Report("frame_wait", ok, nullptr);
    allBoot &= ok;

    void* xa = nullptr;
    hr = XAudio2Create((IXAudio2**)&xa, 0, 0);
    ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    Report("audio", ok, detail);
    allBoot &= ok;

    void* gi = nullptr;
    hr = GameInputCreate((IGameInput**)&gi);
    ok = SUCCEEDED(hr);
    Report("input", ok, nullptr);
    allBoot &= ok;

    void* ds = nullptr;
    hr = DStorageGetFactory(&ds);
    ok = SUCCEEDED(hr);
    Report("storage", ok, nullptr);
    allBoot &= ok;

    // Translation-target proof (separate from the boot path).
    detail[0] = 0;
    ok = TestRealD3D12(detail, sizeof(detail));
    Report("gfx_real", ok, detail);

    printf("[guest] boot path %s\n", allBoot ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allBoot ? 0 : 1;
}
