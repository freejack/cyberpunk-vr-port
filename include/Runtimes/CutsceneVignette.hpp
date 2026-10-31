#pragma once

// ================================================================================================
// CUTSCENE COMFORT VIGNETTE -- the policy and the per-eye geometry.
//
// In a cinematic the camera is moved by an authored animation (a car ride, being carried, a
// stumble), not by the player. The eyes see motion the inner ear does not feel, and the PERIPHERY
// is where that optic flow is sensed most strongly -- which is why darkening the edges of the view
// is the standard VR comfort measure. This draws that vignette, but only while the player's own
// scene tier says a cinematic is running, and fades it in and out so the field of view never snaps.
//
// WHERE IT IS DRAWN: into each eye's OpenXR swapchain image, on the submit command list, right after
// the eye's pixels are copied in (OpenXRFrameLoop.cpp, beside the VR menu's blink). So it is:
//   * head-locked by construction -- it is part of the image the lens shows, not of the world;
//   * on every submitted frame, generated ones included -- it is applied AFTER frame generation, so
//     the interpolator never sees a dark border it would have to track;
//   * under the HUD quads and the F10 panel, which are separate composition layers -- subtitles,
//     dialogue choices and the settings panel stay fully readable.
//
// WHERE IT IS CENTRED: on the eye's optical axis, not the middle of the image. With the asymmetric
// projection the image spans an off-centre window (e.g. -52..+43 degrees), so the image centre is
// several degrees off where the eye looks. The submitted FOV says exactly which direction every pixel
// is, so the vignette is a ring of constant ANGLE around that eye's forward direction -- the same
// ring in every headset and in both eyes.
//
// Everything here is pure: no D3D, no RED4ext, no globals. The shader in src/Render/VignettePass.cpp
// is a transcription of VignetteKeep() below; tools/comfort_tests checks the two against each other
// on the real adapter.
// ================================================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cvr::comfort {

// ---- settings ------------------------------------------------------------------------------------

// The tier threshold uses the SAME stored encoding as the VRIK cutscene suspend
// (LiveControls::xrCutsceneSuspendTier), so the two combos read alike:
//   -1 (or 0) = off, 1 = Tier2+, 2 = Tier3+, 3 = Tier4+ (FPP cinematics and up), 4 = Tier5 only.
// The engine's GameplayTier: 1 = Tier1_FullGameplay .. 4 = Tier4_FPPCinematic, 5 = Tier5_Cinematic
// (static_assert'ed against the SDK enum in src/Runtimes/CutsceneVignette.cpp).
inline constexpr int kEngineTierFullGameplay = 1;
inline constexpr int kEngineTierCinematic = 5;
inline constexpr int kVignetteTierOff = -1;

// Tier4+ by default: the same threshold the VRIK suspend ships with, measured there to be "the camera
// belongs to the scene". Takeover cameras and the AV turret run at Tier 3, so they are not touched.
inline constexpr int kDefaultVignetteTier = 3;
inline constexpr float kDefaultVignetteStrength = 0.85f;  // perceived darkening at the outer edge

// THE SIZE IS THREE STEPS, not a free angle. clearDeg is the full angle left untouched around the eye's
// axis; past it the darkening ramps up over softDeg to full. Medium is the default: in a headset test
// Light was too weak. Strong is a tunnel for players who still feel the scene move with Medium.
struct VignetteSizeStep {
    float clearDeg;
    float softDeg;
};
inline constexpr int kVignetteSizeLight = 0, kVignetteSizeMedium = 1, kVignetteSizeStrong = 2;
inline constexpr int kVignetteSizeCount = 3;
inline constexpr VignetteSizeStep kVignetteSizes[kVignetteSizeCount] = {
    {60.0f, 20.0f},  // Light:  darkening from 30 deg off-axis, full at 50
    {45.0f, 20.0f},  // Medium: from 22.5, full at 42.5
    {30.0f, 20.0f},  // Strong: from 15, full at 35
};
inline constexpr int kDefaultVignetteSize = kVignetteSizeMedium;
// Seconds for a full fade in or out. A cinematic's first frames are often its most violent, so it
// cannot be slow; a snap of the field of view is itself a jolt, so it cannot be instant.
inline constexpr float kVignetteFadeSeconds = 0.5f;
// How old the published scene tier may get before it is not believed. It is refreshed with the player
// flags on the locate path; with no player (main menu, loading) it stops being refreshed and would
// otherwise hold whatever the last cutscene left in it. Same window as the VR menu blink.
inline constexpr uint64_t kSceneTierStaleMs = 2000;

