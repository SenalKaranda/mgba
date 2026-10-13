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

## Playing with friends

1. **Everyone gets the same download.** On GitHub, open Actions, the newest
   green "Netplay" run, and download `mgba-netplay-windows` from its
   artifacts. Unzip it anywhere and run `mGBA.exe`. Everyone must use that
   very same download: the host turns away any other build.
2. **Everyone has the same game.** Each player needs their own copy of the
   exact same ROM (the same patched Quetzal, byte for byte) and keeps their
   own save next to it as usual (`Quetzal.gba` and `Quetzal.sav`), or in
   mGBA's save folder if they set one.
3. **Connect the PCs.** See "Connecting the PCs" below (Tailscale is the
   easy way).
4. **The host** opens File → Netplay..., picks the ROM, chooses "Host a
   game", sets the number of players (2 to 4) and presses Host. The dialog
   lists the host's addresses; send friends the Tailscale one.
5. **Everyone else** opens File → Netplay..., picks the ROM, types the
   host's address under "Join a game" and presses Join.
6. The game starts on every PC once everyone has joined. The title bar
   shows your player number and the round trip to the others.

While playing:

- Your keyboard or controller works as set up in mGBA's settings.
- Everything that would make your console differ from the others' is off:
  save states, rewind, fast forward, pause, reset, cheats, loading another
  game, and the debugging tools. Volume and video settings work as usual.
- **Input delay:** button presses take effect a few frames late (about
  17 ms each), the same for everyone, chosen from the slowest connection
  when the game starts. Nearby friends get 2 to 5 frames.
- **"Waiting for player N..."** means that player's keys are late (a poor
  connection, or a PC that cannot keep up). After 20 seconds of silence the
  game ends for everyone.
- **If anyone leaves or drops, the game ends for everyone**: a linked GBA
  cannot be unplugged mid-game. Progress since the last in-game save is lost
  then, so **save in the game often**. Start a new session the same way.

Saves:

- Every PC emulates every player's GBA, so each player's save is sent to
  everyone, but each PC writes back **only its own player's save**, to the
  usual place, whenever the game saves and when the session ends.
- The first time a session writes your save, it first copies the old one to
  `NAME.sav.netplay-DATE-TIME.bak` beside it. Delete old backups whenever
  you like. The save keeps working in plain mGBA.

Every PC runs all the players' GBAs at once, so it needs several times the
power of normal play; see "Checking Pokémon Quetzal on your PC" below for
a quick speed test. If one PC cannot keep up, everyone else sees "Waiting
for player N..." now and then; play with fewer players.

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
- `src/platform/qt/NetplayController.cpp`, `NetplayView.cpp`: the app's
  File → Netplay... dialog. In the lobby the peer is polled from the GUI
  thread; once the game starts, the local player's console runs in an
  ordinary `CoreController` (in its netplay mode, which refuses anything
  that would desync, such as loading a state) whose core thread drives the
  whole session. `test/qt-game.sh` plays the app against the runner, both
  ways round, on Qt's offscreen platform.

Not built yet: rejoining a running game, rollback (hiding the input delay),
IPv6.

Two mGBA savestate gaps surfaced along the way and are handled here: the
link's outgoing word (SIOMLT_SEND) is never saved in savestates, and the
lockstep driver's restored event queue was never terminated (fixed in
`src/gba/sio/lockstep.c`).

## Checking Pokémon Quetzal on your PC

In the `mgba-netplay-windows` download (see above) is also
`mgba-netplay-runner.exe`, which runs the linked GBAs with no window. Run it
from a command prompt in that folder:

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

## Connecting the PCs, and testing the connection

The runner (in the same download) can play over the internet without the
app, pressing random keys, which shows whether your connection keeps up and
whether every PC computes the same game.

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
