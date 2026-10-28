#!/usr/bin/env bash
# Capture the loopback test traffic and summarize TCP loss indicators.
# Run as root (or with sudo) because packet capture needs CAP_NET_RAW.
#
# CHAT_LOSS_PERCENT=5 emulates 5% packet loss on loopback with tc/netem.
# This is useful for observing TCP retransmission; the application-level
# sequence check should still report no missing chat messages.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${CHAT_TEST_PORT:-19002}"
LOSS="${CHAT_LOSS_PERCENT:-0}"
PCAP="${CHAT_PCAP:-$ROOT/build/chat-test.pcap}"
LOG="${PCAP%.pcap}.tshark.txt"
TCPDUMP_PID=""
LOSS_ACTIVE=0

if [[ "$(id -u)" -ne 0 ]]; then
    echo "run this script as root (packet capture requires CAP_NET_RAW)" >&2
    exit 1
fi

cleanup() {
    if [[ -n "$TCPDUMP_PID" ]]; then
        kill -INT "$TCPDUMP_PID" 2>/dev/null || true
        wait "$TCPDUMP_PID" 2>/dev/null || true
        TCPDUMP_PID=""
    fi
    if [[ "$LOSS_ACTIVE" -eq 1 ]]; then
        tc qdisc del dev lo root 2>/dev/null || true
        LOSS_ACTIVE=0
    fi
}
trap cleanup EXIT INT TERM

cd "$ROOT"
mkdir -p build
rm -f "$PCAP" "$LOG"

if [[ "$LOSS" != "0" ]]; then
    echo "adding ${LOSS}% loss on loopback for the test..."
    tc qdisc add dev lo root netem loss "${LOSS}%"
    LOSS_ACTIVE=1
fi

tcpdump -i lo -U -w "$PCAP" "tcp port $PORT" >/tmp/chat-tcpdump.log 2>&1 &
TCPDUMP_PID=$!
sleep 1

set +e
CHAT_TEST_PORT="$PORT" python3 tests/integration_test.py
TEST_RC=$?
set -e

sleep 0.5
cleanup

{
    echo "=== capture ==="
    echo "emulated loss on lo: ${LOSS}%"
    ls -lh "$PCAP"
    echo
    echo "=== TCP conversations ==="
    tshark -r "$PCAP" -q -z conv,tcp 2>/dev/null
    echo
    echo "=== possible packet loss / retransmission / reset ==="
    tshark -r "$PCAP" \
        -Y "tcp.analysis.lost_segment || tcp.analysis.retransmission || tcp.flags.reset == 1" \
        -T fields -e frame.number -e _ws.col.Info 2>/dev/null
    COUNT=$(tshark -r "$PCAP" \
        -Y "tcp.analysis.lost_segment || tcp.analysis.retransmission || tcp.flags.reset == 1" \
        2>/dev/null | wc -l)
    echo "count: $COUNT"
    echo
    echo "=== packet totals ==="
    tshark -r "$PCAP" -q -z io,stat,0 2>/dev/null
} >"$LOG" 2>&1

cat "$LOG"
echo "pcap: $PCAP"
echo "summary: $LOG"
exit "$TEST_RC"
