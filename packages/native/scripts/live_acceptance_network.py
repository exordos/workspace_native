"""Loopback, single-destination CONNECT tunnel. Workspace TLS stays end-to-end."""

import asyncio
import contextlib
import threading
from urllib.parse import urlsplit


class NetworkConditions:
    def __init__(self, origin):
        parsed = urlsplit(origin)
        self.host, self.port = parsed.hostname, parsed.port or 443
        self.authority = (
            (f"[{self.host}]" if ":" in self.host else self.host) + ":" + str(self.port)
        )
        self.mode = None
        self.connections = set()
        self.matched = 0
        self.effects = 0
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
                self.effects += 1
                return
            source, upstream = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), 8
            )
            self.matched += 1
            writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            await writer.drain()

            async def copy(src, dst, down):
                while chunk := await src.read(65536):
                    if self.mode == "disconnect":
                        self.effects += 1
                        return
                    if down and self.mode == "delay":
                        self.effects += 1
                        await asyncio.sleep(0.75)
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
            if mode == "disconnect":
                for writer in self.connections:
                    writer.close()
                self.effects += len(self.connections)
            applied.set()

        self.loop.call_soon_threadsafe(enable)
        if not applied.wait(2):
            raise RuntimeError("fault-control-timeout")
        try:
            yield
        finally:
            self.loop.call_soon_threadsafe(setattr, self, "mode", None)

    def __exit__(self, *_):
        async def close():
            self.server.close()
            await self.server.wait_closed()
            for writer in tuple(self.connections):
                writer.close()
            current = asyncio.current_task()
            tasks = [t for t in asyncio.all_tasks() if t is not current]
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

        asyncio.run_coroutine_threadsafe(close(), self.loop).result(5)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        self.loop.close()
