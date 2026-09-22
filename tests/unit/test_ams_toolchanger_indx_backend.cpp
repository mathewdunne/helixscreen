// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_indx_backend.cpp
 * @brief AmsBackendToolChanger's Bondtech INDX integration: package B of
 * docs/devel/plans/2026-09-20-bondtech-indx.md.
 *
 * Covers (see plan §9 "Package B" and §10):
 *  - the resolved INDX active-tool reader bound only to its own provider
 *    instance, with strict absent/malformed/valid delta handling;
 *  - §7.1's default command choice and availability gating (T<n> shortcut,
 *    CHANGE_TOOL fallback, explicit unsupported when neither exists);
 *  - §7.3's async-failure unwind fix for dispatch_operation(), exercised
 *    through the real backend against a live IMoonrakerAPI/MoonrakerClientMock
 *    so the fix is proven against the actual RPC error path, not a
 *    hand-rolled substitute;
 *  - §7.3's unsupported reset/recovery (no INITIALIZE_TOOLCHANGER substitute).
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_toolchanger.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/scoped_home_confirm_prompter.h"
#include "test_helpers/toolchanger_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::AmsAction;
using helix::AmsErrorHelper;
using helix::ToolChangerTestAccess;

namespace {

/// A minimal INDX-shaped ToolCommands: the fields
/// toolchanger_addon::resolve_tool_commands() would produce for a printer
/// whose configured tools all have a working T<n> shortcut.
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

json save_variables_delta(int active_tool) {
    return json{{"save_variables", {{"variables", {{"active_tool", active_tool}}}}}};
}

json notification(const json& params) {
    return json{{"method", "notify_status_update"}, {"params", json::array({params, 0.0})}};
}

std::vector<std::string> discovered_tools(int count) {
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        names.push_back(std::to_string(i));
    }
    return names;
}

/// Real backend against a real MoonrakerAPI/MoonrakerClientMock pair, so the
/// async-error unwind fix is exercised through the actual RPC callback chain
/// dispatch_operation() uses in production, not a synchronous virtual-method
/// substitute. See dispatch_payload()'s own doc comment: any non-default
/// on_error/timeout/caller_surfaces_errors combination requires a live api_.
class LiveIndxHarness : public LVGLTestFixture {
  public:
    explicit LiveIndxHarness(int tool_count = 3)
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state), backend(&api, nullptr) {
        state.init_subjects(false);
        // Homed by default so ensure_homed_then() skips G28 and goes straight
        // to the payload -- most cases here are about the PAYLOAD's own
        // async failure, not the pre-op home.
        lv_subject_copy_string(state.get_homed_axes_subject(), "xyz");

        backend.set_discovered_tools(discovered_tools(tool_count));
        backend.set_tool_commands(indx_commands(tool_count));
        ToolChangerTestAccess::mark_running(backend);
    }

    ~LiveIndxHarness() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void set_homed(bool homed) {
        lv_subject_copy_string(state.get_homed_axes_subject(), homed ? "xyz" : "");
    }

    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPI api;
    helix::AmsBackendToolChanger backend;
};

} // namespace

// =============================================================================
// Cross-provider isolation and strict delta/validity handling (plan §5.2, §10)
// =============================================================================

TEST_CASE("ToolChanger/INDX: expected-hardware marker names the detected provider",
          "[indx][backend][hardware]") {
    helix::AmsBackendToolChanger indx(nullptr, nullptr);
    indx.set_tool_commands(indx_commands(3));
    CHECK(std::string(indx.get_klipper_object_name()) == "indx");

    helix::AmsBackendToolChanger native(nullptr, nullptr);
    CHECK(std::string(native.get_klipper_object_name()) == "toolchanger");
}

TEST_CASE("ToolChanger/INDX: a valid save_variables delta sets the mounted tool",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(6));
    backend.set_tool_commands(indx_commands(6));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(3)));

    CHECK(backend.get_current_tool() == 3);
    CHECK(backend.get_current_slot() == 3);
    CHECK(backend.get_system_info().filament_loaded);
}

TEST_CASE("ToolChanger/INDX: explicit park (-1) clears the mounted tool",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(6));
    backend.set_tool_commands(indx_commands(6));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));
    REQUIRE(backend.get_current_tool() == 2);

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(-1)));
    CHECK(backend.get_current_tool() == -1);
    CHECK_FALSE(backend.get_system_info().filament_loaded);
}

