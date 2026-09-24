import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import struct
import tempfile
import time
import unittest
from unittest import mock
import warnings
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Build"))
import p1
import evidence_ledger


class ReleaseProofContractTests(unittest.TestCase):
    def test_p1_automation_manifest_includes_exact_slice_tests_but_not_m3(self):
        declared = set(p1.PLACE_SEARCH_TESTS + p1.SITE_PICKER_TESTS + p1.UI_TESTS
                       + p1.TERRAIN_SCRATCH_TESTS
                       + p1.PRESENTATION_INSTALLED_TERRAIN_TESTS)
        self.assertEqual(declared, {
            "MountainPlanner.P1.Preparation.PlaceSearch.NormalizationCacheAndBounds",
            "MountainPlanner.P1.Preparation.PlaceSearch.PacingCancellationAndBounds",
            "MountainPlanner.P1.Presentation.SitePicker.CancellationToken",
            "MountainPlanner.P1.Presentation.SitePicker.ZeroOverflowIsAlreadyAtEnd",
            "MountainPlanner.P1.Presentation.UI.ResponsiveLayout",
            "MountainPlanner.P1.Presentation.UI.SitePickerPanelLayout",
            "MountainPlanner.P1.Presentation.InstalledTerrain.VerifiedMountainHandoff",
            "MountainPlanner.P1.Presentation.InstalledTerrain.SiteContextSummary",
            "MountainPlanner.P1.TerrainScratch.BitExactIndexedLodsAndProvenance",
            "MountainPlanner.P1.TerrainScratch.BoundsAndMalformedInputs",
            "MountainPlanner.P1.TerrainScratch.CancellationInvalidatesStore",
            "MountainPlanner.P1.TerrainScratch.MultiBlockEdgesAndClamp",
        })
        self.assertTrue(declared.issubset(p1.P1_TESTS))
        self.assertEqual(len(p1.P1_TESTS), len(set(p1.P1_TESTS)))
        self.assertIn(
            "MountainPlanner.P1.Presentation.InstalledTerrain.VerifiedMountainHandoff",
            p1.P1_TESTS)
        self.assertIn(
            "MountainPlanner.P1.Presentation.InstalledTerrain.SiteContextSummary",
            p1.P1_TESTS)
        self.assertFalse(any(test.startswith("MountainPlanner.M5.")
                             for test in p1.P1_TESTS))
        self.assertFalse(any(test.startswith("MountainPlanner.M3.")
                             for test in p1.P1_TESTS))

        source_tests = []
        for path in (ROOT / "Source").rglob("*Tests.cpp"):
            source = path.read_text(encoding="utf-8")
            source_tests.extend(re.findall(r'"(MountainPlanner\.P1\.[^"]+)"', source))
        self.assertEqual(len(source_tests), len(set(source_tests)), "duplicate native P1 test identity")
        self.assertCountEqual(p1.P1_TESTS, source_tests)

    def test_focused_m3_s1m_xml_reader_filter_matches_source_and_stays_out_of_p1(self):
        group = "MountainPlanner.M3.S1mXmlReader"
        source = (ROOT / "Source/SkiPreparation/Private/Tests/S1mXmlReaderTests.cpp").read_text(
            encoding="utf-8")
        registered = re.findall(r'"(MountainPlanner\.M3\.S1mXmlReader\.[^"]+)"', source)
        self.assertCountEqual(p1.FOCUSED_AUTOMATION_TESTS[group], registered)
        self.assertEqual(len(registered), 2)
        declared = [test for tests in p1.FOCUSED_AUTOMATION_TESTS.values() for test in tests]
        self.assertEqual(len(declared), len(set(declared)), "duplicate focused test identity")
        self.assertFalse(any(test.startswith("MountainPlanner.M3.S1mXmlReader")
                             for test in p1.P1_TESTS))

    def test_focused_m5_filters_match_registered_source_tests_and_stay_out_of_p1(self):
        registered = []
        for path in (ROOT / "Source").rglob("*Tests.cpp"):
            source = path.read_text(encoding="utf-8")
            registered.extend(re.findall(r'"(MountainPlanner\.M5\.[^"]+)"', source))

        m5_groups = {group: tests for group, tests in p1.FOCUSED_AUTOMATION_TESTS.items()
                     if group.startswith("MountainPlanner.M5.")}
        declared = [test for tests in m5_groups.values() for test in tests]
        self.assertEqual(len(registered), len(set(registered)), "duplicate native M5 test identity")
        self.assertEqual(len(declared), len(set(declared)), "duplicate focused test identity")
        self.assertCountEqual(declared, registered)

        # SiteContext is an intentionally broad Unreal prefix: it also matches
        # the sibling SiteContextCompositeAssembler test group.
        for test in registered:
            matching_groups = [group for group in m5_groups if test.startswith(group)]
            self.assertEqual(len(matching_groups), 1, test)
        self.assertCountEqual(m5_groups["MountainPlanner.M5.SiteContext"], [
            "MountainPlanner.M5.SiteContext.CompositeReceiptGateAndLegacyRead",
            "MountainPlanner.M5.SiteContext.PyramidStoreAndIntegrity",
            "MountainPlanner.M5.SiteContext.VectorSchema2PartsAndLegacy",
            "MountainPlanner.M5.SiteContextCompositeAssembler.AtomicInstallAndPathMapping",
        ])
        self.assertCountEqual(m5_groups["MountainPlanner.M5.TerrainCoreRepository"], [
            "MountainPlanner.M5.TerrainCoreRepository.LegacyProvenanceUnavailable",
            "MountainPlanner.M5.TerrainCoreRepository.VerifiedProvenanceSidecar",
        ])
        self.assertCountEqual(m5_groups["MountainPlanner.M5.ImageryAcquisition"], [
            "MountainPlanner.M5.ImageryAcquisition.BudgetsCancellationAndNoData",
            "MountainPlanner.M5.ImageryAcquisition.ProductionGatewayEntryPointCancellation",
            "MountainPlanner.M5.ImageryAcquisition.ScriptedReprojectionAndHash",
        ])
        for group, tests in m5_groups.items():
            with self.subTest(group=group):
                expected = [test for test in registered if test.startswith(group)]
                self.assertCountEqual(tests, expected)
                self.assertTrue(all(test.startswith(group) for test in tests))

    def test_focused_m4_staged_terrain_filter_matches_source_declarations(self):
        group = "SkiPreparation.M4.StagedTerrain"
        source = (ROOT / "Source/SkiPreparation/Private/Tests/StagedTerrainAcquisitionTests.cpp").read_text(
            encoding="utf-8")
        registered = re.findall(r'"(SkiPreparation\.M4\.StagedTerrain[^"]+)"', source)
        self.assertCountEqual(p1.FOCUSED_AUTOMATION_TESTS[group], registered)
        self.assertEqual(len(registered), 6)
        self.assertFalse(any(test.startswith("SkiPreparation.M4.StagedTerrain")
                             for test in p1.P1_TESTS))

    def test_focused_m4_native_staged_adapter_matches_source_without_duplicates(self):
        group = "SkiPreparation.M4.NativeStagedAdapter"
        source = (ROOT / "Source/SkiPreparation/Private/Tests/NativeStagedTerrainAcquisitionAdapterTests.cpp").read_text(
            encoding="utf-8")
        registered = re.findall(r'"(SkiPreparation\.M4\.NativeStagedAdapter[^"]+)"', source)
        self.assertCountEqual(p1.FOCUSED_AUTOMATION_TESTS[group], registered)
        self.assertEqual(len(registered), 6)
        declared = [test for tests in p1.FOCUSED_AUTOMATION_TESTS.values() for test in tests]
        self.assertEqual(len(declared), len(set(declared)), "duplicate focused test identity")
        self.assertFalse(any(test.startswith(group) for test in p1.P1_TESTS))

    def test_focused_m6_ten_km_preflight_filter_matches_source_declarations(self):
        group = "SkiPreparation.M6.TenKmResourcePreflight"
        source = (ROOT / "Source/SkiPreparation/Private/Tests/TenKmResourcePreflightTests.cpp").read_text(
            encoding="utf-8")
        registered = re.findall(r'"(SkiPreparation\.M6\.TenKmResourcePreflight[^\"]+)"', source)
        self.assertCountEqual(p1.FOCUSED_AUTOMATION_TESTS[group], registered)
        self.assertEqual(len(registered), 4)
        self.assertFalse(any(test.startswith("SkiPreparation.M6.TenKmResourcePreflight")
                             for test in p1.P1_TESTS))

    def test_p1_installed_terrain_handoff_name_is_registered_in_presentation_source(self):
        source = (ROOT / "Source/SkiPresentation/Private/Tests/P1PresentationTests.cpp").read_text(
            encoding="utf-8")
        self.assertEqual(source.count(
            '"MountainPlanner.P1.Presentation.InstalledTerrain.VerifiedMountainHandoff"'), 1)

    def test_p1_picker_zero_overflow_name_is_registered_in_presentation_source(self):
        source = (ROOT / "Source/SkiPresentation/Private/Tests/P1PresentationTests.cpp").read_text(
            encoding="utf-8")
        test_name = "MountainPlanner.P1.Presentation.SitePicker.ZeroOverflowIsAlreadyAtEnd"
        self.assertEqual(source.count(f'"{test_name}"'), 1)
        self.assertIn(test_name, p1.P1_TESTS)

    def test_native_ctest_identities_match_all_release_consumers(self):
        self.assertEqual(set(p1.TERRAINCORE_NATIVE_TESTS),
                         set(evidence_ledger.TERRAINCORE_CTESTS))
        script = (ROOT / "Tools/Build/assemble_p1_release.ps1").read_text(
            encoding="utf-8")
        declaration = re.search(r"\$expectedNativeTerrainCoreTests = @\(([^\n]+)\)",
                                script)
        self.assertIsNotNone(declaration)
        self.assertEqual(set(re.findall(r"'([^']+)'", declaration.group(1))),
                         set(p1.TERRAINCORE_NATIVE_TESTS))

    def test_shipping_forbidden_plugin_count_matches_release_assembly(self):
        script = (ROOT / "Tools/Build/assemble_p1_release.ps1").read_text(
            encoding="utf-8")
        expected = len(p1.FORBIDDEN_SHIPPING_PLUGINS)
        self.assertIn(
            f"@($shippingMcpProof.forbidden_plugins_absent).Count -ne {expected}",
            script)

