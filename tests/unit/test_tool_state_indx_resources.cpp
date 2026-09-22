// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_state_indx_resources.cpp
 * @brief Package C of docs/devel/plans/2026-09-20-bondtech-indx.md: shared
 * extruder/heater resources, no-active-tool identity through the topology
 * bridge, nozzle-identity vs extruder-count, per-slot consumption
 * attribution for a shared-resource nozzle changer, and §6.2's temperature-
 * transient / Z-offset compatibility.
 *
 * Covers §6, §6.1, §6.2 and the "Shared resources" / "Consumption" /
 * "Temperature transient" / "Z-offset compatibility" rows of §10's
 * regression matrix.
 */

#include "ui_ams_tool_text.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_tool_topology.h"
#include "app_globals.h"
#include "filament_consumption_tracker.h"
#include "filament_consumption_tracker_test_access.h"
#include "printer_motion_state.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "static_subject_registry.h"
#include "test_helpers/toolchanger_test_access.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using Catch::Approx;
using helix::AmsState;
using helix::FilamentConsumptionTracker;
using helix::FilamentConsumptionTrackerTestAccess;
using helix::PrinterMotionState;
using helix::PrinterTemperatureState;
using helix::ToolChangerTestAccess;
using helix::ToolState;
using helix::ToolTopology;
using helix::ui::UpdateQueue;

namespace {

/// Mirrors test_ams_toolchanger_indx_backend.cpp's fixture — an INDX-shaped
/// ToolCommands (working T<n> shortcut for every configured tool, PARK_TOOL
/// present), which is what makes AmsBackendToolChanger answer
/// shared_extruder_name() and negative_active_tool_is_unreported().
helix::toolchanger_addon::ToolCommands indx_commands(int tool_count) {
    helix::toolchanger_addon::ToolCommands c;
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
    names.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        names.push_back(std::to_string(i));
    }
    return names;
}

json save_variables_delta(int active_tool) {
    return json{{"save_variables", {{"variables", {{"active_tool", active_tool}}}}}};
}

json notification(const json& params) {
    return json{{"method", "notify_status_update"}, {"params", json::array({params, 0.0})}};
}

struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

// =============================================================================
// Shared extruder/heater mapping (plan §6)
// =============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "INDX-shaped topology maps every tool to the shared extruder, not a "
                 "positional subset",
                 "[indx][resources][tool-state]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(4));
    backend.set_tool_commands(indx_commands(4));

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    REQUIRE(topo->shared_extruder_name.has_value());
    CHECK(*topo->shared_extruder_name == "extruder");

    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    auto& ts = ToolState::instance();
    REQUIRE(ts.tool_count() == 4);
    // Every tool — not just tool 0 — carries the shared extruder. Before this
    // fix, init_tools()'s positional carry-over only ever covered tools up to
    // the physical extruder count (one), leaving tools 1-3 unmapped.
    for (int i = 0; i < 4; ++i) {
        const auto& tool = ts.tools()[static_cast<size_t>(i)];
        REQUIRE(tool.extruder_name.has_value());
        CHECK(*tool.extruder_name == "extruder");
        CHECK(tool.effective_heater() == "extruder");
    }
    // One physical extruder despite four tools: exactly the distinction
    // has_multiple_extruders() must preserve for heater controls.
    CHECK(ts.extruder_count() == 1);
    CHECK_FALSE(ts.has_multiple_extruders());
}

TEST_CASE_METHOD(ToolStateFixture, "an ordinary toolchanger reports no shared extruder name",
                 "[indx][resources][tool-state][regression]") {
    // Regression: a plain klipper-toolchanger (tool_commands_ default,
    // present == false) must not pick up the shared-resource mapping.
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(4));

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    CHECK_FALSE(topo->shared_extruder_name.has_value());
    CHECK_FALSE(topo->active_tool_unreported);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "a plain multi-extruder printer keeps its T0 default active tool",
                 "[indx][resources][tool-state][regression]") {
    // resolve_tool_commands() answers present == true for ANY printer without
    // [toolchanger], including a dual-extruder/IDEX box with no changer at
    // all. Nothing there ever writes current_tool, so treating its negative
    // reading as "unreported" would leave it with no active tool forever.
    helix::toolchanger_addon::ToolCommands plain;
    plain.present = true;
    plain.provider_name = "";
    plain.select_prefix = "T";

    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_tool_commands(plain);
    backend.set_discovered_tools(discovered_tools(2));

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    CHECK_FALSE(topo->shared_extruder_name.has_value());
    CHECK_FALSE(topo->active_tool_unreported);
}

