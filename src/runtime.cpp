#include "runtime.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <intrin.h>

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>
#include <reshade_api.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <memory>
#include <share.h>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace luminex {
namespace {

using PresentFunction = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFunction = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using OMSetRenderTargetsFunction = void(__stdcall*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using GetAsyncKeyStateFunction = SHORT(WINAPI*)(int);
using GetKeyStateFunction = SHORT(WINAPI*)(int);
using GetKeyboardStateFunction = BOOL(WINAPI*)(PBYTE);
using ClipCursorFunction = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFunction = BOOL(WINAPI*)(int, int);
using GetRawInputDataFunction = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFunction = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);

HMODULE g_module = nullptr;
std::filesystem::path g_moduleDirectory;
FILE* g_log = nullptr;
std::atomic_bool g_stopping = false;
std::atomic_bool g_effectFaulted = false;
// Only application rendering may contribute scene-depth candidates.
thread_local bool g_renderingLuminex = false;
std::atomic_uint64_t g_blockedRawInputDataMouse = 0;
std::atomic_uint64_t g_blockedRawInputBufferMouse = 0;
std::atomic_uint64_t g_blockedRawInputDataKeyboard = 0;
std::atomic_uint64_t g_blockedRawInputBufferKeyboard = 0;
std::atomic_uint64_t g_blockedWindowWheel = 0;

PresentFunction g_originalPresent = nullptr;
ResizeBuffersFunction g_originalResizeBuffers = nullptr;
OMSetRenderTargetsFunction g_originalOMSetRenderTargets = nullptr;
GetAsyncKeyStateFunction g_originalGetAsyncKeyState = nullptr;
GetKeyStateFunction g_originalGetKeyState = nullptr;
GetKeyboardStateFunction g_originalGetKeyboardState = nullptr;
ClipCursorFunction g_originalClipCursor = nullptr;
SetCursorPosFunction g_originalSetCursorPos = nullptr;
GetRawInputDataFunction g_originalGetRawInputData = nullptr;
GetRawInputBufferFunction g_originalGetRawInputBuffer = nullptr;
IDXGISwapChain* g_primarySwapChain = nullptr;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11Device1> g_device1;
ComPtr<ID3D11DeviceContext1> g_context1;
ComPtr<ID3DDeviceContextState> g_luminexContextState;
ComPtr<ID3D11RenderTargetView> g_backBufferRtv;
ComPtr<ID3D11Texture2D> g_colorCopy;
ComPtr<ID3D11ShaderResourceView> g_colorSrv;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_pixelShader;
ComPtr<ID3D11PixelShader> g_bloomExtractShader;
ComPtr<ID3D11PixelShader> g_bloomBlurHShader;
ComPtr<ID3D11PixelShader> g_bloomBlurVShader;
ComPtr<ID3D11Buffer> g_settingsBuffer;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11Texture2D> g_bloomTextureA;
ComPtr<ID3D11Texture2D> g_bloomTextureB;
ComPtr<ID3D11ShaderResourceView> g_bloomSrvA;
ComPtr<ID3D11ShaderResourceView> g_bloomSrvB;
ComPtr<ID3D11RenderTargetView> g_bloomRtvA;
ComPtr<ID3D11RenderTargetView> g_bloomRtvB;

HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_imguiReady = false;
bool g_resourcesReady = false;
bool g_loggedColorCopy = false;
bool g_loggedColorDraw = false;
bool g_menuOpen = false;
bool g_effectEnabled = true;
bool g_depthPreview = false;
bool g_depthInvert = false;
bool g_depthFogEnabled = false;
bool g_sharpenEnabled = false;
bool g_vignetteEnabled = false;
bool g_bloomEnabled = false;
bool g_chromaticEnabled = false;
bool g_grainEnabled = false;
bool g_sepiaEnabled = false;
bool g_posterizeEnabled = false;
bool g_dofEnabled = false;
bool g_depthOutlineEnabled = false;
bool g_ssaoEnabled = false;
bool g_ssrEnabled = false;
std::atomic_bool g_collectDepth = false;
float g_exposure = 0.0f;
float g_contrast = 1.05f;
float g_saturation = 1.08f;
float g_depthPower = 1.0f;
float g_fogStart = 0.05f;
float g_fogEnd = 0.6f;
float g_fogStrength = 0.35f;
float g_fogColor[3] = { 0.65f, 0.72f, 0.82f };
float g_sharpenStrength = 0.25f;
float g_vignetteStrength = 0.25f;
float g_gamma = 1.0f;
float g_vibrance = 0.0f;
float g_temperature = 0.0f;
float g_tint = 0.0f;
int g_tonemapMode = 0;
float g_bloomThreshold = 0.75f;
float g_bloomStrength = 0.20f;
float g_bloomRadius = 2.0f;
float g_chromaticStrength = 1.5f;
float g_grainStrength = 0.035f;
float g_sepiaStrength = 0.45f;
float g_posterizeLevels = 8.0f;
float g_dofFocus = 0.20f;
float g_dofRange = 0.08f;
float g_dofStrength = 4.0f;
float g_depthOutlineThreshold = 0.012f;
float g_depthOutlineStrength = 0.65f;
float g_ssaoRadius = 3.0f;
float g_ssaoStrength = 0.35f;
float g_ssrStrength = 0.65f;
float g_ssrMaxDistance = 0.18f;
float g_ssrThickness = 0.025f;
float g_runtimeCpuMs = 0.0f;
float g_presentFps = 0.0f;
int g_hookLevel = 1;
int g_colorStage = 1;
UINT g_backBufferWidth = 0;
UINT g_backBufferHeight = 0;

struct DepthCandidate
{
    ComPtr<ID3D11DepthStencilView> view;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bindFlags = 0;
    UINT sampleCount = 1;
    UINT arraySize = 1;
    UINT mipLevels = 1;
    UINT bindings = 0;
};

struct DepthCandidateInfo
{
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bindFlags = 0;
    UINT sampleCount = 1;
    UINT arraySize = 1;
    UINT mipLevels = 1;
    UINT bindings = 0;
    bool live = false;
};

struct DepthCopy
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct DepthSelectionKey
{
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT sampleCount = 0;
    UINT arraySize = 0;
    UINT mipLevels = 0;
    bool valid = false;
};

std::mutex g_depthMutex;
std::vector<DepthCandidate> g_depthCandidates;
std::vector<DepthCandidateInfo> g_lastDepthCandidates;
int g_selectedDepth = -1;
DepthSelectionKey g_selectedDepthKey;
bool g_depthSelectionManual = false;
DepthCopy g_depthCopy;
std::vector<std::filesystem::path> g_presetPaths;
std::string g_activePreset = "Custom";

struct FxTextureBinding
{
    UINT slot = 0;
    bool depth = false;
};

enum class FxUniformKind { Boolean, Integer, FloatingPoint };

struct FxUniformControl
{
    std::string name;
    std::string label;
    std::string uiType;
    FxUniformKind kind = FxUniformKind::FloatingPoint;
    UINT offset = 0;
    UINT components = 1;
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct FxTechniqueRuntime
{
    std::string name;
    std::string source;
    bool enabled = false;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> uniformBuffer;
    std::vector<unsigned char> uniformData;
    std::vector<FxUniformControl> uniforms;
    std::vector<FxTextureBinding> textures;
    std::vector<UINT> samplers;
};

std::vector<FxTechniqueRuntime> g_fxTechniques;
std::vector<std::string> g_fxMessages;

using ReShadeInitializeFunction = bool(*)(const wchar_t*);
using ReShadeHandleMessageFunction = bool(*)(const MSG*);
using ReShadeIsOverlayOpenFunction = bool(*)(reshade::api::effect_runtime*);
using ReShadeCreateRuntimeFunction = bool(*)(reshade::api::device_api, void*, void*, void*, const char*, reshade::api::effect_runtime**);
using ReShadeDestroyRuntimeFunction = void(*)(reshade::api::effect_runtime*);
using ReShadePresentRuntimeFunction = void(*)(reshade::api::effect_runtime*);
using ReShadeBeginResizeFunction = void(*)(reshade::api::effect_runtime*);
using ReShadeEndResizeFunction = bool(*)(reshade::api::effect_runtime*);

HMODULE g_reshadeModule = nullptr;
reshade::api::effect_runtime* g_reshadeRuntime = nullptr;
ReShadeDestroyRuntimeFunction g_reshadeDestroyRuntime = nullptr;
ReShadePresentRuntimeFunction g_reshadePresentRuntime = nullptr;
ReShadeHandleMessageFunction g_reshadeHandleMessage = nullptr;
ReShadeIsOverlayOpenFunction g_reshadeIsOverlayOpen = nullptr;
ReShadeBeginResizeFunction g_reshadeBeginResize = nullptr;
ReShadeEndResizeFunction g_reshadeEndResize = nullptr;
ComPtr<ID3D11ShaderResourceView> g_reshadeDepthSrv;
bool g_reshadeAttempted = false;

struct alignas(16) ShaderSettings
{
    float exposure;
    float contrast;
    float saturation;
    float depthPreview;
    float depthAvailable;
    float depthInvert;
    float depthPower;
    float padding;
    float fogEnabled;
    float fogStart;
    float fogEnd;
    float fogStrength;
    float fogColor[3];
    float padding2;
    float sharpenEnabled;
    float sharpenStrength;
    float texelWidth;
    float texelHeight;
    float vignetteEnabled;
    float vignetteStrength;
    float gamma;
    float vibrance;
    float temperature;
    float tint;
    float tonemapMode;
    float bloomEnabled;
    float bloomThreshold;
    float bloomStrength;
    float bloomRadius;
    float chromaticEnabled;
    float chromaticStrength;
    float grainEnabled;
    float grainStrength;
    float time;
    float sepiaEnabled;
    float sepiaStrength;
    float posterizeEnabled;
    float posterizeLevels;
    float dofEnabled;
    float dofFocus;
    float dofRange;
    float dofStrength;
    float depthOutlineEnabled;
    float depthOutlineThreshold;
    float depthOutlineStrength;
    float ssaoEnabled;
    float ssaoRadius;
    float ssaoStrength;
    float ssrEnabled;
    float ssrStrength;
    float ssrMaxDistance;
    float ssrThickness;
    float padding3[2];
};
static_assert(sizeof(ShaderSettings) == 224, "ShaderSettings must match the HLSL constant buffer");

constexpr const char* ShaderSource = R"(
cbuffer LuminexSettings : register(b0)
{
    float Exposure;
    float Contrast;
    float Saturation;
    float DepthPreview;
    float DepthAvailable;
    float DepthInvert;
    float DepthPower;
    float Padding;
    float FogEnabled;
    float FogStart;
    float FogEnd;
    float FogStrength;
    float3 FogColor;
    float Padding2;
    float SharpenEnabled;
    float SharpenStrength;
    float TexelWidth;
    float TexelHeight;
    float VignetteEnabled;
    float VignetteStrength;
    float Gamma;
    float Vibrance;
    float Temperature;
    float Tint;
    float TonemapMode;
    float BloomEnabled;
    float BloomThreshold;
    float BloomStrength;
    float BloomRadius;
    float ChromaticEnabled;
    float ChromaticStrength;
    float GrainEnabled;
    float GrainStrength;
    float Time;
    float SepiaEnabled;
    float SepiaStrength;
    float PosterizeEnabled;
    float PosterizeLevels;
    float DofEnabled;
    float DofFocus;
    float DofRange;
    float DofStrength;
    float DepthOutlineEnabled;
    float DepthOutlineThreshold;
    float DepthOutlineStrength;
    float SsaoEnabled;
    float SsaoRadius;
    float SsaoStrength;
    float SsrEnabled;
    float SsrStrength;
    float SsrMaxDistance;
    float SsrThickness;
    float2 Padding3;
};

Texture2D ColorTexture : register(t0);
Texture2D DepthTexture : register(t1);
Texture2D BloomTexture : register(t2);
SamplerState LinearSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint id : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSBloomExtract(VertexOutput input) : SV_Target
{
    float2 offset = float2(TexelWidth, TexelHeight);
    float3 color = ColorTexture.SampleLevel(LinearSampler, input.uv, 0).rgb * 0.4;
    color += ColorTexture.SampleLevel(LinearSampler, input.uv + offset, 0).rgb * 0.15;
    color += ColorTexture.SampleLevel(LinearSampler, input.uv - offset, 0).rgb * 0.15;
    color += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(offset.x, -offset.y), 0).rgb * 0.15;
    color += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(-offset.x, offset.y), 0).rgb * 0.15;
    float brightness = max(color.r, max(color.g, color.b));
    color *= saturate((brightness - BloomThreshold) / max(brightness, 0.0001));
    return float4(color, 1.0);
}

float4 BloomBlur(float2 uv, float2 direction) : SV_Target
{
    float2 stepSize = direction * float2(TexelWidth, TexelHeight) * 2.0 * BloomRadius;
    float3 color = ColorTexture.SampleLevel(LinearSampler, uv, 0).rgb * 0.227027;
    color += ColorTexture.SampleLevel(LinearSampler, uv + stepSize * 1.384615, 0).rgb * 0.316216;
    color += ColorTexture.SampleLevel(LinearSampler, uv - stepSize * 1.384615, 0).rgb * 0.316216;
    color += ColorTexture.SampleLevel(LinearSampler, uv + stepSize * 3.230769, 0).rgb * 0.070270;
    color += ColorTexture.SampleLevel(LinearSampler, uv - stepSize * 3.230769, 0).rgb * 0.070270;
    return float4(color, 1.0);
}

float4 PSBloomBlurH(VertexOutput input) : SV_Target { return BloomBlur(input.uv, float2(1, 0)); }
float4 PSBloomBlurV(VertexOutput input) : SV_Target { return BloomBlur(input.uv, float2(0, 1)); }

