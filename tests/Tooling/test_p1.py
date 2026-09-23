import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Build"))
import p1
import evidence_ledger


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


class ExactCTestEvidenceTests(unittest.TestCase):
    expected = ("SkiDomain.Revision", "SkiDomain.Terrain", "SkiDomain.TerrainCore")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.executables = []
        tests = []
        for index, name in enumerate(self.expected):
            executable = self.root / f"test-{index}.exe"
            executable.write_bytes(f"fixture-{name}".encode())
            self.executables.append(executable)
            tests.append({"name": name, "command": [str(executable)]})
        self.discovery = json.dumps({"tests": tests})
        self.junit = self.root / "results.xml"
        self.write_junit(self.expected)

    def tearDown(self):
        self.temporary.cleanup()

    def write_junit(self, names, failure=None):
        cases = []
        for name in names:
            body = "<failure/>" if name == failure else ""
            cases.append(f'<testcase name="{name}">{body}</testcase>')
        self.junit.write_text("<testsuite>" + "".join(cases) + "</testsuite>", encoding="utf-8")

    def test_exact_discovery_and_machine_results_bind_executables(self):
        receipts = p1.require_exact_ctest(self.discovery, self.junit, self.expected)
        self.assertEqual([item["name"] for item in receipts], list(self.expected))
        self.assertTrue(all(item["result"] == "PASS" for item in receipts))

    def test_missing_unexpected_failed_and_unbound_ctest_evidence_fail(self):
        cases = []
        cases.append(json.dumps({"tests": json.loads(self.discovery)["tests"][:-1]}))
        unexpected = json.loads(self.discovery)
        unexpected["tests"][-1]["name"] = "SkiDomain.Other"
        cases.append(json.dumps(unexpected))
        for discovery in cases:
            with self.subTest(discovery=discovery), self.assertRaises(RuntimeError):
                p1.require_exact_ctest(discovery, self.junit, self.expected)
        self.write_junit(self.expected, failure=self.expected[1])
        with self.assertRaises(RuntimeError):
            p1.require_exact_ctest(self.discovery, self.junit, self.expected)
        self.write_junit(self.expected)
        self.executables[0].unlink()
        with self.assertRaisesRegex(RuntimeError, "not bound"):
            p1.require_exact_ctest(self.discovery, self.junit, self.expected)


class PackageReceiptEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.package = self.root / "package"
        self.launcher = self.package / "Windows/SkiAreaDesignChallenge.exe"
        self.launcher.parent.mkdir(parents=True)
        self.launcher.write_bytes(b"launcher")
        self.digest = "a" * 64
        self.receipt_path = self.output / "package-Development.json"
        self.write_receipt()

    def tearDown(self):
        self.temporary.cleanup()

    def write_receipt(self, **overrides):
        receipt = {"invocation": "fixture", "command": "package",
                   "configuration": "Development", "source_before": self.digest,
                   "result": {"status": "PASS", "directory": str(self.package),
                              "launcher": str(self.launcher),
                              "launcher_sha256": hashlib.sha256(b"launcher").hexdigest(),
                              "manifest": p1.p0.package_manifest(self.package)}}
        receipt.update(overrides)
        self.receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

    def verify(self):
        with mock.patch.object(p1, "OUTPUT", self.output):
            return p1.verify_package_report("Development", self.digest)

    def test_matching_package_receipt_passes(self):
        _, launcher = self.verify()
        self.assertEqual(launcher, self.launcher.resolve())

    def test_missing_stale_and_manifest_mismatch_receipts_fail(self):
        self.receipt_path.unlink()
        with self.assertRaisesRegex(RuntimeError, "Package Development"):
            self.verify()
        self.write_receipt(source_before="b" * 64)
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.verify()
        self.write_receipt()
        (self.launcher.parent / "late.dll").write_bytes(b"late")
        with self.assertRaisesRegex(RuntimeError, "files changed"):
            self.verify()

    def test_shipping_target_proof_mismatch_fails(self):
        receipt = json.loads(self.receipt_path.read_text(encoding="utf-8"))
        receipt["configuration"] = "Shipping"
        receipt["result"]["shipping_mcp_proof"] = {"target_receipt_sha256": "old"}
        shipping_path = self.output / "package-Shipping.json"
        shipping_path.write_text(json.dumps(receipt), encoding="utf-8")
        with mock.patch.object(p1, "OUTPUT", self.output), \
                mock.patch.object(p1, "shipping_target_module_proof",
                                  return_value={"target_receipt_sha256": "current"}), \
                self.assertRaisesRegex(RuntimeError, "missing, stale, or mismatched"):
            p1.verify_package_report("Shipping", self.digest)


class ShippingMcpEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.target = self.root / "Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.target"
        self.target.parent.mkdir(parents=True)
        self.rules = self.root / "Source/SkiAreaDesignChallenge.Target.cs"
        self.rules.parent.mkdir(parents=True)
        self.rules.write_text('Type = TargetType.Game; ExtraModuleNames.Add("SkiPresentation");', encoding="utf-8")
        self.write_target([])

    def tearDown(self):
        self.temporary.cleanup()

    def write_target(self, plugins, **overrides):
        value = {"TargetName": "SkiAreaDesignChallenge", "Platform": "Win64",
                 "Configuration": "Shipping", "TargetType": "Game",
                 "IsTestTarget": False, "BuildPlugins": plugins}
        value.update(overrides)
        self.target.write_text(json.dumps(value), encoding="utf-8")

    def test_shipping_game_target_and_module_proof_passes(self):
        self.assertEqual(p1.shipping_target_module_proof(self.root)["root_module"], "SkiPresentation")

    def test_editor_plugin_and_target_mismatch_fail(self):
        self.write_target(["ModelContextProtocol"])
        with self.assertRaisesRegex(RuntimeError, "entered"):
            p1.shipping_target_module_proof(self.root)
        self.write_target([], TargetType="Editor")
        with self.assertRaisesRegex(RuntimeError, "Shipping Game"):
            p1.shipping_target_module_proof(self.root)


class ProcessNetworkAuditTests(unittest.TestCase):
    valid = {"method": "GetExtendedTcpTable process-attributed polling", "snapshots": 10,
             "listen_ports": [], "remote_endpoints": []}

    def test_observed_quiet_process_passes(self):
        p1.validate_tcp_audit(self.valid, require_no_connections=True)

    def test_missing_listener_and_offline_connection_evidence_fail_closed(self):
        cases = [
            ({**self.valid, "snapshots": 0}, True),
            ({**self.valid, "listen_ports": [8000]}, False),
            ({**self.valid, "remote_endpoints": ["203.0.113.1:443"]}, True),
        ]
        for audit, offline in cases:
            with self.subTest(audit=audit), self.assertRaises(RuntimeError):
                p1.validate_tcp_audit(audit, require_no_connections=offline)


class TerrainCoreEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        for name in ("TerrainCore", "TerrainEdits"):
            directory = self.root / name
            directory.mkdir()
            (directory / "manifest.json").write_text(f"{name}-fixture")

    def tearDown(self):
        self.temporary.cleanup()

    def test_full_contract_tree_hashes_are_exact_and_deterministic(self):
        first = p1.terraincore_contract_trees(self.root)
        second = p1.terraincore_contract_trees(self.root)
        self.assertEqual(first, second)
        (self.root / "TerrainEdits/manifest.json").write_text("changed")
        self.assertNotEqual(first, p1.terraincore_contract_trees(self.root))

    def test_missing_and_empty_contract_trees_fail(self):
        (self.root / "TerrainEdits/manifest.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "empty"):
            p1.terraincore_contract_trees(self.root)
        (self.root / "TerrainEdits").rmdir()
        with self.assertRaisesRegex(RuntimeError, "missing"):
            p1.terraincore_contract_trees(self.root)

    def test_renderer_receipt_is_fail_closed(self):
        valid = {"rendererPath": True, "renderedTileCount": 1,
                 "revisionAligned": True, "actorPicked": True,
                 "actorMutationObserved": True,
                 "actorStaleMeshRejected": True,
                 "syntheticGuestMarkers": 3000, "overlaySegments": 1}
        p1.require_terraincore_renderer(valid, "fixture")
        for key, value in (("rendererPath", False), ("renderedTileCount", 0),
                           ("revisionAligned", False), ("actorPicked", False),
                           ("actorMutationObserved", False),
                           ("actorStaleMeshRejected", False),
                           ("syntheticGuestMarkers", 2999), ("overlaySegments", 0),
                           ("overlaySegments", 1.0)):
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                p1.require_terraincore_renderer({**valid, key: value}, "fixture")

    def test_acquisition_port_guard_requires_installed_guard_and_exact_zero(self):
        p1.require_acquisition_port_guard(
            {"acquisitionPortGuardInstalled": True, "acquisitionTransportCalls": 0})
        for receipt in (
            {"acquisitionPortGuardInstalled": False, "acquisitionTransportCalls": 0},
            {"acquisitionPortGuardInstalled": True, "acquisitionTransportCalls": 1},
            {"acquisitionPortGuardInstalled": True, "acquisitionTransportCalls": False},
            {"acquisitionPortGuardInstalled": True},
        ):
            with self.subTest(receipt=receipt), self.assertRaises(RuntimeError):
                p1.require_acquisition_port_guard(receipt)


class EvidenceLedgerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.runs = self.root / "runs"
        self.runs.mkdir()
        self.source_files = {"Source/example.cpp": hashlib.sha256(b"source").hexdigest()}
        self.digest = hashlib.sha256(
            json.dumps(self.source_files, sort_keys=True).encode()).hexdigest()
        self.invocation = "20260922T120000.000000Z-1234abcd"
        self.run = self.runs / self.invocation
        self.run.mkdir()
        self.receipt_path = self.root / "terraincore.json"
        self.executables = []
        native_receipts = []
        discovery_tests = []
        for index, name in enumerate(evidence_ledger.TERRAINCORE_CTESTS):
            executable = self.root / f"native-{index}.exe"
            executable.write_bytes(name.encode())
            self.executables.append(executable)
            digest = hashlib.sha256(executable.read_bytes()).hexdigest()
            native_receipts.append({"name": name, "result": "PASS",
                                    "executable": str(executable.resolve()),
                                    "executable_sha256": digest})
            discovery_tests.append({"name": name, "command": [str(executable.resolve())]})
        self.receipt = {
            "invocation": self.invocation,
            "run_output": str(self.run),
            "command": "terraincore",
            "source_before": self.digest,
            "result": {"status": "PASS",
                       "native_tests": list(evidence_ledger.TERRAINCORE_CTESTS),
                       "native_receipts": native_receipts},
        }
        self.write_receipt_and_report()
        (self.run / "source-manifest.json").write_text(
            json.dumps({"sha256": self.digest, "files": self.source_files}), encoding="utf-8")
        (self.run / "terraincore-ctest-discovery.json").write_text(
            json.dumps({"tests": discovery_tests}), encoding="utf-8")
        (self.run / "terraincore-ctest-results.xml").write_text(
            "<testsuite>" + "".join(
                f'<testcase name="{name}"/>' for name in evidence_ledger.TERRAINCORE_CTESTS)
            + "</testsuite>", encoding="utf-8")
        (self.run / "terraincore-ctest-run.log").write_text(
            "\n".join(evidence_ledger.TERRAINCORE_CTESTS)
            + f"\n100% tests passed, 0 tests failed out of {len(evidence_ledger.TERRAINCORE_CTESTS)}",
            encoding="utf-8")
        (self.run / "retained.bin").write_bytes(b"all retained evidence is hashed")

    def tearDown(self):
        self.temporary.cleanup()

    def write_receipt_and_report(self):
        serialized = json.dumps(self.receipt, indent=2) + "\n"
        self.receipt_path.write_text(serialized, encoding="utf-8")
        (self.run / "report.json").write_text(serialized, encoding="utf-8")

    def collect(self):
        return evidence_ledger.collect_evidence(
            ROOT, self.runs, self.digest, self.invocation,
            [("editor-terraincore", self.receipt_path)],
            {"editor-terraincore": ("terraincore-ctest-results.xml",)})

    def test_exact_run_binding_hashes_every_retained_artifact(self):
        ledger = self.collect()
        self.assertEqual(ledger["schemaVersion"], 2)
        retained = (self.run / "retained.bin").relative_to(ROOT).as_posix()
        self.assertEqual(
            ledger["files"][retained]["sha256"],
            hashlib.sha256(b"all retained evidence is hashed").hexdigest())
        self.assertIn(retained, ledger["receipts"]["editor-terraincore"]["files"])

    def test_mismatched_invocation_report_source_and_required_artifact_fail(self):
        other = self.runs / "20260922T120001.000000Z-1234abcd"
        other.mkdir()
        self.receipt["run_output"] = str(other)
        self.write_receipt_and_report()
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.collect()

        self.receipt["run_output"] = str(self.run)
        self.write_receipt_and_report()
        (self.run / "report.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "exact retained run report"):
            self.collect()

        self.write_receipt_and_report()
        (self.run / "source-manifest.json").write_text(
            json.dumps({"sha256": "b" * 64, "files": self.source_files}), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "does not match its file map"):
            self.collect()

        (self.run / "source-manifest.json").write_text(
            json.dumps({"sha256": self.digest, "files": self.source_files}), encoding="utf-8")
        (self.run / "terraincore-ctest-results.xml").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing or empty"):
            self.collect()

    def test_fabricated_pass_empty_or_wrong_ctest_identity_fails(self):
        log = self.run / "terraincore-ctest-run.log"
        log.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "missing or empty"):
            self.collect()
        log.write_text("\n".join(evidence_ledger.TERRAINCORE_CTESTS)
                       + f"\n100% tests passed, 0 tests failed out of "
                       f"{len(evidence_ledger.TERRAINCORE_CTESTS)}", encoding="utf-8")
        junit = self.run / "terraincore-ctest-results.xml"
        junit.write_text('<testsuite><testcase name="SkiDomain.Revision"/></testsuite>',
                         encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "identities/results are not exact"):
            self.collect()


class ShippingEvidenceBindingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.package = self.root / "Saved/StagedBuilds/Windows"
        launcher = self.package / "Windows/SkiAreaDesignChallenge.exe"
        launcher.parent.mkdir(parents=True)
        launcher.write_bytes(b"launcher")
        self.target = self.root / "Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.target"
        self.target.parent.mkdir(parents=True)
        self.target.write_text(json.dumps({
            "TargetName": "SkiAreaDesignChallenge", "Platform": "Win64",
            "Configuration": "Shipping", "TargetType": "Game", "IsTestTarget": False,
            "Project": "../../SkiAreaDesignChallenge.uproject",
            "Launch": "$(ProjectDir)/Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.exe",
            "BuildPlugins": ["WebBrowserWidget"],
        }), encoding="utf-8")
        self.rules = self.root / "Source/SkiAreaDesignChallenge.Target.cs"
        self.rules.parent.mkdir(parents=True)
        self.rules.write_text(
            'Type = TargetType.Game; ExtraModuleNames.Add("SkiPresentation");',
            encoding="utf-8")
        files = {launcher.relative_to(self.package).as_posix():
                 hashlib.sha256(launcher.read_bytes()).hexdigest()}
        manifest_hash = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
        self.invocation = "20260922T120000.000000Z-1234abcd"
        self.receipt = {
            "invocation": self.invocation, "command": "package", "configuration": "Shipping",
            "result": {"status": "PASS", "directory": str(self.package),
                       "manifest": {"sha256": manifest_hash, "files": files},
                       "shipping_mcp_proof": {
                           "status": "PASS", "target": "SkiAreaDesignChallenge",
                           "platform": "Win64", "configuration": "Shipping",
                           "target_type": "Game", "root_module": "SkiPresentation",
                           "target_receipt": str(self.target),
                           "target_receipt_sha256": hashlib.sha256(self.target.read_bytes()).hexdigest(),
                           "target_rules_sha256": hashlib.sha256(self.rules.read_bytes()).hexdigest(),
                       }}}

    def tearDown(self):
        self.temporary.cleanup()

    def test_package_manifest_and_target_hashes_are_recomputed(self):
        bindings = evidence_ledger._validate_shipping_package(
            self.receipt, self.invocation, "shipping-package")
        self.assertEqual(bindings["target_receipt_sha256"],
                         hashlib.sha256(self.target.read_bytes()).hexdigest())
        self.receipt["result"]["manifest"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(RuntimeError, "digest is fabricated"):
            evidence_ledger._validate_shipping_package(
                self.receipt, self.invocation, "shipping-package")

    def test_stale_target_receipt_hash_fails(self):
        self.target.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "target receipt hash is stale"):
            evidence_ledger._validate_shipping_package(
                self.receipt, self.invocation, "shipping-package")

    def test_minimal_or_forbidden_shipping_target_receipt_fails_semantically(self):
        attacks = [
            {"TargetType": "Game"},
            {"TargetName": "SkiAreaDesignChallenge", "Platform": "Win64",
             "Configuration": "Shipping", "TargetType": "Game", "IsTestTarget": False,
             "Project": "../../SkiAreaDesignChallenge.uproject",
             "Launch": "$(ProjectDir)/Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.exe",
             "BuildPlugins": ["ModelContextProtocol"]},
        ]
        for target in attacks:
            with self.subTest(target=target):
                self.target.write_text(json.dumps(target), encoding="utf-8")
                self.receipt["result"]["shipping_mcp_proof"]["target_receipt_sha256"] = \
                    hashlib.sha256(self.target.read_bytes()).hexdigest()
                with self.assertRaisesRegex(
                        RuntimeError, "malformed|identity is not exact|Forbidden MCP/toolset"):
                    evidence_ledger._validate_shipping_package(
                        self.receipt, self.invocation, "shipping-package")


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
        self.readme = self.artifact_root / "README FIRST.txt"
        self.readme.write_text("validated release")
        self.ledger = self.artifact_root / "P1-EVIDENCE-LEDGER.json"
        self.ledger.write_text('{"schemaVersion":2}')
        self.manifest = self.artifact_root / "ReleaseManifest.json"
        self.write_release_manifest()
        self.zip_path = self.release / f"{self.artifact}.zip"
        self.write_zip()
        self.latest = self.release / "MountainPlanner-P1-LATEST.json"
        self.write_handoff()

    def tearDown(self):
        self.temporary.cleanup()

    def write_release_manifest(self):
        records = []
        for path in sorted(value for value in self.artifact_root.rglob("*")
                           if value.is_file() and value.name != "ReleaseManifest.json"):
            records.append({
                "path": path.relative_to(self.artifact_root).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            })
        self.manifest.write_text(json.dumps({
            "schemaVersion": 1, "artifact": self.artifact,
            "sourceDigest": self.digest, "files": records,
        }), encoding="utf-8")

    def write_zip(self, entries=None, prefixed=True):
        if entries is None:
            entries = [(path.relative_to(self.artifact_root).as_posix(), path.read_bytes())
                       for path in sorted(self.artifact_root.rglob("*")) if path.is_file()]
        with zipfile.ZipFile(self.zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in entries:
                archive.writestr(f"{self.artifact}/{name}" if prefixed else name, data)

    def trust_current_zip(self):
        self.write_handoff()

    def write_handoff(self, **overrides):
        self.zip_hash = hashlib.sha256(self.zip_path.read_bytes()).hexdigest()
        (self.release / f"{self.artifact}.zip.sha256").write_text(
            f"{self.zip_hash}  {self.artifact}.zip")
        receipt = {
            "Status": "PASS",
            "Artifact": self.artifact,
            "SourceDigest": self.digest,
            "PackageInvocation": "20260922T120000.000000Z-example",
            "LauncherSha256": hashlib.sha256(self.launcher.read_bytes()).hexdigest(),
            "ReleaseManifestSha256": hashlib.sha256(self.manifest.read_bytes()).hexdigest(),
            "EvidenceLedgerSha256": hashlib.sha256(self.ledger.read_bytes()).hexdigest(),
            "ZipSha256": self.zip_hash,
        }
        receipt.update(overrides)
        self.latest.write_text(json.dumps(receipt), encoding="utf-8")

    def resolver_command(self, release=None, *, launch=False, provider="sample", delay=0):
        command = [
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(ROOT / "Tools/Build/resolve_p1_release.ps1"),
            "-ReleaseBase", str(release or self.release),
        ]
        if launch:
            command.extend(["-Launch", "-Provider", provider,
                            "-PinnedLaunchDelayMilliseconds", str(delay)])
        return command

    def resolve(self, release=None, *, launch=False, provider="sample", delay=0):
        return subprocess.run(
            self.resolver_command(release, launch=launch, provider=provider, delay=delay),
            capture_output=True, text=True, timeout=30)

    def test_valid_handoff_resolves_exact_launcher(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()), self.launcher)

        self.write_zip(prefixed=False)
        self.trust_current_zip()
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_run_p1_delegates_launch_to_handle_pinning_resolver_mode(self):
        launcher = (ROOT / "Run-P1.bat").read_text(encoding="utf-8")
        self.assertIn('resolve_p1_release.ps1" -Launch -Provider', launcher)
        self.assertNotIn('start "Mountain Planner P1"', launcher)

    @unittest.skipUnless(os.name == "nt", "Windows pinned launch")
    def test_launch_mode_creates_process_while_validated_handle_is_pinned(self):
        shutil.copy2(Path(os.environ["WINDIR"]) / "System32/hostname.exe", self.launcher)
        self.write_release_manifest()
        self.write_zip()
        self.write_handoff()
        result = self.resolve(launch=True, provider="live")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"LAUNCHED:\d+")

    @unittest.skipUnless(os.name == "nt", "Windows pinned launch race")
    def test_launcher_write_and_replacement_are_denied_until_process_creation(self):
        shutil.copy2(Path(os.environ["WINDIR"]) / "System32/hostname.exe", self.launcher)
        self.write_release_manifest()
        self.write_zip()
        self.write_handoff()
        attacker = self.release / "attacker.exe"
        shutil.copy2(Path(os.environ["WINDIR"]) / "System32/where.exe", attacker)
        process = subprocess.Popen(
            self.resolver_command(launch=True, provider="live", delay=3000),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pinned = False
        deadline = time.monotonic() + 15
        while process.poll() is None and time.monotonic() < deadline:
            try:
                with self.launcher.open("r+b"):
                    pass
            except PermissionError:
                pinned = True
                break
            time.sleep(0.01)
        self.assertTrue(pinned, "resolver never held a write-denying launcher handle")
        with self.assertRaises(PermissionError):
            os.replace(attacker, self.launcher)
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, stderr)
        self.assertRegex(stdout, r"LAUNCHED:\d+")

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

    def test_changed_missing_or_extra_folder_files_and_changed_or_missing_zip_fail(self):
        self.readme.write_text("changed")
        self.assertNotEqual(self.resolve().returncode, 0)
        self.readme.write_text("validated release")

        self.readme.unlink()
        self.assertNotEqual(self.resolve().returncode, 0)
        self.readme.write_text("validated release")

        (self.artifact_root / "unexpected.dll").write_bytes(b"unexpected")
        self.assertNotEqual(self.resolve().returncode, 0)
        (self.artifact_root / "unexpected.dll").unlink()

        self.write_zip([("SkiAreaDesignChallenge.exe", b"changed launcher in archive"),
                        ("README FIRST.txt", self.readme.read_bytes()),
                        ("P1-EVIDENCE-LEDGER.json", self.ledger.read_bytes()),
                        ("ReleaseManifest.json", self.manifest.read_bytes())])
        self.assertNotEqual(self.resolve().returncode, 0)
        self.write_zip()

        self.zip_path.unlink()
        self.assertNotEqual(self.resolve().returncode, 0)

    def test_zip_wrong_bytes_missing_unexpected_and_traversal_fail_even_when_rehashed(self):
        base = [(path.relative_to(self.artifact_root).as_posix(), path.read_bytes())
                for path in sorted(self.artifact_root.rglob("*")) if path.is_file()]
        attacks = [
            [(name, b"wrong" if name == "README FIRST.txt" else data)
             for name, data in base],
            base[:-1],
            base + [("unexpected.dll", b"unexpected")],
            base + [("../escaped.txt", b"traversal")],
        ]
        for entries in attacks:
            with self.subTest(entries=[name for name, _ in entries]):
                self.write_zip(entries)
                self.trust_current_zip()
                self.assertNotEqual(self.resolve().returncode, 0)

    def test_zip_duplicate_normalized_and_mixed_root_entries_fail(self):
        base = [(path.relative_to(self.artifact_root).as_posix(), path.read_bytes())
                for path in sorted(self.artifact_root.rglob("*")) if path.is_file()]
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, data in base:
                    archive.writestr(f"{self.artifact}/{name}", data)
                archive.writestr(
                    f"{self.artifact}\\README FIRST.txt", self.readme.read_bytes())
        self.trust_current_zip()
        self.assertNotEqual(self.resolve().returncode, 0)

        with zipfile.ZipFile(self.zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in base:
                archive.writestr(f"{self.artifact}/{name}", data)
            archive.writestr("unprefixed.txt", b"mixed")
        self.trust_current_zip()
        self.assertNotEqual(self.resolve().returncode, 0)

    @unittest.skipUnless(os.name == "nt", "Windows junction hardening")
    def test_release_ancestor_artifact_root_and_nested_directory_junctions_fail(self):
        junction_parent = tempfile.TemporaryDirectory()
        link = Path(junction_parent.name) / "release-link"
        created = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(self.release)],
            capture_output=True, text=True).returncode == 0
        self.assertTrue(created)
        try:
            result = self.resolve(link)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reparse point", result.stderr)
        finally:
            subprocess.run(["cmd", "/c", "rmdir", str(link)], capture_output=True)
            junction_parent.cleanup()

        outside = tempfile.TemporaryDirectory()
        nested = self.artifact_root / "empty-junction"
        created = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(nested), outside.name],
            capture_output=True, text=True).returncode == 0
        self.assertTrue(created)
        try:
            result = self.resolve()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reparse point", result.stderr)
        finally:
            subprocess.run(["cmd", "/c", "rmdir", str(nested)], capture_output=True)
            outside.cleanup()

    def test_zip_non_regular_entry_fails(self):
        base = [(path.relative_to(self.artifact_root).as_posix(), path.read_bytes())
                for path in sorted(self.artifact_root.rglob("*")) if path.is_file()]
        with zipfile.ZipFile(self.zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in base:
                archive.writestr(f"{self.artifact}/{name}", data)
            link = zipfile.ZipInfo(f"{self.artifact}/link")
            link.create_system = 3
            link.external_attr = 0o120777 << 16
            archive.writestr(link, "README FIRST.txt")
        self.trust_current_zip()
        self.assertNotEqual(self.resolve().returncode, 0)


if __name__ == "__main__":
    unittest.main()


class NetworkPolicyTests(unittest.TestCase):
    def audit(self, owners):
        return {"method": "GetExtendedTcpTable process-attributed polling", "snapshots": 3,
                "listen_ports": [], "remote_endpoints": [o["remote"] for o in owners],
                "endpoint_owners": owners}

    def test_zero_policy_names_owning_image(self):
        audit = self.audit([{"pid": 7, "image": "UnrealCEFSubProcess.exe",
                             "remote": "[2607:f8b0::54]:443", "first_seen_ms": 5.0}])
        with self.assertRaisesRegex(RuntimeError, "ui-layout 1280x720 ready.*UnrealCEFSubProcess"):
            p1.validate_tcp_audit(audit, require_no_connections=True,
                                  label="ui-layout 1280x720 ready")

    def test_selector_policy_allows_only_browser_to_tile_host(self):
        with tempfile.TemporaryDirectory() as folder:
            user = Path(folder)
            ok = self.audit([{"pid": 7, "image": "EpicWebHelper.exe",
                              "remote": "[2a04:4e42::347]:443", "first_seen_ms": 1.0}])
            p1.validate_selector_audit(ok, user, label="selector",
                                       tile_addresses={"2a04:4e42::347"})
            for owner in ({"pid": 7, "image": "EpicWebHelper.exe",
                           "remote": "[2607:f8b0::54]:443"},
                          {"pid": 8, "image": "SkiAreaDesignChallenge.exe",
                           "remote": "[2a04:4e42::347]:443"}):
                with self.subTest(owner=owner), self.assertRaises(RuntimeError):
                    p1.validate_selector_audit(self.audit([{**owner, "first_seen_ms": 1.0}]),
                                               user, label="selector",
                                               tile_addresses={"2a04:4e42::347"})

    def test_selector_policy_rejects_recorded_google_contact(self):
        with tempfile.TemporaryDirectory() as folder:
            state = Path(folder) / "Saved/webcache_1/Default/Network/Network Persistent State"
            state.parent.mkdir(parents=True)
            state.write_text(json.dumps({"net": {"http_server_properties": {"servers": [
                {"server": "https://accounts.google.com"}]}}}), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "accounts.google.com"):
                p1.validate_selector_audit(self.audit([]), Path(folder), label="selector",
                                           tile_addresses=set())

    def test_non_selecting_state_rejects_browser_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            p1.require_no_browser_profile(Path(folder), "ready")
            (Path(folder) / "Saved/webcache_6613").mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, "outside the Selecting state"):
                p1.require_no_browser_profile(Path(folder), "ready")
