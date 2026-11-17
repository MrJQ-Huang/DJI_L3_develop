#!/usr/bin/env bash
# Ordinary HD video window; SSH controls preview selection, TCP carries H.264.
set -euo pipefail
umask 077
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec "$repo_dir/.venv/bin/python" "$repo_dir/pc_viewer/l3_preview.py" "$@"