float4 PSMain(VertexOutput input) : SV_Target
{
    if (DepthPreview > 0.5 && DepthAvailable > 0.5)
    {
        float depth = saturate(DepthTexture.SampleLevel(LinearSampler, input.uv, 0).r);
        if (DepthInvert > 0.5)
            depth = 1.0 - depth;
        depth = pow(depth, max(DepthPower, 0.01));
        return float4(depth.xxx, 1.0);
    }

    float rawDepth = DepthAvailable > 0.5
        ? saturate(DepthTexture.SampleLevel(LinearSampler, input.uv, 0).r)
        : 0.0;
    float2 texel = float2(TexelWidth, TexelHeight);
    float2 centered = input.uv - 0.5;
    float3 color = ColorTexture.Sample(LinearSampler, input.uv).rgb;
    if (ChromaticEnabled > 0.5)
    {
        float2 offset = centered * texel * ChromaticStrength * 3.0;
        color.r = ColorTexture.SampleLevel(LinearSampler, input.uv + offset, 0).r;
        color.b = ColorTexture.SampleLevel(LinearSampler, input.uv - offset, 0).b;
    }
    if (DofEnabled > 0.5 && DepthAvailable > 0.5)
    {
        float distanceDepth = DepthInvert > 0.5 ? 1.0 - rawDepth : rawDepth;
        float blur = saturate(abs(distanceDepth - DofFocus) / max(DofRange, 0.001));
        float2 radius = texel * DofStrength * blur;
        float3 blurred = 0.0;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(radius.x, 0), 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv - float2(radius.x, 0), 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(0, radius.y), 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv - float2(0, radius.y), 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv + radius, 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv - radius, 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(radius.x, -radius.y), 0).rgb;
        blurred += ColorTexture.SampleLevel(LinearSampler, input.uv + float2(-radius.x, radius.y), 0).rgb;
        color = lerp(color, blurred * 0.125, blur);
    }
    if (SharpenEnabled > 0.5)
    {
        float3 neighbors =
            ColorTexture.SampleLevel(LinearSampler, input.uv + float2(texel.x, 0.0), 0).rgb +
            ColorTexture.SampleLevel(LinearSampler, input.uv - float2(texel.x, 0.0), 0).rgb +
            ColorTexture.SampleLevel(LinearSampler, input.uv + float2(0.0, texel.y), 0).rgb +
            ColorTexture.SampleLevel(LinearSampler, input.uv - float2(0.0, texel.y), 0).rgb;
        color += (color - neighbors * 0.25) * SharpenStrength;
    }
    if (BloomEnabled > 0.5)
        color += BloomTexture.SampleLevel(LinearSampler, input.uv, 0).rgb * BloomStrength;
    color *= exp2(Exposure);
    color = (color - 0.5) * Contrast + 0.5;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(luminance.xxx, color, Saturation);
    float saturationMax = max(color.r, max(color.g, color.b));
    float saturationMin = min(color.r, min(color.g, color.b));
    float colorfulness = saturationMax - saturationMin;
    color = lerp(luminance.xxx, color, 1.0 + Vibrance * (1.0 - colorfulness));
    color *= float3(1.0 + Temperature * 0.12, 1.0 + Tint * 0.06, 1.0 - Temperature * 0.12);
    color.g *= 1.0 - Tint * 0.08;
    color = pow(max(color, 0.0), 1.0 / max(Gamma, 0.05));
    if (TonemapMode > 0.5 && TonemapMode < 1.5)
        color = color / (1.0 + color);
    else if (TonemapMode >= 1.5)
        color = saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
    if (FogEnabled > 0.5 && DepthAvailable > 0.5)
    {
        float distanceDepth = DepthInvert > 0.5 ? 1.0 - rawDepth : rawDepth;
        float fog = smoothstep(FogStart, max(FogEnd, FogStart + 0.0001), distanceDepth);
        color = lerp(color, FogColor, saturate(fog * FogStrength));
    }
    if (SsaoEnabled > 0.5 && DepthAvailable > 0.5)
    {
        float distanceDepth = DepthInvert > 0.5 ? 1.0 - rawDepth : rawDepth;
        float2 radius = texel * SsaoRadius;
        float d0 = DepthTexture.SampleLevel(LinearSampler, input.uv + float2(radius.x, 0), 0).r;
        float d1 = DepthTexture.SampleLevel(LinearSampler, input.uv - float2(radius.x, 0), 0).r;
        float d2 = DepthTexture.SampleLevel(LinearSampler, input.uv + float2(0, radius.y), 0).r;
        float d3 = DepthTexture.SampleLevel(LinearSampler, input.uv - float2(0, radius.y), 0).r;
        if (DepthInvert > 0.5) { d0 = 1-d0; d1 = 1-d1; d2 = 1-d2; d3 = 1-d3; }
        float occlusion = saturate(((distanceDepth-d0)+(distanceDepth-d1)+(distanceDepth-d2)+(distanceDepth-d3)) * 10.0);
        color *= 1.0 - occlusion * SsaoStrength;
    }
    if (DepthOutlineEnabled > 0.5 && DepthAvailable > 0.5)
    {
        float dx = abs(rawDepth - DepthTexture.SampleLevel(LinearSampler, input.uv + float2(texel.x, 0), 0).r);
        float dy = abs(rawDepth - DepthTexture.SampleLevel(LinearSampler, input.uv + float2(0, texel.y), 0).r);
        float edge = smoothstep(DepthOutlineThreshold, DepthOutlineThreshold * 3.0, max(dx, dy));
        color *= 1.0 - edge * DepthOutlineStrength;
    }
    if (SsrEnabled > 0.5 && DepthAvailable > 0.5)
    {
        float centerDepth = DepthInvert > 0.5 ? 1.0 - rawDepth : rawDepth;
        float depthRight = DepthTexture.SampleLevel(LinearSampler, input.uv + float2(texel.x, 0), 0).r;
        float depthDown = DepthTexture.SampleLevel(LinearSampler, input.uv + float2(0, texel.y), 0).r;
        if (DepthInvert > 0.5) { depthRight = 1.0-depthRight; depthDown = 1.0-depthDown; }
        float3 normal = normalize(float3((centerDepth-depthRight) * 180.0,
                                         (centerDepth-depthDown) * 180.0, 1.0));
        float3 viewRay = normalize(float3(centered * 1.8, 1.0));
        float3 reflected = reflect(viewRay, normal);
        float2 direction = normalize(reflected.xy + float2(0.0001, 0.0001));
        float2 rayUv = input.uv;
        float3 reflectionColor = 0.0;
        float hit = 0.0;
        [unroll] for (int stepIndex = 0; stepIndex < 16; ++stepIndex)
        {
            float progress = (stepIndex + 1.0) / 16.0;
            rayUv += direction * (SsrMaxDistance / 16.0);
            if (rayUv.x <= 0.001 || rayUv.y <= 0.001 || rayUv.x >= 0.999 || rayUv.y >= 0.999)
                break;
            float sceneDepth = DepthTexture.SampleLevel(LinearSampler, rayUv, 0).r;
            if (DepthInvert > 0.5) sceneDepth = 1.0 - sceneDepth;
            float expectedDepth = centerDepth + reflected.z * progress * SsrMaxDistance;
            if (stepIndex > 1 && abs(sceneDepth - expectedDepth) < SsrThickness)
            {
                reflectionColor = ColorTexture.SampleLevel(LinearSampler, rayUv, 0).rgb;
                hit = 1.0 - progress * 0.35;
                break;
            }
        }
        float fresnel = pow(1.0 - saturate(abs(dot(normal, -viewRay))), 3.0);
        color = lerp(color, reflectionColor, hit * SsrStrength * (0.25 + 0.75 * fresnel));
    }
    if (VignetteEnabled > 0.5)
    {
        float edge = smoothstep(0.25, 0.72, distance(input.uv, float2(0.5, 0.5)));
        color *= 1.0 - edge * VignetteStrength;
    }
    if (SepiaEnabled > 0.5)
    {
        float3 sepia = float3(dot(color, float3(0.393, 0.769, 0.189)),
                              dot(color, float3(0.349, 0.686, 0.168)),
                              dot(color, float3(0.272, 0.534, 0.131)));
        color = lerp(color, sepia, SepiaStrength);
    }
    if (PosterizeEnabled > 0.5)
        color = floor(saturate(color) * PosterizeLevels + 0.5) / max(PosterizeLevels, 2.0);
    if (GrainEnabled > 0.5)
    {
        float noise = frac(sin(dot(input.uv * float2(1234.5, 6789.1) + Time, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
        color += noise * GrainStrength;
    }
    return float4(saturate(color), 1.0);
}
)";

void Log(const char* format, ...)
{
    if (g_log == nullptr)
        return;

    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_log, format, arguments);
    va_end(arguments);
    fputc('\n', g_log);
    fflush(g_log);
}

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    default: return "OTHER";
    }
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT DepthCopyFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

template<typename Candidate>
bool CanAccessDepth(const Candidate& candidate)
{
    return candidate.sampleCount == 1 && candidate.arraySize == 1 && candidate.mipLevels == 1 &&
        DepthCopyFormat(candidate.format) != DXGI_FORMAT_UNKNOWN;
}

template<typename Candidate>
const char* DepthAccessName(const Candidate& candidate)
{
    if (!CanAccessDepth(candidate))
        return "no";
    if ((candidate.bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        DepthSrvFormat(candidate.format) != DXGI_FORMAT_UNKNOWN)
        return "direct";
    return "copy";
}

template<typename Candidate>
bool MatchesDepthSelection(const Candidate& candidate, const DepthSelectionKey& key)
{
    return key.valid && candidate.width == key.width && candidate.height == key.height &&
        candidate.format == key.format && candidate.sampleCount == key.sampleCount &&
        candidate.arraySize == key.arraySize && candidate.mipLevels == key.mipLevels;
}

template<typename Candidate>
DepthSelectionKey MakeDepthSelection(const Candidate& candidate)
{
    return { candidate.width, candidate.height, candidate.format, candidate.sampleCount,
        candidate.arraySize, candidate.mipLevels, true };
}

bool SameDepthSelection(const DepthSelectionKey& left, const DepthSelectionKey& right)
{
    return left.valid == right.valid && (!left.valid ||
        (left.width == right.width && left.height == right.height && left.format == right.format &&
         left.sampleCount == right.sampleCount && left.arraySize == right.arraySize &&
         left.mipLevels == right.mipLevels));
}

float ReadPresetFloat(const std::filesystem::path& path, const wchar_t* section,
    const wchar_t* key, float fallback);

void LoadSettings()
{
    const std::wstring path = (g_moduleDirectory / L"Config" / L"Luminex.ini").wstring();
    const int configuredHookLevel = static_cast<int>(
        GetPrivateProfileIntW(L"Diagnostics", L"HookLevel", 1, path.c_str()));
    g_hookLevel = std::clamp(configuredHookLevel, 0, 5);
    const int configuredColorStage = static_cast<int>(
        GetPrivateProfileIntW(L"Diagnostics", L"ColorStage", 1, path.c_str()));
    g_colorStage = std::clamp(configuredColorStage, 1, 2);
    g_effectEnabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, path.c_str()) != 0;
    g_collectDepth.store(
        g_hookLevel >= 5 && GetPrivateProfileIntW(L"Depth", L"Enabled", 1, path.c_str()) != 0,
        std::memory_order_relaxed);
    wchar_t value[64]{};
    g_depthPreview = GetPrivateProfileIntW(L"Depth", L"Preview", 0, path.c_str()) != 0;
    g_depthInvert = GetPrivateProfileIntW(L"Depth", L"Invert", 0, path.c_str()) != 0;
    GetPrivateProfileStringW(L"Depth", L"PreviewPower", L"1.0", value, 64, path.c_str());
    g_depthPower = std::clamp(std::wcstof(value, nullptr), 0.05f, 32.0f);
    g_depthFogEnabled = GetPrivateProfileIntW(L"DepthFog", L"Enabled", 0, path.c_str()) != 0;
    GetPrivateProfileStringW(L"DepthFog", L"Start", L"0.05", value, 64, path.c_str());
    g_fogStart = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"DepthFog", L"End", L"0.60", value, 64, path.c_str());
    g_fogEnd = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"DepthFog", L"Strength", L"0.35", value, 64, path.c_str());
    g_fogStrength = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"DepthFog", L"ColorR", L"0.65", value, 64, path.c_str());
    g_fogColor[0] = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"DepthFog", L"ColorG", L"0.72", value, 64, path.c_str());
    g_fogColor[1] = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"DepthFog", L"ColorB", L"0.82", value, 64, path.c_str());
    g_fogColor[2] = std::clamp(std::wcstof(value, nullptr), 0.0f, 1.0f);
    g_sharpenEnabled = GetPrivateProfileIntW(L"Sharpen", L"Enabled", 0, path.c_str()) != 0;
    g_sharpenStrength = std::clamp(ReadPresetFloat(path, L"Sharpen", L"Strength", 0.25f), 0.0f, 1.5f);
    g_vignetteEnabled = GetPrivateProfileIntW(L"Vignette", L"Enabled", 0, path.c_str()) != 0;
    g_vignetteStrength = std::clamp(ReadPresetFloat(path, L"Vignette", L"Strength", 0.25f), 0.0f, 1.0f);
    g_gamma = std::clamp(ReadPresetFloat(path, L"Color", L"Gamma", 1.0f), 0.25f, 3.0f);
    g_vibrance = std::clamp(ReadPresetFloat(path, L"Color", L"Vibrance", 0.0f), -1.0f, 1.0f);
    g_temperature = std::clamp(ReadPresetFloat(path, L"Color", L"Temperature", 0.0f), -1.0f, 1.0f);
    g_tint = std::clamp(ReadPresetFloat(path, L"Color", L"Tint", 0.0f), -1.0f, 1.0f);
    g_tonemapMode = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneMap", 0, path.c_str())), 0, 2);
    g_bloomEnabled = GetPrivateProfileIntW(L"Bloom", L"Enabled", 0, path.c_str()) != 0;
    g_bloomThreshold = std::clamp(ReadPresetFloat(path, L"Bloom", L"Threshold", 0.75f), 0.0f, 2.0f);
    g_bloomStrength = std::clamp(ReadPresetFloat(path, L"Bloom", L"Strength", 0.20f), 0.0f, 2.0f);
    g_bloomRadius = std::clamp(ReadPresetFloat(path, L"Bloom", L"Radius", 2.0f), 0.5f, 12.0f);
    g_chromaticEnabled = GetPrivateProfileIntW(L"ChromaticAberration", L"Enabled", 0, path.c_str()) != 0;
    g_chromaticStrength = std::clamp(ReadPresetFloat(path, L"ChromaticAberration", L"Strength", 1.5f), 0.0f, 10.0f);
    g_grainEnabled = GetPrivateProfileIntW(L"FilmGrain", L"Enabled", 0, path.c_str()) != 0;
    g_grainStrength = std::clamp(ReadPresetFloat(path, L"FilmGrain", L"Strength", 0.035f), 0.0f, 0.25f);
    g_sepiaEnabled = GetPrivateProfileIntW(L"Sepia", L"Enabled", 0, path.c_str()) != 0;
    g_sepiaStrength = std::clamp(ReadPresetFloat(path, L"Sepia", L"Strength", 0.45f), 0.0f, 1.0f);
    g_posterizeEnabled = GetPrivateProfileIntW(L"Posterize", L"Enabled", 0, path.c_str()) != 0;
    g_posterizeLevels = std::clamp(ReadPresetFloat(path, L"Posterize", L"Levels", 8.0f), 2.0f, 32.0f);
    g_dofEnabled = GetPrivateProfileIntW(L"DepthOfField", L"Enabled", 0, path.c_str()) != 0;
    g_dofFocus = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Focus", 0.20f), 0.0f, 1.0f);
    g_dofRange = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Range", 0.08f), 0.001f, 1.0f);
    g_dofStrength = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Strength", 4.0f), 0.0f, 16.0f);
    g_depthOutlineEnabled = GetPrivateProfileIntW(L"DepthOutline", L"Enabled", 0, path.c_str()) != 0;
    g_depthOutlineThreshold = std::clamp(ReadPresetFloat(path, L"DepthOutline", L"Threshold", 0.012f), 0.0001f, 0.2f);
    g_depthOutlineStrength = std::clamp(ReadPresetFloat(path, L"DepthOutline", L"Strength", 0.65f), 0.0f, 1.0f);
    g_ssaoEnabled = GetPrivateProfileIntW(L"SSAO", L"Enabled", 0, path.c_str()) != 0;
    g_ssaoRadius = std::clamp(ReadPresetFloat(path, L"SSAO", L"Radius", 3.0f), 0.5f, 16.0f);
    g_ssaoStrength = std::clamp(ReadPresetFloat(path, L"SSAO", L"Strength", 0.35f), 0.0f, 1.0f);
    // Disabled until reflection techniques are provided by the ReShade FX runtime.
    g_ssrEnabled = false;
    g_ssrStrength = std::clamp(ReadPresetFloat(path, L"SSR", L"Strength", 0.65f), 0.0f, 1.5f);
    g_ssrMaxDistance = std::clamp(ReadPresetFloat(path, L"SSR", L"MaxDistance", 0.18f), 0.02f, 0.5f);
    g_ssrThickness = std::clamp(ReadPresetFloat(path, L"SSR", L"Thickness", 0.025f), 0.001f, 0.15f);
    GetPrivateProfileStringW(L"Color", L"Exposure", L"0.0", value, 64, path.c_str());
    g_exposure = std::wcstof(value, nullptr);
    GetPrivateProfileStringW(L"Color", L"Contrast", L"1.05", value, 64, path.c_str());
    g_contrast = std::wcstof(value, nullptr);
    GetPrivateProfileStringW(L"Color", L"Saturation", L"1.08", value, 64, path.c_str());
    g_saturation = std::wcstof(value, nullptr);
}

