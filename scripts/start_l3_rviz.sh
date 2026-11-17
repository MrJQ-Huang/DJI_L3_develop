#!/usr/bin/env bash
# Local receive/display only. This script never starts an onboard recording.
set -eo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bridge_args=()
show_rviz=1
fixed_frame=l3_raw
rviz_config="$task_root/pc_viewer/l3.rviz"
previous_argument=
for argument in "$@"; do
    if [[ "$argument" == --streams=video || ( "$previous_argument" == --streams && "$argument" == video ) ]]; then
        rviz_config="$task_root/pc_viewer/l3_preview.rviz"
        fixed_frame=l3_camera_optical
    fi
    case "$argument" in
        --no-rviz) show_rviz=0 ;;
        --help|-h) show_rviz=0; bridge_args+=("$argument") ;;
        --xyz-scale-to-meters|--xyz-scale-to-meters=*)
            fixed_frame=l3_local; bridge_args+=("$argument") ;;
        *) bridge_args+=("$argument") ;;
    esac
    previous_argument=$argument
done
if [[ ! -f /opt/ros/humble/setup.bash ]]; then
    echo 'ROS 2 Humble is required at /opt/ros/humble.' >&2
    exit 2
fi
if [[ ! -x "$task_root/.venv/bin/python" ]]; then
    echo 'Prepare the repository .venv with requirements.txt first.' >&2
    exit 2
fi
source /opt/ros/humble/setup.bash
# ROS stays local; compressed application streams use the explicit host/proxy.
export ROS_LOCALHOST_ONLY=1
if [[ "$show_rviz" == 0 ]]; then
    exec "$task_root/.venv/bin/python" "$task_root/pc_viewer/l3_ros_bridge.py" "${bridge_args[@]}"
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo 'No graphical session; use --no-rviz for a bounded receive test.' >&2
    exit 2
fi
rviz_pid=
bridge_pid=
cleanup() {
    [[ -z "$bridge_pid" ]] || kill -INT "$bridge_pid" 2>/dev/null || true
    [[ -z "$rviz_pid" ]] || kill -TERM "$rviz_pid" 2>/dev/null || true
    [[ -z "$bridge_pid" ]] || wait "$bridge_pid" 2>/dev/null || true
    [[ -z "$rviz_pid" ]] || wait "$rviz_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$task_root/.venv/bin/python" "$task_root/pc_viewer/l3_ros_bridge.py" "${bridge_args[@]}" &
bridge_pid=$!
rviz2 -d "$rviz_config" -f "$fixed_frame" &
rviz_pid=$!
# Closing RViz or ending the bounded bridge test stops the other local process.
set +e
wait -n "$bridge_pid" "$rviz_pid"
result=$?
exit "$result"
