// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_indx_paused_homing.cpp
 * @brief Package D of docs/devel/plans/2026-09-20-bondtech-indx.md: §7.4/D6
 * paused-print and homing policy.
 *
 * The stock INDX CHANGE_TOOL/PARK_TOOL macros home conditionally themselves,
 * so AmsBackendToolChanger::delegates_homing_to_printer() answers true for
 * this provider (never a HelixScreen-synthesized G28, never a home-confirm
 * prompt). That is safe while idle or printing, where the macro's own G28
 * runs unencumbered. It is NOT safe on a PAUSED print: Layer 1
 * (helix::api::reject_homing_during_active_print) blocks any
 * HelixScreen-emitted G28 while paused, but cannot see one buried inside the
 * macro, and injecting a home into a paused print is exactly what this plan
 * forbids. dispatch_operation()'s own paused/unhomed precondition (added by
 * this package) is what closes that gap: known homed xyz required before
 * ANY paused-print dispatch on this provider, zero commands otherwise.
 *
 * Mock only, per plan §7.4 ("no physical paused-print test is authorized").
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_toolchanger.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/toolchanger_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "toolchanger_addon.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::ToolChangerTestAccess;
using helix::toolchanger_addon::ToolCommands;

namespace {

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

ToolCommands medusahc_fork_commands(int tool_count) {
    ToolCommands c;
    c.present = true;
    c.provider_name = "MedusaHC"; // ToolCommands::present, but NOT INDX.
    c.select_prefix = "T";
    c.unselect = "DROP_TOOL";
    c.select_shortcut_available.assign(static_cast<size_t>(tool_count), true);
    return c;
}

std::vector<std::string> discovered_tools(int count) {
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        names.push_back(std::to_string(i));
    }
    return names;
}

/// Real backend against a real MoonrakerAPI/MoonrakerClientMock/PrinterState,
/// so print lifecycle and homed_axes are the actual live subjects
/// dispatch_operation() and ensure_homed_then() read — not a hand-rolled
/// substitute. Modeled on test_ams_toolchanger_indx_backend.cpp's
/// LiveIndxHarness.
class PausedHomingHarness : public LVGLTestFixture {
  public:
    explicit PausedHomingHarness(ToolCommands commands, int tool_count = 3)
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state), backend(&api, nullptr) {
        state.init_subjects(false);
        set_homed(true); // idle/homed by default; tests override explicitly.
        backend.set_discovered_tools(discovered_tools(tool_count));
        backend.set_tool_commands(std::move(commands));
        ToolChangerTestAccess::mark_running(backend);
    }

    ~PausedHomingHarness() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void set_homed(bool homed) {
        lv_subject_copy_string(state.get_homed_axes_subject(), homed ? "xyz" : "");
    }

    void set_unknown_homed() {
        // Never-reported homed_axes: same empty-string representation as
        // "definitely not homed" on this subject — toolhead_is_homed() has no
        // separate "unknown" state, so this exercises the same refusal path
        // as an explicit unhomed reading (plan §7.4: "unhomed or unknown").
        lv_subject_copy_string(state.get_homed_axes_subject(), "");
    }

    void pause_print() {
        helix::test::set_wire_state(state, PrintJobState::PAUSED);
    }

    void print_printing() {
        helix::test::set_wire_state(state, PrintJobState::PRINTING);
    }

    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPI api;
    helix::AmsBackendToolChanger backend;
};

} // namespace

// =============================================================================
// INDX: idle — delegates_homing_to_printer() means no HelixScreen G28/prompt
// =============================================================================

TEST_CASE("INDX paused/homing: idle and homed dispatches normally", "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    auto err = h.backend.change_tool(1);
    CHECK(err.success());
}

TEST_CASE("INDX paused/homing: idle and UNHOMED still dispatches — the macro homes itself",
          "[indx][dispatch]") {
    // delegates_homing_to_printer() short-circuits ensure_homed_then()'s own
    // toolhead_homed() check entirely while idle: HelixScreen synthesizes no
    // G28 and asks no confirmation, exactly as the stock CHANGE_TOOL's own
    // conditional home expects.
    PausedHomingHarness h(indx_commands(3));
    h.set_homed(false);
    auto err = h.backend.change_tool(1);
    CHECK(err.success());
}

// =============================================================================
// INDX: Printing — existing refuse_if_printing() regression (unchanged)
// =============================================================================

TEST_CASE("INDX paused/homing: Printing refuses regardless of homed state",
          "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    h.print_printing();
    h.set_homed(true);
    auto err = h.backend.change_tool(1);
    CHECK_FALSE(err.success());
}

// =============================================================================
// INDX: Paused — the new precondition this package adds
// =============================================================================

TEST_CASE("INDX paused/homing: Paused and homed dispatches (D6 — manual ops allowed paused)",
          "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    h.pause_print();
    h.set_homed(true);
    auto err = h.backend.change_tool(1);
    CHECK(err.success());
}

TEST_CASE("INDX paused/homing: Paused and UNHOMED refuses with zero commands",
          "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    h.pause_print();
    h.set_homed(false);
    auto err = h.backend.change_tool(1);
    CHECK_FALSE(err.success());
}

TEST_CASE("INDX paused/homing: Paused and UNKNOWN homed state refuses too",
          "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    h.pause_print();
    h.set_unknown_homed();
    auto err = h.backend.change_tool(1);
    CHECK_FALSE(err.success());
}

TEST_CASE("INDX paused/homing: Paused Park (unmount) is gated the same as Select",
          "[indx][dispatch]") {
    PausedHomingHarness h(indx_commands(3));
    // Seat a tool while idle/homed, then pause and go unhomed.
    REQUIRE(h.backend.change_tool(1).success());
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    h.pause_print();
    h.set_homed(false);

    auto err = h.backend.unload_filament(1);
    CHECK_FALSE(err.success());
}

// =============================================================================
// Regression: an ordinary ToolCommands::present provider that is NOT INDX
// keeps its existing behavior — the new precondition is scoped to INDX only
// (plan §7.4 is explicit about this; a different provider has no documented
// self-homing macro contract to delegate to).
// =============================================================================

TEST_CASE("Non-INDX paused/homing: a MedusaHC-fork provider is unaffected by the new precondition",
          "[indx][dispatch][regression]") {
    // provider_name != "INDX", so delegates_homing_to_printer() stays false
    // and the paused/unhomed precondition added for INDX never engages.
    // ensure_homed_then()'s ordinary confirm-then-G28 path runs instead, and
    // with no confirmation prompter installed that resolves synchronously to
    // a dispatch (the existing default every pre-existing test relies on).
    PausedHomingHarness h(medusahc_fork_commands(3));
    h.pause_print();
    h.set_homed(false);

    auto err = h.backend.change_tool(1);
    // Not refused by the INDX-only paused/unhomed gate. Whatever
    // ensure_homed_then() does next (synthesize a G28, then dispatch) is
    // package B/existing behavior, unchanged by this package.
    CHECK(err.success());
}