void SaveSettings()
{
    const std::wstring path = (g_moduleDirectory / L"Config" / L"Luminex.ini").wstring();
    WritePrivateProfileStringW(L"General", L"Enabled", g_effectEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Depth", L"Enabled",
        g_collectDepth.load(std::memory_order_relaxed) ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Depth", L"Preview", g_depthPreview ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Depth", L"Invert", g_depthInvert ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"DepthFog", L"Enabled", g_depthFogEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Sharpen", L"Enabled", g_sharpenEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Vignette", L"Enabled", g_vignetteEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Bloom", L"Enabled", g_bloomEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ChromaticAberration", L"Enabled", g_chromaticEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FilmGrain", L"Enabled", g_grainEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Sepia", L"Enabled", g_sepiaEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Posterize", L"Enabled", g_posterizeEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"DepthOfField", L"Enabled", g_dofEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"DepthOutline", L"Enabled", g_depthOutlineEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"SSAO", L"Enabled", g_ssaoEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"SSR", L"Enabled", g_ssrEnabled ? L"1" : L"0", path.c_str());
    wchar_t value[64]{};
    swprintf_s(value, L"%.4f", g_exposure);
    WritePrivateProfileStringW(L"Color", L"Exposure", value, path.c_str());
    swprintf_s(value, L"%.4f", g_contrast);
    WritePrivateProfileStringW(L"Color", L"Contrast", value, path.c_str());
    swprintf_s(value, L"%.4f", g_saturation);
    WritePrivateProfileStringW(L"Color", L"Saturation", value, path.c_str());
    swprintf_s(value, L"%.4f", g_depthPower);
    WritePrivateProfileStringW(L"Depth", L"PreviewPower", value, path.c_str());
    swprintf_s(value, L"%.4f", g_fogStart);
    WritePrivateProfileStringW(L"DepthFog", L"Start", value, path.c_str());
    swprintf_s(value, L"%.4f", g_fogEnd);
    WritePrivateProfileStringW(L"DepthFog", L"End", value, path.c_str());
    swprintf_s(value, L"%.4f", g_fogStrength);
    WritePrivateProfileStringW(L"DepthFog", L"Strength", value, path.c_str());
    for (int index = 0; index < 3; ++index)
    {
        swprintf_s(value, L"%.4f", g_fogColor[index]);
        constexpr const wchar_t* keys[] = { L"ColorR", L"ColorG", L"ColorB" };
        WritePrivateProfileStringW(L"DepthFog", keys[index], value, path.c_str());
    }
    swprintf_s(value, L"%.4f", g_sharpenStrength);
    WritePrivateProfileStringW(L"Sharpen", L"Strength", value, path.c_str());
    swprintf_s(value, L"%.4f", g_vignetteStrength);
    WritePrivateProfileStringW(L"Vignette", L"Strength", value, path.c_str());
#define SAVE_FLOAT(section, key, field) swprintf_s(value, L"%.4f", field); WritePrivateProfileStringW(section, key, value, path.c_str())
    SAVE_FLOAT(L"Color", L"Gamma", g_gamma);
    SAVE_FLOAT(L"Color", L"Vibrance", g_vibrance);
    SAVE_FLOAT(L"Color", L"Temperature", g_temperature);
    SAVE_FLOAT(L"Color", L"Tint", g_tint);
    swprintf_s(value, L"%d", g_tonemapMode);
    WritePrivateProfileStringW(L"Color", L"ToneMap", value, path.c_str());
    SAVE_FLOAT(L"Bloom", L"Threshold", g_bloomThreshold);
    SAVE_FLOAT(L"Bloom", L"Strength", g_bloomStrength);
    SAVE_FLOAT(L"Bloom", L"Radius", g_bloomRadius);
    SAVE_FLOAT(L"ChromaticAberration", L"Strength", g_chromaticStrength);
    SAVE_FLOAT(L"FilmGrain", L"Strength", g_grainStrength);
    SAVE_FLOAT(L"Sepia", L"Strength", g_sepiaStrength);
    SAVE_FLOAT(L"Posterize", L"Levels", g_posterizeLevels);
    SAVE_FLOAT(L"DepthOfField", L"Focus", g_dofFocus);
    SAVE_FLOAT(L"DepthOfField", L"Range", g_dofRange);
    SAVE_FLOAT(L"DepthOfField", L"Strength", g_dofStrength);
    SAVE_FLOAT(L"DepthOutline", L"Threshold", g_depthOutlineThreshold);
    SAVE_FLOAT(L"DepthOutline", L"Strength", g_depthOutlineStrength);
    SAVE_FLOAT(L"SSAO", L"Radius", g_ssaoRadius);
    SAVE_FLOAT(L"SSAO", L"Strength", g_ssaoStrength);
    SAVE_FLOAT(L"SSR", L"Strength", g_ssrStrength);
    SAVE_FLOAT(L"SSR", L"MaxDistance", g_ssrMaxDistance);
    SAVE_FLOAT(L"SSR", L"Thickness", g_ssrThickness);
#undef SAVE_FLOAT
}

float ReadPresetFloat(const std::filesystem::path& path, const wchar_t* section,
    const wchar_t* key, float fallback)
{
    wchar_t fallbackText[64]{};
    wchar_t value[64]{};
    swprintf_s(fallbackText, L"%.6f", fallback);
    GetPrivateProfileStringW(section, key, fallbackText, value, 64, path.c_str());
    return std::wcstof(value, nullptr);
}

void DiscoverPresets()
{
    g_presetPaths.clear();
    const auto directory = g_moduleDirectory / L"Presets";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".ini") == 0)
            g_presetPaths.push_back(entry.path());
    }
    std::sort(g_presetPaths.begin(), g_presetPaths.end());
    Log("Discovered %zu shader packs", g_presetPaths.size());
}

void DiscoverReShadeEffects(UINT width, UINT height)
{
    g_fxTechniques.clear();
    g_fxMessages.clear();
    const auto shaderDirectory = g_moduleDirectory / L"ReShade" / L"Shaders";
    std::error_code directoryError;
    if (!std::filesystem::is_directory(shaderDirectory, directoryError))
    {
        g_fxMessages.emplace_back("ReShade shader directory is not installed.");
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderDirectory, directoryError))
    {
        if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".fx") != 0)
            continue;

        reshadefx::preprocessor preprocessor;
        preprocessor.add_include_path(shaderDirectory);
        preprocessor.add_include_path(entry.path().parent_path());
        preprocessor.add_macro_definition("__RESHADE__", "60800");
        preprocessor.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
        preprocessor.add_macro_definition("__RENDERER__", "0xb100");
        preprocessor.add_macro_definition("__VENDOR__", "0");
        preprocessor.add_macro_definition("__DEVICE__", "0");
        preprocessor.add_macro_definition("BUFFER_WIDTH", std::to_string(width));
        preprocessor.add_macro_definition("BUFFER_HEIGHT", std::to_string(height));
        preprocessor.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
        preprocessor.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
        preprocessor.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
        preprocessor.add_macro_definition("BUFFER_COLOR_SPACE", "0");
        if (!preprocessor.append_file(entry.path()))
        {
            g_fxMessages.push_back(entry.path().filename().string() + ": preprocessing failed");
            Log("ReShade FX preprocessing failed for %s: %s", entry.path().filename().string().c_str(),
                preprocessor.errors().c_str());
            continue;
        }

        std::unique_ptr<reshadefx::codegen> backend(
            reshadefx::create_codegen_dxbc(50, false, false, 3));
        reshadefx::parser parser;
        if (!parser.parse(preprocessor.output(), backend.get()))
        {
            g_fxMessages.push_back(entry.path().filename().string() + ": compilation failed");
            Log("ReShade FX parsing failed for %s: %s", entry.path().filename().string().c_str(),
                parser.errors().c_str());
            continue;
        }

        const auto& module = backend->module();
        for (const auto& technique : module.techniques)
        {
            if (technique.passes.size() != 1)
                continue;
            const auto& pass = technique.passes.front();
            if (pass.vs_entry_point.empty() || pass.ps_entry_point.empty() || !pass.cs_entry_point.empty())
                continue;
            bool hasCustomTarget = false;
            for (const auto& target : pass.render_target_names)
                hasCustomTarget = hasCustomTarget || !target.empty();
            if (hasCustomTarget)
                continue;

            FxTechniqueRuntime runtimeTechnique;
            runtimeTechnique.name = technique.name;
            runtimeTechnique.source = entry.path().filename().string();
            bool supported = true;
            for (const auto& binding : pass.texture_bindings)
            {
                if (binding.index >= module.samplers.size()) { supported = false; break; }
                const auto& sampler = module.samplers[binding.index];
                const auto texture = std::find_if(module.textures.begin(), module.textures.end(),
                    [&sampler](const reshadefx::texture& item) {
                        return item.name == sampler.texture_name || item.unique_name == sampler.texture_name;
                    });
                if (texture == module.textures.end()) { supported = false; break; }
                if (_stricmp(texture->semantic.c_str(), "COLOR") == 0)
                    runtimeTechnique.textures.push_back({ binding.entry_point_binding, false });
                else if (_stricmp(texture->semantic.c_str(), "DEPTH") == 0)
                    runtimeTechnique.textures.push_back({ binding.entry_point_binding, true });
                else
                    supported = false;
            }
            for (const auto& binding : pass.sampler_bindings)
                runtimeTechnique.samplers.push_back(binding.entry_point_binding);
            if (!supported)
                continue;

            std::string vertexBytecode, vertexAssembly, vertexErrors;
            std::string pixelBytecode, pixelAssembly, pixelErrors;
            if (!backend->assemble_code_for_entry_point(pass.vs_entry_point,
                    vertexBytecode, vertexAssembly, vertexErrors) ||
                !backend->assemble_code_for_entry_point(pass.ps_entry_point,
                    pixelBytecode, pixelAssembly, pixelErrors) ||
                FAILED(g_device->CreateVertexShader(vertexBytecode.data(), vertexBytecode.size(), nullptr,
                    &runtimeTechnique.vertexShader)) ||
                FAILED(g_device->CreatePixelShader(pixelBytecode.data(), pixelBytecode.size(), nullptr,
                    &runtimeTechnique.pixelShader)))
            {
                g_fxMessages.push_back(runtimeTechnique.name + ": bytecode creation failed");
                Log("ReShade FX bytecode failed for %s/%s: %s%s", runtimeTechnique.source.c_str(),
                    runtimeTechnique.name.c_str(), vertexErrors.c_str(), pixelErrors.c_str());
                continue;
            }

            if (module.total_uniform_size != 0)
            {
                const UINT uniformSize = (module.total_uniform_size + 15u) & ~15u;
                std::vector<unsigned char> defaults(uniformSize, 0);
                for (const auto& uniform : module.uniforms)
                {
                    if (!uniform.has_initializer_value || uniform.type.is_array() ||
                        uniform.type.is_matrix() || uniform.offset >= defaults.size())
                        continue;
                    const size_t bytes = std::min<size_t>(uniform.size,
                        uniform.type.components() * sizeof(uint32_t));
                    if (uniform.offset + bytes <= defaults.size())
                        memcpy(defaults.data() + uniform.offset, uniform.initializer_value.as_uint, bytes);

                    if (uniform.type.is_array() || uniform.type.is_matrix() ||
                        !uniform.type.is_numeric() || uniform.type.components() > 4)
                        continue;
                    const auto annotation = [&uniform](const char* name) {
                        return std::find_if(uniform.annotations.begin(), uniform.annotations.end(),
                            [name](const reshadefx::annotation& item) { return item.name == name; });
                    };
                    if (annotation("source") != uniform.annotations.end())
                        continue;
                    FxUniformControl control;
                    control.name = uniform.name;
                    control.label = uniform.name;
                    control.offset = uniform.offset;
                    control.components = uniform.type.components();
                    if (uniform.type.base == reshadefx::type::t_bool)
                        control.kind = FxUniformKind::Boolean;
                    else if (uniform.type.is_integral())
                        control.kind = FxUniformKind::Integer;
                    const auto label = annotation("ui_label");
                    if (label != uniform.annotations.end() && !label->value.string_data.empty())
                        control.label = label->value.string_data;
                    const auto uiType = annotation("ui_type");
                    if (uiType != uniform.annotations.end())
                        control.uiType = uiType->value.string_data;
                    const auto minimum = annotation("ui_min");
                    const auto maximum = annotation("ui_max");
                    if (minimum != uniform.annotations.end())
                        control.minimum = control.kind == FxUniformKind::FloatingPoint ?
                            minimum->value.as_float[0] : static_cast<float>(minimum->value.as_int[0]);
                    if (maximum != uniform.annotations.end())
                        control.maximum = control.kind == FxUniformKind::FloatingPoint ?
                            maximum->value.as_float[0] : static_cast<float>(maximum->value.as_int[0]);
                    runtimeTechnique.uniforms.push_back(std::move(control));
                }
                runtimeTechnique.uniformData = defaults;
                D3D11_BUFFER_DESC bufferDesc{};
                bufferDesc.ByteWidth = uniformSize;
                bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
                bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                D3D11_SUBRESOURCE_DATA initialData{ defaults.data() };
                if (FAILED(g_device->CreateBuffer(&bufferDesc, &initialData, &runtimeTechnique.uniformBuffer)))
                    continue;
            }
            g_fxTechniques.push_back(std::move(runtimeTechnique));
        }
    }
    Log("ReShade FX runtime discovered %zu directly executable techniques", g_fxTechniques.size());
}

