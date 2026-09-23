// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tool-changer add-ons: hardware bolted onto klipper-toolchanger that it does
// not model.
//
// klipper-toolchanger swaps a whole toolhead, and `toolchanger.tool_number` is
// simply whatever SELECT_TOOL last set. A hotend changer swaps only the hot end,
// which brings two things the toolchanger object cannot answer:
//
//   1. Which tool is PHYSICALLY on the head. MedusaHC ships toolchanger.cfg with
//      `verify_tool_pickup: False`, so klipper-toolchanger never checks; the
//      truth lives in dock sensors. A failed pickup leaves tool_number claiming
//      a tool that is not there.
//   2. A filament feeder. Only the hot end travels, so the filament is held by a
//      servo gripper on the frame that has to be released around a swap.
//
// This module is the ONLY place that knows which machines have those, what their
// status objects are called, and what gcode drives them. AmsBackendToolChanger
// and the subscription builder ask these functions and never name a machine.
//
// Adding a machine means adding one Provider to the table in
// toolchanger_addon.cpp - no call site changes.

#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::toolchanger_addon {

/// Sentinel meaning "use the detected default" in the settings picker.
inline constexpr const char* kAutoMacro = "auto";

/// ToolCommands::provider_name resolve_tool_commands() gives Bondtech INDX.
inline constexpr const char* kIndxProviderName = "INDX";

/// Filament feeder on the frame. Default-constructed is the "no feeder" answer,
/// so a tool changer nobody told anything exposes nothing.
struct Feeder {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs and the UI
    /// What the buttons actually send: the detected default, or the user's pick.
    std::string open_gcode;  ///< Releases the filament; empty when no feeder
    std::string close_gcode; ///< Re-grips it; empty when no feeder
    /// What detection chose, ignoring any override. Restored when the user
    /// picks "auto" again, so migrating to a controller that registers the
    /// native command is picked up without revisiting this setting.
    std::string detected_open;
    std::string detected_close;
    /// The user's stored choice: kAutoMacro, or an explicit macro name.
    std::string open_choice{kAutoMacro};
    std::string close_choice{kAutoMacro};
    /// Options for the settings picker: kAutoMacro followed by the plausible
    /// macros this printer reports. Empty when there is nothing to choose from.
    std::vector<std::string> macro_options;
};

/// What the add-on's own sensors say is on the head. Authoritative over
/// `toolchanger.tool_number` when a provider is present.
struct ToolReading {
    /// 0..N-1 mounted, -1 nothing on the head, -2 the sensors cannot tell.
    /// nullopt when this frame did not name it. Moonraker republishes only the
    /// fields that CHANGED, so a frame silent about the carriage is not a frame
    /// reporting an empty one - the same rule `docks` below is written to.
    std::optional<int> current_tool;
    /// current_tool == -2. A distinct state from "no tool": the machine does not
    /// KNOW, and acting on a guess would drive the carriage into a dock.
    bool sensor_error = false;
    /// The controller published `sensor_error` itself rather than us deriving it
    /// from -2. Only Irbis3D publishes the flag, and only the flag is a fault on
    /// its own: -2 is one value for two answers, a pin fault and "the switch
    /// pattern matches no settled configuration". A swap in flight is always the
    /// second, because the tool is between its dock and the head.
    bool sensor_error_reported = false;
    /// The machine's phase word, or empty when the frame did not say.
    ///
    /// The two controllers do NOT share a vocabulary, and this is deliberately
    /// the raw word rather than a normalised enum, because callers need to tell
    /// them apart:
    ///   Irbis3D MedusaHC-Python-Controller  `operation`: idle/picking/dropping
    ///   topi314/MedusaHC                    `state`:     uninitialized/ready/
    ///                                                    changing/error
    /// Verified against both sources, not inferred: `state` is a COARSER
    /// vocabulary than `operation`, not another spelling of it.
    std::string operation;
    /// True when `operation` came from the key that names the swap DIRECTION.
    /// False for a machine whose phase word is only ever "changing", which
    /// cannot say whether it is docking or picking. Available from the first
    /// status frame, unlike the phase words themselves, which only appear once a
    /// swap is already running - so this is what a caller keys on to decide what
    /// it can render BEFORE anything moves.
    bool phase_names_direction = false;
    /// 0 when this frame carried no tool count.
    int tool_count = 0;
    /// Per-dock occupancy, indexed by tool number: true seated, false empty,
    /// nullopt not reported in this frame. EMPTY when the frame carried no dock
    /// state at all - which is not the same answer as "every dock is vacant",
    /// and callers must not conflate them: Moonraker republishes only the fields
    /// that CHANGED.
    ///
    /// Upstream spells it `sensors` ({"e":1,"t0":1,...}), the fork spells it
    /// flat `tool<N>_docked` booleans. Same physical answer.
    std::vector<std::optional<bool>> docks;
    /// Whether anything is on the head at all (`sensors.e` / `head_loaded`).
    /// nullopt when the frame did not say.
    std::optional<bool> head_loaded;
    /// Frame-side gripper released. nullopt when this machine does not report it
    /// at all. BOTH MedusaHC controllers publish `feeder_open`, so in practice
    /// every machine carrying [medusahc] fills this in; the nullopt case is a
    /// changer with no such extra. The difference between "closed" and "never
    /// said" is what decides whether the step bar can name the release/grip
    /// phases (see AmsBackendToolChanger::get_operation_step_model).
    std::optional<bool> feeder_open;
};