class OfflineReopenSmokeArgumentTests(unittest.TestCase):
    content_id = "403d307ec11a9c45d565c3a8f27b3e972193380a8bf8915e48fea953a978bc00"
    edit_set_id = "1ec5f42c04a8fe3f6e9b471e65c796123563afa3fdc024476dbab5fc88bbd58c"

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.run_output = self.root / "run-output"
        self.run_output.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def run_smoke_and_capture_command(self, scenario, receipt_overrides=None,
                                      omitted_fields=()):
        commands = []

        def run_child(command, **_kwargs):
            commands.append(command)
            value = lambda prefix: next(
                part.split("=", 1)[1] for part in command if part.startswith(prefix))
            receipt = Path(value("-SkiP1Receipt="))
            receipt_values = {
                "token": value("-SkiP1Token="), "scenario": scenario,
                "ready": True, "picked": True, "reopened": True,
            }
            if scenario == "offline-reopen":
                receipt_values.update({
                    "contentId": self.content_id, "editSetId": self.edit_set_id,
                    "offlineReopen": True, "editDeltaReconstructed": True,
                    "acquisitionPortGuardInstalled": True,
                    "acquisitionTransportCalls": 0,
                })
            receipt_values.update(receipt_overrides or {})
            for field in omitted_fields:
                receipt_values.pop(field, None)
            receipt.write_text(json.dumps(receipt_values), encoding="utf-8")
            return {"method": "fixture", "snapshots": 1}

        with mock.patch.object(p1, "OUTPUT", self.output), \
                mock.patch.object(p1, "verify_package_report",
                                  return_value=({"invocation": "fixture"},
                                                self.root / "launcher.exe")), \
                mock.patch.object(p1, "checked_with_tcp_audit", side_effect=run_child), \
                mock.patch.object(p1, "validate_tcp_audit"), \
                mock.patch.object(p1, "require_no_browser_profile"):
            p1.smoke("Development", "source-digest", scenario, self.content_id,
                     self.run_output, edit_set_id=self.edit_set_id)
        self.assertEqual(len(commands), 1)
        return commands[0]

    def test_offline_reopen_forwards_both_receipt_identities(self):
        command = self.run_smoke_and_capture_command("offline-reopen")
        self.assertIn(f"-SkiP1ContentId={self.content_id}", command)
        self.assertIn(f"-SkiP1EditSetId={self.edit_set_id}", command)

    def test_offline_reopen_receipt_binds_ids_reconstruction_and_acquisition_guard(self):
        invalid_receipts = (
            ({"contentId": "f" * 64}, ()),
            ({}, ("contentId",)),
            ({"editSetId": "e" * 64}, ()),
            ({}, ("editSetId",)),
            ({"offlineReopen": False}, ()),
            ({}, ("offlineReopen",)),
            ({"editDeltaReconstructed": False}, ()),
            ({}, ("editDeltaReconstructed",)),
            ({"acquisitionPortGuardInstalled": False}, ()),
            ({}, ("acquisitionPortGuardInstalled",)),
            ({"acquisitionTransportCalls": 1}, ()),
            ({}, ("acquisitionTransportCalls",)),
        )
        for updates, omitted in invalid_receipts:
            with self.subTest(updates=updates, omitted=omitted), \
                    self.assertRaises(RuntimeError):
                self.run_smoke_and_capture_command(
                    "offline-reopen", receipt_overrides=updates,
                    omitted_fields=omitted)

    def test_edit_set_identity_is_not_forwarded_to_other_scenarios(self):
        command = self.run_smoke_and_capture_command("import")
        self.assertFalse(any(part.startswith("-SkiP1ContentId=") for part in command))
        self.assertFalse(any(part.startswith("-SkiP1EditSetId=") for part in command))

    def test_offline_reopen_rejects_missing_or_malformed_receipt_ids(self):
        with mock.patch.object(p1, "OUTPUT", self.output), \
                mock.patch.object(p1, "verify_package_report",
                                  return_value=({"invocation": "fixture"},
                                                self.root / "launcher.exe")):
            with self.assertRaisesRegex(RuntimeError, "exact contentId"):
                p1.smoke("Development", "source-digest", "offline-reopen",
                         "not-a-content-id", self.run_output, edit_set_id=self.edit_set_id)
            with self.assertRaisesRegex(RuntimeError, "exact editSetId"):
                p1.smoke("Development", "source-digest", "offline-reopen",
                         self.content_id, self.run_output, edit_set_id=None)
            with self.assertRaisesRegex(RuntimeError, "exact editSetId"):
                p1.smoke("Development", "source-digest", "offline-reopen",
                         self.content_id, self.run_output, edit_set_id="f" * 63)

    def test_cli_exposes_and_validates_edit_set_id(self):
        script = ROOT / "Tools/Build/p1.py"
        help_result = subprocess.run(
            [sys.executable, str(script), "smoke", "--help"],
            capture_output=True, text=True, check=False)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("--edit-set-id", help_result.stdout)
        invalid = subprocess.run(
            [sys.executable, str(script), "smoke", "--edit-set-id", "not-a-sha256"],
            capture_output=True, text=True, check=False)
        self.assertEqual(invalid.returncode, 2)
        self.assertIn("64 lowercase hexadecimal characters", invalid.stderr)


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


