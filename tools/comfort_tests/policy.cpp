// The cutscene comfort vignette's policy and geometry (include/Runtimes/CutsceneVignette.hpp): which
// scene tiers want it, the settings' normalization, the fade, and the ring's placement on each eye's
// optical axis under an asymmetric FOV. Pure: no game, no GPU.
#include "Runtimes/CutsceneVignette.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace cvr::comfort;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
static constexpr float kInf = std::numeric_limits<float>::infinity();
static float Rad(float deg) { return deg * kDegToRad; }

// u (0..1, left to right) of the pixel that looks along tangent tx.
static float UOf(const VignetteShape& s, float tx) { return (tx - s.tanLeft) / (s.tanRight - s.tanLeft); }
static float VOf(const VignetteShape& s, float ty) { return (ty - s.tanUp) / (s.tanDown - s.tanUp); }

static void TierEncoding() {
    // Same stored encoding as the VRIK suspend: -1/0 off, 1..4 = Tier2+..Tier5.
    CHECK(NormalizeVignetteTier(-1) == kVignetteTierOff);
    CHECK(NormalizeVignetteTier(0) == kVignetteTierOff);
    CHECK(NormalizeVignetteTier(-7) == kVignetteTierOff);
    CHECK(NormalizeVignetteTier(3) == 3);
    CHECK(NormalizeVignetteTier(9) == 4);

    // Default Tier4+: FPP cinematics and full cinematics; never limited gameplay (takeovers run at 3).
    CHECK(!SceneTierWantsVignette(0, 3));   // Undefined
    CHECK(!SceneTierWantsVignette(1, 3));
    CHECK(!SceneTierWantsVignette(3, 3));
    CHECK(SceneTierWantsVignette(4, 3));
    CHECK(SceneTierWantsVignette(5, 3));
    CHECK(!SceneTierWantsVignette(6, 3));   // not a tier
    // Tier2+ and Tier5-only bounds.
    CHECK(!SceneTierWantsVignette(1, 1));
    CHECK(SceneTierWantsVignette(2, 1));
    CHECK(!SceneTierWantsVignette(4, 4));
    CHECK(SceneTierWantsVignette(5, 4));
    // Off is off at every tier.
    for (int t = 0; t <= 6; ++t) {
        CHECK(!SceneTierWantsVignette(t, kVignetteTierOff));
        CHECK(!SceneTierWantsVignette(t, 0));
    }
}

static void Inputs() {
    VignetteInputs in;
    in.engineTier = 4;
    in.tierAgeMs = 16;
    CHECK(WantsVignette(in, 3));
    // A tier nobody refreshed (main menu, loading) is not believed, at the boundary exactly.
    in.tierAgeMs = kSceneTierStaleMs;
    CHECK(WantsVignette(in, 3));
    in.tierAgeMs = kSceneTierStaleMs + 1;
    CHECK(!WantsVignette(in, 3));
    in.tierAgeMs = UINT64_MAX;   // never published
    CHECK(!WantsVignette(in, 3));
    // The two interactive camera modes keep their periphery even in a qualifying tier.
    in.tierAgeMs = 16;
    in.braindance = true;
    CHECK(!WantsVignette(in, 3));
    in.braindance = false;
    in.deviceCamera = true;
    CHECK(!WantsVignette(in, 3));
}

static void Settings() {
    const VignetteSettings d = NormalizeVignette({});
    CHECK(d.tier == kDefaultVignetteTier && d.strength == kDefaultVignetteStrength);
    CHECK(d.size == kDefaultVignetteSize && kDefaultVignetteSize == kVignetteSizeMedium);

    const VignetteSettings bad = NormalizeVignette({12, kNaN, 7});
    CHECK(bad.tier == 4);
    CHECK(bad.strength == kDefaultVignetteStrength);
    CHECK(bad.size == kVignetteSizeStrong);        // a hand-edited size past the end clamps

    const VignetteSettings low = NormalizeVignette({0, -1.0f, -3});
    CHECK(low.tier == kVignetteTierOff && low.strength == 0.0f && low.size == kVignetteSizeLight);
    const VignetteSettings high = NormalizeVignette({4, kInf, kVignetteSizeStrong});
    CHECK(high.strength == kDefaultVignetteStrength && high.size == kVignetteSizeStrong);
    CHECK(NormalizeVignette({4, 3.0f, 1}).strength == 1.0f);
}

