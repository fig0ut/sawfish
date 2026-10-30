#!/usr/bin/env python3
"""Native CI build/package driver. Uses only the Python standard library."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("SAWFISH_BUILD_DIR", ROOT / "build")).resolve()
GRPC_PREFIX = ROOT / ".deps/ci-grpc"
GRPC_COMMIT = "3252a89f10d8e92997862167ca7d095ecda85973"  # v1.84.0
GRPC_VERSION = "1.84.0"
VERSION_PATTERN = r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.+-]+)?"
PROTOS = ("daemon/started_service.proto", "daemon/managed_service.proto",
          "experimental/boxdd/desktop_service.proto")
WINDOWS = sys.platform == "win32"
SUFFIX = ".exe" if WINDOWS else ""


def run(*args, cwd=ROOT, capture=False, env=None):
    print("+", " ".join(str(a) for a in args), flush=True)
    result = subprocess.run([str(a) for a in args], cwd=cwd, check=True,
                            text=True, stdout=subprocess.PIPE if capture else None, env=env)
    return result.stdout.strip() if capture else None


def revision():
    values = dict(line.split("=", 1) for line in (ROOT / "proto/REVISION").read_text().splitlines()
                  if line and not line.startswith("#"))
    match = re.fullmatch(r"https://github.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)",
                         values.get("repository", ""))
    if not match or not re.fullmatch(r"[0-9a-f]{40}", values.get("commit", "")):
        raise ValueError("proto/REVISION must contain a GitHub repository and a full commit SHA")
    if values.get("api_version") != "4":
        raise ValueError("Review the client API version before changing the core pin")
    return match[1], values["commit"]


def checked_version(version):
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError(f"Invalid Sawfish version: {version!r}")
    return version


def metadata():
    repository, commit = revision()
    tag = os.environ.get("SAWFISH_RELEASE_TAG", "")
    if tag:
        if not tag.startswith("v"):
            raise ValueError("Release tags must use vMAJOR.MINOR.PATCH, optionally with a prerelease suffix")
        version = checked_version(tag[1:])
    else:
        base = re.search(r"project\(sawfish VERSION ([0-9.]+)",
                         (ROOT / "CMakeLists.txt").read_text())[1]
        sha = os.environ.get("GITHUB_SHA", "uncommitted")[:12]
        version = checked_version(f"{base}-dev.{sha}")
    values = {"rostra_repository": repository, "rostra_commit": commit, "version": version}
    for key, value in values.items():
        print(f"{key}={value}")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.writelines(f"{key}={value}\n" for key, value in values.items())


def verify_core(source, require_clean=False):
    _, commit = revision()
    actual = run("git", "rev-parse", "HEAD", cwd=source, capture=True)
    if actual != commit:
        raise ValueError("Rostra HEAD differs from proto/REVISION; update the pin deliberately")
    for proto in PROTOS:
        if (source / proto).read_bytes() != (ROOT / "proto" / proto).read_bytes():
            raise ValueError(f"Pinned protocol differs: {proto}")
    if not (source / "experimental/boxdd/cmd_version.go").is_file():
        raise ValueError("The pinned Rostra commit predates the owned core command interface. "
                         "Commit/push those Rostra changes, then update proto/REVISION before CI can pass.")
    if require_clean and run("git", "status", "--porcelain", cwd=source, capture=True):
        raise ValueError("CI requires a clean Rostra checkout")


def environment():
    qt = (ROOT / ".deps/qt" / os.environ["SAWFISH_QT_VERSION"] /
          os.environ["QT_DIRECTORY"]).resolve()
    if not (qt / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
        raise ValueError(f"Qt SDK not found at {qt}")
    temporary = Path(os.environ["RUNNER_TEMP"]) / "sf"
    temporary.mkdir(parents=True, exist_ok=True)
    values = {"SAWFISH_QT_PREFIX": qt, "TEMP": temporary, "TMP": temporary, "TMPDIR": temporary}
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
        output.writelines(f"{key}={value}\n" for key, value in values.items())
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
        output.write(str(qt / "bin") + "\n")


def copy_licenses(source, destination):
    """Keep dependency license/notice files next to the installed static libraries."""
    for directory, directories, files in os.walk(source):
        directories[:] = [name for name in directories if name != ".git"]
        for name in files:
            if name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")):
                path = Path(directory) / name
                target = destination / path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)


def dependencies():
    source, build = ROOT / ".ci/grpc", ROOT / ".ci/grpc-build"
    source.parent.mkdir(exist_ok=True)
    if not source.exists():
        run("git", "init", source)
        run("git", "remote", "add", "origin", "https://github.com/grpc/grpc.git", cwd=source)
    run("git", "fetch", "--depth", "1", "origin", GRPC_COMMIT, cwd=source)
    run("git", "checkout", "--detach", GRPC_COMMIT, cwd=source)
    # Only dependencies used by this C++ build; grpc-proto supplies the channelz
    # and reflection schemas even when tests are disabled. Avoid test suites
    # such as bloaty and its own large collection of submodules.
    run("git", "submodule", "update", "--init", "--recursive", "--depth", "1", "--",
        "third_party/abseil-cpp", "third_party/boringssl-with-bazel", "third_party/cares/cares",
        "third_party/grpc-proto", "third_party/protobuf", "third_party/re2",
        "third_party/zlib", cwd=source)
    options = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_STANDARD=17",
               "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DCMAKE_INSTALL_LIBDIR=lib",
               "-DBUILD_SHARED_LIBS=OFF", "-DgRPC_INSTALL=ON", "-DgRPC_BUILD_TESTS=OFF",
               "-DABSL_PROPAGATE_CXX_STD=ON", "-DABSL_ENABLE_INSTALL=ON",
               "-Dprotobuf_BUILD_TESTS=OFF", "-Dprotobuf_BUILD_SHARED_LIBS=OFF",
               "-DgRPC_MSVC_STATIC_RUNTIME=OFF", "-Dprotobuf_MSVC_STATIC_RUNTIME=OFF"]
    options += [f"-DgRPC_BUILD_GRPC_{language}_PLUGIN=OFF"
                for language in ("CSHARP", "NODE", "OBJECTIVE_C", "PHP", "PYTHON", "RUBY")]
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        f"-DCMAKE_INSTALL_PREFIX={GRPC_PREFIX}", *options)
    run("cmake", "--build", build, "--parallel", os.environ.get("SAWFISH_BUILD_JOBS", "2"))
    run("cmake", "--install", build)
    copy_licenses(source, GRPC_PREFIX / "share/licenses/grpc-and-dependencies")


def core_info(binary):
    info = json.loads(run(binary, "version", capture=True))
    if info.get("api_version") != 4 or not isinstance(info.get("version"), str) or not info["version"]:
        raise ValueError("Bundled Rostra does not implement the required core version/API contract")
    return info


def build(source):
    verify_core(source)
    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / ("rostra-daemon" + SUFFIX)
    _, commit = revision()
    core_version = "sawfish-" + commit[:9]
    # Match Rostra's single-file desktop release; Naive needs a companion libcronet.
    tags = (source / "release/DEFAULT_BUILD_TAGS_OTHERS").read_text().strip()
    shared_ldflags = (source / "release/LDFLAGS").read_text().strip()
    run("go", "build", "-v", "-trimpath", "-tags", tags, "-ldflags",
        f"-X github.com/fig0ut/rostra/constant.Version={core_version} {shared_ldflags} -s -w -buildid=",
        "-o", binary, "./experimental/boxdd", cwd=source, env=os.environ | {"CGO_ENABLED": "0"})
    core_info(binary)
    prefix = os.environ["SAWFISH_QT_PREFIX"] + ";" + str(GRPC_PREFIX)
    version = checked_version(os.environ["SAWFISH_VERSION"])
    run("cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_PREFIX_PATH={prefix}", f"-DSAWFISH_VERSION={version}",
        f"-DSAWFISH_ROSTRA_EXECUTABLE={binary}", "-DBUILD_TESTING=ON")
    run("cmake", "--build", BUILD, "--parallel", os.environ.get("SAWFISH_BUILD_JOBS", "2"))


def assert_test_results(path):
    suites = ET.parse(path).getroot()
    cases = list(suites.iter("testcase"))
    if not cases or any(case.get("status") in ("notrun", "disabled") or
                        case.find("skipped") is not None or
                        case.find("failure") is not None or case.find("error") is not None
                        for case in cases):
        raise ValueError("CI requires all registered tests to run and pass; skipped tests are failures")


def test():
    output = BUILD / "test-results.xml"
    run("ctest", "--test-dir", BUILD, "--output-on-failure", "--no-tests=error",
        "--output-junit", output)
    assert_test_results(output)


def verify_architecture(binary, target):
    with binary.open("rb") as executable:
        header = executable.read(64)
        if target.startswith("linux-") and len(header) == 64 and header[:6] == b"\x7fELF\x02\x01":
            machine = struct.unpack_from("<H", header, 18)[0]
            expected = 62 if target.endswith("amd64") else 183
        elif target.startswith("windows-") and header[:2] == b"MZ" and len(header) == 64:
            executable.seek(struct.unpack_from("<I", header, 60)[0])
            pe = executable.read(6)
            if len(pe) != 6 or pe[:4] != b"PE\0\0":
                raise ValueError(f"Invalid Windows executable: {binary}")
            machine = struct.unpack_from("<H", pe, 4)[0]
            expected = 0x8664 if target.endswith("amd64") else 0xAA64
        else:
            raise ValueError(f"Executable format does not match {target}: {binary}")
        if machine != expected:
            raise ValueError(f"Executable architecture does not match {target}: {binary}")


def package(source):
    verify_core(source)
    version = checked_version(os.environ["SAWFISH_VERSION"])
    target = os.environ["SAWFISH_TARGET"]
    if target not in {"linux-amd64", "linux-arm64", "windows-amd64", "windows-arm64"}:
        raise ValueError("Unsupported package target")
    name = f"sawfish-{version}-{target}"
    stage = BUILD / "package" / name
    if stage.exists():
        shutil.rmtree(stage)
    run("cmake", "--install", BUILD, "--prefix", stage)
    binary = stage / "bin" / ("rostra-daemon" + SUFFIX)
    verify_architecture(binary, target)
    verify_architecture(stage / "bin" / ("sawfish" + SUFFIX), target)
    info = core_info(binary)
    licenses = stage / "share/licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "LICENSE", licenses / "Rostra-LICENSE")
    grpc_licenses = GRPC_PREFIX / "share/licenses"
    if grpc_licenses.exists():
        shutil.copytree(grpc_licenses, licenses, dirs_exist_ok=True)
    qt = Path(os.environ["SAWFISH_QT_PREFIX"])
    for directory in (qt / "LICENSES", qt / "licenses"):
        if directory.exists():
            shutil.copytree(directory, licenses / "Qt", dirs_exist_ok=True)
    qt_version = os.environ["SAWFISH_QT_VERSION"]
    qt_licenses = licenses / "Qt"
    qt_licenses.mkdir(exist_ok=True)
    for license_name in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt", "Qt-GPL-exception-1.0.txt"):
        url = f"https://raw.githubusercontent.com/qt/qtbase/v{qt_version}/LICENSES/{license_name}"
        with urllib.request.urlopen(url, timeout=30) as response:
            (qt_licenses / license_name).write_bytes(response.read())
    repository, commit = revision()
    manifest = {"sawfish_version": version, "sawfish_commit": os.environ.get("GITHUB_SHA", "local"),
                "target": target, "rostra_repository": repository, "rostra_commit": commit,
                "rostra_dirty": bool(run("git", "status", "--porcelain", cwd=source, capture=True)),
                "rostra": info, "qt_version": qt_version, "grpc_version": GRPC_VERSION,
                "grpc_commit": GRPC_COMMIT,
                "qt_source": f"https://download.qt.io/archive/qt/{'.'.join(qt_version.split('.')[:2])}/{qt_version}/submodules/"}
    (stage / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for filename in ("README.md", "LICENSE", "proto/LICENSE", "assets/heroicons/LICENSE",
                     "docs/development.md"):
        destination = stage / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / filename, destination)
    # The installed program must resolve its deployed Qt libraries/plugins. Do
    # not let the SDK's PATH or plugin path disguise missing runtime files.
    smoke_env = dict(os.environ)
    for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "LD_LIBRARY_PATH"):
        smoke_env.pop(key, None)
    smoke_env["PATH"] = os.pathsep.join(p for p in smoke_env.get("PATH", "").split(os.pathsep)
                                       if str(qt).lower() not in p.lower())
    # An offscreen plugin is bundled explicitly for this native smoke test.
    smoke_env["QT_QPA_PLATFORM"] = "offscreen"
    native_plugin = "qwindows.dll" if WINDOWS else "libqxcb.so"
    platforms = list(stage.rglob(native_plugin))
    if len(platforms) != 1:
        raise ValueError(f"Expected one deployed {native_plugin}; found {len(platforms)}")
    platform_folder = platforms[0].parent
    offscreen = "qoffscreen.dll" if WINDOWS else "libqoffscreen.so"
    shutil.copy2(qt / "plugins/platforms" / offscreen, platform_folder / offscreen)
    if not WINDOWS:
        relative = os.path.relpath(stage / "lib", platform_folder)
        run("patchelf", "--set-rpath", "$ORIGIN/" + relative, platform_folder / offscreen)
    run(stage / "bin" / ("sawfish" + SUFFIX), "--version", env=smoke_env)
    # Exercise the exact packaged core, including authenticated gRPC over UDS.
    run(BUILD / ("sawfish_desktop_tests" + SUFFIX), env=os.environ | {"SAWFISH_TEST_BACKEND": str(binary)})
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    shutil.make_archive(str(output / name), "zip" if WINDOWS else "gztar",
                        root_dir=stage.parent, base_dir=name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("metadata", "verify-core", "environment", "dependencies",
                                            "build", "test", "package"))
    parser.add_argument("--rostra", type=Path, default=ROOT / ".ci/rostra")
    parser.add_argument("--require-clean", action="store_true")
    args = parser.parse_args()
    source = args.rostra.resolve()
    if args.command == "verify-core":
        verify_core(source, args.require_clean)
    elif args.command in ("build", "package"):
        {"build": build, "package": package}[args.command](source)
    else:
        {"metadata": metadata, "environment": environment, "dependencies": dependencies,
         "test": test}[args.command]()


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"CI error: {error}", file=sys.stderr)
        sys.exit(1)
