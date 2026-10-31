// The cutscene comfort vignette's running state -- see include/Runtimes/CutsceneVignette.hpp for what
// it is and why it is drawn where it is. This file only answers "how much, this frame": it reads the
// player's scene tier (published by the locate path, Hooks/LocateCamera.cpp), the two interactive
// camera modes that must keep their periphery, and the settings, and it advances the fade.
//
// THREADS. SampleCutsceneVignette() is called once per submitted frame by the OpenXR submit thread,
// and by nothing else, so the fade itself needs no lock. The status the settings panel reads is
// published through relaxed atomics: each field is independent and a panel one frame behind is fine.
#include "Runtimes/CutsceneVignette.hpp"

#include "Camera/CameraState.hpp"
#include "Core/LiveControls.hpp"

#include <RED4ext/Scripting/Natives/Generated/GameplayTier.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>

static_assert(cvr::comfort::kEngineTierFullGameplay == static_cast<int>(RED4ext::GameplayTier::Tier1_FullGameplay),
              "the vignette's tier gate assumes the SDK's GameplayTier numbering");
static_assert(cvr::comfort::kEngineTierCinematic == static_cast<int>(RED4ext::GameplayTier::Tier5_Cinematic),
              "the vignette's tier gate assumes the SDK's GameplayTier numbering");

namespace cvr::comfort {
namespace {
using Clock = std::chrono::steady_clock;

// Submit thread only.
float g_fade = 0.0f;
Clock::time_point g_lastSample{};
bool g_sampled = false;

// Published for the panel.
std::atomic<int> g_statusTier{0};
std::atomic<bool> g_statusFresh{false};
std::atomic<bool> g_statusWanted{false};
std::atomic<bool> g_statusExcluded{false};
std::atomic<float> g_statusAmount{0.0f};

VignetteSettings CurrentSettings() {
    VignetteSettings s;
    s.tier = g_liveControls.xrCutsceneVignetteTier;
    s.strength = g_liveControls.xrCutsceneVignetteStrength;
    s.size = g_liveControls.xrCutsceneVignetteSize;
    return NormalizeVignette(s);
}
}  // namespace

VignetteFrame SampleCutsceneVignette() {
    const auto now = Clock::now();
    const float dt = g_sampled ? std::chrono::duration<float>(now - g_lastSample).count() : 0.0f;
    g_lastSample = now;
    g_sampled = true;

    VignetteInputs in;
    in.engineTier = g_sceneTier.load(std::memory_order_relaxed);
    const uint64_t stamp = g_sceneTierStampMs.load(std::memory_order_relaxed);
    const uint64_t tick = GetTickCount64();
    in.tierAgeMs = stamp != 0 && tick >= stamp ? tick - stamp : UINT64_MAX;
    in.braindance = g_bdActive.load(std::memory_order_relaxed) != 0;
    in.deviceCamera = DeviceCamActive();

    const VignetteSettings settings = CurrentSettings();
    const bool want = WantsVignette(in, settings.tier);
    g_fade = StepVignetteFade(g_fade, want, dt);
    const float amount = settings.strength > 0.0f ? EaseVignetteFade(g_fade) : 0.0f;

    const bool fresh = in.tierAgeMs <= kSceneTierStaleMs;
    g_statusTier.store(fresh ? in.engineTier : 0, std::memory_order_relaxed);
    g_statusFresh.store(fresh, std::memory_order_relaxed);
    g_statusWanted.store(want, std::memory_order_relaxed);
    g_statusExcluded.store(fresh && (in.braindance || in.deviceCamera) &&
                               SceneTierWantsVignette(in.engineTier, settings.tier),
                           std::memory_order_relaxed);
    g_statusAmount.store(amount, std::memory_order_relaxed);
    return {amount, settings};
}

VignetteStatus GetVignetteStatus() {
    VignetteStatus s;
    s.engineTier = g_statusTier.load(std::memory_order_relaxed);
    s.tierFresh = g_statusFresh.load(std::memory_order_relaxed);
    s.wanted = g_statusWanted.load(std::memory_order_relaxed);
    s.excluded = g_statusExcluded.load(std::memory_order_relaxed);
    s.amount = g_statusAmount.load(std::memory_order_relaxed);
    return s;
}

}  // namespace cvr::comfort
