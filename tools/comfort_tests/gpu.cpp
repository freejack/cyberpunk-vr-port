// The cutscene vignette pass on the real adapter (src/Render/VignettePass.cpp): for every kind of eye
// target the submit can pick (PickMonoSwapchainFormat), a cleared image is darkened by the pass with an
// asymmetric FOV, read back, and EVERY pixel is compared with the CPU reference VignetteKeep(). That
// proves the HLSL is the header's maths, that u/v and the tangents are not swapped, that the gamma
// choice per target is right, and that the eye image's alpha is never written.
//   comfort_vignette_gpu [warp]
#include "Render/VignettePass.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cvr::comfort;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static bool Ok(HRESULT hr, const char* what) {
    if (FAILED(hr)) { std::printf("%s failed 0x%08lx\n", what, static_cast<unsigned long>(hr)); return false; }
    return true;
}

static float SrgbEncode(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}
static float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v;
    if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) v = mantissa ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -v : v;
}

enum class Encoding { Srgb8, Unorm8, Half, Unorm10 };
struct Case {
    const char* name;
    DXGI_FORMAT format;
    bool writtenLinear;   // the submit wrote LINEAR values into it (the decode path)
    Encoding encoding;
    bool bgra;
};

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
    bool Init(bool warp) {
        ComPtr<IDXGIFactory6> factory;
        if (!Ok(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory")) return false;
        ComPtr<IDXGIAdapter1> adapter;
        if (warp) {
            if (!Ok(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "warp adapter")) return false;
        } else if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter: %ls\n", desc.Description);
        if (!Ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device")) return false;
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return Ok(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue") &&
               Ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator") &&
               Ok(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list") &&
               Ok(list->Close(), "close") &&
               Ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence") &&
               (event = CreateEventA(nullptr, FALSE, FALSE, nullptr)) != nullptr;
    }
    bool Run() {
        if (!Ok(list->Close(), "close")) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!Ok(queue->Signal(fence.Get(), ++value), "signal")) return false;
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 10000);
        }
        return fence->GetCompletedValue() >= value && Ok(device->GetDeviceRemovedReason(), "device");
    }
};

static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    list->ResourceBarrier(1, &barrier);
}

// Decoded RGBA of one texel, in the target's own (stored) space.
static void Decode(const Case& c, const uint8_t* texel, float out[4]) {
    switch (c.encoding) {
        case Encoding::Srgb8:
        case Encoding::Unorm8:
            for (int i = 0; i < 4; ++i) out[i] = texel[i] / 255.0f;
            if (c.bgra) std::swap(out[0], out[2]);
            break;
        case Encoding::Half: {
            uint16_t h[4];
            std::memcpy(h, texel, sizeof(h));
            for (int i = 0; i < 4; ++i) out[i] = HalfToFloat(h[i]);
            break;
        }
        case Encoding::Unorm10: {
            uint32_t p;
            std::memcpy(&p, texel, 4);
            out[0] = (p & 1023) / 1023.0f;
            out[1] = ((p >> 10) & 1023) / 1023.0f;
            out[2] = ((p >> 20) & 1023) / 1023.0f;
            out[3] = (p >> 30) / 3.0f;
            break;
        }
    }
}
static uint32_t TexelBytes(Encoding e) { return e == Encoding::Half ? 8u : 4u; }

