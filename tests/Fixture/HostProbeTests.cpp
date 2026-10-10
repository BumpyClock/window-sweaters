// SPDX-License-Identifier: GPL-3.0-only
#include "HostProbe.h"

#include <iostream>
#include <string>

namespace
{
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const std::string kProducerSample = R"({
  "host_pid": 4242,
  "host_hwnd": 1311000,
  "overlay_hwnd": 2622000,
  "target_hwnd": 3933000,
  "target_pid": 777,
  "target_process_start": 133735000000000000,
  "target_generation": 3,
  "target_bounds": [-1280, -12, 640, 480],
  "overlay_bounds": [-1296, -28, 656, 496],
  "overlay_visible": false,
  "stopped": false,
  "tracked_entries": 1,
  "creates": 1, "removes": 0,
  "geometry_updates": 5, "draws": 2,
  "pure_moves": 4, "hides": 0,
  "shows": 1, "reorders": 6,
  "stale_actions": 0, "placement_failures": 0,
  "configuration_revision": 9, "paused": false,
  "design_id": "fair-isle", "chart_id": "",
  "color_rgb": [12, 34, 56], "render_kind": "native-knit",
  "composition": "Four narrow BGRA perimeter surfaces / DirectComposition / layered alpha 255 + transparent",
  "tracker_callbacks": 18446744073709551615, "tracker_coalesced": 2,
  "tracker_uncertain_queries": 0, "tracker_error": 0,
  "border_width_pixels": 16,
  "preview_mode": false, "targets": [
    {"hwnd":3933000,"pid":777,"chart_id":"\"quoted\\\/","base_argb":-1.5e3,"overlay_hwnd":9,"overlay_visible":true,"nested":{"a":[null,{}]}}
  ]
}
)";

std::string Replace(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    Check(position != std::string::npos, "Test mutation anchor exists");
    if (position != std::string::npos)
    {
        text.replace(position, from.size(), to);
    }
    return text;
}

std::string Compact(std::string_view text)
{
    std::string compact;
    bool inString = false;
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char current = text[index];
        if (inString && current == '\\')
        {
            compact += current;
            compact += text[++index];
            continue;
        }
        if (current == '"')
        {
            inString = !inString;
        }
        if (inString || (current != ' ' && current != '\n' && current != '\r' && current != '\t'))
        {
            compact += current;
        }
    }
    return compact;
}

void Reject(const std::string& json, const char* message)
{
    Check(!ParseHostSnapshot(json).has_value(), message);
}

void ProducerSampleTest()
{
    const auto parsed = ParseHostSnapshot(kProducerSample);
    Check(parsed.has_value(), "Pretty-printed producer sample parses");
    if (!parsed)
    {
        return;
    }
    const HostSnapshot& snapshot = *parsed;
    Check(snapshot.hostProcessId == 4242 && snapshot.targetProcessId == 777, "Process ids are exact");
    Check(snapshot.hostWindow == reinterpret_cast<HWND>(uintptr_t{1311000}) &&
        snapshot.targetWindow == reinterpret_cast<HWND>(uintptr_t{3933000}), "Decimal HWND values are exact");
    Check(snapshot.overlayWindow == reinterpret_cast<HWND>(uintptr_t{2622000}),
        "Nested target overlay_hwnd does not replace the root value");
    Check(!snapshot.overlayVisible, "Nested target overlay_visible does not replace the root value");
    Check(snapshot.targetProcessStart == 133735000000000000ull && snapshot.targetGeneration == 3,
        "Identity counters keep full 64-bit precision");
    Check(snapshot.targetBounds.left == -1280 && snapshot.targetBounds.top == -12 &&
        snapshot.targetBounds.right == 640 && snapshot.targetBounds.bottom == 480,
        "Signed target coordinates are preserved");
    Check(snapshot.overlayBounds.left == -1296 && snapshot.overlayBounds.bottom == 496, "Overlay bounds are preserved");
    Check(snapshot.trackerCallbacks == 18446744073709551615ull, "Maximum uint64 counter is accepted");
    Check(snapshot.reorders == 6 && snapshot.pureMoves == 4 && snapshot.borderPaddingPixels == 16,
        "Counters on shared lines are read by key");

    const auto compact = ParseHostSnapshot(Compact(kProducerSample));
    Check(compact.has_value() && compact->overlayWindow == snapshot.overlayWindow &&
        compact->trackerCallbacks == snapshot.trackerCallbacks && !compact->overlayVisible,
        "Compact JSON is equivalent to spaced JSON");
    const auto visible = ParseHostSnapshot(Replace(Compact(kProducerSample),
        R"("overlay_visible":false,"stopped":false)", R"("overlay_visible":true,"stopped":false)"));
    Check(visible.has_value() && visible->overlayVisible && !visible->stopped,
        "Compact true literal directly after the colon is accepted");
}