struct VignetteSettings {
    int tier = kDefaultVignetteTier;
    float strength = kDefaultVignetteStrength;
    int size = kDefaultVignetteSize;
};

inline float FiniteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

inline int NormalizeVignetteTier(int tier) {
    if (tier < 1) return kVignetteTierOff;
    return tier > 4 ? 4 : tier;
}

inline int NormalizeVignetteSize(int size) {
    return std::clamp(size, 0, kVignetteSizeCount - 1);
}

inline VignetteSettings NormalizeVignette(VignetteSettings s) {
    s.tier = NormalizeVignetteTier(s.tier);
    s.strength = std::clamp(FiniteOr(s.strength, kDefaultVignetteStrength), 0.0f, 1.0f);
    s.size = NormalizeVignetteSize(s.size);
    return s;
}

// ---- when ----------------------------------------------------------------------------------------

// engineTier at or above the configured threshold (and a real tier, not Undefined = 0).
inline bool SceneTierWantsVignette(int engineTier, int configuredTier) {
    configuredTier = NormalizeVignetteTier(configuredTier);
    if (configuredTier == kVignetteTierOff) return false;
    const int minimum = kEngineTierFullGameplay + configuredTier;
    return engineTier >= minimum && engineTier <= kEngineTierCinematic;
}

struct VignetteInputs {
    int engineTier = 0;
    uint64_t tierAgeMs = UINT64_MAX;  // since the tier was last read off the player
    bool braindance = false;          // the player scrubs and orbits the camera: theirs, not a scene's
    bool deviceCamera = false;        // a camera taken over (CCTV, turret): driven by the head
};

// Whether this frame wants the vignette at all. The two interactive camera modes are excluded on
// purpose: in both the player is the one moving the view and needs the periphery to look around.
inline bool WantsVignette(const VignetteInputs& in, int configuredTier) {
    if (in.tierAgeMs > kSceneTierStaleMs) return false;
    if (in.braindance || in.deviceCamera) return false;
    return SceneTierWantsVignette(in.engineTier, configuredTier);
}

// Linear ramp toward the target, at most kVignetteFadeSeconds end to end. dt is clamped so a hitch
// (or the first call) cannot jump the fade, and so a clock going backwards cannot reverse it.
inline float StepVignetteFade(float fade, bool want, float dtSeconds) {
    fade = std::clamp(FiniteOr(fade, 0.0f), 0.0f, 1.0f);
    const float dt = std::clamp(FiniteOr(dtSeconds, 0.0f), 0.0f, 0.1f);
    const float step = dt / kVignetteFadeSeconds;
    return want ? std::min(1.0f, fade + step) : std::max(0.0f, fade - step);
}

// What the eye sees of the ramp: eased at both ends, so the vignette creeps in instead of starting
// with a visible jump in its first frame.
inline float EaseVignetteFade(float fade) {
    fade = std::clamp(fade, 0.0f, 1.0f);
    return fade * fade * (3.0f - 2.0f * fade);
}

// ---- where ---------------------------------------------------------------------------------------

// The pixel shader's root constants, in register order: b0 = { tan, shape }. 8 x 32 bit.
struct VignetteShape {
    // Tangents of the submitted half-angles. u = 0..1 maps tanLeft..tanRight, v = 0..1 (top down)
    // maps tanUp..tanDown -- exactly how the compositor maps the image rect onto the FOV.
    float tanLeft = 0.0f, tanRight = 0.0f, tanUp = 0.0f, tanDown = 0.0f;
    float innerRad = 0.0f;  // angle from the optical axis where darkening starts
    float outerRad = 0.0f;  // where it reaches `darken`
    float darken = 0.0f;    // perceived darkening at and past outerRad, fade included; 0 = nothing to draw
    // 2.2 when the target blends in linear light (an _SRGB view, a float format, or the decoded path):
    // the darkening is specified in PERCEIVED brightness, and the multiply happens in linear, so
    // keep = perceived^2.2. Without it a strength of 0.85 would look like barely half. 1.0 when the
    // target holds display-encoded values and blends them as they are.
    float exponent = 1.0f;
};
static_assert(sizeof(VignetteShape) == 8 * sizeof(float), "VignetteShape is the shader's root constant block");