TEST_CASE("ToolChanger/INDX: an absent active_tool field preserves the last known identity",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(6));
    backend.set_tool_commands(indx_commands(6));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(4)));
    REQUIRE(backend.get_current_tool() == 4);

    // A delta naming an unrelated saved variable, not active_tool at all.
    ToolChangerTestAccess::handle_status(
        backend, notification(json{{"save_variables", {{"variables", {{"other_var", 1}}}}}}));
    CHECK(backend.get_current_tool() == 4);
}

TEST_CASE("ToolChanger/INDX: a malformed active_tool is held, never a successful park",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(6));
    backend.set_tool_commands(indx_commands(6));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));
    REQUIRE(backend.get_current_tool() == 2);

    // Out of range for a 6-tool inventory: present but unusable this frame.
    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(99)));
    CHECK(backend.get_current_tool() == 2);
    CHECK(backend.get_system_info().filament_loaded);
}

TEST_CASE("ToolChanger/INDX: no saved active_tool at all leaves identity unreported, not T0",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));

    // A frame that names other objects but never save_variables at all.
    ToolChangerTestAccess::handle_status(backend, notification(json{{"toolchanger", {}}}));
    CHECK(backend.get_current_tool() == -1);
}

TEST_CASE("ToolChanger/INDX: an unbound instance (no tool_commands_) ignores save_variables",
          "[indx][backend][toolchanger][priority]") {
    // Simulates a plain klipper-toolchanger instance -- tool_commands_.present
    // stays false, and an INDX-shaped save_variables delta must never reach
    // it (the global toolchanger_addon::read_tool() dispatch is a different,
    // MedusaHC-only code path; this proves the BACKEND-level gate as well).
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));
    CHECK(backend.get_current_tool() == -1);
}

TEST_CASE("ToolChanger/INDX: a MedusaHC-provider instance ignores save_variables too",
          "[indx][backend][toolchanger][priority]") {
    // provider_name != "INDX" -- the fork-without-klipper-toolchanger
    // MedusaHC shape also sets tool_commands_.present, and an INDX-style
    // active_tool field must not leak into it either.
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));
    helix::toolchanger_addon::ToolCommands medusa;
    medusa.present = true;
    medusa.provider_name = "MedusaHC";
    medusa.select_prefix = "T";
    medusa.unselect = "DROP_TOOL";
    backend.set_tool_commands(medusa);

    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(2)));
    CHECK(backend.get_current_tool() == -1);
}

// =============================================================================
// §7.1 default command choice and availability gating
// =============================================================================

TEST_CASE("ToolChanger/INDX: default select uses the T<n> shortcut when present",
          "[indx][backend][toolchanger]") {
    class CapturingBackend : public helix::AmsBackendToolChanger {
      public:
        CapturingBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        // The macro-ack completion path (finalize_dispatch_after_macro) always
        // marshals through UpdateQueue via token.defer(), even on this
        // null-api/synchronous-on_complete route -- drain so the queued work
        // does not leak into a later test (scripts/check_update_queue_leaks.py).
        ~CapturingBackend() override {
            helix::ui::UpdateQueue::instance().drain();
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            captured.push_back(gcode);
            return AmsErrorHelper::success();
        }
        helix::AmsError execute_gcode(const std::string& gcode,
                                      std::function<void()> on_complete) override {
            captured.push_back(gcode);
            if (on_complete) {
                on_complete();
            }
            return AmsErrorHelper::success();
        }
        std::vector<std::string> captured;
    };

    CapturingBackend backend;
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));

    REQUIRE(backend.load_filament(2).success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "T2");
}

