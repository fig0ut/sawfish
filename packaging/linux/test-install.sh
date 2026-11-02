#!/usr/bin/env bash
# Run inside a disposable Ubuntu 24.04 container, never on the developer's host.
set -euo pipefail
package=$1
expected_version=$2
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
apt-get update -qq
apt-get install -y --no-install-recommends passwd util-linux

# Simulate an older installation without requiring a previously published .deb.
dpkg-deb -R "$package" /tmp/previous
sed -i 's/^Version:.*/Version: 0.0.0-1/' /tmp/previous/DEBIAN/control
dpkg-deb --root-owner-group -b /tmp/previous /tmp/previous.deb
apt-get install -y /tmp/previous.deb
useradd -m sawfish-test
data=/home/sawfish-test/.local/share/Sawfish/Sawfish
install -d -o sawfish-test -g sawfish-test "$data/cores/retained"
printf '%s\n' 'retained-profile' > "$data/profiles.json"
printf '%s\n' 'retained-settings' > "$data/settings.ini"
printf '%s\n' 'retained-core' > "$data/cores/retained/rostra-daemon"
chown -R sawfish-test:sawfish-test "$data"
apt-get install -y "$package"
test -f /usr/share/applications/io.github.fig0ut.sawfish.desktop
test -f /usr/share/icons/hicolor/scalable/apps/io.github.fig0ut.sawfish.svg
test "$(runuser -u sawfish-test -- env QT_QPA_PLATFORM=offscreen sawfish --version)" = "Sawfish $expected_version"
runuser -u sawfish-test -- /opt/sawfish/bin/rostra-daemon version
dpkg --purge sawfish
test ! -e /opt/sawfish/bin/sawfish
test ! -e /usr/bin/sawfish
test ! -e /usr/share/applications/io.github.fig0ut.sawfish.desktop
test "$(cat "$data/profiles.json")" = retained-profile
test "$(cat "$data/settings.ini")" = retained-settings
test "$(cat "$data/cores/retained/rostra-daemon")" = retained-core
