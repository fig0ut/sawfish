#!/usr/bin/env python3
"""Create a Debian package from the already deployed and tested Linux bundle."""

from pathlib import Path
import re
import shutil
import subprocess

DESKTOP_ID = "io.github.fig0ut.sawfish"


def debian_version(version):
    match = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?", version)
    if not match:
        raise ValueError(f"Invalid package version: {version!r}")
    base, prerelease, build = match.groups()
    # Debian sorts '~' before a final release. Keep the packaging revision separate.
    return base + ("~" + prerelease[1:] if prerelease else "") + (build or "")


def prepare_deb(stage, version, target, work, output):
    architecture = {"linux-amd64": "amd64", "linux-arm64": "arm64"}.get(target)
    if not architecture:
        raise ValueError(f"Unsupported Debian target: {target}")
    stage, work, output = (Path(p).resolve() for p in (stage, work, output))
    root = work / "root"
    if root.exists():
        shutil.rmtree(root)
    bundle = root / "opt/sawfish"
    shutil.copytree(stage, bundle, symlinks=True)
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/bin/sawfish").symlink_to("/opt/sawfish/bin/sawfish")
    for relative in (f"applications/{DESKTOP_ID}.desktop",
                     f"icons/hicolor/scalable/apps/{DESKTOP_ID}.svg"):
        destination = root / "usr/share" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(stage / "share" / relative, destination)
    copyright_file = root / "usr/share/doc/sawfish/copyright"
    copyright_file.parent.mkdir(parents=True)
    shutil.copy2(stage / "LICENSE", copyright_file)
    (copyright_file.parent / "licenses").symlink_to("/opt/sawfish/share/licenses")
    subprocess.run(["desktop-file-validate", str(root / "usr/share/applications" /
                                                f"{DESKTOP_ID}.desktop")], check=True)
    output.mkdir(parents=True, exist_ok=True)
    values = {
        "CPACK_GENERATOR": "DEB",
        "CPACK_PACKAGE_NAME": "sawfish",
        "CPACK_PACKAGE_VENDOR": "Sawfish contributors",
        "CPACK_PACKAGE_CONTACT": "fig0ut <fig0ut@proton.me>",
        "CPACK_PACKAGE_DESCRIPTION_SUMMARY": "Qt desktop client for Rostra",
        "CPACK_PACKAGE_DESCRIPTION": "Manage Rostra profiles, proxies, connections and core updates.",
        "CPACK_PACKAGE_HOMEPAGE_URL": "https://github.com/fig0ut/sawfish",
        "CPACK_PACKAGE_VERSION": version,
        "CPACK_PACKAGE_DIRECTORY": output.as_posix(),
        "CPACK_PACKAGE_FILE_NAME": f"sawfish-{version}-{target}",
        "CPACK_INSTALLED_DIRECTORIES": root.as_posix() + ";/",
        "CPACK_PACKAGING_INSTALL_PREFIX": "/",
        "CPACK_DEBIAN_PACKAGE_VERSION": debian_version(version),
        "CPACK_DEBIAN_PACKAGE_RELEASE": "1",
        "CPACK_DEBIAN_PACKAGE_ARCHITECTURE": architecture,
        "CPACK_DEBIAN_PACKAGE_SECTION": "net",
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS": "ON",
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS": (bundle / "lib").as_posix(),
        # These are loaded at runtime and are not all visible in ELF dependencies.
        "CPACK_DEBIAN_PACKAGE_DEPENDS": "ca-certificates, libssl3t64, libgl1, libegl1",
        "CPACK_DEBIAN_COMPRESSION_TYPE": "xz",
        "CPACK_THREADS": "4",
    }
    config = work / "CPackConfig.cmake"
    # CMake bracket arguments preserve paths literally, including spaces and '$'.
    if any("]=]" in value for value in values.values()):
        raise ValueError("Unsupported CMake delimiter in a packaging path")
    config.write_text("".join(f"set({key} [=[{value}]=])\n" for key, value in values.items()),
                      encoding="utf-8")
    return config


def package_deb(stage, version, target, work, output):
    if any(not shutil.which(tool) for tool in ("cpack", "dpkg-shlibdeps", "file",
                                               "desktop-file-validate")):
        raise ValueError("Build the .deb on Ubuntu 24.04 with cmake, dpkg-dev, file and desktop-file-utils")
    config = prepare_deb(stage, version, target, work, output)
    subprocess.run(["cpack", "--config", str(config)], check=True, cwd=config.parent, timeout=180)
