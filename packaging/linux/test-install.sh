#!/usr/bin/env bash
# Run inside a disposable Ubuntu 24.04 container, never on the developer's host.
set -euo pipefail
package=$1
expected_version=$2
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
source "$(dirname "$0")/install-checks.sh"
apt-get update -qq
apt-get install -y --no-install-recommends passwd util-linux

# Simulate an older installation without requiring a previously published .deb.
dpkg-deb -R "$package" /tmp/previous
sed -i 's/^Version:.*/Version: 0.0.0-1/' /tmp/previous/DEBIAN/control
dpkg-deb --root-owner-group -b /tmp/previous /tmp/previous.deb
apt-get install -y /tmp/previous.deb
create_user_data
apt-get install -y "$package"
check_installed
dpkg --purge sawfish
check_removed