TEST_CASE("ToolChanger/INDX: do_change_tool falls back to CHANGE_TOOL when the shortcut is missing",
          "[indx][backend][toolchanger]") {
    class CapturingBackend : public helix::AmsBackendToolChanger {
      public:
        CapturingBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        ~CapturingBackend() override {
            helix::ui::UpdateQueue::instance().drain();
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            captured.push_back(gcode);
            return AmsErrorHelper::success();
        }
        helix::AmsError execute_gcode(const std::string& gcode,
                                      std::function<void()> on_complete) override {
            captured.push_back(gcode);
            if (on_complete) {
                on_complete();
            }
            return AmsErrorHelper::success();
        }
        std::vector<std::string> captured;
    };

    CapturingBackend backend;
    backend.set_discovered_tools(discovered_tools(3));

    // Tool 2 has no T2 shortcut, but CHANGE_TOOL is present.
    helix::toolchanger_addon::ToolCommands commands = indx_commands(3);
    commands.select_shortcut_available = {true, true, false};
    backend.set_tool_commands(commands);

    auto err = backend.load_filament(0);
    REQUIRE(err.success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "T0");

    // The macro-ack completion route resolves through UpdateQueue
    // (token.defer()), so the first dispatch's action must be drained back to
    // IDLE before a second dispatch is attempted, or it is refused as busy.
    helix::ui::UpdateQueue::instance().drain();

    err = backend.load_filament(2);
    REQUIRE(err.success());
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[1] == "CHANGE_TOOL TOOL=2");
}

TEST_CASE("ToolChanger/INDX: neither shortcut nor CHANGE_TOOL is an explicit unsupported send",
          "[indx][backend][toolchanger]") {
    class CapturingBackend : public helix::AmsBackendToolChanger {
      public:
        CapturingBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            captured.push_back(gcode);
            return AmsErrorHelper::success();
        }
        helix::AmsError execute_gcode(const std::string& gcode,
                                      std::function<void()> on_complete) override {
            captured.push_back(gcode);
            if (on_complete) {
                on_complete();
            }
            return AmsErrorHelper::success();
        }
        std::vector<std::string> captured;
    };

    CapturingBackend backend;
    backend.set_discovered_tools(discovered_tools(2));

    helix::toolchanger_addon::ToolCommands commands;
    commands.present = true;
    commands.provider_name = "INDX";
    commands.select_prefix = "T";
    commands.unselect = ""; // PARK_TOOL absent too
    commands.select_shortcut_available = {true, false};
    // change_tool_macro left empty: no verified fallback on this printer.
    backend.set_tool_commands(commands);

    auto err = backend.load_filament(1);
    CHECK_FALSE(err.success());
    CHECK(backend.captured.empty());
}

TEST_CASE("ToolChanger/INDX: an absent PARK_TOOL macro makes unmount an explicit unsupported send",
          "[indx][backend][toolchanger]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    backend.set_discovered_tools(discovered_tools(3));

    helix::toolchanger_addon::ToolCommands commands = indx_commands(3);
    commands.unselect.clear(); // no PARK_TOOL on this printer
    backend.set_tool_commands(commands);

    // running_ is private; reach through ToolChangerTestAccess to keep this
    // case focused on the command gate rather than lifecycle plumbing.
    ToolChangerTestAccess::mark_running(backend);

    // Seat a tool first so unload_filament(-1) ("unmount the current tool")
    // does not refuse earlier on not_loaded() instead.
    ToolChangerTestAccess::handle_status(backend, notification(save_variables_delta(0)));
    REQUIRE(backend.get_current_tool() == 0);

    auto err = backend.unload_filament(-1);
    CHECK_FALSE(err.success());
}

// =============================================================================
// §5.1/§4 recover/reset: no INITIALIZE_TOOLCHANGER substitute without a real
// toolchanger object (plan §7.3)
// =============================================================================

TEST_CASE("ToolChanger/INDX: recover() and reset() are unsupported, never INITIALIZE_TOOLCHANGER",
          "[indx][backend][toolchanger]") {
    class CapturingBackend : public helix::AmsBackendToolChanger {
      public:
        CapturingBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            captured.push_back(gcode);
            return AmsErrorHelper::success();
        }
        std::vector<std::string> captured;
    };

    CapturingBackend backend;
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));

    CHECK_FALSE(backend.recover().success());
    CHECK_FALSE(backend.reset().success());
    CHECK(backend.captured.empty());
}

