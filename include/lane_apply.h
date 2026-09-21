// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_resolver.h"
#include "lane_source_store.h"

namespace helix::ams {

/// Combine the resolver's presence answer with the backend's lane flavour.
///
/// SlotStatus carries four unrelated facts: presence, which lane is at the
/// extruder (LOADED), a fault (BLOCKED), and Happy Hare's gate_status 2
/// (FROM_BUFFER). Only presence is the resolver's, and it is the only one this
/// narrows: an absent lane reads EMPTY whatever the backend wrote, and a
/// present lane never reads EMPTY or UNKNOWN. The other three flavours belong
/// to firmware state machines that read them back, so they pass through.
[[nodiscard]] SlotStatus narrow_status(SlotStatus backend_status, bool present);

/// Lay a resolved lane onto the SlotInfo a backend has just built.
///
/// Only the fields a source actually observed are written; an unobserved field
/// leaves the backend's own value standing, and presence narrows the status
/// only when a sensor has spoken. The fields SlotInfo carries that the resolver
/// does not own (tool mapping, extruder name, endless-spool group, error,
/// environment, remaining length, temps, indices) are left exactly as the
/// backend set them. Pure: no clock, no globals, no I/O.
void apply_resolved(SlotInfo& slot, const ResolvedLane& resolved);

/// Copy the fields apply_resolved() can write, status aside, from @p src.
///
/// The inverse of a paint: it puts a caller's own values back over one. A
/// backend that paints from the lane in the middle of applying a write has
/// painted a lane that does not know about that write yet, so the values it
/// laid down are the ones being replaced. Snapshot before, copy back after,
/// and the paint keeps only what it is there for.
void copy_resolver_owned_identity(SlotInfo& dst, const SlotInfo& src);

/// This lane's resolved values. Never calls into a backend: backends call it
/// while holding their own mutex_.
[[nodiscard]] ResolvedLane resolved_lane(LaneId lane);

} // namespace helix::ams
