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
#include <stdlib.h>
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_3.h>  // IDXGIFactory2 + CreateDXGIFactory2
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

// TLS test (phase 2, milestone D): a __declspec(thread) variable and a
// .CRT$XLB callback. The loader must allocate a TLS slot + block, copy
// the template (init 42), and run the callback before the entry point.
static __declspec(thread) int g_tlsVar = 42;
static bool g_tlsCallbackRan = false;
static void NTAPI TlsCallback(PVOID base, DWORD reason, PVOID reserved) {
    (void)base;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g_tlsCallbackRan = true;
}
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) static PIMAGE_TLS_CALLBACK g_tlsCb =
    TlsCallback;

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

// Services (phase 2, milestone E): XUser sign-in (silent default user),
// XAsync completion, XGameSave provider + container, XPackage mount path.
static bool g_userCallbackRan = false;
static void CALLBACK UserAddCallback(XAsyncBlock* async) {
    (void)async;
    g_userCallbackRan = true;
}

static void CALLBACK TestAsyncWork(XAsyncBlock* async) {
    async->internal[1] = (void*)42;
}

static bool TestServices(char* detail, size_t n) {
    // XUser: silent add -> status -> result -> id/gamertag/localid.
    XAsyncBlock async = {};
    async.callback = UserAddCallback;
    HRESULT hr = XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &async);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserAddAsync hr=0x%08x", (unsigned)hr);
        return false;
    }
    if (!g_userCallbackRan) {
        snprintf(detail, n, "XUserAddAsync callback did not run");
        return false;
    }
    hr = XAsyncGetStatus(&async, FALSE);
    if (FAILED(hr)) {
        snprintf(detail, n, "XAsyncGetStatus hr=0x%08x", (unsigned)hr);
        return false;
    }
    XUserHandle user = nullptr;
    hr = XUserAddResult(&async, &user);
    if (FAILED(hr) || !user) {
        snprintf(detail, n, "XUserAddResult hr=0x%08x", (unsigned)hr);
        return false;
    }
    uint64_t xuid = 0;
    hr = XUserGetId(user, &xuid);
    if (FAILED(hr) || xuid == 0) {
        snprintf(detail, n, "XUserGetId hr=0x%08x", (unsigned)hr);
        return false;
    }
    char tag[32] = {};
    size_t tagSize = sizeof(tag);
    hr = XUserGetGamertag(user, XUserGamertagOptions_UniqueModern, tag,
                          &tagSize);
    if (FAILED(hr) || strcmp(tag, "Player") != 0) {
        snprintf(detail, n, "XUserGetGamertag hr=0x%08x tag=%s",
                 (unsigned)hr, tag);
        return false;
    }
    XUserLocalId localId = {};
    hr = XUserGetLocalId(user, &localId);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserGetLocalId hr=0x%08x", (unsigned)hr);
        return false;
    }
    // XAsyncRun: work runs synchronously and completes the block.
    XAsyncBlock runBlk = {};
    hr = XAsyncRun(&runBlk, TestAsyncWork);
    size_t resultSize = 0;
    HRESULT hr2 = XAsyncGetResultSize(&runBlk, &resultSize);
    if (FAILED(hr) || FAILED(hr2) || resultSize != 42) {
        snprintf(detail, n, "XAsyncRun hr=0x%08x resultSize=%zu",
                 (unsigned)hr, resultSize);
        return false;
    }
    // XGameSave: initialize provider + create a container, then close.
    XGameSaveProviderHandle provider = nullptr;
    hr = XGameSaveInitializeProvider(
        user, "00000000-0000-0000-0000-000000000001", FALSE, &provider);
    if (FAILED(hr) || !provider) {
        snprintf(detail, n, "XGameSaveInitializeProvider hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    XGameSaveContainerHandle container = nullptr;
    hr = XGameSaveCreateContainer(provider, "test-container", &container);
    if (FAILED(hr) || !container) {
        snprintf(detail, n, "XGameSaveCreateContainer hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseProvider(provider);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseProvider hr=0x%08x", (unsigned)hr);
        return false;
    }
    // XPackage: mount path + packaged flag.
    uint32_t mountSize = 0;
    hr = XPackageGetMountPathSize(&mountSize);
    if (FAILED(hr) || mountSize == 0) {
        snprintf(detail, n, "XPackageGetMountPathSize hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    wchar_t mountPath[MAX_PATH] = {};
    uint32_t mountCap = MAX_PATH;
    hr = XPackageGetMountPath(mountPath, &mountCap);
    if (FAILED(hr) || mountCap == 0 || mountPath[0] == 0) {
        snprintf(detail, n, "XPackageGetMountPath hr=0x%08x", (unsigned)hr);
        return false;
    }
    BOOL packaged = XPackageIsPackagedProcess();
    if (!packaged) {
        snprintf(detail, n, "XPackageIsPackagedProcess returned FALSE");
        return false;
    }
    hr = XUserCloseHandle(user);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserCloseHandle hr=0x%08x", (unsigned)hr);
        return false;
    }
    snprintf(detail, n,
             "user xuid=0x%llx tag=%s local=%llu mount=%lu chars "
             "packaged=%d",
             (unsigned long long)xuid, tag,
             (unsigned long long)localId.value, mountCap, (int)packaged);
    return true;
}

// Per-section page protections (phase 2, milestone C): the loader must
// have applied each section's characteristics — .text executable+read,
// .rdata read-only, .data read/write. VirtualQuery on an address in each
// section tells us the page protection. (We do NOT attempt a write to
// .text — that would fault if protections are correct.)
static bool TestRealD3D12(char* detail, size_t n);
static int g_protProbe = 1234;  // nonzero so it lands in .data, not .bss
static bool TestProtections(char* detail, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    DWORD textProt = 0, rodataProt = 0, dataProt = 0;
    if (VirtualQuery((void*)&TestRealD3D12, &mbi, sizeof(mbi)) == 0) {
        snprintf(detail, n, "VirtualQuery(.text) failed (%lu)",
                 GetLastError());
        return false;
    }
    textProt = mbi.Protect;
    // String literal lives in .rdata.
    if (VirtualQuery("ScarlettCompat protection probe", &mbi, sizeof(mbi)) ==
        0) {
        snprintf(detail, n, "VirtualQuery(.rdata) failed (%lu)",
                 GetLastError());
        return false;
    }
    rodataProt = mbi.Protect;
    if (VirtualQuery(&g_protProbe, &mbi, sizeof(mbi)) == 0) {
        snprintf(detail, n, "VirtualQuery(.data) failed (%lu)",
                 GetLastError());
        return false;
    }
    dataProt = mbi.Protect;
    bool ok = (textProt == PAGE_EXECUTE_READ) &&
              (rodataProt == PAGE_READONLY) && (dataProt == PAGE_READWRITE);
    snprintf(detail, n,
             "text=0x%x (want 0x20 XR) rdata=0x%x (want 0x2 R) "
             "data=0x%x (want 0x4 RW)",
             textProt, rodataProt, dataProt);
    return ok;
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

// First real d3d12_xs frame: render a frame with the shim device and
// VERIFY the pixels on the CPU via a readback buffer — that is the proof
// the frame was actually rendered by the GPU through the shim device.
// The swapchain Present is then attempted as a best-effort second step:
// in this environment (SSH-launched harness, session without a compositor)
// DXGI presentation is unavailable — a standalone probe program fails
// identically (0x887a0022), so a present failure here is environmental,
// not a layer bug, and does not fail the subsystem. When the session is
// interactive, the present path is exercised and reported in the detail.
static bool TestXsFrame(ID3D12Device* dev, char* detail, size_t n) {
    // ---- core proof: offscreen render + readback pixel check ----
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommandQueue hr=0x%08x", (unsigned)hr);
        return false;
    }
    ID3D12Resource* rt = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 640;
        rd.Height = 360;
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
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommittedResource(rt) hr=0x%08x",
                 (unsigned)hr);
        q->Release();
        return false;
    }
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = nullptr;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    }
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEv) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        snprintf(detail, n, "frame setup hr=0x%08x", (unsigned)hr);
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (rtvHeap) rtvHeap->Release();
        rt->Release();
        q->Release();
        return false;
    }
    // Record + execute the clear.
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, h);
    FLOAT color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    list->ClearRenderTargetView(h, color, 0, nullptr);
    hr = list->Close();
    UINT64 fenceVal = 0;
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        if (WaitForSingleObject(fenceEv, 10000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    // Readback: copy the RT to a CPU-visible buffer and check the pixel.
    bool pixelOk = false;
    char pixelDetail[128] = {};
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC rtDesc = rt->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT numRows = 0;
        UINT64 rowSize = 0, totalBytes = 0;
        dev->GetCopyableFootprints(&rtDesc, 0, 1, 0, &footprint, &numRows,
                                   &rowSize, &totalBytes);
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
        hr = dev->CreateCommittedResource(
            &hpRb, D3D12_HEAP_FLAG_NONE, &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr)) {
            alloc->Reset();
            list->Reset(alloc, nullptr);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = rt;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = rt;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            hr = list->Close();
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {list};
                q->ExecuteCommandLists(1, lists);
                q->Signal(fence, ++fenceVal);
                fence->SetEventOnCompletion(fenceVal, fenceEv);
                if (WaitForSingleObject(fenceEv, 10000) != WAIT_OBJECT_0)
                    hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                void* mapped = nullptr;
                D3D12_RANGE range = {0, totalBytes};
                hr = readback->Map(0, &range, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    // Center pixel (320,180). Clear color (0.2,0.4,0.8,1)
                    // -> UNORM (51,102,204,255).
                    const uint8_t* px =
                        (const uint8_t*)mapped + footprint.Offset +
                        (UINT64)180 * footprint.Footprint.RowPitch + 320 * 4;
                    int r = px[0], g = px[1], b = px[2], a = px[3];
                    pixelOk = (abs(r - 51) <= 2) && (abs(g - 102) <= 2) &&
                              (abs(b - 204) <= 2) && (abs(a - 255) <= 2);
                    snprintf(pixelDetail, sizeof(pixelDetail),
                             "pixel (%d,%d,%d,%d) expect (51,102,204,255)",
                             r, g, b, a);
                    D3D12_RANGE empty = {0, 0};
                    readback->Unmap(0, &empty);
                } else {
                    snprintf(pixelDetail, sizeof(pixelDetail),
                             "Map hr=0x%08x", (unsigned)hr);
                }
            }
            readback->Release();
        } else {
            snprintf(pixelDetail, sizeof(pixelDetail),
                     "readback alloc hr=0x%08x", (unsigned)hr);
        }
    }
    if (!pixelOk) {
        snprintf(detail, n, "pixel check FAILED (%s)", pixelDetail);
        CloseHandle(fenceEv);
        fence->Release();
        list->Release();
        alloc->Release();
        rtvHeap->Release();
        rt->Release();
        q->Release();
        return false;
    }
    // ---- best-effort: swapchain present (environmental if unavailable) ----
    char presentDetail[160] = {};
    {
        const wchar_t* kClass = L"ScarlettGuestWnd";
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        if (!GetClassInfoW(hi, kClass, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = hi;
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
        }
        HWND hwnd = CreateWindowExW(0, kClass, L"ScarlettGuest",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                                    nullptr, nullptr, hi, nullptr);
        if (hwnd) {
            IDXGIFactory2* factory = nullptr;
            HRESULT hr2 = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
            IDXGISwapChain1* sc = nullptr;
            if (SUCCEEDED(hr2)) {
                DXGI_SWAP_CHAIN_DESC1 sd = {};
                sd.Width = 640;
                sd.Height = 360;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                hr2 = factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr,
                                                      nullptr, &sc);
            }
            if (SUCCEEDED(hr2)) {
                hr2 = sc->Present(1, 0);
                if (SUCCEEDED(hr2))
                    snprintf(presentDetail, sizeof(presentDetail),
                             "swapchain present ok");
                else
                    snprintf(presentDetail, sizeof(presentDetail),
                             "swapchain present hr=0x%08x", (unsigned)hr2);
                sc->Release();
            } else {
                snprintf(presentDetail, sizeof(presentDetail),
                         "swapchain unavailable hr=0x%08x (environmental; "
                         "standalone probe fails identically)",
                         (unsigned)hr2);
            }
            if (factory) factory->Release();
            DestroyWindow(hwnd);
        } else {
            snprintf(presentDetail, sizeof(presentDetail),
                     "no window for swapchain (%lu)", GetLastError());
        }
    }
    snprintf(detail, n,
             "640x360 frame rendered via shim device, %s; %s",
             pixelDetail, presentDetail);
    CloseHandle(fenceEv);
    fence->Release();
    list->Release();
    alloc->Release();
    rtvHeap->Release();
    rt->Release();
    q->Release();
    return true;
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    printf("[guest] synthetic Xbox guest starting\n");
    // Keep .xbld: a volatile read forces the compiler to emit the section
    // (an unreferenced static const array is dropped at /O2); /OPT:NOREF
    // keeps it at link time. This exercises the loader's .xbld detection.
    volatile const char* xbldKeep = kXbld;
    (void)*xbldKeep;
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

    // The device from the shim must be a REAL ID3D12Device, not the old
    // fake pointer: prove it by calling methods on it.
    void* dev = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                               __uuidof(ID3D12Device), &dev);
    ok = false;
    if (SUCCEEDED(hr) && dev) {
        ID3D12Device* d = (ID3D12Device*)dev;
        HRESULT removed = d->GetDeviceRemovedReason();
        UINT nodes = d->GetNodeCount();
        ok = (removed == S_OK) && (nodes >= 1);
        snprintf(detail, sizeof(detail),
                 "hr=0x%08x device=%p nodes=%u removed=0x%08x (REAL)",
                 (unsigned)hr, dev, nodes, (unsigned)removed);
    } else {
        snprintf(detail, sizeof(detail), "hr=0x%08x device=%p",
                 (unsigned)hr, dev);
    }
    Report("gfx_xs", ok, detail);
    allBoot &= ok;

    // First real d3d12_xs frame: swapchain off the shim device, clear,
    // present, fence-wait.
    detail[0] = 0;
    ok = false;
    if (dev) ok = TestXsFrame((ID3D12Device*)dev, detail, sizeof(detail));
    Report("frame_xs", ok, detail);
    allBoot &= ok;

    hr = SetFrameIntervalX(1, 0);
    ok = SUCCEEDED(hr);
    Report("present", ok, ok ? "frame interval set" : "SetFrameIntervalX failed");
    allBoot &= ok;
    hr = ScheduleFrameEventX(1);
    ok = SUCCEEDED(hr);
    Report("frame_evt", ok, nullptr);
    allBoot &= ok;
    // Real pacing: WaitFrameEventX must actually wait ~one frame period
    // (interval 1 = 16.7 ms). Measure it; >= 8 ms proves a real wait.
    LARGE_INTEGER qpcFreq, qpc0, qpc1;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpc0);
    hr = WaitFrameEventX(1, 1000);
    QueryPerformanceCounter(&qpc1);
    double waitedMs = (double)(qpc1.QuadPart - qpc0.QuadPart) * 1000.0 /
                      (double)qpcFreq.QuadPart;
    ok = SUCCEEDED(hr) && waitedMs >= 8.0;
    snprintf(detail, sizeof(detail), "waited %.1f ms (interval 1 = 16.7 ms)",
             waitedMs);
    Report("frame_wait", ok, detail);
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

    // Per-section page protections (loader, milestone C).
    detail[0] = 0;
    ok = TestProtections(detail, sizeof(detail));
    Report("protections", ok, detail);
    allBoot &= ok;

    // TLS (loader, milestone D): callback ran at load, __declspec(thread)
    // variable reads its template value (42) and is writable per-thread.
    detail[0] = 0;
    int tlsInit = g_tlsVar;
    bool tlsOk = g_tlsCallbackRan && (tlsInit == 42);
    g_tlsVar = 123;
    tlsOk = tlsOk && (g_tlsVar == 123);
    snprintf(detail, sizeof(detail), "callback=%d init=%d set=%d",
             (int)g_tlsCallbackRan, tlsInit, g_tlsVar);
    Report("tls", tlsOk, detail);
    allBoot &= tlsOk;

    // Services (milestone E): XUser / XAsync / XGameSave / XPackage.
    detail[0] = 0;
    ok = TestServices(detail, sizeof(detail));
    Report("services", ok, detail);
    allBoot &= ok;

    printf("[guest] boot path %s\n", allBoot ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allBoot ? 0 : 1;
}