void RejectionTest()
{
    Reject(Replace(kProducerSample, R"("overlay_visible": false)", R"("overlay_visible": null)"),
        "Null Boolean does not borrow a later literal");
    Reject(Replace(kProducerSample, R"("stopped": false)", R"("stopped": 0)"), "Numeric Boolean is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": "2")"), "Quoted counter is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": -2)"), "Negative counter is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": 2.0)"), "Fractional counter is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": 2e0)"), "Exponent counter is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": 02)"), "Leading-zero counter is rejected");
    Reject(Replace(kProducerSample, R"("draws": 2)", R"("draws": 2x)"), "Counter token must end at a delimiter");
    Reject(Replace(kProducerSample, R"("tracker_callbacks": 18446744073709551615)",
        R"("tracker_callbacks": 18446744073709551616)"), "uint64 overflow is rejected");
    Reject(Replace(kProducerSample, R"("host_pid": 4242)", R"("host_pid": 4294967296)"),
        "Process id beyond DWORD is rejected instead of truncated");
    Reject(Replace(kProducerSample, R"([-1280, -12, 640, 480])", R"([-2147483649, -12, 640, 480])"),
        "Coordinate beyond LONG is rejected");
    Reject(Replace(kProducerSample, R"([-1280, -12, 640, 480])", R"([-1280, -12, 640])"),
        "Three-coordinate rectangle is rejected");
    Reject(Replace(kProducerSample, R"([-1280, -12, 640, 480])", R"([-1280, -12, 640, 480, 1])"),
        "Five-coordinate rectangle is rejected");
    Reject(Replace(kProducerSample, R"([-1280, -12, 640, 480])", R"([-1280.5, -12, 640, 480])"),
        "Fractional coordinate is rejected");
    Reject(Replace(kProducerSample, R"("hides": 0,)", ""), "Missing required field is rejected");
    Reject(Replace(kProducerSample, R"("hides": 0,)", R"("hides": 0, "hides": 0,)"),
        "Duplicate required root field is rejected");
    // Not a raw literal: MSVC can translate \u escapes before raw-string processing.
    Reject(Replace(kProducerSample, "\"design_id\"", "\"design\\u005fid\""), "Escaped root key is rejected");
    Check(ParseHostSnapshot(Replace(kProducerSample, "\"fair-isle\"", "\"fair-\\u00e9\"")).has_value(),
        "Unicode escape in an unknown string is accepted");
    Reject(Replace(kProducerSample, "\"fair-isle\"", "\"fair-\\u00g9\""), "Malformed Unicode escape is rejected");
    Reject(Replace(kProducerSample, R"("fair-isle")", R"("fair-isle\q")"), "Invalid string escape is rejected");
    Reject(kProducerSample + "{}", "Trailing content is rejected");
    Reject(Replace(kProducerSample, R"("targets": [)", R"("targets": [,)"), "Malformed skipped array is rejected");
    std::string deep(40, '[');
    deep += std::string(40, ']');
    Reject(Replace(kProducerSample, R"("color_rgb": [12, 34, 56])", R"("color_rgb": )" + deep),
        "Unknown values have bounded nesting depth");
    Reject("", "Empty document is rejected");
    Reject("[]", "Non-object root is rejected");

    const std::string compact = Compact(kProducerSample);
    bool everyPrefixRejected = true;
    for (size_t length = 0; length < compact.size(); ++length)
    {
        everyPrefixRejected = everyPrefixRejected && !ParseHostSnapshot(std::string_view(compact).substr(0, length));
    }
    Check(everyPrefixRejected, "Every truncated document is rejected");
}
}

int main()
{
    ProducerSampleTest();
    RejectionTest();
    if (failures != 0)
    {
        std::cerr << failures << " of " << checks << " host probe parser checks failed\n";
        return 1;
    }
    std::cout << checks << " host probe parser checks passed (fixture-local; no host launched).\n";
    return 0;
}
