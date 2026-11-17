#!/usr/bin/env bash
# Bounded Manifold-only preview. SSH controls the onboard program; image data
# uses the explicitly supplied Tailnet host/SOCKS route, never the SSH tunnel.
set -euo pipefail
umask 077
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=
ssh_host=l3-manifold
identity=
socks5=127.0.0.1:1055
remote_root=/open_app/workspaces/dji/l3-dev/preview-20261117
remote_config=/open_app/workspaces/dji/l3-dev/private/psdk.conf
seconds=60
expected_source=
mount=1
display=video
while (($#)); do
    case "$1" in
        --help|-h)
            echo 'Usage: start_l3_preview_4g.sh --host MANIFOLD_TAILNET_IP [--ssh-host SSH_ALIAS] [--identity KEY_PATH] [--socks5 HOST:PORT] [--remote-root DIR] [--remote-config PATH] [--seconds 60] [--mount 1] [--expected-source ID] [--rviz|--no-display]'
            exit 0 ;;
        --rviz) display=rviz; shift ;;
        --no-display) display=none; shift ;;
        --host|--ssh-host|--identity|--socks5|--remote-root|--remote-config|--seconds|--mount|--expected-source)
            (($# >= 2)) || { echo "missing value: $1" >&2; exit 2; }
            option=$1; value=$2; shift 2
            case "$option" in
                --host) host=$value ;; --ssh-host) ssh_host=$value ;; --identity) identity=$value ;;
                --socks5) socks5=$value ;; --remote-root) remote_root=$value ;;
                --remote-config) remote_config=$value ;; --seconds) seconds=$value ;;
                --mount) mount=$value ;; --expected-source) expected_source=$value ;;
            esac ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$host" && "$ssh_host" != -* && -n "$ssh_host" && "$seconds" =~ ^[1-9][0-9]{0,2}$ ]] &&
    ((seconds <= 240)) || { echo '--host and --seconds 1..240 are required' >&2; exit 2; }
ssh_args=(-o BatchMode=yes -o ConnectTimeout=8)
[[ -z "$identity" ]] || ssh_args+=(-i "$identity")
remote_args=(bash "$remote_root/scripts/run_l3_preview_onboard.sh" --bin-dir "$remote_root/build"
             --config "$remote_config" --seconds "$seconds" --mount "$mount")
[[ -z "$expected_source" ]] || remote_args+=(--expected-source "$expected_source")
printf -v remote_command '%q ' "${remote_args[@]}"
mkdir -p "$repo_dir/.validation"
run_dir=$(mktemp -d "$repo_dir/.validation/preview-4g-XXXXXXXX")
publisher=
cleanup() {
    # All remote workers have their own finite timeout, including SDK cleanup.
    [[ -z "$publisher" ]] || kill -TERM "$publisher" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
ssh "${ssh_args[@]}" "$ssh_host" "$remote_command" > "$run_dir/onboard.private.log" 2>&1 &
publisher=$!
deadline=$((SECONDS + 40))
while ! grep -q '"event":"video_relay_ready"' "$run_dir/onboard.private.log"; do
    if ! kill -0 "$publisher" 2>/dev/null || ((SECONDS >= deadline)); then
        echo "Preview did not start; local log: $run_dir/onboard.private.log" >&2
        exit 1
    fi
    sleep 0.1
done
receive_args=(--host "$host" --socks5 "$socks5" --streams video --seconds "$seconds")
set +e
if [[ "$display" == rviz ]]; then
    bash "$repo_dir/scripts/start_l3_rviz.sh" "${receive_args[@]}" > "$run_dir/receiver.private.log" 2>&1
else
    [[ "$display" != none ]] || receive_args+=(--no-display)
    QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu "$repo_dir/.venv/bin/python" \
        "$repo_dir/pc_viewer/l3_stream.py" "${receive_args[@]}" > "$run_dir/receiver.private.log" 2>&1
fi
receiver_rc=$?
wait "$publisher"; onboard_rc=$?; publisher=
set -e
echo "event=preview_result receiver_rc=$receiver_rc onboard_rc=$onboard_rc logs=$run_dir"
((receiver_rc == 0 && onboard_rc == 0))
