// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_indx.cpp
 * @brief toolchanger_addon's Bondtech INDX provider contract: required
 * subscriptions, command defaults, and the validated inventory/state readers
 * from docs/devel/plans/2026-09-20-bondtech-indx.md §5.
 *
 * Fixtures are synthetic and source-derived — see tests/fixtures/indx/README.md.
 */

#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrinterDiscovery;
namespace addon = helix::toolchanger_addon;

namespace {

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/indx/";
    }
    return "tests/fixtures/indx/";
}

nlohmann::json load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + name;
    std::ifstream f(path);
    INFO("fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j;
}

PrinterDiscovery discover(const std::string& objects_fixture) {
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture(objects_fixture));
    return hw;
}

} // namespace

// ---------------------------------------------------------------------------
// Candidate recognition
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: indx object alone is an inventory candidate", "[indx][discovery]") {
    auto hw = discover("objects_list_six_tool.json");
    REQUIRE(addon::is_indx_inventory_candidate(hw));
}

TEST_CASE("toolchanger_addon: no indx object is never a candidate", "[indx][discovery]") {
    auto hw = discover("objects_list_lookalikes.json");
    REQUIRE_FALSE(addon::is_indx_inventory_candidate(hw));
}

TEST_CASE("toolchanger_addon: a native toolchanger outranks an indx candidate",
          "[indx][discovery][priority]") {
    auto hw = discover("objects_list_alongside_toolchanger.json");
    REQUIRE(hw.has_indx());
    REQUIRE_FALSE(addon::is_indx_inventory_candidate(hw));
}

TEST_CASE("toolchanger_addon: an already-claimed MMU outranks an indx candidate",
          "[indx][discovery][priority]") {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"mmu", "mmu_encoder mmu_encoder", "extruder", "heater_bed", "indx"}));
    REQUIRE(hw.has_indx());
    REQUIRE(hw.has_mmu());
    REQUIRE_FALSE(addon::is_indx_inventory_candidate(hw));
}

// ---------------------------------------------------------------------------
// Required subscriptions
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: indx subscribes save_variables and TOOL_POSITIONS only",
          "[indx][discovery][subscriptions]") {
    auto hw = discover("objects_list_six_tool.json");
    auto objects = addon::required_status_objects(hw);

    REQUIRE(std::find(objects.begin(), objects.end(), "save_variables") != objects.end());
    REQUIRE(std::find(objects.begin(), objects.end(), "gcode_macro TOOL_POSITIONS") !=
            objects.end());
    // Never a fabricated toolchanger/tool object for this provider (plan §5.2).
    REQUIRE(std::find(objects.begin(), objects.end(), "toolchanger") == objects.end());
    for (const auto& o : objects) {
        REQUIRE(o.rfind("tool ", 0) != 0);
    }
    // No per-tool spool macro subscription for this delivery (plan §5.2).
    for (const auto& o : objects) {
        REQUIRE(o.find("spool_id") == std::string::npos);
    }
}

TEST_CASE("toolchanger_addon: the TOOL_POSITIONS subscription keeps the config's case",
          "[indx][discovery][subscriptions]") {
    // has_macro() is case-insensitive, so a mixed-case section passes every
    // macro check; Klipper keys the STATUS OBJECT on the config spelling, so an
    // uppercased subscription would leave this printer with no inventory and
    // therefore no INDX backend at all.
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"indx", "save_variables", "gcode_macro Tool_Positions",
                                            "gcode_macro PARK_TOOL", "gcode_macro T0",
                                            "gcode_macro T1", "extruder"}));
    REQUIRE(addon::is_indx_inventory_candidate(hw));
    REQUIRE(addon::indx_tool_positions_object(hw) == "gcode_macro Tool_Positions");

    auto objects = addon::required_status_objects(hw);
    REQUIRE(std::find(objects.begin(), objects.end(), "gcode_macro Tool_Positions") !=
            objects.end());
    REQUIRE(std::find(objects.begin(), objects.end(), "gcode_macro TOOL_POSITIONS") ==
            objects.end());
}