static void RunCase(Gpu& gpu, const Case& c, int size) {
    // Not square, so a swapped u/v or width/height shows up; an asymmetric eye, so a ring centred on
    // the image middle instead of the optical axis shows up.
    constexpr UINT W = 192, H = 160;
    const float clear[4] = {0.80f, 0.50f, 0.25f, 2.0f / 3.0f};   // alpha survives 2-bit storage exactly

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = W;
    desc.Height = H;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = c.format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = c.format;
    std::memcpy(clearValue.Color, clear, sizeof(clear));
    ComPtr<ID3D12Resource> target;
    if (!Ok(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue, IID_PPV_ARGS(&target)), "target")) { ++failures; return; }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (!Ok(gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "rtv heap")) { ++failures; return; }
    const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = readbackBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (!Ok(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&readback)), "readback")) { ++failures; return; }

    VignettePass pass;
    if (!pass.EnsureInitialized(gpu.device.Get(), c.format)) { std::printf("FAIL %s: init\n", c.name); ++failures; return; }
    CHECK(pass.EnsureInitialized(gpu.device.Get(), c.format));   // multi-call safe

    const bool linear = VignettePass::BlendsInLinear(c.format, c.writtenLinear);
    const VignetteSettings settings{3, 0.85f, size};
    const float r = kDegToRad;
    const VignetteShape shape = MakeVignetteShape(-52 * r, 43 * r, 48 * r, -55 * r, settings, EaseVignetteFade(0.9f), linear);
    // Nothing to draw: nothing recorded.
    CHECK(!pass.Record(gpu.list.Get(), rtv, W, H, MakeVignetteShape(-1, 1, 1, -1, settings, 0.0f, linear)));

    gpu.allocator->Reset();
    gpu.list->Reset(gpu.allocator.Get(), nullptr);
    gpu.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    const bool recorded = pass.Record(gpu.list.Get(), rtv, W, H, shape);
    CHECK(recorded);
    Barrier(gpu.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!gpu.Run()) { std::printf("FAIL %s: GPU run\n", c.name); ++failures; return; }

    uint8_t* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(readbackBytes)};
    if (!Ok(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map")) { ++failures; return; }

    // Tolerances: one step of the storage, plus a little for the GPU's transcendental precision.
    const float tolerance = c.encoding == Encoding::Half ? 4e-3f : (c.encoding == Encoding::Unorm10 ? 2.0f / 1023 : 2.0f / 255);
    int bad = 0, darkened = 0, untouched = 0;
    float worst = 0.0f;
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            const uint8_t* texel = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * TexelBytes(c.encoding);
            float got[4];
            Decode(c, texel, got);
            const float keep = VignetteKeep(shape, (x + 0.5f) / W, (y + 0.5f) / H);
            (keep < 0.999f ? darkened : untouched) += 1;
            // D3D lets the blend unit take a blend factor at the render target's precision, and hardware
            // does (measured on an RTX 5090: SRC_ALPHA 0.0255 becomes 6/255 on an 8-bit target; WARP
            // keeps float). So on 8-bit targets the factor may be either neighbouring 1/255 step. In the
            // darkest rim that is ~2 encoded levels of near-black, which no lens shows.
            float keeps[3] = {keep, keep, keep};
            if (c.encoding == Encoding::Srgb8 || c.encoding == Encoding::Unorm8) {
                keeps[1] = std::floor(keep * 255.0f) / 255.0f;
                keeps[2] = std::ceil(keep * 255.0f) / 255.0f;
            }
            for (int i = 0; i < 3; ++i) {
                // What the blend unit computes: in linear light for an sRGB view (decode, multiply,
                // encode), on the stored value otherwise.
                float error = INFINITY, expected = 0.0f;
                for (float k : keeps) {
                    const float e = c.encoding == Encoding::Srgb8 ? SrgbEncode(clear[i] * k) : clear[i] * k;
                    if (std::fabs(got[i] - e) < error) { error = std::fabs(got[i] - e); expected = e; }
                }
                worst = std::max(worst, error);
                if (error > tolerance && ++bad <= 5)
                    std::printf("  %s (%u,%u) ch%d got %.4f expected %.4f keep %.4f\n", c.name, x, y, i, got[i], expected, keep);
            }
            // The eye's alpha is never written.
            if (std::fabs(got[3] - clear[3]) > tolerance && ++bad <= 5)
                std::printf("  %s (%u,%u) alpha got %.4f expected %.4f\n", c.name, x, y, got[3], clear[3]);
        }
    }
    readback->Unmap(0, nullptr);
    CHECK(bad == 0);
    CHECK(darkened > 1000 && untouched > 500);   // a real ring, with a clear centre
    static const char* kSizeNames[] = {"light", "medium", "strong"};
    std::printf("%-34s %-6s linear=%d  darkened %5d  untouched %5d  worst error %.5f  %s\n", c.name,
                kSizeNames[NormalizeVignetteSize(size)], linear ? 1 : 0, darkened, untouched, worst, bad ? "FAIL" : "ok");
}

int main(int argc, char** argv) {
    const bool warp = argc > 1 && !std::strcmp(argv[1], "warp");
    Gpu gpu;
    if (!gpu.Init(warp)) {
        std::printf("comfort_vignette_gpu: no D3D12 device\n");
        return 1;
    }
    const Case cases[] = {
        // The usual pick: the game's bytes copied into their sRGB companion.
        {"R8G8B8A8_UNORM_SRGB (copy)", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, Encoding::Srgb8, false},
        {"B8G8R8A8_UNORM_SRGB (copy)", DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false, Encoding::Srgb8, true},
        // The decode path (SrgbToLinearPass) into a float or a plain UNORM target: linear values.
        {"R16G16B16A16_FLOAT (decoded)", DXGI_FORMAT_R16G16B16A16_FLOAT, true, Encoding::Half, false},
        {"R8G8B8A8_UNORM (decoded)", DXGI_FORMAT_R8G8B8A8_UNORM, true, Encoding::Unorm8, false},
        // The copy fallback in the game's own format: encoded values, blended as they are.
        {"R8G8B8A8_UNORM (copy, encoded)", DXGI_FORMAT_R8G8B8A8_UNORM, false, Encoding::Unorm8, false},
        {"R10G10B10A2_UNORM (copy, encoded)", DXGI_FORMAT_R10G10B10A2_UNORM, false, Encoding::Unorm10, false},
    };
    for (const Case& c : cases)
        for (int size = 0; size < kVignetteSizeCount; ++size) RunCase(gpu, c, size);
    // The gamma choice per target.
    CHECK(VignettePass::BlendsInLinear(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false));
    CHECK(VignettePass::BlendsInLinear(DXGI_FORMAT_R16G16B16A16_FLOAT, false));
    CHECK(VignettePass::BlendsInLinear(DXGI_FORMAT_R8G8B8A8_UNORM, true));
    CHECK(!VignettePass::BlendsInLinear(DXGI_FORMAT_R8G8B8A8_UNORM, false));
    CHECK(!VignettePass::BlendsInLinear(DXGI_FORMAT_R10G10B10A2_UNORM, false));
    if (gpu.event) CloseHandle(gpu.event);
    if (failures) {
        std::printf("comfort_vignette_gpu: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("comfort_vignette_gpu: all checks passed\n");
    return 0;
}
