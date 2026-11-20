"""Evaluate an anonymized, monotonic Wayland acceptance trace."""

from collections import Counter
from statistics import median
import itertools


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
        "realtime": ("peer",),
        "request": ("older",),
        "response": ("older", "elapsed", "status"),
    }
    kind = raw.get("event")
    if kind not in allowed or type(raw.get("ms")) is not int:
        return None
    event = {"event": kind, "ms": raw["ms"]}
    for key in allowed[kind]:
        if key in raw and type(raw[key]) in (bool, int, float):
            event[key] = raw[key]
    if kind == "runtime":
        import re

        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(raw.get("qt", ""))):
            return None
        event["qt"] = raw["qt"]
    if kind == "sample" and raw.get("view") in ("a", "b", "aggregate", "none", "other"):
        event["view"] = raw["view"]
    if kind in ("request", "response"):
        if raw.get("category") not in ("history", "auth", "image", "events", "other"):
            return None
        event["category"] = raw["category"]
    return event


def percentile(values, percentile_value):
    if not values:
        return None
    values = sorted(values)
    return values[round((len(values) - 1) * percentile_value)]


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
        heartbeats = [e["ms"] for e in observed]
        heartbeat_gaps = [b - a for a, b in itertools.pairwise(heartbeats)]
        kind = start["kind"]
        key = (start["id"], kind)
        metrics = {
            "frames": len(rendered),
            "pictures": len(pictures),
            "max_heartbeat_ms": max(heartbeat_gaps, default=0),
            "p95_frame_ms": percentile(frame_intervals, 0.95),
            "max_frame_ms": max(frame_intervals, default=0),
        }
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
            visible = []
            for mark in received:
                previous = next(
                    (e for e in reversed(pictures) if e["ms"] < mark["ms"]), None
                )
                if previous is not None:
                    later = next(
                        (
                            e
                            for e in pictures
                            if e["ms"] >= mark["ms"]
                            and e["signature"] != previous["signature"]
                        ),
                        None,
                    )
                    if later:
                        visible.append(later["ms"] - mark["ms"])
            metrics["wheel_to_changed_capture_p95_ms"] = percentile(visible, 0.95)
            if not visible:
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
            if (
                kind == "typing"
                and sample.get("draftLength", 0) > 0
                and not sample.get("editorFocus")
            ):
                failures.add("typing-focus-lost")
        if start.get("expectContent") and pictures and limits:
            for picture in pictures:
                if picture["occupancy"] < limits["min_occupancy"]:
                    failures.add("transient-blank-frame")
        if (
            kind in {"loaded-scroll", "reverse-scroll", "paged-scroll"}
            and limits
            and frame_intervals
        ) and max(frame_intervals) > limits["active_frame_ms"]:
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
            ready = next(
                (
                    e
                    for e in observed
                    if (
                        e.get("draftLength", 0) > 0
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
        if kind == "stopped" and "anchor" not in start:
            blocked.add("viewport-anchor-unobserved")
        if kind == "paged-scroll":
            metrics["older_requests"] = sum(
                e["event"] == "request"
                and e.get("category") == "history"
                and e.get("older")
                for e in window
            )
            if metrics["older_requests"] < 2:
                blocked.add("several-pagination-boundaries-unobserved")
        if kind == "loaded-scroll" and any(
            e["event"] == "request"
            and e.get("category") == "history"
            and e.get("older")
            for e in window
        ):
            blocked.add("loaded-range-not-isolated")
        if kind == "warm-delayed":
            pending = served = False
            for e in window:
                if e["event"] == "request" and e.get("category") == "history":
                    pending = True
                elif e["event"] == "response" and e.get("category") == "history":
                    pending = False
                if pending and e["event"] == "sample" and e.get("count", 0) > 0:
                    served = True
            if not served:
                blocked.add("cached-content-during-revalidation-unobserved")
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
    return {
        "status": "FAIL" if failures else "BLOCKED" if blocked else "PASS",
        "failures": sorted(failures),
        "blocked": sorted(blocked),
        "phases": [dict(id=i, kind=kind, **m) for (i, kind), m in phases.items()],
        "resources": {
            "rss_peak_kib": max((e["rss"] for e in usage), default=0),
            "rss_final_kib": usage[-1]["rss"] if usage else None,
            "cpu_ticks": (usage[-1]["cpu"] - usage[0]["cpu"]) if usage else None,
        },
        "requests": dict(
            Counter(e["category"] for e in events if e["event"] == "request")
        ),
        "cache_hits": max((e.get("hits", 0) for e in samples), default=0),
        "frame_interval_median_ms": median(
            [b - a for a, b in itertools.pairwise(frames)]
        )
        if len(frames) > 1
        else None,
    }