/// How a swap is commanded on a machine where klipper-toolchanger is not the
/// one doing it. Default-constructed - `present` false - means the printer has
/// [toolchanger] and its SELECT_TOOL/UNSELECT_TOOL own the swap.
struct ToolCommands {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs
    /// Prefixed to the tool number: "T" sends T0, T1, ... Empty when absent.
    std::string select_prefix;
    /// Unmounts whatever is on the head. Empty when the machine has no such
    /// command and the tool can only be swapped for another.
    std::string unselect;
    /// Numbered tools (0..N-1, indexed to match) with a working `T<n>`
    /// shortcut macro on THIS printer. Empty means every numbered tool has
    /// one -- true by construction for the MedusaHC-shaped providers above,
    /// whose extra registers T<n> unconditionally. Bondtech INDX is the one
    /// provider where this can differ: a configured tool count can exceed
    /// the shortcuts a user's indx.cfg declares, so a numbered tool can have
    /// no working T<n> and must fall back to change_tool_macro.
    std::vector<bool> select_shortcut_available;
    /// The verified upstream fallback selection macro accepting a bare
    /// `TOOL=<n>` parameter (e.g. "CHANGE_TOOL"), or empty when this printer
    /// has none. Only consulted for a tool select_shortcut_available marks
    /// unavailable.
    std::string change_tool_macro;
};

/// Presence of an add-on dock sensor. When set, read_tool() is worth calling on
/// every status frame and its answer beats toolchanger.tool_number.
struct ToolSensor {
    bool present = false;
    std::string provider_name;
};

/// Whether any provider claims this printer.
bool present(const PrinterDiscovery& hw);

// --- Bondtech INDX ----------------------------------------------------------
//
// A nozzle changer with its own T<n>/PARK_TOOL commands and no
// klipper-toolchanger, several tools sharing one physical extruder/heater.
// Its inventory is not in printer.objects.list at all: the configured tool
// count lives in a runtime macro variable, discovered only once that macro's
// status is subscribed and read. This is why INDX is NOT folded into the
// MedusaHC-shaped Provider table above - that table answers "does this
// printer have a DOCK SENSOR / FEEDER add-on", which INDX has neither of, and
// its identity signal (`save_variables.active_tool`) is deliberately never
// merged into the shared read_tool() dispatch (see read_indx_active_tool()).
// Only resolve_tool_commands()'s existing generic seam gains an INDX default;
// resolve_tool_sensor()/resolve_feeder() correctly stay absent for it.

/// Bounds on INDX's provider-supplied tool inventory. Mirrors
/// `AmsState::MAX_SLOTS` (currently 16); kept as its own constant so this
/// low-level module does not depend on the UI-facing AmsState header.
inline constexpr int kIndxMaxTools = 16;

/// Whether the exact `indx` status object is present. The sole detection
/// signal for this delivery - see PrinterDiscovery::has_indx().
bool has_indx(const PrinterDiscovery& hw);

/// Whether this printer is an unresolved INDX inventory candidate: the exact
/// `indx` object is present and no other filament-management or tool-changer
/// backend has already claimed it from object-list facts alone. `parse_objects()`
/// cannot pick INDX's slot count itself - it lives in a runtime macro value,
/// not the object list - so a true result means the caller must subscribe
/// required_status_objects() and finalize inventory (read_indx_inventory())
/// before this printer's AMS backend can be selected. A false result here
/// with has_indx() true means a different backend legitimately outranks INDX
/// and its facts must not be disturbed.
bool is_indx_inventory_candidate(const PrinterDiscovery& hw);

