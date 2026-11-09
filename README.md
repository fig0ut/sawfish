# Sawfish

A Qt desktop client for [Rostra](https://github.com/fig0ut/rostra).

- Import profiles from files or URLs, edit them, and validate configurations automatically.
- Switch outbound proxies and test their latency.
- Customize a dashboard with traffic, connection, CPU, and memory charts.
- Inspect live connections and logs.
- Enable the desktop system proxy on GNOME and KDE Plasma, with automatic restoration on stop.
- Update the Rostra core independently through stable and prerelease channels.

Sawfish starts and manages its core automatically. Closing the window hides it in
the tray; **Quit Sawfish** stops the connection and exits both applications.

The project is a Linux-first preview. CI targets Linux and Windows on amd64 and
arm64; macOS support is planned.

## Download and run

Download a bundle from [GitHub Releases](https://github.com/fig0ut/sawfish/releases).
Choose `amd64` for Intel/AMD 64-bit computers or `arm64` for ARM64 computers.
Rostra and the required Qt libraries are included.

| System | Download | Start |
| --- | --- | --- |
| Ubuntu 24.04 / compatible Linux | `linux-amd64.tar.gz` or `linux-arm64.tar.gz` | Extract the complete archive, then run `bin/sawfish`. |
| Windows 10/11 x64 or Windows 11 ARM64 | `windows-amd64.zip` or `windows-arm64.zip` | Extract the complete archive, then open `bin/sawfish.exe`. |

Keep the extracted directories together. If Windows reports missing runtime
DLLs, install the matching [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
The archive does not create menu shortcuts. Linux currently requires X11 or
XWayland; the Ubuntu-built bundle is not guaranteed to run on older distributions.

In Profiles, use **+ → From file** or **From URL** to import a Rostra configuration,
then use the switch on Home to connect. On GNOME or KDE Plasma, enable **System
proxy** on Home to route applications that follow desktop proxy settings through
Sawfish. The profile needs a local mixed, HTTP, or SOCKS inbound with a fixed port
and no authentication or TLS. The switch starts off; Sawfish restores the previous
desktop settings when disconnected or closed using **Quit**, and after a GUI crash.
Closing the window to the tray keeps the connection and system proxy active.

Applications with their own proxy settings still need the inbound address/port
configured manually. Privileged TUN integration is not implemented yet.

### Linux packages

New releases provide ready-to-install `.deb`, `.rpm`, and `.pkg.tar.zst` packages.
The original `v0.1.0-alpha.1` has archives only; `v0.1.0-alpha.2` also has `.deb`
packages and an Arch recipe. The recipe is now used internally by CI.

On Ubuntu 24.04, download the `.deb` for your architecture and install it with
`sudo apt install ./sawfish-<version>-linux-<architecture>.deb`. Start **Sawfish**
from the applications menu or run `sawfish`. Remove it with `sudo apt remove sawfish`.

On Fedora 44, install the matching `.rpm` with
`sudo dnf install ./sawfish-<version>-linux-<architecture>.rpm`.
Remove it with `sudo dnf remove sawfish`. Other RPM distributions are not yet
tested.

On Arch / Arch Linux ARM, install the matching `.pkg.tar.zst` with
`sudo pacman -U ./sawfish-<version>-linux-<architecture>.pkg.tar.zst`.
Remove it with `sudo pacman -R sawfish-bin`. No compilation is needed.

Signed releases include an embedded `debsigs` signature for `.deb`, an embedded
RPM signature, a `.sig` for Arch packages, and an `.asc` for Linux archives.
Before installation, follow the [signature verification instructions](docs/development.md#verifying-downloads)
to check the public-key fingerprint and set up the appropriate verifier.
Installing a local `.deb` with APT does not automatically verify its embedded signature.

Packages preserve profiles, settings, and downloaded cores when upgraded or
removed. Sawfish GUI updates currently require installing a newer package or
extracting a newer archive; Rostra updates are available inside Sawfish.

## Build from source

Install the [build dependencies](docs/development.md#build-and-run), and check out
Rostra beside this repository with the API version recorded in
[`proto/REVISION`](proto/REVISION) and matching protocol definitions. The source
commit can change without updating Sawfish. On Linux:

```sh
./scripts/build.sh
./scripts/run-dev.sh
```

See the [development guide](docs/development.md) for Windows builds, tests,
architecture, core updates, and release packaging.

## License

Copyright (C) 2026 Sawfish contributors.

Sawfish is licensed under the [GNU General Public License v3.0 or later](LICENSE)
(`GPL-3.0-or-later`). Third-party materials retain their own notices, including
the [Rostra protocol definitions](proto/LICENSE) and
[Heroicons](assets/heroicons/LICENSE).
