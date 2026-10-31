#include "Render/VignettePass.hpp"

#include <cstring>
#include <d3dcompiler.h>

extern void Log(const char* fmt, ...);

namespace {
using Microsoft::WRL::ComPtr;

// One triangle over the whole viewport; uv runs 0..1 over the visible part, top-left origin, which is
// how the compositor maps the image rect onto the submitted FOV (v = 0 is angleUp).
constexpr char kVertexShader[] = R"(
struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    static const float2 positions[3] = { float2(-1.0, 1.0), float2(3.0, 1.0), float2(-1.0, -3.0) };
    static const float2 uvs[3] = { float2(0.0, 0.0), float2(2.0, 0.0), float2(0.0, 2.0) };
    VSOut o;
    o.position = float4(positions[vertexId], 0.0, 1.0);
    o.uv = uvs[vertexId];
    return o;
}
)";

// cvr::comfort::VignetteKeep (Runtimes/CutsceneVignette.hpp), line for line. The output alpha is the
// factor the destination is multiplied by (blend ZERO / SRC_ALPHA); colour writes only, so the eye
// image's own alpha is left as it was.
constexpr char kPixelShader[] = R"(
cbuffer Vignette : register(b0) {
    float4 g_tan;    // tan(angleLeft), tan(angleRight), tan(angleUp), tan(angleDown)
    float4 g_shape;  // innerRad, outerRad, darken, exponent
};

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VSOut input) : SV_Target {
    const float tx = g_tan.x + (g_tan.y - g_tan.x) * input.uv.x;
    const float ty = g_tan.z + (g_tan.w - g_tan.z) * input.uv.y;
    const float angle = atan(sqrt(tx * tx + ty * ty));
    const float span = max(g_shape.y - g_shape.x, 1e-4);
    const float t = saturate((angle - g_shape.x) / span);
    const float ramp = t * t * (3.0 - 2.0 * t);
    const float darken = g_shape.z * ramp;
    if (darken <= 0.0005) discard;
    return float4(0.0, 0.0, 0.0, pow(1.0 - darken, g_shape.w));
}
)";

bool CompileShader(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (FAILED(hr)) {
        Log("VignettePass: shader compile failed %s/%s hr=0x%08X %.*s\n",
            entry, target, static_cast<unsigned>(hr),
            errors ? static_cast<int>(errors->GetBufferSize()) : 0,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    return true;
}
}  // namespace

VignettePass::~VignettePass() {
    Shutdown();
}

void VignettePass::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_device.Reset();
    m_format = DXGI_FORMAT_UNKNOWN;
}

bool VignettePass::BlendsInLinear(DXGI_FORMAT rtvFormat, bool writtenLinear) {
    switch (rtvFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return writtenLinear;
    }
}

bool VignettePass::EnsureInitialized(ID3D12Device* device, DXGI_FORMAT rtvFormat) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!device || rtvFormat == DXGI_FORMAT_UNKNOWN) return false;
    if (m_device.Get() == device && m_format == rtvFormat && m_pipelineState) return true;

    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_device = device;
    m_format = rtvFormat;

    D3D12_ROOT_PARAMETER constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 0;
    constants.Constants.Num32BitValues = sizeof(cvr::comfort::VignetteShape) / sizeof(uint32_t);
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &constants;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rootBlob, rootErrors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors))) {
        Log("VignettePass: root signature serialization failed %.*s\n",
            rootErrors ? static_cast<int>(rootErrors->GetBufferSize()) : 0,
            rootErrors ? static_cast<const char*>(rootErrors->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)))) {
        Log("VignettePass: failed to create root signature\n");
        return false;
    }

    ComPtr<ID3DBlob> vertexShader, pixelShader;
    if (!CompileShader(kVertexShader, "VSMain", "vs_5_0", vertexShader) ||
        !CompileShader(kPixelShader, "PSMain", "ps_5_0", pixelShader)) {
        m_rootSignature.Reset();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pso.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    // dst.rgb *= src.a; the eye's alpha is not touched.
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ZERO;
    blend.DestBlend = D3D12_BLEND_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipelineState)))) {
        Log("VignettePass: failed to create pipeline state for format %u\n", static_cast<unsigned>(rtvFormat));
        m_rootSignature.Reset();
        return false;
    }
    Log("VignettePass: initialized rtvFmt=%u\n", static_cast<unsigned>(rtvFormat));
    return true;
}

bool VignettePass::Record(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                          uint32_t width, uint32_t height, const cvr::comfort::VignetteShape& shape) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!cmdList || !rtv.ptr || !width || !height || !m_pipelineState || !m_rootSignature) return false;
    if (!(shape.darken > cvr::comfort::kVignetteDiscardBelow)) return false;

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pipelineState.Get());
    cmdList->SetGraphicsRoot32BitConstants(0, sizeof(shape) / sizeof(uint32_t), &shape, 0);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
    return true;
}
