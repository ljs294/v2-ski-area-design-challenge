import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Build"))
import p1


class ExactAutomationEvidenceTests(unittest.TestCase):
    expected = ("MountainPlanner.P1.Example.A", "MountainPlanner.P1.Example.B")
    test_filter = "MountainPlanner.P1.Example"

    def log(self, results=("Success", "Success"), completed=None, found=2):
        paths = list(completed or self.expected)
        lines = [f"Found {found} automation tests based on '{self.test_filter}'"]
        lines.extend(f"Test Started. Name={{case}} Path={{{path}}}" for path in self.expected)
        lines.extend(f"Test Completed. Result={{{result}}} Name={{case}} Path={{{path}}}"
                     for result, path in zip(results, paths))
        lines.append("Automation Test Queue Empty")
        return "\n".join(lines)

    def test_exact_success_set_passes(self):
        p1.require_exact_automation(self.log(), self.expected, self.test_filter)

    def test_failure_missing_unexpected_duplicate_and_count_mismatch_fail(self):
        cases = [
            self.log(results=("Fail", "Success")),
            self.log(completed=(self.expected[0],)),
            self.log(completed=(self.expected[0], "MountainPlanner.P1.Example.Other")),
            self.log(completed=(self.expected[0], self.expected[0])),
            self.log(found=3),
            self.log().replace("Automation Test Queue Empty", ""),
        ]
        for value in cases:
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                p1.require_exact_automation(value, self.expected, self.test_filter)


class ReleaseResolverTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.release = Path(self.temporary.name)
        self.digest = "a" * 64
        self.artifact = "MountainPlanner-P1-Windows-aaaaaaaa-20260922T120000"
        self.artifact_root = self.release / self.artifact
        self.artifact_root.mkdir()
        self.launcher = self.artifact_root / "SkiAreaDesignChallenge.exe"
        self.launcher.write_bytes(b"validated launcher")
        (self.release / f"{self.artifact}.zip").write_bytes(b"validated zip")
        self.latest = self.release / "MountainPlanner-P1-LATEST.json"
        self.write_handoff()

    def tearDown(self):
        self.temporary.cleanup()

    def write_handoff(self, **overrides):
        receipt = {
            "Status": "PASS",
            "Artifact": self.artifact,
            "SourceDigest": self.digest,
            "PackageInvocation": "20260922T120000.000000Z-example",
            "LauncherSha256": hashlib.sha256(b"validated launcher").hexdigest(),
            "ZipSha256": hashlib.sha256(b"validated zip").hexdigest(),
        }
        receipt.update(overrides)
        self.latest.write_text(json.dumps(receipt), encoding="utf-8")

    def resolve(self):
        return subprocess.run([
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(ROOT / "Tools/Build/resolve_p1_release.ps1"),
            "-ReleaseBase", str(self.release),
        ], capture_output=True, text=True, timeout=30)

    def test_valid_handoff_resolves_exact_launcher(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()), self.launcher)

    def test_traversal_invalid_marker_hash_and_invalid_zip_fail(self):
        self.write_handoff(Artifact="..\\outside")
        self.assertNotEqual(self.resolve().returncode, 0)

        self.write_handoff()
        marker = self.artifact_root / "DO NOT USE - SOURCE CHANGED.txt"
        marker.write_text("invalid", encoding="utf-8")
        self.assertNotEqual(self.resolve().returncode, 0)
        marker.unlink()

        self.write_handoff(LauncherSha256="0" * 64)
        self.assertNotEqual(self.resolve().returncode, 0)

        self.write_handoff()
        invalid_zip = self.release / f"{self.artifact}.zip.INVALID"
        invalid_zip.write_bytes(b"invalid")
        self.assertNotEqual(self.resolve().returncode, 0)


if __name__ == "__main__":
    unittest.main()
