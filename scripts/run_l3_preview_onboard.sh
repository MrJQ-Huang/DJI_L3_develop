#!/usr/bin/env bash
# Run on Manifold 3: PSDK DEFAULT -> local FFmpeg -> bounded H.264 TCP relay.
# No media files, source writes, MOP subscriptions or recording commands.
# --expected-source checks the current source; it never changes the source.
set -euo pipefail
umask 077
bin_dir=
config=
seconds=60
mount=1
video_port=9102
raw_port=19102
width=960
fps=10
bitrate=800
expected_source=
while (($#)); do
    case "$1" in
        --help|-h)
            echo 'Usage: run_l3_preview_onboard.sh --bin-dir DIR --config PRIVATE_FILE [--seconds 60] [--mount 1] [--expected-source ID] [--video-port 9102] [--raw-port 19102] [--width 960] [--fps 10] [--bitrate-kbps 800]'
            exit 0 ;;
        --bin-dir|--config|--seconds|--mount|--expected-source|--video-port|--raw-port|--width|--fps|--bitrate-kbps)
            (($# >= 2)) || { echo "missing value: $1" >&2; exit 2; }
            option=$1; value=$2; shift 2
            case "$option" in
                --bin-dir) bin_dir=$value ;; --config) config=$value ;;
                --seconds) seconds=$value ;; --mount) mount=$value ;;
                --video-port) video_port=$value ;; --raw-port) raw_port=$value ;;
                --width) width=$value ;; --fps) fps=$value ;;
                --bitrate-kbps) bitrate=$value ;;
                --expected-source) expected_source=$value ;;
            esac ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
for value in "$seconds" "$mount" "$video_port" "$raw_port" "$width" "$fps" "$bitrate"; do
    [[ "$value" =~ ^[1-9][0-9]{0,5}$ ]] || { echo 'numeric arguments must be positive decimal integers' >&2; exit 2; }
done
((seconds <= 240 && mount <= 3 && video_port <= 65535 && raw_port <= 65535 &&
   video_port != raw_port && video_port != 19101 && raw_port != 19101 &&
   width >= 320 && width <= 1920 && width % 2 == 0 && fps <= 30 &&
   bitrate >= 100 && bitrate <= 8000)) || { echo 'argument out of range' >&2; exit 2; }
[[ "$bin_dir" = /* && "$config" = /* && -x "$bin_dir/l3_metadata_inspect" &&
   -x "$bin_dir/l3_video_relay" && -f "$config" ]] || {
    echo '--bin-dir and --config must be absolute paths to the built programs/private config' >&2; exit 2;
}
command -v ffmpeg >/dev/null
command -v timeout >/dev/null
source_args=()
if [[ -n "$expected_source" ]]; then
    [[ "$expected_source" =~ ^(0|[1-9][0-9]{0,2})$ ]] && ((expected_source <= 255)) || {
        echo '--expected-source must be a hardware-advertised decimal ID in 0..255' >&2; exit 2;
    }
    source_args=(--expected-source "$expected_source")
fi
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/l3-preview-XXXXXXXX")
cd "$run_dir"
mkfifo encoded.fifo
publisher=
encoder=
relay=
cleanup() {
    trap - EXIT INT TERM
    for pid in "$publisher" "$encoder" "$relay"; do
        [[ -z "$pid" ]] || kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "$publisher" "$encoder" "$relay"; do
        [[ -z "$pid" ]] || wait "$pid" 2>/dev/null || true
    done
    rm -f encoded.fifo
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "event=preview_begin seconds=$seconds width=$width fps=$fps bitrate_kbps=$bitrate recording_start=no run_dir=$run_dir"
timeout --signal=TERM --kill-after=3s "$((seconds + 35))s" \
    "$bin_dir/l3_metadata_inspect" --config "$config" --stream --streams video \
    --mount "$mount" --seconds "$seconds" --bind 127.0.0.1 \
    --point-port 19101 --video-port "$raw_port" --exclusive-test "${source_args[@]}" > onboard.private.log 2>&1 &
publisher=$!
deadline=$((SECONDS + 30))
while ! grep -q 'phase=video_hold' onboard.private.log; do
    if ! kill -0 "$publisher" 2>/dev/null || ((SECONDS >= deadline)); then
        echo "event=preview_failed phase=publisher_start details=$run_dir/onboard.private.log" >&2
        exit 1
    fi
    sleep 0.1
done
# Capture bytes only in a FIFO: no video recording/file. Both workers have
# independent outer deadlines if SDK or encoder cleanup fails to return.
timeout --signal=TERM --kill-after=3s "$((seconds + 15))s" \
    "$bin_dir/l3_video_relay" "$video_port" "$((seconds + 10))" < encoded.fifo &
relay=$!
timeout --signal=TERM --kill-after=3s "$((seconds + 10))s" \
    ffmpeg -hide_banner -loglevel warning -nostdin -threads 2 \
    -probesize 32768 -analyzeduration 0 -f h264 -i "tcp://127.0.0.1:$raw_port" \
    -an -vf "fps=$fps,scale=$width:-2" -c:v libx264 -threads 2 \
    -preset veryfast -tune zerolatency -pix_fmt yuv420p \
    -b:v "${bitrate}k" -maxrate "${bitrate}k" -bufsize "$((bitrate / 2))k" \
    -g "$fps" -keyint_min "$fps" -x264-params 'repeat-headers=1:scenecut=0' \
    -f h264 pipe:1 > encoded.fifo 2> encoder.private.log &
encoder=$!
set +e
wait "$publisher"; publisher_rc=$?; publisher=
wait "$encoder"; encoder_rc=$?; encoder=
wait "$relay"; relay_rc=$?; relay=
set -e
echo "event=preview_complete publisher_rc=$publisher_rc encoder_rc=$encoder_rc relay_rc=$relay_rc run_dir=$run_dir"
((publisher_rc == 0 && encoder_rc == 0 && relay_rc == 0))
