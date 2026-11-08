from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from package_linux import arch_version, debian_version, native_architecture, render_pkgbuild
from sign_linux import policy_xml, release_files, signing_subkey


class PackagingTest(unittest.TestCase):
    def test_prerelease_and_final_versions(self):
        for original, deb, arch in (("0.1.0", "0.1.0", "0.1.0"),
                                    ("0.1.0-alpha.2", "0.1.0~alpha.2", "0.1.0alpha.2"),
                                    ("1.2.3-rc.1+build.4", "1.2.3~rc.1+build.4", "1.2.3rc.1+build.4")):
            with self.subTest(original=original):
                self.assertEqual(debian_version(original), deb)
                self.assertEqual(arch_version(original), arch)

    def test_versions_cannot_inject_recipe_code(self):
        for version in ("v0.1.0", "1.0", "0.1.0;id", "0.1.0\nmalicious=1", "0.1.0-$(id)"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                render_pkgbuild(version)

    def test_arch_recipe_uses_only_local_payload(self):
        recipe = render_pkgbuild("0.1.0-alpha.3")
        self.assertIn("pkgver=0.1.0alpha.3\n", recipe)
        self.assertIn("SAWFISH_PACKAGE_ROOT", recipe)
        self.assertNotIn("releases/download", recipe)
        self.assertNotIn("@", recipe)
        self.assertEqual(native_architecture("linux-arm64"), "aarch64")
        with self.assertRaises(ValueError):
            native_architecture("windows-amd64")

    def test_signing_requires_complete_release(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                release_files(directory, "0.1.0")
            for arch in ("amd64", "arm64"):
                for extension in ("deb", "rpm", "pkg.tar.zst", "tar.gz"):
                    (Path(directory) / f"sawfish-0.1.0-linux-{arch}.{extension}").touch()
            self.assertEqual(len(release_files(directory, "0.1.0")), 8)

    def test_signing_key_is_pinned_and_primary_is_offline(self):
        primary, subkey = "A" * 40, "B" * 40
        def record(kind, capability, secret):
            fields = [kind, "u", "4096", "1", "", "1", "", "", "", "", "", capability, "", "", secret]
            return ":".join(fields) + "\n"
        listing = (record("sec", "cSC", "#") + "fpr:::::::::" + primary + ":\n" +
                   record("ssb", "s", "+") + "fpr:::::::::" + subkey + ":\n")
        self.assertEqual(signing_subkey(listing, primary), subkey)
        for invalid, expected in ((listing, "C" * 40), (listing.replace("#", "+"), primary),
                                  (listing.replace(":s:", ":e:"), primary)):
            with self.assertRaises(ValueError):
                signing_subkey(invalid, expected)
        self.assertIn(subkey[-16:], policy_xml(subkey))


if __name__ == "__main__":
    unittest.main()
