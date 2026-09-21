# Bondtech INDX

Bondtech INDX is a **nozzle changer**: it swaps a whole passive tool (nozzle + its own
filament path), but unlike a klipper-toolchanger toolhead changer every tool shares one
physical extruder/heater on the carriage. It has no `[toolchanger]` object and no native
`tool T<n>` objects of its own — its inventory and active-tool identity live in a runtime
macro (`TOOL_POSITIONS`) and a saved variable (`save_variables.active_tool`), not in
`printer.objects.list`. It is not its own `AmsType` and has no backend of its own:
`AmsBackendToolChanger` drives it, following the precedent set for MedusaHC
(`FILAMENT_BACKEND_MEDUSAHC.md`) — a nozzle/hotend changer with its own commands and no
klipper-toolchanger belongs on this backend, not a new one.

- Upstream macros: [BondtechAB/INDX](https://github.com/BondtechAB/INDX) — `macros/indx.cfg`
  (`TOOL_POSITIONS`, `T0..T<n-1>` shortcuts, `CHANGE_TOOL`, `PARK_TOOL`).
- Host module: [BondtechAB/indx_klipper](https://github.com/BondtechAB/indx_klipper) —
  `IndxToolboard.get_status()` publishes a dock-measurement result only; it does not carry
  active-tool identity and is never read for that purpose.

This document is a durable feature doc. It absorbs the implementation contract from
`docs/devel/plans/2026-09-20-bondtech-indx.md`, which is deleted once this change ships.

## Why it is not a backend, and why it is not a sibling `AmsType`

An earlier maintainer issue comment suggested a sibling of AD5X IFS / QIDI Box. That
architecture predates three later maintainer commits (02b31dab6, 929665d3d, 4e9fedc54)
that extend `AmsBackendToolChanger` + `toolchanger_addon` to hotend/nozzle changers with
no native klipper-toolchanger objects. This implementation follows that later,
maintainer-authored precedent: no new `AmsType`, no copied backend. This is a documented
architectural choice, not a claim that the original issue comment's maintainer has
explicitly reviewed or approved this specific INDX implementation.

IFS/QIDI's HUB model routes filament lanes into one nozzle; a HUB's load/retract
semantics do not describe INDX, which exchanges separate nozzles with independent
filament paths. `AmsType::TOOL_CHANGER` / `PathTopology::PARALLEL` already model that
correctly — the work this feature needed was generalizing the backend's shared-resource
and inventory assumptions, not picking a new type.

## Detection

Sole detection signal, deliberately narrow: the **exact** Klipper status object `indx`
(`PrinterDiscovery::has_indx()`, set in `parse_objects()`). Config-only detection (an
`indx.cfg` include with no live `indx` object reachable) is explicitly out of scope for
this delivery — see `toolchanger_addon.h`'s comment on `has_indx()` and plan §0/§12.
Lookalike object names (`mcu indxmcu`, `angle indx`, `neopixel indx`) do not match.

`is_indx_inventory_candidate()` (`toolchanger_addon.cpp`) additionally requires that no
other filament-management or tool-changer backend has already claimed the printer from
object-list facts alone (`has_tool_changer()`, `has_mmu()`, `has_snapmaker()`, or an
already-populated tool list) — a real klipper-toolchanger, MMU or Snapmaker always
outranks an INDX guess.

## Inventory and active-tool identity: a deferred, subscription-sourced contract

Unlike every other provider in this module, INDX's tool **count** is not in
`printer.objects.list` at all — it is the runtime value of
`gcode_macro TOOL_POSITIONS.tool_count`, readable only once that macro's status is
subscribed. `PrinterDiscovery::finalize_indx_inventory()` re-runs the tool-facts
derivation with the provider-supplied ids once the subscription snapshot lands, which
requires ordering the discovery sequence around it (`MoonrakerDiscoverySequence`):

1. Object list confirms `has_indx()` and `is_indx_inventory_candidate()`.
2. `toolchanger_addon::required_status_objects()` adds `save_variables` and
   `gcode_macro TOOL_POSITIONS` to the subscription.
3. The first subscription snapshot's `TOOL_POSITIONS.tool_count` is read
   (`read_indx_inventory()`) and validated (1..`kIndxMaxTools` == 16); `indx_tool_ids()`
   produces the numbered ids in natural order.
4. Only then is inventory finalized and the deferred hardware-initialization callback
   released — the same deferred-callback seam `application.cpp`/`printer_discovery.cpp`
   already provide, extended so an inventory-dependent provider can hold subsystem
   initialization until its facts exist, without changing the timing of any other
   printer's discovery.

A **configured** count can exceed the shortcuts a user's `indx.cfg` actually declares
(the pinned upstream fixture: 3 configured tools, `T0..T3` shortcuts present — 4). The
runtime inventory always outranks shortcut count; `ToolCommands::select_shortcut_available`
records, per tool, whether its `T<n>` shortcut actually exists on this printer, and a tool
with none falls back to `CHANGE_TOOL TOOL=<n>` when that macro is present.

`active_tool` (`save_variables.variables.active_tool`) is a **saved macro assertion**,
never physical seating proof — INDX's host module exposes a dock measurement, not
per-tool presence sensing, so nothing in this delivery treats it as verified hardware
state. `read_indx_active_tool()` distinguishes three outcomes: no news this frame
(preserve last known identity), a valid value (a numbered tool, or `-1` for parked), and
malformed (present but unusable — reported as unavailable, never coerced and never
silently treated as fresh truth). A value outside the finalized inventory is malformed.

## Shared extruder, tool identity and consumption (Package C)

Every printing tool maps to the printer's one physical extruder/heater — never the
positional per-index carry-over the backend previously used, which only ever covered as
many tools as there were physical extruders. `ToolState::set_ams_topology()` preserves
"unreported" active-tool identity distinctly from other backends' own negative-value
conventions (Happy Hare bypass, an unmapped route), so a printer with no saved
`active_tool` yet is not painted as T0, and a shared-extruder status frame cannot select
T0 either.

`has_multiple_nozzles()` lets badges/labels answer "which nozzle" independently of
extruder count, and the reverse extruder→tool lookup resolves through the valid active
tool rather than the first positional match when several tools share one extruder — the
same fix keeps single-lane AFC-style badges unchanged, since those still map one tool to
one nozzle. `slot_for_extruder()` returns `nullopt` for this shared-resource shape, so
consumption routes through the existing aggregate path keyed on the backend's current
slot rather than a fixed slot index; `FilamentConsumptionTracker` rebaselines a slot the
instant it becomes current, instead of charging it for the whole print's prior history.

### Temperature transients and Z-offset — verified, not a gap

Package C's audit found **no INDX-specific production fix was needed** for the
temperature-transient and Z-offset/babystep compatibility this plan's §6.2 required:
existing generic mechanisms already cover it correctly for a shared-extruder machine —

- **Temperature**: the per-extruder last-nonzero-target latch already used elsewhere
  displays the sensible target through a hot→zero→restored cycle without introducing an
  INDX-specific read path.
- **Z-offset**: `z_offset_utils::adjust()`'s existing stale-session-base widening already
  tolerates a combined-origin change across a tool swap; the tune overlay's travel guard
  and save/apply command capture are unaffected.

This is documented, tested (regression cases pinning both), verified behavior — not
deferred work. No per-tool offset editor exists or is planned for this delivery (§12).

## Commands (Package B/D)

`resolve_tool_commands()` records INDX's defaults when no klipper-toolchanger Provider
matches and `has_indx()` is true:

| Direction | Default command | Gated on |
|---|---|---|
| Select tool `<n>` | `T<n>` (shortcut), else `CHANGE_TOOL TOOL=<n>` | `hw.has_macro("T<n>")` / `hw.has_macro("CHANGE_TOOL")` — a missing command is an explicit unsupported capability, never a silent no-op |
| Park (unmount) | `PARK_TOOL` | `hw.has_macro("PARK_TOOL")` |

Unsupported operations emit no Klipper command at all.

### Per-printer Select/Park overrides (D3)

`toolchanger_addon::resolve_tool_movement_override()` is a tri-state override distinct
from `StandardMacroInfo`'s "missing → fallback" convention: **Auto** (detected default),
a validated explicit choice, or a stored choice this printer no longer reports — the last
case sends nothing rather than silently substituting a different physical movement
command. `AmsBackendToolChanger` exposes both as Device Action dropdowns
(`tool_select_macro` / `tool_park_macro`, no feeder required), persisted per-printer via
`SettingsManager::get_tool_select_macro()`/`get_tool_park_macro()`
(`wizard::TOOL_SELECT_MACRO`/`TOOL_PARK_MACRO`). An explicit choice is sent as
`<macro> TOOL=<n>` (select) or bare `<macro>` (park) — a fixed contract, never an
arbitrary template.

### Filament Load/Unload separation (D1/D2)

A shared-extruder tool changer needs a real distinction klipper-toolchanger's own model
does not: **mounting a tool is not feeding its filament.** `filament_op_dispatch.h` adds
`OperationIntent` (`ToolMount` / `Filament`) and
`BackendCaps::has_separate_filament_operation`
(`AmsBackend::shared_extruder_name().has_value()` — true only for this backend shape). A
`Filament`-intent caller on such a backend never reaches the generic `AmsBackend`
mount/park tier and never falls back to raw extrusion
(`FilamentRefusal::NoMacroConfigured`) — an unconfigured Load/Unload macro refuses
cleanly rather than silently extruding through a movement command. The AMS tool-grid and
sidebar mount/park controls pass `ToolMount` explicitly; the Filament panel and runout
controls keep the `Filament` default. Every other backend is unaffected — the capability
is false everywhere else.

**Global Load/Unload choice (D5).** HelixScreen's existing standard-macro Load/Unload
selection (`/standard_macros/...`) is unchanged by this feature and stays **global across
every printer profile** — INDX adds no new storage or per-printer scoping for it. Picking
a custom Load or Unload wrapper while connected to an INDX printer changes that choice for
every other printer profile too; availability is still evaluated against whichever printer
is currently connected. Only the Select/Park overrides above are per-printer. A stock
INDX installation ships no `LOAD_FILAMENT`/`UNLOAD_FILAMENT` of its own — until the user
configures one, Load/Unload for filament (not tool mount/park) has nothing to run.

### Preparation and completion policy (D1/§7.3)

`preheat_skip_reason()` and `needs_home_confirmation()` answer "the macro self-heats" /
"no confirmation needed" for this provider's macro tier, without adding the generic
spelling `LOAD_FILAMENT` to `filament_macro_profiles.cpp`'s global table — that table
stays vendor-neutral. The Filament panel does not schedule a post-operation cooldown from
this provider's macro-tier completion: a macro request returning is not proof of physical
completion (no firmware phase reporting exists to confirm it).

### Paused-print and homing policy (D6/§7.4)

The stock `CHANGE_TOOL`/`PARK_TOOL` macros home conditionally themselves, so
`AmsBackendToolChanger::delegates_homing_to_printer()` answers true for this provider:
HelixScreen never prompts for home confirmation and never synthesizes a `G28` ahead of a
Select/Park dispatch. That is safe while idle or printing, where the macro's own
conditional `G28` runs unencumbered — but not safe on a **paused** print, where Layer 1
(`helix::api::reject_homing_during_active_print`) blocks any HelixScreen-emitted `G28` but
cannot see one buried inside a macro, and injecting a home into a paused print is exactly
what this feature must not do. `dispatch_operation()` therefore adds a narrower
paused-print precondition specific to this provider: a `PAUSED`-state dispatch requires a
known-homed toolhead (all axes), refusing with **zero commands** otherwise. Unhomed or
partially-homed axes refuse the same way. Ordinary Preparing/Printing-state refusals are
unchanged, and every other provider's existing pause/homing behavior is untouched.

## Metadata and Spoolman (§8) — an explicit scope boundary

INDX uses the existing `FilamentSlotOverrideStore` and `lane_data` `T<n>` key convention
unchanged — the same store, edit paths (slot editor, color picker, Spoolman picker) and
persistence every other toolchanger-shaped backend already uses. This feature adds **no**
new synchronization mechanism and performs **zero writes** to custom macro variables
(`T<n>.spool_id`, `t<n>__spool_id`, or any other printer-side macro variable): `lane_data`,
a printer's runtime macro variables, and Moonraker's active-spool endpoint are three
different stores, and a HelixScreen slot assignment does not change what a user's own
toolchange macro privately selects. This is a deliberate scope boundary recorded in the
plan (§8/§12), not unfinished compatibility work — assignment, unlinking, remote metadata
refresh, offline cache and printer-switching behavior are exercised through the existing
slot-memory/spool test suite with INDX in the mix, not a new one.

## Mock mode: `HELIX_MOCK_AMS=indx`

Follows the MedusaHC production-backend pattern
(`docs/devel/FILAMENT_BACKEND_MEDUSAHC.md`, `moonraker_client_mock.cpp`): this mode is
mock **hardware**, not a mock backend. `HELIX_MOCK_AMS=indx` implies `--real-ams`
(`src/system/cli_args.cpp#parse_cli_args`), so `try_create_mock()` (`src/printer/ams_backend.cpp`) declines to
build `AmsBackendMock` and real discovery runs the production
`AmsBackendToolChanger` + `toolchanger_addon` path against the objects and status this
mode publishes.

`MoonrakerClientMock::populate_capabilities()` emits **honest stock objects only** — the
exact `indx` object, one physical heater (the bare `extruder` every printer type already
has; unlike the toolchanger/MedusaHC mock modes, INDX does **not** get the extra
`extruder1/2/3` heaters), `save_variables`, `gcode_macro TOOL_POSITIONS`,
`gcode_macro PARK_TOOL`, and a `gcode_macro T<n>` per configured tool
(`kIndxDefaultToolCount = 12` by default — deliberately not a round number, so a
lexicographic tool-grid sort would visibly misorder tool "10" ahead of "2" if one crept
back in). It suppresses the mock's otherwise-default `mmu` object and the unconditional
`LOAD_FILAMENT`/`UNLOAD_FILAMENT` macros that every other printer type gets — a stock
INDX installation ships none of these, and leaving them in would let a detection or
missing-action test exercise a printer the upstream configuration does not actually
describe. No fake `toolchanger` or `tool T<n>` objects are ever published for this mode.

`gcode_script()` handles `T<n>` and `PARK_TOOL` (token-exact, mirroring the MedusaHC
block) by arming a swap simulation (`start_indx_swap()`) that completes after a short,
observable delay (`advance_indx_swap()`, driven from the same periodic notification tick
as the MedusaHC/IFS simulations) — long enough that a caller can observe "busy" before the
reported `active_tool` changes, short enough not to be a wait. `is_mock_indx()` and the
new `indx_tool_positions_status_json()`/`indx_save_variables_status_json()` helpers are
consulted from both `printer.objects.query` and `printer.objects.subscribe`, and honor
Package A's existing unit-test-only `set_indx_tool_count()`/`set_indx_active_tool()`
override seam first — so the discovery-sequence controlled-transport tests that seam
exists for are unaffected by this mode's addition.

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_MOCK_AMS=indx ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate filament
```

## Boundaries (§12)

Out of scope for this delivery, unless a future plan explicitly revises it: firmware
installation, printer macro edits, thermal-model tuning, latch/dock mechanics,
force-state recovery, dock/load-cell calibration, **per-tool offset editing or
persistence** (offset display/save compatibility is verified; an editor is not built),
config-only detection, a StandardMacros storage migration, non-printing tools
(`no_heat_tools` — pens/cameras; D4 defers these entirely, including their resource/UI
model), and a vendor-specific panel. INDX uses the same generic toolchanger UI every
other provider on this backend uses.

---

Part of the filament system — see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for
the shared architecture and [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md)
for the backend that drives it. [FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md)
is the sibling add-on this implementation's architecture follows.
