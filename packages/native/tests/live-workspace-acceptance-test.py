#!/usr/bin/env python3
"""Temporal fault controls run independently of credentials or public services."""

import socket
import socketserver
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from live_acceptance_metrics import evaluate, sanitize_native_event
from live_acceptance_network import NetworkConditions

LIMITS = {
    "gui_stall_ms": 120,
    "visible_response_ms": 100,
    "settled_anchor_px": 12,
    "min_occupancy": 0.03,
    "active_frame_ms": 120,
}


def trace(kind="loaded-scroll"):
    return [
        {
            "event": "action",
            "ms": 0,
            "id": 1,
            "kind": kind,
            "intent": 2,
            "direction": 1,
            "expectContent": True,
            "anchor": 8,
            "anchorY": 5,
        },
        {
            "event": "sample",
            "ms": 1,
            "intent": 2,
            "count": 22,
            "anchor": 8,
            "anchorY": 5,
        },
        {"event": "frame", "ms": 2},
        {"event": "picture", "ms": 3, "signature": "before", "occupancy": 0.2},
        {"event": "wheel", "ms": 10, "angle": 120, "pixel": 0, "phase": 2},
        {
            "event": "sample",
            "ms": 18,
            "intent": 3,
            "count": 22,
            "anchor": 8,
            "anchorY": 5,
        },
        {"event": "frame", "ms": 19},
        {"event": "picture", "ms": 20, "signature": "after", "occupancy": 0.25},
        {"event": "end", "id": 1, "ms": 250},
    ]


class Controls(unittest.TestCase):
    def test_healthy_scroll_separates_other_missing_workloads(self):
        result = evaluate(trace(), LIMITS)
        self.assertEqual(result["failures"], [])
        self.assertNotIn("no-pane-wheel", result["blocked"])
        self.assertEqual(result["phases"][0]["wheel_to_changed_capture_p95_ms"], 10)

    def test_missing_pane_input_blocks_even_when_pixels_change(self):
        report = evaluate([e for e in trace() if e["event"] != "wheel"], LIMITS)
        self.assertEqual(report["status"], "BLOCKED")
        self.assertIn("no-pane-wheel", report["blocked"])

    def test_stall_fails_despite_a_final_frame(self):
        rows = trace()
        rows.insert(-1, {"event": "sample", "ms": 200, "intent": 3, "count": 22})
        self.assertIn("gui-stall", evaluate(rows, LIMITS)["failures"])

    def test_one_blank_capture_is_enough(self):
        rows = trace()
        rows.insert(
            -2, {"event": "picture", "ms": 14, "signature": "blank", "occupancy": 0.0}
        )
        self.assertIn("transient-blank-frame", evaluate(rows, LIMITS)["failures"])

    def test_stopped_viewport_jump_is_detected(self):
        rows = trace("stopped")
        rows[5]["anchorY"] = 45
        self.assertIn("viewport-jump-after-stop", evaluate(rows, LIMITS)["failures"])

    def test_only_approved_native_values_enter_artifacts(self):
        raw = {
            "event": "sample",
            "ms": 10,
            "count": 9,
            "origin": "private",
            "draftLength": "secret",
            "view": "private",
        }
        self.assertEqual(
            sanitize_native_event(raw), {"event": "sample", "ms": 10, "count": 9}
        )
        self.assertIsNone(
            sanitize_native_event(
                {"event": "response", "ms": 10, "category": "private-url"}
            )
        )

    def test_missing_reference_or_incomplete_trace_cannot_pass(self):
        for rows in ([], trace()):
            self.assertEqual(evaluate(rows, None)["status"], "BLOCKED")


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(512):
            self.request.sendall(data)


class NetworkControlTests(unittest.TestCase):
    def test_only_the_requested_destination_receives_data_and_disconnect_is_applied(
        self,
    ):
        with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo) as endpoint:
            thread = threading.Thread(target=endpoint.serve_forever, daemon=True)
            thread.start()
            self.addCleanup(thread.join, 2)
            self.addCleanup(endpoint.shutdown)
            port = endpoint.server_address[1]
            with NetworkConditions(f"https://127.0.0.1:{port}") as network:
                addr = ("127.0.0.1", network.local_port)
                with socket.create_connection(addr, 2) as rejected:
                    rejected.sendall(b"CONNECT other.invalid:443 HTTP/1.1\r\n\r\n")
                    self.assertIn(b"403", rejected.recv(256))
                with socket.create_connection(addr, 2) as allowed:
                    allowed.sendall(
                        f"CONNECT 127.0.0.1:{port} HTTP/1.1\r\n\r\n".encode()
                    )
                    self.assertIn(b"200", allowed.recv(256))
                    allowed.sendall(b"fixture")
                    self.assertEqual(allowed.recv(256), b"fixture")
                    with network.disturb("disconnect"):
                        self.assertEqual(allowed.recv(256), b"")
                self.assertEqual(network.matched, 1)
                self.assertGreater(network.effects, 0)
            endpoint.shutdown()
            thread.join(2)


if __name__ == "__main__":
    unittest.main()
