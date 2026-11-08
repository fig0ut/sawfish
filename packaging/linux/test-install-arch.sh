#!/usr/bin/env bash
# Run only in a disposable Arch / Arch Linux ARM container.
set -euo pipefail
package=$1
expected_version=$2
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
source "$(dirname "$0")/install-checks.sh"
pacman-key --init
if [[ $(uname -m) == aarch64 ]]; then
    pacman-key --populate archlinuxarm
else
    pacman-key --populate archlinux
fi
# Hosted ARM containers can reject Landlock rules with EPERM. Limit this
# workaround to pacman's downloader in this disposable installation test.
# Pacman 7.0 only has the combined flag; newer versions can keep syscall filtering.
pacman_options=(--disable-sandbox)
pacman_help=$(pacman --sync --help)
if [[ "$pacman_help" == *--disable-sandbox-filesystem* ]]; then
    pacman_options=(--disable-sandbox-filesystem)
fi
pacman "${pacman_options[@]}" -Syu --needed --noconfirm shadow util-linux zstd
mkdir -p /tmp/previous/opt/sawfish
touch /tmp/previous/opt/sawfish/upgrade-marker
cat > /tmp/previous/.PKGINFO <<'INFO'
pkgname = sawfish-bin
pkgver = 0.0.0-1
pkgdesc = Older installation fixture
size = 0
arch = any
INFO
tar --zstd -cf /tmp/previous.pkg.tar.zst -C /tmp/previous .PKGINFO opt
pacman "${pacman_options[@]}" -U --noconfirm /tmp/previous.pkg.tar.zst
create_user_data
pacman "${pacman_options[@]}" -U --noconfirm "$package"
check_installed
pacman -R --noconfirm sawfish-bin
check_removed
