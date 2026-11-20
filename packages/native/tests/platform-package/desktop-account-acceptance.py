"""Sign in to a loopback TLS fixture with the normal Windows/macOS preview executable."""
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
        self.refresh = secrets.token_hex(24)
        self.base = secrets.token_hex(22)
        self.access = secrets.token_hex(23)
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
        if path.endswith("/messenger/server_settings"):
            self.reply(200, {"realm_name": "Cassi package fixture"})
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
    ca = directory / "ca.pem"
    ca_key = directory / "ca.key"
    cert = directory / "server.pem"
    key = directory / "server.key"
    csr = directory / "request.pem"
    config = directory / "server.ext"
    config.write_text("subjectAltName=IP:127.0.0.1\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    subject = "/CN=Cassi desktop package " + uuid.uuid4().hex
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-sha256",
        "-subj", subject, "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign",
        "-keyout", ca_key, "-out", ca, **quiet)
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=127.0.0.1", "-keyout", key, "-out", csr, **quiet)
    run("openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
        "-days", "1", "-sha256", "-extfile", config, "-out", cert, **quiet)
    der = ssl.PEM_cert_to_DER_cert(ca.read_text())
    thumbprint = hashlib.sha1(der).hexdigest().upper()
    if WINDOWS:
        der_file = directory / "ca.cer"
        der_file.write_bytes(der)
        run("certutil", "-f", "-addstore", "Root", der_file, **quiet)
    else:
        run("sudo", "security", "add-trusted-cert", "-d", "-r", "trustRoot", "-k",
            "/Library/Keychains/System.keychain", ca, **quiet)
    try:
        yield cert, key
    finally:
        if WINDOWS:
            run("certutil", "-delstore", "Root", thumbprint, **quiet)
        else:
            run("sudo", "security", "remove-trusted-cert", "-d", ca, **quiet)
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


def scenario(executable):
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
            identity = json.dumps(["workspace-session-v1", origin, PROJECT, USER], separators=(",", ":"))
            record = hashlib.sha256(identity.encode()).hexdigest()
            check(not stored(record), "The new account already has an OS store entry")
            thread = threading.Thread(target=fixture.serve_forever, daemon=True)
            thread.start()
            try:
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
                                drive(app.pid, "update", origin)
                                print("The installed application reported its unsupported update channel in App version.", flush=True)
                                drive(app.pid, "logout", origin)
                                wait_for(lambda: not stored(record), "Signing out did not remove the OS credential")
                                print("The restarted package restored the rotated session, then sign-out removed the OS credential.", flush=True)
                            drive(app.pid, "quit", origin)
                            check(app.wait(timeout=20) == 0, "The package did not quit normally")
                        except Exception:
                            print(f"Observed login/refresh/introspection counts: {fixture.login_count}/{fixture.refresh_count}/{fixture.introspect_count}", flush=True)
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
    scenario(parser.parse_args().executable)
