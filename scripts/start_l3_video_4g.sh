#!/usr/bin/env bash
# Run one bounded L3 visible-light push over the Manifold 4G/Tailscale path.
# Start the onboard publisher first so Tailscale Serve has a live backend before
# the local receiver connects (avoids Serve rejecting a client that connects
# while 127.0.0.1:9102 is still down).
set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PY="$REPO_DIR/.venv/bin/python"
SSH="ssh -i /home/mrjq311/.ssh/id_ed25519_l3 -o BatchMode=yes l3-manifold"

seconds=60
host=100.119.255.52
socks5=127.0.0.1:1055
point_port=9101
video_port=9102
mount=1
remote_bin=/open_app/workspaces/dji/l3-dev/video-20261117/build/l3_metadata_inspect
remote_config=/open_app/workspaces/dji/l3-dev/private/psdk.conf

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) seconds="$2"; shift 2 ;;
        --host) host="$2"; shift 2 ;;
        --socks5) socks5="$2"; shift 2 ;;
        --point-port) point_port="$2"; shift 2 ;;
        --video-port) video_port="$2"; shift 2 ;;
        --mount) mount="$2"; shift 2 ;;
        --remote-bin) remote_bin="$2"; shift 2 ;;
        --remote-config) remote_config="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$seconds" in
    ''|*[!0-9]*) echo "--seconds must be a positive integer" >&2; exit 2 ;;
esac

run_dir="/open_app/workspaces/dji/l3-dev/video-20261117/run-4g-$seconds"
stats_dir="$REPO_DIR/.validation/video-4g-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$stats_dir"
onboard_log="$stats_dir/onboard.log"
receiver_log="$stats_dir/receiver.log"

echo "event=test_begin seconds=$seconds host=$host socks5=$socks5 stats_dir=$stats_dir"

# 1. Start the onboard video-only publisher (no point recording).
$SSH "set -e; mkdir -p '$run_dir'; chmod 700 '$run_dir'; cd '$run_dir'; timeout --signal=TERM --kill-after=3s $((seconds + 30))s '$remote_bin' --config '$remote_config' --stream --streams video --mount $mount --seconds $seconds --bind 127.0.0.1 --point-port $point_port --video-port $video_port --exclusive-test" \
    >"$onboard_log" 2>&1 &
onboard_pid=$!

# 2. Wait until the onboard publisher is streaming, or it exits early.
deadline=$((SECONDS + 45))
while [ "$SECONDS" -lt "$deadline" ]; do
    if grep -q 'phase=video_hold' "$onboard_log" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$onboard_pid" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

# 3. Start the local receiver only after the onboard backend is live.
"$PY" "$REPO_DIR/pc_viewer/l3_stream.py" \
    --host "$host" --socks5 "$socks5" \
    --point-port "$point_port" --video-port "$video_port" \
    --streams video --seconds "$seconds" >"$receiver_log" 2>&1
receiver_rc=$?

wait "$onboard_pid"
onboard_rc=$?

# 4. Extract the facts worth keeping. Video bytes are H.264 payload bytes
#    received by the local decoder; frame count is decoded frames.
recv_bytes=$(grep -o '"event": "complete".*' "$receiver_log" | tail -1 | grep -o '"video_bytes": [0-9]*' | awk '{print $2}')
recv_frames=$(grep -o '"event": "complete".*' "$receiver_log" | tail -1 | grep -o '"video_frames": [0-9]*' | awk '{print $2}')
decode_errors=$(grep -o '"event": "complete".*' "$receiver_log" | tail -1 | grep -o '"decode_errors": [0-9]*' | awk '{print $2}')
conn_errors=$(grep -o '"event": "complete".*' "$receiver_log" | tail -1 | grep -o '"connection_errors": [0-9]*' | awk '{print $2}')
superseded=$(grep -o '"event": "complete".*' "$receiver_log" | tail -1 | grep -o '"video_superseded": [0-9]*' | awk '{print $2}')
send_bytes=$(grep 'phase=summary' "$onboard_log" | tail -1 | grep -o 'video_bytes=[0-9]*' | cut -d= -f2)
send_errors=$(grep 'phase=summary' "$onboard_log" | tail -1 | grep -o 'send_errors=[0-9]*' | cut -d= -f2)

echo "event=test_summary seconds=$seconds stats_dir=$stats_dir"
echo "event=onboard rc=$onboard_rc h264_bytes=$send_bytes send_errors=$send_errors"
echo "event=receiver rc=$receiver_rc h264_bytes=$recv_bytes decoded_frames=$recv_frames decode_errors=$decode_errors connection_errors=$conn_errors superseded=$superseded"

if [ -n "$recv_bytes" ] && [ "$recv_bytes" -gt 0 ]; then
    mbit_per_s=$(awk "BEGIN{printf \"%.3f\", ($recv_bytes * 8 / 1000000) / $seconds}")
    echo "event=throughput average_mbit_per_s=$mbit_per_s"
fi

exit $((receiver_rc != 0 || onboard_rc != 0))
