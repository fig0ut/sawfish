"""Real package signing checks; requires SAWFISH_TEST_PACKAGES and Linux signing tools."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from sign_linux import run, signing_session


class SigningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="sawfish-signing-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        master = cls.root / "master"
        master.mkdir(mode=0o700)
        env = os.environ | {"GNUPGHOME": str(master)}
        cls.addClassCleanup(lambda: subprocess.run(["gpgconf", "--kill", "all"], env=env,
                                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        password = "temporary test passphrase"
        gpg = ["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase-fd", "0"]
        run(gpg + ["--quick-generate-key", "Sawfish CI <ci@example.invalid>", "rsa2048", "cert", "1d"],
            env, data=password.encode())
        listing = run(["gpg", "--with-colons", "--list-keys"], env).decode()
        fingerprint = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
        run(gpg + ["--quick-add-key", fingerprint, "rsa2048", "sign", "1d"], env, data=password.encode())
        secret = run(gpg + ["--armor", "--export-secret-subkeys", fingerprint],
                     env, data=password.encode()).decode()
        session = signing_session(fingerprint, secret, password)
        cls.signer = session.__enter__()
        cls.addClassCleanup(lambda: session.__exit__(None, None, None))
        source = Path(os.environ["SAWFISH_TEST_PACKAGES"])
        cls.originals, cls.signed = {}, {}
        for extension in ("deb", "rpm", "pkg.tar.zst", "tar.gz"):
            files = list(source.glob(f"sawfish-*-linux-amd64.{extension}"))
            if len(files) != 1:
                raise ValueError(f"Expected one amd64 .{extension} test package in {source}")
            cls.originals[extension] = files[0]
            target = cls.root / files[0].name
            shutil.copy2(files[0], target)
            cls.signer.sign(target)
            cls.signed[extension] = target
        cls.signer.export_verification_files(cls.root)

    def test_all_formats_verify_using_public_key_only(self):
        self.assertEqual(run(["gpg", "--with-colons", "--list-secret-keys"], self.signer.verify_env), b"")
        for package in self.signed.values():
            with self.subTest(package=package.name):
                self.signer.verify(package)

    def test_unsigned_packages_are_rejected(self):
        for package in self.originals.values():
            with self.subTest(package=package.name), self.assertRaises((RuntimeError, ValueError)):
                self.signer.verify(package)

    def test_modified_packages_are_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            for extension, original in self.signed.items():
                target = Path(temporary) / original.name
                shutil.copy2(original, target)
                if extension == "deb":
                    members = run(["ar", "t", target], self.signer.verify_env).decode().splitlines()
                    data_name = next(name for name in members if name.startswith("data.tar"))
                    data = bytearray(run(["ar", "p", target, data_name], self.signer.verify_env))
                    data[len(data) // 2] ^= 1
                    payload = Path(temporary) / data_name
                    payload.write_bytes(data)
                    run(["ar", "r", target, payload], self.signer.verify_env)
                else:
                    with target.open("r+b") as stream:
                        stream.seek(-1, 2)
                        value = stream.read(1)
                        stream.seek(-1, 2)
                        stream.write(bytes([value[0] ^ 1]))
                    if extension != "rpm":
                        suffix = ".sig" if extension == "pkg.tar.zst" else ".asc"
                        shutil.copy2(str(original) + suffix, str(target) + suffix)
                with self.subTest(extension=extension), self.assertRaises((RuntimeError, ValueError)):
                    self.signer.verify(target)

    def test_deb_policy_is_required(self):
        with tempfile.TemporaryDirectory(dir=self.root) as empty:
            with self.assertRaises(RuntimeError):
                run(["debsig-verify", "--root", empty, self.signed["deb"]], self.signer.verify_env)

    def test_deb_policy_rejects_an_untrusted_certificate(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            root = Path(temporary)
            home = root / "gpg"
            home.mkdir(mode=0o700)
            env = self.signer.verify_env | {"GNUPGHOME": str(home)}
            try:
                run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                     "--quick-generate-key", "Unrelated CI <other@example.invalid>",
                     "rsa2048", "cert", "1d"], env)
                public = run(["gpg", "--export"], env)
                policy = root / "policy"
                shutil.copytree(self.signer.policy_root, policy)
                keyring = next(policy.rglob("sawfish.pgp"))
                keyring.write_bytes(public)
                with self.assertRaises(RuntimeError):
                    run(["debsig-verify", "--root", policy, self.signed["deb"]], self.signer.verify_env)
            finally:
                subprocess.run(["gpgconf", "--kill", "all"], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def test_verification_bundle_has_only_public_policy_and_keys(self):
        archive = self.root / "sawfish-debsig-policy.tar.gz"
        self.signer.verify(archive)
        with tarfile.open(archive) as tar:
            files = [member for member in tar if member.isfile()]
            self.assertEqual(len(files), 2)
            self.assertTrue(any(member.name.endswith("sawfish.pol") for member in files))
            self.assertTrue(any(member.name.endswith("sawfish.pgp") for member in files))
            self.assertTrue(all(member.uid == 0 and member.mode == 0o644 for member in files))


if __name__ == "__main__":
    unittest.main()
