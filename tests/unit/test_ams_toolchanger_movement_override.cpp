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

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "toolchanger_addon.h"

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

    /// Overridden rather than driven through the homed_axes subject: with api_
    /// null the production answer is an unconditional "homed" (no G28 is ever
    /// synthesized against a printer we cannot talk to), which would make the
    /// unhomed override path below untestable. Same shape as
    /// test_afc_delegates_homing.cpp and test_ams_toolchanger_indx_backend.cpp.
    bool toolhead_homed() const override {
        return false;
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

/// A printer exposing exactly one plausible movement macro besides the
/// INDX-style T<n>/CHANGE_TOOL/PARK_TOOL vocabulary. macros() stores names
/// uppercased (PrinterDiscovery::parse_objects()).
PrinterDiscovery discovery_with_macro(const std::string& macro_name) {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "gcode_macro " + macro_name}));
    return hw;
}

/// A printer whose macros none of tool_movement_macro_candidates()' name
/// fragments match, so both movement option lists come back empty.
PrinterDiscovery discovery_without_candidates() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "gcode_macro HEAT_SOAK"}));
    return hw;
}

/// The options one action offers, by id.
std::vector<std::string> options_for(const std::vector<helix::printer::DeviceAction>& actions,
                                     const std::string& id) {
    auto it =
        std::find_if(actions.begin(), actions.end(), [&](const auto& a) { return a.id == id; });
    return it == actions.end() ? std::vector<std::string>{} : it->options;
}

/// The value one dropdown action shows as selected, by id.
std::string value_for(const std::vector<helix::printer::DeviceAction>& actions,
                      const std::string& id) {
    auto it =
        std::find_if(actions.begin(), actions.end(), [&](const auto& a) { return a.id == id; });
    return it == actions.end() ? std::string{} : std::any_cast<std::string>(it->current_value);
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

TEST_CASE("Movement override: a stored choice matches the macro in any casing",
          "[indx][dispatch]") {
    // has_macro() is case-insensitive everywhere else in the module; a
    // hand-edited settings.json must not be the one spelling that refuses.
    auto hw = discovery_with_macro("MY_SELECT_TOOL");
    auto ov =
        toolchanger_addon::resolve_tool_movement_override(hw, "my_select_tool", "My_Select_Tool");

    REQUIRE(ov.select_choice == Choice::kValid);
    CHECK(ov.select_macro == "MY_SELECT_TOOL");
    REQUIRE(ov.park_choice == Choice::kValid);
    CHECK(ov.park_macro == "MY_SELECT_TOOL");
    // The raw spelling survives for the settings UI to echo back.
    CHECK(ov.select_choice_raw == "my_select_tool");
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

TEST_CASE("Movement override: direction options list Auto plus plausible candidates",
          "[indx][dispatch]") {
    auto hw = discovery_with_macro("CUSTOM_PARK_TOOL");
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw);

    REQUIRE_FALSE(ov.park_macro_options.empty());
    CHECK(ov.park_macro_options.front() == toolchanger_addon::kAutoMacro);
    CHECK(std::find(ov.park_macro_options.begin(), ov.park_macro_options.end(),
                    "CUSTOM_PARK_TOOL") != ov.park_macro_options.end());
}

TEST_CASE("Movement override: stock commands appear only in their movement picker",
          "[indx][dispatch]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "gcode_macro PARK_TOOL", "gcode_macro CHANGE_TOOL",
                                  "gcode_macro CUSTOM_TOOL_MACRO"}));
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw);

    CHECK(std::find(ov.select_macro_options.begin(), ov.select_macro_options.end(), "PARK_TOOL") ==
          ov.select_macro_options.end());
    CHECK(std::find(ov.park_macro_options.begin(), ov.park_macro_options.end(), "CHANGE_TOOL") ==
          ov.park_macro_options.end());
    CHECK(std::find(ov.select_macro_options.begin(), ov.select_macro_options.end(),
                    "CHANGE_TOOL") != ov.select_macro_options.end());
    CHECK(std::find(ov.park_macro_options.begin(), ov.park_macro_options.end(), "PARK_TOOL") !=
          ov.park_macro_options.end());
    CHECK(std::find(ov.select_macro_options.begin(), ov.select_macro_options.end(),
                    "CUSTOM_TOOL_MACRO") != ov.select_macro_options.end());
    CHECK(std::find(ov.park_macro_options.begin(), ov.park_macro_options.end(),
                    "CUSTOM_TOOL_MACRO") != ov.park_macro_options.end());

    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(ov);
    auto actions = h.get_device_actions();
    CHECK(options_for(actions, "tool_select_macro") == ov.select_macro_options);
    CHECK(options_for(actions, "tool_park_macro") == ov.park_macro_options);

    CHECK_FALSE(
        h.execute_device_action("tool_select_macro", std::any(std::string("PARK_TOOL"))).success());
    CHECK_FALSE(
        h.execute_device_action("tool_park_macro", std::any(std::string("CHANGE_TOOL"))).success());
    CHECK(h.change_tool(1).success());
    CHECK(h.sent().back() == "T1");
}

