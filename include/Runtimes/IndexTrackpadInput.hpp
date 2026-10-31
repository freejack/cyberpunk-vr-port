#pragma once
#include <openxr/openxr.h>
#include <cmath>
#include <cstdint>

namespace cvr::input {
// THE INDEX HAS NO MENU BUTTON THE GAME CAN SEE. The only menu-like input in its profile is
// /input/system/click, which the OpenXR registry marks system="true": SteamVR keeps it for its
// dashboard and the application never receives a press. Cyberpunk needs two -- Start (the Hub and
// map with the default bindings) and Back/View (the pause menu) -- so a firm press on each trackpad
// becomes one, placed as on a gamepad: LEFT = Back, RIGHT = Start. The layout is VirtualyMichael's
// (PR #105), confirmed by an Index owner on that build.
inline constexpr char IndexProfile[]="/interaction_profiles/valve/index_controller";

// The pad has no click in this profile, only position, force and touch, so a press is decided here.
//
// Measured on Index controllers (Pimax OpenXR, 2026-10-31, two play sessions logged per thumb contact):
//   * A PRESS PULLS THE REPORTED POSITION DOWN. A thumb landing at y 0.24..0.52 read -0.09..0.12 at peak
//     force, 0.3..0.4 lower -- the press flattens the thumb towards the palm. So where the press is, is
//     judged by the highest the thumb has been since it touched down, not by where it ends up.
//   * EVERYDAY PRESSES LAND IN THE MIDDLE AND ARE LIGHTER than presses made to order: aimed at "the
//     pad", they landed at y -0.07..0.41 with 0.36..0.53 force (against 0.6..1.0 when asked to press
//     firmly). A thumb RESTING on the pad never went over 0.24 force in either session.
// So a press is force alone, anywhere but the bottom of the pad (kLandedAbove: the lowest contacts,
// -0.56..-0.76, are left alone, and nothing else uses the pad).
//
// It STARTS when the force rises past kPressForce with the thumb having landed above kLandedAbove. A
// firm press that landed at the bottom does not count, even if it then slides up (blocked until eased
// off). It ENDS at or under kReleaseForce, or when the thumb lifts. The gap between the force
// thresholds keeps a wavering press from chattering.
struct TrackpadMenuPress {
    static constexpr float kLandedAbove = -0.40f;  // pad y, -1 bottom .. +1 top: all but the bottom part
    static constexpr float kPressForce = 0.35f;    // a deliberate press; a resting thumb stayed at or under 0.24
    static constexpr float kReleaseForce = 0.20f;
    bool held = false;
    bool blocked = false;   // pressing firmly, but the thumb landed at the bottom of the pad
    float topY = -2.0f;     // highest y since the thumb touched down; reset when it lifts

    bool Update(bool touching, float y, float force) {
        if (!std::isfinite(y) || !std::isfinite(force)) touching = false;
        if (!touching) {
            held = blocked = false;
            topY = -2.0f;
            return false;
        }
        if (y > topY) topY = y;
        if (held) {
            held = force > kReleaseForce;
            return held;
        }
        if (force <= kReleaseForce) {
            blocked = false;
            return false;
        }
        if (force >= kPressForce) {
            if (!blocked && topY >= kLandedAbove) held = true;
            else blocked = true;
        }
        return held;
    }
};

struct IndexTrackpadActions {
    XrAction position{}, force{}, touch{};
    TrackpadMenuPress press[2];   // [0] left, [1] right -- the order of m_handPaths

    static constexpr uint16_t kBack = 0x0020;    // XINPUT_GAMEPAD_BACK
    static constexpr uint16_t kStart = 0x0010;   // XINPUT_GAMEPAD_START
    static constexpr uint16_t HandButton(int hand) { return hand == 0 ? kBack : kStart; }

    // The paths each hand's actions are suggested on, checked against the registry by
    // tools/index_trackpad_tests: one path the runtime does not know rejects the whole profile.
    static constexpr const char* kPositionPath[2] = {"/user/hand/left/input/trackpad", "/user/hand/right/input/trackpad"};
    static constexpr const char* kForcePath[2] = {"/user/hand/left/input/trackpad/force", "/user/hand/right/input/trackpad/force"};
    static constexpr const char* kTouchPath[2] = {"/user/hand/left/input/trackpad/touch", "/user/hand/right/input/trackpad/touch"};

    template<class MakeAction> void Create(MakeAction&& make) {
        make(position, XR_ACTION_TYPE_VECTOR2F_INPUT, "index_trackpad", "Index Trackpad", true);
        make(force, XR_ACTION_TYPE_FLOAT_INPUT, "index_trackpad_force", "Index Trackpad Press", true);
        make(touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "index_trackpad_touch", "Index Trackpad Touch", true);
    }
    bool Created() const {
        return position != XR_NULL_HANDLE && force != XR_NULL_HANDLE && touch != XR_NULL_HANDLE;
    }

    // Appends both hands' trackpad bindings to a profile's suggestion list (Bind = {action, path}).
    template<class Bind, class List> void AppendBindings(List& list) const {
        if (!Created()) return;
        for (int hand = 0; hand < 2; ++hand) {
            list.push_back(Bind{position, kPositionPath[hand]});
            list.push_back(Bind{force, kForcePath[hand]});
            list.push_back(Bind{touch, kTouchPath[hand]});
        }
    }

    // One hand, once per input poll, with the frame loop's per-hand readers. Returns the gamepad
    // button that hand's press holds down (Back left, Start right), or 0. On any other controller the
    // actions are simply inactive: the readers return zero and nothing is pressed.
    template<class ReadVec2, class ReadFloat, class ReadBool>
    uint16_t UpdateHand(int hand, ReadVec2&& readVec2, ReadFloat&& readFloat, ReadBool&& readBool) {
        if (hand < 0 || hand > 1) return 0;
        if (!Created()) {
            press[hand] = {};
            return 0;
        }
        float x = 0.0f, y = 0.0f;
        readVec2(position, x, y);
        return press[hand].Update(readBool(touch), y, readFloat(force)) ? HandButton(hand) : uint16_t(0);
    }
};
}
