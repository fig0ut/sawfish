#!/usr/bin/env python3
"""Sign and verify Linux release assets using an isolated GPG signing subkey."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time

from package_linux import debian_version

SECRET_VARIABLES = ("LINUX_SIGNING_KEY", "LINUX_SIGNING_PASSPHRASE")


def run(args, env, *, data=None):
    result = subprocess.run([str(arg) for arg in args], env=env, input=data,
                            capture_output=True, timeout=180)
    if result.returncode:
        detail = (result.stderr + result.stdout).decode(errors="replace").strip()
        raise RuntimeError(f"{args[0]} failed: {detail}")
    return result.stdout


def signing_subkey(listing, expected):
    """Accept one expected certificate with an offline primary and a signing subkey."""
    if not re.fullmatch(r"[0-9A-F]{40}", expected):
        raise ValueError("LINUX_SIGNING_FINGERPRINT must be the full primary fingerprint")
    keys = []
    pending = None
    for line in listing.splitlines():
        fields = line.split(":")
        if fields[0] in ("sec", "ssb"):
            pending = fields
        elif fields[0] == "fpr" and pending is not None:
            keys.append((pending, fields[9]))
            pending = None
    primaries = [(fields, fingerprint) for fields, fingerprint in keys if fields[0] == "sec"]
    if len(primaries) != 1 or primaries[0][1] != expected:
        raise ValueError("Imported signing key does not match LINUX_SIGNING_FINGERPRINT")
    primary = primaries[0][0]
    if primary[14] != "#":
        raise ValueError("Export only secret subkeys; the primary private key must remain offline")
    now = int(time.time())
    def usable(fields):
        return fields[1] not in ("r", "e", "d") and (not fields[6] or int(fields[6]) > now)
    if not usable(primary):
        raise ValueError("The release signing certificate is expired, revoked or disabled")
    candidates = [(int(fields[5]), fingerprint) for fields, fingerprint in keys
                  if fields[0] == "ssb" and "s" in fields[11] and fields[14] == "+" and usable(fields)]
    if not candidates:
        raise ValueError("No usable secret signing subkey was imported")
    return max(candidates)[1]


def policy_xml(subkey):
    if not re.fullmatch(r"[0-9A-F]{40}", subkey):
        raise ValueError("Expected a full signing subkey fingerprint")
    # debsig-verify 0.29's optional Selection/id lookup only resolves primary
    # keys. Omit Selection and require a real origin-signature verification
    # against our single-certificate keyring, which validates bound subkeys.
    return f'''<?xml version="1.0"?>
<Policy xmlns="https://www.debian.org/debsig/1.0/">
  <Origin Name="Sawfish" id="{subkey}" Description="Sawfish release signing"/>
  <Verification>
    <Required Type="origin" File="sawfish.pgp"/>
  </Verification>
</Policy>
'''


class Signer:
    def __init__(self, root, fingerprint, key, passphrase):
        self.root = root
        self.env = {key: value for key, value in os.environ.items() if key not in SECRET_VARIABLES}
        self.home = root / "secret"
        self.home.mkdir(mode=0o700)
        self.env["GNUPGHOME"] = str(self.home)
        passfile = self.home / "passphrase"
        passfile.write_text(passphrase + "\n", encoding="utf-8")
        passfile.chmod(0o600)
        # debsigs and rpmsign invoke GPG themselves; keep passphrases out of
        # command lines and logs, and apply loopback settings to that invocation.
        config_path = str(passfile).replace("\\", "\\\\").replace('"', '\\"')
        (self.home / "gpg.conf").write_text(
            f'batch\nyes\npinentry-mode loopback\npassphrase-file "{config_path}"\ndigest-algo SHA256\n',
            encoding="utf-8")
        run(["gpg", "--import"], self.env, data=key.encode())
        listing = run(["gpg", "--with-colons", "--with-fingerprint", "--with-subkey-fingerprint",
                       "--list-secret-keys"], self.env).decode()
        self.subkey = signing_subkey(listing, fingerprint)
        self.fingerprint = fingerprint
        self.public = run(["gpg", "--export-options", "export-minimal", "--export", fingerprint], self.env)
        self.public_armor = run(["gpg", "--armor", "--export-options", "export-minimal", "--export",
                                 fingerprint], self.env)
        self.public_home = root / "public"
        self.public_home.mkdir(mode=0o700)
        self.verify_env = self.env | {"GNUPGHOME": str(self.public_home)}
        run(["gpg", "--batch", "--import"], self.verify_env, data=self.public)
        self.policy_root = root / "policy"
        key_id = self.subkey[-16:]
        policy = self.policy_root / "etc/debsig/policies" / key_id / "sawfish.pol"
        keyring = self.policy_root / "usr/share/debsig/keyrings" / key_id / "sawfish.pgp"
        policy.parent.mkdir(parents=True)
        keyring.parent.mkdir(parents=True)
        policy.write_text(policy_xml(self.subkey), encoding="utf-8")
        keyring.write_bytes(self.public)
        self.rpmdb = root / "rpmdb"
        self.rpmdb.mkdir()
        rpm_key = root / "release-key.asc"
        rpm_key.write_bytes(self.public_armor)
        run(["rpmkeys", "--dbpath", self.rpmdb, "--import", rpm_key], self.verify_env)

    def sign(self, package):
        package = Path(package).resolve()
        if package.suffix == ".deb":
            run(["debsigs", "--sign=origin", "--default-key=" + self.subkey + "!", package], self.env)
        elif package.suffix == ".rpm":
            run(["rpmsign", "--define", "_gpg_name " + self.subkey + "!",
                 "--define", "__gpg " + str(shutil.which("gpg")),
                 "--define", "_gpg_digest_algo sha256", "--addsign", package], self.env)
        else:
            arch = package.name.endswith(".pkg.tar.zst")
            signature = Path(str(package) + (".sig" if arch else ".asc"))
            run(["gpg", "--local-user", self.subkey + "!", "--output", signature,
                 *([] if arch else ["--armor"]), "--detach-sign", package], self.env)
        self.verify(package)

    def verify(self, package):
        package = Path(package).resolve()
        if package.suffix == ".deb":
            run(["debsig-verify", "--root", self.policy_root, package], self.verify_env)
        elif package.suffix == ".rpm":
            output = run(["rpmkeys", "--dbpath", self.rpmdb, "--checksig", "--verbose", package],
                         self.verify_env).decode()
            # rpmkeys can return success for an unsigned package's digests.
            if not re.search(r"Signature.*: OK", output, re.IGNORECASE):
                raise ValueError("RPM has no verified signature")
        else:
            signature = str(package) + (".sig" if package.name.endswith(".pkg.tar.zst") else ".asc")
            output = run(["gpg", "--batch", "--status-fd=1", "--verify", signature, package],
                         self.verify_env).decode()
            valid = [line.split() for line in output.splitlines() if line.startswith("[GNUPG:] VALIDSIG ")]
            if not any(fields[2] == self.subkey and fields[-1] == self.fingerprint for fields in valid):
                raise ValueError("Detached signature does not match the release signing key")

    def export_verification_files(self, output):
        output = Path(output)
        (output / "sawfish-release-key.asc").write_bytes(self.public_armor)
        (output / "sawfish-release-key-fingerprint.txt").write_text(self.fingerprint + "\n", encoding="ascii")
        archive = output / "sawfish-debsig-policy.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for path in sorted(self.policy_root.rglob("*")):
                info = tar.gettarinfo(str(path), str(path.relative_to(self.policy_root)))
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                info.mode = 0o755 if path.is_dir() else 0o644
                if path.is_file():
                    with path.open("rb") as data:
                        tar.addfile(info, data)
                else:
                    tar.addfile(info)
        self.sign(archive)


@contextmanager
def signing_session(fingerprint, key, passphrase):
    with tempfile.TemporaryDirectory(prefix="sawfish-sign-") as temporary:
        root = Path(temporary)
        try:
            yield Signer(root, fingerprint.upper(), key, passphrase)
        finally:
            for name in ("secret", "public"):
                if (root / name).exists():
                    subprocess.run(["gpgconf", "--kill", "all"],
                                   env=os.environ | {"GNUPGHOME": str(root / name)},
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)


def release_files(directory, version):
    debian_version(version)
    packages = [Path(directory) / f"sawfish-{version}-linux-{arch}.{suffix}"
                for arch in ("amd64", "arm64") for suffix in ("deb", "rpm", "pkg.tar.zst", "tar.gz")]
    if any(not package.is_file() or package.is_symlink() for package in packages):
        raise ValueError("Expected all eight Linux release packages before signing")
    return packages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("dist"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    packages = release_files(args.directory, args.version)
    fingerprint = os.environ.get("LINUX_SIGNING_FINGERPRINT", "")
    key = os.environ.get("LINUX_SIGNING_KEY", "")
    if not fingerprint or not key:
        raise ValueError("Set LINUX_SIGNING_FINGERPRINT and LINUX_SIGNING_KEY in the linux-release environment")
    if "LINUX_SIGNING_PASSPHRASE" not in os.environ:
        raise ValueError("Set LINUX_SIGNING_PASSPHRASE in the linux-release environment")
    with signing_session(fingerprint, key, os.environ["LINUX_SIGNING_PASSPHRASE"]) as signer:
        for package in packages:
            print(f"Signing and verifying {package.name}", flush=True)
            signer.sign(package)
        signer.export_verification_files(args.directory)


if __name__ == "__main__":
    main()
