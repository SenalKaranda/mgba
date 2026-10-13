# Handoff: internet netplay for Pokémon Quetzal (mGBA fork)

For the next agent picking this up. Read this, then `src/feature/netplay/README.md`.

## The user and the goal

- Wants to play **Pokémon Quetzal** (Emerald-based hack, 2–4 player link co-op, trades, battles)
  with 3–4 friends over the internet, **everyone on Windows**.
- VBA-M's network link crawls (one round trip per SIO transfer, ~9 transfers/frame). The fix
  chosen and built here: **input-synchronized netplay** in an mGBA fork. Every PC emulates *all*
  linked GBAs in one process; only button presses cross the network.
- User preferences: be a blunt, critical sparring partner ("ruthless mentor"), no glazing; say when
  unsure; verify with web research and cite sources.
- The user confirmed Quetzal's link works in **stock mGBA multiplayer windows** (same lockstep
  code this uses). Still **unmeasured**: the fps of 4 Quetzal GBAs at once on their weakest PC.
- The user got frustrated that they "assumed you were fixing the netcode this whole time": be
  explicit about what is *playable* vs. infrastructure, every time.

## Repo, branch, conventions

- Fork: `SenalKaranda/mgba` (private), branch **`senal/charming-ramanujan-yliica`**. Push only
  there. No PRs unless asked. Upstream base `143adf5`.
