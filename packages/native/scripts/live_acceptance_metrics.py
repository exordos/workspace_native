"""Evaluate an anonymized, monotonic Wayland acceptance trace."""

import itertools
from collections import Counter
from statistics import median


def sanitize_native_event(raw):
    """Project telemetry onto numeric fields and explicit enums before retaining it."""
    allowed = {
        "runtime": ("scale", "graphicsApi"),
        "sample": (
            "active",
            "authenticated",
            "ready",
            "frames",
            "dropped",
            "count",
            "offset",
            "intent",
            "paneX",
            "paneY",
            "width",
            "height",
            "anchor",
            "anchorY",
            "editorFocus",
            "draftLength",
            "hits",
            "misses",
        ),
        "frame": (),
        "click": (),
        "wheel": ("angle", "pixel", "phase"),
        "realtime": ("peer", "reaction", "receipt", "revalidation"),
        "request": ("older", "index"),
        "response": ("older", "elapsed", "status", "index"),
    }
    kind = raw.get("event")
    if kind not in allowed or type(raw.get("ms")) is not int:
        return None
    event = {"event": kind, "ms": raw["ms"]}
    for key in allowed[kind]:
        if key in raw and type(raw[key]) in (bool, int, float):
            event[key] = raw[key]
    if kind == "realtime" and raw.get("kind") in (
        "message.created",
        "message.updated",
        "message.deleted",
        "message.read",
        "messages.read",
        "message_reaction.created",
        "message_reaction.updated",
        "user.updated",
        "other",
    ):
        event["kind"] = raw["kind"]
    if kind == "runtime":
        import re

        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(raw.get("qt", ""))):
            return None
        event["qt"] = raw["qt"]
    if kind in ("sample", "request", "response") and raw.get("view") in (
        "a",
        "b",
        "aggregate",
        "none",
        "other",
    ):
        event["view"] = raw["view"]
    if kind in ("request", "response"):
        if raw.get("category") not in ("history", "auth", "image", "events", "other"):
            return None
        event["category"] = raw["category"]
    return event


def write_frame_sequence(events, destination):
    """Encode the published density grids, with the original frame intervals."""
    from PIL import Image

    pictures = sorted(
        (e for e in events if e["event"] == "picture"), key=lambda e: e["ms"]
    )
    if len(pictures) < 2:
        return None
    images = []
    durations = []
    last_cells = None
    palette = [v for i in range(16) for v in (17 * i, 17 * i, 17 * i)]
    for index, picture in enumerate(pictures):
        cells = picture["cells"]
        duration = (
            max(1, pictures[index + 1]["ms"] - picture["ms"])
            if index + 1 < len(pictures)
            else 20
        )
        if cells == last_cells:
            durations[-1] += duration
            continue
        grid = Image.frombytes("P", (16, 12), bytes(int(c, 16) for c in cells))
        grid.putpalette(palette)
        images.append(grid.resize((96, 72), Image.Resampling.NEAREST))
        durations.append(duration)
        last_cells = cells
    images[0].save(
        destination,
        save_all=True,
        append_images=images[1:],
        duration=durations,
        loop=0,
        optimize=False,
        disposal=1,
    )
    return {
        "start_ms": pictures[0]["ms"],
        "end_ms": pictures[-1]["ms"],
        "samples": len(pictures),
    }


def percentile(values, percentile_value):
    if not values:
        return None
    values = sorted(values)
    return values[round((len(values) - 1) * percentile_value)]


