#!/bin/bash
set -e

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

nohup sudo "$PROJECT_DIR/build/MID" "$PROJECT_DIR/setting.yaml" >/dev/null 2>&1 &