class FocusedAutomationCommandTests(unittest.TestCase):
    def test_named_m3_s1m_xml_reader_group_runs_with_its_exact_filter(self):
        test_filter = "MountainPlanner.M3.S1mXmlReader"
        expected = p1.FOCUSED_AUTOMATION_TESTS[test_filter]
        engine = Path("C:/Unreal/Engine")
        with mock.patch.object(p1.p0, "native_build", return_value={"status": "PASS"}), \
                mock.patch.object(p1.p0, "engine_path", return_value=engine), \
                mock.patch.object(p1.p0, "checked", return_value="fixture log") as checked, \
                mock.patch.object(p1, "require_exact_automation") as exact:
            result = p1.focused_automation({}, Path("unused"), test_filter)

        self.assertIn(
            f"-ExecCmds=Automation RunTests {test_filter};Quit",
            checked.call_args.args[0])
        exact.assert_called_once_with("fixture log", expected, test_filter)
        self.assertEqual(result["tests"], list(expected))

    def test_named_m5_group_runs_and_checks_its_exact_filter(self):
        test_filter = "MountainPlanner.M5.SiteContext"
        expected = p1.FOCUSED_AUTOMATION_TESTS[test_filter]
        engine = Path("C:/Unreal/Engine")
        output = "fixture automation log"
        with mock.patch.object(p1.p0, "native_build", return_value={"status": "PASS"}) as build, \
                mock.patch.object(p1.p0, "engine_path", return_value=engine), \
                mock.patch.object(p1.p0, "checked", return_value=output) as checked, \
                mock.patch.object(p1, "require_exact_automation") as exact:
            result = p1.focused_automation({}, Path("unused"), test_filter)

        build.assert_called_once_with({}, "Editor", "Development")
        command = checked.call_args.args[0]
        self.assertIn(
            f"-ExecCmds=Automation RunTests {test_filter};Quit", command)
        self.assertEqual(checked.call_args.kwargs["log"],
                         "focused-sitecontext-automation.log")
        exact.assert_called_once_with(output, expected, test_filter)
        self.assertEqual(result["filter"], test_filter)
        self.assertEqual(result["tests"], list(expected))

    def test_m4_staged_terrain_group_runs_with_its_source_exact_filter(self):
        test_filter = "SkiPreparation.M4.StagedTerrain"
        expected = p1.FOCUSED_AUTOMATION_TESTS[test_filter]
        engine = Path("C:/Unreal/Engine")
        with mock.patch.object(p1.p0, "native_build", return_value={"status": "PASS"}), \
                mock.patch.object(p1.p0, "engine_path", return_value=engine), \
                mock.patch.object(p1.p0, "checked", return_value="fixture log") as checked, \
                mock.patch.object(p1, "require_exact_automation") as exact:
            result = p1.focused_automation({}, Path("unused"), test_filter)

        self.assertIn(
            f"-ExecCmds=Automation RunTests {test_filter};Quit",
            checked.call_args.args[0])
        exact.assert_called_once_with("fixture log", expected, test_filter)
        self.assertEqual(result["tests"], list(expected))

    def test_m4_native_staged_adapter_group_runs_with_its_source_exact_filter(self):
        test_filter = "SkiPreparation.M4.NativeStagedAdapter"
        expected = p1.FOCUSED_AUTOMATION_TESTS[test_filter]
        engine = Path("C:/Unreal/Engine")
        with mock.patch.object(p1.p0, "native_build", return_value={"status": "PASS"}), \
                mock.patch.object(p1.p0, "engine_path", return_value=engine), \
                mock.patch.object(p1.p0, "checked", return_value="fixture log") as checked, \
                mock.patch.object(p1, "require_exact_automation") as exact:
            result = p1.focused_automation({}, Path("unused"), test_filter)

        self.assertIn(
            f"-ExecCmds=Automation RunTests {test_filter};Quit",
            checked.call_args.args[0])
        exact.assert_called_once_with("fixture log", expected, test_filter)
        self.assertEqual(result["tests"], list(expected))

    def test_m6_preflight_group_runs_with_its_source_exact_filter(self):
        test_filter = "SkiPreparation.M6.TenKmResourcePreflight"
        expected = p1.FOCUSED_AUTOMATION_TESTS[test_filter]
        engine = Path("C:/Unreal/Engine")
        with mock.patch.object(p1.p0, "native_build", return_value={"status": "PASS"}), \
                mock.patch.object(p1.p0, "engine_path", return_value=engine), \
                mock.patch.object(p1.p0, "checked", return_value="fixture log") as checked, \
                mock.patch.object(p1, "require_exact_automation") as exact:
            result = p1.focused_automation({}, Path("unused"), test_filter)

        self.assertIn(
            f"-ExecCmds=Automation RunTests {test_filter};Quit",
            checked.call_args.args[0])
        exact.assert_called_once_with("fixture log", expected, test_filter)
        self.assertEqual(result["tests"], list(expected))

    def test_unknown_group_fails_before_building(self):
        with mock.patch.object(p1.p0, "native_build") as build, \
                self.assertRaisesRegex(RuntimeError, "Unknown or unregistered"):
            p1.focused_automation({}, Path("unused"), "SkiPreparation.M4.StagedTerrain.Unknown")
        build.assert_not_called()


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
        self.write_target(["webbrowserwidget"])
        with self.assertRaisesRegex(RuntimeError, "entered"):
            p1.shipping_target_module_proof(self.root)
        self.write_target([], TargetType="Editor")
        with self.assertRaisesRegex(RuntimeError, "Shipping Game"):
            p1.shipping_target_module_proof(self.root)


