// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_movement_override.cpp
 * @brief Package D of docs/devel/plans/2026-09-20-bondtech-indx.md: per-printer
 * Select/Park command overrides (D3, plan §7.1).
 *
 * Covers:
 *  - toolchanger_addon::resolve_tool_movement_override()'s tri-state Auto /
 *    valid explicit choice / invalid explicit choice, distinct from
 *    StandardMacroInfo::missing_macro's fallback-for-standard-ops behavior;
 *  - an explicit override outranking the automatic T<n>/CHANGE_TOOL/PARK_TOOL
 *    choice, sent under the fixed contract "MACRO TOOL=<n>" / bare "MACRO";
 *  - a stored choice this printer no longer reports resolving to zero sends,
 *    never a silent fall-through to the automatic command;
 *  - Auto restoring the automatic default exactly as package B implemented it
 *    (regression against test_ams_toolchanger_indx_backend.cpp's baseline);
 *  - the settings surface (get_device_sections/get_device_actions/
 *    execute_device_action) exposing and persisting the two choices.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "toolchanger_addon.h"
#include "ui_update_queue.h"

#include <algorithm>
#include <any>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::toolchanger_addon::ToolCommands;
using helix::toolchanger_addon::ToolMovementOverride;
using Choice = ToolMovementOverride::Choice;

namespace {

/// Same minimal INDX-shaped ToolCommands package B's backend test builds by
/// hand — every configured tool has a working T<n> shortcut, plus the
/// verified CHANGE_TOOL fallback and PARK_TOOL.
ToolCommands indx_commands(int tool_count) {
    ToolCommands c;
    c.present = true;
    c.provider_name = "INDX";
    c.select_prefix = "T";
    c.unselect = "PARK_TOOL";
    c.select_shortcut_available.assign(static_cast<size_t>(tool_count), true);
    c.change_tool_macro = "CHANGE_TOOL";
    return c;
}

std::vector<std::string> discovered_tools(int count) {
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        names.push_back(std::to_string(i));
    }
    return names;
}

/// A real backend with gcode captured. client_/api_ are null, so
/// ensure_homed_then() routes straight to execute_gcode() and the new
/// paused/homed precondition (which requires a live api_) never engages —
/// that precondition is covered separately in
/// test_ams_toolchanger_indx_paused_homing.cpp-shaped cases below via the
/// tri-state override alone.
class MovementHelper : public LVGLTestFixture, public AmsBackendToolChanger {
  public:
    explicit MovementHelper(int tool_count)
        : LVGLTestFixture(), AmsBackendToolChanger(nullptr, nullptr) {
        set_discovered_tools(discovered_tools(tool_count));
        running_ = true;
    }

    ~MovementHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent_.push_back(gcode);
        // Must fire on_complete() (matches test_ams_toolchanger_indx_backend.cpp's
        // CapturingBackend): dispatch_operation()'s on_complete queues
        // finalize_dispatch_after_macro() through token.defer(), which is what
        // resolves the optimistic action back to IDLE. Skipping this leaves
        // is_busy() latched forever, refusing every later op in the same test.
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

/// A printer exposing exactly one plausible movement macro besides the
/// INDX-style T<n>/CHANGE_TOOL/PARK_TOOL vocabulary. macros() stores names
/// uppercased (PrinterDiscovery::parse_objects()), so callers pass the name
/// already in the case they expect resolve_tool_movement_override() to match.
PrinterDiscovery discovery_with_macro(const std::string& macro_name) {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "gcode_macro " + macro_name}));
    return hw;
}

} // namespace

// =============================================================================
// resolve_tool_movement_override(): the tri-state itself
// =============================================================================

TEST_CASE("Movement override: default settings resolve to Auto for both directions",
          "[indx][dispatch]") {
    auto hw = discovery_with_macro("MY_SELECT_TOOL");
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw);

    CHECK(ov.select_choice == Choice::kAuto);
    CHECK(ov.park_choice == Choice::kAuto);
    CHECK(ov.select_choice_raw == toolchanger_addon::kAutoMacro);
    CHECK(ov.park_choice_raw == toolchanger_addon::kAutoMacro);
}

