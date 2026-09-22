// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "consumption_sink.h"

namespace helix {
enum class PrintJobState;
} // namespace helix

#include <array>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace helix {

class FilamentConsumptionTracker {
  public:
    /// Maximum number of per-extruder filament_used subjects the tracker
    /// subscribes to. Aligns with `PrinterPrintState::MAX_EXTRUDER_SCAN` — the
    /// pool of pre-populated dynamic subjects on PrinterState. Klipper
    /// toolchanger setups never come close to this.
    static constexpr int MAX_TRACKED_EXTRUDERS = 16;

    /// Opaque handle returned by register_sink() and consumed by unregister_sink().
    using SinkHandle = IConsumptionSink*;

    static FilamentConsumptionTracker& instance();

    /// Register lifecycle + filament-used observers. Call once during app init
    /// after AmsState and PrinterState subjects are available. Also installs
    /// the ExternalSpoolSink on first call.
    void start();

    /// Tear down observers. Safe to call multiple times. Does NOT unregister
    /// sinks — the tracker keeps its registry across start/stop cycles so that
    /// `register_sink` / `unregister_sink` calls from AmsState remain valid.
    void stop();

    /// True while a snapshot is live (print in progress; at least one sink
    /// successfully snapshotted).
    [[nodiscard]] bool is_active() const {
        return active_;
    }

    /// Register a sink. Tracker takes ownership. If a print is currently
    /// active, immediately calls snapshot() on the new sink using the current
    /// aggregate filament_used reading so mid-print registration still tracks
    /// from the point of registration. Returns a non-owning handle.
    ///
    /// **Thread safety**: must be called from the main thread. Tracker state
    /// (sinks_, active_, print_in_progress_) is NOT guarded — observer callbacks
    /// fire deferred via UpdateQueue (also main thread), so registration and
    /// dispatch don't race today. Adding background-thread callers requires a
    /// mutex.
    SinkHandle register_sink(std::unique_ptr<IConsumptionSink> sink);

    /// Unregister a sink. Flushes the sink before destruction. Safe to call
    /// with a null / already-removed handle (no-op).
    ///
    /// **Thread safety**: main thread only. See register_sink().
    void unregister_sink(SinkHandle handle);

  private:
    friend struct FilamentConsumptionTrackerTestAccess;

    FilamentConsumptionTracker() = default;
    ~FilamentConsumptionTracker() = default;
    FilamentConsumptionTracker(const FilamentConsumptionTracker&) = delete;
    FilamentConsumptionTracker& operator=(const FilamentConsumptionTracker&) = delete;

    /// All registered sinks. The ExternalSpoolSink is installed lazily by
    /// start() and lives here for the process lifetime; AmsSlotSinks are added
    /// and removed by AmsState in response to backend lifecycle events.
    std::vector<std::unique_ptr<IConsumptionSink>> sinks_;

    /// Convenience handle to the one-and-only ExternalSpoolSink. Non-owning.
    IConsumptionSink* external_sink_raw_ = nullptr;

    /// True when at least one sink successfully snapshotted on print start.
    /// Preserved for backwards-compatible `is_active()` semantics and used to
    /// decide whether on_filament_used_changed forwards deltas.
    bool active_ = false;

    /// True between the PRINTING transition and a terminal state transition.
    /// Tracks the print lifecycle independently of sink trackability so that
    /// mid-print sink registration can snapshot the new sink.
    bool print_in_progress_ = false;

    /// Aggregate-path bookkeeping: the backend slot last seen as
    /// backend->get_current_slot() when on_filament_used_changed() applied a
    /// delta, keyed by backend index. AmsSlotSink::apply_delta() computes its
    /// decrement from the TOTAL filament used since its own snapshot, not
    /// since it last ran -- correct only while the same slot stays current the
    /// whole time. A shared-resource tool changer (or any other multi-slot
    /// backend routed through this aggregate path) changes which slot is
    /// current mid-print, so the newly-current slot must rebaseline from HERE
    /// rather than being charged for the whole print's filament history it
    /// was never mounted for. Absent means "not seen since the last
    /// snapshot_all_sinks()", which on_print_state_changed() clears on every
    /// PRINTING transition so a fresh print never inherits a stale slot from
    /// the previous one (or from being parked, which leaves no entry at all).
    std::unordered_map<int, int> last_current_slot_by_backend_;

    /// Previous aggregate filament_used reading. When a backend changes its
    /// current slot between notifications, the next reading contains the first
    /// real delta for that slot. Rebaselining at this previous value lets that
    /// delta be applied to the newly-current slot without charging it for any
    /// earlier part of the print.
    float previous_aggregate_filament_used_mm_ = 0.0f;

    ObserverGuard print_state_obs_;
    ObserverGuard filament_used_obs_;

    /// Shared lifetime for every per-extruder filament_used subject observer.
    /// Per [L077]: must be reset BEFORE `extruder_obs_` in stop() so the
    /// weak_ptr held inside each ObserverGuard expires before the guard runs
    /// lv_observer_remove() — otherwise the guard can try to remove an observer
    /// from a freed subject when PrinterState tears down.
    SubjectLifetime extruder_lifetime_;

    /// Per-extruder filament_used_mm observers. Index i observes Klipper's
    /// `extruder` (i=0) / `extruder1` / `extruder2` / ... filament_used field.
    std::array<ObserverGuard, MAX_TRACKED_EXTRUDERS> extruder_obs_{};

    void on_print_state_changed(helix::PrintJobState state);
    void on_filament_used_changed(int filament_mm);

    /// Handler for a single extruder's filament_used delta. Routes the delta
    /// to any AmsSlotSink whose backend declares `slot_for_extruder(idx)`
    /// mapping to this sink's slot.
    void on_extruder_filament_used_changed(int extruder_idx, int mm);

    /// Snapshot every registered sink. Called on PRINTING transition.
    void snapshot_all_sinks(float filament_used_mm);

    /// Log, once per print start, every backend slot mapped to an extruder
    /// index Klipper does not report; such a slot receives no deltas. Returns
    /// the number of mappings reported. Quiet before extruder discovery.
    int warn_unreported_extruder_mappings();

    /// Mappings warn_unreported_extruder_mappings() reported at the last
    /// print start; read by tests, since the warning itself is only a log line.
    int unreported_mappings_at_start_ = 0;

    /// Flush every registered sink. Called on COMPLETE / CANCELLED / ERROR / PAUSED.
    void flush_all_sinks();

    /// True when at least one registered sink is currently trackable.
    [[nodiscard]] bool any_sink_trackable() const;
};

} // namespace helix