class ProcessNetworkAuditTests(unittest.TestCase):
    valid = {"method": "GetExtendedTcpTable process-attributed polling", "snapshots": 10,
             "listen_ports": [], "remote_endpoints": [],
             "all_observed_pids_exited": True, "last_live_pids": []}

    def test_observed_quiet_process_passes(self):
        p1.validate_tcp_audit(self.valid, require_no_connections=True)

    def test_missing_listener_and_offline_connection_evidence_fail_closed(self):
        cases = [
            ({**self.valid, "snapshots": 0}, True),
            ({**self.valid, "all_observed_pids_exited": False}, True),
            ({**self.valid, "last_live_pids": [42]}, True),
            ({**self.valid, "listen_ports": [8000]}, False),
            ({**self.valid, "remote_endpoints": ["203.0.113.1:443"]}, True),
        ]
        for audit, offline in cases:
            with self.subTest(audit=audit), self.assertRaises(RuntimeError):
                p1.validate_tcp_audit(audit, require_no_connections=offline)

    def test_audit_waits_for_child_after_launcher_exits(self):
        class Launcher:
            pid = 101
            _handle = 901
            returncode = None
            polls = 0

            def poll(self):
                self.polls += 1
                if self.polls >= 2:
                    self.returncode = 0
                return self.returncode

        launcher = Launcher()
        root_identity = (101, 1001)
        child_identity = (202, 2002)
        generations = [({root_identity, child_identity}, {root_identity, child_identity}),
                       ({root_identity, child_identity}, {child_identity}),
                       ({root_identity, child_identity}, {child_identity}),
                       ({root_identity, child_identity}, set())]

        def process_snapshot(*_args):
            p1._PROCESS_IMAGES.update({
                root_identity: "SkiAreaDesignChallenge.exe",
                child_identity: "SkiAreaDesignChallenge.exe",
            })
            return generations.pop(0)

        def child_rows(pid):
            if pid == 202 and launcher.polls >= 2:
                return [{"state": 5, "local": "127.0.0.1:5000",
                         "remote": "203.0.113.1:443"},
                        {"state": 2, "local": "127.0.0.1:1985",
                         "remote": "0.0.0.0:0"}]
            return []
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(p1.p0, "RUN_OUTPUT", Path(folder)), \
                mock.patch.dict(p1._PROCESS_IMAGES,
                                {root_identity: "SkiAreaDesignChallenge.exe",
                                 child_identity: "SkiAreaDesignChallenge.exe"}, clear=True), \
                mock.patch.object(p1.subprocess, "Popen", return_value=launcher), \
                mock.patch.object(p1, "_windows_process_creation_time_from_handle",
                                  return_value=root_identity[1]), \
                mock.patch.object(p1, "_windows_process_creation_time",
                                  side_effect=lambda pid: {101: 1001, 202: 2002}[pid]), \
                mock.patch.object(p1, "_windows_process_descendants",
                                  side_effect=process_snapshot), \
                mock.patch.object(p1, "_windows_ipv4_tcp_rows",
                                  side_effect=child_rows), \
                mock.patch.object(p1, "_windows_ipv6_tcp_rows", return_value=[]), \
                mock.patch.object(p1.time, "sleep"):
            audit = p1.checked_with_tcp_audit(["launcher.exe"], timeout=1,
                                              log="audit.log",
                                              include_listener_owners=True)
        self.assertEqual(audit["snapshots"], 4)
        self.assertEqual(audit["observed_pids"], [101, 202])
        self.assertEqual(audit["observed_process_identities"], [
            {"pid": 101, "creation_time_filetime": 1001,
             "image": "SkiAreaDesignChallenge.exe"},
            {"pid": 202, "creation_time_filetime": 2002,
             "image": "SkiAreaDesignChallenge.exe"},
        ])
        self.assertEqual(audit["remote_endpoints"], ["203.0.113.1:443"])
        self.assertEqual(audit["listen_ports"], [1985])
        self.assertEqual(audit["listener_owners"], [{
            "pid": 202, "process_creation_time_filetime": 2002,
            "image": "SkiAreaDesignChallenge.exe",
            "local": "127.0.0.1:1985",
        }])
        self.assertTrue(audit["all_observed_pids_exited"])
        self.assertEqual(audit["sampling_mode"], "sampled")
        self.assertIn("may be missed", audit["sampling_limitation"])

    def test_timeout_cleanup_verifies_creation_identity_before_terminating(self):
        with mock.patch.object(p1, "_windows_open_process_handle", return_value=44), \
                mock.patch.object(p1, "_windows_process_creation_time_from_handle",
                                  return_value=2002), \
                mock.patch.object(p1, "_windows_terminate_process_handle",
                                  return_value=True) as terminate, \
                mock.patch.object(p1, "_windows_close_process_handle"):
            self.assertFalse(p1._windows_terminate_if_same_process(202, 2001))
            terminate.assert_not_called()

            self.assertTrue(p1._windows_terminate_if_same_process(202, 2002))
            terminate.assert_called_once_with(44)

    def test_audit_timeout_uses_the_observed_child_identity_for_cleanup(self):
        class RunningLauncher:
            pid = 101
            _handle = 901
            returncode = None

            def poll(self):
                return self.returncode

            def kill(self):
                self.returncode = -9

            def wait(self, timeout):
                return self.returncode

        root_identity = (101, 1001)
        child_identity = (202, 2002)
        launcher = RunningLauncher()
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(p1.p0, "RUN_OUTPUT", Path(folder)), \
                mock.patch.object(p1.subprocess, "Popen", return_value=launcher), \
                mock.patch.object(p1, "_windows_process_creation_time_from_handle",
                                  return_value=1001), \
                mock.patch.object(p1, "_windows_process_creation_time",
                                  side_effect=lambda pid: {101: 1001, 202: 2002}[pid]), \
                mock.patch.object(p1, "_windows_process_descendants",
                                  return_value=({root_identity, child_identity},
                                                {root_identity, child_identity})), \
                mock.patch.object(p1, "_windows_ipv4_tcp_rows", return_value=[]), \
                mock.patch.object(p1, "_windows_ipv6_tcp_rows", return_value=[]), \
                mock.patch.object(p1, "_windows_terminate_if_same_process",
                                  return_value=True) as terminate_child, \
                mock.patch.object(p1.time, "monotonic", side_effect=[0, 1]), \
                mock.patch.object(p1.time, "sleep"):
            with self.assertRaises(p1.p0.TimedOut):
                p1.checked_with_tcp_audit(["launcher.exe"], timeout=0, log="timeout.log")

        terminate_child.assert_called_once_with(202, 2002)
        self.assertEqual(launcher.returncode, -9)


class FrontendReceiptEscapeAuditTests(unittest.TestCase):
    content_id = "a" * 64

    def make_audit(self, *, remote_endpoints=(), endpoint_owners=()):
        return {"method": "GetExtendedTcpTable process-attributed polling",
                "snapshots": 10, "listen_ports": [],
                "remote_endpoints": list(remote_endpoints),
                "endpoint_owners": list(endpoint_owners),
                "all_observed_pids_exited": True, "last_live_pids": [],
                "sampling_mode": "sampled", "poll_interval_milliseconds": 10,
                "sampling_limitation": p1.TCP_AUDIT_SAMPLING_LIMITATION}

    def run_frontend_smoke(self, escape_audit):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        run_output = root / "run"
        run_output.mkdir()
        launcher = root / "SkiAreaDesignChallenge.exe"
        commands = []
        primary_audit = self.make_audit()

        def run_audited_child(command, **kwargs):
            commands.append((command, kwargs))
            receipt_arg = next(part.split("=", 1)[1] for part in command
                               if part.startswith("-SkiP1Receipt="))
            receipt_path = Path(receipt_arg)
            if receipt_path.parent == (run_output / "isolated-data"):
                receipt_path.write_text(json.dumps({
                    "token": next(part.split("=", 1)[1] for part in command
                                  if part.startswith("-SkiP1Token=")),
                    "scenario": "frontend", "passed": True, "nativeTitle": True,
                    "nativePickerPlaceholder": True, "installedIdForwarded": True,
                    "browserWidgetAbsent": True, "mountainTravel": True,
                    "offlineReopen": True, "contentId": self.content_id,
                }), encoding="utf-8")
                return primary_audit
            return escape_audit

        with mock.patch.object(p1, "verify_package_report",
                               return_value=({"invocation": "fixture"}, launcher)), \
                mock.patch.object(p1, "checked_with_tcp_audit",
                                  side_effect=run_audited_child) as checked, \
                mock.patch.object(p1, "validate_tcp_audit",
                                  wraps=p1.validate_tcp_audit) as validate:
            result = p1.smoke("Development", "source-digest", "frontend", None,
                              run_output)
        return result, commands, checked, validate

    def test_frontend_escape_probe_is_audited_rejects_connections_and_is_in_receipt(self):
        escape_audit = self.make_audit()
        result, commands, checked, validate = self.run_frontend_smoke(escape_audit)
        self.assertEqual(len(commands), 2)
        self.assertTrue(commands[1][1]["allow_nonzero_exit"])
        self.assertEqual(validate.call_count, 2)
        self.assertTrue(all(call.kwargs["require_no_connections"]
                            for call in validate.call_args_list))
        self.assertEqual(result["receipt_path_escape_audit"], escape_audit)
        self.assertEqual(result["process_network_audit"]["sampling_mode"], "sampled")
        self.assertIn("shorter-lived endpoints may be missed", result["network_policy"])

    def test_frontend_escape_probe_rejects_sampled_remote_connection(self):
        escape_audit = self.make_audit(
            remote_endpoints=("203.0.113.2:443",),
            endpoint_owners=({"pid": 7, "image": "SkiAreaDesignChallenge.exe",
                              "remote": "203.0.113.2:443"},))
        with self.assertRaisesRegex(RuntimeError, "receipt-path escape probe.*connections"):
            self.run_frontend_smoke(escape_audit)