TEST_CASE("Movement override: an explicit choice naming a real macro is valid",
          "[indx][dispatch]") {
    auto hw = discovery_with_macro("MY_SELECT_TOOL");
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw, "MY_SELECT_TOOL", "auto");

    REQUIRE(ov.select_choice == Choice::kValid);
    CHECK(ov.select_macro == "MY_SELECT_TOOL");
    CHECK(ov.park_choice == Choice::kAuto);
}

TEST_CASE("Movement override: a stored choice this printer does not report is invalid",
          "[indx][dispatch]") {
    auto hw = discovery_with_macro("MY_SELECT_TOOL");
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw, "GHOST_MACRO", "auto");

    CHECK(ov.select_choice == Choice::kInvalid);
    CHECK(ov.select_macro.empty());
    // The raw (invalid) name is retained for the settings UI to show the
    // user what they configured, distinct from the resolved macro to send.
    CHECK(ov.select_choice_raw == "GHOST_MACRO");
}

TEST_CASE("Movement override: macro_options lists Auto plus plausible candidates",
          "[indx][dispatch]") {
    auto hw = discovery_with_macro("CUSTOM_PARK_TOOL");
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw);

    REQUIRE_FALSE(ov.macro_options.empty());
    CHECK(ov.macro_options.front() == toolchanger_addon::kAutoMacro);
    CHECK(std::find(ov.macro_options.begin(), ov.macro_options.end(), "CUSTOM_PARK_TOOL") !=
         ov.macro_options.end());
}

// =============================================================================
// Explicit override outranks the automatic command (§7.1)
// =============================================================================

TEST_CASE("Movement override: a valid Select override outranks the T<n> shortcut",
          "[indx][dispatch]") {
    MovementHelper h(4);
    h.set_tool_commands(indx_commands(4));
    ToolMovementOverride ov;
    ov.select_choice = Choice::kValid;
    ov.select_macro = "MY_CHANGE";
    h.set_tool_movement_override(ov);

    auto err = h.change_tool(2);
    REQUIRE(err.success());
    REQUIRE(h.sent().size() == 2);
    CHECK(h.sent().front() == "G28");
    CHECK(h.sent().back() == "MY_CHANGE TOOL=2");
}

TEST_CASE("Movement override: an invalid Select override sends nothing",
          "[indx][dispatch]") {
    MovementHelper h(4);
    h.set_tool_commands(indx_commands(4));
    ToolMovementOverride ov;
    ov.select_choice = Choice::kInvalid;
    h.set_tool_movement_override(ov);

    auto err = h.change_tool(2);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
}

TEST_CASE("Movement override: Auto restores the automatic T<n>/CHANGE_TOOL choice",
          "[indx][dispatch]") {
    // Regression against package B's baseline (test_ams_toolchanger_indx_backend.cpp):
    // an explicit override must not disturb the default path when unset.
    MovementHelper h(4);
    h.set_tool_commands(indx_commands(4));
    // set_tool_movement_override() never called -> default-constructed Auto.

    auto err = h.change_tool(2);
    REQUIRE(err.success());
    REQUIRE_FALSE(h.sent().empty());
    CHECK(h.sent().back() == "T2");
}

TEST_CASE("Movement override: a valid Park override outranks PARK_TOOL", "[indx][dispatch]") {
    MovementHelper h(4);
    h.set_tool_commands(indx_commands(4));
    REQUIRE(h.change_tool(1).success());
    // finalize_dispatch_after_macro() resolves through UpdateQueue (token.defer()),
    // not synchronously -- drain before the dependent unload or IDLE never lands
    // and is_busy() refuses it.
    helix::ui::UpdateQueue::instance().drain();

    ToolMovementOverride ov;
    ov.park_choice = Choice::kValid;
    ov.park_macro = "MY_PARK";
    h.set_tool_movement_override(ov);

    auto err = h.unload_filament(1);
    REQUIRE(err.success());
    REQUIRE(h.sent().size() == 3);
    CHECK(h.sent()[1] == "G28");
    // Bare macro -- a parking override takes no argument (§7.1).
    CHECK(h.sent().back() == "MY_PARK");
}

TEST_CASE("Movement override: an invalid Park override sends nothing", "[indx][dispatch]") {
    MovementHelper h(4);
    h.set_tool_commands(indx_commands(4));
    REQUIRE(h.change_tool(1).success());
    helix::ui::UpdateQueue::instance().drain();
    ToolMovementOverride ov;
    ov.park_choice = Choice::kInvalid;
    h.set_tool_movement_override(ov);

    auto err = h.unload_filament(1);
    CHECK_FALSE(err.success());
    // Only the earlier change_tool's T1 is in the log -- no PARK_TOOL leaked.
    CHECK(h.sent().size() == 1);
}

