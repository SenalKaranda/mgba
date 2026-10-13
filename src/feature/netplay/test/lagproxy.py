#!/usr/bin/env python3
# Copyright (c) 2026 Senal Karanda
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""Puts a slow, lossy network between netplay clients and a host.

Clients connect to --listen instead of the host. TCP keeps its order but is
delayed; UDP is delayed, jittered (so it can arrive out of order) and dropped.
Delays apply in each direction, so the round trip is twice --delay. The
transport hello names the host's UDP port, so the proxy rewrites it to point
at itself.

    lagproxy.py --listen 47100 --target 127.0.0.1:47000 --delay 25 --jitter 6 --loss 2
"""
import argparse
import asyncio
import random
import struct
import sys

HELLO = struct.Struct("<I4sIH")  # length, "MMNT", token, UDP port


class Link:
    def __init__(self, args, rng):
        self.args = args
        self.rng = rng

    def delay(self):
        return (self.args.delay + self.rng.uniform(0, self.args.jitter)) / 1000

    def lost(self):
        return self.rng.uniform(0, 100) < self.args.loss


async def pump(link, reader, writer, patch_hello=None):
    """Copies a TCP stream in order, each chunk delivered after a delay."""
    loop = asyncio.get_running_loop()
    queue = asyncio.Queue()

    async def deliver():
        while True:
            at, data = await queue.get()
            if data is None:
                break
            await asyncio.sleep(max(0, at - loop.time()))
            writer.write(data)
            await writer.drain()
        writer.close()

    task = asyncio.create_task(deliver())
    pending = b""
    last = 0
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            if patch_hello:
                pending += data
                if len(pending) < HELLO.size:
                    continue
                size, magic, token, _ = HELLO.unpack_from(pending)
                if size != 10 or magic != b"MMNT":
                    sys.exit("lagproxy: unexpected transport hello")
                data = HELLO.pack(size, magic, token, patch_hello) + pending[HELLO.size:]
                patch_hello = None
            last = max(last, loop.time() + link.delay())
            queue.put_nowait((last, data))
    except ConnectionError:
        pass
    queue.put_nowait((last, None))
    await task


class Upstream(asyncio.DatagramProtocol):
    """One client's UDP traffic towards the host."""

    def __init__(self, proxy, client):
        self.proxy = proxy
        self.client = client
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.proxy.send_later(self.proxy.transport, data, self.client)

    def error_received(self, exc):
        pass


class UdpProxy(asyncio.DatagramProtocol):
    def __init__(self, link, target):
        self.link = link
        self.target = target
        self.transport = None
        self.upstreams = {}

    def connection_made(self, transport):
        self.transport = transport

    def send_later(self, transport, data, addr):
        if self.link.lost():
            return
        loop = asyncio.get_running_loop()
        loop.call_later(self.link.delay(), lambda: transport.sendto(data, addr))

    def datagram_received(self, data, addr):
        upstream = self.upstreams.get(addr)
        if upstream is None:
            upstream = Upstream(self, addr)
            self.upstreams[addr] = upstream
            loop = asyncio.get_running_loop()
            loop.create_task(loop.create_datagram_endpoint(lambda: upstream, remote_addr=self.target))
        if upstream.transport:
            self.send_later(upstream.transport, data, None)

    def error_received(self, exc):
        pass


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--listen", type=int, required=True, help="port for clients (TCP and UDP)")
    parser.add_argument("--target", required=True, help="the host, as HOST:PORT")
    parser.add_argument("--delay", type=float, default=25, help="one-way delay in ms")
    parser.add_argument("--jitter", type=float, default=0, help="extra random one-way delay in ms")
    parser.add_argument("--loss", type=float, default=0, help="UDP loss in percent, each way")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    host, port = args.target.rsplit(":", 1)
    target = (host, int(port))
    link = Link(args, random.Random(args.seed))

    async def accept(client_reader, client_writer):
        try:
            host_reader, host_writer = await asyncio.open_connection(*target)
        except OSError:
            client_writer.close()
            return
        await asyncio.gather(
            pump(link, client_reader, host_writer),
            pump(link, host_reader, client_writer, patch_hello=args.listen))

    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: UdpProxy(link, target), local_addr=("127.0.0.1", args.listen))
    server = await asyncio.start_server(accept, "127.0.0.1", args.listen)
    print(f"lagproxy: 127.0.0.1:{args.listen} -> {args.target}, {args.delay} ms +{args.jitter} ms, "
          f"{args.loss}% UDP loss", file=sys.stderr, flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