def measure_pending_history(window):
    """Correlate target rows, compositor pixels and a delivered wheel with the same outstanding read."""
    pending = set()
    sample, picture, wheel = {}, {}, None
    cached_frames, wheel_latency = 0, None
    for event in window:
        kind = event["event"]
        if (
            event.get("category") == "history"
            and event.get("view") == "b"
            and "index" in event
        ):
            if kind == "request":
                pending.add(event["index"])
            elif kind == "response":
                pending.discard(event["index"])
        if not pending:
            wheel, picture = None, {}
        if kind == "sample":
            sample = event
        if not pending or sample.get("view") != "b" or sample.get("count", 0) <= 0:
            wheel, picture = None, {}
            continue
        if (
            kind == "wheel"
            and wheel is None
            and picture
            and "offset" in sample
            and (event["pixel"] or event["angle"])
        ):
            wheel = (
                event["ms"],
                sample["offset"],
                picture["signature"],
                pending.copy(),
            )
        if kind == "picture" and event["occupancy"] > 0:
            cached_frames += 1
            picture = event
            if (
                wheel
                and pending.intersection(wheel[3])
                and sample.get("offset", wheel[1]) != wheel[1]
                and picture["signature"] != wheel[2]
            ):
                if wheel_latency is None:
                    wheel_latency = event["ms"] - wheel[0]
                wheel = None
    return {
        "cached_frames_while_pending": cached_frames,
        "cached_scroll_visible_ms": wheel_latency,
    }


def wheel_responses(window):
    """One observed viewport move and one changed capture can each credit at most one wheel."""
    sample, picture = {}, {}
    awaiting_move = awaiting_capture = None
    latencies = []
    for event in window:
        kind = event["event"]
        if kind == "wheel" and (event["angle"] or event["pixel"]):
            if (
                awaiting_move is None
                and "offset" in sample
                and "intent" in sample
                and picture
            ):
                # Several deliveries may be coalesced into one native movement. Use the
                # first timestamp; the others remain explicitly unattributed.
                awaiting_move = (
                    event["ms"],
                    sample["offset"],
                    sample["intent"],
                    picture["signature"],
                )
        elif kind == "sample":
            sample = event
            if (
                awaiting_move
                and sample.get("offset", awaiting_move[1]) != awaiting_move[1]
                and sample.get("intent", awaiting_move[2]) > awaiting_move[2]
            ):
                if awaiting_capture is None:
                    awaiting_capture = awaiting_move
                awaiting_move = None
        elif kind == "picture":
            picture = event
            if awaiting_capture and event["signature"] != awaiting_capture[3]:
                latencies.append(event["ms"] - awaiting_capture[0])
                awaiting_capture = None
    return latencies


def history_is_restored(events, restored, target):
    """A matched head-page reply must precede a ready sample of that same target."""
    requested = set()
    completed = None
    for event in events:
        kind = event["event"]
        if event.get("view") != target:
            continue
        if (
            event.get("category") == "history"
            and not event.get("older")
            and "index" in event
        ):
            if kind == "request":
                requested.add(event["index"])
            elif (
                kind == "response"
                and event["ms"] >= restored
                and event["index"] in requested
                and 200 <= event.get("status", 0) < 300
            ):
                completed = event["ms"]
        if (
            kind == "sample"
            and completed is not None
            and event["ms"] > completed
            and event.get("ready")
            and event.get("count", 0) > 0
        ):
            return True
    return False


