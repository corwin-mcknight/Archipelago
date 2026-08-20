#!/usr/bin/env python3
"""Serial console mux for the jh7110 board.

Holds the single TCP connection to the Mac's socat serial bridge and fans it
out to any number of local clients, so automation and an interactive console
can share the port without stealing bytes from each other.

  upstream: host.docker.internal:5555 (socat <-> /dev/cu.usbserial-*)
  clients:  connect to port 5556, e.g.  socat -,rawer,escape=0x1d TCP:localhost:5556

Run it in the background and leave it running; it reconnects upstream as the
bridge comes and goes.
"""

import asyncio
import sys

UPSTREAM = ("host.docker.internal", 5555)
LISTEN_PORT = 5556

clients: set[asyncio.StreamWriter] = set()
upstream_writer: asyncio.StreamWriter | None = None


async def upstream_loop() -> None:
    global upstream_writer
    while True:
        try:
            reader, writer = await asyncio.open_connection(*UPSTREAM)
        except OSError:
            await asyncio.sleep(2)
            continue
        upstream_writer = writer
        print(f"[mux] upstream connected to {UPSTREAM[0]}:{UPSTREAM[1]}", file=sys.stderr)
        try:
            while data := await reader.read(4096):
                for client in list(clients):
                    try:
                        client.write(data)
                    except ConnectionError:
                        clients.discard(client)
        except OSError:
            pass
        upstream_writer = None
        writer.close()
        print("[mux] upstream lost; reconnecting", file=sys.stderr)


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    clients.add(writer)
    try:
        while data := await reader.read(4096):
            if upstream_writer is not None:
                upstream_writer.write(data)
    except OSError:
        pass
    finally:
        clients.discard(writer)
        writer.close()


async def main() -> None:
    server = await asyncio.start_server(handle_client, "0.0.0.0", LISTEN_PORT)
    print(f"[mux] clients: port {LISTEN_PORT}", file=sys.stderr)
    async with server:
        await asyncio.gather(server.serve_forever(), upstream_loop())


if __name__ == "__main__":
    asyncio.run(main())
