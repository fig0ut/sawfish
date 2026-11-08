"""Check core compatibility independently of its source commit."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ci


class CoreCompatibilityTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "rostra"
        self.source.mkdir()
        self.enterContext(patch.object(ci, "ROOT", self.root))
        self.enterContext(patch.dict(os.environ, {"SAWFISH_ROSTRA_COMMIT": "", "GITHUB_OUTPUT": ""}))
        (self.root / "proto").mkdir()
        (self.root / "proto/REVISION").write_text(
            "repository=https://github.com/fig0ut/rostra\napi_version=4\n", encoding="utf-8")
        for proto in ci.PROTOS:
            for base in (self.root / "proto", self.source):
                path = base / proto
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('syntax = "proto3";\n', encoding="utf-8")
        (self.source / "experimental/boxdd/cmd_version.go").write_text("package main\n", encoding="utf-8")
        self.git("init", "--template=", "-q")
        self.commit()

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false",
             "-c", "user.name=Sawfish tests", "-c", "user.email=ci@example.invalid", *args],
            cwd=self.source, text=True).strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "Test core revision")
        return self.git("rev-parse", "HEAD")

    def test_unrelated_commits_do_not_require_a_protocol_revision_change(self):
        previous = ci.verify_core(self.source, require_clean=True)
        (self.source / "README.md").write_text("Unrelated change\n", encoding="utf-8")
        current = self.commit()
        self.assertNotEqual(previous, current)
        self.assertEqual(ci.verify_core(self.source, require_clean=True), current)

    def test_ci_uses_the_commit_resolved_for_the_run(self):
        expected = ci.verify_core(self.source)
        with patch.dict(os.environ, {"SAWFISH_ROSTRA_COMMIT": expected}):
            self.assertEqual(ci.verify_core(self.source), expected)
            (self.source / "README.md").write_text("Branch advanced\n", encoding="utf-8")
            self.commit()
            with self.assertRaisesRegex(ValueError, "commit resolved"):
                ci.verify_core(self.source)

    def test_local_changes_are_allowed_but_ci_requires_clean_source(self):
        (self.source / "local.txt").write_text("Local changes\n", encoding="utf-8")
        self.assertEqual(ci.verify_core(self.source), self.git("rev-parse", "HEAD"))
        with self.assertRaisesRegex(ValueError, "clean Rostra checkout"):
            ci.verify_core(self.source, require_clean=True)

    def test_protocol_changes_still_require_review(self):
        (self.source / ci.PROTOS[0]).write_text("Changed protocol\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Protocol definition differs"):
            ci.verify_core(self.source)

    def test_required_core_command_is_checked(self):
        (self.source / "experimental/boxdd/cmd_version.go").unlink()
        with self.assertRaisesRegex(ValueError, "version command"):
            ci.verify_core(self.source)

    def test_api_version_is_checked_in_metadata_and_binary(self):
        revision = self.root / "proto/REVISION"
        revision.write_text(revision.read_text().replace("api_version=4", "api_version=5"),
                            encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "client API version"):
            ci.verify_core(self.source)
        for version, accepted in ((4, True), (5, False)):
            with self.subTest(version=version), patch.object(ci, "run", return_value=json.dumps(
                    {"version": "new-core", "api_version": version})):
                if accepted:
                    self.assertEqual(ci.core_info(Path("rostra-daemon"))["api_version"], version)
                else:
                    with self.assertRaisesRegex(ValueError, "version/API contract"):
                        ci.core_info(Path("rostra-daemon"))

    def test_metadata_no_longer_requires_a_core_commit(self):
        output = self.root / "github-output"
        with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output), "SAWFISH_RELEASE_TAG": "v0.1.0",
                                     "GITHUB_EVENT_NAME": "release"}):
            ci.metadata()
        self.assertEqual(output.read_text().splitlines(),
                         ["rostra_repository=fig0ut/rostra", "version=0.1.0", "package_checks=true"])

    def package_checks(self, event_name, payload):
        event = self.root / "event.json"
        event.write_text(json.dumps(payload), encoding="utf-8")
        with patch.object(ci, "ROOT", self.source), patch.dict(os.environ, {
                "GITHUB_EVENT_NAME": event_name, "GITHUB_EVENT_PATH": str(event)}):
            return ci.package_checks_required()

    def test_application_changes_keep_only_regular_checks(self):
        base = self.git("rev-parse", "HEAD")
        (self.source / "main.cpp").write_text("Application change\n", encoding="utf-8")
        (self.source / "README.md").write_text("Documentation change\n", encoding="utf-8")
        self.commit()
        self.assertFalse(self.package_checks("push", {"before": base}))
        self.assertFalse(self.package_checks("pull_request", {"pull_request": {"base": {"sha": base}}}))

    def test_package_checks_consider_the_whole_push(self):
        base = self.git("rev-parse", "HEAD")
        (self.source / "CMakeLists.txt").write_text("Dependency change\n", encoding="utf-8")
        self.commit()
        (self.source / "README.md").write_text("Later documentation change\n", encoding="utf-8")
        self.commit()
        self.assertTrue(self.package_checks("push", {"before": base}))

    def test_signer_changes_on_pull_requests_get_package_checks(self):
        base = self.git("rev-parse", "HEAD")
        (self.source / "scripts").mkdir()
        (self.source / "scripts/sign_linux.py").write_text("Signing change\n", encoding="utf-8")
        self.commit()
        self.assertTrue(self.package_checks("pull_request", {"pull_request": {"base": {"sha": base}}}))

    def test_moving_a_packaging_file_still_gets_package_checks(self):
        (self.source / "packaging").mkdir()
        (self.source / "packaging/recipe").write_text("Recipe\n", encoding="utf-8")
        base = self.commit()
        self.git("mv", "packaging/recipe", "old-recipe.txt")
        self.commit()
        self.assertTrue(self.package_checks("push", {"before": base}))

    def test_release_manual_and_unknown_changes_always_get_package_checks(self):
        for event, payload in (("release", {}), ("workflow_dispatch", {}), ("push", {}),
                               ("push", {"before": "0" * 40}),
                               ("push", {"before": "--invalid-revision"}),
                               ("push", {"before": "a" * 40})):
            with self.subTest(event=event, payload=payload):
                self.assertTrue(self.package_checks(event, payload))


if __name__ == "__main__":
    unittest.main()
