// ScarlettCompat shader pipeline - Phase 2: GPU render check (MIT, clean-room).
#include "shader/sc_gpu_render_test.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#endif

namespace sc {

std::string sc_gpu_caps::to_string() const {
    if (!valid) {
        return std::string("caps unavailable") + (note.empty() ? std::string() : (": " + note));
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "adapter=\"%s\" vendor=0x%04x device=0x%04x FL=0x%x SM=%u.%u wave=%u..%u mesh_tier=%u rt_tier=%u",
                  adapter_name.c_str(), vendor_id, device_id, feature_level, shader_model_major,
                  shader_model_minor, wave_lane_min, wave_lane_max, mesh_shader_tier, raytracing_tier);
    return std::string(buf);
}

#ifdef _WIN32
namespace {

constexpr uint32_t kRtvFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

std::string narrow_ascii(const wchar_t* w) {
    std::string s;
    if (w == nullptr) {
        return s;
    }
    for (; *w != 0; ++w) {
        s.push_back((*w > 0 && *w < 128) ? static_cast<char>(*w) : '?');
    }
    return s;
}

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 0;
    hp.VisibleNodeMask = 0;
    return hp;
}

D3D12_RESOURCE_BARRIER make_transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

uint32_t align_up(uint32_t value, uint32_t alignment) { return (value + alignment - 1u) & ~(alignment - 1u); }

std::string device_removed_reason(ID3D12Device* device) {
    if (device == nullptr) {
        return std::string();
    }
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (reason == S_OK) {
        return std::string();
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), " (device removed reason hr=0x%08lx)", static_cast<unsigned long>(reason));
    return std::string(buf);
}

// Picks the first adapter that can create a D3D12 device and fills `caps`
// from that device.
bool create_best_device(ComPtr<ID3D12Device>& device, sc_gpu_caps& caps) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        caps.note = "CreateDXGIFactory1 failed";
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0};
    for (UINT i = 0; i < 16; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ComPtr<ID3D12Device> candidate;
        uint32_t level = 0;
        for (D3D_FEATURE_LEVEL lv : levels) {
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), lv, IID_PPV_ARGS(&candidate)))) {
                level = static_cast<uint32_t>(lv);
                break;
            }
        }
        if (!candidate) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            caps.adapter_name = narrow_ascii(desc.Description);
            caps.vendor_id = desc.VendorId;
            caps.device_id = desc.DeviceId;
        }
        caps.feature_level = level;

        for (uint32_t v = 0x67; v >= 0x60; --v) {
            D3D12_FEATURE_DATA_SHADER_MODEL sm{};
            sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(v);
            if (SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                static_cast<uint32_t>(sm.HighestShaderModel) >= v) {
                caps.shader_model_major = v >> 4;
                caps.shader_model_minor = v & 0xfu;
                break;
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)))) {
            caps.wave_lane_min = options1.WaveLaneCountMin;
            caps.wave_lane_max = options1.WaveLaneCountMax;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
            caps.mesh_shader_tier = static_cast<uint32_t>(options7.MeshShaderTier);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
            caps.raytracing_tier = static_cast<uint32_t>(options5.RaytracingTier);
        }

        caps.valid = true;
        device = candidate;
        return true;
    }
    caps.note = "no D3D12-capable adapter";
    return false;
}

} // namespace
#endif // _WIN32

sc_gpu_caps sc_query_gpu_caps() {
    sc_gpu_caps caps;
#ifdef _WIN32
    ComPtr<ID3D12Device> device;
    create_best_device(device, caps);
#else
    caps.note = "GPU render check is Windows-only";
#endif
    return caps;
}