TEST_CASE("ToolChanger: a plain klipper-toolchanger keeps INITIALIZE_TOOLCHANGER recovery",
          "[toolchanger][backend]") {
    // Regression: tool_commands_.present stays false (default) for a real
    // klipper-toolchanger install, and recover()/reset() must be unaffected
    // by the INDX-motivated gate above.
    class CapturingBackend : public helix::AmsBackendToolChanger {
      public:
        CapturingBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            captured.push_back(gcode);
            return AmsErrorHelper::success();
        }
        std::vector<std::string> captured;
    };

    CapturingBackend backend;
    backend.set_discovered_tools(discovered_tools(3));

    CHECK(backend.recover().success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "INITIALIZE_TOOLCHANGER");

    CHECK(backend.reset().success());
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[1] == "INITIALIZE_TOOLCHANGER");
}

// =============================================================================
// §7.3 async-failure unwind, against a real IMoonrakerAPI/MoonrakerClientMock
// =============================================================================

TEST_CASE_METHOD(LiveIndxHarness, "dispatch_operation unwinds on an async RPC rejection",
                 "[indx][backend][toolchanger][error-unwind]") {
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Tool not present", "T1");

    auto err = backend.load_filament(1);
    REQUIRE(err.success()); // the SEND was accepted; the failure is async

    // The optimistic action must be visible before the async failure lands --
    // otherwise the busy-leg half of this fix would be unproven.
    REQUIRE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.get_current_action() == AmsAction::SELECTING);

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK_FALSE(backend.get_system_info().is_busy());
}

TEST_CASE_METHOD(LiveIndxHarness, "dispatch_operation unwinds on an async timeout",
                 "[indx][backend][toolchanger][error-unwind]") {
    client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "Request timed out", "T0");

    auto err = backend.load_filament(0);
    REQUIRE(err.success());
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // A timeout is not proof the physical macro stopped -- it must still
    // release the optimistic busy state rather than leaving a permanent
    // spinner, and must not retry automatically (only one T0 send).
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(std::count(client.gcode_script_history().begin(), client.gcode_script_history().end(),
                     "T0") == 1);
}

TEST_CASE_METHOD(LiveIndxHarness, "dispatch_operation gives the ack-owned swap a longer timeout",
                 "[indx][backend][toolchanger][error-unwind]") {
    // With no toolchanger status frames the ack is the only completion signal,
    // and the tracker drops it the moment the timeout fires -- so the ceiling
    // must sit above a heat-from-cold swap, not at the generic 5 min.
    REQUIRE(backend.load_filament(1).success());
    CHECK(client.last_send_timeout_ms() > helix::IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(LiveIndxHarness,
                 "dispatch_operation sends the payload unhomed instead of a G28 (plan §7.4, "
                 "package D: delegates_homing_to_printer())",
                 "[indx][backend][toolchanger][error-unwind]") {
    // Superseded by package D's §7.4 implementation: the stock INDX macros
    // home conditionally themselves, so AmsBackendToolChanger now answers
    // true from delegates_homing_to_printer() for this provider (scoped to
    // provider_name == "INDX", like shared_extruder_name()) and
    // ensure_homed_then() never synthesizes its own G28 or asks for
    // confirmation while unhomed -- idle or printing, the macro's own
    // conditional home runs unencumbered. (Package D's paused-print
    // precondition in dispatch_operation() is the separate, narrower guard
    // that keeps a PAUSED+unhomed dispatch from ever reaching this point;
    // see test_ams_toolchanger_indx_paused_homing.cpp.)
    set_homed(false);

    auto err = backend.load_filament(1);
    REQUIRE(err.success());
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    // No G28 at all -- the payload went straight out, unhomed.
    const auto& history = client.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(), "G28") == history.end());
    CHECK(std::find(history.begin(), history.end(), "T1") != history.end());
}

TEST_CASE_METHOD(LiveIndxHarness,
                 "a stale ack after an unwound dispatch resolves nothing, and a later "
                 "dispatch still works",
                 "[indx][backend][toolchanger][error-unwind]") {
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "boom", "T1");

    REQUIRE(backend.load_filament(1).success());
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);

    // A later status delta must not re-arm busy off the abandoned dispatch.
    ToolChangerTestAccess::handle_status(
        backend, notification(json{{"toolchanger", {{"status", "ready"}}}}));
    CHECK(backend.get_current_action() == AmsAction::IDLE);

    // The backend is not wedged: a subsequent dispatch succeeds normally.
    auto err = backend.load_filament(0);
    REQUIRE(err.success());
    CHECK(backend.get_current_action() == AmsAction::SELECTING);
    CHECK(std::find(client.gcode_script_history().begin(), client.gcode_script_history().end(),
                    "T0") != client.gcode_script_history().end());
}