TEST_CASE("Movement override: Auto park still uses PARK_TOOL when set unconditionally",
          "[indx][dispatch]") {
    MovementHelper h(2);
    h.set_tool_commands(indx_commands(2));
    REQUIRE(h.change_tool(0).success());
    helix::ui::UpdateQueue::instance().drain();

    auto err = h.unload_filament(0);
    REQUIRE(err.success());
    CHECK(h.sent().back() == "PARK_TOOL");
}

// =============================================================================
// Ordinary (non-INDX) tool changer regression: no override applies
// =============================================================================

TEST_CASE("Movement override: a plain klipper-toolchanger is unaffected", "[indx][dispatch]") {
    // tool_commands_ stays absent -- the override struct is never consulted
    // (both branches in do_change_tool()/do_unload_filament() are gated on
    // tool_commands_.present).
    MovementHelper h(2);
    ToolMovementOverride ov;
    ov.select_choice = Choice::kValid;
    ov.select_macro = "SHOULD_NOT_BE_USED";
    h.set_tool_movement_override(ov);

    auto err = h.change_tool(1);
    REQUIRE(err.success());
    CHECK(h.sent().back() == "SELECT_TOOL T=1");
}

// =============================================================================
// Settings surface: sections, actions, execute_device_action
// =============================================================================

TEST_CASE("Movement override: device actions expose select/park dropdowns when candidates exist",
          "[indx][dispatch]") {
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(discovery_with_macro("CUSTOM_TOOL_MACRO")));

    auto sections = h.get_device_sections();
    REQUIRE_FALSE(sections.empty());
    CHECK(std::any_of(sections.begin(), sections.end(),
                      [](const auto& s) { return s.id == "tool_commands"; }));

    auto actions = h.get_device_actions();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].id == "tool_select_macro");
    CHECK(actions[1].id == "tool_park_macro");
    CHECK(actions[0].type == helix::printer::ActionType::DROPDOWN);
    CHECK_FALSE(actions[0].options.empty());
}

TEST_CASE("Movement override: a plain tool changer with no candidate macros exposes no rows",
          "[indx][dispatch]") {
    MovementHelper h(2);
    h.set_tool_commands(indx_commands(2));
    // No set_tool_movement_override() call -> macro_options stays empty.
    CHECK(h.get_device_actions().empty());
    CHECK(h.get_device_sections().empty());
}

TEST_CASE("Movement override: execute_device_action resolves and persists a valid choice",
          "[indx][dispatch]") {
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(discovery_with_macro("CUSTOM_TOOL_MACRO")));

    auto err = h.execute_device_action("tool_select_macro", std::any(std::string("CUSTOM_TOOL_MACRO")));
    REQUIRE(err.success());
    CHECK(helix::SettingsManager::instance().get_tool_select_macro() == "CUSTOM_TOOL_MACRO");

    // The change takes effect immediately -- no restart/re-resolve needed.
    auto tc_err = h.change_tool(1);
    REQUIRE(tc_err.success());
    CHECK(h.sent().back() == "CUSTOM_TOOL_MACRO TOOL=1");

    // Restore the singleton default so this test does not leak into others.
    helix::SettingsManager::instance().set_tool_select_macro("auto");
}

TEST_CASE("Movement override: execute_device_action rejects an unknown macro name",
          "[indx][dispatch]") {
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(discovery_with_macro("CUSTOM_TOOL_MACRO")));

    // Not among the resolved macro_options -- an unrecognized choice reaching
    // execute_device_action() through anything other than the offered
    // dropdown values still resolves to Invalid, not a silent accept.
    auto err = h.execute_device_action("tool_park_macro", std::any(std::string("NOT_A_REAL_MACRO")));
    REQUIRE(err.success()); // the SETTING is accepted and persisted...
    auto park_err = h.unload_filament(0);
    // ...but dispatch of the now-invalid choice sends nothing.
    CHECK_FALSE(park_err.success());

    helix::SettingsManager::instance().set_tool_park_macro("auto");
}
