#!/usr/bin/env bash
set -euo pipefail
version=${SAWFISH_VERSION:?}
target=${SAWFISH_TARGET:?}
root=$(cd "$(dirname "$0")/.." && pwd)
mounts=(--mount "type=bind,src=$root/dist,dst=/packages,readonly"
        --mount "type=bind,src=$root/packaging/linux,dst=/checks,readonly")
docker run --rm "${mounts[@]}" ubuntu:24.04 bash /checks/test-install.sh \
    "/packages/sawfish-$version-$target.deb" "$version"
docker run --rm "${mounts[@]}" registry.fedoraproject.org/fedora:44 bash /checks/test-install-rpm.sh \
    "/packages/sawfish-$version-$target.rpm" "$version"
arch_image=archlinux:base
if [[ "$target" == linux-arm64 ]]; then
    # Arch Linux's official container is x86_64. Use the official Arch Linux ARM
    # root filesystem on the native ARM runner; no third-party image or emulator.
    mkdir -p "$root/.ci"
    curl --fail --location --retry 3 --max-time 600 \
        https://ca.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz \
        -o "$root/.ci/archlinuxarm.tar.gz"
    arch_image=$(docker import "$root/.ci/archlinuxarm.tar.gz")
fi
docker run --rm "${mounts[@]}" "$arch_image" bash /checks/test-install-arch.sh \
    "/packages/sawfish-$version-$target.pkg.tar.zst" "$version"
