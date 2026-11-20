#!/usr/bin/env python3
"""Temporal fault controls run independently of credentials or public services."""

import runpy
import socket
import socketserver
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, call

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

    def test_peer_uses_the_public_uuid_returned_by_the_deployed_backend(self):
        runner = runpy.run_path(
            str(
                Path(__file__).resolve().parents[1]
                / "scripts"
                / "live-workspace-acceptance.py"
            )
        )
        cfg = {
            "stream": "stream",
            "topic_a": "topic",
            "peers": {"writer": {"uuid": "peer", "token": "synthetic"}},
        }
        peer = runner["Peer"](cfg, "writer")
        canonical = "11111111-1111-4111-8111-111111111111"
        row = {
            "uuid": canonical,
            "topic_uuid": "topic",
            "stream_uuid": "stream",
            "user_uuid": "peer",
            "payload": {"kind": "markdown", "content": "Acceptance arrival"},
        }
        peer.request = Mock(side_effect=[row, row, {}])
        with peer.temporary_message("a", peer.message) as create:
            create()
        self.assertEqual(
            peer.request.call_args_list[1:],
            [
                call("GET", "/api/workspace/v1/messenger/messages/" + canonical),
                call("DELETE", "/api/workspace/v1/messenger/messages/" + canonical),
            ],
        )

    def test_fixture_stream_selection_keeps_the_auto_expanded_topics_open(self):
        runner = runpy.run_path(
            str(
                Path(__file__).resolve().parents[1]
                / "scripts"
                / "live-workspace-acceptance.py"
            )
        )
        desktop = Mock()
        desktop.config = {"stream": "fixture"}
        runner["Desktop"].open_fixture_stream(desktop)
        self.assertEqual(
            desktop.method_calls, [call.sidebar_click("stream-fixture"), call.topic("a")]
        )

    def test_organization_preview_moving_next_allows_one_retry(self):
        runner = runpy.run_path(
            str(
                Path(__file__).resolve().parents[1]
                / "scripts"
                / "live-workspace-acceptance.py"
            )
        )
        desktop = Mock()
        desktop.config = {
            "origin": "https://example.invalid",
            "username": "user",
            "password": "temporary",
            "project": "fixture",
        }
        desktop.lookup.side_effect = runner["Blocked"]("preview-arrived")
        runner["Desktop"].login(desktop)
        self.assertEqual(
            desktop.click.call_args_list[:2],
            [call("organizationNextButton"), call("organizationNextButton")],
        )

    def test_normal_workload_records_an_action_and_its_end(self):
        runner = runpy.run_path(
            str(
                Path(__file__).resolve().parents[1]
                / "scripts"
                / "live-workspace-acceptance.py"
            )
        )
        desktop = object.__new__(runner["Desktop"])
        desktop.lock = threading.Lock()
        desktop.events = []
        desktop.sequence = 0
        with desktop.action("cold-start", False):
            pass
        self.assertEqual([r["event"] for r in desktop.events], ["action", "end"])
        self.assertEqual(desktop.events[0]["kind"], "cold-start")

    def test_lost_recorder_is_an_infrastructure_failure(self):
        report = evaluate([e for e in trace() if e["event"] != "picture"], LIMITS)
        self.assertEqual(report["status"], "BLOCKED")
        self.assertNotIn("no-visible-wheel-response", report["failures"])

    def test_delayed_navigation_requires_target_pixels_while_its_own_request_is_pending(
        self,
    ):
        rows = [
            {"event": "action", "ms": 0, "id": 1, "kind": "warm-delayed"},
            {
                "event": "request",
                "ms": 2,
                "category": "history",
                "view": "b",
                "index": 4,
            },
            {"event": "sample", "ms": 5, "view": "b", "count": 30},
            {
                "event": "response",
                "ms": 6,
                "category": "history",
                "view": "a",
                "index": 2,
            },
            {"event": "picture", "ms": 7, "occupancy": 0.3, "signature": "content"},
            {
                "event": "response",
                "ms": 9,
                "category": "history",
                "view": "b",
                "index": 4,
            },
            {"event": "end", "ms": 15, "id": 1},
        ]
        missing = "cached-content-during-revalidation-unobserved"
        self.assertNotIn(missing, evaluate(rows, LIMITS)["blocked"])
        rows[4]["ms"] = 11
        self.assertIn(missing, evaluate(rows, LIMITS)["blocked"])

    def test_cpu_ticks_accumulate_across_the_real_restart(self):
        rows = [
            {"event": "launch", "ms": 1},
            {"event": "process", "ms": 2, "cpu": 15, "rss": 20},
            {"event": "launch", "ms": 3},
            {"event": "process", "ms": 4, "cpu": 5, "rss": 18},
        ]
        self.assertEqual(evaluate(rows, None)["resources"]["cpu_ticks"], 20)

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
    def test_shutdown_with_a_connected_client_waiting_for_the_backend(self):
        with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo) as endpoint:
            threading.Thread(target=endpoint.serve_forever, daemon=True).start()
            port = endpoint.server_address[1]
            client = None
            try:
                with NetworkConditions(f"https://127.0.0.1:{port}") as network:
                    client = socket.create_connection(
                        ("127.0.0.1", network.local_port), 2
                    )
                    client.sendall(
                        f"CONNECT 127.0.0.1:{port} HTTP/1.1\r\n\r\n".encode()
                    )
                    self.assertIn(b"200", client.recv(256))
                self.assertEqual(client.recv(256), b"")
            finally:
                if client:
                    client.close()
                endpoint.shutdown()

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
                self.assertGreater(network.effects["disconnect"], 0)
            endpoint.shutdown()
            thread.join(2)


if __name__ == "__main__":
    unittest.main()