TEST_CASE("Movement override: stored opposite-direction commands refuse movement",
          "[indx][dispatch]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "gcode_macro PARK_TOOL", "gcode_macro CHANGE_TOOL"}));
    auto ov = toolchanger_addon::resolve_tool_movement_override(hw, "park_tool", "change_tool");

    CHECK(ov.select_choice == Choice::kInvalid);
    CHECK(ov.park_choice == Choice::kInvalid);
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(ov);
    CHECK_FALSE(h.change_tool(1).success());
    CHECK(h.sent().empty());

    auto actions = h.get_device_actions();
    CHECK(value_for(actions, "tool_select_macro") == "park_tool");
    CHECK(value_for(actions, "tool_park_macro") == "change_tool");
    const auto select_options = options_for(actions, "tool_select_macro");
    const auto park_options = options_for(actions, "tool_park_macro");
    CHECK(std::find(select_options.begin(), select_options.end(), "park_tool") !=
          select_options.end());
    CHECK(std::find(park_options.begin(), park_options.end(), "change_tool") != park_options.end());

    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(hw, "auto", "change_tool"));
    REQUIRE(h.change_tool(1).success());
    helix::ui::UpdateQueue::instance().drain();
    const size_t before_park = h.sent().size();
    CHECK_FALSE(h.unload_filament(1).success());
    CHECK(h.sent().size() == before_park);
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

TEST_CASE("Movement override: an invalid Select override sends nothing", "[indx][dispatch]") {
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
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO")));

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
    // No set_tool_movement_override() call -> both option lists stay empty.
    CHECK(h.get_device_actions().empty());
    CHECK(h.get_device_sections().empty());
}

TEST_CASE("Movement override: execute_device_action resolves and persists a valid choice",
          "[indx][dispatch]") {
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO")));

    auto err =
        h.execute_device_action("tool_select_macro", std::any(std::string("CUSTOM_TOOL_MACRO")));
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
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO")));

    // Not among the resolved options -- an unrecognized choice reaching
    // execute_device_action() through anything other than the offered
    // dropdown values still resolves to Invalid, not a silent accept.
    auto err =
        h.execute_device_action("tool_park_macro", std::any(std::string("NOT_A_REAL_MACRO")));
    REQUIRE(err.success()); // the SETTING is accepted and persisted...
    auto park_err = h.unload_filament(0);
    // ...but dispatch of the now-invalid choice sends nothing.
    CHECK_FALSE(park_err.success());

    helix::SettingsManager::instance().set_tool_park_macro("auto");
}

// =============================================================================
// An invalid stored choice has to stay reachable (plan §7.1 keeps it invalid;
// the picker is the only way back to Auto)
// =============================================================================

