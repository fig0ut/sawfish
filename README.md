# Sawfish

A Qt desktop client for [Rostra](https://github.com/fig0ut/rostra).

- Import profiles from files or URLs, edit them, and validate configurations automatically.
- Switch outbound proxies and test their latency.
- Customize a dashboard with traffic, connection, CPU, and memory charts.
- Inspect live connections and logs.
- Update the Rostra core independently through stable and prerelease channels.

Sawfish starts and manages its core automatically. Closing the window hides it in
the tray; **Quit Sawfish** stops the connection and exits both applications.

The project is a Linux-first preview. CI targets Linux and Windows on amd64 and
arm64; macOS support is planned.

## Build and run

Install the [build dependencies](docs/development.md#build-and-run), and check out
Rostra beside this repository at the revision recorded in
[`proto/REVISION`](proto/REVISION). On Linux:

```sh
./scripts/build.sh
./scripts/run-dev.sh
```

In Profiles, use **+ → From file** to import your Rostra configuration, then use
the switch on Home to connect.

See the [development guide](docs/development.md) for Windows builds, tests,
architecture, core updates, and release packaging.

## License

Copyright (C) 2026 Sawfish contributors.

Sawfish is licensed under the [GNU General Public License v3.0 or later](LICENSE)
(`GPL-3.0-or-later`). Third-party materials retain their own notices, including
the [Rostra protocol definitions](proto/LICENSE) and
[Heroicons](assets/heroicons/LICENSE).