// =============================================================================
// No-active-tool / unreported identity through the topology bridge (plan §6,
// "no automatic T0 default" — before this fix set_ams_topology() converted
// EVERY negative active_tool to T0, which is still correct for Happy Hare
// bypass / disconnected-data backends and must stay that way.
// =============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "INDX topology with no reported active tool leaves ToolState unreported, not T0",
                 "[indx][resources][tool-state]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));
    // No save_variables delta has ever arrived: get_current_tool() reads the
    // AmsSystemInfo default of -1.
    REQUIRE(backend.get_current_tool() == -1);

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    REQUIRE(topo->active_tool_unreported);

    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    CHECK(ToolState::instance().active_tool_index() == -1);
    CHECK(ToolState::instance().active_tool() == nullptr);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "a valid INDX active tool overrides the unreported state, and parking returns to it",
                 "[indx][resources][tool-state]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));
    REQUIRE(backend.get_current_tool() == 2);

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    // Explicit park (-1) is a VALID reading, not an absence, and it is still
    // "unreported" as far as ToolState's active-tool concept goes: no tool is
    // mounted, so no T0 highlight.
    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(-1)));
    REQUIRE(backend.get_current_tool() == -1);

    auto topo2 = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo2.has_value());
    ToolState::instance().set_ams_topology(*topo2);
    UpdateQueue::instance().drain();

    CHECK(ToolState::instance().active_tool_index() == -1);
    CHECK(ToolState::instance().active_tool() == nullptr);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "a backend with its own negative-value meaning still falls back to T0",
                 "[indx][resources][tool-state][regression]") {
    // Pins the existing AFC-bypass-shaped convention: negative_active_tool_is_
    // unreported() is false by default, so set_ams_topology() must keep
    // coercing a negative active_tool to T0 for every backend that has not
    // opted in, exactly as before this feature.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = -2; // Happy Hare bypass shape
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.active_tool_unreported = false;

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    CHECK(ToolState::instance().active_tool_index() == 0);
    CHECK(ToolState::instance().active_tool() != nullptr);
}

// =============================================================================
// Nozzle identity vs extruder count (plan §6, ui_ams_tool_text.cpp badge)
// =============================================================================

namespace {

struct ToolBadgeFixture : public LVGLTestFixture {
    ToolBadgeFixture() {
        AmsState::instance().init_subjects(/*register_xml=*/false);
        ToolState::instance().init_subjects(/*register_xml=*/false);
        helix::ui::init_ams_tool_text_observers();
    }
    ~ToolBadgeFixture() override {
        StaticSubjectRegistry::instance().deinit_one("AmsToolTextObservers");
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }
    static std::string badge_text() {
        return lv_subject_get_string(ToolState::instance().get_tool_badge_text_subject());
    }
    static int badge_shown() {
        return lv_subject_get_int(ToolState::instance().get_show_tool_badge_subject());
    }
};

} // namespace

TEST_CASE_METHOD(ToolBadgeFixture,
                 "a shared-resource nozzle changer gets a numeric badge despite one extruder",
                 "[indx][resources][tool-badge]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));
    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(1)));

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    REQUIRE_FALSE(ToolState::instance().has_multiple_extruders());
    CHECK(ToolState::instance().has_multiple_nozzles());
    CHECK(badge_shown() == 1);
    CHECK(badge_text() == "2"); // 1-based: storage index 1 displays as "2"
    CHECK(ToolState::instance().nozzle_label() == "Nozzle 2");
}

