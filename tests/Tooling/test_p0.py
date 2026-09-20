from pathlib import Path
import json
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Build"))
sys.path.insert(0, str(ROOT / "Tools/Editor"))
import p0
import asset_ownership as ownership


class HarnessTests(unittest.TestCase):
    def invoke(self, script):
        return p0.run([sys.executable, "-c", script], timeout=5)

    def test_real_child_success_requires_nonempty_report(self):
        result = self.invoke('print(\'SKI_TEST_RESULT {"total":3,"passed":3,"failed":0}\')')
        self.assertEqual(p0.parse_native_result(result)["total"], 3)

    def test_nonzero_exit_cannot_be_hidden_by_success_report(self):
        result = self.invoke('print(\'SKI_TEST_RESULT {"total":1,"passed":1,"failed":0}\'); raise SystemExit(7)')
        with self.assertRaises(p0.Failed):
            p0.parse_native_result(result)

    def test_empty_missing_failed_duplicate_and_malformed_reports_fail(self):
        for text in ["", 'SKI_TEST_RESULT {"total":0,"passed":0,"failed":0}',
                     'SKI_TEST_RESULT {"total":2,"passed":1,"failed":1}',
                     'SKI_TEST_RESULT {"total":2,"passed":1,"failed":0}',
                     'SKI_TEST_RESULT {"total":true,"passed":true,"failed":0}',
                     'SKI_TEST_RESULT {}', 'SKI_TEST_RESULT []', 'SKI_TEST_RESULT invalid',
                     'SKI_TEST_RESULT {}\nSKI_TEST_RESULT {}']:
            with self.subTest(text=text), self.assertRaises(p0.Failed):
                p0.parse_native_result(subprocess.CompletedProcess([], 0, text))

    def test_crashed_child_fails(self):
        with self.assertRaises(p0.Failed):
            p0.parse_native_result(self.invoke("raise RuntimeError('deliberate crash')"))

    def test_timeout_fails(self):
        started = time.monotonic()
        with self.assertRaisesRegex(p0.Failed, "Timed out"):
            p0.run([sys.executable, "-c", "import time; time.sleep(30)"], timeout=0.1)
        self.assertLess(time.monotonic() - started, 5, "Timeout must not wait for the child's normal exit")

    def test_stderr_does_not_contaminate_source_listing(self):
        result = p0.run([sys.executable, "-c", "import sys; print('source.cpp'); print('warning', file=sys.stderr)"],
                        separate_stderr=True)
        self.assertEqual(result.stdout.strip(), "source.cpp")
        self.assertEqual(result.stderr.strip(), "warning")

    def test_diagnostic_log_survives_child_timeout(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "test-results/p0") as directory:
            log = Path(directory) / "child.log"
            with self.assertRaises(p0.TimedOut):
                p0.run([sys.executable, "-c", "import time; print('started', flush=True); time.sleep(30)"],
                       timeout=0.5, log_path=log)
            self.assertEqual(log.read_text().strip(), "started")

    def test_changed_added_and_removed_sources_invalidate_evidence(self):
        before = {"sha256": "a", "files": {"source": "old", "asset.uasset": "same"}}
        for files in [{"source": "new", "asset.uasset": "same"},
                      {**before["files"], "untracked": "new"}, {"source": "old"}]:
            with self.subTest(files=files), self.assertRaises(p0.Failed):
                p0.verify_frozen(before, {"sha256": "b", "files": files})
        p0.verify_frozen(before, before)

    def test_path_escape_is_rejected(self):
        with self.assertRaises(p0.Failed):
            p0.resolve_inside(ROOT, "../outside")

    def test_asset_generation_cannot_change_project_or_unowned_content(self):
        before = {"files": {"SkiAreaDesignChallenge.uproject": "same"}}
        allowed = {"files": {**before["files"], "Content/P0Generated/Bootstrap.umap": "generated"}}
        p0.verify_asset_changes(before, allowed)
        for relative in ["SkiAreaDesignChallenge.uproject", "Content/Manual.uasset", "AGENTS.md"]:
            with self.subTest(relative=relative), self.assertRaises(p0.Failed):
                p0.verify_asset_changes(before, {"files": {**allowed["files"], relative: "unexpected"}})


class AssetOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p0")
        self.root = Path(self.directory.name)
        self.relative = "Content/P0Generated/M_Bootstrap.uasset"

    def tearDown(self):
        self.directory.cleanup()

    def write_asset(self, value=b"synthetic fixture; not a real Unreal asset"):
        path = self.root / self.relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    def test_unknown_asset_is_not_overwritten(self):
        self.write_asset()
        with self.assertRaisesRegex(RuntimeError, "unowned"):
            ownership.preflight(self.root, "recipe")

    def test_recorded_unchanged_asset_can_be_reused(self):
        receipt = ownership.preflight(self.root, "recipe")
        self.write_asset()
        ownership.record_asset(self.root, receipt, self.relative, {"fixture": True})
        self.assertEqual(ownership.preflight(self.root, "recipe"), receipt)

    def test_manual_edit_and_changed_recipe_are_rejected(self):
        receipt = ownership.preflight(self.root, "recipe")
        self.write_asset()
        ownership.record_asset(self.root, receipt, self.relative, {})
        with self.assertRaisesRegex(RuntimeError, "Recipe changed"):
            ownership.preflight(self.root, "new-recipe")
        self.write_asset(b"manual edit")
        with self.assertRaisesRegex(RuntimeError, "modified"):
            ownership.preflight(self.root, "recipe")

    def test_missing_or_forged_owned_asset_is_rejected(self):
        receipt = ownership.preflight(self.root, "recipe")
        receipt["assets"][self.relative] = {"sha256": "missing", "class": "Material"}
        target = self.root / ownership.RECEIPT
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(receipt), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "disappeared"):
            ownership.preflight(self.root, "recipe")

    def test_cannot_claim_an_unowned_path(self):
        with self.assertRaises(RuntimeError):
            ownership.record_asset(self.root, {"assets": {}}, "../user.asset", {})


class PackageEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p0")
        self.root = Path(self.directory.name)
        self.launcher = self.root / "Windows/SkiAreaDesignChallenge.exe"
        self.launcher.parent.mkdir()
        self.launcher.write_bytes(b"synthetic package fixture; never executed")
        self.content = self.launcher.parent / "Content.pak"
        self.content.write_bytes(b"synthetic cooked content")
        self.receipt = {"command": "package", "configuration": "Shipping", "source_before": "source",
                        "result": {"status": "PASS", "directory": str(self.root),
                                   "launcher": str(self.launcher), "manifest": p0.package_manifest(self.root)}}

    def tearDown(self):
        self.directory.cleanup()

    def test_package_requires_matching_source_configuration_and_success(self):
        self.assertEqual(p0.verify_package_receipt(self.receipt, "source", "Shipping"), self.launcher)
        for source, configuration in [("changed", "Shipping"), ("source", "Development")]:
            with self.subTest(source=source, configuration=configuration), self.assertRaises(p0.Failed):
                p0.verify_package_receipt(self.receipt, source, configuration)
        self.receipt["result"]["status"] = "FAIL"
        with self.assertRaises(p0.Failed):
            p0.verify_package_receipt(self.receipt, "source", "Shipping")

    def test_changed_cooked_content_invalidates_unchanged_launcher(self):
        self.content.write_bytes(b"changed cooked content")
        with self.assertRaisesRegex(p0.Failed, "files changed"):
            p0.verify_package_receipt(self.receipt, "source", "Shipping")

    def test_added_binary_invalidates_package_but_runtime_logs_do_not(self):
        saved = self.launcher.parent / "Saved/Logs"
        saved.mkdir(parents=True)
        (saved / "runtime.log").write_text("runtime output")
        p0.verify_package_receipt(self.receipt, "source", "Shipping")
        (self.launcher.parent / "unexpected.dll").write_bytes(b"extra library")
        with self.assertRaisesRegex(p0.Failed, "files changed"):
            p0.verify_package_receipt(self.receipt, "source", "Shipping")

    def test_missing_launcher_and_empty_output_are_rejected(self):
        self.launcher.unlink()
        with self.assertRaisesRegex(p0.Failed, "launcher is missing"):
            p0.verify_package_receipt(self.receipt, "source", "Shipping")
        self.content.unlink()
        with self.assertRaisesRegex(p0.Failed, "empty"):
            p0.package_manifest(self.root)


if __name__ == "__main__":
    unittest.main()