inline constexpr float kDegToRad = 0.01745329252f;

// angleLeft/Right/Up/Down: the XrFovf the eye is SUBMITTED with (radians, left and down negative).
// Call it after every adjustment to that FOV, so the vignette follows what the lens actually shows.
inline VignetteShape MakeVignetteShape(float angleLeft, float angleRight, float angleUp, float angleDown,
                                       const VignetteSettings& settings, float easedFade, bool linearTarget) {
    const VignetteSettings s = NormalizeVignette(settings);
    VignetteShape shape;
    shape.tanLeft = std::tan(angleLeft);
    shape.tanRight = std::tan(angleRight);
    shape.tanUp = std::tan(angleUp);
    shape.tanDown = std::tan(angleDown);
    const VignetteSizeStep& step = kVignetteSizes[s.size];
    shape.innerRad = 0.5f * step.clearDeg * kDegToRad;
    shape.outerRad = shape.innerRad + step.softDeg * kDegToRad;
    shape.darken = s.strength * std::clamp(FiniteOr(easedFade, 0.0f), 0.0f, 1.0f);
    shape.exponent = linearTarget ? 2.2f : 1.0f;
    const bool usable = std::isfinite(shape.tanLeft) && std::isfinite(shape.tanRight) &&
        std::isfinite(shape.tanUp) && std::isfinite(shape.tanDown) &&
        shape.tanRight > shape.tanLeft && shape.tanUp > shape.tanDown;
    if (!usable) shape.darken = 0.0f;
    return shape;
}

// Pixels the shader leaves alone entirely (it discards them, so the inside of the ring costs no
// blend bandwidth).
inline constexpr float kVignetteDiscardBelow = 0.0005f;

// The shader, on the CPU: the factor the pixel at (u, v) is multiplied by, 1 = untouched.
// Transcribed line for line into kPixelShader in src/Render/VignettePass.cpp.
inline float VignetteKeep(const VignetteShape& shape, float u, float v) {
    const float tx = shape.tanLeft + (shape.tanRight - shape.tanLeft) * u;
    const float ty = shape.tanUp + (shape.tanDown - shape.tanUp) * v;
    const float angle = std::atan(std::sqrt(tx * tx + ty * ty));
    const float span = std::max(shape.outerRad - shape.innerRad, 1e-4f);
    const float t = std::clamp((angle - shape.innerRad) / span, 0.0f, 1.0f);
    const float ramp = t * t * (3.0f - 2.0f * t);
    const float darken = shape.darken * ramp;
    if (darken <= kVignetteDiscardBelow) return 1.0f;
    return std::pow(1.0f - darken, shape.exponent);
}

// ---- the running state (src/Runtimes/CutsceneVignette.cpp) ---------------------------------------

struct VignetteFrame {
    float amount = 0.0f;         // eased fade, 0..1; 0 = draw nothing
    VignetteSettings settings;   // normalized, read once so both eyes agree
};
// Once per submitted frame, from the OpenXR submit thread only: what to draw with, both eyes alike.
// Reads the scene tier and the settings, and advances the fade.
VignetteFrame SampleCutsceneVignette();

struct VignetteStatus {
    int engineTier = 0;     // the player's scene tier as last read, 0 when stale
    bool tierFresh = false;
    bool wanted = false;    // this frame wants the vignette (before the fade)
    bool excluded = false;  // a braindance or a device camera is holding it off
    float amount = 0.0f;    // eased fade as last drawn, 0..1
};
// Any thread; for the settings panel.
VignetteStatus GetVignetteStatus();

}  // namespace cvr::comfort
