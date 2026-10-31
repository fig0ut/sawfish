#!/usr/bin/env python3
"""Pinned gRPC/Protobuf build recipe; CI hashes this file for its dependency cache."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GRPC_PREFIX = ROOT / ".deps/ci-grpc"
GRPC_COMMIT = "3252a89f10d8e92997862167ca7d095ecda85973"  # v1.84.0
GRPC_VERSION = "1.84.0"


def run(*args, cwd=ROOT):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


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


if __name__ == "__main__":
    dependencies()
