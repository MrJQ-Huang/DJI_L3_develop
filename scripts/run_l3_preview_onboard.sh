#!/usr/bin/env bash
# Manifold 3: original PSDK H.264 -> TCP. No encoder, media files or ROS.
# The native app reads an allowlist of preview commands from stdin.
set -euo pipefail
umask 077
bin_dir=
config=
seconds=60
mount=1
video_port=9102
expected_source=
fast_switch=()
while (($#)); do
    case "$1" in
        --help|-h)
            echo 'Usage: run_l3_preview_onboard.sh --bin-dir DIR --config PRIVATE_FILE [--seconds 60] [--mount 1] [--expected-source ID] [--video-port 9102] [--fast-switch]'
            exit 0 ;;
        --fast-switch) fast_switch=(--fast-switch); shift ;;
        --bin-dir|--config|--seconds|--mount|--expected-source|--video-port)
            (($# >= 2)) || { echo "missing value: $1" >&2; exit 2; }
            option=$1; value=$2; shift 2
            case "$option" in
                --bin-dir) bin_dir=$value ;; --config) config=$value ;;
                --seconds) seconds=$value ;; --mount) mount=$value ;;
                --video-port) video_port=$value ;; --expected-source) expected_source=$value ;;
            esac ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
for value in "$seconds" "$mount" "$video_port"; do
    [[ "$value" =~ ^[1-9][0-9]{0,4}$ ]] || { echo 'numeric arguments must be positive decimal integers' >&2; exit 2; }
done
((seconds <= 300 && mount <= 3 && video_port <= 65535 && video_port != 19101)) || {
    echo 'argument out of range' >&2; exit 2;
}
[[ "$bin_dir" = /* && "$config" = /* && -x "$bin_dir/l3_metadata_inspect" && -f "$config" ]] || {
    echo '--bin-dir and --config must be absolute paths to the program/private config' >&2; exit 2;
}
source_args=()
if [[ -n "$expected_source" ]]; then
    [[ "$expected_source" =~ ^(0|[1-9][0-9]{0,2})$ ]] && ((expected_source <= 255)) || exit 2
    source_args=(--expected-source "$expected_source")
fi
command -v timeout >/dev/null
command -v flock >/dev/null
# Hold the lock through timeout/SDK cleanup, including an interrupted SSH client.
exec 9>"${TMPDIR:-/tmp}/l3-hd-preview-${UID}.lock"
flock -n 9 || { echo 'event=preview_failed reason=preview_already_running' >&2; exit 1; }
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/l3-hd-preview-XXXXXXXX")
cd "$run_dir"
echo "event=preview_begin seconds=$seconds encoding=source_h264 reencode=no recording_start=no run_dir=$run_dir"
exec timeout --signal=TERM --kill-after=3s "$((seconds + 35))s" \
    "$bin_dir/l3_metadata_inspect" --config "$config" --stream --streams video \
    --mount "$mount" --seconds "$seconds" --bind 127.0.0.1 \
    --point-port 19101 --video-port "$video_port" --exclusive-test \
    --video-control-stdin "${source_args[@]}" "${fast_switch[@]}"