static void Sizes() {
    // The three steps, as the panel labels them: 60 / 45 / 30 degrees clear, the same 20 degree ramp.
    const float clear[kVignetteSizeCount] = {60.0f, 45.0f, 30.0f};
    for (int size = 0; size < kVignetteSizeCount; ++size) {
        const VignetteShape s = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50),
            {3, kDefaultVignetteStrength, size}, 1.0f, true);
        CHECK(Near(s.innerRad, Rad(0.5f * clear[size])));
        CHECK(Near(s.outerRad, Rad(0.5f * clear[size] + 20.0f)));
        // Right at the edge of the clear view: untouched.
        CHECK(VignetteKeep(s, UOf(s, std::tan(Rad(0.5f * clear[size] - 0.5f))), 0.5f) == 1.0f);
    }
    // Each step is narrower than the one before it, at every angle the ramps overlap: stronger = darker.
    for (float deg : {20.0f, 25.0f, 30.0f, 35.0f, 40.0f, 45.0f}) {
        float keep[kVignetteSizeCount];
        for (int size = 0; size < kVignetteSizeCount; ++size) {
            const VignetteShape s = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50),
                {3, kDefaultVignetteStrength, size}, 1.0f, true);
            keep[size] = VignetteKeep(s, UOf(s, std::tan(Rad(deg))), 0.5f);
        }
        CHECK(keep[kVignetteSizeStrong] <= keep[kVignetteSizeMedium]);
        CHECK(keep[kVignetteSizeMedium] <= keep[kVignetteSizeLight]);
    }
    // The default (Medium) is clearly stronger than Light, which a headset test found too weak:
    // at 35 degrees off-axis Light has barely started while Medium is most of the way.
    const auto at35 = [](int size) {
        const VignetteShape s = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50),
            {3, kDefaultVignetteStrength, size}, 1.0f, true);
        return VignetteKeep(s, UOf(s, std::tan(Rad(35))), 0.5f);
    };
    CHECK(at35(kDefaultVignetteSize) < at35(kVignetteSizeLight) - 0.3f);
    // An out-of-range size cannot index past the table.
    const VignetteShape wild = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50), {3, 0.85f, 99}, 1.0f, true);
    CHECK(Near(wild.innerRad, Rad(15)));
}

static void Fade() {
    // Full fade in kVignetteFadeSeconds, both ways, at display rate.
    float f = 0.0f;
    int frames = 0;
    while (f < 1.0f && frames < 1000) { f = StepVignetteFade(f, true, 1.0f / 90.0f); ++frames; }
    CHECK(Near(f, 1.0f));
    CHECK(frames >= 44 && frames <= 46);   // 0.5 s at 90 Hz
    f = StepVignetteFade(f, true, 1.0f / 90.0f);
    CHECK(f == 1.0f);                        // saturates
    frames = 0;
    while (f > 0.0f && frames < 1000) { f = StepVignetteFade(f, false, 1.0f / 90.0f); ++frames; }
    CHECK(f == 0.0f && frames >= 44 && frames <= 46);

    // A hitch, a first call, or a clock going backwards cannot jump or reverse it.
    CHECK(Near(StepVignetteFade(0.0f, true, 5.0f), 0.1f / kVignetteFadeSeconds));
    CHECK(StepVignetteFade(0.5f, true, -1.0f) == 0.5f);
    CHECK(StepVignetteFade(0.5f, false, kNaN) == 0.5f);
    CHECK(StepVignetteFade(kNaN, true, 0.0f) == 0.0f);

    // The eased ramp: fixed ends and middle, monotonic, flat at both ends.
    CHECK(EaseVignetteFade(0.0f) == 0.0f && EaseVignetteFade(1.0f) == 1.0f && Near(EaseVignetteFade(0.5f), 0.5f));
    float previous = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        const float e = EaseVignetteFade(i / 100.0f);
        CHECK(e >= previous);
        previous = e;
    }
    CHECK(EaseVignetteFade(0.05f) < 0.05f);  // creeps in
}

static void SymmetricRing() {
    // A symmetric 100 x 100 degree eye, Light (60 clear, 20 soft), 85 %, linear target.
    const VignetteShape s = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50),
        {3, kDefaultVignetteStrength, kVignetteSizeLight}, 1.0f, true);
    CHECK(Near(s.innerRad, Rad(30)) && Near(s.outerRad, Rad(50)));
    CHECK(Near(s.darken, kDefaultVignetteStrength) && s.exponent == 2.2f);

    CHECK(VignetteKeep(s, 0.5f, 0.5f) == 1.0f);                                   // the axis
    CHECK(VignetteKeep(s, UOf(s, std::tan(Rad(29.5f))), 0.5f) == 1.0f);          // inside the clear view
    const float edge = VignetteKeep(s, 0.0f, 0.5f);                               // 50 deg: full
    CHECK(Near(edge, std::pow(1.0f - kDefaultVignetteStrength, 2.2f), 1e-4f));
    CHECK(VignetteKeep(s, 0.0f, 0.0f) == edge);                                   // corner: past outer, held

    // Monotonic outward along a ray, and the same at the same angle in any direction.
    float previous = 1.0f;
    for (int i = 0; i <= 50; ++i) {
        const float k = VignetteKeep(s, UOf(s, std::tan(Rad(float(i)))), 0.5f);
        CHECK(k <= previous + 1e-6f);
        previous = k;
        const float up = VignetteKeep(s, 0.5f, VOf(s, std::tan(Rad(float(i)))));
        CHECK(Near(k, up, 1e-5f));
    }
    // The ring's midpoint: perceived darkening of exactly half the strength.
    const float mid = VignetteKeep(s, UOf(s, std::tan(Rad(40))), 0.5f);
    CHECK(Near(std::pow(mid, 1.0f / 2.2f), 1.0f - 0.5f * kDefaultVignetteStrength, 1e-3f));
}

