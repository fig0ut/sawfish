# Shared assertions for disposable package installation test environments.
create_user_data() {
    useradd -m sawfish-test
    data=/home/sawfish-test/.local/share/Sawfish/Sawfish
    install -d -o sawfish-test -g sawfish-test "$data/cores/retained"
    printf '%s\n' retained-profile > "$data/profiles.json"
    printf '%s\n' retained-settings > "$data/settings.ini"
    printf '%s\n' retained-core > "$data/cores/retained/rostra-daemon"
    chown -R sawfish-test:sawfish-test "$data"
}

check_user_data() {
    test "$(cat "$data/profiles.json")" = retained-profile
    test "$(cat "$data/settings.ini")" = retained-settings
    test "$(cat "$data/cores/retained/rostra-daemon")" = retained-core
}

check_installed() {
    test -f /usr/share/applications/io.github.fig0ut.sawfish.desktop
    test -f /usr/share/icons/hicolor/scalable/apps/io.github.fig0ut.sawfish.svg
    test ! -e /opt/sawfish/upgrade-marker
    test "$(runuser -u sawfish-test -- env QT_QPA_PLATFORM=offscreen sawfish --version)" = "Sawfish $expected_version"
    runuser -u sawfish-test -- /opt/sawfish/bin/rostra-daemon version
    check_user_data
}

check_removed() {
    test ! -e /opt/sawfish/bin/sawfish
    test ! -L /usr/bin/sawfish
    test ! -e /usr/share/applications/io.github.fig0ut.sawfish.desktop
    check_user_data
}
