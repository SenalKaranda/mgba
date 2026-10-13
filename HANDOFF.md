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
  (Commits made from Cursor on the user's PC have no session URL, so only the first trailer;
  author `Claude <noreply@anthropic.com>` via `--author`.) No model names elsewhere in
  commits/code.
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

- **Playable Windows build**: the newest green "Netplay" run's artifact **`mgba-netplay-windows`**
  (~40 MB: `mGBA.exe`, `mgba-netplay-runner.exe`, Qt 6.11.2 plugins, DLLs, README). Last checked:
  run `38349913541` for `aca5739`, all five jobs green. The repo is private, so friends can't
  download artifacts; the user must share the zip. Artifacts expire after ~90 days; a GitHub
  Release would give a stable link (offered, not done).
- `a0c1551` (graceful socket close) is confirmed: run `38338811488`, all five jobs green.
- `2a0563a` and `aca5739` fix an **upstream** Qt bug (not netplay's): with Qt 6.11 on Windows the
  main window opened at its 240x160 minimum (menus hidden, picture squashed) on every launch, and
  a window closed maximized came back tiny. Qt's window log showed `showEvent()`'s resize and
  maximize applied, then undone when the window became visible (around `reloadDisplayDriver()`
  creating the GL display). Fix: size and maximize again from a `QTimer::singleShot(0)`. Upstream
  `master` was still `143adf5` on 2026-10-12, so upstream has not fixed it. Full screen on start
  is untested and may have the same problem.
- All 25 local tests passed in the Linux container; localhost games through `lagproxy.py` (50 ms,
  2% loss, 4 players) were clean.

## Verified vs. not verified

Verified (Linux container, test ROM): Qt client and Qt host each play full games against the
runner with 0 desyncs/stalls; existing `.sav` untouched and backed up; volume/renderer changes do
not change hashes.

Verified **on the user's Windows 11 PC** (build 26200, real display and GPU, the CI package,
`linktest.gba`, localhost):
- `mGBA.exe` joins a runner host (900 frames, 0 stalls, 0 desyncs) and hosts a runner (0 desyncs);
  closing the host's window ends the game for the others; the dialog shows "The game is over".
- `mGBA.exe` hosts a 4-player game with three runners: 0 desyncs, 0 resyncs. The later clients
  see a one-off 150-300 ms stall (same totals over 20 s and 60 s, so at the start or end, not
  during play); an all-runner 4-player game has none. Harmless, cause not pinned down.
- The lobby lists the host's addresses with the Tailscale one labelled.
- Windows Firewall asks the first time each exe hosts (once per folder); allowing works.
- Window size: fresh, saved and maximized cases now match stock mGBA 0.10.5.

**Not verified**, and the honest open risks:
1. A person playing on the Windows GUI: keyboard/controller input, audio, feel.
2. Quetzal itself under the session (only `linktest.gba` so far), and 4×Quetzal speed. There is
   no `.gba` under the user's profile or on `S:\` except the test ROM; ask where it is.
3. Real internet / real NAT (only localhost + a userspace lag proxy so far).
4. The window's action lock against keyboard shortcuts: automated keypresses (xdotool, even under
   openbox) never reached mGBA's shortcuts here, so it is code-reviewed only. The real backstop is
   `CoreController` netplay mode refusing those operations.
5. Audio: frames are paced by the wall clock (16742706 ns/frame) with audio/video sync off, so
   occasional crackle is possible; the fix if it happens is dynamic rate control in
   `NetplayController::stepOnCoreThread`.
6. In-game saves mid-session go through the `savedataUpdated` callback, which is untested (the test
   ROM never saves); the end-of-session write is tested.

## Next steps (suggested order)

1. Check CI for the newest commit; fix anything red.
2. Get the Quetzal ROM's location from the user and run the README's speed and determinism check
   (`mgba-netplay-runner --players 4 ... quetzal.gba`) on this PC.
3. Wait for the user's real-world results (two `mGBA.exe` on one PC via `127.0.0.1` with Quetzal →
   one friend over Tailscale → four). Expect follow-ups on: Quetzal speed, audio, link behaviour in
   Quetzal's co-op, saves.
4. Optional: publish a Release; frame-advantage throttle; rejoining a running game; rollback.

## The user's Windows PC (where the latest work happened)

- Repo at `S:\Projects\mgba`. The clone had HEAD on `master` with the branch's files copied over
  it; it is now on the branch tracking `origin`, with `core.autocrlf=false` set locally so the
  LF working tree matches the repo (the `.sh` scripts need LF).
- No Qt or MSYS2 here (only Strawberry Perl's MinGW gcc, CMake, Ninja, Python, Tailscale), so
  Qt changes are built by CI and the artifact is tested locally. `gh` is not logged in; with the
  user's approval, `GH_TOKEN` was taken from git's credential store for the session.
- Test scripts live outside the repo in `S:\Projects\netplay-test` (PowerShell): `helpers.ps1`
  (launch, close, screenshot windows with `PrintWindow`), `gui-game.ps1 -Build DIR` (`qt-game.sh`
  on the real display, with screenshots), `four-player.ps1 -Build DIR [-RunnerHost] [-NoSnap]`,
  `size-test.ps1 -Build DIR`, `resize-test.ps1`. Each build folder (`app`, `app2`, `app3`, one per
  CI run) has a `portable.ini`, so the stock mGBA settings in `%APPDATA%\mGBA` stay untouched.
- Stock mGBA 0.10.5 is installed at `S:\Program Files\mGBA`; a portable copy for comparisons is
  `S:\Projects\netplay-test\stock`.
- Qt's own messages are rerouted into mGBA's log. To see them, put `logToStdout=1` and
  `logLevel=127` under `[ports.qt]` in the build's `config.ini`, and set e.g.
  `QT_LOGGING_RULES=qt.qpa.window=true` (native window geometry and state).
- Running the packaged runner from outside the repo needs the user's approval each session.

## Local environment notes (the earlier Linux container; a fresh one needs setup)

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
