#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="${DJI_PYTHON:-$repo_dir/.venv/bin/python}"
if [[ ! -x "$python_bin" ]]; then
  echo 'Create .venv and install requirements.txt first, or set DJI_PYTHON.' >&2
  exit 1
fi
command -v adb >/dev/null
adb -d get-state
adb -d reverse tcp:9000 tcp:9000
adb -d shell am start -n com.dji.sampleV5.aircraft/dji.sampleV5.aircraft.L3ReadOnlyActivity
exec "$python_bin" "$repo_dir/pc_viewer/receiver.py" \
  --stats-json "$repo_dir/pc_viewer/live_stats.json" "$@"