TEST_CASE("Movement override: an invalid stored choice is listed so it can be cleared",
          "[indx][dispatch]") {
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO"), "GHOST_TOOL", "auto"));

    // The dropdown echoes current_value back by exact string match against
    // options. Without the stale name among them the selection stays on the
    // first entry -- "auto" -- while the backend refuses every swap, and
    // picking "auto" then changes no index and fires no event.
    auto actions = h.get_device_actions();
    auto select_options = options_for(actions, "tool_select_macro");
    REQUIRE_FALSE(select_options.empty());
    CHECK(select_options.front() == toolchanger_addon::kAutoMacro);
    CHECK(std::find(select_options.begin(), select_options.end(), "GHOST_TOOL") !=
          select_options.end());

    // ...and the invalid state is real until it is cleared.
    CHECK_FALSE(h.change_tool(1).success());

    REQUIRE(h.execute_device_action("tool_select_macro",
                                    std::any(std::string(toolchanger_addon::kAutoMacro)))
                .success());
    REQUIRE(h.change_tool(1).success());
    CHECK(h.sent().back() == "T1");

    helix::SettingsManager::instance().set_tool_select_macro("auto");
}

TEST_CASE("Movement override: a valid choice stored in other casing is shown selected",
          "[indx][dispatch]") {
    // The dropdown selects by exact match against options, which spell the
    // macro uppercased. Showing the stored spelling would leave "auto"
    // selected while every swap uses the override.
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO"), "custom_tool_macro", "auto"));

    auto actions = h.get_device_actions();
    auto select_options = options_for(actions, "tool_select_macro");
    CHECK(value_for(actions, "tool_select_macro") == "CUSTOM_TOOL_MACRO");
    CHECK(std::count(select_options.begin(), select_options.end(), "CUSTOM_TOOL_MACRO") == 1);
    CHECK(std::find(select_options.begin(), select_options.end(), "custom_tool_macro") ==
          select_options.end());
}

TEST_CASE("Movement override: a valid choice outside the candidate filter is listed",
          "[indx][dispatch]") {
    // MY_SWAP names none of the fragments tool_movement_macro_candidates()
    // matches, yet it resolves and every swap sends it.
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"extruder", "gcode_macro CUSTOM_TOOL_MACRO", "gcode_macro MY_SWAP"}));
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(hw, "MY_SWAP", "auto"));

    auto actions = h.get_device_actions();
    auto select_options = options_for(actions, "tool_select_macro");
    CHECK(value_for(actions, "tool_select_macro") == "MY_SWAP");
    CHECK(std::find(select_options.begin(), select_options.end(), "MY_SWAP") !=
          select_options.end());
}

TEST_CASE("Movement override: re-picking a valid select choice outside the candidate filter stays "
          "valid",
          "[indx][dispatch]") {
    // The dropdown lists MY_SWAP because it resolved valid; picking it again
    // after Auto must not turn a macro the printer reports into an invalid one.
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"extruder", "gcode_macro CUSTOM_TOOL_MACRO", "gcode_macro MY_SWAP"}));
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(hw, "MY_SWAP", "auto"));

    REQUIRE(h.execute_device_action("tool_select_macro", std::any(std::string("auto"))).success());
    REQUIRE(
        h.execute_device_action("tool_select_macro", std::any(std::string("MY_SWAP"))).success());
    REQUIRE(h.change_tool(1).success());
    CHECK(h.sent().back() == "MY_SWAP TOOL=1");

    helix::SettingsManager::instance().set_tool_select_macro("auto");
}