TEST_CASE("toolchanger_addon: an indx printer with no TOOL_POSITIONS macro subscribes none",
          "[indx][discovery][subscriptions]") {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"indx", "save_variables", "extruder"}));
    REQUIRE(addon::indx_tool_positions_object(hw).empty());
    auto objects = addon::required_status_objects(hw);
    for (const auto& o : objects) {
        REQUIRE(o.rfind("gcode_macro", 0) != 0);
    }
}

TEST_CASE("toolchanger_addon: a non-indx printer requests no indx objects",
          "[indx][discovery][subscriptions]") {
    auto hw = discover("objects_list_lookalikes.json");
    auto objects = addon::required_status_objects(hw);
    REQUIRE(objects.empty());
}

TEST_CASE("toolchanger_addon: MedusaHC subscriptions are unaffected by indx additions",
          "[indx][discovery][subscriptions][priority]") {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"pin_watch io", "toolchanger", "tool T0", "extruder"}));
    auto objects = addon::required_status_objects(hw);
    REQUIRE(std::find(objects.begin(), objects.end(), "medusahc") != objects.end());
    REQUIRE(std::find(objects.begin(), objects.end(), "pin_watch io") != objects.end());
    REQUIRE(std::find(objects.begin(), objects.end(), "save_variables") == objects.end());
}

// ---------------------------------------------------------------------------
// Command defaults
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: indx default commands are T<n> select / PARK_TOOL park",
          "[indx][backend]") {
    auto hw = discover("objects_list_six_tool.json");
    // Finalized first, as the discovery sequence does: the provider's command
    // contract is defined against ITS numbered inventory, so an unfinalized
    // candidate has nothing for the T<n> probe to answer about yet.
    REQUIRE(hw.finalize_indx_inventory(addon::indx_tool_ids(6)));
    auto commands = addon::resolve_tool_commands(hw);

    REQUIRE(commands.present);
    REQUIRE(commands.provider_name == "INDX");
    REQUIRE(commands.select_prefix == "T");
    REQUIRE(commands.unselect == "PARK_TOOL");
}

TEST_CASE("toolchanger_addon: the variable-only TOOL_POSITIONS macro is no movement candidate",
          "[indx][dispatch]") {
    // TOOL_POSITIONS only carries INDX's variables; "TOOL_POSITIONS TOOL=<n>"
    // moves nothing, so offering it as a Select/Park macro is a trap.
    auto hw = discover("objects_list_six_tool.json");
    auto candidates = addon::tool_movement_macro_candidates(hw);

    CHECK(std::find(candidates.begin(), candidates.end(), "TOOL_POSITIONS") == candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), "PARK_TOOL") != candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), "CHANGE_TOOL") != candidates.end());
}

TEST_CASE("toolchanger_addon: an unfinalized indx candidate claims no provider",
          "[indx][backend][priority]") {
    // Before the tool count is read there is no inventory to define T<n>
    // against, and select_shortcut_available would come back empty - which
    // do_change_tool() reads as "every tool has a shortcut".
    auto hw = discover("objects_list_six_tool.json");
    auto commands = addon::resolve_tool_commands(hw);

    REQUIRE(commands.present); // no [toolchanger] on this machine
    CHECK(commands.provider_name.empty());
    CHECK(commands.unselect.empty());
    CHECK(commands.select_shortcut_available.empty());
}

TEST_CASE("toolchanger_addon: indx alongside several extruder heaters keeps plain T<n>",
          "[indx][backend][priority]") {
    // parse_objects() names the tools after the hot ends here ("T0", "T1"),
    // which is_indx_inventory_candidate() rejects - so nothing ever finalizes
    // them and the T<n> probe would ask about "TT0". Claiming the provider
    // anyway would refuse every swap on a machine whose plain T<n> works, and
    // fold its real second extruder onto one shared_extruder_name().
    PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"extruder", "extruder1", "indx", "save_variables",
                               "gcode_macro TOOL_POSITIONS", "gcode_macro PARK_TOOL"}));
    REQUIRE(hw.has_indx());
    REQUIRE(hw.tool_names() == std::vector<std::string>{"T0", "T1"});
    REQUIRE_FALSE(addon::is_indx_inventory_candidate(hw));

    auto commands = addon::resolve_tool_commands(hw);
    REQUIRE(commands.present);
    CHECK(commands.provider_name.empty());
    CHECK(commands.select_prefix == "T");
    CHECK(commands.select_shortcut_available.empty());
    CHECK(commands.unselect.empty());
}