TEST_CASE_METHOD(ToolBadgeFixture, "a parked/unreported shared-resource tool hides the badge",
                 "[indx][resources][tool-badge]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));
    // No save_variables delta at all: unreported.

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().active_tool_index() == -1);
    CHECK(badge_shown() == 0);
    CHECK(badge_text().empty());
    // Generic label, no T0 guess.
    CHECK(ToolState::instance().nozzle_label() == "Nozzle");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "single-nozzle multi-lane AFC badges are unaffected by has_multiple_nozzles()",
                 "[indx][resources][regression]") {
    // The AFC/CFS/HH shape: many filament lanes, one nozzle, no shared_extruder_name
    // (only AmsBackendToolChanger ever sets that). has_multiple_nozzles() must
    // stay false here exactly as has_multiple_extruders() already did.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    CHECK_FALSE(ToolState::instance().has_multiple_extruders());
    CHECK_FALSE(ToolState::instance().has_multiple_nozzles());
    CHECK(ToolState::instance().nozzle_label() == "Nozzle");
}

// =============================================================================
// Reverse resource-to-tool lookup: the valid active tool, not the first match
// (plan §6: "reverse lookup uses the valid active tool, not the first
// matching tool")
// =============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "tool_name_for_extruder resolves the ACTIVE tool when several share one extruder",
                 "[indx][resources][tool-state]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));
    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    auto& ts = ToolState::instance();
    REQUIRE(ts.active_tool_index() == 2);
    // Before this fix, the first positional match (tool 0) would win here
    // regardless of which tool was actually mounted.
    CHECK(ts.tool_name_for_extruder("extruder") == ts.tools()[2].name);
    CHECK(ts.display_label_for_extruder("extruder") == ts.tools()[2].display_label);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "tool_name_for_extruder answers nothing specific when identity is unreported",
                 "[indx][resources][tool-state]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));
    // Never reported.

    auto topo = helix::build_ams_topology(&backend, 0);
    REQUIRE(topo.has_value());
    ToolState::instance().set_ams_topology(*topo);
    UpdateQueue::instance().drain();

    auto& ts = ToolState::instance();
    REQUIRE(ts.active_tool_index() == -1);
    CHECK(ts.tool_name_for_extruder("extruder").empty());
    CHECK(ts.display_label_for_extruder("extruder").empty());
}

TEST_CASE_METHOD(ToolStateFixture,
                 "a lane-expanded single-nozzle AMS keeps its stable first-match label",
                 "[indx][resources][tool-state][regression]") {
    // Every lane-expanded AMS (AFC, Happy Hare, CFS, QIDI, AD5X IFS) also
    // leaves every tool on ToolInfo's default "extruder" name, so the lookup
    // above is ambiguous there too -- but those lanes feed ONE nozzle. There is
    // no mounted tool to identify, and answering with the loaded lane would
    // rename the single nozzle row (NozzleTempsWidget::create_extruder_row())
    // every time the lane changed. Only a shared-NOZZLE topology resolves.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    // No shared_extruder_name: these are lanes feeding one hot end, not
    // several nozzles sharing one extruder.
    REQUIRE_FALSE(topo.shared_extruder_name.has_value());
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    auto& ts = ToolState::instance();
    REQUIRE(ts.active_tool_index() == 2);
    REQUIRE(ts.tools().size() == 4);
    // All four claim the default "extruder"...
    REQUIRE(ts.tools()[0].extruder_name.value_or("") == "extruder");
    REQUIRE(ts.tools()[2].extruder_name.value_or("") == "extruder");
    // ...and the answer is the first of them, whatever lane is loaded.
    CHECK(ts.tool_name_for_extruder("extruder") == ts.tools()[0].name);
    CHECK(ts.display_label_for_extruder("extruder") == ts.tools()[0].display_label);
}

// =============================================================================
// slot_for_extruder() and consumption attribution (plan §6, §9 Package C item 3)
// =============================================================================

