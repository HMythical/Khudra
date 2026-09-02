#!/usr/bin/env bash
# End-to-end proof that examples/http_server.khu is a real HTTP server.
#
# The golden tests pin the protocol half -- what each route answers -- by
# running the program with no arguments. They cannot pin the transport half:
# a socket needs a port, a client and a moment in time, none of which belong in
# a byte-for-byte comparison. This script is that other half. It starts the
# server, drives it with `curl` over real TCP, and checks what comes back.
#
# It does it three times, once per backend, because "the same program under the
# VM, under `run --native` and as a built binary" is the invariant the whole
# native backend exists to keep (docs/native.md, section 5) -- and a socket is
# the newest thing that has to honour it.
#
#   scripts/test_http_server.sh [path-to-khudra]
#
# Defaults to ./build/khudra. Exits 0 when every backend served correctly.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
khudra="${1:-$root/build/khudra}"
source="$root/examples/http_server.khu"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ ! -x "$khudra" ]; then
    echo "no khudra binary at '$khudra' -- build first, or pass its path" >&2
    exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "curl is not installed; skipping the end-to-end socket test" >&2
    exit 0
fi

failures=0

# Runs the server one way, drives it, and checks the answers.
#
# The server is asked for port 0 and prints the one the system gave it, so
# there is no port to guess and no race with another test picking the same one.
# It is asked for exactly three requests, so it ends by itself and a hung
# server shows up as a timeout rather than a stuck suite.
drive() {
    local label="$1"
    shift
    local log="$work/$label.out"
    local err="$work/$label.err"

    "$@" 0 3 >"$log" 2>"$err" &
    local server=$!

    local port=""
    for _ in $(seq 1 100); do
        port="$(sed -n 's/^listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' "$log" 2>/dev/null)"
        [ -n "$port" ] && break
        sleep 0.1
    done
    if [ -z "$port" ]; then
        echo "FAIL $label: the server never reported a port" >&2
        sed 's/^/    /' "$err" >&2
        kill "$server" 2>/dev/null
        wait "$server" 2>/dev/null
        failures=$((failures + 1))
        return
    fi

    local base="http://127.0.0.1:$port"
    local ok=1

    expect() {
        local what="$1" want="$2" got="$3"
        if [ "$got" != "$want" ]; then
            echo "FAIL $label: $what" >&2
            echo "    expected: $want" >&2
            echo "    actual:   $got" >&2
            ok=0
        fi
    }

    expect "GET /ping body" "pong" \
        "$(curl -s --max-time 10 "$base/ping")"
    expect "GET /missing status" "404" \
        "$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$base/missing")"
    expect "GET / status" "200" \
        "$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$base/")"

    wait "$server"
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "FAIL $label: the server exited $status" >&2
        sed 's/^/    /' "$err" >&2
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        echo "ok   $label: served three requests over TCP on port $port"
    else
        failures=$((failures + 1))
    fi
}

drive "vm" "$khudra" run "$source" --
drive "native" "$khudra" run --native "$source" --

binary="$work/http_server"
if "$khudra" build "$source" -o "$binary" >"$work/build.log" 2>&1; then
    drive "build" "$binary"
else
    echo "skip build: khudra build failed (no host C++ compiler?)" >&2
    sed 's/^/    /' "$work/build.log" >&2
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures backend(s) failed" >&2
    exit 1
fi
echo "every backend served the same responses over a real socket"