void ApplyPreset(const std::filesystem::path& path)
{
    g_effectEnabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, path.c_str()) != 0;
    g_exposure = std::clamp(ReadPresetFloat(path, L"Color", L"Exposure", g_exposure), -2.0f, 2.0f);
    g_contrast = std::clamp(ReadPresetFloat(path, L"Color", L"Contrast", g_contrast), 0.0f, 2.0f);
    g_saturation = std::clamp(ReadPresetFloat(path, L"Color", L"Saturation", g_saturation), 0.0f, 2.0f);
    g_depthInvert = GetPrivateProfileIntW(L"Depth", L"Invert", g_depthInvert ? 1 : 0, path.c_str()) != 0;
    g_depthFogEnabled = GetPrivateProfileIntW(L"DepthFog", L"Enabled", 0, path.c_str()) != 0;
    g_fogStart = std::clamp(ReadPresetFloat(path, L"DepthFog", L"Start", g_fogStart), 0.0f, 1.0f);
    g_fogEnd = std::clamp(ReadPresetFloat(path, L"DepthFog", L"End", g_fogEnd), 0.0f, 1.0f);
    g_fogStrength = std::clamp(ReadPresetFloat(path, L"DepthFog", L"Strength", g_fogStrength), 0.0f, 1.0f);
    g_fogColor[0] = std::clamp(ReadPresetFloat(path, L"DepthFog", L"ColorR", g_fogColor[0]), 0.0f, 1.0f);
    g_fogColor[1] = std::clamp(ReadPresetFloat(path, L"DepthFog", L"ColorG", g_fogColor[1]), 0.0f, 1.0f);
    g_fogColor[2] = std::clamp(ReadPresetFloat(path, L"DepthFog", L"ColorB", g_fogColor[2]), 0.0f, 1.0f);
    g_sharpenEnabled = GetPrivateProfileIntW(L"Sharpen", L"Enabled", 0, path.c_str()) != 0;
    g_sharpenStrength = std::clamp(ReadPresetFloat(path, L"Sharpen", L"Strength", g_sharpenStrength), 0.0f, 1.5f);
    g_vignetteEnabled = GetPrivateProfileIntW(L"Vignette", L"Enabled", 0, path.c_str()) != 0;
    g_vignetteStrength = std::clamp(ReadPresetFloat(path, L"Vignette", L"Strength", g_vignetteStrength), 0.0f, 1.0f);
    g_gamma = std::clamp(ReadPresetFloat(path, L"Color", L"Gamma", g_gamma), 0.25f, 3.0f);
    g_vibrance = std::clamp(ReadPresetFloat(path, L"Color", L"Vibrance", g_vibrance), -1.0f, 1.0f);
    g_temperature = std::clamp(ReadPresetFloat(path, L"Color", L"Temperature", g_temperature), -1.0f, 1.0f);
    g_tint = std::clamp(ReadPresetFloat(path, L"Color", L"Tint", g_tint), -1.0f, 1.0f);
    g_tonemapMode = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneMap", g_tonemapMode, path.c_str())), 0, 2);
#define LOAD_EFFECT(section, variable) variable = GetPrivateProfileIntW(section, L"Enabled", 0, path.c_str()) != 0
    LOAD_EFFECT(L"Bloom", g_bloomEnabled);
    LOAD_EFFECT(L"ChromaticAberration", g_chromaticEnabled);
    LOAD_EFFECT(L"FilmGrain", g_grainEnabled);
    LOAD_EFFECT(L"Sepia", g_sepiaEnabled);
    LOAD_EFFECT(L"Posterize", g_posterizeEnabled);
    LOAD_EFFECT(L"DepthOfField", g_dofEnabled);
    LOAD_EFFECT(L"DepthOutline", g_depthOutlineEnabled);
    LOAD_EFFECT(L"SSAO", g_ssaoEnabled);
    g_ssrEnabled = false;
#undef LOAD_EFFECT
    g_bloomThreshold = std::clamp(ReadPresetFloat(path, L"Bloom", L"Threshold", g_bloomThreshold), 0.0f, 2.0f);
    g_bloomStrength = std::clamp(ReadPresetFloat(path, L"Bloom", L"Strength", g_bloomStrength), 0.0f, 2.0f);
    g_bloomRadius = std::clamp(ReadPresetFloat(path, L"Bloom", L"Radius", g_bloomRadius), 0.5f, 12.0f);
    g_chromaticStrength = std::clamp(ReadPresetFloat(path, L"ChromaticAberration", L"Strength", g_chromaticStrength), 0.0f, 10.0f);
    g_grainStrength = std::clamp(ReadPresetFloat(path, L"FilmGrain", L"Strength", g_grainStrength), 0.0f, 0.25f);
    g_sepiaStrength = std::clamp(ReadPresetFloat(path, L"Sepia", L"Strength", g_sepiaStrength), 0.0f, 1.0f);
    g_posterizeLevels = std::clamp(ReadPresetFloat(path, L"Posterize", L"Levels", g_posterizeLevels), 2.0f, 32.0f);
    g_dofFocus = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Focus", g_dofFocus), 0.0f, 1.0f);
    g_dofRange = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Range", g_dofRange), 0.001f, 1.0f);
    g_dofStrength = std::clamp(ReadPresetFloat(path, L"DepthOfField", L"Strength", g_dofStrength), 0.0f, 16.0f);
    g_depthOutlineThreshold = std::clamp(ReadPresetFloat(path, L"DepthOutline", L"Threshold", g_depthOutlineThreshold), 0.0001f, 0.2f);
    g_depthOutlineStrength = std::clamp(ReadPresetFloat(path, L"DepthOutline", L"Strength", g_depthOutlineStrength), 0.0f, 1.0f);
    g_ssaoRadius = std::clamp(ReadPresetFloat(path, L"SSAO", L"Radius", g_ssaoRadius), 0.5f, 16.0f);
    g_ssaoStrength = std::clamp(ReadPresetFloat(path, L"SSAO", L"Strength", g_ssaoStrength), 0.0f, 1.0f);
    g_ssrStrength = std::clamp(ReadPresetFloat(path, L"SSR", L"Strength", g_ssrStrength), 0.0f, 1.5f);
    g_ssrMaxDistance = std::clamp(ReadPresetFloat(path, L"SSR", L"MaxDistance", g_ssrMaxDistance), 0.02f, 0.5f);
    g_ssrThickness = std::clamp(ReadPresetFloat(path, L"SSR", L"Thickness", g_ssrThickness), 0.001f, 0.15f);
    g_depthPreview = false;
    g_activePreset = path.stem().string();
    Log("Shader pack applied: %s", g_activePreset.c_str());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_reshadeRuntime != nullptr && g_reshadeHandleMessage != nullptr)
    {
        const bool overlayWasOpen = g_reshadeIsOverlayOpen != nullptr &&
            g_reshadeIsOverlayOpen(g_reshadeRuntime);
        MSG details{};
        details.hwnd = window;
        details.message = message;
        details.wParam = wParam;
        details.lParam = lParam;
        details.time = GetMessageTime();
        const DWORD position = GetMessagePos();
        details.pt = { static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position)) };
        const bool handled = g_reshadeHandleMessage(&details);
        const bool overlayIsOpen = g_reshadeIsOverlayOpen != nullptr &&
            g_reshadeIsOverlayOpen(g_reshadeRuntime);
        if (!overlayWasOpen && overlayIsOpen && g_originalClipCursor != nullptr)
            g_originalClipCursor(nullptr);
        const bool inputMessage =
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            message == WM_INPUT || message == WM_CHAR || message == WM_SYSCHAR ||
            message == WM_UNICHAR;
        if (handled || (inputMessage && (overlayWasOpen || overlayIsOpen)))
        {
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
                g_blockedWindowWheel.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    else if (message == WM_KEYUP && wParam == VK_HOME)
    {
        g_menuOpen = !g_menuOpen;
        return 0;
    }

    if (g_menuOpen && g_imguiReady)
    {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        const bool mouseMessage =
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_NCMOUSEMOVE && message <= WM_NCXBUTTONDBLCLK);
        const bool keyboardMessage =
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            message == WM_CHAR || message == WM_SYSCHAR || message == WM_UNICHAR;
        if ((mouseMessage && io.WantCaptureMouse) || (keyboardMessage && io.WantCaptureKeyboard))
            return 1;
    }

    return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
}

bool CompileShader(const char* entry, const char* profile, ComPtr<ID3DBlob>& output)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        ShaderSource, strlen(ShaderSource), "LuminexEmbedded.hlsl", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output, &errors);
    if (FAILED(result))
    {
        Log("Shader compilation failed for %s: %s", entry,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }
    return true;
}

void ReleaseFrameResources()
{
    // A stored D3D11 context state can retain the old RTV. Release it before
    // ResizeBuffers so Luminex never keeps a swap-chain back buffer alive.
    g_luminexContextState.Reset();
    g_depthCopy = {};
    {
        std::scoped_lock lock(g_depthMutex);
        g_depthCandidates.clear();
    }
    g_backBufferRtv.Reset();
    g_bloomRtvA.Reset();
    g_bloomRtvB.Reset();
    g_bloomSrvA.Reset();
    g_bloomSrvB.Reset();
    g_bloomTextureA.Reset();
    g_bloomTextureB.Reset();
    g_colorSrv.Reset();
    g_colorCopy.Reset();
    g_resourcesReady = false;
    g_loggedColorCopy = false;
    g_loggedColorDraw = false;
}

void ShutdownReShadeRuntime()
{
    g_reshadeDepthSrv.Reset();
    if (g_reshadeRuntime != nullptr && g_reshadeDestroyRuntime != nullptr)
        g_reshadeDestroyRuntime(g_reshadeRuntime);
    g_reshadeRuntime = nullptr;
    g_reshadeDestroyRuntime = nullptr;
    g_reshadePresentRuntime = nullptr;
    g_reshadeHandleMessage = nullptr;
    g_reshadeIsOverlayOpen = nullptr;
    g_reshadeBeginResize = nullptr;
    g_reshadeEndResize = nullptr;
    if (g_reshadeModule != nullptr)
        FreeLibrary(g_reshadeModule);
    g_reshadeModule = nullptr;
}

