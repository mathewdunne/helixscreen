// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_indx_discovery.cpp
 * @brief PrinterDiscovery detection of the Bondtech INDX nozzle changer.
 *
 * Object-list-only detection: the exact `indx` status object, independent of
 * final AMS type or tool count (docs/devel/plans/2026-09-20-bondtech-indx.md
 * §5.1 point 1). Fixtures are synthetic and source-derived — see
 * tests/fixtures/indx/README.md.
 */

#include "printer_discovery.h"

#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::AmsType;
using helix::PrinterDiscovery;

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

} // namespace

TEST_CASE("PrinterDiscovery: exact indx object detected as a plain fact", "[indx][discovery]") {
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_six_tool.json"));

    REQUIRE(hw.has_indx());
    // An object fact only — never claims the priority chain by itself. INDX
    // has one shared extruder heater, so the extruder-count fallback that
    // synthesizes tool_names for plain multi-extruder printers must not fire.
    REQUIRE(hw.tool_names().empty());
    REQUIRE(hw.mmu_type() == AmsType::NONE);
    REQUIRE(hw.detected_ams_systems().empty());
}

TEST_CASE("PrinterDiscovery: three configured tools with four shortcuts still just has_indx",
          "[indx][discovery]") {
    // The configured runtime count (read later from TOOL_POSITIONS.tool_count)
    // must outrank shortcut count; parse_objects() cannot know the count yet,
    // so it must not guess anything from the four T<n> macros present here.
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_three_tool_four_shortcuts.json"));

    REQUIRE(hw.has_indx());
    REQUIRE(hw.tool_names().empty());
}

TEST_CASE("PrinterDiscovery: lookalike objects do not trigger indx detection",
          "[indx][discovery]") {
    // mcu indxmcu / angle indx / neopixel indx / bare T<n> shortcuts: none of
    // these is the exact `indx` status object (plan §5.1 point 1).
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_lookalikes.json"));

    REQUIRE_FALSE(hw.has_indx());
}

TEST_CASE("PrinterDiscovery: a finalized indx inventory names Bondtech INDX",
          "[indx][discovery][inventory]") {
    // The other half of the label: with no klipper-toolchanger, the INDX
    // commands resolve_tool_commands() hands out ARE what the printer runs.
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_six_tool.json"));
    REQUIRE(hw.detected_ams_systems().empty());

    REQUIRE(hw.finalize_indx_inventory({"0", "1", "2", "3", "4", "5"}));
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].name == "Bondtech INDX");
}

TEST_CASE("PrinterDiscovery: indx alongside several extruder heaters is not named INDX",
          "[indx][discovery][inventory]") {
    // The tools here are counted from the hot ends ("T0", "T1") and swap by
    // plain T<n>; resolve_tool_commands() does not claim INDX for them.
    PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"extruder", "extruder1", "indx", "save_variables",
                               "gcode_macro TOOL_POSITIONS", "gcode_macro PARK_TOOL"}));

    REQUIRE(hw.has_indx());
    REQUIRE(hw.tool_names() == std::vector<std::string>{"T0", "T1"});
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems()[0].name == "Tool Changer");
}

TEST_CASE("PrinterDiscovery: indx alongside a native toolchanger keeps native priority",
          "[indx][discovery][priority]") {
    // A defensive matrix case: object presence is recorded, but detection
    // alone must never disturb an existing native tool-changer's inventory.
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_alongside_toolchanger.json"));

    REQUIRE(hw.has_indx());
    REQUIRE(hw.has_tool_changer());
    REQUIRE(hw.tool_names() == std::vector<std::string>{"T0", "T1"});
    REQUIRE(hw.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    // Named for whoever sends the swap: klipper-toolchanger owns it here
    // (resolve_tool_commands() returns the native SELECT_TOOL contract), so
    // calling the system "Bondtech INDX" would name commands never sent.
    REQUIRE(hw.detected_ams_systems()[0].name == "Tool Changer");
}

TEST_CASE("PrinterDiscovery: config-only [indx] section with no exact object is not detection",
          "[indx][discovery]") {
    // D7/§0: config-only compatibility is explicitly out of scope. An object
    // list carrying no `indx` status object — even with plausible-looking
    // gcode_macro names — must not detect.
    PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"webhooks", "configfile", "extruder", "heater_bed",
                               "gcode_macro TOOL_POSITIONS", "gcode_macro T0", "gcode_macro T1"});
    hw.parse_objects(objects);

    REQUIRE_FALSE(hw.has_indx());
}

TEST_CASE("PrinterDiscovery: indx object fact survives clear()/reparse cycle",
          "[indx][discovery]") {
    PrinterDiscovery hw;
    hw.parse_objects(load_fixture("objects_list_six_tool.json"));
    REQUIRE(hw.has_indx());

    // parse_objects() calls clear() internally; reparsing a printer without
    // indx must not leave the fact stuck true (reconnect to a different
    // printer, or a printer switch).
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    REQUIRE_FALSE(hw.has_indx());
}
