"""Sign in to a loopback TLS fixture with the normal Windows/macOS preview executable."""
from email import policy
from email.parser import BytesParser
import argparse
import contextlib
import ctypes
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import urllib.request

from hosted_capture import HostedCapture


HERE = Path(__file__).resolve().parent
WINDOWS = sys.platform == "win32"
PROJECT = str(uuid.uuid4())
USER = str(uuid.uuid4())
SERVICE = "com.exordos.workspace.native.sessions"
TOKEN_ROUTE = "/api/core/v1/iam/clients/default/actions/get_token/invoke"


def check(condition, error):
    if not condition:
        raise RuntimeError(error)


def wait_for(operation, message, timeout=25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if operation():
            return
        time.sleep(.15)
    raise RuntimeError(message)


class SessionFixture(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.capture = None
        self.selected_file = None
        self.uploaded = False
        self.refresh = secrets.token_hex(24)
        self.base = secrets.token_hex(22)
        self.access = secrets.token_hex(23)
        self.settings_count = 0
        self.login_count = 0
        self.refresh_count = 0
        self.introspect_count = 0

    def get_request(self):
        # Complete TLS on a bounded socket. Qt can speculatively open a
        # connection before it has a request ready.
        connection, address = self.socket.accept()
        connection.settimeout(4)
        try:
            return self.tls.wrap_socket(connection, server_side=True), address
        except BaseException:
            connection.close()
            raise


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, obj):
        payload = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        fixture = self.server
        path = self.path.partition("?")[0]
        if fixture.capture and fixture.capture.handle(self):
            return
        if path.endswith("/messenger/server_settings"):
            fixture.settings_count += 1
            settings = {"realm_name": "Cassi package fixture"}
            if fixture.capture:
                settings["meet_url"] = fixture.capture.origin
            self.reply(200, settings)
        elif path == "/api/core/v1/iam/projects/" and self.headers.get("Authorization") == "Bearer " + fixture.base:
            self.reply(200, [{"uuid": PROJECT, "name": "Package acceptance", "status": "ACTIVE"}])
        elif path.endswith("/actions/introspect") and self.headers.get("Authorization") == "Bearer " + fixture.access:
            fixture.introspect_count += 1
            self.reply(200, {"project_id": PROJECT, "user_info": {"uuid": USER, "username": "package-member"},
                             "permissions": [], "otp_verified": True})
        elif path.startswith("/api/workspace/") and self.headers.get("Authorization") == "Bearer " + fixture.access:
            self.reply(200, [])
        else:
            self.reply(404, {})

    def do_POST(self):
        fixture = self.server
        length = int(self.headers.get("Content-Length") or 0)
        if self.path == "/api/workspace/v1/messenger/files/" and fixture.selected_file:
            check(self.headers.get("Authorization") == "Bearer " + fixture.access, "Unauthenticated file selection")
            content_type = self.headers.get("Content-Type", "")
            check(0 < length < 65536 and content_type.startswith("multipart/form-data;"), "Unexpected file body")
            headers = ("Content-Type: " + content_type + "\r\n\r\n").encode()
            parts = BytesParser(policy=policy.default).parsebytes(headers + self.rfile.read(length))
            files = [part for part in parts.iter_parts() if part.get_filename()]
            check(len(files) == 1, "Expected one selected file")
            selected = fixture.selected_file
            data = selected.read_bytes()
            check(files[0].get_filename() == selected.name and files[0].get_payload(decode=True) == data,
                  "The application did not upload the selected file intact")
            fixture.uploaded = True
            self.reply(201, {"uuid": str(uuid.uuid4()), "stream_uuid": fixture.capture.stream,
                             "name": selected.name, "content_type": "text/plain", "size_bytes": len(data),
                             "hash": hashlib.sha256(data).hexdigest()})
            return
        body = json.loads(self.rfile.read(length) or "{}")
        if self.path != TOKEN_ROUTE:
            self.reply(404, {})
            return
        if body.get("grant_type") == "login+password" and body.get("login") == "package-member" and body.get("password") == "package-demo":
            fixture.login_count += 1
            token = fixture.base
        elif body.get("grant_type") == "refresh_token" and body.get("refresh_token") == fixture.refresh and body.get("scope", "").endswith(PROJECT):
            fixture.refresh_count += 1
            fixture.refresh = secrets.token_hex(24)
            token = fixture.access
        else:
            self.reply(403, {})
            return
        self.reply(200, {"access_token": token, "refresh_token": fixture.refresh, "expires_in": 3600})


def run(*args, **kwargs):
    kwargs.setdefault("timeout", 35)
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


@contextlib.contextmanager
def trust_loopback_certificate(directory):
    openssl = "openssl"
    if sys.platform == "darwin":
        prefix = subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
        openssl = str(Path(prefix) / "bin/openssl")
    run(openssl, "version")
    ca = directory / "ca.pem"
    ca_key = directory / "ca.key"
    cert = directory / "server.pem"
    key = directory / "server.key"
    csr = directory / "request.pem"
    config = directory / "server.ext"
    config.write_text("basicConstraints=critical,CA:FALSE\nsubjectAltName=IP:127.0.0.1\n"
                      "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n"
                      "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    subject = "/CN=Cassi desktop package " + uuid.uuid4().hex
    run(openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-sha256",
        "-subj", subject, "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
        "-addext", "subjectKeyIdentifier=hash", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        "-keyout", ca_key, "-out", ca, **quiet)
    run(openssl, "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=127.0.0.1", "-keyout", key, "-out", csr, **quiet)
    run(openssl, "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
        "-days", "1", "-sha256", "-extfile", config, "-out", cert, **quiet)
    run(openssl, "verify", "-purpose", "sslserver", "-x509_strict", "-verify_ip", "127.0.0.1", "-CAfile", ca, cert)
    der = ssl.PEM_cert_to_DER_cert(ca.read_text())
    thumbprint = hashlib.sha1(der).hexdigest().upper()
    if WINDOWS:
        der_file = directory / "ca.cer"
        der_file.write_bytes(der)
        run("certutil", "-f", "-addstore", "Root", der_file, **quiet)
    else:
        run("sudo", "security", "add-trusted-cert", "-d", "-r", "trustRoot", "-p", "ssl", "-k",
            "/Library/Keychains/System.keychain", ca, **quiet)
    try:
        yield cert, key
    finally:
        if WINDOWS:
            run("certutil", "-delstore", "Root", thumbprint, **quiet)
        else:
            run("sudo", "security", "delete-certificate", "-Z", thumbprint, "/Library/Keychains/System.keychain", **quiet)


def stored(key):
    if WINDOWS:
        api = ctypes.windll.advapi32
        credential = ctypes.c_void_p()
        if api.CredReadW(key, 1, 0, ctypes.byref(credential)):
            api.CredFree(credential)
            return True
        error = ctypes.windll.kernel32.GetLastError()
        check(error == 1168, f"Credential Manager lookup failed with {error}")
        return False
    result = subprocess.run(["security", "find-generic-password", "-s", SERVICE, "-a", key],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    check(result.returncode in (0, 44), f"Keychain lookup failed with {result.returncode}")
    return result.returncode == 0


def drive(pid, mode, origin):
    print(f"Driving the packaged {mode} interaction.", flush=True)
    if WINDOWS:
        run("pwsh", "-NoProfile", "-File", HERE / "windows-account.ps1", "-ApplicationPid", pid,
            "-Mode", mode, "-Origin", origin, timeout=100)
    else:
        run("swift", HERE / "macos-account.swift", pid, mode, origin, timeout=100)


def capture_inventory():
    if WINDOWS:
        output = run("powershell", "-NoProfile", "-File", HERE / "windows-capture-inventory.ps1",
                     stdout=subprocess.PIPE, text=True).stdout
    else:
        script = ('import AVFoundation; import Foundation; '
                  'let devices = ["audio": AVCaptureDevice.devices(for: .audio).count, '
                  '"video": AVCaptureDevice.devices(for: .video).count]; '
                  'FileHandle.standardOutput.write(try JSONSerialization.data(withJSONObject: devices))')
        output = run("swift", "-e", script, stdout=subprocess.PIPE, text=True).stdout
    counts = json.loads(output)
    check(all(type(counts.get(kind)) is int and counts[kind] >= 0 for kind in ("audio", "video")),
          "Could not enumerate the native capture endpoints")
    return counts


def scenario(executable, check_updater, check_capture):
    check(os.environ.get("GITHUB_ACTIONS") == "true" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted",
          "The TLS fixture requires a disposable GitHub-hosted runner")
    check(WINDOWS or sys.platform == "darwin", "A Windows or macOS desktop is required")
    with tempfile.TemporaryDirectory(prefix="cassi-package-session-") as name:
        folder = Path(name)
        with trust_loopback_certificate(folder) as (cert, key), SessionFixture() as fixture:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            fixture.tls = context
            print("Trusted an ephemeral loopback TLS fixture.", flush=True)
            origin = "https://127.0.0.1:" + str(fixture.server_port)
            if check_capture or os.environ.get("CASSI_HOSTED_CHOOSER"):
                fixture.capture = HostedCapture(PROJECT, USER, origin)
                devices = capture_inventory()
                print("Native capture devices:", devices, flush=True)
            identity = json.dumps(["workspace-session-v1", origin, PROJECT, USER], separators=(",", ":"))
            record = hashlib.sha256(identity.encode()).hexdigest()
            check(not stored(record), "The new account already has an OS store entry")
            thread = threading.Thread(target=fixture.serve_forever, daemon=True)
            thread.start()
            try:
                # Verify the independent test issuer, server certificate, IP SAN
                # and organization response before starting the real application.
                control = ssl.create_default_context(cafile=folder / "ca.pem")
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                                     urllib.request.HTTPSHandler(context=control))
                with opener.open(origin + "/api/workspace/v1/messenger/server_settings", timeout=5) as response:
                    check(json.load(response)["realm_name"] == "Cassi package fixture", "The TLS fixture returned the wrong realm")
                fixture.settings_count = 0
                for mode in ("login", "restore"):
                    with (folder / (mode + ".log")).open("wb") as log:
                        app = subprocess.Popen([executable], stdout=log, stderr=log)
                        try:
                            print(f"Started the normal package for {mode}.", flush=True)
                            if mode == "login":
                                drive(app.pid, "login", origin)
                                wait_for(lambda: fixture.introspect_count >= 1 and stored(record), "Login did not populate the native store")
                                print("The signed-in package created its account in the real OS credential store.", flush=True)
                            else:
                                wait_for(lambda: fixture.introspect_count >= 2, "Restart did not authenticate using the stored refresh token")
                                check(fixture.login_count == 1 and fixture.refresh_count >= 2, "Restore used unexpected credentials")
                                if check_updater:
                                    drive(app.pid, "update", origin)
                                    print("The installed application reported its unsupported update channel in App version.", flush=True)
                                if os.environ.get("CASSI_HOSTED_CHOOSER"):
                                    fixture.selected_file = folder / "selected package fixture.txt"
                                    fixture.selected_file.write_text("Selected in an actual OS dialog " + uuid.uuid4().hex)
                                    drive(app.pid, "choose-file", str(fixture.selected_file))
                                    wait_for(lambda: fixture.uploaded, "The selected file did not reach the organization")
                                    print("The installed application's system chooser returned the exact file and bytes.", flush=True)
                                if check_capture:
                                    drive(app.pid, "join-call", origin)
                                    results = fixture.capture.results
                                    wait_for(lambda: all(kind in results for kind in ("page", "audio", "video")),
                                             "The installed meeting did not report both media results", timeout=40)
                                    check(results["page"] == "loaded", "The installed meeting did not load")
                                    for kind in ("audio", "video"):
                                        expected = "live" if devices[kind] else "NotFoundError"
                                        check(results[kind] == expected, f"{kind}: {results[kind]}, expected {expected}")
                                    print("Installed meeting capture:", results, flush=True)
                                    drive(app.pid, "close-call", origin)
                                drive(app.pid, "logout", origin)
                                wait_for(lambda: not stored(record), "Signing out did not remove the OS credential")
                                print("The restarted package restored the rotated session, then sign-out removed the OS credential.", flush=True)
                            drive(app.pid, "quit", origin)
                            check(app.wait(timeout=20) == 0, "The package did not quit normally")
                        except Exception:
                            print(f"Server settings/login/refresh/introspection counts: {fixture.settings_count}/{fixture.login_count}/{fixture.refresh_count}/{fixture.introspect_count}", flush=True)
                            log.flush()
                            diagnostics = (folder / (mode + ".log")).read_text(encoding="utf-8", errors="replace").lower()
                            notes = {word: diagnostics.count(word) for word in ("ssl", "tls", "certificate", "keychain", "credential store")}
                            print(f"Packaged diagnostics counts: {notes}", flush=True)
                            raise
                        finally:
                            if app.poll() is None:
                                app.kill()
                                app.wait()
            finally:
                fixture.shutdown()
                thread.join()
                if stored(record):
                    if WINDOWS:
                        check(ctypes.windll.advapi32.CredDeleteW(record, 1, 0), "Fixture credential cleanup failed")
                    else:
                        run("security", "delete-generic-password", "-s", SERVICE, "-a", record,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    check(not stored(record), "Fixture cleanup left an OS credential")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--check-updater", action="store_true")
    parser.add_argument("--check-capture", action="store_true")
    args = parser.parse_args()
    scenario(args.executable, args.check_updater, args.check_capture)