bool InitializeReShadeRuntime(IDXGISwapChain* swapChain)
{
    if (g_reshadeRuntime != nullptr)
        return true;
    if (g_reshadeAttempted)
        return false;
    g_reshadeAttempted = true;

    const auto dllPath = g_moduleDirectory / L"LuminexReShadeRuntime.dll";
    if (!std::filesystem::is_regular_file(dllPath))
    {
        Log("Full ReShade runtime is not installed; using Luminex fallback renderer");
        return false;
    }

    g_reshadeModule = LoadLibraryW(dllPath.c_str());
    if (g_reshadeModule == nullptr)
    {
        Log("Full ReShade runtime load failed: error=%lu", GetLastError());
        return false;
    }

    const auto initialize = reinterpret_cast<ReShadeInitializeFunction>(
        GetProcAddress(g_reshadeModule, "LuminexInitializeReShadeRuntime"));
    const auto createRuntime = reinterpret_cast<ReShadeCreateRuntimeFunction>(
        GetProcAddress(g_reshadeModule, "ReShadeCreateEffectRuntime"));
    g_reshadeDestroyRuntime = reinterpret_cast<ReShadeDestroyRuntimeFunction>(
        GetProcAddress(g_reshadeModule, "ReShadeDestroyEffectRuntime"));
    g_reshadePresentRuntime = reinterpret_cast<ReShadePresentRuntimeFunction>(
        GetProcAddress(g_reshadeModule, "ReShadeUpdateAndPresentEffectRuntime"));
    g_reshadeHandleMessage = reinterpret_cast<ReShadeHandleMessageFunction>(
        GetProcAddress(g_reshadeModule, "LuminexReShadeHandleWindowMessage"));
    g_reshadeIsOverlayOpen = reinterpret_cast<ReShadeIsOverlayOpenFunction>(
        GetProcAddress(g_reshadeModule, "LuminexReShadeIsOverlayOpen"));
    g_reshadeBeginResize = reinterpret_cast<ReShadeBeginResizeFunction>(
        GetProcAddress(g_reshadeModule, "LuminexReShadeBeginResize"));
    g_reshadeEndResize = reinterpret_cast<ReShadeEndResizeFunction>(
        GetProcAddress(g_reshadeModule, "LuminexReShadeEndResize"));
    if (initialize == nullptr || createRuntime == nullptr ||
        g_reshadeDestroyRuntime == nullptr || g_reshadePresentRuntime == nullptr ||
        g_reshadeHandleMessage == nullptr || g_reshadeIsOverlayOpen == nullptr ||
        g_reshadeBeginResize == nullptr || g_reshadeEndResize == nullptr ||
        !initialize(g_moduleDirectory.c_str()))
    {
        Log("Full ReShade runtime exports or initialization are unavailable");
        ShutdownReShadeRuntime();
        return false;
    }

    const std::string configPath = (g_moduleDirectory / L"ReShade.ini").string();
    if (!createRuntime(reshade::api::device_api::d3d11, g_device.Get(), g_context.Get(),
        swapChain, configPath.c_str(), &g_reshadeRuntime))
    {
        Log("Full ReShade effect runtime creation failed");
        ShutdownReShadeRuntime();
        return false;
    }

    Log("Full ReShade effect runtime initialized");
    return true;
}

bool IsReShadeOverlayOpen()
{
    return g_reshadeRuntime != nullptr && g_reshadeIsOverlayOpen != nullptr &&
        g_reshadeIsOverlayOpen(g_reshadeRuntime);
}

bool IsAddressInsideModule(const void* address, HMODULE module)
{
    if (address == nullptr || module == nullptr)
        return false;
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const auto* caller = reinterpret_cast<const std::byte*>(address);
    return caller >= base && caller < base + nt->OptionalHeader.SizeOfImage;
}

enum class NeutralizedRawInput
{
    None,
    Mouse,
    Keyboard,
};

NeutralizedRawInput NeutralizeRawInput(RAWINPUT& input)
{
    if (input.header.dwType == RIM_TYPEMOUSE)
    {
        input.header.hDevice = nullptr;
        input.header.wParam = RIM_INPUTSINK;
        ZeroMemory(&input.data.mouse, sizeof(input.data.mouse));
        return NeutralizedRawInput::Mouse;
    }
    if (input.header.dwType == RIM_TYPEKEYBOARD)
    {
        input.header.hDevice = nullptr;
        input.header.wParam = RIM_INPUTSINK;
        ZeroMemory(&input.data.keyboard, sizeof(input.data.keyboard));
        input.data.keyboard.Flags = RI_KEY_BREAK;
        input.data.keyboard.Message = WM_KEYUP;
        return NeutralizedRawInput::Keyboard;
    }
    return NeutralizedRawInput::None;
}

UINT WINAPI HookGetRawInputData(
    HRAWINPUT inputHandle, UINT command, LPVOID data, PUINT size, UINT headerSize)
{
    const bool reshadeRead = IsAddressInsideModule(_ReturnAddress(), g_reshadeModule);
    const UINT result = g_originalGetRawInputData(inputHandle, command, data, size, headerSize);
    if (!reshadeRead && IsReShadeOverlayOpen() && command == RID_INPUT &&
        result != static_cast<UINT>(-1) && data != nullptr && result >= sizeof(RAWINPUTHEADER))
    {
        auto& input = *static_cast<RAWINPUT*>(data);
        const auto neutralized = NeutralizeRawInput(input);
        if (neutralized == NeutralizedRawInput::Mouse)
            g_blockedRawInputDataMouse.fetch_add(1, std::memory_order_relaxed);
        else if (neutralized == NeutralizedRawInput::Keyboard)
            g_blockedRawInputDataKeyboard.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
{
    using QWORD = UINT64;
    const bool reshadeRead = IsAddressInsideModule(_ReturnAddress(), g_reshadeModule);
    const UINT result = g_originalGetRawInputBuffer(data, size, headerSize);
    if (!reshadeRead && IsReShadeOverlayOpen() && result != static_cast<UINT>(-1) && data != nullptr)
    {
        PRAWINPUT current = data;
        for (UINT index = 0; index < result; ++index)
        {
            const auto neutralized = NeutralizeRawInput(*current);
            if (neutralized == NeutralizedRawInput::Mouse)
                g_blockedRawInputBufferMouse.fetch_add(1, std::memory_order_relaxed);
            else if (neutralized == NeutralizedRawInput::Keyboard)
                g_blockedRawInputBufferKeyboard.fetch_add(1, std::memory_order_relaxed);
            current = NEXTRAWINPUTBLOCK(current);
        }
    }
    return result;
}

SHORT WINAPI HookGetAsyncKeyState(int virtualKey)
{
    return IsReShadeOverlayOpen() ? 0 : g_originalGetAsyncKeyState(virtualKey);
}

SHORT WINAPI HookGetKeyState(int virtualKey)
{
    return IsReShadeOverlayOpen() ? 0 : g_originalGetKeyState(virtualKey);
}

BOOL WINAPI HookGetKeyboardState(PBYTE state)
{
    const BOOL result = g_originalGetKeyboardState(state);
    if (IsReShadeOverlayOpen() && state != nullptr)
        ZeroMemory(state, 256);
    return result;
}

BOOL WINAPI HookClipCursor(const RECT* bounds)
{
    return g_originalClipCursor(IsReShadeOverlayOpen() ? nullptr : bounds);
}

BOOL WINAPI HookSetCursorPos(int x, int y)
{
    return IsReShadeOverlayOpen() ? TRUE : g_originalSetCursorPos(x, y);
}

bool CreateIsolatedContextState()
{
    if (g_luminexContextState != nullptr)
        return true;
    if (g_device1 == nullptr || g_context1 == nullptr)
        return false;

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL chosenLevel{};
    const HRESULT result = g_device1->CreateDeviceContextState(
        0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        __uuidof(ID3D11Device), &chosenLevel, &g_luminexContextState);
    if (FAILED(result))
    {
        Log("CreateDeviceContextState failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }
    Log("D3D11 context isolation ready: featureLevel=0x%X", static_cast<unsigned>(chosenLevel));
    return true;
}

class ScopedContextIsolation
{
public:
    ScopedContextIsolation()
    {
        if (CreateIsolatedContextState())
        {
            previousRendering_ = g_renderingLuminex;
            g_renderingLuminex = true;
            g_context1->SwapDeviceContextState(g_luminexContextState.Get(), &previous_);
            active_ = true;
        }
    }

    ~ScopedContextIsolation()
    {
        if (!active_)
            return;
        ComPtr<ID3DDeviceContextState> updatedLuminexState;
        g_context1->SwapDeviceContextState(previous_.Get(), &updatedLuminexState);
        g_luminexContextState = std::move(updatedLuminexState);
        g_renderingLuminex = previousRendering_;
    }

    bool Active() const { return active_; }

private:
    ComPtr<ID3DDeviceContextState> previous_;
    bool active_ = false;
    bool previousRendering_ = false;
};

bool InitializeRuntime(IDXGISwapChain* swapChain)
{
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);
    if (FAILED(g_device.As(&g_device1)) || FAILED(g_context.As(&g_context1)) ||
        !CreateIsolatedContextState())
    {
        Log("D3D11.1 full context isolation is unavailable; rendering remains disabled");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc)))
        return false;
    g_window = swapDesc.OutputWindow;

    ComPtr<ID3DBlob> vertexBlob;
    ComPtr<ID3DBlob> pixelBlob;
    ComPtr<ID3DBlob> bloomExtractBlob;
    ComPtr<ID3DBlob> bloomBlurHBlob;
    ComPtr<ID3DBlob> bloomBlurVBlob;
    if (!CompileShader("VSMain", "vs_5_0", vertexBlob) ||
        !CompileShader("PSMain", "ps_5_0", pixelBlob) ||
        !CompileShader("PSBloomExtract", "ps_5_0", bloomExtractBlob) ||
        !CompileShader("PSBloomBlurH", "ps_5_0", bloomBlurHBlob) ||
        !CompileShader("PSBloomBlurV", "ps_5_0", bloomBlurVBlob))
        return false;
    if (FAILED(g_device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &g_vertexShader)) ||
        FAILED(g_device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &g_pixelShader)) ||
        FAILED(g_device->CreatePixelShader(bloomExtractBlob->GetBufferPointer(), bloomExtractBlob->GetBufferSize(), nullptr, &g_bloomExtractShader)) ||
        FAILED(g_device->CreatePixelShader(bloomBlurHBlob->GetBufferPointer(), bloomBlurHBlob->GetBufferSize(), nullptr, &g_bloomBlurHShader)) ||
        FAILED(g_device->CreatePixelShader(bloomBlurVBlob->GetBufferPointer(), bloomBlurVBlob->GetBufferSize(), nullptr, &g_bloomBlurVShader)))
        return false;

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderSettings);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&bufferDesc, nullptr, &g_settingsBuffer)))
        return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&samplerDesc, &g_sampler)))
        return false;

    ComPtr<ID3D11Texture2D> initialBackBuffer;
    D3D11_TEXTURE2D_DESC initialDesc{};
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&initialBackBuffer))))
        initialBackBuffer->GetDesc(&initialDesc);
    const bool fullReShadeReady = InitializeReShadeRuntime(swapChain);
    if (!fullReShadeReady)
        DiscoverReShadeEffects(std::max(1u, initialDesc.Width), std::max(1u, initialDesc.Height));

    if (fullReShadeReady)
    {
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
        Log("Luminex fallback UI and effects disabled; Home is owned by ReShade");
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    static std::string iniPath = (g_moduleDirectory / "Config" / "imgui.ini").string();
    io.IniFilename = iniPath.c_str();
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device.Get(), g_context.Get()))
        return false;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
    g_imguiReady = true;
    Log("Runtime initialized for HWND=%p", g_window);
    return true;
}

bool BindPrimarySwapChain(IDXGISwapChain* swapChain)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDesc(&desc)) || desc.OutputWindow == nullptr ||
        FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &windowProcessId);
    if (windowProcessId != GetCurrentProcessId())
        return false;

    RECT client{};
    GetClientRect(desc.OutputWindow, &client);
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width < 640 || height < 360)
        return false;

    g_primarySwapChain = swapChain;
    g_window = desc.OutputWindow;
    Log("Primary swap chain selected: swap=%p device=%p hwnd=%p client=%ldx%ld",
        swapChain, device.Get(), desc.OutputWindow, width, height);
    return true;
}

bool AcceptSwapChain(IDXGISwapChain* swapChain)
{
    if (g_primarySwapChain == nullptr)
        return BindPrimarySwapChain(swapChain);
    if (swapChain == g_primarySwapChain)
        return true;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || desc.OutputWindow != g_window)
        return false;

    ComPtr<ID3D11Device> newDevice;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&newDevice))))
        return false;

    Log("Primary swap chain replaced: old=%p new=%p oldDevice=%p newDevice=%p",
        g_primarySwapChain, swapChain, g_device.Get(), newDevice.Get());
    g_primarySwapChain = swapChain;
    if (g_device != nullptr && newDevice.Get() != g_device.Get())
    {
        Log("Device replacement detected; rendering is suspended until a clean reinitialization is implemented");
        return false;
    }
    ReleaseFrameResources();
    return true;
}

