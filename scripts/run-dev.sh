#!/usr/bin/env bash
set -euo pipefail
umask 077
sawfish_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sawfish_build=${SAWFISH_BUILD_DIR:-"$sawfish_root/build"}
if [[ ! -f "$sawfish_build/CMakeCache.txt" || ! -x "$sawfish_build/rostra-daemon" ]]; then
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
# Compile GUI changes before launching so this command cannot silently run an
# older interface. The full build script remains responsible for the Go core.
cmake --build "$sawfish_build" --target sawfish --parallel "${SAWFISH_BUILD_JOBS:-4}"
exec "$sawfish_build/sawfish" --backend "$sawfish_build/rostra-daemon" \
    --data-dir "$sawfish_build/dev-runtime/client"