TEST_CASE("toolchanger_addon: indx has no feeder or dock sensor capability", "[indx][backend]") {
    auto hw = discover("objects_list_six_tool.json");
    REQUIRE(hw.finalize_indx_inventory(addon::indx_tool_ids(6)));
    REQUIRE_FALSE(addon::resolve_feeder(hw).present);
    // toolchanger_addon's sensor/feeder table only recognizes dock-sensor
    // add-ons (MedusaHC-shaped); INDX must never be folded into that
    // recognition, since it has neither a dock sensor nor a feeder.
    REQUIRE_FALSE(addon::resolve_tool_sensor(hw).present);
}

TEST_CASE("toolchanger_addon: a real toolchanger keeps SELECT_TOOL/UNSELECT_TOOL despite indx",
          "[indx][backend][priority]") {
    auto hw = discover("objects_list_alongside_toolchanger.json");
    auto commands = addon::resolve_tool_commands(hw);
    // has_tool_changer() short-circuits to the empty (native) answer.
    REQUIRE_FALSE(commands.present);
}

// ---------------------------------------------------------------------------
// Inventory finalization: gcode_macro TOOL_POSITIONS.tool_count
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: valid tool counts finalize to numbered tool ids",
          "[indx][discovery][inventory]") {
    auto positions = load_fixture("tool_positions_status_valid.json");

    {
        auto inv = addon::read_indx_inventory(positions["six_tool"]);
        REQUIRE(inv.has_value());
        REQUIRE(inv->valid);
        REQUIRE(inv->tool_count == 6);
        REQUIRE(addon::indx_tool_ids(inv->tool_count) ==
                std::vector<std::string>{"0", "1", "2", "3", "4", "5"});
    }
    {
        // Three configured tools, even though four T<n> shortcuts exist on
        // the printer — shortcut count is never the inventory source.
        auto inv = addon::read_indx_inventory(positions["three_tool"]);
        REQUIRE(inv.has_value());
        REQUIRE(inv->valid);
        REQUIRE(inv->tool_count == 3);
        REQUIRE(addon::indx_tool_ids(inv->tool_count) == std::vector<std::string>{"0", "1", "2"});
    }
    {
        auto inv = addon::read_indx_inventory(positions["one_tool"]);
        REQUIRE(inv.has_value());
        REQUIRE(inv->valid);
        REQUIRE(inv->tool_count == 1);
    }
    {
        auto inv = addon::read_indx_inventory(positions["sixteen_tool"]);
        REQUIRE(inv.has_value());
        REQUIRE(inv->valid);
        REQUIRE(inv->tool_count == addon::kIndxMaxTools);
    }
}

TEST_CASE("toolchanger_addon: an absent tool_count field is no news, not a rejection",
          "[indx][discovery][inventory]") {
    auto positions = load_fixture("tool_positions_status_malformed.json");
    auto inv = addon::read_indx_inventory(positions["empty_object"]);
    REQUIRE_FALSE(inv.has_value());
}

TEST_CASE("toolchanger_addon: malformed tool_count values are explicitly rejected",
          "[indx][discovery][inventory]") {
    auto positions = load_fixture("tool_positions_status_malformed.json");

    for (const char* key : {"boolean_tool_count", "fractional_tool_count", "zero_tool_count",
                            "negative_tool_count", "oversized_tool_count", "string_tool_count"}) {
        INFO("case: " << key);
        auto inv = addon::read_indx_inventory(positions[key]);
        REQUIRE(inv.has_value());
        REQUIRE_FALSE(inv->valid);
        REQUIRE_FALSE(inv->rejection.empty());
    }
}

TEST_CASE("toolchanger_addon: indx_tool_ids is empty for a non-positive count",
          "[indx][discovery][inventory]") {
    REQUIRE(addon::indx_tool_ids(0).empty());
    REQUIRE(addon::indx_tool_ids(-3).empty());
}

