#!/usr/bin/env bash
set -euo pipefail
umask 077
sawfish_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sawfish_build=${SAWFISH_BUILD_DIR:-"$sawfish_root/build"}
if [[ ! -d "$sawfish_build" ]]; then
    echo "Build Sawfish with scripts/build.sh first." >&2
    exit 1
fi
sawfish_build=$(cd -- "$sawfish_build" && pwd)
if [[ "${1:-gui}" != gui ]]; then
    echo "Usage: $0 [gui]. Sawfish starts and stops Rostra automatically." >&2
    exit 2
fi
mkdir -p "$sawfish_build/dev-runtime/client"
if [[ -d "$sawfish_root/.deps/grpc/usr/lib" ]]; then
    export LD_LIBRARY_PATH="$sawfish_root/.deps/grpc/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$sawfish_build/sawfish" --backend "$sawfish_build/rostra-daemon" \
    --data-dir "$sawfish_build/dev-runtime/client"