bool CreateFrameResources(IDXGISwapChain* swapChain)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(result))
    {
        Log("GetBuffer failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }
    result = g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_backBufferRtv);
    if (FAILED(result))
    {
        Log("CreateRenderTargetView failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1)
    {
        Log("Unsupported multisampled swap chain: samples=%u", desc.SampleDesc.Count);
        return false;
    }
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    result = g_device->CreateTexture2D(&desc, nullptr, &g_colorCopy);
    if (FAILED(result))
    {
        Log("Color-copy texture creation failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }
    result = g_device->CreateShaderResourceView(g_colorCopy.Get(), nullptr, &g_colorSrv);
    if (FAILED(result))
    {
        Log("Color-copy SRV creation failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }

    D3D11_TEXTURE2D_DESC bloomDesc{};
    bloomDesc.Width = std::max(1u, desc.Width / 2);
    bloomDesc.Height = std::max(1u, desc.Height / 2);
    bloomDesc.MipLevels = 1;
    bloomDesc.ArraySize = 1;
    bloomDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bloomDesc.SampleDesc.Count = 1;
    bloomDesc.Usage = D3D11_USAGE_DEFAULT;
    bloomDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    result = g_device->CreateTexture2D(&bloomDesc, nullptr, &g_bloomTextureA);
    if (SUCCEEDED(result)) result = g_device->CreateTexture2D(&bloomDesc, nullptr, &g_bloomTextureB);
    if (SUCCEEDED(result)) result = g_device->CreateRenderTargetView(g_bloomTextureA.Get(), nullptr, &g_bloomRtvA);
    if (SUCCEEDED(result)) result = g_device->CreateRenderTargetView(g_bloomTextureB.Get(), nullptr, &g_bloomRtvB);
    if (SUCCEEDED(result)) result = g_device->CreateShaderResourceView(g_bloomTextureA.Get(), nullptr, &g_bloomSrvA);
    if (SUCCEEDED(result)) result = g_device->CreateShaderResourceView(g_bloomTextureB.Get(), nullptr, &g_bloomSrvB);
    if (FAILED(result))
    {
        Log("Bloom resource creation failed: hr=0x%08X", static_cast<unsigned>(result));
        return false;
    }
    g_resourcesReady = true;
    g_backBufferWidth = desc.Width;
    g_backBufferHeight = desc.Height;
    Log("Frame resources created: %ux%u format=%u", desc.Width, desc.Height, desc.Format);
    return true;
}

bool EnsureFrameResources(IDXGISwapChain* swapChain)
{
    if (g_resourcesReady && g_backBufferRtv != nullptr)
    {
        ComPtr<ID3D11Texture2D> currentBackBuffer;
        ComPtr<ID3D11Resource> resourceFromRtv;
        if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&currentBackBuffer))))
            g_backBufferRtv->GetResource(&resourceFromRtv);
        if (currentBackBuffer != nullptr && resourceFromRtv.Get() == currentBackBuffer.Get())
            return true;

        Log("Back-buffer identity changed without a matching ResizeBuffers call; rebuilding resources");
        ReleaseFrameResources();
    }
    return CreateFrameResources(swapChain);
}

ComPtr<ID3D11ShaderResourceView> CreateDepthSrv(const DepthCandidate& candidate)
{
    if (!CanAccessDepth(candidate) || candidate.view == nullptr)
        return nullptr;

    ComPtr<ID3D11Resource> resource;
    candidate.view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
        return nullptr;
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    const DXGI_FORMAT copyFormat = DepthCopyFormat(textureDesc.Format);
    const DXGI_FORMAT srvFormat = DepthSrvFormat(copyFormat);
    if (srvFormat == DXGI_FORMAT_UNKNOWN)
        return nullptr;

    const bool directAccess =
        (textureDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        DepthSrvFormat(textureDesc.Format) != DXGI_FORMAT_UNKNOWN;

    if (!directAccess)
    {
        const bool copyMatches = g_depthCopy.texture != nullptr &&
            g_depthCopy.width == textureDesc.Width && g_depthCopy.height == textureDesc.Height &&
            g_depthCopy.format == copyFormat;
        if (!copyMatches)
        {
            g_depthCopy = {};
            D3D11_TEXTURE2D_DESC copyDesc = textureDesc;
            copyDesc.Format = copyFormat;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;

            HRESULT result = g_device->CreateTexture2D(&copyDesc, nullptr, &g_depthCopy.texture);
            if (FAILED(result))
            {
                Log("Depth copy texture creation failed: %ux%u format=%u hr=0x%08X",
                    copyDesc.Width, copyDesc.Height, copyDesc.Format, static_cast<unsigned>(result));
                return nullptr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC copySrvDesc{};
            copySrvDesc.Format = srvFormat;
            copySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            copySrvDesc.Texture2D.MipLevels = 1;
            result = g_device->CreateShaderResourceView(
                g_depthCopy.texture.Get(), &copySrvDesc, &g_depthCopy.srv);
            if (FAILED(result))
            {
                Log("Depth copy SRV creation failed: hr=0x%08X", static_cast<unsigned>(result));
                g_depthCopy = {};
                return nullptr;
            }
            g_depthCopy.width = textureDesc.Width;
            g_depthCopy.height = textureDesc.Height;
            g_depthCopy.format = copyFormat;
            Log("Depth copy allocated: %ux%u source=%u copy=%u", textureDesc.Width, textureDesc.Height,
                textureDesc.Format, copyFormat);
        }
        g_context->CopyResource(g_depthCopy.texture.Get(), texture.Get());
        return g_depthCopy.srv;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = srvFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(g_device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv)))
        return nullptr;
    return srv;
}

struct StateBackup
{
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef = 0;
    ComPtr<ID3D11RasterizerState> rasterizer;
    D3D11_VIEWPORT viewport{};
    UINT viewportCount = 1;
    ComPtr<ID3D11InputLayout> inputLayout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> psResources[2];
    ComPtr<ID3D11SamplerState> psSampler;
    ComPtr<ID3D11Buffer> psConstantBuffer;

    void Capture(ID3D11DeviceContext* context)
    {
        context->OMGetRenderTargets(1, &rtv, &dsv);
        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(&depthState, &stencilRef);
        context->RSGetState(&rasterizer);
        context->RSGetViewports(&viewportCount, &viewport);
        context->IAGetInputLayout(&inputLayout);
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(&vs, nullptr, nullptr);
        context->PSGetShader(&ps, nullptr, nullptr);
        context->PSGetShaderResources(0, 2, reinterpret_cast<ID3D11ShaderResourceView**>(psResources));
        context->PSGetSamplers(0, 1, &psSampler);
        context->PSGetConstantBuffers(0, 1, &psConstantBuffer);
    }

    void Restore(ID3D11DeviceContext* context)
    {
        ID3D11RenderTargetView* savedRtv = rtv.Get();
        context->OMSetRenderTargets(1, &savedRtv, dsv.Get());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthState.Get(), stencilRef);
        context->RSSetState(rasterizer.Get());
        if (viewportCount != 0)
            context->RSSetViewports(viewportCount, &viewport);
        context->IASetInputLayout(inputLayout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* resources[] = { psResources[0].Get(), psResources[1].Get() };
        context->PSSetShaderResources(0, 2, resources);
        ID3D11SamplerState* sampler = psSampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* constantBuffer = psConstantBuffer.Get();
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
    }
};

void RenderSettingsPanel()
{
    if (!g_menuOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(680.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Luminex Shaders 0.2-dev", &g_menuOpen))
    {
        ImGui::TextUnformatted("Roblox D3D11 post-processing runtime");
        if (g_effectFaulted.load(std::memory_order_relaxed))
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Effects disabled after a graphics failure");
        if (ImGui::BeginTabBar("LuminexTabs"))
        {
            if (ImGui::BeginTabItem("Packs"))
            {
                ImGui::TextUnformatted("Choose an effect bundle. Individual controls remain editable.");
                if (ImGui::BeginCombo("Shader pack", g_activePreset.c_str()))
                {
                    for (const auto& preset : g_presetPaths)
                    {
                        const std::string name = preset.stem().string();
                        const bool selected = name == g_activePreset;
                        if (ImGui::Selectable(name.c_str(), selected))
                            ApplyPreset(preset);
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Reload packs"))
                    DiscoverPresets();
                ImGui::Text("Discovered packs: %zu", g_presetPaths.size());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Effects"))
            {
                ImGui::SeparatorText("Color grading");
                ImGui::Checkbox("Enable color grading", &g_effectEnabled);
                ImGui::SliderFloat("Exposure", &g_exposure, -2.0f, 2.0f, "%.2f EV");
                ImGui::SliderFloat("Contrast", &g_contrast, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Saturation", &g_saturation, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Vibrance", &g_vibrance, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Gamma", &g_gamma, 0.25f, 3.0f, "%.2f");
                ImGui::SliderFloat("Temperature", &g_temperature, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Tint", &g_tint, -1.0f, 1.0f, "%.2f");
                const char* toneMaps[] = { "None", "Reinhard", "ACES filmic" };
                ImGui::Combo("Tone mapping", &g_tonemapMode, toneMaps, IM_ARRAYSIZE(toneMaps));
                ImGui::SeparatorText("Detail");
                ImGui::Checkbox("Enable sharpening", &g_sharpenEnabled);
                ImGui::SliderFloat("Sharpen strength", &g_sharpenStrength, 0.0f, 1.5f, "%.2f");
                ImGui::Checkbox("Enable vignette", &g_vignetteEnabled);
                ImGui::SliderFloat("Vignette strength", &g_vignetteStrength, 0.0f, 1.0f, "%.2f");
                if (ImGui::CollapsingHeader("Bloom and lens", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::Checkbox("Enable bloom", &g_bloomEnabled);
                    ImGui::SliderFloat("Bloom threshold", &g_bloomThreshold, 0.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat("Bloom strength", &g_bloomStrength, 0.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat("Bloom radius", &g_bloomRadius, 0.5f, 12.0f, "%.1f px");
                    ImGui::Checkbox("Chromatic aberration", &g_chromaticEnabled);
                    ImGui::SliderFloat("Chromatic strength", &g_chromaticStrength, 0.0f, 10.0f, "%.2f");
                }
                if (ImGui::CollapsingHeader("Film and stylize"))
                {
                    ImGui::Checkbox("Film grain", &g_grainEnabled);
                    ImGui::SliderFloat("Grain strength", &g_grainStrength, 0.0f, 0.25f, "%.3f");
                    ImGui::Checkbox("Sepia", &g_sepiaEnabled);
                    ImGui::SliderFloat("Sepia strength", &g_sepiaStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("Posterize", &g_posterizeEnabled);
                    ImGui::SliderFloat("Posterize levels", &g_posterizeLevels, 2.0f, 32.0f, "%.0f");
                }
                ImGui::SeparatorText("Depth fog");
                ImGui::Checkbox("Enable depth fog", &g_depthFogEnabled);
                ImGui::SliderFloat("Fog start", &g_fogStart, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Fog end", &g_fogEnd, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Fog strength", &g_fogStrength, 0.0f, 1.0f, "%.2f");
                ImGui::ColorEdit3("Fog color", g_fogColor);
                if (ImGui::CollapsingHeader("Advanced depth effects"))
                {
                    ImGui::Checkbox("Depth of field", &g_dofEnabled);
                    ImGui::SliderFloat("DOF focus", &g_dofFocus, 0.0f, 1.0f, "%.3f");
                    ImGui::SliderFloat("DOF range", &g_dofRange, 0.001f, 1.0f, "%.3f");
                    ImGui::SliderFloat("DOF blur", &g_dofStrength, 0.0f, 16.0f, "%.1f px");
                    ImGui::Checkbox("Depth outlines", &g_depthOutlineEnabled);
                    ImGui::SliderFloat("Outline threshold", &g_depthOutlineThreshold, 0.0001f, 0.2f, "%.4f");
                    ImGui::SliderFloat("Outline strength", &g_depthOutlineStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("Ambient occlusion", &g_ssaoEnabled);
                    ImGui::SliderFloat("AO radius", &g_ssaoRadius, 0.5f, 16.0f, "%.1f px");
                    ImGui::SliderFloat("AO strength", &g_ssaoStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::TextDisabled("Built-in SSR removed; reflections will use ReShade FX.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Depth"))
            {
                if (g_hookLevel < 5)
                {
                    ImGui::TextDisabled("Depth access is disabled at this diagnostic level.");
                }
                else
                {
                    bool collectDepth = g_collectDepth.load(std::memory_order_relaxed);
                    if (ImGui::Checkbox("Enable depth access", &collectDepth))
                    {
                        g_collectDepth.store(collectDepth, std::memory_order_relaxed);
                        if (!collectDepth)
                            g_depthPreview = false;
                    }
                    const auto selectedInfo = std::find_if(g_lastDepthCandidates.begin(), g_lastDepthCandidates.end(),
                        [](const DepthCandidateInfo& candidate) {
                            return MatchesDepthSelection(candidate, g_selectedDepthKey);
                        });
                    if (selectedInfo != g_lastDepthCandidates.end())
                    {
                        ImGui::Text("Selected: %ux%u %s (%s)", selectedInfo->width, selectedInfo->height,
                            FormatName(selectedInfo->format), selectedInfo->live ? "live" : "cached");
                    }
                    else
                    {
                        ImGui::TextDisabled("Selected: waiting for main-resolution depth");
                    }
                    if (ImGui::Button("Auto-select main depth"))
                    {
                        g_selectedDepthKey = {};
                        g_depthSelectionManual = false;
                    }
                    const bool selectable = collectDepth && selectedInfo != g_lastDepthCandidates.end();
                    if (!selectable)
                        ImGui::BeginDisabled();
                    ImGui::Checkbox("Visualize selected depth", &g_depthPreview);
                    ImGui::Checkbox("Invert depth preview", &g_depthInvert);
                    ImGui::SliderFloat("Depth preview curve", &g_depthPower, 0.05f, 32.0f, "%.2f");
                    if (!selectable)
                        ImGui::EndDisabled();

                    if (ImGui::CollapsingHeader("Advanced candidate diagnostics"))
                    {
                        ImGui::TextDisabled("Cached entries remain stable; live marks this frame's observations.");
                        for (int index = 0; index < static_cast<int>(g_lastDepthCandidates.size()); ++index)
                        {
                            const auto& candidate = g_lastDepthCandidates[index];
                            const bool mainResolution = candidate.width == g_backBufferWidth &&
                                candidate.height == g_backBufferHeight;
                            char label[224]{};
                            snprintf(label, sizeof(label), "%ux%u %s %s %s binds:%u access:%s##%d",
                                candidate.width, candidate.height, FormatName(candidate.format),
                                mainResolution ? "main-size" : "off-size",
                                candidate.live ? "live" : "cached", candidate.bindings,
                                DepthAccessName(candidate), index);
                            const bool accessible = CanAccessDepth(candidate);
                            if (!accessible)
                                ImGui::BeginDisabled();
                            const bool selected = MatchesDepthSelection(candidate, g_selectedDepthKey);
                            if (ImGui::RadioButton(label, selected) && accessible)
                            {
                                g_selectedDepthKey = MakeDepthSelection(candidate);
                                g_depthSelectionManual = true;
                            }
                            if (!accessible)
                                ImGui::EndDisabled();
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("ReShade FX"))
            {
                ImGui::TextUnformatted("Real ReShade-FX techniques compiled to D3D11 bytecode.");
                ImGui::TextDisabled("This first runtime slice supports one-pass COLOR/DEPTH effects.");
                for (auto& technique : g_fxTechniques)
                {
                    const std::string label = technique.name + "##" + technique.source;
                    ImGui::Checkbox(label.c_str(), &technique.enabled);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", technique.source.c_str());
                }
                for (const auto& message : g_fxMessages)
                    ImGui::TextDisabled("%s", message.c_str());
                if (g_fxTechniques.empty() && g_fxMessages.empty())
                    ImGui::TextDisabled("No directly executable techniques were found.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics"))
            {
                ImGui::Text("Present rate: %.1f FPS", g_presentFps);
                ImGui::Text("Luminex CPU cost: %.3f ms", g_runtimeCpuMs);
                ImGui::Text("Hook level: %d", g_hookLevel);
                ImGui::Text("Back buffer: %ux%u", g_backBufferWidth, g_backBufferHeight);
                ImGui::Text("Cached depth descriptors: %zu", g_lastDepthCandidates.size());
                if (g_hookLevel == 4 && g_colorStage == 1)
                    ImGui::TextUnformatted("Diagnostic: back-buffer copy only");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::Button("Save settings"))
            SaveSettings();
        ImGui::SameLine();
        ImGui::TextUnformatted("Home toggles this window");
    }
    ImGui::End();
}

void RenderReShadeEffects(ID3D11Texture2D* backBuffer,
    ID3D11ShaderResourceView* depthSrv, const D3D11_TEXTURE2D_DESC& desc)
{
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MaxDepth = 1.0f;

    for (auto& technique : g_fxTechniques)
    {
        if (!technique.enabled)
            continue;

        g_context->OMSetRenderTargets(0, nullptr, nullptr);
        g_context->CopyResource(g_colorCopy.Get(), backBuffer);
        g_context->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* target = g_backBufferRtv.Get();
        g_context->OMSetRenderTargets(1, &target, nullptr);
        g_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_context->OMSetDepthStencilState(nullptr, 0);
        g_context->RSSetState(nullptr);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(technique.vertexShader.Get(), nullptr, 0);
        g_context->PSSetShader(technique.pixelShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* resources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        UINT highestResource = 0;
        for (const auto& binding : technique.textures)
        {
            if (binding.slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
                continue;
            resources[binding.slot] = binding.depth ? depthSrv : g_colorSrv.Get();
            highestResource = std::max(highestResource, binding.slot + 1);
        }
        if (highestResource != 0)
            g_context->PSSetShaderResources(0, highestResource, resources);

        ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
        UINT highestSampler = 0;
        for (const UINT slot : technique.samplers)
        {
            if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
                continue;
            samplers[slot] = g_sampler.Get();
            highestSampler = std::max(highestSampler, slot + 1);
        }
        if (highestSampler != 0)
            g_context->PSSetSamplers(0, highestSampler, samplers);

        ID3D11Buffer* uniformBuffer = technique.uniformBuffer.Get();
        if (uniformBuffer != nullptr)
        {
            g_context->VSSetConstantBuffers(0, 1, &uniformBuffer);
            g_context->PSSetConstantBuffers(0, 1, &uniformBuffer);
        }
        g_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullResources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        if (highestResource != 0)
            g_context->PSSetShaderResources(0, highestResource, nullResources);
    }
}

void RenderEffect(IDXGISwapChain* swapChain)
{
    if (!EnsureFrameResources(swapChain))
        return;

    std::vector<DepthCandidate> candidates;
    if (g_collectDepth.load(std::memory_order_relaxed))
    {
        std::scoped_lock lock(g_depthMutex);
        candidates.swap(g_depthCandidates);
        std::sort(candidates.begin(), candidates.end(), [](const DepthCandidate& left, const DepthCandidate& right) {
            const unsigned long long leftArea = static_cast<unsigned long long>(left.width) * left.height;
            const unsigned long long rightArea = static_cast<unsigned long long>(right.width) * right.height;
            return leftArea != rightArea ? leftArea > rightArea : left.bindings > right.bindings;
        });
        for (auto& cached : g_lastDepthCandidates)
            cached.live = false;
        for (const auto& candidate : candidates)
        {
            const auto found = std::find_if(g_lastDepthCandidates.begin(), g_lastDepthCandidates.end(),
                [&candidate](const DepthCandidateInfo& cached) {
                    return cached.width == candidate.width && cached.height == candidate.height &&
                        cached.format == candidate.format && cached.bindFlags == candidate.bindFlags &&
                        cached.sampleCount == candidate.sampleCount && cached.arraySize == candidate.arraySize &&
                        cached.mipLevels == candidate.mipLevels;
                });
            if (found == g_lastDepthCandidates.end())
            {
                g_lastDepthCandidates.push_back({ candidate.width, candidate.height, candidate.format,
                    candidate.bindFlags, candidate.sampleCount, candidate.arraySize, candidate.mipLevels,
                    candidate.bindings, true });
            }
            else
            {
                found->bindings = candidate.bindings;
                found->live = true;
            }
        }
        static unsigned long long lastDepthSignature = 0;
        static auto lastDepthLog = std::chrono::steady_clock::time_point{};
        unsigned long long depthSignature = 1469598103934665603ull;
        for (const auto& item : candidates)
        {
            const unsigned long long values[] = { item.width, item.height,
                static_cast<unsigned long long>(item.format), item.sampleCount,
                item.arraySize, item.mipLevels, item.bindings };
            for (const auto value : values)
            {
                depthSignature ^= value;
                depthSignature *= 1099511628211ull;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!candidates.empty() && depthSignature != lastDepthSignature &&
            (lastDepthLog.time_since_epoch().count() == 0 || now - lastDepthLog >= std::chrono::seconds(10)))
        {
            Log("Depth scan: %zu candidates", candidates.size());
            for (size_t index = 0; index < candidates.size(); ++index)
            {
                const auto& item = candidates[index];
                Log("  [%zu] %ux%u format=%u samples=%u array=%u mips=%u binds=%u access=%s", index,
                    item.width, item.height, item.format, item.sampleCount, item.arraySize, item.mipLevels,
                    item.bindings, DepthAccessName(item));
            }
            lastDepthSignature = depthSignature;
            lastDepthLog = now;
        }
    }
    else
    {
        std::scoped_lock lock(g_depthMutex);
        g_depthCandidates.clear();
        for (auto& cached : g_lastDepthCandidates)
            cached.live = false;
    }
    g_selectedDepth = -1;
    if (g_selectedDepthKey.valid)
    {
        for (int index = 0; index < static_cast<int>(candidates.size()); ++index)
        {
            if (CanAccessDepth(candidates[index]) &&
                MatchesDepthSelection(candidates[index], g_selectedDepthKey))
            {
                g_selectedDepth = index;
                break;
            }
        }
    }
    if (g_selectedDepth < 0 && !g_depthSelectionManual && !candidates.empty())
    {
        for (int index = 0; index < static_cast<int>(candidates.size()); ++index)
        {
            if (CanAccessDepth(candidates[index]) && candidates[index].width == g_backBufferWidth &&
                candidates[index].height == g_backBufferHeight)
            {
                g_selectedDepth = index;
                break;
            }
        }
        if (g_selectedDepth < 0 && !g_selectedDepthKey.valid)
        {
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                [](const DepthCandidate& candidate) { return CanAccessDepth(candidate); });
            if (found != candidates.end())
                g_selectedDepth = static_cast<int>(std::distance(candidates.begin(), found));
        }
        if (g_selectedDepth >= 0)
            g_selectedDepthKey = MakeDepthSelection(candidates[g_selectedDepth]);
    }
    static DepthSelectionKey lastLoggedDepthSelection;
    if (!SameDepthSelection(g_selectedDepthKey, lastLoggedDepthSelection))
    {
        if (g_selectedDepth >= 0 && g_selectedDepth < static_cast<int>(candidates.size()))
        {
            const auto& selected = candidates[g_selectedDepth];
            Log("Depth selected: index=%d size=%ux%u format=%u samples=%u access=%s",
                g_selectedDepth, selected.width, selected.height, selected.format,
                selected.sampleCount, DepthAccessName(selected));
        }
        else
        {
            Log("Depth selected: none");
        }
        lastLoggedDepthSelection = g_selectedDepthKey;
    }

    ScopedContextIsolation isolation;
    if (!isolation.Active())
        return;

    ComPtr<ID3D11ShaderResourceView> depthSrv;
    if (g_selectedDepth >= 0 && g_selectedDepth < static_cast<int>(candidates.size()))
        depthSrv = CreateDepthSrv(candidates[g_selectedDepth]);

    if (g_reshadeRuntime != nullptr && g_reshadePresentRuntime != nullptr)
    {
        g_reshadeDepthSrv = depthSrv;
        const reshade::api::resource_view view = {
            reinterpret_cast<uint64_t>(g_reshadeDepthSrv.Get())
        };
        g_reshadeRuntime->update_texture_bindings("DEPTH", view, view);
        g_reshadePresentRuntime(g_reshadeRuntime);
        return;
    }

    const bool anyPostEffect = g_effectEnabled || g_sharpenEnabled || g_vignetteEnabled ||
        g_bloomEnabled || g_chromaticEnabled || g_grainEnabled || g_sepiaEnabled ||
        g_posterizeEnabled || g_depthFogEnabled || g_dofEnabled || g_depthOutlineEnabled ||
        g_ssaoEnabled || g_ssrEnabled || std::any_of(g_fxTechniques.begin(), g_fxTechniques.end(),
            [](const FxTechniqueRuntime& technique) { return technique.enabled; });
    if (g_hookLevel >= 4 && !g_effectFaulted.load(std::memory_order_relaxed) &&
        (anyPostEffect || (g_depthPreview && depthSrv != nullptr)))
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            g_context->CopyResource(g_colorCopy.Get(), backBuffer.Get());
            if (!g_loggedColorCopy)
            {
                Log("Color back-buffer copy completed");
                g_loggedColorCopy = true;
            }

            if (g_colorStage >= 2)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(g_context->Map(g_settingsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    auto* settings = static_cast<ShaderSettings*>(mapped.pData);
                    const float effectExposure = g_effectEnabled ? g_exposure : 0.0f;
                    const float effectContrast = g_effectEnabled ? g_contrast : 1.0f;
                    const float effectSaturation = g_effectEnabled ? g_saturation : 1.0f;
                    const float effectGamma = g_effectEnabled ? g_gamma : 1.0f;
                    const float effectVibrance = g_effectEnabled ? g_vibrance : 0.0f;
                    const float effectTemperature = g_effectEnabled ? g_temperature : 0.0f;
                    const float effectTint = g_effectEnabled ? g_tint : 0.0f;
                    const float effectTonemap = g_effectEnabled ? static_cast<float>(g_tonemapMode) : 0.0f;
                    const float animationTime = std::chrono::duration<float>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    *settings = { effectExposure, effectContrast, effectSaturation, g_depthPreview ? 1.0f : 0.0f,
                        depthSrv != nullptr ? 1.0f : 0.0f, g_depthInvert ? 1.0f : 0.0f,
                        g_depthPower, 0.0f, g_depthFogEnabled ? 1.0f : 0.0f,
                        g_fogStart, g_fogEnd, g_fogStrength,
                        { g_fogColor[0], g_fogColor[1], g_fogColor[2] }, 0.0f,
                        g_sharpenEnabled ? 1.0f : 0.0f, g_sharpenStrength,
                        1.0f / static_cast<float>(desc.Width), 1.0f / static_cast<float>(desc.Height),
                        g_vignetteEnabled ? 1.0f : 0.0f, g_vignetteStrength,
                        effectGamma, effectVibrance, effectTemperature, effectTint,
                        effectTonemap, g_bloomEnabled ? 1.0f : 0.0f, g_bloomThreshold, g_bloomStrength,
                        g_bloomRadius, g_chromaticEnabled ? 1.0f : 0.0f, g_chromaticStrength,
                        g_grainEnabled ? 1.0f : 0.0f, g_grainStrength, animationTime,
                        g_sepiaEnabled ? 1.0f : 0.0f, g_sepiaStrength,
                        g_posterizeEnabled ? 1.0f : 0.0f, g_posterizeLevels,
                        g_dofEnabled ? 1.0f : 0.0f, g_dofFocus, g_dofRange, g_dofStrength,
                        g_depthOutlineEnabled ? 1.0f : 0.0f, g_depthOutlineThreshold,
                        g_depthOutlineStrength, g_ssaoEnabled ? 1.0f : 0.0f,
                        g_ssaoRadius, g_ssaoStrength, g_ssrEnabled ? 1.0f : 0.0f,
                        g_ssrStrength, g_ssrMaxDistance, g_ssrThickness, {} };
                    g_context->Unmap(g_settingsBuffer.Get(), 0);
                }

                if (g_bloomEnabled)
                {
                    D3D11_VIEWPORT bloomViewport{};
                    bloomViewport.Width = static_cast<float>(std::max(1u, desc.Width / 2));
                    bloomViewport.Height = static_cast<float>(std::max(1u, desc.Height / 2));
                    bloomViewport.MaxDepth = 1.0f;
                    g_context->RSSetViewports(1, &bloomViewport);
                    g_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                    g_context->OMSetDepthStencilState(nullptr, 0);
                    g_context->RSSetState(nullptr);
                    g_context->IASetInputLayout(nullptr);
                    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
                    ID3D11SamplerState* bloomSampler = g_sampler.Get();
                    g_context->PSSetSamplers(0, 1, &bloomSampler);
                    ID3D11Buffer* bloomBuffer = g_settingsBuffer.Get();
                    g_context->PSSetConstantBuffers(0, 1, &bloomBuffer);

                    ID3D11RenderTargetView* bloomTarget = g_bloomRtvA.Get();
                    g_context->OMSetRenderTargets(1, &bloomTarget, nullptr);
                    g_context->PSSetShader(g_bloomExtractShader.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* extractResources[] = { g_colorSrv.Get(), nullptr, nullptr };
                    g_context->PSSetShaderResources(0, 3, extractResources);
                    g_context->Draw(3, 0);
                    ID3D11ShaderResourceView* nullBloomResources[] = { nullptr, nullptr, nullptr };
                    g_context->PSSetShaderResources(0, 3, nullBloomResources);

                    bloomTarget = g_bloomRtvB.Get();
                    g_context->OMSetRenderTargets(1, &bloomTarget, nullptr);
                    g_context->PSSetShader(g_bloomBlurHShader.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* horizontalResources[] = { g_bloomSrvA.Get(), nullptr, nullptr };
                    g_context->PSSetShaderResources(0, 3, horizontalResources);
                    g_context->Draw(3, 0);
                    g_context->PSSetShaderResources(0, 3, nullBloomResources);

                    bloomTarget = g_bloomRtvA.Get();
                    g_context->OMSetRenderTargets(1, &bloomTarget, nullptr);
                    g_context->PSSetShader(g_bloomBlurVShader.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* verticalResources[] = { g_bloomSrvB.Get(), nullptr, nullptr };
                    g_context->PSSetShaderResources(0, 3, verticalResources);
                    g_context->Draw(3, 0);
                    g_context->PSSetShaderResources(0, 3, nullBloomResources);
                }

                D3D11_VIEWPORT viewport{};
                viewport.Width = static_cast<float>(desc.Width);
                viewport.Height = static_cast<float>(desc.Height);
                viewport.MaxDepth = 1.0f;
                g_context->RSSetViewports(1, &viewport);
                ID3D11RenderTargetView* rtv = g_backBufferRtv.Get();
                g_context->OMSetRenderTargets(1, &rtv, nullptr);
                g_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                g_context->OMSetDepthStencilState(nullptr, 0);
                g_context->RSSetState(nullptr);
                g_context->IASetInputLayout(nullptr);
                g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
                g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
                ID3D11ShaderResourceView* resources[] = { g_colorSrv.Get(), depthSrv.Get(),
                    g_bloomEnabled ? g_bloomSrvA.Get() : nullptr };
                g_context->PSSetShaderResources(0, 3, resources);
                ID3D11SamplerState* sampler = g_sampler.Get();
                g_context->PSSetSamplers(0, 1, &sampler);
                ID3D11Buffer* buffer = g_settingsBuffer.Get();
                g_context->PSSetConstantBuffers(0, 1, &buffer);
                g_context->Draw(3, 0);
                if (!g_loggedColorDraw)
                {
                    Log("Fullscreen color draw submitted");
                    g_loggedColorDraw = true;
                }
                ID3D11ShaderResourceView* nullResources[] = { nullptr, nullptr, nullptr };
                g_context->PSSetShaderResources(0, 3, nullResources);

                RenderReShadeEffects(backBuffer.Get(), depthSrv.Get(), desc);
            }
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderSettingsPanel();
    ImGui::Render();
    ID3D11RenderTargetView* rtv = g_backBufferRtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

}

HRESULT __stdcall HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    static thread_local bool insideLuminexPresent = false;
    if (insideLuminexPresent || (flags & DXGI_PRESENT_TEST) != 0)
        return g_originalPresent(swapChain, syncInterval, flags);

    if (g_hookLevel <= 1)
        return g_originalPresent(swapChain, syncInterval, flags);

    if (!AcceptSwapChain(swapChain))
        return g_originalPresent(swapChain, syncInterval, flags);
    if (g_hookLevel == 2)
        return g_originalPresent(swapChain, syncInterval, flags);

    struct PresentGuard
    {
        explicit PresentGuard(bool& value) : value_(value) { value_ = true; }
        ~PresentGuard() { value_ = false; }
        bool& value_;
    } guard(insideLuminexPresent);

    using Clock = std::chrono::steady_clock;
    static auto previousPresent = Clock::now();
    const auto presentStart = Clock::now();
    const float frameSeconds = std::chrono::duration<float>(presentStart - previousPresent).count();
    previousPresent = presentStart;
    if (frameSeconds > 0.0f && frameSeconds < 1.0f)
    {
        const float instantaneousFps = 1.0f / frameSeconds;
        g_presentFps = g_presentFps == 0.0f ? instantaneousFps :
            g_presentFps * 0.95f + instantaneousFps * 0.05f;
    }

    if (!g_device && !InitializeRuntime(swapChain))
    {
        Log("Runtime initialization failed");
        return g_originalPresent(swapChain, syncInterval, flags);
    }
    if (g_device && g_reshadeRuntime == nullptr && !g_reshadeAttempted)
        InitializeReShadeRuntime(swapChain);
    if (g_reshadeRuntime != nullptr || g_imguiReady)
        RenderEffect(swapChain);
    static bool loggedInputBlockingPath = false;
    if (!loggedInputBlockingPath)
    {
        const auto rawData = g_blockedRawInputDataMouse.load(std::memory_order_relaxed);
        const auto rawBuffer = g_blockedRawInputBufferMouse.load(std::memory_order_relaxed);
        const auto rawDataKeyboard = g_blockedRawInputDataKeyboard.load(std::memory_order_relaxed);
        const auto rawBufferKeyboard = g_blockedRawInputBufferKeyboard.load(std::memory_order_relaxed);
        const auto windowWheel = g_blockedWindowWheel.load(std::memory_order_relaxed);
        if (rawData != 0 || rawBuffer != 0 || rawDataKeyboard != 0 || rawBufferKeyboard != 0 || windowWheel != 0)
        {
            Log("Overlay input blocked: window-wheel=%llu raw-data-mouse=%llu raw-buffer-mouse=%llu raw-data-keyboard=%llu raw-buffer-keyboard=%llu",
                static_cast<unsigned long long>(windowWheel),
                static_cast<unsigned long long>(rawData),
                static_cast<unsigned long long>(rawBuffer),
                static_cast<unsigned long long>(rawDataKeyboard),
                static_cast<unsigned long long>(rawBufferKeyboard));
            loggedInputBlockingPath = true;
        }
    }
    const float elapsedMs = std::chrono::duration<float, std::milli>(Clock::now() - presentStart).count();
    g_runtimeCpuMs = g_runtimeCpuMs == 0.0f ? elapsedMs : g_runtimeCpuMs * 0.95f + elapsedMs * 0.05f;
    const HRESULT result = g_originalPresent(swapChain, syncInterval, flags);
    if (FAILED(result))
    {
        const HRESULT removedReason = g_device != nullptr ? g_device->GetDeviceRemovedReason() : S_OK;
        Log("Present failed: hr=0x%08X deviceReason=0x%08X; effects disabled",
            static_cast<unsigned>(result), static_cast<unsigned>(removedReason));
        g_effectFaulted.store(true, std::memory_order_relaxed);
    }
    return result;
}

HRESULT __stdcall HookResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (g_hookLevel > 1 && swapChain == g_primarySwapChain)
    {
        Log("ResizeBuffers begin: buffers=%u size=%ux%u format=%u", bufferCount, width, height, format);
        g_reshadeDepthSrv.Reset();
        if (g_reshadeRuntime != nullptr && g_reshadeBeginResize != nullptr)
            g_reshadeBeginResize(g_reshadeRuntime);
        ReleaseFrameResources();
    }
    const HRESULT result = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
    if (g_hookLevel > 1 && swapChain == g_primarySwapChain)
    {
        Log("ResizeBuffers end: hr=0x%08X", static_cast<unsigned>(result));
        if (SUCCEEDED(result) && g_reshadeRuntime != nullptr && g_reshadeEndResize != nullptr)
        {
            if (g_reshadeEndResize(g_reshadeRuntime))
                Log("ReShade GPU environment resized without recreating the runtime");
            else
            {
                Log("ReShade resize reinitialization failed; falling back to full recreation");
                ShutdownReShadeRuntime();
                g_reshadeAttempted = false;
            }
        }
    }
    return result;
}

void __stdcall HookOMSetRenderTargets(
    ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* renderTargets,
    ID3D11DepthStencilView* depthView)
{
    if (g_renderingLuminex || !g_collectDepth.load(std::memory_order_relaxed) || context != g_context.Get())
    {
        g_originalOMSetRenderTargets(context, count, renderTargets, depthView);
        return;
    }

    if (depthView != nullptr)
    {
        bool needsMetadata = false;
        {
            std::scoped_lock lock(g_depthMutex);
            auto found = std::find_if(g_depthCandidates.begin(), g_depthCandidates.end(),
                [depthView](const DepthCandidate& candidate) { return candidate.view.Get() == depthView; });
            if (found != g_depthCandidates.end())
            {
                ++found->bindings;
            }
            else
            {
                needsMetadata = g_depthCandidates.size() < 16;
            }
        }

        if (needsMetadata)
        {
            ComPtr<ID3D11Resource> resource;
            depthView->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture)))
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                DepthCandidate candidate;
                candidate.view = depthView;
                candidate.width = desc.Width;
                candidate.height = desc.Height;
                candidate.format = desc.Format;
                candidate.bindFlags = desc.BindFlags;
                candidate.sampleCount = desc.SampleDesc.Count;
                candidate.arraySize = desc.ArraySize;
                candidate.mipLevels = desc.MipLevels;
                candidate.bindings = 1;
                std::scoped_lock lock(g_depthMutex);
                const auto duplicate = std::find_if(g_depthCandidates.begin(), g_depthCandidates.end(),
                    [depthView](const DepthCandidate& item) { return item.view.Get() == depthView; });
                if (duplicate == g_depthCandidates.end() && g_depthCandidates.size() < 16)
                    g_depthCandidates.push_back(std::move(candidate));
            }
        }
    }
    g_originalOMSetRenderTargets(context, count, renderTargets, depthView);
}

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

bool InstallHooks()
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = DummyWindowProc;
    windowClass.hInstance = g_module;
    windowClass.lpszClassName = L"LuminexHookDiscovery";
    RegisterClassExW(&windowClass);
    HWND dummyWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, g_module, nullptr);
    if (dummyWindow == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 1;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = dummyWindow;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;

    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_0 };
    const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        requested, 1, D3D11_SDK_VERSION, &swapDesc, &swapChain, &device, &featureLevel, &context);
    if (FAILED(result))
    {
        DestroyWindow(dummyWindow);
        UnregisterClassW(windowClass.lpszClassName, g_module);
        return false;
    }

    void** swapVtable = *reinterpret_cast<void***>(swapChain.Get());
    void** contextVtable = *reinterpret_cast<void***>(context.Get());
    const bool coreHooksCreated =
        MH_Initialize() == MH_OK &&
        MH_CreateHook(swapVtable[8], HookPresent, reinterpret_cast<void**>(&g_originalPresent)) == MH_OK &&
        MH_CreateHook(swapVtable[13], HookResizeBuffers, reinterpret_cast<void**>(&g_originalResizeBuffers)) == MH_OK;
    const bool depthHookCreated = g_hookLevel < 5 ||
        MH_CreateHook(contextVtable[33], HookOMSetRenderTargets,
            reinterpret_cast<void**>(&g_originalOMSetRenderTargets)) == MH_OK;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const bool inputHooksCreated = user32 != nullptr &&
        MH_CreateHook(GetProcAddress(user32, "GetAsyncKeyState"), HookGetAsyncKeyState,
            reinterpret_cast<void**>(&g_originalGetAsyncKeyState)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "GetKeyState"), HookGetKeyState,
            reinterpret_cast<void**>(&g_originalGetKeyState)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "GetKeyboardState"), HookGetKeyboardState,
            reinterpret_cast<void**>(&g_originalGetKeyboardState)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "ClipCursor"), HookClipCursor,
            reinterpret_cast<void**>(&g_originalClipCursor)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "SetCursorPos"), HookSetCursorPos,
            reinterpret_cast<void**>(&g_originalSetCursorPos)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "GetRawInputData"), HookGetRawInputData,
            reinterpret_cast<void**>(&g_originalGetRawInputData)) == MH_OK &&
        MH_CreateHook(GetProcAddress(user32, "GetRawInputBuffer"), HookGetRawInputBuffer,
            reinterpret_cast<void**>(&g_originalGetRawInputBuffer)) == MH_OK;
    if (!coreHooksCreated || !depthHookCreated || !inputHooksCreated ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        DestroyWindow(dummyWindow);
        UnregisterClassW(windowClass.lpszClassName, g_module);
        return false;
    }

    DestroyWindow(dummyWindow);
    UnregisterClassW(windowClass.lpszClassName, g_module);
    Log("D3D11 hooks installed at diagnostic level %d", g_hookLevel);
    return true;
}

} // namespace

void SetModuleHandle(HMODULE module)
{
    g_module = module;
}

DWORD WINAPI RuntimeThread(void*)
{
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, MAX_PATH);
    g_moduleDirectory = std::filesystem::path(modulePath).parent_path();
    std::error_code directoryError;
    std::filesystem::create_directories(g_moduleDirectory / L"Config", directoryError);
    const auto diagnosticsPath = g_moduleDirectory / L"Config" / L"Diagnostics.ini";
    if (GetPrivateProfileIntW(L"Diagnostics", L"SaveLogs", 1, diagnosticsPath.c_str()) != 0)
    {
        std::filesystem::create_directories(g_moduleDirectory / L"Logs", directoryError);
        g_log = _wfsopen((g_moduleDirectory / L"Logs" / L"Luminex.log").c_str(), L"w", _SH_DENYNO);
    }
    Log("Luminex 0.2-dev loading");
    LoadSettings();
    DiscoverPresets();

    if (g_hookLevel == 0)
    {
        Log("Diagnostic level 0: runtime loaded without graphics hooks");
        return 0;
    }

    for (int attempt = 0; attempt < 300 && !g_stopping; ++attempt)
    {
        if (GetModuleHandleW(L"d3d11.dll") != nullptr && GetModuleHandleW(L"dxgi.dll") != nullptr)
            break;
        Sleep(100);
    }

    if (!g_stopping && !InstallHooks())
        Log("Failed to install D3D11 hooks");
    return 0;
}

void Shutdown()
{
    g_stopping = true;
    ShutdownReShadeRuntime();
    if (g_originalWndProc != nullptr && g_window != nullptr && IsWindow(g_window))
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    if (g_imguiReady)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ReleaseFrameResources();
    g_sampler.Reset();
    g_settingsBuffer.Reset();
    g_pixelShader.Reset();
    g_vertexShader.Reset();
    g_context.Reset();
    g_device.Reset();
    if (g_log != nullptr)
    {
        fclose(g_log);
        g_log = nullptr;
    }
}

} // namespace luminex