static void AsymmetricRing() {
    // A canted-style asymmetric eye: 52 degrees outward, 43 inward. The ring must be centred on the
    // optical axis (tan 0), which is NOT the middle of the image.
    // Light, so the 43 degree inner edge is still on the ramp while the 52 degree outer one is past it.
    const VignetteShape s = MakeVignetteShape(-Rad(52), Rad(43), Rad(48), -Rad(55),
        {3, kDefaultVignetteStrength, kVignetteSizeLight}, 1.0f, true);
    const float u0 = UOf(s, 0.0f), v0 = VOf(s, 0.0f);
    CHECK(std::fabs(u0 - 0.5f) > 0.02f && std::fabs(v0 - 0.5f) > 0.01f);
    CHECK(VignetteKeep(s, u0, v0) == 1.0f);
    // Equal angles either side of the AXIS darken equally...
    for (float deg : {32.0f, 36.0f, 40.0f, 42.0f}) {
        const float l = VignetteKeep(s, UOf(s, -std::tan(Rad(deg))), v0);
        const float r = VignetteKeep(s, UOf(s, std::tan(Rad(deg))), v0);
        CHECK(Near(l, r, 1e-5f));
        CHECK(l < 1.0f);
    }
    // ...while the image's own edges, which sit at different angles, do not.
    CHECK(VignetteKeep(s, 0.0f, v0) < VignetteKeep(s, 1.0f, v0));
    // Equal offsets either side of the IMAGE CENTRE are not equal: centring on 0.5 would be wrong.
    const float a = VignetteKeep(s, 0.5f - 0.4f, v0), b = VignetteKeep(s, 0.5f + 0.4f, v0);
    CHECK(!Near(a, b, 1e-3f));
}

static void NothingToDraw() {
    // Fade 0, strength 0, or an unusable FOV: darken 0, so the pass records nothing.
    CHECK(MakeVignetteShape(-1, 1, 1, -1, {}, 0.0f, true).darken == 0.0f);
    CHECK(MakeVignetteShape(-1, 1, 1, -1, {3, 0.0f, kDefaultVignetteSize}, 1.0f, true).darken == 0.0f);
    CHECK(MakeVignetteShape(1, -1, 1, -1, {}, 1.0f, true).darken == 0.0f);      // left/right swapped
    CHECK(MakeVignetteShape(-1, 1, -1, 1, {}, 1.0f, true).darken == 0.0f);      // up/down swapped
    CHECK(MakeVignetteShape(kNaN, 1, 1, -1, {}, 1.0f, true).darken == 0.0f);
    CHECK(MakeVignetteShape(-1, 1, 1, -1, {}, kNaN, true).darken == 0.0f);
    const VignetteShape off = MakeVignetteShape(-1, 1, 1, -1, {}, 0.0f, true);
    for (int i = 0; i <= 10; ++i) CHECK(VignetteKeep(off, i / 10.0f, i / 10.0f) == 1.0f);
}

static void EncodedTarget() {
    // A target that blends display-encoded values: the factor is the perceived darkening itself.
    const VignetteShape s = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50), {3, 0.6f, kVignetteSizeLight}, 1.0f, false);
    CHECK(s.exponent == 1.0f);
    CHECK(Near(VignetteKeep(s, 0.0f, 0.5f), 0.4f));
    // Half fade: half the darkening at the edge.
    const VignetteShape h = MakeVignetteShape(-Rad(50), Rad(50), Rad(50), -Rad(50), {3, 0.6f, kVignetteSizeLight}, 0.5f, false);
    CHECK(Near(VignetteKeep(h, 0.0f, 0.5f), 0.7f));
}

int main() {
    TierEncoding();
    Inputs();
    Settings();
    Sizes();
    Fade();
    SymmetricRing();
    AsymmetricRing();
    NothingToDraw();
    EncodedTarget();
    if (failures) {
        std::printf("comfort_vignette_policy: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("comfort_vignette_policy: all checks passed\n");
    return 0;
}
