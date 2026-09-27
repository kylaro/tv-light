#!/usr/bin/env bash
# Builds the deployable tvlightd image (tvlight:latest).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
podman build -t tvlight:latest -f "$ROOT/container/Containerfile" "$ROOT"
