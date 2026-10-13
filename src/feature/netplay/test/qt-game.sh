#!/bin/sh
# Copyright (c) 2026 Senal Karanda
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Plays netplay games between the Qt frontend and the headless runner on
# this machine, with Qt on its offscreen platform (no display needed):
#   1. the runner hosts and Qt joins: the host checks the Qt player's state
#      every second, so the game must end with no desyncs, no stalls (Qt kept
#      up) and the Qt side must notice the end;
#   2. Qt hosts and the runner joins: the runner must play at full speed and
#      never be resynced (Qt's state matched).
#
#   qt-game.sh MGBA-QT RUNNER ROM PORT [SECONDS]
set -u

if [ $# -lt 4 ]; then
	echo "usage: $0 MGBA-QT RUNNER ROM PORT [SECONDS]" >&2
	exit 2
fi
qt=$1
runner=$2
rom=$3
port=$4
seconds=${5:-15}
frames=$((seconds * 60))
out=$(mktemp -d)
pids=

cleanup() {
	for pid in $pids; do
		kill "$pid" 2>/dev/null
	done
	rm -rf "$out"
}
trap cleanup EXIT

# Waits until FILE has a line starting with TEXT, for at most ten seconds.
await() {
	tries=0
	until grep -q "^$2" "$1" 2>/dev/null; do
		tries=$((tries + 1))
		if [ $tries -gt 100 ]; then
			echo "Timed out waiting for \"$2\" in $1:" >&2
			cat "$1" >&2
			exit 1
		fi
		sleep 0.1
	done
}

QT_QPA_PLATFORM=offscreen
SDL_AUDIODRIVER=dummy
XDG_CONFIG_HOME=$out/config
export QT_QPA_PLATFORM SDL_AUDIODRIVER XDG_CONFIG_HOME
status=0

echo "=== The runner hosts, Qt joins"
"$runner" --host "$port" --players 2 --frames "$frames" --random-keys 3 --hash-every 0 "$rom" > "$out/host.txt" 2> "$out/host.err" &
host=$!
pids=$host
await "$out/host.err" Hosting
"$qt" --netplay-join "127.0.0.1:$port" "$rom" > "$out/qt1.log" 2>&1 &
pids="$pids $!"
wait "$host" || status=1
cat "$out/host.err"
if ! grep -q "^Player 1: $frames frames.* 0 stalls (0 ms), 0 desyncs, 0 resyncs" "$out/host.err"; then
	echo "the game with Qt joining did not run cleanly"
	status=1
fi

echo "=== Qt hosts, the runner joins"
port=$((port + 1))
"$qt" --netplay-host "$port" --netplay-players 2 "$rom" > "$out/qt2.log" 2>&1 &
qthost=$!
pids="$pids $qthost"
# Qt takes a moment to open its port; a runner that finds it closed quits
# at once, so start another.
tries=0
while :; do
	"$runner" --connect "127.0.0.1:$port" --random-keys 5 --hash-every 0 "$rom" > "$out/client.txt" 2> "$out/client.err" &
	client=$!
	sleep 2
	if kill -0 "$client" 2>/dev/null; then
		break
	fi
	tries=$((tries + 1))
	if [ $tries -gt 10 ]; then
		echo "the runner could not join Qt's game:"
		cat "$out/client.err"
		exit 1
	fi
done
pids="$pids $client"
sleep "$seconds"
# Quitting Qt ends the game for the runner.
kill "$qthost"
wait "$client"
cat "$out/client.err"
played=$(sed -n 's/^Player 2: \([0-9]*\) frames.*/\1/p' "$out/client.err")
if [ -z "$played" ] || [ "$played" -lt $((frames * 8 / 10)) ]; then
	echo "the runner played ${played:-no} frames of about $frames"
	status=1
fi
if ! grep -q "^Player 2: .* 0 desyncs, 0 resyncs" "$out/client.err"; then
	echo "Qt's game and the runner's disagreed"
	status=1
fi
exit $status
