// The cutscene comfort vignette, drawn straight into an eye's swapchain image on the submit command
// list (Runtimes/CutsceneVignette.hpp has the why and the geometry). One fullscreen triangle whose pixel
// shader multiplies what is already there; the inside of the ring is discarded, so it costs a blend
// only where it darkens.
//
// Root constants only -- no descriptor heap, no texture, nothing that lives with a command list. The
// submit keeps three lists in flight, and a pass with nothing to retire cannot have it rewritten under
// a list the GPU is still executing.
#pragma once

#include "Runtimes/CutsceneVignette.hpp"

#include <windows.h>
#include <d3d12.h>
#include <mutex>
#include <wrl.h>

class VignettePass {
public:
    VignettePass() = default;
    ~VignettePass();

    // rtvFormat: the typed format the destination's render target view was created with.
    // Multi-call safe; rebuilds only when the device or the format changes.
    bool EnsureInitialized(ID3D12Device* device, DXGI_FORMAT rtvFormat);
    // The destination must be in RENDER_TARGET and `rtv` a view of it in rtvFormat. Leaves the
    // command list's render target, viewport, scissor, root signature and pipeline set to its own.
    // Returns false (and records nothing) when there is nothing to draw or the pass is not ready.
    bool Record(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                uint32_t width, uint32_t height, const cvr::comfort::VignetteShape& shape);
    void Shutdown();

    // Whether a target of this format blends in linear light: an _SRGB view decodes before the blend
    // and encodes after, and float formats hold linear values. A plain UNORM target is only linear when
    // the submit wrote linear values into it (the decode path, SrgbToLinearPass) -- the caller knows.
    static bool BlendsInLinear(DXGI_FORMAT rtvFormat, bool writtenLinear);

private:
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
};