- Commit style: mGBA prefixes (`Feature:`, `Qt:`, `Test:`, `CI:`, `GBA SIO:`), title ≤ ~50 chars,
  prose body wrapped at 72, then these trailers:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01ERGZMGbGzJwhY5duz4w9KH
  ```
  No model names elsewhere in commits/code.
- mGBA's CONTRIBUTING forbids AI-generated code, so this stays a private fork. MPL-2.0: modified
  files stay MPL; sharing the binary means offering the source.

## Where things are

| Path | What |
|---|---|
| `src/feature/netplay/session.c`, `include/mgba/feature/netplay.h` | Deterministic session: 2–4 GBA cores stepped on one thread through the lockstep link; pins (HLE BIOS, idle loop ignore, fake-epoch RTC, TZ=UTC0, fresh config); state hashes; snapshots |
| `src/feature/netplay/peer.c`, `input.c`, `protocol.c`, `include/mgba/feature/netplay-net.h` | Host/client protocol (star via host), key exchange over UDP with acks, hash checks every 60 frames + snapshot resync, input delay 2–12 frames from lobby RTT |
| `src/feature/netplay/transport-socket.c` / `transport-sim.c` | Real TCP+UDP on one port (default 5738, IPv4 only) / simulated network for tests |
| `src/platform/qt/NetplayController.*`, `NetplayView.*` | **The playable app**: File → Netplay… host/join dialog; core-thread driver; save backup + write-back of the local player's `.sav` only |
| `src/platform/qt/CoreController.*` | "Netplay mode": non-owning core, no foreign config, keys go to the session, refuses every desyncing operation |
| `src/platform/qt/Window.*`, `ShortcutController.cpp`, `ConfigController.cpp` | Menu action, action lock during netplay, `--netplay-host PORT [--netplay-players N]`, `--netplay-join HOST[:PORT]` |
| `src/platform/test/netplay-main.c` | `mgba-netplay-runner`: headless runner (offline, `--host`, `--connect`, `--delay`) |
| `src/feature/netplay/test/` | cmocka tests (`session.c`, `peer.c`), `linktest.gba` (+ source in `rom/`), `localhost-game.sh`, `qt-game.sh`, `lagproxy.py` |
| `.github/workflows/netplay.yml` | Jobs: `linux`, `windows` (runner, tests, Valgrind, localhost games), `linux-qt`, `windows-qt` (Qt app + Qt↔runner games + packaging), `compare` (Linux vs Windows hashes) |
| `/root/.claude/plans/i-want-to-fork-valiant-planet.md` | Full plan and history (only in this container) |

## Current state

- **Playable Windows build exists**: CI run #6 (`38337046643`) artifact **`mgba-netplay-windows`**
  (39.7 MB: `mGBA.exe`, `mgba-netplay-runner.exe`, Qt plugins, DLLs, README). Tested in CI against
  the runner both ways on Qt's offscreen platform. The repo is private, so friends can't download
  artifacts; the user must share the zip. Artifacts expire after ~90 days; a GitHub Release would
  give a stable link (offered, not done).
- Run #6 is marked failed only because the old `windows` job's lag-proxy game ended with "Lost the
  connection to the host" at the last frame. Diagnosis: the host closed its socket with unread data
  → RST → the client lost the host's final BYE. **Fixed in `a0c1551`** (graceful close in
  `transport-socket.c _destroy`: shutdown(SEND) then drain until EOF or 500 ms; a client that has
  played every frame ends normally if the host vanishes; new sim test
  `clientAtEndSurvivesLosingHost`). **Pushed, CI not yet checked: verify run #7+ is fully green**
  (all five jobs) and that `mgba-netplay-windows` is produced again.
- All 25 local tests pass (Linux shared + static builds); localhost games through `lagproxy.py`
  (50 ms, 2% loss, 4 players) are clean.

## Verified vs. not verified

Verified (Linux, test ROM): Qt client and Qt host each play full games against the runner with
0 desyncs/stalls; picture renders (screenshot); game end handled; existing `.sav` untouched and
backed up; volume/renderer changes do not change hashes; Windows Qt build packages and starts.

**Not verified**, and the honest open risks:
1. Anyone running the Windows GUI (I never saw it on Windows).
2. Quetzal itself under the session (only `linktest.gba` so far), and 4×Quetzal speed.
3. Real internet / real NAT / Windows Firewall (only localhost + a userspace lag proxy; the
   container kernel has no netem).
4. The window's action lock against keyboard shortcuts: automated keypresses (xdotool, even under
   openbox) never reached mGBA's shortcuts here, so it is code-reviewed only. The real backstop is
   `CoreController` netplay mode refusing those operations.
5. Audio: frames are paced by the wall clock (16742706 ns/frame) with audio/video sync off, so
   occasional crackle is possible; the fix if it happens is dynamic rate control in
   `NetplayController::stepOnCoreThread`.
6. In-game saves mid-session go through the `savedataUpdated` callback, which is untested (the test
   ROM never saves); the end-of-session write is tested.

## Next steps (suggested order)

1. Check CI for `a0c1551` (and the next commit); fix anything red.
2. Tell the user, plainly: where the download is, how to share it, how to play (README "Playing
   with friends"), and the list above.
3. Wait for the user's real-world results (two `mGBA.exe` on one PC via `127.0.0.1` with Quetzal →
   one friend over Tailscale → four). Expect follow-ups on: Quetzal speed, audio, link behaviour in
   Quetzal's co-op, saves.
4. Optional: publish a Release; frame-advantage throttle; rejoining a running game; rollback.

## Local environment notes (this container; a fresh one needs setup)

- Build dirs: `/home/user/mgba-build` (shared, tests), `/home/user/mgba-build-static`,
  `/home/user/mgba-qt-build` (Qt, RelWithDebInfo), `/home/user/mgba-qt-static` (CI's Qt config).
- Packages used: `cmake ninja-build libcmocka-dev gcc-arm-none-eabi qt6-base-dev
  qt6-multimedia-dev libqt6opengl6-dev libsdl2-dev libepoxy-dev xvfb x11-utils xdotool openbox
  imagemagick`. Configure with `-DUSE_LIBZIP=OFF` (zipcmp missing), see `QT_CMAKE_FLAGS` in the
  workflow for the Qt build.
- Tests: `ctest -R netplay`; `sh src/feature/netplay/test/localhost-game.sh RUNNER ROM PLAYERS
  FRAMES PORT [--delay 25 --jitter 6 --loss 2]`; `sh src/feature/netplay/test/qt-game.sh MGBA-QT
  RUNNER ROM PORT [SECONDS]` (offscreen Qt, no display needed).
- Gotchas:
  - The runner and the Qt app must come from the **same build** (build ID = git commit + dirty
    state); otherwise the host rejects the join ("Different emulator build").
  - `pkill -f PATTERN` kills your own shell if the pattern is in the command line; use `pkill -x`.
  - Piping `xvfb-run` output hid the app's stderr; redirect to files.
  - Upstream bugs fixed on the way: Qt `CoreController::setLogger` cut all connections
    (`disconnect(nullptr)`); Qt CMake broke without git tags; lockstep restore queue never
    terminated.
  - Shallow clone: the remote-tracking ref had to be added (`git remote set-branches --add origin
    senal/charming-ramanujan-yliica`).
