#!/usr/bin/env bash
# Run only in a disposable Fedora 44 container.
set -euo pipefail
package=$1
expected_version=$2
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
source "$(dirname "$0")/install-checks.sh"
dnf install -y rpm-build shadow-utils util-linux-core
# A synthetic older package exercises replacement and obsolete-file cleanup.
mkdir -p /tmp/previous/SPECS
cat > /tmp/previous/SPECS/sawfish.spec <<'SPEC'
Name: sawfish
Version: 0.0.0
Release: 1
Summary: Installation test fixture
License: GPL-3.0-or-later
BuildArch: noarch
%description
Older installation fixture.
%install
mkdir -p %{buildroot}/opt/sawfish
touch %{buildroot}/opt/sawfish/upgrade-marker
%files
/opt/sawfish/upgrade-marker
SPEC
rpmbuild --define '_topdir /tmp/previous' -bb /tmp/previous/SPECS/sawfish.spec
dnf remove -y rpm-build
dnf install -y /tmp/previous/RPMS/noarch/*.rpm
create_user_data
dnf install -y "$package"
check_installed
dnf remove -y sawfish
check_removed