bool sc_render_synthetic_triangle(const std::vector<uint8_t>& vs_dxil, const std::vector<uint8_t>& ps_dxil,
                                  uint32_t width, uint32_t height, const uint8_t clear[4],
                                  sc_triangle_render_result& out) {
    out = sc_triangle_render_result{};
    out.width = width;
    out.height = height;
    if (clear != nullptr) {
        std::memcpy(out.clear_rgba, clear, 4);
    }
#ifdef _WIN32
    if (vs_dxil.empty() || ps_dxil.empty()) {
        out.errors.push_back("empty shader bytecode");
        return false;
    }
    if (width == 0 || height == 0) {
        out.errors.push_back("zero-sized target");
        return false;
    }

    ComPtr<ID3D12Device> device;
    if (!create_best_device(device, out.caps)) {
        out.errors.push_back("device: " + out.caps.note);
        return false;
    }

    auto fail = [&](const char* what, HRESULT hr) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed hr=0x%08lx%s", what, static_cast<unsigned long>(hr),
                      device_removed_reason(device.Get()).c_str());
        out.errors.push_back(buf);
        return false;
    };

    // ---- root signature: no parameters, input assembler allowed ----------------
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 0;
    root_desc.pParameters = nullptr;
    root_desc.NumStaticSamplers = 0;
    root_desc.pStaticSamplers = nullptr;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> sig_error;
    HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &sig_error);
    if (FAILED(hr)) {
        if (sig_error) {
            out.errors.push_back(std::string("root signature: ") +
                                 static_cast<const char*>(sig_error->GetBufferPointer()));
        }
        return fail("D3D12SerializeRootSignature", hr);
    }
    ComPtr<ID3D12RootSignature> root_signature;
    hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                     IID_PPV_ARGS(&root_signature));
    if (FAILED(hr)) {
        return fail("CreateRootSignature", hr);
    }

    // ---- pipeline state from the emitted DXIL ----------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature.Get();
    pso.VS.pShaderBytecode = vs_dxil.data();
    pso.VS.BytecodeLength = vs_dxil.size();
    pso.PS.pShaderBytecode = ps_dxil.data();
    pso.PS.BytecodeLength = ps_dxil.size();

    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& rt0 = pso.BlendState.RenderTarget[0];
    rt0.BlendEnable = FALSE;
    rt0.LogicOpEnable = FALSE;
    rt0.SrcBlend = D3D12_BLEND_ONE;
    rt0.DestBlend = D3D12_BLEND_ZERO;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt0.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = FALSE;
    pso.RasterizerState.AntialiasedLineEnable = FALSE;
    pso.RasterizerState.ForcedSampleCount = 0;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    pso.SampleMask = 0xffffffffu;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = static_cast<DXGI_FORMAT>(kRtvFormat);
    pso.SampleDesc.Count = 1;
    pso.SampleDesc.Quality = 0;

    ComPtr<ID3D12PipelineState> pipeline;
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        return fail("CreateGraphicsPipelineState", hr);
    }
    out.pso_created = true;

    // ---- command objects -------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        return fail("CreateCommandQueue", hr);
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        return fail("CreateCommandAllocator", hr);
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        return fail("CreateCommandList", hr);
    }
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        return fail("CreateFence", hr);
    }
    HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
        out.errors.push_back("CreateEvent failed");
        return false;
    }

    // ---- render target + readback ---------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) {
        CloseHandle(fence_event);
        return fail("CreateDescriptorHeap", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = width;
    target_desc.Height = height;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = static_cast<DXGI_FORMAT>(kRtvFormat);
    target_desc.SampleDesc.Count = 1;
    target_desc.SampleDesc.Quality = 0;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = target_desc.Format;
    for (int i = 0; i < 4; ++i) {
        clear_value.Color[i] = static_cast<float>(out.clear_rgba[i]) / 255.0f;
    }

    const D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target;
    hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                         IID_PPV_ARGS(&target));
    if (FAILED(hr)) {
        CloseHandle(fence_event);
        return fail("CreateCommittedResource(target)", hr);
    }
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    const uint32_t row_pitch = align_up(width * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = static_cast<UINT64>(row_pitch) * height;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.SampleDesc.Quality = 0;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        CloseHandle(fence_event);
        return fail("CreateCommittedResource(readback)", hr);
    }

    // ---- record --------------------------------------------------------------
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear_value.Color, 0, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
    list->RSSetScissorRects(1, &scissor);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetPipelineState(pipeline.Get());
    list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER to_copy =
        make_transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION copy_dst{};
    copy_dst.pResource = readback.Get();
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_dst.PlacedFootprint.Offset = 0;
    copy_dst.PlacedFootprint.Footprint.Format = target_desc.Format;
    copy_dst.PlacedFootprint.Footprint.Width = width;
    copy_dst.PlacedFootprint.Footprint.Height = height;
    copy_dst.PlacedFootprint.Footprint.Depth = 1;
    copy_dst.PlacedFootprint.Footprint.RowPitch = row_pitch;

    D3D12_TEXTURE_COPY_LOCATION copy_src{};
    copy_src.pResource = target.Get();
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_src.SubresourceIndex = 0;
    list->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);

    D3D12_RESOURCE_BARRIER to_draw = make_transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_draw);

    hr = list->Close();
    if (FAILED(hr)) {
        CloseHandle(fence_event);
        return fail("Close", hr);
    }

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);

    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr)) {
        CloseHandle(fence_event);
        return fail("Signal", hr);
    }
    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, fence_event);
        if (FAILED(hr)) {
            CloseHandle(fence_event);
            return fail("SetEventOnCompletion", hr);
        }
        if (WaitForSingleObject(fence_event, 20000) != WAIT_OBJECT_0) {
            CloseHandle(fence_event);
            out.errors.push_back("fence wait timed out" + device_removed_reason(device.Get()));
            return false;
        }
    }
    out.drew = true;

    // ---- read the two probe pixels ------------------------------------------
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(row_pitch) * height};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        CloseHandle(fence_event);
        return fail("Map(readback)", hr);
    }
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
    const uint32_t center_x = width / 2;
    const uint32_t center_y = height / 2;
    std::memcpy(out.center_rgba, pixels + static_cast<size_t>(center_y) * row_pitch + center_x * 4u, 4);
    std::memcpy(out.corner_rgba, pixels, 4);
    const D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);

    CloseHandle(fence_event);
    out.ok = out.errors.empty();
    return out.ok;
#else
    (void)vs_dxil;
    (void)ps_dxil;
    out.errors.push_back("GPU render check is Windows-only");
    return false;
#endif
}

bool sc_run_synthetic_triangle_test(const std::vector<uint8_t>& vs_dxil, const std::vector<uint8_t>& ps_dxil,
                                    sc_triangle_render_result& out) {
    const uint8_t clear[4] = {0, 0, 0, 255};
    return sc_render_synthetic_triangle(vs_dxil, ps_dxil, 64, 64, clear, out);
}

} // namespace sc
