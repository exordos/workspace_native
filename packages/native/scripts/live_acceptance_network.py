"""Loopback, single-destination CONNECT tunnel. Workspace TLS stays end-to-end."""

import asyncio
import contextlib
import http.client
import json
import socket
import ssl
import threading
from urllib.parse import urlsplit


def probe_organization_settings(origin):
    """Check the runner's unauthenticated path independently of the Qt proxy."""
    parsed = urlsplit(origin)
    connection = http.client.HTTPSConnection(
        parsed.hostname, parsed.port or 443, timeout=8
    )
    try:
        connection.request("GET", "/api/workspace/v1/messenger/server_settings")
        response = connection.getresponse()
        if not 200 <= response.status < 300:
            return f"http-{response.status}"
        body = response.read(65537)
        if len(body) > 65536 or not isinstance(json.loads(body), dict):
            return "invalid-settings"
        return "ready"
    except socket.gaierror:
        return "dns"
    except ssl.SSLError:
        return "tls"
    except TimeoutError:
        return "timeout"
    except (OSError, http.client.HTTPException):
        return "connect"
    except (ValueError, RecursionError):
        return "invalid-settings"
    finally:
        connection.close()


class NetworkConditions:
    def __init__(self, origin):
        parsed = urlsplit(origin)
        self.host, self.port = parsed.hostname, parsed.port or 443
        self.authority = (
            (f"[{self.host}]" if ":" in self.host else self.host) + ":" + str(self.port)
        )
        self.mode = None
        self.fault_generation = 0
        self.connections = set()
        self.matched = 0
        self.effects = {"delay": 0, "disconnect": 0}
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        self.server = asyncio.run_coroutine_threadsafe(
            asyncio.start_server(self.accept, "127.0.0.1", 0, limit=4096), self.loop
        ).result(3)
        self.local_port = self.server.sockets[0].getsockname()[1]
        return self

    async def accept(self, reader, writer):
        upstream = None
        self.connections.add(writer)
        try:
            header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
            request = header.split(b"\r\n", 1)[0].decode("ascii").split()
            if (
                len(request) != 3
                or request[0] != "CONNECT"
                or request[1].lower() != self.authority.lower()
            ):
                writer.write(b"HTTP/1.1 403 Forbidden\r\n\r\n")
                await writer.drain()
                return
            if self.mode == "disconnect":
                # Keep the proxy operational and reset the upstream TLS stream.
                # Dropping CONNECT itself teaches Qt that the proxy is unavailable.
                writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
                await writer.drain()
                self.effects[self.mode] += 1
                return
            source, upstream = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), 8
            )
            self.matched += 1
            writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            await writer.drain()

            async def copy(src, dst, down):
                last_delay = 0
                while chunk := await src.read(65536):
                    if self.mode == "disconnect":
                        self.effects[self.mode] += 1
                        return
                    if (
                        down
                        and self.mode == "delay"
                        and last_delay != self.fault_generation
                    ):
                        # Delay the response stream once per fault window, not once per chunk.
                        last_delay = self.fault_generation
                        self.effects[self.mode] += 1
                        await asyncio.sleep(2)
                    dst.write(chunk)
                    await dst.drain()

            tasks = [
                asyncio.create_task(copy(reader, upstream, False)),
                asyncio.create_task(copy(source, writer, True)),
            ]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        except (
            OSError,
            ValueError,
            UnicodeError,
            asyncio.IncompleteReadError,
            asyncio.LimitOverrunError,
            TimeoutError,
        ):
            pass
        finally:
            self.connections.discard(writer)
            writer.close()
            if upstream:
                upstream.close()

    @contextlib.contextmanager
    def disturb(self, mode):
        applied = threading.Event()

        def enable():
            self.mode = mode
            self.fault_generation += 1
            if mode == "disconnect":
                for writer in self.connections:
                    writer.close()
                self.effects["disconnect"] += len(self.connections)
            applied.set()

        self.loop.call_soon_threadsafe(enable)
        if not applied.wait(2):
            raise RuntimeError("fault-control-timeout")
        try:
            yield
        finally:
            applied.clear()

            def restore():
                self.mode = None
                applied.set()

            self.loop.call_soon_threadsafe(restore)
            if not applied.wait(2):
                raise RuntimeError("fault-restore-timeout")

    def restored_tls(self):
        # Independent reachability check through exactly the same tunnel and TLS origin.
        connection = http.client.HTTPSConnection(
            "127.0.0.1",
            self.local_port,
            timeout=5,
            context=ssl.create_default_context(),
        )
        try:
            connection.set_tunnel(self.host, self.port)
            connection.connect()
            return True
        except (OSError, http.client.HTTPException):
            return False
        finally:
            connection.close()

    def __exit__(self, *_):
        async def close():
            self.server.close()
            for writer in tuple(self.connections):
                writer.close()
            current = asyncio.current_task()
            tasks = [t for t in asyncio.all_tasks() if t is not current]
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await self.server.wait_closed()

        asyncio.run_coroutine_threadsafe(close(), self.loop).result(5)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        self.loop.close()
