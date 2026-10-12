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
- `test/`: tests over `linktest.gba`, a tiny MIT-licensed ROM with the
  same link pattern as pokeemerald (`test/rom/`).
- `src/platform/test/netplay-main.c`: `mgba-netplay-runner`, which runs a
  game through a session with no network or GUI.

Not yet built: the network layer, the Qt lobby/UI, and save write-back.
See the plan in the original planning session.

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

## Building

Same as mGBA (see the main README). The runner and tests need
`-DBUILD_PERF=ON -DBUILD_SUITE=ON`; tests need cmocka. On Windows use the
MSYS2 MinGW 64-bit shell; every player must run the same build, because
different compilers are the most likely source of desyncs.
