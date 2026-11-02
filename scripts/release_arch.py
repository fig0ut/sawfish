#!/usr/bin/env python3
"""Generate a binary Arch recipe after both Linux release assets are uploaded."""

import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import quote, unquote

from package_linux import debian_version

ROOT = Path(__file__).resolve().parents[1]


def render_pkgbuild(release, repository):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Expected owner/repository")
    tag = release.get("tag_name", "")
    if not tag.startswith("v") or release.get("draft", False):
        raise ValueError("Expected a published vMAJOR.MINOR.PATCH release")
    version = tag[1:]
    debian_version(version)  # Validate before inserting into shell source.
    values = {"REPOSITORY": repository, "VERSION": version,
              "PKGVER": version.replace("-", "", 1).replace("-", "_")}
    for architecture in ("amd64", "arm64"):
        name = f"sawfish-{version}-linux-{architecture}.tar.gz"
        assets = [a for a in release.get("assets", []) if a.get("name") == name]
        if len(assets) != 1 or assets[0].get("state") != "uploaded":
            raise ValueError(f"Missing uploaded release asset: {name}")
        asset = assets[0]
        digest = asset.get("digest") or ""
        if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
            raise ValueError(f"GitHub has not supplied a SHA-256 digest for {name}")
        url = f"https://github.com/{repository}/releases/download/{tag}/{name}"
        if unquote(asset.get("browser_download_url", "")) != url:
            raise ValueError(f"Unexpected download URL for {name}")
        values[architecture.upper() + "_URL"] = (
            f"https://github.com/{repository}/releases/download/{quote(tag, safe='')}/{quote(name, safe='')}")
        values[architecture.upper() + "_SHA256"] = digest[7:].lower()
    recipe = (ROOT / "packaging/arch/PKGBUILD.in").read_text(encoding="utf-8")
    for key, value in values.items():
        recipe = recipe.replace("@" + key + "@", value)
    return recipe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default="fig0ut/sawfish")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/PKGBUILD")
    parser.add_argument("--release-json", type=Path, help="Use saved release metadata offline")
    args = parser.parse_args()
    if args.release_json:
        release = json.loads(args.release_json.read_text())
    else:
        release = json.loads(subprocess.run(
            ["gh", "api", f"repos/{args.repository}/releases/tags/{args.tag}"],
            capture_output=True, text=True, check=True, timeout=30).stdout)
    if release.get("tag_name") != args.tag:
        raise ValueError("Release metadata does not match the requested tag")
    recipe = render_pkgbuild(release, args.repository)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(recipe, encoding="utf-8")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