TEST_CASE("INDX-shaped backend reports no per-extruder slot mapping",
          "[indx][resources][consumption]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(4));
    backend.set_tool_commands(indx_commands(4));

    for (int e = 0; e < 4; ++e) {
        CHECK_FALSE(backend.slot_for_extruder(e).has_value());
    }
}

TEST_CASE("a plain toolchanger keeps identity slot_for_extruder mapping",
          "[indx][resources][consumption][regression]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(4));
    // tool_commands_ default: no INDX shape.

    for (int e = 0; e < 4; ++e) {
        REQUIRE(backend.slot_for_extruder(e).has_value());
        CHECK(*backend.slot_for_extruder(e) == e);
    }
    CHECK_FALSE(backend.slot_for_extruder(4).has_value()); // out of range
}

namespace {

/// One INDX-shaped backend registered with the real AmsState/FilamentConsumptionTracker
/// pipeline, so consumption routing is exercised through the actual sink
/// registration AmsState::add_backend() performs — not a hand-rolled substitute.
struct IndxConsumptionFixture : public LVGLTestFixture {
    helix::AmsBackendToolChanger* backend = nullptr;
    int backend_index = -1;

    IndxConsumptionFixture() {
        auto& ams = AmsState::instance();
        auto& printer = get_printer_state();
        ams.clear_backends();
        ams.deinit_subjects();
        printer.init_subjects(false);
        ams.init_subjects(false);

        auto b = std::make_unique<helix::AmsBackendToolChanger>(nullptr, nullptr);
        backend = b.get();
        backend->set_discovered_tools(discovered_tools(4));
        backend->set_tool_commands(indx_commands(4));
        backend_index = ams.add_backend(std::move(b));

        // Seed slots 0 and 3 with trackable PLA metadata; leave 1/2 unset so an
        // accidental attribution to the wrong slot is visible as "unknown".
        helix::SlotInfo info0 = backend->get_slot_info(0);
        info0.material = "PLA";
        info0.remaining_weight_g = 1000.0f;
        info0.total_weight_g = 1000.0f;
        info0.spoolman_id = 0;
        backend->sync_external_identity(0, info0);

        helix::SlotInfo info3 = backend->get_slot_info(3);
        info3.material = "PLA";
        info3.remaining_weight_g = 500.0f;
        info3.total_weight_g = 500.0f;
        info3.spoolman_id = 0;
        backend->sync_external_identity(3, info3);
    }

    ~IndxConsumptionFixture() override {
        AmsState::instance().clear_backends();
        AmsState::instance().deinit_subjects();
    }

    float remaining(int slot) const {
        return backend->get_slot_info(slot).remaining_weight_g;
    }

    void set_active_tool(int tool) {
        ToolChangerTestAccess::handle_status(*backend, notification(save_variables_delta(tool)));
    }
};

} // namespace

