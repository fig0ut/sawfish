#!/usr/bin/env python3
"""Create native Linux packages from the already deployed and tested bundle."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

DESKTOP_ID = "io.github.fig0ut.sawfish"
ROOT = Path(__file__).resolve().parents[1]


def native_architecture(target):
    try:
        return {"linux-amd64": "x86_64", "linux-arm64": "aarch64"}[target]
    except KeyError:
        raise ValueError(f"Unsupported Linux target: {target}") from None


def debian_version(version):
    match = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?", version)
    if not match:
        raise ValueError(f"Invalid package version: {version!r}")
    base, prerelease, build = match.groups()
    # Debian sorts '~' before a final release. Keep the packaging revision separate.
    return base + ("~" + prerelease[1:] if prerelease else "") + (build or "")


def arch_version(version):
    debian_version(version)
    return version.replace("-", "", 1).replace("-", "_")


def prepare_root(stage, work):
    stage, work = (Path(p).resolve() for p in (stage, work))
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
    license_dir = root / "usr/share/licenses/sawfish"
    license_dir.mkdir(parents=True)
    shutil.copy2(stage / "LICENSE", license_dir / "LICENSE")
    (license_dir / "bundled").symlink_to("/opt/sawfish/share/licenses")
    subprocess.run(["desktop-file-validate", str(root / "usr/share/applications" /
                                                f"{DESKTOP_ID}.desktop")], check=True)
    return root


def cpack_values(root, version, target, output):
    debian_version(version)
    native_architecture(target)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    return {
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
        "CPACK_THREADS": "4",
    }


def write_cpack(work, values):
    config = Path(work).resolve() / "CPackConfig.cmake"
    # CMake bracket arguments preserve paths literally, including spaces and '$'.
    if any("]=]" in value for value in values.values()):
        raise ValueError("Unsupported CMake delimiter in a packaging path")
    config.write_text("".join(f"set({key} [=[{value}]=])\n" for key, value in values.items()),
                      encoding="utf-8")
    return config


def prepare_deb(stage, version, target, work, output):
    native_architecture(target)
    root = prepare_root(stage, work)
    values = cpack_values(root, version, target, output) | {
        "CPACK_GENERATOR": "DEB",
        "CPACK_DEBIAN_PACKAGE_VERSION": debian_version(version),
        "CPACK_DEBIAN_PACKAGE_RELEASE": "1",
        "CPACK_DEBIAN_PACKAGE_ARCHITECTURE": target.removeprefix("linux-"),
        "CPACK_DEBIAN_PACKAGE_SECTION": "net",
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS": "ON",
        "CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS": (root / "opt/sawfish/lib").as_posix(),
        # These are loaded at runtime and are not all visible in ELF dependencies.
        "CPACK_DEBIAN_PACKAGE_DEPENDS": "ca-certificates, libssl3t64, libgl1, libegl1",
        "CPACK_DEBIAN_COMPRESSION_TYPE": "xz",
    }
    return write_cpack(work, values)


def prepare_rpm(stage, version, target, work, output):
    architecture = native_architecture(target)
    root = prepare_root(stage, work)
    # Private Qt/ICU libraries must neither require a system Qt installation nor
    # advertise that this application provides system-wide shared libraries.
    sonames = set()
    for library in (root / "opt/sawfish/lib").glob("*.so*"):
        if library.is_symlink() or not library.is_file():
            continue
        dynamic = subprocess.run(["readelf", "-d", str(library)], check=True,
                                 capture_output=True, text=True).stdout
        sonames.update(re.findall(r"\(SONAME\).*\[([^\]]+)\]", dynamic))
    # Use POSIX ERE escaping for rpm's dependency filter.
    if any(not re.fullmatch(r"[A-Za-z0-9_.+-]+", name) for name in sonames):
        raise ValueError("Unexpected private library SONAME")
    escaped = [re.sub(r"([.+])", r"[\1]", name) for name in sorted(sonames)]
    macros = "%global debug_package %{nil}\n%global _build_id_links none"
    if escaped:
        macros += "\n%global __requires_exclude ^(" + "|".join(escaped) + r")([(].*)?$"
    values = cpack_values(root, version, target, output) | {
        "CPACK_GENERATOR": "RPM",
        "CPACK_RPM_PACKAGE_VERSION": debian_version(version),
        "CPACK_RPM_PACKAGE_RELEASE": "1",
        "CPACK_RPM_PACKAGE_ARCHITECTURE": architecture,
        "CPACK_RPM_PACKAGE_LICENSE": "GPL-3.0-or-later",
        "CPACK_RPM_PACKAGE_GROUP": "Applications/Internet",
        "CPACK_RPM_PACKAGE_URL": "https://github.com/fig0ut/sawfish",
        "CPACK_RPM_PACKAGE_DESCRIPTION": "Manage Rostra profiles, proxies, connections and core updates.",
        "CPACK_RPM_PACKAGE_AUTOREQ": "ON",
        "CPACK_RPM_PACKAGE_AUTOPROV": "OFF",
        # These are dlopened rather than present in ELF DT_NEEDED entries.
        "CPACK_RPM_PACKAGE_REQUIRES": "ca-certificates, openssl-libs, libglvnd-glx, libglvnd-egl",
        "CPACK_RPM_SPEC_MORE_DEFINE": macros,
        "CPACK_RPM_RELOCATABLE": "OFF",
        "CPACK_RPM_COMPRESSION_TYPE": "xz",
        "CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION": ";".join((
            "/usr/share/applications", "/usr/share/licenses", "/usr/share/icons",
            "/usr/share/icons/hicolor", "/usr/share/icons/hicolor/scalable",
            "/usr/share/icons/hicolor/scalable/apps")),
    }
    return write_cpack(work, values)


def package_rpm(stage, version, target, work, output):
    config = prepare_rpm(stage, version, target, work, output)
    subprocess.run(["cpack", "--config", str(config)], check=True, cwd=config.parent, timeout=240)


def render_pkgbuild(version):
    recipe = (ROOT / "packaging/arch/PKGBUILD.in").read_text(encoding="utf-8")
    return recipe.replace("@PKGVER@", arch_version(version))


def package_arch(stage, version, target, work, output):
    if any(not shutil.which(tool) for tool in ("makepkg", "pacman", "fakeroot", "zstd", "bsdtar")):
        raise ValueError("Install makepkg, pacman-package-manager, fakeroot, zstd and libarchive-tools")
    architecture = native_architecture(target)
    root = prepare_root(stage, work)
    work, output = Path(work).resolve(), Path(output).resolve()
    (work / "PKGBUILD").write_text(render_pkgbuild(version), encoding="utf-8")
    # A Debian host has no pacman database. Use an isolated empty database for
    # makepkg's build-info query; the original build provenance is in build-info.json.
    database = work / "pacman-db"
    (database / "local").mkdir(parents=True, exist_ok=True)
    # makepkg deliberately ignores PACMAN_OPTS for queries, so set PACMAN to a
    # wrapper that also directs its build-info query to the isolated database.
    pacman = work / "pacman"
    pacman.write_text('#!/bin/sh\nexec ' + shlex.quote(shutil.which("pacman")) +
                      ' --dbpath "$SAWFISH_PACMAN_DB" "$@"\n', encoding="utf-8")
    pacman.chmod(0o755)
    config = work / "makepkg.conf"
    config.write_text('source /etc/makepkg.conf\n'
                      'COMPRESSZST=(zstd -c -T2 -6 -)\n', encoding="utf-8")
    environment = os.environ | {"SAWFISH_PACKAGE_ROOT": str(root), "CARCH": architecture,
                               "SAWFISH_PACMAN_DB": str(database), "PKGEXT": ".pkg.tar.zst",
                               "PACMAN": str(pacman), "PKGDEST": str(work), "LC_ALL": "C"}
    # This recipe only copies our staged payload. Dependencies are checked by
    # pacman in the clean installation test, not against the Ubuntu build host.
    subprocess.run(["makepkg", "--config", str(config), "--force", "--nodeps", "--nosign", "--noconfirm"],
                   cwd=work, env=environment, check=True, timeout=240)
    package = work / f"sawfish-bin-{arch_version(version)}-1-{architecture}.pkg.tar.zst"
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(package, output / f"sawfish-{version}-{target}.pkg.tar.zst")


def package_deb(stage, version, target, work, output):
    if any(not shutil.which(tool) for tool in ("cpack", "dpkg-shlibdeps", "file",
                                               "desktop-file-validate")):
        raise ValueError("Build the .deb on Ubuntu 24.04 with cmake, dpkg-dev, file and desktop-file-utils")
    config = prepare_deb(stage, version, target, work, output)
    subprocess.run(["cpack", "--config", str(config)], check=True, cwd=config.parent, timeout=180)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=("deb", "rpm", "arch"))
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    {"deb": package_deb, "rpm": package_rpm, "arch": package_arch}[args.format](
        args.stage, args.version, args.target, args.work, args.output)
