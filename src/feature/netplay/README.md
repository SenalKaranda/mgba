# Netplay (work in progress)

Internet play for linked GBA games, aimed at Pokémon Quetzal's co-op.

## Why not just send link-cable traffic over the network?

A link transfer cannot finish until every other GBA has answered, and
pokeemerald-based games make nine transfers per frame. Sent over the
internet, each transfer costs a full round trip, so at 50 ms ping a game
crawls along at a few percent speed. That is what VBA-M's network link
does.

Instead, every player's PC emulates **all** the linked GBAs, connected
locally with no delay, and only button presses cross the network. Latency
then costs a few frames of input delay instead of a round trip per
transfer. This only works if every PC computes exactly the same state from
the same button presses, so determinism is what most of this code is
about.

## What exists

- `session.c`: a netplay session runs 2 to 4 GBA cores on one thread in
  a fixed order through mGBA's lockstep link, so link results depend only
  on emulated time and keys. It pins everything host-dependent (BIOS,
  idle-loop optimization, overrides, RTC, time zone), hashes state for
  desync checks, and snapshots/restores whole sessions for resyncing.
- `peer.c`: the network game. One player hosts and the others connect to
  the host (a star). The host checks everyone runs the same build and ROM,
  collects saves, picks the input delay from measured round trips (2 to 12
  frames) and starts everyone at once. Keys travel over UDP, repeated until
  acknowledged, so lost packets cost nothing but a resend; the host relays
  each player's keys to the others as soon as they arrive. Clients report
  state hashes every second, and a client that drifts out of sync gets a
  snapshot of the host's session. A player who drops ends the game for
  everyone (a linked GBA cannot just vanish).
- `transport-socket.c`: TCP for messages and UDP for keys, both on one
  port number (default 5738); `transport-sim.c`: a simulated network with
  latency, jitter and loss for the tests. IPv4 only.
- `test/`: tests over `linktest.gba`, a tiny MIT-licensed ROM with the
  same link pattern as pokeemerald (`test/rom/`), `localhost-game.sh`,
  which plays real runner processes against each other, and
  `lagproxy.py`, which puts a slow, lossy network between them.
- `src/platform/test/netplay-main.c`: `mgba-netplay-runner`, which runs a
  game through a session with no GUI, alone or over the network.

Not yet built: the Qt lobby/UI (so no picture, sound or controller yet)
and save write-back.

Two mGBA savestate gaps surfaced along the way and are handled here: the
link's outgoing word (SIOMLT_SEND) is never saved in savestates, and the
lockstep driver's restored event queue was never terminated (fixed in
`src/gba/sio/lockstep.c`).

## Checking Pokémon Quetzal on your PC

Download `mgba-netplay-runner-windows` from the latest run of the
"Netplay" workflow (GitHub → Actions), unzip it, and run from a command
prompt in that folder:

```
mgba-netplay-runner --players 4 --frames 36000 --random-keys 1 ^
    --check-snapshots 1800 --hash-every 600 quetzal.gba > hashes.txt
```

Add `--sav0 p1.sav --sav1 p2.sav ...` to start from real saves (copies are
read; nothing is written unless you pass `--write-saves DIR`).

What to look at:

- **The summary line** (printed at the end) gives frames per second for all
  four GBAs together. It must stay above 60, with headroom, on the slowest
  PC in your group. The test ROM runs at thousands of fps because it idles
  in Halt; Quetzal will be much slower, which is why you have to measure it.
- **`emulator warnings`** should be 0. `snapshot checks: N run, 0 failed`
  means desync recovery works for this game.
- **Determinism across PCs**: run the exact same command on a friend's PC
  and compare the two `hashes.txt` files; they must be identical.

Random keys will not get through Quetzal's menus into a link session. For
that, write a key script (`--input FILE`, lines of
`FRAME SLOT KEYS [DURATION]`, e.g. `1200 0 A 2`) and check the dumped
frames (`--dump DIR --dump-every 600`).

## Trying the network with friends (no GUI yet)

The runner can already play over the internet, pressing random keys, which
shows whether your connection keeps up and whether every PC computes the
same game. Everyone needs the same `mgba-netplay-runner-windows` download.

1. **Connect the PCs.** The host must be reachable by everyone. The easy
   way is [Tailscale](https://tailscale.com/download): everyone installs it
   and joins the host's tailnet (or the host shares their machine with
   them), then uses the host's Tailscale IPv4 address (`100.x.y.z`; IPv6
   addresses do not work yet). The alternative is forwarding TCP **and** UDP
   port 5738 on the host's router to the host's PC.
2. **Host** (choose `--players` to match):

   ```
   mgba-netplay-runner --host 5738 --players 4 --frames 7200 ^
       --random-keys 1 --hash-every 600 linktest.gba > hashes.txt
   ```

   When Windows asks whether to let it on the network, allow it.
   Tailscale normally marks its network as Private, so Private is enough;
   if nobody can connect, also allow Public under Windows Security →
   Firewall & network protection → Allow an app through firewall.
3. **Everyone else**, once the host is waiting:

   ```
   mgba-netplay-runner --connect 100.x.y.z --random-keys 2 ^
       --hash-every 600 linktest.gba > hashes.txt
   ```

The game starts when everyone has joined and runs for two minutes. Each
player's last line reads like:

```
Player 2: 7200 frames in 120.5 s (59.75 fps), input delay 5, round trip 55 ms, 3 stalls (40 ms), 0 desyncs, 0 resyncs
```

- **fps** should be 59.7 (the GBA's real rate) for everyone.
- **input delay** is how many frames a button press waits before it
  counts (about 17 ms each); it grows with the slowest round trip to the
  host. 5 or less feels fine in an RPG; 12 is the limit.
- **stalls** are moments a PC had to wait for someone's keys. A few short
  ones are normal; many, or long ones, mean a poor connection (Wi-Fi is
  the usual suspect).
- **desyncs** must be 0, and every player's `hashes.txt` must be identical.
  Anything else is a bug; please keep the files.

Replace `linktest.gba` with your copy of Quetzal to check that four GBAs
of it run at full speed on everyone's PC (random keys won't get it into a
link session, but every PC must still compute the same game).

## Building

Same as mGBA (see the main README). The runner and tests need
`-DBUILD_PERF=ON -DBUILD_SUITE=ON`; tests need cmocka. On Windows use the
MSYS2 MinGW 64-bit shell; every player must run the same build, because
different compilers are the most likely source of desyncs.