TEST_CASE_METHOD(IndxConsumptionFixture,
                 "two-tool trace: T0 -> T3 -> park -> reset attributes deltas to the correct "
                 "slot with no double counting",
                 "[indx][resources][consumption]") {
    auto& tracker = FilamentConsumptionTracker::instance();
    auto& printer = get_printer_state();

    set_active_tool(0);
    tracker.start();
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    UpdateQueue::instance().drain();

    // The per-extruder path must be a no-op for this capability: slot_for_extruder()
    // returns nullopt for every extruder, so a delta arriving there must not
    // ALSO be applied — that would double-count against the aggregate path below.
    FilamentConsumptionTrackerTestAccess::on_extruder_filament_used(0, 1000);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(1000.0f)); // unchanged

    // Aggregate path: 1000mm of 1.75mm PLA (1.24 g/cm^3) consumed while T0 is
    // mounted attributes ~2.982g to slot 0, and only slot 0.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(997.018f).margin(0.05));
    CHECK(remaining(3) == Approx(500.0f)); // untouched while T0 is mounted

    // Swap to T3: further consumption must land on slot 3, and slot 0 must not
    // move again for a delta it was never mounted for. Routing keys off
    // backend->get_current_slot() directly (on_filament_used_changed()'s
    // aggregate path), independent of ToolState.
    set_active_tool(3);
    REQUIRE(backend->get_current_slot() == 3);
    UpdateQueue::instance().drain();

    // The first notification after a swap contains real extrusion by the new
    // tool. Rebaseline T3 at the previous aggregate reading (1000), then charge
    // this notification's 30mm to T3 without including T0's earlier history.
    // 30mm (0.089 g) clears AmsSlotSink's DELTA_WRITE_THRESHOLD_G; a smaller
    // delta is coalesced rather than written, so it would prove nothing about
    // WHICH slot the tracker chose.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 1030);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(997.018f).margin(0.05)); // frozen at T0's value
    CHECK(remaining(3) == Approx(499.911f).margin(0.01)); // first post-swap delta charged

    // Further extrusion while T3 stays continuously mounted brings its own
    // total to exactly 1000mm, and slot 0 still does not move.
    lv_subject_set_int(printer.get_print_filament_used_subject(), 2000);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(997.018f).margin(0.05)); // frozen at T0's value
    CHECK(remaining(3) == Approx(497.018f).margin(0.05)); // T3's OWN delta, not the print's total

    // Park: no tool mounted, so no slot may accrue the next delta at all — it
    // must not be silently charged to whichever slot was last active either.
    set_active_tool(-1);
    lv_subject_set_int(printer.get_print_filament_used_subject(), 3000);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(997.018f).margin(0.05));
    CHECK(remaining(3) == Approx(497.018f).margin(0.05));

    // Remounting the same tool still starts a new attribution window. The
    // parked 1000mm remains uncharged, while the first 30mm after remount is
    // charged to T3. 30mm again, and a margin tighter than the delta: at 10mm
    // the expected and unchanged values both sit inside a 0.05 g margin, so the
    // assertion holds whether or not the parked window was excluded.
    set_active_tool(3);
    lv_subject_set_int(printer.get_print_filament_used_subject(), 3030);
    UpdateQueue::instance().drain();
    CHECK(remaining(0) == Approx(997.018f).margin(0.05));
    CHECK(remaining(3) == Approx(496.928f).margin(0.01));

    // Reset: print completes and a new one starts. The tracker re-snapshots
    // from each slot's OWN current remaining weight, not a stale running
    // total, so the next print's first delta is not a "catch-up" against
    // whatever accrued (or didn't) during the parked window above.
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::COMPLETE));
    UpdateQueue::instance().drain();

    set_active_tool(0);
    lv_subject_set_int(printer.get_print_filament_used_subject(), 0);
    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    UpdateQueue::instance().drain();

    lv_subject_set_int(printer.get_print_filament_used_subject(), 1000);
    UpdateQueue::instance().drain();
    // Second print's 1000mm on T0 charges the SAME per-print delta again from
    // slot 0's now-current weight — no leftover catch-up from the first print.
    CHECK(remaining(0) == Approx(994.036f).margin(0.05));
    CHECK(remaining(3) == Approx(496.928f).margin(0.01)); // untouched this print

    lv_subject_set_int(printer.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::COMPLETE));
    UpdateQueue::instance().drain();
    tracker.stop();
}

// =============================================================================
// §6.2: stock macro temperature transient (plan §9 Package C item 5,
// §10 "Temperature transient" row)
//
// The pinned CHANGE_TOOL sends M104 S0 before motion; _PICKUP_TOOL restores a
// positive target through blocking M109 after locking. INDX has exactly one
// Klipper `extruder` object (its shared_extruder_name()), so this replay is
// PrinterTemperatureState's existing single-extruder machinery observing
// ordinary status updates on the name "extruder" — nothing here is a new
// per-tool code path. The assertions pin the plan's contract: truthful live
// display at every step, and the last-nonzero latch surviving the zero so a
// caller (purge, print-status heating indicators) is not misled into
// reporting completion on the intermediate M104 S0.
// =============================================================================