TEST_CASE_METHOD(LiveIndxHarness, "a CONNECTION_LOST failure also unwinds the dispatch",
                 "[indx][backend][toolchanger][error-unwind]") {
    // A dropped connection surfaces through the same on_error this fix wires
    // up, with its own MoonrakerErrorType -- a distinct failure shape from a
    // JSON-RPC rejection or a timeout, even though this fix's unwind treats
    // all three uniformly (never retries, always abandons the generation).
    client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST, "connection lost", "T1");

    auto err = backend.load_filament(1);
    REQUIRE(err.success()); // the send was accepted; the failure is async
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK_FALSE(backend.get_system_info().is_busy());
}

// =============================================================================
// Package D's §7.4 implementation: delegates_homing_to_printer() (superseding
// the "asks like any other provider" placeholder B left here for D to fill).
// =============================================================================

TEST_CASE("ToolChanger/INDX: an unhomed dispatch is NEVER asked to confirm — the "
          "macro homes itself (plan §7.4)",
          "[indx][backend][toolchanger][homing]") {
    class ProbeBackend : public helix::AmsBackendToolChanger {
      public:
        ProbeBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        helix::AmsError execute_gcode(const std::string& gcode) override {
            sent.push_back(gcode);
            return AmsErrorHelper::success();
        }
        helix::AmsError execute_gcode(const std::string& gcode, std::function<void()>) override {
            sent.push_back(gcode);
            return AmsErrorHelper::success();
        }
        bool toolhead_homed() const override {
            return false;
        }
        std::vector<std::string> sent;
    };

    LVGLTestFixture fixture;
    ProbeBackend backend;
    backend.set_discovered_tools(discovered_tools(3));
    backend.set_tool_commands(indx_commands(3));

    bool asked = false;
    ScopedHomeConfirmPrompter guard(
        [&asked](std::function<void()>, std::function<void()>) { asked = true; });

    ToolChangerTestAccess::call_dispatch_operation(backend, "T1", AmsAction::SELECTING);

    // delegates_homing_to_printer() short-circuits ensure_homed_then() before
    // it ever asks toolhead_homed() -- no confirmation, no G28, the payload
    // goes straight out.
    CHECK_FALSE(asked);
    REQUIRE(backend.sent.size() == 1);
    CHECK(backend.sent[0] == "T1");
}

TEST_CASE("ToolChanger/INDX: a non-INDX ToolCommands::present provider keeps the "
          "existing confirmation gate (regression)",
          "[indx][backend][toolchanger][homing][regression]") {
    // delegates_homing_to_printer() is scoped to provider_name == "INDX",
    // like shared_extruder_name() -- a MedusaHC-shaped fork with no verified
    // self-homing contract keeps asking, exactly as every toolchanger did
    // before this package.
    class ProbeBackend : public helix::AmsBackendToolChanger {
      public:
        ProbeBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {
            running_ = true;
        }
        helix::AmsError execute_gcode(const std::string&) override {
            return AmsErrorHelper::success();
        }
        helix::AmsError execute_gcode(const std::string&, std::function<void()>) override {
            return AmsErrorHelper::success();
        }
        bool toolhead_homed() const override {
            return false;
        }
    };

    LVGLTestFixture fixture;
    ProbeBackend backend;
    backend.set_discovered_tools(discovered_tools(3));
    helix::toolchanger_addon::ToolCommands medusa = indx_commands(3);
    medusa.provider_name = "MedusaHC";
    backend.set_tool_commands(medusa);

    bool asked = false;
    std::function<void()> pending_cancel;
    ScopedHomeConfirmPrompter guard(
        [&asked, &pending_cancel](std::function<void()>, std::function<void()> cancel) {
            asked = true;
            pending_cancel = std::move(cancel);
        });

    ToolChangerTestAccess::call_dispatch_operation(backend, "T1", AmsAction::SELECTING);

    CHECK(asked);
    REQUIRE(pending_cancel);
    CHECK(ToolChangerTestAccess::has_pending_dispatch(backend));

    pending_cancel();
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.get_current_action() == AmsAction::IDLE);
}
