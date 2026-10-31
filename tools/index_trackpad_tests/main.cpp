// Valve Index trackpad menu presses (include/Runtimes/IndexTrackpadInput.hpp): the bound paths and action
// types against the OpenXR registry's own Index profile (one unknown path would make the runtime refuse
// every Index binding), the press detector, the per-hand button mapping and the action lifecycle.
//   index_trackpad_tests <path to xr.xml>
#include "Runtimes/IndexTrackpadInput.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cvr::input;
static void Check(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }

// ---- the registry ----------------------------------------------------------------------------------
struct Component { std::string type; bool system = false; };
static std::map<std::string, Component> IndexComponents(const std::string& registry, std::vector<std::string>& users) {
    std::ifstream file(registry);
    Check(file.good(), "cannot read the OpenXR registry: " + registry);
    std::stringstream all; all << file.rdbuf();
    const std::string text = all.str();
    const std::string open = std::string("<interaction_profile name=\"") + IndexProfile + "\"";
    const auto begin = text.find(open);
    Check(begin != std::string::npos, "the registry has no Index profile named " + std::string(IndexProfile));
    const auto end = text.find("</interaction_profile>", begin);
    const std::string block = text.substr(begin, end - begin);
    auto attribute = [](const std::string& line, const std::string& name) {
        const auto at = line.find(name + "=\"");
        if (at == std::string::npos) return std::string();
        const auto from = at + name.size() + 2;
        return line.substr(from, line.find('"', from) - from);
    };
    std::map<std::string, Component> components;
    std::istringstream lines(block);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("<user_path") != std::string::npos) users.push_back(attribute(line, "path"));
        if (line.find("<component") == std::string::npos) continue;
        components[attribute(line, "subpath")] = {attribute(line, "type"), attribute(line, "system") == "true"};
    }
    return components;
}

static void Registry(const std::string& registry) {
    std::vector<std::string> users;
    const auto components = IndexComponents(registry, users);
    Check(users == std::vector<std::string>{"/user/hand/left", "/user/hand/right"}, "unexpected Index user paths");
    // Why this exists at all: the system button is the system's, and the pad has no click of its own.
    Check(components.count("/input/system/click") && components.at("/input/system/click").system,
          "system/click is no longer reserved -- the trackpad substitute may be unnecessary");
    Check(!components.count("/input/trackpad/click"), "the Index profile gained a trackpad click; use it instead of force");

    // Every path the trackpad actions are bound on exists, on the right hand, with the action's type.
    IndexTrackpadActions actions;
    std::map<XrAction, XrActionType> types;
    uintptr_t next = 1;
    actions.Create([&](XrAction& action, XrActionType type, const char*, const char*, bool perHand) {
        Check(perHand, "trackpad actions must be per hand");
        action = reinterpret_cast<XrAction>(next++);
        types[action] = type;
    });
    const auto typeName = [](XrActionType type) {
        switch (type) {
            case XR_ACTION_TYPE_BOOLEAN_INPUT: return std::string("XR_ACTION_TYPE_BOOLEAN_INPUT");
            case XR_ACTION_TYPE_FLOAT_INPUT: return std::string("XR_ACTION_TYPE_FLOAT_INPUT");
            case XR_ACTION_TYPE_VECTOR2F_INPUT: return std::string("XR_ACTION_TYPE_VECTOR2F_INPUT");
            default: return std::string("other");
        }
    };
    struct Bind { XrAction action; const char* path; };
    std::vector<Bind> bindings;
    actions.AppendBindings<Bind>(bindings);
    Check(bindings.size() == 6, "expected three bindings per hand");
    for (size_t i = 0; i < bindings.size(); ++i) {
        const std::string path = bindings[i].path;
        const std::string hand = i < 3 ? "/user/hand/left" : "/user/hand/right";
        Check(path.rfind(hand, 0) == 0, "binding " + path + " is on the wrong hand");
        const std::string subpath = path.substr(hand.size());
        Check(components.count(subpath), "the Index profile has no " + subpath + " -- the runtime would refuse the profile");
        Check(components.at(subpath).type == typeName(types.at(bindings[i].action)),
              subpath + " is " + components.at(subpath).type + ", the action is " + typeName(types.at(bindings[i].action)));
        Check(!components.at(subpath).system, subpath + " is reserved for the system");
    }
}