/// The `gcode_macro <name>` status key carrying INDX's tool count, spelled the
/// way THIS printer's config spells the section, or empty when it has no such
/// macro. Klipper keys the status object on the config case while has_macro()
/// matches the uppercased alias, so subscribing (or looking up) a hardcoded
/// `gcode_macro TOOL_POSITIONS` silently reads nothing on a printer whose
/// indx.cfg says `[gcode_macro Tool_Positions]`. Both the subscription and the
/// reply lookup must derive the key from here, or they name different objects.
std::string indx_tool_positions_object(const PrinterDiscovery& hw);

/// A validated (or explicitly rejected) INDX tool count.
struct IndxInventory {
    bool valid = false;
    int tool_count = 0;    ///< 1..kIndxMaxTools when valid, 0 otherwise
    std::string rejection; ///< populated only when !valid, for diagnostics
};

/// Numbered tool ids "0".."tool_count-1" a valid inventory produces, already
/// in the natural/numeric order finalization must preserve. Empty for a
/// non-positive count.
std::vector<std::string> indx_tool_ids(int tool_count);

/// Parse and bounds-validate the runtime tool count out of a
/// `gcode_macro TOOL_POSITIONS` status object (pass the object itself, e.g.
/// `status["gcode_macro TOOL_POSITIONS"]`).
///
/// nullopt means "no news": the object or its `tool_count` field was absent
/// from this frame (Moonraker republishes only fields that changed), which
/// callers must not treat as a rejection. A non-nullopt result with
/// `valid == false` means the field WAS present this frame but is not usable
/// (wrong JSON type, non-positive, or over kIndxMaxTools) - config text,
/// shortcut count and saved offsets are never a fallback for this value.
std::optional<IndxInventory> read_indx_inventory(const nlohmann::json& tool_positions_status);

/// Outcome of reading INDX's saved active-tool identity.
enum class IndxActiveToolStatus {
    kAbsent,    ///< no news this frame - preserve the last known identity
    kValid,     ///< `value` is a numbered tool (0..count-1) or -1 (parked)
    kMalformed, ///< the field was present but unusable - report unavailable,
                ///< never silently preserved as fresh truth nor treated as "no news"
};

struct IndxActiveTool {
    IndxActiveToolStatus status = IndxActiveToolStatus::kAbsent;
    int value = -1; ///< meaningful only when status == kValid
};

/// Read INDX's saved active-tool identity out of a `save_variables` status
/// object (pass the object itself, e.g. `status["save_variables"]`).
/// `active_tool` is a saved macro assertion, never physical seating proof.
///
/// @param configured_tool_count the finalized inventory size; a positive
///        value bounds-checks the id. 0 (inventory not yet finalized) accepts
///        any -1 or non-negative integer without an upper bound.
IndxActiveTool read_indx_active_tool(const nlohmann::json& save_variables_status,
                                     int configured_tool_count);

/// Per-printer override for the Select/Park commands a `ToolCommands::present`
/// provider uses. Distinct from `Feeder`'s "honour any stored
/// name" contract: an invalid stored macro here must stay visibly invalid and
/// send nothing, never silently substitute a different physical movement
/// command. Three states per direction, not two — "auto" (detected default),
/// a validated explicit choice, and a stored choice this printer no longer
/// reports.
struct ToolMovementOverride {
    enum class Choice {
        kAuto,    ///< use the detected default (T<n> shortcut / change_tool_macro, PARK_TOOL)
        kValid,   ///< explicit choice; the macro is present on this printer
        kInvalid, ///< explicit choice naming a macro this printer does not have
    };

    Choice select_choice = Choice::kAuto;
    /// The raw stored setting value, for the settings dropdown's current
    /// selection — kAutoMacro, or the (possibly invalid) macro name.
    std::string select_choice_raw{kAutoMacro};
    /// Populated only when select_choice == kValid. Sent as
    /// "<select_macro> TOOL=<n>" — the fixed contract for this override,
    /// never an arbitrary template.
    std::string select_macro;

