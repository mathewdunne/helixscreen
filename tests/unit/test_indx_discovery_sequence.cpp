// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_indx_discovery_sequence.cpp
 * @brief Controlled-transport tests for INDX's deferred-inventory discovery
 * ordering against the REAL MoonrakerDiscoverySequence
 * (docs/devel/plans/2026-09-20-bondtech-indx.md §5.1's mandatory package A gate).
 *
 * The interactive mock's discover_printer() override shortcuts the production
 * sequence and cannot prove this ordering, so every case here drives
 * MoonrakerClient::discover_printer() (discovery_.start()) directly, the same
 * way test_discovery_klippy_gate.cpp and test_rediscovery_fingerprint.cpp do,
 * with send_jsonrpc() resolved through the mock's method handler registry.
 */

#include "../lvgl_test_fixture.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsType;
using helix::PrinterDiscovery;

namespace {

/// Exposes the REAL discovery sequence, exactly like TestDiscoveryClient in
/// test_discovery_klippy_gate.cpp.
class IndxDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real(std::function<void()> on_complete,
                               std::function<void(const std::string&)> on_error) {
        MoonrakerClient::discover_printer(std::move(on_complete), std::move(on_error));
    }
};

/// The full set of INDX object-list facts for an N-tool installation with
/// `shortcut_count` T<n> macros present (>= actual tool count is the
/// interesting case — plan §2 correction 4).
std::vector<std::string> indx_objects(int shortcut_count) {
    std::vector<std::string> objects = {"indx", "gcode_macro TOOL_POSITIONS",
                                        "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL",
                                        "save_variables"};
    for (int i = 0; i < shortcut_count; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    return objects;
}

/// Records callback order/content for one discovery pass.
struct DiscoveryTrace {
    std::vector<std::string> callback_order; // "hardware", "complete"
    PrinterDiscovery hardware_snapshot;
    PrinterDiscovery complete_snapshot;
    bool completed = false;
    bool errored = false;
    std::string error_reason;
};

DiscoveryTrace run(IndxDiscoveryClient& client) {
    DiscoveryTrace trace;
    client.set_on_hardware_discovered([&trace](const PrinterDiscovery& hw) {
        trace.callback_order.push_back("hardware");
        trace.hardware_snapshot = hw;
    });
    client.set_on_discovery_complete(
        [&trace](const PrinterDiscovery& hw, const nlohmann::json&) {
            trace.callback_order.push_back("complete");
            trace.complete_snapshot = hw;
        });
    client.discover_printer_real(
        [&trace]() { trace.completed = true; },
        [&trace](const std::string& reason) {
            trace.errored = true;
            trace.error_reason = reason;
        });
    return trace;
}

} // namespace

TEST_CASE("INDX discovery: valid tool_count finalizes inventory before discovery-complete",
          "[indx][discovery][priority]") {
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_additional_objects(indx_objects(/*shortcut_count=*/6));
    client.set_indx_tool_count(6);

    DiscoveryTrace trace = run(client);

    REQUIRE(trace.completed);
    REQUIRE_FALSE(trace.errored);

    // Both callbacks fired, hardware callback first — the deferred callback's
    // whole point is initialization-before-status ordering (plan §5.1 point 7).
    REQUIRE(trace.callback_order == std::vector<std::string>{"hardware", "complete"});

    // The finalized snapshot is what BOTH callbacks saw — the early callback
    // is not fired twice, once empty and once finalized.
    REQUIRE(trace.hardware_snapshot.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(trace.hardware_snapshot.tool_names() ==
            std::vector<std::string>{"0", "1", "2", "3", "4", "5"});
    REQUIRE(trace.complete_snapshot.mmu_type() == AmsType::TOOL_CHANGER);
    REQUIRE(trace.complete_snapshot.tool_names() == trace.hardware_snapshot.tool_names());

    // One shared physical heater — the mock's default single extruder,
    // untouched by INDX's tool_names_ population (plan §6).
    int extruder_heaters = 0;
    for (const auto& h : trace.complete_snapshot.heaters()) {
        if (h == "extruder" || h.rfind("extruder", 0) == 0) {
            ++extruder_heaters;
        }
    }
    REQUIRE(extruder_heaters == 1);
}

TEST_CASE("INDX discovery: three configured tools despite six T<n> shortcuts",
          "[indx][discovery][inventory]") {
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_additional_objects(indx_objects(/*shortcut_count=*/6));
    client.set_indx_tool_count(3);

    DiscoveryTrace trace = run(client);

    REQUIRE(trace.completed);
    REQUIRE(trace.complete_snapshot.tool_names() == std::vector<std::string>{"0", "1", "2"});
}

TEST_CASE("INDX discovery: a missing TOOL_POSITIONS reply completes with no INDX backend",
          "[indx][discovery][priority]") {
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_additional_objects(indx_objects(/*shortcut_count=*/3));
    // No set_indx_tool_count(): the subscription reply carries no
    // TOOL_POSITIONS status at all (plan §5.1 point 8).

    DiscoveryTrace trace = run(client);

    REQUIRE(trace.completed);
    REQUIRE_FALSE(trace.errored);
    // The deferred hardware callback still fires (other subsystems still
    // need to initialize), but with no usable INDX inventory.
    REQUIRE(trace.callback_order == std::vector<std::string>{"hardware", "complete"});
    REQUIRE(trace.complete_snapshot.mmu_type() == AmsType::NONE);
    REQUIRE(trace.complete_snapshot.tool_names().empty());
}

TEST_CASE("INDX discovery: a malformed tool_count is rejected, not coerced",
          "[indx][discovery][inventory]") {
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_additional_objects(indx_objects(/*shortcut_count=*/3));
    client.set_indx_tool_count(0); // out of the valid 1..kIndxMaxTools range

    DiscoveryTrace trace = run(client);

    REQUIRE(trace.completed);
    REQUIRE(trace.complete_snapshot.mmu_type() == AmsType::NONE);
    REQUIRE(trace.complete_snapshot.tool_names().empty());
}

TEST_CASE("INDX discovery: an ordinary printer's early callback is not deferred",
          "[indx][discovery][priority]") {
    // No indx object at all — the default mock printer, which ships an "mmu"
    // object and so detects Happy Hare. The early hardware callback must fire
    // immediately, exactly as before this feature existed, proving unaffected
    // printers keep their timing and their existing detection result.
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    DiscoveryTrace trace = run(client);

    REQUIRE(trace.completed);
    REQUIRE(trace.callback_order == std::vector<std::string>{"hardware", "complete"});
    REQUIRE_FALSE(trace.hardware_snapshot.has_indx());
    REQUIRE(trace.hardware_snapshot.mmu_type() == AmsType::HAPPY_HARE);
}

TEST_CASE("INDX discovery: reconnect on an unchanged inventory does not re-defer or duplicate",
          "[indx][discovery][inventory]") {
    LVGLTestFixture fixture;
    IndxDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_additional_objects(indx_objects(/*shortcut_count=*/3));
    client.set_indx_tool_count(3);

    DiscoveryTrace pass1 = run(client);
    DiscoveryTrace pass2 = run(client);

    REQUIRE(pass1.completed);
    REQUIRE(pass2.completed);
    REQUIRE(pass1.callback_order == std::vector<std::string>{"hardware", "complete"});
    REQUIRE(pass2.callback_order == std::vector<std::string>{"hardware", "complete"});
    REQUIRE(pass1.complete_snapshot.tool_names() == pass2.complete_snapshot.tool_names());
    REQUIRE(pass2.complete_snapshot.mmu_type() == AmsType::TOOL_CHANGER);
}
