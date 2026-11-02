import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from package_linux import debian_version
from release_arch import render_pkgbuild


def published_release():
    version = "0.1.0-alpha.2"
    assets = []
    for arch, digit in (("amd64", "a"), ("arm64", "b")):
        name = f"sawfish-{version}-linux-{arch}.tar.gz"
        assets.append({"name": name, "state": "uploaded", "digest": "sha256:" + digit * 64,
                       "browser_download_url":
                           f"https://github.com/fig0ut/sawfish/releases/download/v{version}/{name}"})
    return {"tag_name": "v" + version, "draft": False, "assets": assets}


class PackagingTest(unittest.TestCase):
    def test_prerelease_and_final_versions(self):
        for original, expected in (("0.1.0", "0.1.0"),
                                   ("0.1.0-alpha.2", "0.1.0~alpha.2"),
                                   ("1.2.3-rc.1+build.4", "1.2.3~rc.1+build.4")):
            with self.subTest(original=original):
                self.assertEqual(debian_version(original), expected)

    def test_versions_cannot_inject_recipe_code(self):
        for version in ("v0.1.0", "1.0", "0.1.0;id", "0.1.0\nmalicious=1", "0.1.0-$(id)"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                debian_version(version)

    def test_arch_uses_both_github_digests(self):
        recipe = render_pkgbuild(published_release(), "fig0ut/sawfish")
        self.assertIn("pkgver=0.1.0alpha.2\n", recipe)
        self.assertIn("sha256sums_x86_64=('" + "a" * 64 + "')", recipe)
        self.assertIn("sha256sums_aarch64=('" + "b" * 64 + "')", recipe)
        self.assertNotIn("@", recipe)
        self.assertNotIn("SKIP", recipe)

    def test_arch_rejects_incomplete_or_untrusted_assets(self):
        original = published_release()
        for changes in ({"digest": None}, {"digest": "sha256:bad"},
                        {"state": "new"}, {"browser_download_url": "https://example.org/file"}):
            release = copy.deepcopy(original)
            release["assets"][0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                render_pkgbuild(release, "fig0ut/sawfish")
        for assets in (original["assets"][:1], original["assets"] + original["assets"][:1]):
            with self.assertRaises(ValueError):
                render_pkgbuild(original | {"assets": assets}, "fig0ut/sawfish")

    def test_arch_accepts_encoded_build_metadata(self):
        release = published_release()
        release["tag_name"] += "+build.4"
        for asset in release["assets"]:
            asset["name"] = asset["name"].replace("alpha.2", "alpha.2+build.4")
            asset["browser_download_url"] = asset["browser_download_url"].replace(
                "alpha.2", "alpha.2%2Bbuild.4")
        recipe = render_pkgbuild(release, "fig0ut/sawfish")
        self.assertIn("pkgver=0.1.0alpha.2+build.4\n", recipe)
        self.assertIn("alpha.2%2Bbuild.4", recipe)

    def test_arch_rejects_drafts_and_bad_tags(self):
        for changes in ({"draft": True}, {"tag_name": "main"},
                        {"tag_name": "v0.1.0-';id;#"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                render_pkgbuild(published_release() | changes, "fig0ut/sawfish")


if __name__ == "__main__":
    unittest.main()
