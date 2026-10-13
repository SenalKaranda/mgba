#!/bin/sh
# Copyright (c) 2026 Senal Karanda
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Plays a netplay game between runner processes on this machine, over real
# sockets, and fails unless every player ends with no desyncs and the same
# hashes as one process running the whole game with the same keys.
#
#   localhost-game.sh RUNNER ROM PLAYERS FRAMES PORT [LAGPROXY OPTIONS...]
#
# With lagproxy.py options (say --delay 25 --jitter 6 --loss 2), clients
# connect through the proxy on PORT+1, which needs python3.
set -u

if [ $# -lt 5 ]; then
	echo "usage: $0 RUNNER ROM PLAYERS FRAMES PORT [LAGPROXY OPTIONS...]" >&2
	exit 2
fi
runner=$1
rom=$2
players=$3
frames=$4
port=$5
shift 5
out=$(mktemp -d)
pids=
proxy=

cleanup() {
	for pid in $pids $proxy; do
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

address=127.0.0.1:$port
if [ $# -gt 0 ]; then
	address=127.0.0.1:$((port + 1))
	python3 "$(dirname "$0")/lagproxy.py" --listen $((port + 1)) --target 127.0.0.1:$port "$@" 2> "$out/proxy.err" &
	proxy=$!
	await "$out/proxy.err" lagproxy:
fi

common="--random-keys 7 --hash-every 300"
"$runner" --host "$port" --players "$players" --frames "$frames" $common "$rom" > "$out/p1.txt" 2> "$out/p1.err" &
pids=$!
# Clients that knock before the host listens are turned away.
await "$out/p1.err" Hosting
i=2
while [ $i -le "$players" ]; do
	"$runner" --connect "$address" $common "$rom" > "$out/p$i.txt" 2> "$out/p$i.err" &
	pids="$pids $!"
	i=$((i + 1))
done

status=0
for pid in $pids; do
	wait "$pid" || status=1
done
pids=

# Clients get their player numbers in the order they connect.
i=1
while [ $i -le "$players" ]; do
	echo "--- process $i"
	cat "$out/p$i.err"
	if ! grep -q "^Player [0-9]: $frames frames.* 0 desyncs" "$out/p$i.err"; then
		echo "process $i did not finish cleanly"
		status=1
	fi
	if ! cmp -s "$out/p1.txt" "$out/p$i.txt"; then
		echo "process $i disagrees with the host:"
		diff "$out/p1.txt" "$out/p$i.txt"
		status=1
	fi
	i=$((i + 1))
done
# The same keys in one process, with none for the frames the input delay
# kept empty, must give the same game.
delay=$(sed -n 's/.*input delay of \([0-9]*\) frames.*/\1/p' "$out/p1.err")
if [ -z "$delay" ]; then
	status=1
elif ! "$runner" --players "$players" --frames "$frames" --delay "$delay" $common "$rom" > "$out/alone.txt" 2> "$out/alone.err"; then
	echo "the game alone failed:"
	cat "$out/alone.err"
	status=1
elif ! cmp -s "$out/p1.txt" "$out/alone.txt"; then
	echo "the network game differs from the same game in one process:"
	diff "$out/alone.txt" "$out/p1.txt"
	status=1
fi
if [ $status = 0 ]; then
	echo "--- all $players players agree with the game in one process (input delay $delay):"
	cat "$out/p1.txt"
fi
exit $status