class DevelopmentPickerTraceAuditTests(unittest.TestCase):
    @staticmethod
    def trace_audit(local="127.0.0.1:1985", *, pid=7,
                    image="SkiAreaDesignChallenge.exe", remotes=()):
        host_port = p1._tcp_endpoint_host_port(local)
        ports = [host_port[1]] if host_port else []
        endpoint_owners = [
            {"pid": 7, "image": "SkiAreaDesignChallenge.exe", "remote": remote}
            for remote in remotes]
        return {
            "method": "GetExtendedTcpTable process-attributed polling",
            "root_pid": 7, "observed_pids": [7] if pid == 7 else [7, pid],
            "process_images": {"7": "SkiAreaDesignChallenge.exe",
                               **({str(pid): image} if pid != 7 else {})},
            "snapshots": 12, "all_observed_pids_exited": True,
            "last_live_pids": [], "listen_ports": ports,
            "listener_owners": [{"pid": pid, "image": image, "local": local}],
            "remote_endpoints": list(remotes), "endpoint_owners": endpoint_owners,
        }

    def validate(self, audit, *, configuration="Development", scenario="picker-viewport"):
        return p1.validate_development_picker_viewport_tcp_audit(
            audit, configuration=configuration, scenario=scenario,
            expected_image="SkiAreaDesignChallenge.exe")

    def test_exact_development_trace_listener_passes_and_marks_zero_remote_network(self):
        result = self.validate(self.trace_audit())
        self.assertTrue(result["developmentTraceListenerObserved"])
        self.assertEqual(result["developmentTraceListenerPolicyException"],
                         p1.DEVELOPMENT_TRACE_LISTENER_POLICY)
        self.assertEqual(result["networkRemoteEndpoints"], [])
        self.assertTrue(result["networkRemoteZero"])

    def test_exact_ipv6_loopback_trace_listener_passes(self):
        result = self.validate(self.trace_audit("[::1]:1985"))
        self.assertTrue(result["developmentTraceListenerObserved"])

    def test_wrong_port_non_loopback_and_child_helper_listeners_fail(self):
        invalid_listener = (
            self.trace_audit("127.0.0.1:1986"),
            self.trace_audit("0.0.0.0:1985"),
            self.trace_audit("[::]:1985"),
        )
        for audit in invalid_listener:
            with self.subTest(listener=audit["listener_owners"]), \
                    self.assertRaisesRegex(RuntimeError, "listener outside"):
                self.validate(audit)
        child_helper = self.trace_audit("127.0.0.1:1985", pid=8,
                                        image="UnrealTraceServer.exe")
        with self.assertRaisesRegex(RuntimeError, "child helper processes"):
            self.validate(child_helper)
        wrong_root_image = self.trace_audit()
        wrong_root_image["process_images"]["7"] = "UnrealTraceServer.exe"
        with self.assertRaisesRegex(RuntimeError, "root image is not the packaged app"):
            self.validate(wrong_root_image)

    def test_picker_exception_rejects_every_remote_endpoint_including_loopback(self):
        audit = self.trace_audit(remotes=("127.0.0.1:1985",))
        with self.assertRaisesRegex(RuntimeError, "remote TCP endpoints are forbidden"):
            self.validate(audit)

    def test_picker_exception_requires_observed_listener_and_exited_process_tree(self):
        no_listener = self.trace_audit()
        no_listener["listen_ports"] = []
        no_listener["listener_owners"] = []
        with self.assertRaisesRegex(RuntimeError, "listener was not observed"):
            self.validate(no_listener)
        with self.assertRaisesRegex(RuntimeError, "TCP audit is missing or was not observed"):
            self.validate({**self.trace_audit(), "all_observed_pids_exited": False})

    def test_listener_exception_is_development_picker_only_and_zero_listener_policies_stay_strict(self):
        audit = self.trace_audit()
        for configuration, scenario in (("Shipping", "picker-viewport"),
                                         ("Development", "ui-layout"),
                                         ("Development", "frontend")):
            with self.subTest(configuration=configuration, scenario=scenario), \
                    self.assertRaisesRegex(RuntimeError, "restricted to picker-viewport"):
                self.validate(audit, configuration=configuration, scenario=scenario)
        with self.assertRaisesRegex(RuntimeError, "opened TCP listeners"):
            p1.validate_tcp_audit(audit, require_no_connections=True, label="frontend")

    def test_picker_smoke_receipt_prominently_records_trace_exception_and_zero_remotes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            run_output = root / "run"
            run_output.mkdir()
            launcher = root / "SkiAreaDesignChallenge.exe"
            audits = []

            def run_capture(command, **_kwargs):
                option = lambda prefix: next(
                    value.split("=", 1)[1] for value in command
                    if value.startswith(prefix))
                receipt = Path(option("-SkiP1Receipt="))
                receipt.write_text(json.dumps({
                    "token": option("-SkiP1Token="), "scenario": "picker-viewport",
                }), encoding="utf-8")
                audit = self.trace_audit()
                audits.append(audit)
                return audit

            def capture(receipt, token, width, height, screenshot):
                return {"resolution": [width, height], "screenshot": str(screenshot)}

            with mock.patch.object(p1, "verify_package_report",
                                   return_value=({"invocation": "fixture"}, launcher)), \
                    mock.patch.object(p1, "checked_with_tcp_audit", side_effect=run_capture), \
                    mock.patch.object(p1, "require_picker_viewport_capture", side_effect=capture):
                result = p1.smoke("Development", "source-digest", "picker-viewport",
                                  None, run_output)

        receipt = result["receipt"]
        self.assertTrue(receipt["developmentTraceListenerObserved"])
        self.assertEqual(receipt["developmentTraceListenerPolicyException"],
                         p1.DEVELOPMENT_TRACE_LISTENER_POLICY)
        self.assertTrue(receipt["networkRemoteZero"])
        self.assertEqual(receipt["networkRemoteEndpoints"], [])
        self.assertEqual(len(audits), len(p1.PICKER_VIEWPORT_RESOLUTIONS))
        self.assertTrue(all(audit["listener_owners"][0]["local"] == "127.0.0.1:1985"
                            for audit in receipt["childNetworkAudits"]))
        self.assertIn("remote TCP endpoints: zero", result["network_policy"])


class FocusedUiAutomationRegistryTests(unittest.TestCase):
    def test_ui_filter_matches_all_registered_p1_ui_tests_exactly(self):
        source = (ROOT / "Source/SkiPresentation/Private/Tests/P1PresentationTests.cpp").read_text(
            encoding="utf-8")
        registered = re.findall(
            r'"(MountainPlanner\.P1\.Presentation\.UI\.[^"]+)"', source)

        self.assertCountEqual(p1.UI_TESTS, registered)
        self.assertEqual(len(p1.UI_TESTS), len(set(p1.UI_TESTS)))
        self.assertTrue(set(p1.UI_TESTS).issubset(p1.P1_TESTS))
        self.assertFalse(set(p1.UI_TESTS).intersection(p1.SITE_PICKER_TESTS))