TEST_CASE("Movement override: re-picking a valid park choice outside the candidate filter stays "
          "valid",
          "[indx][dispatch]") {
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"extruder", "gcode_macro CUSTOM_TOOL_MACRO", "gcode_macro MY_DOCK"}));
    MovementHelper h(3);
    h.set_tool_commands(indx_commands(3));
    h.set_tool_movement_override(
        toolchanger_addon::resolve_tool_movement_override(hw, "auto", "MY_DOCK"));

    REQUIRE(h.execute_device_action("tool_park_macro", std::any(std::string("auto"))).success());
    REQUIRE(h.execute_device_action("tool_park_macro", std::any(std::string("MY_DOCK"))).success());
    REQUIRE(h.unload_filament(0).success());
    CHECK(h.sent().back() == "MY_DOCK");

    helix::SettingsManager::instance().set_tool_park_macro("auto");
}

TEST_CASE("Movement override: a printer with no candidate macros still offers Auto to clear one",
          "[indx][dispatch]") {
    // Both movement option lists are empty here, which can hide the section --
    // leaving a stored choice this printer cannot resolve with no way back.
    MovementHelper h(2);
    h.set_tool_commands(indx_commands(2));
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_without_candidates(), "auto", "GHOST_PARK"));

    auto sections = h.get_device_sections();
    CHECK(std::any_of(sections.begin(), sections.end(),
                      [](const auto& sec) { return sec.id == "tool_commands"; }));

    auto park_options = options_for(h.get_device_actions(), "tool_park_macro");
    REQUIRE(park_options.size() == 2);
    CHECK(park_options[0] == toolchanger_addon::kAutoMacro);
    CHECK(park_options[1] == "GHOST_PARK");

    REQUIRE(h.execute_device_action("tool_park_macro",
                                    std::any(std::string(toolchanger_addon::kAutoMacro)))
                .success());
    REQUIRE(h.unload_filament(0).success());
    CHECK(h.sent().back() == "PARK_TOOL");

    helix::SettingsManager::instance().set_tool_park_macro("auto");
}

// =============================================================================
// Scope: the picker belongs to a changer extra, not to every printer without
// [toolchanger]
// =============================================================================

TEST_CASE("Movement override: a plain multi-extruder printer is offered no picker",
          "[indx][dispatch]") {
    // ToolCommands::present is true for ANY printer without [toolchanger],
    // including a dual-extruder box whose T<n> is Klipper's own
    // ACTIVATE_EXTRUDER. Its macros still match TOOL/PARK/CHANGE by name, so
    // the candidate list is non-empty -- the provider name is what scopes this.
    MovementHelper h(2);
    ToolCommands plain;
    plain.present = true;
    plain.provider_name = "";
    plain.select_prefix = "T";
    h.set_tool_commands(plain);
    h.set_tool_movement_override(toolchanger_addon::resolve_tool_movement_override(
        discovery_with_macro("CUSTOM_TOOL_MACRO")));

    CHECK(h.get_device_actions().empty());
    CHECK(h.get_device_sections().empty());
}

TEST_CASE("Movement override: a plain multi-extruder printer ignores a stored override",
          "[indx][dispatch]") {
    // The choice is one GLOBAL setting across printers. With no picker on this
    // machine, honouring a macro chosen for a different one would send it --
    // or, invalid here, refuse every swap with nothing able to clear it.
    MovementHelper h(2);
    ToolCommands plain;
    plain.present = true;
    plain.select_prefix = "T";
    h.set_tool_commands(plain);

    ToolMovementOverride ov;
    ov.select_choice = Choice::kValid;
    ov.select_macro = "SOMEONE_ELSES_MACRO";
    ov.park_choice = Choice::kInvalid;
    ov.park_choice_raw = "GHOST_PARK";
    h.set_tool_movement_override(ov);

    REQUIRE(h.change_tool(1).success());
    CHECK(h.sent().back() == "T1");

    // The park direction has no unselect command on a plain machine, so it is
    // refused for THAT reason -- never for a stale override it cannot clear.
    auto park = h.unload_filament(0);
    CHECK_FALSE(park.success());
    CHECK(park.technical_msg.find("configured macro not found") == std::string::npos);
}
