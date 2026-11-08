#!/usr/bin/env bash
set -euo pipefail

sawfish_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
rostra_source=${ROSTRA_SOURCE:-"$sawfish_root/../rostra"}
sawfish_build=${SAWFISH_BUILD_DIR:-"$sawfish_root/build"}
mkdir -p "$sawfish_build"
sawfish_build=$(cd -- "$sawfish_build" && pwd)
python3 "$sawfish_root/scripts/ci.py" verify-core --rostra "$rostra_source"
actual_revision=$(git -C "$rostra_source" rev-parse HEAD)

cmake_args=()
# Optional workspace-local dependency prefix; no system installation is performed.
if [[ -d "$sawfish_root/.deps/grpc/usr" ]]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=$sawfish_root/.deps/grpc/usr")
    export LD_LIBRARY_PATH="$sawfish_root/.deps/grpc/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
cmake -S "$sawfish_root" -B "$sawfish_build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    "-DSAWFISH_ROSTRA_EXECUTABLE=$sawfish_build/rostra-daemon" "${cmake_args[@]}"
cmake --build "$sawfish_build" --parallel "${SAWFISH_BUILD_JOBS:-4}"

# Match Rostra's single-file desktop release. Custom tags may need extra libraries.
rostra_tags=${ROSTRA_BUILD_TAGS:-$(cat "$rostra_source/release/DEFAULT_BUILD_TAGS_OTHERS")}
rostra_ldflags=$(cat "$rostra_source/release/LDFLAGS")
rostra_version="sawfish-${actual_revision:0:9}"
mkdir -p "$sawfish_build/go-cache"
(
    cd "$rostra_source"
    env GOCACHE="$sawfish_build/go-cache" CGO_ENABLED="${CGO_ENABLED:-0}" go build -trimpath -tags "$rostra_tags" \
        -ldflags "-X github.com/fig0ut/rostra/constant.Version=$rostra_version $rostra_ldflags -buildid=" \
        -o "$sawfish_build/rostra-daemon" ./experimental/boxdd
)
python3 "$sawfish_root/scripts/ci.py" verify-core --rostra "$rostra_source" \
    --binary "$sawfish_build/rostra-daemon"
echo "Built $sawfish_build/sawfish and $sawfish_build/rostra-daemon"