TEST_CASE("a CHANGE_TOOL-shaped hot -> zero -> restored replay keeps the live "
          "target truthful and retains the last-nonzero latch",
          "[indx][resources][temperature]") {
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    // T0 is mounted and hot.
    state.update_from_status({{"extruder", {{"temperature", 248.0}, {"target", 250.0}}}});
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2500);
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Approx(250.0));

    // CHANGE_TOOL's M104 S0 fires before motion: the live target must read a
    // truthful zero, not the pre-swap value, and must not be reported as
    // "operation complete" by anything reading it.
    state.update_from_status({{"extruder", {{"temperature", 230.0}, {"target", 0.0}}}});
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 0);
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Approx(250.0));

    // _PICKUP_TOOL's blocking M109 restores a positive target for the newly
    // mounted tool (may differ from the parked tool's material).
    state.update_from_status({{"extruder", {{"temperature", 235.0}, {"target", 245.0}}}});
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2450);
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Approx(245.0));
}

// =============================================================================
// §6.2: Z-offset compatibility (plan §9 Package C item 5, §10 "Z-offset
// compatibility" row)
//
// INDX bakes gcode_move.homing_origin.z = tool_z + global_z and overrides
// Z_OFFSET_APPLY_PROBE to restore the tool component. HelixScreen displays
// the reported combined origin as-is (no decomposition, no per-tool editor —
// out of scope per §12) and must not corrupt the tune overlay's session
// travel guard when a swap changes that combined value out from under it.
// =============================================================================

TEST_CASE("PrinterMotionState reports a swap-driven combined Z origin truthfully, "
          "with no decomposition attempted",
          "[indx][resources][zoffset]") {
    PrinterMotionState state;
    state.init_subjects(false);

    // T0 mounted: tool_z(T0) + global bakes to -0.05mm.
    state.update_from_status(
        json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, -0.05, 0.0}}}}});
    REQUIRE(lv_subject_get_int(state.get_gcode_z_offset_subject()) == -50);

    // Swap to T3: INDX's _PICKUP_TOOL bakes a different tool_z with the SAME
    // global component, so the reported origin jumps to +0.30mm with no user
    // babystepping involved. HelixScreen displays exactly what Klipper
    // reports; it must not try to subtract out a "tool" component itself.
    state.update_from_status(
        json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, 0.30, 0.0}}}}});
    REQUIRE(lv_subject_get_int(state.get_gcode_z_offset_subject()) == 300);
}

TEST_CASE("the tune overlay's session-travel guard is not corrupted when the swap "
          "changes the combined offset out from under an open session",
          "[indx][resources][zoffset]") {
    // The tune overlay opens on T0's combined origin (-0.05mm) and caches it as
    // session_base_mm (PrintTuneOverlay::sync_to_state()). update_z_offset_display()
    // deliberately does not re-baseline that cache on every observed change
    // (see its comment in ui_print_tune_overlay.cpp), so a swap to T3 while the
    // overlay stays open leaves session_base_mm stale at -0.05mm even though
    // the live combined offset is now +0.30mm.
    //
    // helix::zoffset::adjust() already defends exactly this "base gone stale"
    // shape generically (see its comment: "should the base ever go stale the
    // worst outcome is a refused step rather than a jump") by widening its
    // clamp window to always include the current offset. This test pins that
    // INDX's swap lands inside that existing invariant rather than needing a
    // narrow INDX-specific fix: no double subtraction, no snap back toward the
    // stale base, and a small step from the new live value still lands where
    // requested.
    constexpr double session_base_mm = -0.05; // stale: cached before the swap
    constexpr double current_offset_mm = 0.30; // live: T3's combined origin

    auto r = helix::zoffset::adjust(/*api=*/nullptr, /*ps=*/nullptr, session_base_mm,
                                    current_offset_mm, /*delta_mm=*/0.01);

    // Not clamped to a no-op: the widened window includes current_offset_mm,
    // so an ordinary small step from the NEW live value is honored exactly,
    // not measured against the stale pre-swap base.
    CHECK_FALSE(r.clamped_to_noop);
    CHECK(r.applied_delta_mm == Approx(0.01));
    CHECK(r.new_offset_mm == Approx(0.31));
}