class PickerViewportVisualDiagnosticTests(unittest.TestCase):
    @staticmethod
    def audit(local="0.0.0.0:1985"):
        audit = DevelopmentPickerTraceAuditTests.trace_audit(local)
        audit["process_return_code"] = 0
        return audit

    def run_visual_diagnostic(self, *, unexpected=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        run_output = root / "run"
        run_output.mkdir()
        launcher = root / "SkiAreaDesignChallenge.exe"
        calls = []
        validated = []

        def run_child(command, **kwargs):
            calls.append((command, kwargs))
            option = lambda prefix: next(
                value.split("=", 1)[1] for value in command if value.startswith(prefix))
            receipt_path = Path(option("-SkiP1Receipt="))
            screenshot_path = Path(option("-SkiP1Screenshot="))
            receipt_path.write_text(json.dumps({
                "token": option("-SkiP1Token="), "scenario": "picker-viewport",
                "stepStates": {name: {"captured": True}
                               for name in ("step1", "step2", "step3")},
            }), encoding="utf-8")
            screenshot_path.write_bytes(b"full rendered viewport fixture")
            audit = self.audit()
            if unexpected and len(calls) == 1:
                audit["remote_endpoints"] = ["198.51.100.40:443"]
                audit["endpoint_owners"] = [{
                    "pid": 7, "image": "SkiAreaDesignChallenge.exe",
                    "remote": "198.51.100.40:443"}]
                audit["listener_owners"].append({
                    "pid": 7, "image": "SkiAreaDesignChallenge.exe",
                    "local": "127.0.0.1:1986"})
                audit["listen_ports"] = [1985, 1986]
            return audit

        def validate_capture(receipt, token, width, height, screenshot):
            self.assertEqual(receipt["token"], token)
            self.assertEqual(receipt["stepStates"], {
                name: {"captured": True} for name in ("step1", "step2", "step3")})
            self.assertTrue(screenshot.is_file())
            result = {**receipt, "resolution": [width, height],
                      "screenshot": str(screenshot)}
            validated.append(result)
            return result

        with mock.patch.object(
                p1, "verify_package_report",
                return_value=({"invocation": "fixture"}, launcher)), \
                mock.patch.object(p1, "checked_with_tcp_audit", side_effect=run_child), \
                mock.patch.object(p1, "require_picker_viewport_capture",
                                  side_effect=validate_capture):
            result = p1.picker_viewport_visual(
                "Development", "source-digest", run_output)
        return result, calls, validated

    def test_known_unrestricted_trace_bind_fails_network_but_all_five_visuals_are_attempted(self):
        result, calls, validated = self.run_visual_diagnostic()

        self.assertEqual(len(calls), 5)
        command_resolutions = [
            [next(value for value in command if value.startswith("-ResX=")),
             next(value for value in command if value.startswith("-ResY="))]
            for command, _kwargs in calls
        ]
        self.assertEqual(command_resolutions, [
            [f"-ResX={width}", f"-ResY={height}"]
            for width, height in p1.PICKER_VIEWPORT_RESOLUTIONS
        ])
        for command, _kwargs in calls:
            self.assertIn("-windowed", command)
            self.assertIn("-RenderOffScreen", command)
            self.assertIn("-ForceRes", command)
            self.assertNotIn("-NullRHI", command)
            self.assertEqual(command[command.index("-windowed") + 1:
                                     command.index("-windowed") + 3],
                             ["-RenderOffScreen", "-ForceRes"])
        self.assertEqual(len(validated), 5)
        self.assertEqual([call[1].get("allow_nonzero_exit") for call in calls],
                         [True] * 5)
        self.assertEqual([call[1].get("include_listener_owners") for call in calls],
                         [True] * 5)
        self.assertTrue(result["visualCapturePass"])
        self.assertEqual(result["networkAuditStatus"], "FAIL")
        self.assertEqual(result["status"], "FAIL")
        self.assertFalse(result["releaseGatePass"])
        self.assertFalse(result["releaseAcceptanceEligible"])
        self.assertEqual(len(result["runs"]), 5)
        self.assertEqual(len(result["captures"]), 5)
        self.assertTrue(all(set(capture["stepStates"]) == {"step1", "step2", "step3"}
                            for capture in result["captures"]))
        self.assertTrue(all(run["processNetworkAudit"]["listener_owners"][0]["local"]
                            == "0.0.0.0:1985" for run in result["runs"]))
        self.assertTrue(any("listener outside" in failure["finding"]
                            for failure in result["networkAuditFailures"]))

    def test_offscreen_flag_is_scoped_away_from_release_smoke_scenarios(self):
        source = (ROOT / "Tools/Build/p1.py").read_text(encoding="utf-8")
        smoke_begin = source.index("def smoke(")
        diagnostic_begin = source.index("def picker_viewport_visual(", smoke_begin)
        visual_diagnostic_source = source[diagnostic_begin:source.index(
            "\ndef visual(", diagnostic_begin)]
        smoke_source = source[smoke_begin:diagnostic_begin]

        self.assertIn('"-RenderOffScreen"', visual_diagnostic_source)
        self.assertIn('"-ForceRes"', visual_diagnostic_source)
        self.assertNotIn('"-NullRHI"', visual_diagnostic_source)
        self.assertEqual(smoke_source.count('"-RenderOffScreen"'), 1)
        self.assertNotIn('"-ForceRes"', smoke_source)
        self.assertIn(
            'if scenario == "frontend":\n'
            '        command.extend(("-RenderOffScreen", "-NullRHI"))',
            smoke_source)
        picker_smoke_source = smoke_source[
            smoke_source.index('elif scenario == "picker-viewport":'):]
        self.assertNotIn('"-RenderOffScreen"', picker_smoke_source)

    def test_unexpected_remote_and_extra_listener_are_retained_and_rejected(self):
        result, calls, _validated = self.run_visual_diagnostic(unexpected=True)
        first_run = result["runs"][0]

        self.assertEqual(len(calls), 5)
        self.assertEqual(first_run["processNetworkAudit"]["remote_endpoints"],
                         ["198.51.100.40:443"])
        self.assertEqual(first_run["processNetworkAudit"]["listener_owners"][1]["local"],
                         "127.0.0.1:1986")
        self.assertTrue(any("Remote TCP endpoints observed" in failure["finding"]
                            for failure in result["networkAuditFailures"]))
        self.assertTrue(any("Unexpected or additional TCP listener(s)" in failure["finding"]
                            for failure in result["networkAuditFailures"]))
        self.assertTrue(result["visualCapturePass"])
        self.assertEqual(result["networkAuditStatus"], "FAIL")

    def test_shipping_is_rejected_before_package_or_process_access(self):
        with mock.patch.object(p1, "verify_package_report") as verify:
            with self.assertRaisesRegex(RuntimeError, "Development-only"):
                p1.picker_viewport_visual("Shipping", "unused", Path("unused"))
        verify.assert_not_called()

    def test_cli_failure_report_is_separate_from_release_smoke_receipt(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output = Path(temporary.name) / "p1"
        output.mkdir()
        smoke_report = output / "smoke-Development-picker-viewport.json"
        smoke_report.write_text('{"releaseSmoke":"unchanged"}', encoding="utf-8")
        generic_smoke_report = output / "smoke-Development.json"
        generic_smoke_report.write_text('{"genericSmoke":"unchanged"}', encoding="utf-8")
        diagnostic = {
            "status": "FAIL", "kind": "non_release_picker_viewport_visual_diagnostic",
            "releaseGatePass": False, "releaseAcceptanceEligible": False,
            "networkAuditStatus": "FAIL", "visualCapturePass": True,
        }
        source = {"sha256": "a" * 64, "files": {}}
        with mock.patch.object(p1, "OUTPUT", output), \
                mock.patch.object(p1.p0, "doctor", return_value={"missing": []}), \
                mock.patch.object(p1.p0, "source_snapshot", return_value=source), \
                mock.patch.object(p1.p0, "verify_frozen"), \
                mock.patch.object(p1, "picker_viewport_visual", return_value=diagnostic), \
                mock.patch.object(sys, "argv", ["p1.py", "picker-viewport-visual"]), \
                mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch("builtins.print"):
            exit_code = p1._main()

        self.assertEqual(exit_code, 1)
        self.assertEqual(smoke_report.read_text(encoding="utf-8"),
                         '{"releaseSmoke":"unchanged"}')
        self.assertEqual(generic_smoke_report.read_text(encoding="utf-8"),
                         '{"genericSmoke":"unchanged"}')
        diagnostic_path = output / "picker-viewport-visual-Development.json"
        self.assertTrue(diagnostic_path.is_file())
        report = json.loads(diagnostic_path.read_text(encoding="utf-8"))
        self.assertEqual(report["command"], "picker-viewport-visual")
        self.assertFalse(report["result"]["releaseGatePass"])
        self.assertEqual(report["result"]["networkAuditStatus"], "FAIL")


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


class PickerViewportCaptureTests(unittest.TestCase):
    @staticmethod
    def make_rgba_png(width, height):
        def chunk(name, data):
            payload = name + data
            return (len(data).to_bytes(4, "big") + payload
                    + (zlib.crc32(payload) & 0xffffffff).to_bytes(4, "big"))

        header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
        row = b"\x00" + (b"\x20\x40\x80\xff" * width)
        pixels = zlib.compress(row * height)
        return (p1.PNG_SIGNATURE + chunk(b"IHDR", header)
                + chunk(b"IDAT", pixels) + chunk(b"IEND", b""))

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "test-results/p1")
        self.root = Path(self.temporary.name)
        self.screenshot = self.root / "picker.png"
        width, height = 1280, 720
        png = self.make_rgba_png(width, height)
        self.screenshot.write_bytes(png)
        self.png = png
        self.receipt = {
            "token": "picker-token", "scenario": "picker-viewport",
            "resolution": [width, height], "viewport": [width, height],
            "captureKind": "rendered-viewport-png", "capturedStep": "name-resort",
            "capturedScrollPosition": "end", "nativePickerVisible": True,
            "mapWidgetPresent": True, "networkDisabled": True,
            "topContentVisible": True, "bottomContentReachable": True,
            "visualReviewRequired": True, "screenshotPath": str(self.screenshot),
            "screenshotSha256": hashlib.sha256(png).hexdigest(),
            "scrollAtStart": 0.0, "scrollAtEnd": 440.0, "scrollMaximum": 440.0,
            "rects": {
                "map": [0, 0, width, height], "panel": [24, 24, 440, 672],
                "scroll": [32, 250, 420, 440], "heading": [32, 32, 420, 40],
                "subtitle": [32, 80, 420, 30], "steps": [32, 120, 420, 100],
                "stepItems": [[32, 120 + index * 25, 420, 24]
                              for index in range(4)],
            },
            "texts": {
                "heading": "SKI AREA DESIGN CHALLENGE\nNew resort",
                "subtitle": "Find the mountain you want to make your own.",
                "steps": ["✓  1  Choose location", "✓  2  Define boundary",
                          "●  3  Name resort", "○  4  Download"],
            },
            "stepStates": {
                "step1": {
                    "locationVisible": True, "boundaryVisible": False, "nameVisible": False,
                    "selectSiteInitiallyDisabled": True, "selectSiteEnabledAfterSearch": True,
                    "locationQuery": "47.25, -121.55",
                    "activeLabel": "●  1  Choose location",
                    "searchStatus": "Centered at 47.25000°, -121.55000°. Select site to define its boundary.",
                    "texts": {
                        "locationHeading": "Search a place or enter lat, lon / DMS",
                        "searchButton": "Search / go to coordinates",
                        "selectSiteButton": "Select site",
                    },
                    "rects": {
                        "locationControls": [40, 260, 400, 210],
                        "locationHeading": [40, 260, 400, 30],
                        "locationSearch": [40, 295, 400, 36],
                        "searchButton": [40, 335, 400, 40],
                        "selectSiteButton": [40, 420, 400, 40],
                    },
                },
                "step2": {
                    "locationVisible": False, "boundaryVisible": True, "nameVisible": False,
                    "activeLabel": "●  2  Define boundary", "scrollAtStart": 0.0,
                    "texts": {
                        "boundaryHeading": "Define your boundary",
                        "boundaryInstructions": "Drag on the map to draw a 2–4 km site.",
                        "boundaryStatus": "Drag on the map to draw the site boundary.",
                        "previewStatus": "M3 will add coverage and source-quality details.",
                    },
                    "rects": {
                        "boundaryHeading": [40, 260, 400, 30],
                        "boundaryInstructions": [40, 295, 400, 50],
                        "clearBoundary": [40, 350, 400, 40],
                        "boundaryStatus": [40, 395, 400, 30],
                        "previewStatus": [40, 430, 400, 30],
                    },
                },
                "step3": {
                    "locationVisible": False, "boundaryVisible": True, "nameVisible": True,
                    "selectionValid": True, "downloadEnabled": False,
                    "activeLabel": "●  3  Name resort",
                    "texts": {
                        "boundaryHeading": "Define your boundary",
                        "boundaryInstructions": "Drag on the map to draw a 2–4 km site.",
                        "resortNameHeading": "Name your resort",
                        "downloadLabel": "Download unavailable until M5",
                    },
                    "rects": {
                        "boundaryControls": [40, 260, 400, 200],
                        "boundaryHeading": [40, 260, 400, 30],
                        "boundaryInstructions": [40, 295, 400, 50],
                        "nameControls": [40, 465, 400, 180],
                        "resortNameHeading": [40, 465, 400, 30],
                        "nameBox": [40, 500, 400, 36],
                        "downloadButton": [40, 540, 400, 36],
                        "downloadLabel": [50, 548, 380, 20],
                    },
                    "scrollAtEnd": 440.0, "scrollMaximum": 440.0,
                    "bottomRects": {
                        "nameControls": [40, 465, 400, 180],
                        "resortNameHeading": [40, 465, 400, 30],
                        "nameBox": [40, 500, 400, 36],
                        "downloadButton": [40, 540, 400, 36],
                        "downloadLabel": [50, 548, 380, 20],
                    },
                },
            },
        }

    def tearDown(self):
        self.temporary.cleanup()

    def test_rendered_picker_capture_receipt_and_screenshot_are_bound(self):
        self.assertEqual(p1.PICKER_VIEWPORT_RESOLUTIONS,
                         ((1280, 720), (1920, 1080), (2560, 1080),
                          (2560, 1440), (576, 1024)))
        result = p1.require_picker_viewport_capture(
            self.receipt, "picker-token", 1280, 720, self.screenshot)
        self.assertEqual(result["screenshot"], str(self.screenshot))

    def test_zero_or_subpixel_overflow_is_at_end_with_native_step3_proof(self):
        step3 = self.receipt["stepStates"]["step3"]
        for maximum in (0.0, 0.5, 1.0):
            with self.subTest(maximum=maximum):
                receipt = {
                    **self.receipt,
                    "scrollAtEnd": 0.0,
                    "scrollMaximum": maximum,
                    "stepStates": {**self.receipt["stepStates"],
                                   "step3": {**step3, "scrollAtEnd": 0.0,
                                             "scrollMaximum": maximum}},
                }
                result = p1.require_picker_viewport_capture(
                    receipt, "picker-token", 1280, 720, self.screenshot)
                self.assertEqual(result["scrollMaximum"], maximum)

    def test_zero_overflow_requires_visible_name_and_disabled_download_receipt(self):
        step3 = self.receipt["stepStates"]["step3"]
        hidden_name = {**step3, "nameVisible": False}
        enabled_download = {**step3, "downloadEnabled": True}
        missing_download = {
            **step3,
            "bottomRects": {name: rect for name, rect in step3["bottomRects"].items()
                            if name != "downloadButton"},
        }
        outside_scroll = {
            **step3,
            "bottomRects": {**step3["bottomRects"],
                            "nameBox": [40, 800, 400, 36]},
        }
        invalid_receipts = (
            {**self.receipt, "bottomContentReachable": False},
            {**self.receipt, "capturedScrollPosition": "start"},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": hidden_name}},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": enabled_download}},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": missing_download}},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": outside_scroll}},
        )
        for receipt in invalid_receipts:
            with self.subTest(receipt=receipt), self.assertRaises(RuntimeError):
                p1.require_picker_viewport_capture(
                    receipt, "picker-token", 1280, 720, self.screenshot)

    def test_scroll_extent_must_be_valid_and_real_overflow_must_reach_end(self):
        step3 = self.receipt["stepStates"]["step3"]

        def with_scroll(maximum, offset):
            return {
                **self.receipt,
                "scrollAtEnd": offset,
                "scrollMaximum": maximum,
                "stepStates": {**self.receipt["stepStates"],
                               "step3": {**step3, "scrollAtEnd": offset,
                                         "scrollMaximum": maximum}},
            }

        within_tolerance = p1.require_picker_viewport_capture(
            with_scroll(20.0, 19.0), "picker-token", 1280, 720, self.screenshot)
        self.assertEqual(within_tolerance["scrollMaximum"], 20.0)
        for receipt in (with_scroll(-0.1, 0.0),
                        with_scroll(0.5, 2.0),
                        with_scroll(20.0, 18.0),
                        with_scroll(float("nan"), 0.0)):
            with self.subTest(receipt=receipt), self.assertRaises(RuntimeError):
                p1.require_picker_viewport_capture(
                    receipt, "picker-token", 1280, 720, self.screenshot)

    def test_native_harness_drives_real_picker_steps_and_names_the_capture_step(self):
        source = (ROOT / "Source/SkiPresentation/Private/SkiBootstrapGameMode.cpp").read_text(
            encoding="utf-8")
        begin = source.index("bool ASkiBootstrapGameMode::BeginP1PickerViewportSmoke()")
        step1 = source.index("void ASkiBootstrapGameMode::InspectP1PickerViewportTop()")
        step2 = source.index("void ASkiBootstrapGameMode::InspectP1PickerViewportBoundary()")
        step3 = source.index("void ASkiBootstrapGameMode::InspectP1PickerViewportName()")
        self.assertLess(begin, source.index("NewResortButton->OnClicked.Broadcast();", begin))
        transition = source[step1:step2]
        expected_order = (
            'LocationSearch->SetText(FText::FromString(TEXT("47.25, -121.55")))',
            "SearchButton->OnClicked.Broadcast()",
            "SelectSiteButton->OnClicked.Broadcast()",
        )
        positions = [transition.index(item) for item in expected_order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("CreateViewportSmokeBoundary(SmokeToken)", source[step2:step3])
        self.assertIn("capturedStep", source)
        self.assertIn("name-resort", source)
        self.assertIn("capturedScrollPosition", source)
        self.assertIn("stepStates", source)

    def test_capture_fails_closed_for_wrong_viewport_visual_or_scroll_evidence(self):
        invalid_receipts = (
            {**self.receipt, "resolution": [576, 1024]},
            {**self.receipt, "visualReviewRequired": False},
            {**self.receipt, "networkDisabled": False},
            {**self.receipt, "scrollAtEnd": 10.0},
            {**self.receipt, "capturedStep": "location"},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": {**self.receipt["stepStates"]["step3"],
                          "texts": {**self.receipt["stepStates"]["step3"]["texts"],
                                    "downloadLabel": ""}}}},
            {**self.receipt, "stepStates": {**self.receipt["stepStates"],
                "step3": {**self.receipt["stepStates"]["step3"],
                          "bottomRects": {**self.receipt["stepStates"]["step3"]["bottomRects"],
                                          "downloadButton": [40, 800, 400, 36]}}}},
        )
        for receipt in invalid_receipts:
            with self.subTest(receipt=receipt), self.assertRaises(RuntimeError):
                p1.require_picker_viewport_capture(
                    receipt, "picker-token", 1280, 720, self.screenshot)
        self.screenshot.write_bytes(b"not a png")
        with self.assertRaisesRegex(RuntimeError, "screenshot does not match"):
            p1.require_picker_viewport_capture(
                self.receipt, "picker-token", 1280, 720, self.screenshot)

    def test_capture_rejects_incomplete_or_corrupt_png_data(self):
        header_only = (p1.PNG_SIGNATURE + (13).to_bytes(4, "big") + b"IHDR"
                       + struct.pack(">IIBBBBB", 1280, 720, 8, 6, 0, 0, 0))
        truncated = self.png[:-1]
        bad_crc = bytearray(self.png)
        bad_crc[29] ^= 1
        for label, png in (("header-only", header_only), ("truncated", truncated),
                           ("crc", bytes(bad_crc))):
            with self.subTest(png=label):
                self.screenshot.write_bytes(png)
                receipt = {**self.receipt,
                           "screenshotSha256": hashlib.sha256(png).hexdigest()}
                with self.assertRaisesRegex(RuntimeError, "screenshot does not match"):
                    p1.require_picker_viewport_capture(
                        receipt, "picker-token", 1280, 720, self.screenshot)

    def test_picker_viewport_smoke_is_development_only(self):
        with self.assertRaisesRegex(RuntimeError, "Development-only"):
            p1.smoke("Shipping", "unused", "picker-viewport", None, self.root)


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
            "BuildPlugins": [],
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
            {"TargetName": "SkiAreaDesignChallenge", "Platform": "Win64",
             "Configuration": "Shipping", "TargetType": "Game", "IsTestTarget": False,
             "Project": "../../SkiAreaDesignChallenge.uproject",
             "Launch": "$(ProjectDir)/Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.exe",
             "BuildPlugins": ["WebBrowserWidget"]},
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
                "endpoint_owners": owners, "all_observed_pids_exited": True,
                "last_live_pids": []}

    def test_zero_policy_names_owning_image(self):
        audit = self.audit([{"pid": 7, "image": "UnrealCEFSubProcess.exe",
                             "remote": "[2607:f8b0::54]:443", "first_seen_ms": 5.0}])
        with self.assertRaisesRegex(RuntimeError, "ui-layout 1280x720 ready.*UnrealCEFSubProcess"):
            p1.validate_tcp_audit(audit, require_no_connections=True,
                                  label="ui-layout 1280x720 ready")

    def test_native_package_rejects_browser_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            p1.assert_no_browser_bundle(root)
            helper = root / "Engine/Binaries/ThirdParty/CEF3/EpicWebHelper.exe"
            helper.parent.mkdir(parents=True)
            helper.write_bytes(b"browser")
            with self.assertRaisesRegex(RuntimeError, "Browser helper"):
                p1.assert_no_browser_bundle(root)

    def test_native_package_rejects_browser_path_inside_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "assets.pak").write_bytes(b"prefix/Content/P1Selector/index.html")
            with self.assertRaisesRegex(RuntimeError, "Browser helper"):
                p1.assert_no_browser_bundle(root)

    def test_native_frontend_rejects_browser_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            p1.require_no_browser_profile(Path(folder), "ready")
            (Path(folder) / "Saved/webcache_6613").mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, "after native frontend cutover"):
                p1.require_no_browser_profile(Path(folder), "ready")
