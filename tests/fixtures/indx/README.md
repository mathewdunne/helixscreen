# Bondtech INDX fixtures

Synthetic, source-derived protocol fixtures for the Bondtech INDX nozzle changer
(`docs/devel/plans/2026-09-20-bondtech-indx.md`). None of these files come from
the unexamined debug bundle `NYWQDT7V` referenced in that plan's §0/§2 — that
bundle has not been directly inspected in any session that produced this
directory. Every value here is hand-derived from the following public sources,
cited in the plan:

- [Bondtech INDX repository](https://github.com/BondtechAB/INDX/tree/f5bf9a68e1b99922aff80d1ee31cc80ca3b2189c) —
  `macros/indx.cfg` (the `TOOL_POSITIONS` macro shape, `T0`..`T3` shortcuts,
  `CHANGE_TOOL`, `PARK_TOOL`).
- [Bondtech host module](https://github.com/BondtechAB/indx_klipper/blob/703dc04a7379bcef9e4323d9dcfd68661594339e/host/toolboard.py) —
  `IndxToolboard.get_status()` (`last_dock_measurement`; confirms this module
  does NOT carry active-tool identity).
- Mathew's confirmation (2026-09-20/21, recorded in the plan's §0/D7) that a
  real capture's `printer.objects.list` contains the exact object `indx`.

## Files

| File | Shape | Purpose |
|---|---|---|
| `objects_list_six_tool.json` | `printer.objects.list` result array | Six configured tools, `T0`..`T5` shortcuts present, one shared `extruder` + `heater_bed` (one physical heater). |
| `objects_list_three_tool_four_shortcuts.json` | same | Three configured tools but `T0`..`T3` shortcuts exist (four) — the configured runtime count must outrank shortcut count (plan §2 correction 4). |
| `objects_list_lookalikes.json` | same | Near-miss object names that must NOT trigger detection: `mcu indxmcu`, `angle indx`, `neopixel indx`, plus `T0`..`T2` shortcuts with no exact `indx` object at all. |
| `objects_list_alongside_toolchanger.json` | same | A real `toolchanger` + native `tool T0`/`tool T1` objects alongside an (implausible but defensively tested) `indx` object — native inventory must keep priority. |
| `tool_positions_status_valid.json` | one `gcode_macro TOOL_POSITIONS` status object | `tool_count: 6`, a plain valid runtime reading. |
| `tool_positions_status_malformed.json` | array of individually-malformed `gcode_macro TOOL_POSITIONS` status objects | Boolean, fractional, zero, negative, oversized (17), and string-typed `tool_count` — each must be rejected, not coerced. |
| `save_variables_status_active_tool.json` | one `save_variables` status object | A full-dictionary snapshot with `variables.active_tool: 3`, as Moonraker would actually deliver it (variables is diffed as a whole dict — plan §5.2 correction). |
| `save_variables_status_parked.json` | one `save_variables` status object | `variables.active_tool: -1` — explicit park, distinct from "never saved". |
| `save_variables_status_sparse_delta.json` | one `save_variables` status object | Defensive synthetic case: a delta carrying unrelated variables but no `active_tool` key at all — must read as "no news", not as a change. Labelled synthetic per plan §5.2; real Moonraker deltas carry the whole `variables` dict. |
| `save_variables_status_malformed.json` | array of individually-malformed `save_variables` status objects | Boolean, fractional, non-numeric string `"3junk"`, huge number, `-5` (below the only valid sentinel), and an id past a 6-tool inventory — each must resolve to `kMalformed`, never a guess. |

## Provenance discipline

Fixtures here describe the **protocol shape** Bondtech's own macros and the
public host module publish, not a specific customer's printer. Do not commit a
sanitized capture from a real printer into this directory — hardware-derived
captures belong under the later "hardware acceptance" phase (plan §11), kept
separate from these source-derived fixtures per plan §9 Package A item 1.