    Choice park_choice = Choice::kAuto;
    std::string park_choice_raw{kAutoMacro};
    /// Populated only when park_choice == kValid. Sent bare — a parking
    /// override takes no argument.
    std::string park_macro;

    /// Options for the settings picker: kAutoMacro followed by the plausible
    /// macros this printer reports. Empty when there is nothing to choose from.
    std::vector<std::string> macro_options;

    /// Uppercased macros a later pick may name and still be valid: every
    /// candidate plus each choice that resolved valid, which the picker lists
    /// even when the candidate filter leaves it out.
    std::vector<std::string> accepted_macros;
};

/// Resolve the stored Select/Park overrides against this printer's actual
/// macros. "auto" (or empty) keeps the detected default. A non-"auto" choice
/// naming a macro this printer does not report resolves to kInvalid — the
/// caller must send nothing for that direction rather than falling back to
/// the automatic command: an invalid configured command stays visibly
/// invalid and sends nothing.
ToolMovementOverride resolve_tool_movement_override(const PrinterDiscovery& hw,
                                                    const std::string& select_choice = kAutoMacro,
                                                    const std::string& park_choice = kAutoMacro);

/// Macros on this printer that could plausibly select or park a tool, for the
/// picker in ToolChanger device-action settings. Sorted, and deliberately
/// filtered like feeder_macro_candidates() — a printer has hundreds of macros
/// and a raw list is unusable.
std::vector<std::string> tool_movement_macro_candidates(const PrinterDiscovery& hw);

/// The dock sensor this printer exposes, or an absent capability.
ToolSensor resolve_tool_sensor(const PrinterDiscovery& hw);

/// The swap commands this printer needs, or an absent capability meaning
/// klipper-toolchanger is there and owns them.
ToolCommands resolve_tool_commands(const PrinterDiscovery& hw);

/// Machine name for logs and the AMS unit label ("MedusaHC"), or empty.
std::string machine_name(const PrinterDiscovery& hw);

/// The feeder this printer exposes, or an absent capability.
///
/// @param open_override,close_override User-chosen macro names. "auto" (or
///        empty) keeps the detected default, which prefers the controller's
///        native command when the printer has it. A name that is not actually
///        on the printer is still honoured: the user may know something
///        discovery does not, and Klipper's own error is the honest signal.
Feeder resolve_feeder(const PrinterDiscovery& hw, const std::string& open_override = "auto",
                      const std::string& close_override = "auto");

/// Macros on this printer that could plausibly drive a feeder, for the picker
/// in AMS settings. Sorted, and deliberately filtered: a printer has hundreds of
/// macros and a raw list is unusable.
std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw);

/// Klipper status objects that must be subscribed for read_tool() to ever
/// return a value. Empty when no provider matches.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Whether this printer's tool inventory is published only in runtime status,
/// so the object list cannot describe its tools yet. True means discovery must
/// hold hardware-dependent initialization until
/// finalize_tool_inventory_from_status() has read the subscription reply; the
/// object carrying it is among required_status_objects().
bool tool_inventory_from_status(const PrinterDiscovery& hw);

/// Read the tool inventory out of the subscription reply's initial @p status
/// and finalize it into @p hw. Logs and leaves @p hw untouched when the reply
/// carries no usable inventory; the printer then completes discovery with no
/// tool-changer backend.
void finalize_tool_inventory_from_status(PrinterDiscovery& hw, const nlohmann::json& status);

/// Pull an authoritative reading out of a Moonraker status frame. nullopt means
/// "no news" - either this printer has no add-on, or this frame simply carried
/// none of its fields. Callers must treat nullopt as no news, never as cleared:
/// Moonraker only republishes fields whose value CHANGED.
std::optional<ToolReading> read_tool(const nlohmann::json& status);

/// Whether a reading's sensor_error names a fault the user must act on, rather
/// than the transitional geometry every swap produces.
///
/// `current_tool == -2` is one value for two answers upstream: a pin that read
/// neither 0 nor 1, and a switch pattern matching none of the settled
/// configurations. A tool in transit between its dock and the head is always the
/// second. So only a flag the controller published itself, or a -2 it still
/// reports once at rest, is a fault. topi314's controller draws the same line,
/// escalating -2 to state:"error" only while its machine state is ready.
[[nodiscard]] bool sensor_error_is_fault(const ToolReading& reading, bool swap_in_flight);

} // namespace helix::toolchanger_addon
