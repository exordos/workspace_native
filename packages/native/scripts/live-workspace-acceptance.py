#!/usr/bin/env python3
"""Run the installed Linux client against a dedicated Workspace fixture in a private Sway session."""

import argparse
import contextlib
import hashlib
import itertools
import json
import os
import re
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import urlsplit
from uuid import UUID, uuid4

from live_acceptance_metrics import evaluate, sanitize_native_event


def clock():
    return time.monotonic_ns() // 1_000_000


class Blocked(Exception):
    """A sanitized, fixed infrastructure reason suitable for the public report."""


def command(argv, *, env, data=None, timeout=8):
    try:
        return subprocess.run(
            argv, input=data, capture_output=True, check=True, env=env, timeout=timeout
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise Blocked("tool-" + Path(argv[0]).name) from exc


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def fixture(document):
    required = ("origin", "backend_version", "project", "stream", "topic_a", "topic_b")
    if any(not isinstance(document.get(key), str) for key in required):
        raise Blocked("fixture-fields")
    origin = urlsplit(document["origin"])
    if (
        origin.scheme != "https"
        or not origin.hostname
        or origin.username
        or origin.password
        or origin.query
        or origin.fragment
        or origin.path not in ("", "/")
    ):
        raise Blocked("fixture-origin")
    try:
        for field in ("project", "stream", "topic_a", "topic_b"):
            document[field] = str(UUID(document[field]))
    except ValueError as exc:
        raise Blocked("fixture-identifiers") from exc
    if document["topic_a"] == document["topic_b"] or not re.fullmatch(
        r"[0-9A-Za-z][0-9A-Za-z._+-]{0,60}", document["backend_version"]
    ):
        raise Blocked("fixture-version-or-topics")
    return document


class Desktop:
    def __init__(self, config, package, env):
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi, GLib

        self.atspi = Atspi
        self.context = GLib.MainContext.default()
        self.accessibility_error = GLib.Error
        self.config = config
        self.package = package
        self.env = env.copy()
        self.events = []
        self.lock = threading.Lock()
        self.proc = self.compositor = self.keyring = self.pointer = self.keyboard = None
        self.capture_stop = threading.Event()
        self.threads = []
        self.sequence = 0
        self.pid = None
        self.last_tick = 0
        self.authenticated = False
        self.pane = None
        self.lost_watchdog = False

    def record(self, event, **fields):
        with self.lock:
            self.events.append(dict(event=event, ms=clock(), **fields))

    def latest(self, kind):
        with self.lock:
            return next((e for e in reversed(self.events) if e["event"] == kind), {})

    def compositor_start(self, root):
        config = root / "sway.conf"
        config.write_text(
            "output * mode 1440x900\nseat seat0 fallback true\n"
            'default_border none\nfor_window [app_id="com.exordos.workspace.native.preview"] fullscreen enable\n'
        )
        self.env.pop("DISPLAY", None)
        self.env.pop("WAYLAND_DISPLAY", None)
        self.env.update(
            WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1"
        )
        self.compositor = subprocess.Popen(
            ["sway", "-c", str(config)],
            env=self.env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for _ in range(100):
            displays = [p for p in root.glob("wayland-*") if p.is_socket()]
            sockets = list(root.glob("sway-ipc*.sock"))
            if displays and sockets:
                self.env.update(
                    WAYLAND_DISPLAY=displays[0].name, SWAYSOCK=str(sockets[0])
                )
                break
            if self.compositor.poll() is not None:
                raise Blocked("compositor-start")
            time.sleep(0.05)
        else:
            raise Blocked("compositor-socket")
        protocol = Path(__file__).resolve().parents[1] / "tests/wayland"
        xml = str(protocol / "wlr-virtual-pointer-unstable-v1.xml")
        command(
            ["wayland-scanner", "client-header", xml, str(root / "virtual-pointer.h")],
            env=self.env,
        )
        command(
            ["wayland-scanner", "private-code", xml, str(root / "virtual-pointer.c")],
            env=self.env,
        )
        flags = (
            command(
                ["pkg-config", "--cflags", "--libs", "wayland-client"], env=self.env
            )
            .decode()
            .split()
        )
        command(
            [
                "cc",
                "-O2",
                "-I",
                str(root),
                str(protocol / "live-pointer.c"),
                str(root / "virtual-pointer.c"),
                "-o",
                str(root / "pointer"),
                *flags,
            ],
            env=self.env,
        )
        self.pointer = subprocess.Popen(
            [str(root / "pointer")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=self.env,
        )
        self.pointer_ack(b"ready")
        # The throwaway home and private session bus isolate both vault and cache across runs.
        if self.config:
            # Provision and own one foreground daemon, as in secure-store-platform-test.
            self.keyring = subprocess.Popen(
                [
                    "gnome-keyring-daemon",
                    "--foreground",
                    "--unlock",
                    "--components=secrets",
                    "--control-directory",
                    self.env["GNOME_KEYRING_CONTROL"],
                ],
                env=self.env,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            self.keyring.stdin.write(os.urandom(30).hex().encode() + b"\n")
            self.keyring.stdin.close()
            control = Path(self.env["GNOME_KEYRING_CONTROL"]) / "control"
            stop_at = time.monotonic() + 5
            while (
                not control.is_socket()
                and self.keyring.poll() is None
                and time.monotonic() < stop_at
            ):
                time.sleep(0.04)
            if not control.is_socket():
                raise Blocked("keyring-control-unavailable")

    def pointer_ack(self, expected):
        if (
            not select.select([self.pointer.stdout], [], [], 3)[0]
            or self.pointer.stdout.readline().strip() != expected
        ):
            raise Blocked("wayland-pointer-unavailable")

    def point(self, instruction):
        self.pointer.stdin.write((instruction + "\n").encode("ascii"))
        self.pointer.stdin.flush()
        self.pointer_ack(b"ok")

    def launch(self):
        self.last_tick = clock()
        self.record("launch")
        self.authenticated = False
        self.lost_watchdog = False
        self.pane = None
        self.capture_stop.clear()
        read_fd, write_fd = os.pipe()
        app_env = dict(
            self.env,
            QT_QPA_PLATFORM="wayland",
            QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1",
            WORKSPACE_NATIVE_ACCEPTANCE_FD=str(write_fd),
            LD_LIBRARY_PATH=str(self.package / "lib"),
        )
        with os.fdopen(write_fd, "wb"):
            self.proc = subprocess.Popen(
                [str(self.package / "bin/workspace-native")],
                env=app_env,
                pass_fds=(write_fd,),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        self.pid = self.proc.pid

        def receive():
            with os.fdopen(read_fd) as channel:
                for line in channel:
                    try:
                        event = sanitize_native_event(json.loads(line))
                    except (TypeError, ValueError):
                        event = None
                    if event is None:
                        continue
                    if event["event"] == "sample":
                        self.last_tick = clock()
                        self.authenticated = event["authenticated"]
                    with self.lock:
                        self.events.append(event)

        self.threads = [
            threading.Thread(target=receive, daemon=True),
            threading.Thread(target=self.monitor, daemon=True),
            threading.Thread(target=self.capture, daemon=True),
        ]
        for thread in self.threads:
            thread.start()

    def monitor(self):
        while not self.capture_stop.wait(0.04):
            if self.proc.poll() is not None:
                return
            now = clock()
            if now - self.last_tick > 10000 and not self.lost_watchdog:
                self.lost_watchdog = True
                self.record("watchdog" if self.authenticated else "telemetry-missing")
            try:
                fields = (
                    Path(f"/proc/{self.pid}/stat").read_text().rsplit(")", 1)[1].split()
                )
                self.record(
                    "process",
                    cpu=int(fields[11]) + int(fields[12]),
                    rss=int(fields[21]) * os.sysconf("SC_PAGE_SIZE") // 1024,
                )
            except (FileNotFoundError, ProcessLookupError):
                return

    def capture(self):
        from PIL import Image, ImageStat

        # Continuous compositor output keeps a single bad frame in the evidence even if
        # Qt becomes idle before the monitor next samples process usage.
        while not self.capture_stop.is_set() and not (self.authenticated and self.pane):
            self.capture_stop.wait(0.02)
        if self.capture_stop.is_set():
            return
        x, y, w, h = self.pane
        # Capture the conversation and its composer separately from a single presented frame.
        capture_height = 900 - y
        pane_bottom = min(143, max(1, round(144 * h / capture_height)))
        recorder = None
        try:
            recorder = subprocess.Popen(
                [
                    "wf-recorder",
                    "-D",
                    "-g",
                    f"{x},{y} {w}x{capture_height}",
                    "-c",
                    "rawvideo",
                    "-F",
                    "scale=192:144",
                    "-x",
                    "gray",
                    "-m",
                    "rawvideo",
                    "-f",
                    "pipe:1",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=self.env,
            )
            frame_bytes = 192 * 144
            buffer = bytearray()
            while not self.capture_stop.is_set():
                if not select.select([recorder.stdout], [], [], 0.1)[0]:
                    continue
                chunk = os.read(recorder.stdout.fileno(), frame_bytes - len(buffer))
                if not chunk:
                    raise Blocked("compositor-capture-ended")
                buffer.extend(chunk)
                if len(buffer) != frame_bytes:
                    continue
                full = Image.frombytes("L", (192, 144), bytes(buffer))
                image = full.crop((0, 0, 192, pane_bottom))
                editor = full.crop((0, pane_bottom, 192, 144))
                buffer.clear()
                background = int(ImageStat.Stat(image).median[0])
                ink = image.point(
                    lambda v, bg=background: 255 if abs(v - bg) > 16 else 0
                )
                tiles = list(ink.resize((16, 12), Image.Resampling.BOX).getdata())
                digest = hashlib.sha256(image.tobytes()).hexdigest()[:16]
                self.record(
                    "picture",
                    signature=digest,
                    editorSignature=hashlib.sha256(editor.tobytes()).hexdigest()[:16],
                    occupancy=sum(tiles) / (255 * len(tiles)),
                    cells="".join(format(min(15, v // 16), "x") for v in tiles),
                )
        except (OSError, ValueError, Blocked):
            if not self.capture_stop.is_set():
                self.record("capture-unavailable")
        finally:
            stop(recorder)

    def close_app(self, graceful=False):
        self.capture_stop.set()
        stop(self.keyboard)
        self.keyboard = None
        if graceful and self.proc and self.proc.poll() is None:
            command(["swaymsg", f"[pid={self.pid}]", "kill"], env=self.env)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired as exc:
                raise Blocked("client-window-did-not-close") from exc
        stop(self.proc)
        for thread in self.threads:
            thread.join(timeout=4)
        self.pane = None

    def lookup(self, suffix, timeout=12, *, offscreen=False):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with contextlib.suppress(self.accessibility_error):
                while self.context.pending():
                    self.context.iteration(False)
                desktop = self.atspi.get_desktop(0)
                apps = (
                    desktop.get_child_at_index(i)
                    for i in range(desktop.get_child_count())
                )
                nodes = [
                    app for app in apps if app and app.get_process_id() == self.pid
                ]
                while nodes:
                    element = nodes.pop()
                    if (element.get_accessible_id() or "").split(".")[-1] == suffix:
                        state = element.get_state_set()
                        if (
                            offscreen
                            or state.contains(self.atspi.StateType.VISIBLE)
                            and state.contains(self.atspi.StateType.SHOWING)
                        ):
                            return element
                    for i in range(element.get_child_count()):
                        child = element.get_child_at_index(i)
                        if child is not None:
                            nodes.append(child)
            if self.proc.poll() is not None:
                raise Blocked("client-exited")
            if self.lost_watchdog:
                raise Blocked("client-observation-unavailable")
            time.sleep(0.08)
        # All lookup prefixes are fixed control names. Never publish fixture identifiers.
        raise Blocked("visible-control-unavailable-" + suffix.split("-", 1)[0])

    def geometry(self, suffix, *, offscreen=False, timeout=12):
        rect = (
            self.lookup(suffix, timeout=timeout, offscreen=offscreen)
            .get_component_iface()
            .get_extents(self.atspi.CoordType.WINDOW)
        )
        if rect.width < 2 or rect.height < 2:
            raise Blocked("control-geometry")
        return rect.x, rect.y, rect.width, rect.height

    def position(self, rect):
        tree = json.loads(command(["swaymsg", "-r", "-t", "get_tree"], env=self.env))
        nodes = [tree]
        while nodes:
            n = nodes.pop()
            if n.get("focused") and n.get("pid") == self.pid:
                x, y, w, h = rect
                if not n.get("fullscreen_mode"):
                    raise Blocked("client-not-fullscreen")
                self.point(f"move {round(x + w / 2)} {round(y + h / 2)}")
                return
            nodes.extend(n.get("nodes", []) + n.get("floating_nodes", []))
        raise Blocked("client-not-focused")

    def click(self, suffix):
        self.position(self.geometry(suffix))
        self.point("click")

    def fill(self, suffix, value):
        field = self.lookup(suffix)
        if not field.get_component_iface().grab_focus():
            raise Blocked("editable-focus")
        command(
            ["wtype", "-M", "ctrl", "-k", "a", "-m", "ctrl", "-"],
            env=self.env,
            data=value.encode(),
        )

    def until(self, predicate, reason, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while self.context.pending():
                self.context.iteration(False)
            if predicate():
                return
            if self.lost_watchdog:
                raise Blocked("client-observation-unavailable")
            time.sleep(0.04)
        raise Blocked(reason)

    def login(self):
        self.fill("serverInput", self.config["origin"])
        self.click("organizationNextButton")
        try:
            self.lookup("userInput", 2)
        except Blocked:
            # The asynchronously inserted realm preview can move Next between lookup and press.
            self.click("organizationNextButton")
        self.fill("userInput", self.config["username"])
        self.fill("passwordInput", self.config["password"])
        self.click("signInButton")
        if self.config.get("otp"):
            self.fill("otpInput", self.config["otp"])
            self.click("otpConfirmButton")
        self.click("projectSelect")
        self.click("projectOption-" + self.config["project"])
        self.click("projectLoginButton")
        self.until(lambda: self.authenticated, "authentication-or-package-trace")

    @contextlib.contextmanager
    def action(self, kind, expect=True, **fields):
        self.sequence += 1
        seq = self.sequence
        self.record("action", id=seq, kind=kind, expectContent=expect, **fields)
        try:
            yield
        finally:
            self.record("end", id=seq)

    def observe_pane(self):
        self.until(
            lambda: self.latest("sample").get("width", 0) > 0, "message-pane-unobserved"
        )
        sample = self.latest("sample")
        rect = tuple(sample[k] for k in ("paneX", "paneY", "width", "height"))
        x, y, w, h = rect
        if x < 0 or y < 0 or w < 2 or h < 2 or x + w > 1440 or y + h > 900:
            raise Blocked("message-pane-outside-output")
        self.pane = rect

    def sidebar_click(self, identifier, timeout=4):
        for _ in range(30):
            x, y, w, h = self.geometry(identifier, offscreen=True, timeout=timeout)
            mid = y + h / 2
            # At the top of this output the activity controls cover the scrolled-off sidebar.
            if 180 <= mid <= 825 or (
                identifier.startswith("stream-") and 135 <= mid <= 825
            ):
                self.position((x, y, w, h))
                self.point("click")
                return
            self.point(f"move {max(5, min(1435, round(x + w / 2)))} 480")
            self.point("wheel 18" if mid > 825 else "wheel -18")
            time.sleep(0.12)
        raise Blocked("sidebar-scroll-into-view")

    def topic(self, index):
        identifier = "topic-" + self.config["topic_" + index]
        stream = self.config["stream"]
        try:
            self.lookup(identifier, 1.2, offscreen=True)
        except Blocked:
            try:
                self.sidebar_click("showMoreTopics-" + stream, timeout=1.5)
            except Blocked:
                self.sidebar_click("stream-" + stream)
                try:
                    self.lookup(identifier, 1.2, offscreen=True)
                except Blocked:
                    self.sidebar_click("showMoreTopics-" + stream)
        self.sidebar_click(identifier)
        self.observe_pane()
        self.until(
            lambda: (
                self.latest("sample").get("view") == index
                and self.latest("sample").get("count", 0) > 0
            ),
            "history-unavailable",
        )

    def scroll(self, kind, direction, bursts, on_input=None):
        x, y, w, h = self.pane
        self.position((x, y, 24, h) if kind == "paged-scroll" else (x, y, w, h))
        with self.action(
            kind, direction=direction, intent=self.latest("sample").get("intent", 0)
        ):
            time.sleep(0.16)
            for i in range(bursts):
                if i == 2 and on_input:
                    on_input()
                # wl_pointer vertical axes use positive for down, Qt angle deltas positive for up.
                self.point(f"wheel {-direction * 12}")
                time.sleep(0.025)
            time.sleep(0.35)

    def wait_for_saved_session(self):
        def saved():
            metadata_root = Path(self.env["XDG_DATA_HOME"])
            for metadata in metadata_root.glob("**/session-vault/accounts.ini"):
                if any(
                    line.startswith(b"currentAccountId=")
                    and line.strip() != b"currentAccountId="
                    for line in metadata.read_bytes().splitlines()
                ):
                    return True
            return False

        self.until(saved, "session-keyring-save", 12)

    def open_fixture_stream(self):
        # Selecting the stream already expands its topic rows.
        self.sidebar_click("stream-" + self.config["stream"])
        self.topic("a")

    def controls(self, limits):
        self.launch()
        self.login()
        self.open_fixture_stream()
        self.scroll("loaded-scroll", 1, 6)
        if not limits:
            # Calibrate detection of our deliberately large disturbances on this idle pane.
            # These control sensitivities are never used to accept a release.
            start = clock()
            time.sleep(1)
            healthy = [e for e in self.events if e["ms"] >= start]
            beats = [e["ms"] for e in healthy if e["event"] == "sample"]
            occupancy = [e["occupancy"] for e in healthy if e["event"] == "picture"]
            anchors = [e["anchorY"] for e in healthy if "anchorY" in e]
            if (
                len(beats) < 4
                or len(occupancy) < 4
                or not anchors
                or min(occupancy) <= 0
            ):
                raise Blocked("control-reference-unobserved")
            limits = {
                "gui_stall_ms": 2 * max(b - a for a, b in itertools.pairwise(beats)),
                "min_occupancy": min(occupancy) / 2,
                "settled_anchor_px": max(anchors) - min(anchors) + 1,
                "visible_response_ms": float("inf"),
                "active_frame_ms": float("inf"),
            }
        results = {}
        for name, expected in (
            ("stall", "gui-stall"),
            ("jump", "viewport-jump-after-stop"),
            ("blank", "transient-blank-frame"),
            ("missing-input", "no-pane-wheel"),
        ):
            start = clock()
            sample = self.latest("sample")
            kind = "loaded-scroll" if name == "missing-input" else "stopped"
            fields = {
                k: sample[k] for k in ("anchor", "anchorY", "intent") if k in sample
            }
            with self.action(kind, direction=1, **fields):
                time.sleep(0.2)
                if name == "stall":
                    os.kill(self.pid, signal.SIGSTOP)
                    try:
                        time.sleep(max(0.5, limits["gui_stall_ms"] / 1000 + 0.3))
                    finally:
                        os.kill(self.pid, signal.SIGCONT)
                elif name == "jump":
                    self.position(self.pane)
                    for _ in range(5):
                        self.point("wheel -20")
                elif name == "blank":
                    command(
                        ["swaymsg", "workspace", "__workspace_acceptance_blank"],
                        env=self.env,
                    )
                    try:
                        time.sleep(0.5)
                    finally:
                        command(
                            ["swaymsg", "workspace", "back_and_forth"], env=self.env
                        )
                time.sleep(0.4)
            report = evaluate([e for e in self.events if e["ms"] >= start], limits)
            results[name] = expected in (
                report["blocked"] if name == "missing-input" else report["failures"]
            )
        return results

    def run(self, extended, peer, network):
        with self.action("cold-start", False):
            self.launch()
            self.login()
            self.open_fixture_stream()
            self.until(
                lambda: bool(self.latest("picture")), "captured-history-unavailable"
            )
            time.sleep(0.1)
        self.topic("b")
        self.topic("a")
        self.wait_for_saved_session()
        self.close_app(graceful=True)
        with self.action("warm-start", False):
            self.launch()
            self.until(lambda: self.authenticated, "saved-session-restore", 30)
            self.observe_pane()
            self.until(
                lambda: self.latest("sample").get("count", 0) > 0,
                "warm-cache-unavailable",
            )
            warm_start = self.latest("runtime")["ms"]
            self.until(
                lambda: self.latest("picture").get("ms", 0) >= warm_start,
                "captured-history-unavailable",
            )
        try:
            self.lookup("topic-" + self.config["topic_b"], 0.7, offscreen=True)
        except Blocked:
            self.sidebar_click("stream-" + self.config["stream"])
        for i in range(4 if extended else 2):
            target = "b" if i % 2 == 0 else "a"
            with self.action("warm-switch", target=target):
                self.topic(target)
                time.sleep(0.25)
        with self.action("aggregate", target="aggregate"):
            self.sidebar_click("stream-" + self.config["stream"])
            time.sleep(0.35)
        self.topic("a")
        self.scroll("loaded-scroll", 1, 8)
        self.scroll("reverse-scroll", -1, 8)
        self.position(self.pane)
        with self.action(
            "touchpad-scroll",
            direction=1,
            intent=self.latest("sample").get("intent", 0),
        ):
            time.sleep(0.16)
            for _ in range(12):
                self.point("finger -6")
                time.sleep(0.02)
            self.point("finger 0")
            time.sleep(0.35)
        with (
            peer.temporary_message("a", peer.message) as create,
            ThreadPoolExecutor(max_workers=1) as worker,
        ):
            arrival = None

            def submit_arrival():
                nonlocal arrival
                arrival = worker.submit(create)

            self.scroll(
                "paged-scroll", 1, 480 if extended else 210, on_input=submit_arrival
            )
            arrival.result()
            time.sleep(0.35)
            settled = self.latest("sample")
            with self.action(
                "stopped",
                **{k: settled[k] for k in ("anchor", "anchorY") if k in settled},
            ):
                time.sleep(0.8)
            with self.action("typing"):
                self.click("messageComposer")
                self.record(
                    "type-start",
                    before=self.latest("picture").get("editorSignature", ""),
                )
                # Keep the virtual keyboard present after typing, as on a real desktop.
                # Removing the last keyboard withdraws Wayland focus from the entire window.
                self.keyboard = subprocess.Popen(
                    ["wtype", "-", "-s", "600000"],
                    env=self.env,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                self.keyboard.stdin.write(b"acceptance draft")
                self.keyboard.stdin.close()
                self.until(
                    lambda: self.latest("sample").get("draftLength", 0) > 0,
                    "typing-not-observed",
                )
                peer.exercise(self)
                time.sleep(0.4)
        # Invalidate only the warmed target's history through a real peer arrival.
        with peer.temporary_message("b", str(uuid4())) as create:
            create()
            with self.action("warm-delayed"), network.disturb("delay"):
                self.topic("b")
                time.sleep(0.8)
        with self.action("reconnect"):
            previous_connections = network.matched
            with network.disturb("disconnect"):
                self.topic("a")
                time.sleep(0.6)
            restored = clock()

            def online():
                return (
                    network.matched > previous_connections
                    and self.latest("sample").get("ready")
                    and any(
                        e["event"] == "response"
                        and e["ms"] >= restored
                        and 200 <= e.get("status", 0) < 300
                        for e in list(self.events)
                    )
                )

            self.until(online, "revalidation-not-restored", 30)
        for i in range(12 if extended else 3):
            target = "b" if i % 2 == 0 else "a"
            with self.action("repeated-navigation", target=target):
                self.topic(target)
                time.sleep(0.2)


class Peer:
    def __init__(self, config, role):
        self.config = config
        self.identity = config.get("peers", {}).get(role)
        self.message = str(uuid4())

    def request(self, method, path, payload=None):
        import requests

        try:
            response = requests.request(
                method,
                self.config["origin"].rstrip("/") + path,
                headers={"Authorization": "Bearer " + self.identity["token"]},
                json=payload,
                timeout=(5, 10),
                allow_redirects=False,
            )
            if not 200 <= response.status_code < 300:
                raise Blocked("peer-api-" + str(response.status_code))
            return response.json() if response.content else {}
        except (requests.RequestException, ValueError) as exc:
            raise Blocked("peer-api-transport") from exc

    @contextlib.contextmanager
    def temporary_message(self, topic, identifier):
        if not self.identity or not self.identity.get("token"):
            raise Blocked("peer-role-unavailable")
        collection = "/api/workspace/v1/messenger/messages/"
        item = None
        expected = {
            "stream_uuid": self.config["stream"],
            "topic_uuid": self.config["topic_" + topic],
            "payload": {"kind": "markdown", "content": "Acceptance arrival"},
        }

        def matches(row):
            return (
                isinstance(row, dict)
                and row.get("user_uuid") == self.identity["uuid"]
                and all(row.get(k) == v for k, v in expected.items())
            )

        def create():
            nonlocal item
            created = self.request("POST", collection, {"uuid": identifier, **expected})
            if not matches(created):
                raise Blocked("peer-created-message-route")
            public_id = str(UUID(created["uuid"]))
            item = collection + public_id
            if topic == "a":
                self.public_message = public_id
            if not matches(self.request("GET", item)):
                raise Blocked("peer-message-route")

        try:
            yield create
        finally:
            if item:
                self.request("DELETE", item)

    def exercise(self, desktop):
        api = "/api/workspace/v1/messenger"
        item = api + "/messages/" + self.public_message
        self.request(
            "PUT",
            item,
            {"payload": {"kind": "markdown", "content": "Acceptance edit"}},
        )
        self.request(
            "POST",
            api + "/message_reactions/",
            {"message_uuid": self.public_message, "emoji_name": "thumbs_up"},
        )
        read_target = str(UUID(self.config["read_boundary"]))
        boundary = self.request("GET", api + "/messages/" + read_target)
        if (
            boundary.get("stream_uuid") != self.config["stream"]
            or boundary.get("topic_uuid") != self.config["topic_a"]
        ):
            raise Blocked("peer-read-route")
        self.request(
            "POST",
            api + "/messages/" + read_target + "/actions/read_up_to/invoke",
            {},
        )
        peer = str(UUID(self.identity["uuid"]))
        self.request(
            "POST",
            "/api/workspace/v1/users/" + peer + "/actions/presence/invoke",
            {"status": "active"},
        )
        desktop.until(
            lambda: any(
                e.get("peer") for e in list(desktop.events) if e["event"] == "realtime"
            ),
            "peer-realtime-delivery",
        )
        desktop.record("peer-workload")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--package",
        type=Path,
        required=True,
        help="extracted package root containing release.json",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("smoke", "extended", "controls", "probe", "login"),
        default="smoke",
    )
    parser.add_argument("--peer-role", default="writer")
    parser.add_argument("--limits", type=Path)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        args.output.mkdir(parents=True, exist_ok=True)
        args.package = args.package.resolve()
        args.output = args.output.resolve()
        with tempfile.TemporaryDirectory(prefix="workspace-live-gui-") as temp:
            env = {
                k: os.environ[k]
                for k in (
                    "PATH",
                    "LANG",
                    "LC_ALL",
                    "TZ",
                    "LD_LIBRARY_PATH",
                    "SSL_CERT_FILE",
                    "REQUESTS_CA_BUNDLE",
                    "PKG_CONFIG_PATH",
                    "PKG_CONFIG_SYSROOT_DIR",
                    "LIBRARY_PATH",
                    "CPATH",
                )
                if k in os.environ
            }
            env.update(
                HOME=temp,
                XDG_RUNTIME_DIR=temp,
                XDG_CONFIG_HOME=temp + "/config",
                XDG_DATA_HOME=temp + "/data",
                XDG_CACHE_HOME=temp + "/cache",
                XDG_STATE_HOME=temp + "/state",
                GNOME_KEYRING_CONTROL=temp + "/keyring",
            )
            command_line = [
                "dbus-run-session",
                "--",
                sys.executable,
                __file__,
                "--worker",
                "--mode",
                args.mode,
                "--package",
                str(args.package),
                "--output",
                str(args.output),
                "--peer-role",
                args.peer_role,
            ]
            if args.limits:
                command_line += ["--limits", str(args.limits.resolve())]
            # All runner subprocess diagnostics stay off shared CI logs. Only the report is exported.
            proc = None
            try:
                proc = subprocess.Popen(
                    command_line,
                    env=env,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
                return proc.wait(timeout=900)
            except (OSError, subprocess.TimeoutExpired):
                (args.output / "report.json").write_text(
                    '{"status":"BLOCKED","blocked":["worker-timeout-or-unavailable"]}\n'
                )
                return 2
            finally:
                if proc is not None:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
    from live_acceptance_network import NetworkConditions

    result = {"status": "BLOCKED", "blocked": ["runner-infrastructure"]}
    desktop = None
    limits = None
    try:
        metadata = json.loads((args.package / "release.json").read_text())
        if (
            metadata.get("system") != "linux"
            or not (args.package / "bin/workspace-native").is_file()
        ):
            raise Blocked("installed-linux-package")
        version = metadata["version"]
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise Blocked("package-version")
        config = fixture(json.load(sys.stdin)) if args.mode != "probe" else {}
        env = dict(os.environ)
        desktop = Desktop(config, args.package, env)
        desktop.compositor_start(Path(env["XDG_RUNTIME_DIR"]))
        if args.mode == "probe":
            desktop.launch()
            desktop.lookup("serverInput")
            desktop.fill("serverInput", "https://127.0.0.1:9")
            next_button = desktop.lookup("organizationNextButton")

            def enabled_after_typing():
                next_button.clear_cache()
                return next_button.get_state_set().contains(
                    desktop.atspi.StateType.ENABLED
                )

            desktop.until(enabled_after_typing, "wayland-keyboard-input-unobserved", 3)
            desktop.until(
                lambda: bool(desktop.latest("runtime")),
                "package-without-acceptance-trace",
                3,
            )
            clicked = clock()
            desktop.click("organizationNextButton")
            desktop.until(
                lambda: desktop.latest("click").get("ms", 0) >= clicked,
                "wayland-click-unobserved",
                3,
            )
            result = {"status": "PROBE", "runtime": desktop.latest("runtime")}
        else:
            limits = None
            if args.limits:
                limits = json.loads(args.limits.read_text())
                if limits.get("reference") != config.get("reference"):
                    raise Blocked("limits-runner-reference")
                required = (
                    "gui_stall_ms",
                    "visible_response_ms",
                    "settled_anchor_px",
                    "min_occupancy",
                    "active_frame_ms",
                )
                if any(
                    not isinstance(limits.get(k), (float, int)) or limits[k] <= 0
                    for k in required
                ):
                    raise Blocked("invalid-reviewed-limits")
            peer = Peer(config, args.peer_role)
            desktop.env["WORKSPACE_NATIVE_ACCEPTANCE_PEER_MESSAGE"] = peer.message
            if peer.identity and peer.identity.get("uuid"):
                desktop.env["WORKSPACE_NATIVE_ACCEPTANCE_PEER_ID"] = str(
                    UUID(peer.identity["uuid"])
                )
            desktop.env["WORKSPACE_NATIVE_ACCEPTANCE_TOPIC_A"] = config["topic_a"]
            desktop.env["WORKSPACE_NATIVE_ACCEPTANCE_TOPIC_B"] = config["topic_b"]
            with NetworkConditions(config["origin"]) as net:
                desktop.env["WORKSPACE_NATIVE_ACCEPTANCE_PROXY_PORT"] = str(
                    net.local_port
                )
                if args.mode == "login":
                    desktop.launch()
                    desktop.login()

                    desktop.wait_for_saved_session()
                    desktop.close_app(graceful=True)
                    desktop.launch()
                    desktop.until(
                        lambda: desktop.authenticated, "saved-session-restore", 25
                    )
                    result = {
                        "status": "PREFLIGHT",
                        "authenticated": True,
                        "restored": True,
                    }
                elif args.mode == "controls":
                    checks = desktop.controls(limits)
                    result = {
                        "status": "CONTROLS" if all(checks.values()) else "FAIL",
                        "controls": checks,
                    }
                else:
                    desktop.run(args.mode == "extended", peer, net)
                    if not net.matched or not all(net.effects.values()):
                        raise Blocked("network-fault-not-delivered")
                    result = evaluate(desktop.events, limits)
            result["runtime"] = desktop.latest("runtime")
            result["compositor"] = (
                command(["sway", "--version"], env=desktop.env).decode().strip()
            )
            result["kernel"] = os.uname().release
            result["logical_cpus"] = os.cpu_count()
            result["input"] = "Wayland virtual pointer"
            result["capture"] = (
                "continuous Wayland output via wf-recorder, 16 by 12 occupancy tiles"
            )
            result["mode"] = args.mode
            result["backend_version"] = config["backend_version"]
        result["package_version"] = version
        with (args.package / "bin/workspace-native").open("rb") as binary:
            result["binary_sha256"] = hashlib.file_digest(binary, "sha256").hexdigest()
    except Blocked as exc:
        result = (
            evaluate(desktop.events, limits)
            if desktop and args.mode in ("smoke", "extended")
            else {"status": "BLOCKED", "blocked": []}
        )
        result["blocked"].append(str(exc))
        if result["status"] != "FAIL":
            result["status"] = "BLOCKED"
    except (
        ImportError,
        KeyError,
        TypeError,
        ValueError,
        OSError,
        RuntimeError,
        subprocess.SubprocessError,
    ) as error:
        # Retain only source line numbers in this runner, never exception values or paths.
        trace = error.__traceback__
        lines = []
        while trace:
            if trace.tb_frame.f_code.co_filename == __file__:
                lines.append(trace.tb_lineno)
            trace = trace.tb_next
        result = {
            "status": "BLOCKED",
            "blocked": ["runner-configuration-or-dependency"],
            "runner_lines": lines,
            "cause": type(error).__name__,
        }
    finally:
        if desktop:
            desktop.close_app()
            stop(desktop.pointer)
            stop(desktop.keyring)
            stop(desktop.compositor)
            if any(e["event"] == "watchdog" for e in desktop.events):
                result["status"] = "FAIL"
                result.setdefault("failures", []).append("gui-watchdog")
            with (args.output / "timeline.jsonl").open("w") as out:
                for e in sorted(desktop.events, key=lambda e: e["ms"]):
                    out.write(json.dumps(e, separators=(",", ":")) + "\n")
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["status"])
    return {
        "PASS": 0,
        "PROBE": 0,
        "PREFLIGHT": 0,
        "CONTROLS": 0,
        "FAIL": 3,
        "BLOCKED": 2,
    }[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