// ---------------------------------------------------------------------------
// Active-tool identity: save_variables.variables.active_tool
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: a valid active_tool reads as kValid", "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_active_tool.json");
    auto reading = addon::read_indx_active_tool(status, /*configured_tool_count=*/6);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kValid);
    REQUIRE(reading.value == 3);
}

TEST_CASE("toolchanger_addon: explicit park (-1) is a distinct valid state",
          "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_parked.json");
    auto reading = addon::read_indx_active_tool(status, /*configured_tool_count=*/6);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kValid);
    REQUIRE(reading.value == -1);
}

TEST_CASE("toolchanger_addon: a sparse delta without active_tool is no news",
          "[indx][backend][state]") {
    // Synthetic defensive coverage (plan §5.2): real Moonraker deltas carry
    // the whole `variables` dict, but the parser must not treat a delta
    // missing the key as a change to -1 or to any other value.
    auto status = load_fixture("save_variables_status_sparse_delta.json");
    auto reading = addon::read_indx_active_tool(status, /*configured_tool_count=*/6);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kAbsent);
}

TEST_CASE("toolchanger_addon: a fresh install with no saved active_tool is absent, not T0",
          "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_malformed.json");
    auto reading = addon::read_indx_active_tool(status["no_variables_key"],
                                                /*configured_tool_count=*/6);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kAbsent);

    auto reading2 =
        addon::read_indx_active_tool(status["empty_delta"], /*configured_tool_count=*/6);
    REQUIRE(reading2.status == addon::IndxActiveToolStatus::kAbsent);
}

TEST_CASE("toolchanger_addon: malformed active_tool values are explicitly rejected",
          "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_malformed.json");

    for (const char* key : {"boolean_active_tool", "fractional_active_tool", "non_numeric_string",
                            "huge_number", "below_sentinel"}) {
        INFO("case: " << key);
        auto reading = addon::read_indx_active_tool(status[key], /*configured_tool_count=*/6);
        REQUIRE(reading.status == addon::IndxActiveToolStatus::kMalformed);
    }
}

TEST_CASE("toolchanger_addon: an active_tool outside the finalized inventory is rejected",
          "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_malformed.json");
    auto reading = addon::read_indx_active_tool(status["out_of_range_for_six_tools"],
                                                /*configured_tool_count=*/6);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kMalformed);
}

TEST_CASE("toolchanger_addon: with inventory not yet finalized, bounds are not enforced",
          "[indx][backend][state]") {
    auto status = load_fixture("save_variables_status_malformed.json");
    // configured_tool_count == 0 means "not finalized yet" — a positive id
    // must still parse (it will be bounds-checked again once inventory is
    // known), so this must NOT read as malformed merely for being un-bounded.
    auto reading = addon::read_indx_active_tool(status["out_of_range_for_six_tools"],
                                                /*configured_tool_count=*/0);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kValid);
    REQUIRE(reading.value == 6);
}

// ---------------------------------------------------------------------------
// Cross-provider isolation (plan §10 "Cross-provider isolation")
// ---------------------------------------------------------------------------

TEST_CASE("toolchanger_addon: INDX-shaped save_variables cannot leak into read_tool()",
          "[indx][backend][state][priority]") {
    // read_tool() is MedusaHC/pin_watch's dispatch. An INDX-style
    // save_variables.active_tool field must never be interpreted by it.
    nlohmann::json status = {
        {"save_variables", {{"variables", {{"active_tool", 3}}}}},
    };
    REQUIRE_FALSE(addon::read_tool(status).has_value());
}

TEST_CASE("toolchanger_addon: MedusaHC fields cannot be read by the indx reader",
          "[indx][backend][state][priority]") {
    // medusahc's current_tool must never satisfy the indx-bound reader, which
    // only ever looks at save_variables.variables.active_tool.
    nlohmann::json status = {
        {"medusahc", {{"current_tool", 2}, {"tool_count", 4}}},
    };
    auto reading = addon::read_indx_active_tool(status, /*configured_tool_count=*/4);
    REQUIRE(reading.status == addon::IndxActiveToolStatus::kAbsent);
}