// ---- the press -------------------------------------------------------------------------------------
static void Press() {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    using P = TrackpadMenuPress;
    P p;
    // A thumb resting on the pad, or anywhere without touching, is not a press.
    Check(!p.Update(false, 0.9f, 0.9f), "pressed without touching");
    Check(!p.Update(true, 0.9f, 0.20f), "a resting thumb pressed");
    Check(!p.Update(true, 0.9f, 0.30f), "pressed below the press force");
    Check(!p.Update(false, 0.0f, 0.0f), "lift");
    // A firm press in the middle of the pad counts; the boundaries themselves count.
    Check(p.Update(true, P::kLandedAbove, P::kPressForce), "a firm press at the boundary did not press");
    Check(!p.Update(false, 0.0f, 0.0f), "lift");
    Check(p.Update(true, 0.0f, 0.50f), "a firm press in the middle did not press");
    // Held while the force stays above the release threshold, wherever the thumb drifts.
    Check(p.Update(true, -0.8f, 0.25f), "a held press dropped when the thumb drifted down");
    Check(!p.Update(true, -0.8f, P::kReleaseForce), "the press did not end at the release force");
    // Eased off but still touching: pressing again counts, the thumb has not left this contact.
    Check(p.Update(true, -0.8f, 0.70f), "a second press in the same contact did not press");
    Check(!p.Update(false, -0.8f, 0.70f), "lift");
    // After a lift, a press that lands at the bottom of the pad does not count, even sliding up.
    Check(!p.Update(true, -0.8f, 0.90f), "a firm press at the bottom pressed");
    Check(!p.Update(true, 0.9f, 0.90f), "a firm press that slid up from the bottom pressed");
    Check(!p.Update(true, 0.9f, 0.30f), "still blocked between the thresholds");
    Check(!p.Update(true, 0.9f, 0.10f), "let go");
    Check(p.Update(true, 0.8f, 0.70f), "a fresh press after easing off did not press");
    // Lifting the thumb ends it, even at full force on the sensor's last sample.
    Check(!p.Update(false, 0.8f, 0.90f), "lifting did not end the press");
    // Garbage never presses, and ends a press.
    Check(p.Update(true, 0.8f, 0.70f), "re-press");
    Check(!p.Update(true, nan, 0.70f), "NaN position kept the press");
    Check(!p.Update(true, 0.8f, nan), "NaN force pressed");
    Check(p.Update(true, 0.8f, 0.70f), "did not recover after NaN");
    // Chatter: a press wavering around the press threshold stays one press.
    P w;
    int edges = 0;
    bool last = false;
    for (int f = 0; f < 200; ++f) {
        const float force = (f < 20) ? 0.0f : 0.55f + 0.2f * std::sin(f * 0.7f);   // 0.35 .. 0.75 once started
        const bool now = w.Update(true, 0.9f, force);
        if (now && !last) ++edges;
        last = now;
    }
    Check(edges == 1, "a wavering press produced " + std::to_string(edges) + " presses");
}

// The contacts measured on Index controllers under Pimax OpenXR (2026-10-31, two play sessions logged per
// thumb contact): the highest the thumb was, where it read at peak force, and the peak force. Replayed
// as landing -> half force -> peak, the way a press slides down as the force rises.
static void Measured() {
    struct Contact { float landedY, yAtPeak, peak; };
    // Presses: session 1 (asked to press firmly, top and middle) and session 2 (everyday attempts, which
    // the upper-half rule at 0.45 force missed: "kind of hard to trigger").
    const Contact presses[] = {
        {0.34f, 0.01f, 1.00f}, {0.30f, -0.07f, 1.00f}, {0.52f, 0.11f, 1.00f}, {0.41f, 0.12f, 1.00f},
        {0.41f, -0.03f, 0.59f}, {0.24f, -0.09f, 0.69f}, {-0.02f, -0.14f, 1.00f}, {-0.12f, -0.20f, 0.93f},
        {-0.09f, -0.25f, 0.48f}, {-0.18f, -0.28f, 0.78f}, {-0.30f, -0.38f, 0.89f},
        {-0.02f, -0.18f, 0.53f}, {-0.01f, -0.24f, 0.45f}, {0.41f, 0.17f, 0.43f}, {-0.07f, -0.07f, 0.36f},
        {-0.04f, -0.06f, 0.42f}};
    // The bottom of the pad, left alone however firm.
    const Contact bottom[] = {{-0.56f, -0.57f, 1.00f}, {-0.75f, -0.75f, 0.32f}};
    // A thumb resting on the pad, both sessions: never above 0.24.
    const Contact resting[] = {{0.26f, 0.12f, 0.00f}, {0.31f, 0.31f, 0.09f}, {0.41f, 0.41f, 0.08f},
                               {0.28f, 0.18f, 0.21f}, {0.33f, 0.32f, 0.06f}, {0.42f, 0.12f, 0.18f},
                               {0.40f, 0.40f, 0.15f}, {0.28f, 0.16f, 0.24f}, {0.57f, 0.36f, 0.15f}};
    auto replay = [](const Contact& c) {
        TrackpadMenuPress p;
        bool pressed = false;
        pressed |= p.Update(true, c.landedY, 0.05f);
        pressed |= p.Update(true, 0.5f * (c.landedY + c.yAtPeak), 0.5f * c.peak);
        pressed |= p.Update(true, c.yAtPeak, c.peak);
        p.Update(false, c.yAtPeak, 0.0f);
        return pressed;
    };
    for (const auto& c : presses)
        Check(replay(c), "a measured press did not press (landed " + std::to_string(c.landedY) + ", force " + std::to_string(c.peak) + ")");
    for (const auto& c : bottom) Check(!replay(c), "a measured press at the bottom pressed (landed " + std::to_string(c.landedY) + ")");
    for (const auto& c : resting) Check(!replay(c), "a measured resting thumb pressed (force " + std::to_string(c.peak) + ")");
}