def evaluate(events, limits):
    """Missing observations block acceptance. An observed defect always wins."""
    events = sorted(events, key=lambda e: e["ms"])
    failures, blocked = set(), set()
    samples = [e for e in events if e["event"] == "sample"]
    frames = [e["ms"] for e in events if e["event"] == "frame"]
    actions = [e for e in events if e["event"] == "action"]
    if not samples or not frames:
        blocked.add("no-native-frame-or-heartbeat")
    if any(e["event"] == "capture-unavailable" for e in events):
        blocked.add("capture-unavailable")
    if any(e.get("dropped", 0) for e in samples):
        blocked.add("incomplete-native-trace")
    if any(e["event"] == "watchdog" for e in events):
        failures.add("gui-watchdog")
    heartbeat_intervals = []
    previous = None
    for e in events:
        if e["event"] == "launch":
            previous = None
        elif e["event"] == "sample":
            if previous is not None:
                heartbeat_intervals.append((previous, e["ms"]))
            previous = e["ms"]
    phases = {}
    for start in actions:
        matches = [e for e in events if e["event"] == "end" and e["id"] == start["id"]]
        if len(matches) != 1:
            blocked.add("unfinished-action")
            continue
        end = matches[0]["ms"]
        window = [e for e in events if start["ms"] <= e["ms"] <= end]
        observed = [e for e in window if e["event"] == "sample"]
        pictures = [e for e in window if e["event"] == "picture"]
        rendered = [e["ms"] for e in window if e["event"] == "frame"]
        received = [
            e for e in window if e["event"] == "wheel" and (e["angle"] or e["pixel"])
        ]
        frame_intervals = [b - a for a, b in itertools.pairwise(rendered)]
        heartbeat_gaps = [
            b - a for a, b in heartbeat_intervals if a < end and b > start["ms"]
        ]
        kind = start["kind"]
        key = (start["id"], kind)
        metrics = {
            "frames": len(rendered),
            "pictures": len(pictures),
            "max_heartbeat_ms": max(heartbeat_gaps, default=0),
            "p95_frame_ms": percentile(frame_intervals, 0.95),
            "max_frame_ms": max(frame_intervals, default=0),
        }
        resources = [e for e in window if e["event"] == "process"]
        if resources:
            metrics["exit_rss_kib"] = resources[-1]["rss"]
            metrics["cpu_ticks_used"] = max(
                0, resources[-1]["cpu"] - resources[0]["cpu"]
            )
        phases[key] = metrics
        if not observed or len(pictures) < 2:
            blocked.add("missing-temporal-evidence")
        if heartbeat_gaps and limits and max(heartbeat_gaps) > limits["gui_stall_ms"]:
            failures.add("gui-stall")
        if kind in {
            "loaded-scroll",
            "paged-scroll",
            "reverse-scroll",
            "touchpad-scroll",
        }:
            intents = [e["intent"] for e in observed if "intent" in e]
            if not received or not intents or max(intents) <= start["intent"]:
                blocked.add("no-pane-wheel")
                continue
            if not any(
                (e["pixel"] or e["angle"]) * start["direction"] > 0 for e in received
            ):
                blocked.add("wrong-wheel-direction")
            if kind == "touchpad-scroll":
                pane_phases = {e["phase"] for e in window if e["event"] == "wheel"}
                if not {1, 3}.issubset(pane_phases):
                    blocked.add("touchpad-start-or-stop-unobserved")
            metrics["wheel_deltas"] = [e["pixel"] or e["angle"] for e in received]
            metrics["wheel_phases"] = [e["phase"] for e in received]
            visible = wheel_responses(window)
            metrics["unmatched_wheels"] = len(received) - len(visible)
            if metrics["unmatched_wheels"]:
                blocked.add("wheel-responses-unattributed")
            metrics["wheel_responses"] = len(visible)
            metrics["wheel_to_changed_capture_p50_ms"] = percentile(visible, 0.5)
            metrics["wheel_to_changed_capture_p95_ms"] = percentile(visible, 0.95)
            has_comparison = bool(
                pictures
                and received
                and pictures[0]["ms"] < received[0]["ms"]
                and pictures[-1]["ms"] >= received[0]["ms"]
            )
            if not visible and not has_comparison:
                blocked.add("missing-temporal-evidence")
            elif not visible:
                failures.add("no-visible-wheel-response")
            elif (
                limits
                and metrics["wheel_to_changed_capture_p95_ms"]
                > limits["visible_response_ms"]
            ):
                failures.add("late-visible-wheel-response")
        for sample in observed:
            if (
                kind == "stopped"
                and "anchor" in start
                and "anchor" in sample
                and limits
            ) and (
                sample["anchor"] != start["anchor"]
                or abs(sample["anchorY"] - start["anchorY"])
                > limits["settled_anchor_px"]
            ):
                failures.add("viewport-jump-after-stop")
        if start.get("expectContent") and pictures and limits:
            for picture in pictures:
                if picture["occupancy"] < limits["min_occupancy"]:
                    failures.add("transient-blank-frame")
        if kind in {"loaded-scroll", "reverse-scroll", "paged-scroll"}:
            active_gap = metrics["max_frame_ms"]
            dispatch = next((e["ms"] for e in window if e["event"] == "wheel-dispatch"), None)
            if dispatch is not None:
                # Start at external dispatch so a delayed pane delivery still counts.
                swaps = [dispatch, *(ms for ms in rendered if ms >= dispatch)]
                active_gap = max((b - a for a, b in itertools.pairwise(swaps)), default=0)
                if len(swaps) == 1:
                    blocked.add("no-frame-after-wheel-dispatch")
            metrics["max_active_frame_ms"] = active_gap
            if limits and active_gap > limits["active_frame_ms"]:
                failures.add("long-active-frame")
        if kind in {
            "cold-start",
            "warm-start",
            "warm-switch",
            "repeated-navigation",
            "typing",
            "aggregate",
        }:
            target = start.get("target")
            typing_start = next((e for e in window if e["event"] == "type-start"), None)
            ready = next(
                (
                    e
                    for e in observed
                    if (
                        (
                            typing_start is not None
                            and e["ms"] >= typing_start["ms"]
                            and e.get("draftLength", 0)
                            > typing_start.get("draftLength", 0)
                        )
                        if kind == "typing"
                        else e.get("count", 0) > 0
                    )
                    and (target is None or e.get("view") == target)
                ),
                None,
            )
            changed = next(
                (pic for pic in pictures if ready and pic["ms"] >= ready["ms"]), None
            )
            metrics["time_to_useful_frame_ms"] = (
                changed["ms"] - start["ms"] if changed else None
            )
            if not changed:
                blocked.add("useful-frame-unobserved")
            if target and changed:
                click = next(
                    (
                        e
                        for e in reversed(window)
                        if e["event"] == "click" and e["ms"] <= changed["ms"]
                    ),
                    None,
                )
                if click:
                    metrics["click_to_useful_capture_ms"] = changed["ms"] - click["ms"]
                else:
                    blocked.add("click-to-visible-unobserved")
                if any(
                    e.get("view") != target
                    for e in observed
                    if e["ms"] >= changed["ms"]
                ):
                    failures.add("selection-reset")
            if kind == "typing":
                mark = next((e for e in window if e["event"] == "type-start"), None)
                ready = next(
                    (
                        e
                        for e in observed
                        if mark
                        and e["ms"] >= mark["ms"]
                        and e.get("draftLength", 0) > mark.get("draftLength", 0)
                    ),
                    None,
                )
                if ready and any(
                    not e.get("editorFocus") for e in observed if e["ms"] >= ready["ms"]
                ):
                    failures.add("typing-focus-lost")
                typed = next(
                    (
                        pic
                        for pic in pictures
                        if ready
                        and mark
                        and mark.get("before")
                        and pic["ms"] >= max(ready["ms"], mark["ms"])
                        and pic.get("editorSignature")
                        and pic["editorSignature"] != mark["before"]
                    ),
                    None,
                )
                if typed:
                    metrics["keyboard_dispatch_to_visible_ms"] = (
                        typed["ms"] - mark["ms"]
                    )
                else:
                    blocked.add("typing-to-visible-unobserved")
        if kind == "stopped" and "anchor" not in start:
            blocked.add("viewport-anchor-unobserved")
        if kind == "paged-scroll":
            metrics["older_requests"] = sum(
                e["event"] == "request"
                and e.get("category") == "history"
                and e.get("older")
                for e in window
            )
            if not any(
                e["event"] == "realtime"
                and e.get("peer")
                and e.get("kind") == "message.created"
                for e in window
            ):
                blocked.add("peer-arrival-during-scroll-unobserved")
            if metrics["older_requests"] < 2:
                blocked.add("several-pagination-boundaries-unobserved")
        if kind == "loaded-scroll" and any(
            e["event"] == "request"
            and e.get("category") == "history"
            and e.get("older")
            for e in window
        ):
            blocked.add("loaded-range-not-isolated")
        if kind == "reconnect":
            off = next((e for e in window if e["event"] == "offline-observation"), None)
            enabled = next((e for e in window if e["event"] == "network-enabled"), None)
            pending = [
                e
                for e in observed
                if off
                and enabled
                and off["ms"] <= e["ms"] < enabled["ms"]
                and e.get("view") == "a"
            ]
            metrics["offline_target_samples"] = len(pending)
            if off and enabled:
                metrics["offline_observation_ms"] = enabled["ms"] - off["ms"]
            if not pending:
                blocked.add("offline-target-unobserved")
            elif not any(e.get("count", 0) > 0 for e in pending):
                failures.add("warmed-topic-blank-during-disconnect")
            if any(
                e["event"] == "tunnel-restored" and not e["reachable"] for e in window
            ):
                blocked.add("backend-unreachable-after-cut")
            if not enabled or not history_is_restored(window, enabled["ms"], "a"):
                blocked.add("reconnect-unverified")
        if kind == "warm-delayed":
            metrics.update(measure_pending_history(window))
            if not metrics["cached_frames_while_pending"]:
                blocked.add("cached-content-during-revalidation-unobserved")
            if metrics["cached_scroll_visible_ms"] is None:
                blocked.add("cached-interaction-during-revalidation-unobserved")
    required = {
        "cold-start",
        "warm-start",
        "warm-switch",
        "aggregate",
        "typing",
        "warm-delayed",
        "reconnect",
        "repeated-navigation",
        "loaded-scroll",
        "reverse-scroll",
        "touchpad-scroll",
        "paged-scroll",
        "stopped",
    }
    if not required.issubset({a["kind"] for a in actions}):
        blocked.add("incomplete-scenarios")
    realtime = Counter(
        e.get("kind", "other")
        for e in events
        if e["event"] == "realtime" and e.get("peer")
    )
    if not {
        "message.created",
        "message.updated",
        "message.deleted",
        "user.updated",
    }.issubset(realtime) or not (
        any(
            k in realtime
            for k in ("message_reaction.created", "message_reaction.updated")
        )
        or any(
            e["event"] == "realtime" and e.get("peer") and e.get("reaction")
            for e in events
        )
    ):
        blocked.add("peer-event-classes-unobserved")
    session_receipt = any(
        e["event"] == "realtime"
        and e.get("kind") == "message.read"
        and e.get("receipt")
        for e in events
    )
    if not session_receipt:
        blocked.add("session-receipt-unobserved")
    if not any(e["event"] == "peer-workload" for e in events):
        blocked.add("peer-workload-unobserved")
    if not any(
        e["event"] == "response"
        and e.get("category") == "image"
        and 200 <= e.get("status", 0) < 300
        for e in events
    ):
        blocked.add("image-retrieval-unobserved")
    if not limits:
        blocked.add("no-reviewed-reference-limits")
    usage = [e for e in events if e["event"] == "process"]
    cpu_ticks = 0
    previous_cpu = 0
    for e in events:
        if e["event"] == "launch":
            previous_cpu = 0
        if e["event"] == "process":
            cpu_ticks += max(0, e["cpu"] - previous_cpu)
            previous_cpu = e["cpu"]
    return {
        "status": "FAIL" if failures else "BLOCKED" if blocked else "PASS",
        "failures": sorted(failures),
        "blocked": sorted(blocked),
        "phases": [dict(id=i, kind=kind, **m) for (i, kind), m in phases.items()],
        "resources": {
            "rss_peak_kib": max((e["rss"] for e in usage), default=0),
            "rss_final_kib": usage[-1]["rss"] if usage else None,
            "cpu_ticks": cpu_ticks if usage else None,
        },
        "requests": dict(
            Counter(e["category"] for e in events if e["event"] == "request")
        ),
        "peer_events": dict(realtime),
        "session_read_receipt": session_receipt,
        "peer_reaction_in_snapshot": any(
            e["event"] == "realtime" and e.get("peer") and e.get("reaction")
            for e in events
        ),
        "cache_hits": max((e.get("hits", 0) for e in samples), default=0),
        "frame_interval_median_ms": median(
            [b - a for a, b in itertools.pairwise(frames)]
        )
        if len(frames) > 1
        else None,
    }