// ---- the buttons and the lifecycle -----------------------------------------------------------------
static void Buttons() {
    IndexTrackpadActions actions;
    unsigned reads = 0;
    auto vec2 = [&](XrAction, float& x, float& y) { ++reads; x = 0; y = 1; };
    auto readFloat = [&](XrAction) { ++reads; return 1.0f; };
    auto readBool = [&](XrAction) { ++reads; return true; };
    // Not created (input actions off, or before init): nothing is read and nothing is pressed.
    Check(actions.UpdateHand(0, vec2, readFloat, readBool) == 0 && actions.UpdateHand(1, vec2, readFloat, readBool) == 0 &&
          reads == 0, "uncreated actions were read or pressed");
    struct Bind { XrAction action; const char* path; };
    std::vector<Bind> none;
    actions.AppendBindings<Bind>(none);
    Check(none.empty(), "uncreated actions were bound");

    uintptr_t next = 1;
    actions.Create([&](XrAction& action, XrActionType, const char*, const char*, bool) { action = reinterpret_cast<XrAction>(next++); });
    Check(actions.Created(), "Create left an action null");
    // Per hand: left is Back, right is Start, and the two are independent.
    float y[2] = {0, 0}, force[2] = {0, 0};
    bool touching[2] = {false, false};
    auto run = [&](int hand) {
        return actions.UpdateHand(hand,
            [&](XrAction a, float& outX, float& outY) { Check(a == actions.position, "position read on the wrong action"); outX = 0; outY = y[hand]; },
            [&](XrAction a) { Check(a == actions.force, "force read on the wrong action"); return force[hand]; },
            [&](XrAction a) { Check(a == actions.touch, "touch read on the wrong action"); return touching[hand]; });
    };
    Check(run(0) == 0 && run(1) == 0, "idle pads pressed");
    touching[0] = true; y[0] = 0.9f; force[0] = 0.8f;
    Check(run(0) == 0x0020 && run(1) == 0, "the left pad is not Back alone");
    touching[1] = true; y[1] = 0.9f; force[1] = 0.8f;
    Check(run(0) == 0x0020 && run(1) == 0x0010, "the right pad is not Start");
    force[0] = 0.1f;
    Check(run(0) == 0 && run(1) == 0x0010, "releasing the left pad released the right");
    reads = 0;
    Check(actions.UpdateHand(2, vec2, readFloat, readBool) == 0 && actions.UpdateHand(-1, vec2, readFloat, readBool) == 0 &&
          reads == 0, "an invalid hand was read");
    // Shutdown clears the handles and any held press.
    actions = {};
    Check(!actions.Created() && !actions.press[0].held && !actions.press[1].held, "shutdown kept handles or a held press");
    reads = 0;
    Check(actions.UpdateHand(1, vec2, readFloat, readBool) == 0 && reads == 0, "shut-down actions were read");
}

int main(int argc, char** argv) try {
    Check(argc > 1, "usage: index_trackpad_tests <xr.xml>");
    Registry(argv[1]);
    Press();
    Measured();
    Buttons();
    std::cout << "PASS Index trackpad paths and types against the registry, press detection, per-hand Back/Start, lifecycle\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
