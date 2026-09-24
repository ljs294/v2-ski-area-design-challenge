"""Failure-propagating P1 Unreal build, package, automation, and packaged-smoke harness."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import ipaddress
import importlib.util
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import uuid
import zipfile
import struct
import zlib
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "test-results/p1"
PROJECT = ROOT / "SkiAreaDesignChallenge.uproject"
TIFF_TESTS = (
    "MountainPlanner.P1.Preparation.GeoTiff.DecodeOrganizations",
    "MountainPlanner.P1.Preparation.GeoTiff.LiveShapedNoData",
    "MountainPlanner.P1.Preparation.GeoTiff.MetadataValidation",
)
ACQUISITION_TESTS = (
    "MountainPlanner.P1.Preparation.Acquisition.Plan",
    "MountainPlanner.P1.Preparation.Acquisition.RetryPolicy",
    "MountainPlanner.P1.Preparation.Acquisition.Stitch",
    "MountainPlanner.P1.Preparation.Acquisition.ActivationFence",
)
PLACE_SEARCH_TESTS = (
    "MountainPlanner.P1.Preparation.PlaceSearch.NormalizationCacheAndBounds",
    "MountainPlanner.P1.Preparation.PlaceSearch.PacingCancellationAndBounds",
)
SITE_PICKER_TESTS = (
    "MountainPlanner.P1.Presentation.SitePicker.CancellationToken",
    "MountainPlanner.P1.Presentation.SitePicker.ZeroOverflowIsAlreadyAtEnd",
)
PRESENTATION_INSTALLED_TERRAIN_TESTS = (
    "MountainPlanner.P1.Presentation.InstalledTerrain.VerifiedMountainHandoff",
    "MountainPlanner.P1.Presentation.InstalledTerrain.SiteContextSummary",
)
UI_TESTS = (
    "MountainPlanner.P1.Presentation.UI.ResponsiveLayout",
    "MountainPlanner.P1.Presentation.UI.SitePickerPanelLayout",
)
TERRAINCORE_TESTS = (
    "MountainPlanner.P1.TerrainCore.AdversarialCache",
    "MountainPlanner.P1.TerrainCore.AtomicEditAndMesh",
    "MountainPlanner.P1.TerrainCore.LegacyPackageCompatibility",
    "MountainPlanner.P1.TerrainCore.StoreRoundTripAndAttacks",
    "MountainPlanner.P1.TerrainCore.ResidencyAndPublication",
    "MountainPlanner.P1.TerrainCore.RuntimeLodAndQuery",
    "MountainPlanner.P1.TerrainCore.SaddleAndSpatialLod",
    "MountainPlanner.P1.TerrainCore.EditPersistence",
)
TERRAIN_SCRATCH_TESTS = (
    "MountainPlanner.P1.TerrainScratch.BitExactIndexedLodsAndProvenance",
    "MountainPlanner.P1.TerrainScratch.BoundsAndMalformedInputs",
    "MountainPlanner.P1.TerrainScratch.CancellationInvalidatesStore",
    "MountainPlanner.P1.TerrainScratch.MultiBlockEdgesAndClamp",
)
TERRAINCORE_NATIVE_TESTS = (
    "SkiDomain.CoverEcology",
    "SkiDomain.ElevationSources",
    "SkiDomain.PlaceCoordinates",
    "SkiDomain.Revision",
    "SkiDomain.SiteSelection",
    "SkiDomain.Terrain",
    "SkiDomain.TerrainCore",
    "SkiDomain.TerrainQuality",
)
P1_PRODUCT_TESTS = (
    "MountainPlanner.P1.Product.CoverEcology.CompositeActivation",
    "MountainPlanner.P1.Product.CoverEcology.ContractAndStore",
    "MountainPlanner.P1.Product.Medium.ProfileContract",
    "MountainPlanner.P1.Product.Medium.ProvenanceHonesty",
    "MountainPlanner.P1.Product.Medium.RequiredCoverBlocksActivation",
    "MountainPlanner.P1.Product.Medium.ScriptedProvider",
    "MountainPlanner.P1.Product.WorldCoverCog.AnalyticalClasses",
)
FOCUSED_AUTOMATION_TESTS = {
    "MountainPlanner.M3.S1mXmlReader": (
        "MountainPlanner.M3.S1mXmlReader.BoundedSafetyPreflight",
        "MountainPlanner.M3.S1mXmlReader.UnpinnedSchemaFailsClosed",
    ),
    "SkiPreparation.M4.StagedTerrain": (
        "SkiPreparation.M4.StagedTerrain.CancelResumeReceipt",
        "SkiPreparation.M4.StagedTerrain.EnforcesResourceLimits",
        "SkiPreparation.M4.StagedTerrain.RejectsLibraryActivation",
        "SkiPreparation.M4.StagedTerrain.RejectsMountainTransition",
        "SkiPreparation.M4.StagedTerrain.RejectsStaleETag",
        "SkiPreparation.M4.StagedTerrain.StageOrderingAndNoActivation",
    ),
    "SkiPreparation.M4.NativeStagedAdapter": (
        "SkiPreparation.M4.NativeStagedAdapter.UnsupportedGeographyFailsClosed",
        "SkiPreparation.M4.NativeStagedAdapter.RejectsNonNavd88Datum",
        "SkiPreparation.M4.NativeStagedAdapter.MissingS1mXmlAndGpkgFailClosed",
        "SkiPreparation.M4.NativeStagedAdapter.TnmBboxAndCogDoNotProveFullSiteCoverage",
        "SkiPreparation.M4.NativeStagedAdapter.StaleCatalogEtagFailsSourcePreflight",
        "SkiPreparation.M4.NativeStagedAdapter.SourcePreflightBindsStrongEtagAndExactSize",
    ),
    "MountainPlanner.M5.InstalledTerrain": (
        "MountainPlanner.M5.InstalledTerrain.CompositeActivationAndLegacyRead",
    ),
    "MountainPlanner.M5.SiteContext": (
        "MountainPlanner.M5.SiteContext.CompositeReceiptGateAndLegacyRead",
        "MountainPlanner.M5.SiteContext.PyramidStoreAndIntegrity",
        "MountainPlanner.M5.SiteContext.VectorSchema2PartsAndLegacy",
        "MountainPlanner.M5.SiteContextCompositeAssembler.AtomicInstallAndPathMapping",
    ),
    "MountainPlanner.M5.TerrainCoreRepository": (
        "MountainPlanner.M5.TerrainCoreRepository.LegacyProvenanceUnavailable",
        "MountainPlanner.M5.TerrainCoreRepository.VerifiedProvenanceSidecar",
    ),
    "MountainPlanner.M5.ImageryPyramid": (
        "MountainPlanner.M5.ImageryPyramid.Cancellation",
        "MountainPlanner.M5.ImageryPyramid.ExactContentVerification",
        "MountainPlanner.M5.ImageryPyramid.LayoutAndStorage",
    ),
    "MountainPlanner.M5.OsmVectorPackage": (
        "MountainPlanner.M5.OsmVectorPackage.GeometryIdsBoundsAndAttribution",
        "MountainPlanner.M5.OsmVectorPackage.HashStorageAndDeterminism",
        "MountainPlanner.M5.OsmVectorPackage.ProviderGatewaySeamAndCancellation",
        "MountainPlanner.M5.OsmVectorPackage.RequiredLayersAndLegacyIndependentParsing",
    ),
    "MountainPlanner.M5.OverpassVectorProvider": (
        "MountainPlanner.M5.OverpassVectorProvider.BoundedQueryAndDeterministicNormalization",
        "MountainPlanner.M5.OverpassVectorProvider.RejectsMalformedDuplicateAndMissingData",
        "MountainPlanner.M5.OverpassVectorProvider.ResponseBoundsAndCancellation",
    ),
    "MountainPlanner.M5.ImageryAcquisition": (
        "MountainPlanner.M5.ImageryAcquisition.BudgetsCancellationAndNoData",
        "MountainPlanner.M5.ImageryAcquisition.ProductionGatewayEntryPointCancellation",
        "MountainPlanner.M5.ImageryAcquisition.ScriptedReprojectionAndHash",
    ),
    "SkiPreparation.M6.TenKmResourcePreflight": (
        "SkiPreparation.M6.TenKmResourcePreflight.DetectsOverflowAndInsufficientDisk",
        "SkiPreparation.M6.TenKmResourcePreflight.FailsClosedOnUnknownCoverageAndSize",
        "SkiPreparation.M6.TenKmResourcePreflight.LedgerAtTwoAndFourKm",
        "SkiPreparation.M6.TenKmResourcePreflight.TenKmLedgerReportsCurrentImageryCap",
    ),
}
DEFAULT_TERRAINCORE_CACHE_BYTES = 512 * 1024 * 1024
PICKER_VIEWPORT_RESOLUTIONS = ((1280, 720), (1920, 1080), (2560, 1080),
                              (2560, 1440), (576, 1024))
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_PICKER_PNG_BYTES = 64 * 1024 * 1024
MAX_PICKER_PNG_DECODED_BYTES = 64 * 1024 * 1024
SHA256_ID_PATTERN = re.compile(r"[0-9a-f]{64}")
DEVELOPMENT_TRACE_LISTENER_PORT = 1985
DEVELOPMENT_TRACE_LISTENER_POLICY = (
    "Development picker-viewport only: packaged app process may listen on "
    "127.0.0.1 or ::1 port 1985 for in-process Unreal Trace; remote TCP endpoints remain forbidden; "
    "TCP is sampled every nominal 10 ms, so shorter-lived endpoints may be missed"
)
TCP_AUDIT_SAMPLING_LIMITATION = (
    "Sampled process-attributed TCP polling at a nominal 10 ms interval; an endpoint that "
    "opens and closes between snapshots may be missed."
)
FORBIDDEN_SHIPPING_PLUGINS = (
    "ModelContextProtocol", "EditorToolset", "AutomationTestToolset",
    "SlateInspectorToolset", "UMGToolSet", "WebBrowserWidget",
)
P1_TESTS = tuple(sorted(
    TIFF_TESTS + ACQUISITION_TESTS + PLACE_SEARCH_TESTS + SITE_PICKER_TESTS
    + PRESENTATION_INSTALLED_TERRAIN_TESTS
    + UI_TESTS + TERRAINCORE_TESTS + TERRAIN_SCRATCH_TESTS + P1_PRODUCT_TESTS
    + (
        "MountainPlanner.P1.Preparation.PackageContract",
        "MountainPlanner.P1.Preparation.ProviderDiagnostics",
    )
))

spec = importlib.util.spec_from_file_location("p0_harness", ROOT / "Tools/Build/p0.py")
if spec is None or spec.loader is None:
    raise RuntimeError("Cannot load the committed P0 harness")
p0 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p0)


def required_inputs() -> dict:
    required = [
        "Source/SkiPreparation/SkiPreparation.Build.cs",
        "Source/SkiTerrainRuntime/SkiTerrainRuntime.Build.cs",
        "Source/SkiPreparation/Public/SkiPreparation/TerrainPackageStore.h",
        "Source/SkiPreparation/Public/SkiPreparation/TerrainCorePackageStore.h",
        "Source/SkiPreparation/Public/SkiPreparation/TerrainCoreDerivation.h",
        "Source/SkiPreparation/Public/SkiPreparation/CoverEcologyStore.h",
        "Source/SkiPreparation/Public/SkiPreparation/WorldCoverCogDecoder.h",
        "Source/SkiPreparation/Public/SkiPreparation/TerrainAcquisition.h",
        "Source/SkiPreparation/Public/SkiPreparation/SkiSiteSelection.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/SkiTerrainActor.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreTileCache.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreLodController.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreMesh.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreRepository.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreEditedRepository.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreSession.h",
        "Source/SkiDomain/Public/SkiDomain/TerrainCore.h",
        "Source/SkiDomain/Public/SkiDomain/CoverEcology.h",
        "Content/P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64",
        "Content/P1Fixtures/worldcover-class-cog-synthetic.tif.base64",
        "Tools/Preparation/generate_tiff_fixture.py",
        "Tools/Preparation/generate_worldcover_cog_fixture.py",
        "docs/UnrealRebuild/P1-runbook.md",
        "docs/UnrealRebuild/P1-requirement-matrix.md",
        ".codex/config.toml",
        "Start-Unreal-MCP.bat",
    ]
    missing = [value for value in required if not (ROOT / value).is_file()]
    if missing:
        raise p0.Failed("P1 inputs missing: " + ", ".join(missing))
    project = json.loads(PROJECT.read_text(encoding="utf-8"))
    modules = {entry["Name"] for entry in project["Modules"]}
    if not {"SkiPreparation", "SkiTerrainRuntime"}.issubset(modules):
        raise p0.Failed("P1 runtime modules are absent from the project descriptor")
    plugins = {entry["Name"]: entry for entry in project["Plugins"]}
    editor_tools = {"ModelContextProtocol", "EditorToolset", "AutomationTestToolset",
                    "SlateInspectorToolset", "UMGToolSet"}
    if "AllToolsets" in plugins or any(plugins.get(name, {}).get("TargetAllowList") != ["Editor"]
                                       for name in editor_tools):
        raise p0.Failed("Unreal MCP must enable only the selected editor-only toolsets")
    codex_config = (ROOT / ".codex/config.toml").read_text(encoding="utf-8")
    if '[mcp_servers.unreal-mcp]' not in codex_config or 'url = "http://127.0.0.1:8000/mcp"' not in codex_config:
        raise p0.Failed("Project-local Unreal MCP endpoint is missing or is not loopback-only")
    if plugins.get("WebBrowserWidget", {}).get("Enabled"):
        raise p0.Failed("CEF WebBrowserWidget remains enabled after native frontend cutover")
    return {"status": "PASS", "required_inputs": required,
            "frontend": "native Unreal"}


def retained_checks() -> dict:
    manifest = json.loads((ROOT / "Tools/Build/domain-sources.json").read_text(encoding="utf-8"))
    actual_sources = {path.relative_to(ROOT).as_posix() for path in (ROOT / "Source/SkiDomain/Private").rglob("*.cpp")
                      if path.name != "SkiDomainModule.cpp"}
    actual_headers = {path.relative_to(ROOT).as_posix() for path in (ROOT / "Source/SkiDomain/Public").rglob("*.h")}
    if set(manifest["sources"]) != actual_sources or set(manifest["headers"]) != actual_headers:
        raise p0.Failed("Pure C++ source manifest differs from UBT discovery")
    p0.checked(["node", "scripts/checkAgentDocs.mjs"])
    p0.checked([sys.executable, "Tools/Preparation/check_inventory.py"])
    provenance = json.loads((ROOT / "docs/UnrealRebuild/provenance.json").read_text(encoding="utf-8"))
    archive = ROOT / "docs/UnrealRebuild/planning-pack-v0.9.zip"
    if p0.sha(archive) != provenance["archive_sha256"]:
        raise p0.Failed("Planning archive differs from the reviewed input")
    with zipfile.ZipFile(archive) as pack:
        if pack.testzip() is not None:
            raise p0.Failed("Planning archive CRC failed")
        prefix = "unreal-port-plan-v0.9/"
        delivery = json.loads(pack.read(prefix + "MANIFEST.json"))
        expected = {entry["path"] for entry in delivery["files"]}
        actual = {name.removeprefix(prefix) for name in pack.namelist()} - {"MANIFEST.json"}
        if actual != expected:
            raise p0.Failed("Planning archive manifest coverage differs")
        for entry in delivery["files"]:
            data = pack.read(prefix + entry["path"])
            if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise p0.Failed("Planning archive entry differs: " + entry["path"])
    return {"status": "PASS", "pure_sources": len(actual_sources), "archive_entries": len(expected)}


def require_exact_automation(output: str, expected: tuple[str, ...], test_filter: str) -> None:
    found = re.findall(rf"Found (\d+) automation tests based on '{re.escape(test_filter)}'", output)
    if found != [str(len(expected))]:
        raise p0.Failed(f"Automation discovery count is not exact for {test_filter}: {found}")
    started = re.findall(r"Test Started\..*?Path=\{([^}]+)\}", output)
    completed = re.findall(r"Test Completed\. Result=\{([^}]+)\}.*?Path=\{([^}]+)\}", output)
    completed_paths = [path for _, path in completed]
    duplicates = sorted({path for path in completed_paths if completed_paths.count(path) != 1})
    unexpected = sorted(set(completed_paths) - set(expected))
    missing = sorted(set(expected) - set(completed_paths))
    failed = sorted(path for result, path in completed if result != "Success")
    if sorted(started) != sorted(expected) or duplicates or unexpected or missing or failed:
        raise p0.Failed("Automation execution set/result is not exact: "
                        f"missing={missing}, unexpected={unexpected}, duplicates={duplicates}, failed={failed}")
    if "Automation Test Queue Empty" not in output:
        raise p0.Failed("Automation queue did not report completion")


def require_exact_ctest(discovery_text: str, junit_path: Path,
                        expected: tuple[str, ...]) -> list[dict]:
    """Bind the exact configured CTest identities to one machine-readable result set."""
    try:
        discovery = json.loads(discovery_text)
        configured = discovery["tests"]
        discovered_names = [entry["name"] for entry in configured]
    except (ValueError, KeyError, TypeError) as error:
        raise p0.Failed("CTest discovery output is malformed") from error
    if sorted(discovered_names) != sorted(expected) or len(discovered_names) != len(expected):
        raise p0.Failed(
            f"CTest discovery set is not exact: expected={sorted(expected)}, "
            f"found={sorted(discovered_names)}")
    if not junit_path.is_file():
        raise p0.Failed("CTest did not produce its requested JUnit result")
    try:
        root = ET.parse(junit_path).getroot()
    except (ET.ParseError, OSError) as error:
        raise p0.Failed("CTest JUnit result is malformed") from error
    cases = list(root.iter("testcase"))
    result_names = [case.attrib.get("name", "") for case in cases]
    failed = [case.attrib.get("name", "") for case in cases
              if any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))]
    if sorted(result_names) != sorted(expected) or len(result_names) != len(expected) or failed:
        raise p0.Failed(
            f"CTest result set is not exact: expected={sorted(expected)}, "
            f"found={sorted(result_names)}, failed={failed}")
    configured_by_name = {entry["name"]: entry for entry in configured}
    receipts = []
    for name in expected:
        command = configured_by_name[name].get("command", [])
        if not command or not Path(command[0]).is_file():
            raise p0.Failed(f"CTest identity is not bound to an executable: {name}")
        executable = Path(command[0]).resolve()
        receipts.append({"name": name, "result": "PASS",
                         "executable": str(executable),
                         "executable_sha256": p0.sha(executable)})
    return receipts


def shipping_target_module_proof(root: Path = ROOT) -> dict:
    """Prove the cooked Shipping target is Game and excludes editor MCP toolsets."""
    target = root / "Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.target"
    if not target.is_file():
        raise p0.Failed("Shipping UBT target receipt is missing")
    try:
        receipt = json.loads(target.read_text(encoding="utf-8-sig"))
        plugins = receipt["BuildPlugins"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise p0.Failed("Shipping UBT target receipt is malformed") from error
    if receipt.get("TargetName") != "SkiAreaDesignChallenge" \
            or receipt.get("Platform") != "Win64" \
            or receipt.get("Configuration") != "Shipping" \
            or receipt.get("TargetType") != "Game" \
            or receipt.get("IsTestTarget") is not False:
        raise p0.Failed("UBT receipt does not identify the Shipping Game target")
    forbidden_folded = {name.casefold() for name in FORBIDDEN_SHIPPING_PLUGINS}
    present = sorted(name for name in plugins if name.casefold() in forbidden_folded)
    if present:
        raise p0.Failed("Editor MCP/toolset plugins entered the Shipping target: "
                        + ", ".join(present))
    target_rules = root / "Source/SkiAreaDesignChallenge.Target.cs"
    rules = target_rules.read_text(encoding="utf-8")
    if "TargetType.Game" not in rules or 'ExtraModuleNames.Add("SkiPresentation")' not in rules:
        raise p0.Failed("Shipping target rules no longer bind the runtime presentation module")
    return {"status": "PASS", "target": "SkiAreaDesignChallenge",
            "platform": "Win64", "configuration": "Shipping", "target_type": "Game",
            "root_module": "SkiPresentation",
            "target_receipt": str(target), "target_receipt_sha256": p0.sha(target),
            "target_rules_sha256": p0.sha(target_rules),
            "forbidden_plugins_absent": list(FORBIDDEN_SHIPPING_PLUGINS)}


def validate_tcp_audit(audit: dict, *, require_no_connections: bool,
                       label: str = "Packaged process") -> None:
    if audit.get("method") != "GetExtendedTcpTable process-attributed polling" \
            or audit.get("snapshots", 0) < 1 \
            or audit.get("all_observed_pids_exited") is not True \
            or audit.get("last_live_pids") != []:
        raise p0.Failed(f"{label}: TCP audit is missing or was not observed")
    if audit.get("listen_ports"):
        raise p0.Failed(f"{label}: opened TCP listeners: {audit['listen_ports']}")
    if require_no_connections and audit.get("remote_endpoints"):
        owners = audit.get("endpoint_owners") or []
        detail = ", ".join(f"{owner.get('image')}#{owner.get('pid')}->{owner.get('remote')}"
                           for owner in owners) or str(audit["remote_endpoints"])
        raise p0.Failed(
            f"{label}: attempted TCP connections under a zero-network policy: {detail}")


def _tcp_endpoint_host_port(endpoint: str) -> tuple[str, int] | None:
    if not isinstance(endpoint, str):
        return None
    if endpoint.startswith("["):
        end = endpoint.find("]")
        if end < 0 or endpoint[end + 1:end + 2] != ":":
            return None
        host, port_text = endpoint[1:end], endpoint[end + 2:]
    else:
        host, separator, port_text = endpoint.rpartition(":")
        if not separator or ":" in host:
            return None
    try:
        return host, int(port_text)
    except ValueError:
        return None


def validate_development_picker_viewport_tcp_audit(
        audit: dict, *, configuration: str, scenario: str, expected_image: str,
        label: str = "picker-viewport") -> dict:
    """Allow only the packaged app's Development Unreal Trace listener for picker QA."""
    if configuration != "Development" or scenario != "picker-viewport":
        raise p0.Failed(
            "Development Trace listener exception is restricted to picker-viewport")

    # Reuse the strict lifetime check while validating connections and listeners below.
    validate_tcp_audit({**audit, "listen_ports": [], "remote_endpoints": [],
                        "endpoint_owners": []}, require_no_connections=True, label=label)
    remote_endpoints = audit.get("remote_endpoints")
    endpoint_owners = audit.get("endpoint_owners")
    if not isinstance(remote_endpoints, list) or not isinstance(endpoint_owners, list):
        raise p0.Failed(f"{label}: TCP audit lacks remote-connection evidence")
    if remote_endpoints or endpoint_owners:
        owners = endpoint_owners
        detail = ", ".join(f"{owner.get('image')}#{owner.get('pid')}->{owner.get('remote')}"
                           for owner in owners if isinstance(owner, dict))
        raise p0.Failed(f"{label}: remote TCP endpoints are forbidden: "
                        f"{detail or remote_endpoints}")

    root_pid = audit.get("root_pid")
    observed_pids = audit.get("observed_pids")
    process_images = audit.get("process_images")
    if type(root_pid) is not int or not isinstance(observed_pids, list) \
            or any(type(pid) is not int for pid in observed_pids) \
            or root_pid not in observed_pids or not isinstance(process_images, dict):
        raise p0.Failed(f"{label}: TCP audit lacks packaged-process attribution")
    child_pids = sorted(set(observed_pids) - {root_pid})
    if child_pids:
        raise p0.Failed(f"{label}: unexpected packaged child helper processes: {child_pids}")
    root_image = process_images.get(str(root_pid), "")
    if not isinstance(expected_image, str) or not expected_image \
            or str(root_image).casefold() != Path(expected_image).name.casefold():
        raise p0.Failed(
            f"{label}: TCP audit root image is not the packaged app: {root_image}")

    listener_owners = audit.get("listener_owners")
    listen_ports = audit.get("listen_ports")
    if not isinstance(listener_owners, list) or not listener_owners \
            or not isinstance(listen_ports, list):
        raise p0.Failed(f"{label}: expected the Development Unreal Trace listener was not observed")
    observed_ports = set()
    for owner in listener_owners:
        if not isinstance(owner, dict):
            raise p0.Failed(f"{label}: malformed TCP listener owner")
        local = _tcp_endpoint_host_port(owner.get("local"))
        if local is None:
            raise p0.Failed(f"{label}: malformed TCP listener endpoint: {owner.get('local')}")
        host, port = local
        try:
            address = ipaddress.ip_address(host)
        except ValueError as error:
            raise p0.Failed(
                f"{label}: malformed TCP listener address: {owner.get('local')}") from error
        if port != DEVELOPMENT_TRACE_LISTENER_PORT \
                or address not in (ipaddress.ip_address("127.0.0.1"),
                                   ipaddress.ip_address("::1")) \
                or owner.get("pid") != root_pid \
                or str(owner.get("image", "")).casefold() != Path(expected_image).name.casefold():
            raise p0.Failed(
                f"{label}: listener outside the packaged app's loopback Unreal Trace exception: "
                f"{owner.get('image')}#{owner.get('pid')}@{owner.get('local')}")
        observed_ports.add(port)
    if listen_ports != sorted(observed_ports):
        raise p0.Failed(f"{label}: TCP listener summary does not match owner records")

    return {
        "developmentTraceListenerObserved": True,
        "developmentTraceListenerPolicyException": DEVELOPMENT_TRACE_LISTENER_POLICY,
        "networkRemoteEndpoints": [],
        "networkRemoteZero": True,
    }


def require_no_browser_profile(user_dir: Path, label: str) -> None:
    if any(user_dir.rglob("webcache_*")):
        raise p0.Failed(f"{label}: a browser profile was created after native frontend cutover")


def exact_tree_manifest(directory: Path) -> dict:
    """Hash every file in a generated contract tree for byte-for-byte comparisons."""
    if not directory.is_dir():
        raise p0.Failed(f"Required generated tree is missing: {directory.name}")
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise p0.Failed(f"Generated tree contains a link: {path}")
        if path.is_file():
            files[path.relative_to(directory).as_posix()] = p0.sha(path)
    if not files:
        raise p0.Failed(f"Required generated tree is empty: {directory.name}")
    digest = hashlib.sha256(json.dumps(files, sort_keys=True,
                                       separators=(",", ":")).encode()).hexdigest()
    return {"sha256": digest, "files": files}


def terraincore_contract_trees(data_root: Path) -> dict:
    return {name: exact_tree_manifest(data_root / name)
            for name in ("TerrainCore", "TerrainEdits")}


def installed_medium_contract_trees(data_root: Path) -> dict:
    return {name: exact_tree_manifest(data_root / name)
            for name in ("TerrainCore", "TerrainEdits", "CoverEcology", "InstalledTerrain")}


def require_terraincore_renderer(receipt: dict, label: str) -> None:
    rendered = receipt.get("renderedTileCount")
    if receipt.get("rendererPath") is not True \
            or type(rendered) is not int or rendered <= 0 \
            or receipt.get("revisionAligned") is not True \
            or receipt.get("actorPicked") is not True \
            or receipt.get("actorMutationObserved") is not True \
            or receipt.get("actorStaleMeshRejected") is not True \
            or receipt.get("syntheticGuestMarkers") != 3000 \
            or type(receipt.get("overlaySegments")) is not int \
            or receipt.get("overlaySegments") <= 0:
        raise p0.Failed(f"{label} did not prove the production TerrainCore renderer path")


def require_acquisition_port_guard(receipt: dict) -> None:
    calls = receipt.get("acquisitionTransportCalls")
    if receipt.get("acquisitionPortGuardInstalled") is not True \
            or type(calls) is not int or calls != 0:
        raise p0.Failed(
            "Offline reopen lacks production acquisition-port denial with zero calls")


def require_offline_reopen_receipt(receipt: dict, content_id: str,
                                   edit_set_id: str) -> None:
    if receipt.get("scenario") != "offline-reopen" \
            or receipt.get("contentId") != content_id \
            or receipt.get("editSetId") != edit_set_id \
            or receipt.get("ready") is not True \
            or receipt.get("picked") is not True \
            or receipt.get("reopened") is not True:
        raise p0.Failed(
            "Packaged offline-reopen receipt identities or qualification steps do not match the request")
    if receipt.get("offlineReopen") is not True \
            or receipt.get("editDeltaReconstructed") is not True:
        raise p0.Failed(
            "Packaged offline-reopen receipt lacks reopen or reconstructed edit-delta proof")
    require_acquisition_port_guard(receipt)


def _decode_png_scanlines(data: bytes, rows: list[tuple[int, int]],
                          bit_depth: int, color_type: int,
                          palette_entries: int | None) -> bool:
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    bytes_per_pixel = max(1, (channels * bit_depth + 7) // 8)
    offset = 0

    for pass_width, pass_height in rows:
        if pass_width == 0 or pass_height == 0:
            continue
        row_bytes = (pass_width * channels * bit_depth + 7) // 8
        previous = bytearray(row_bytes)
        for _ in range(pass_height):
            if offset + row_bytes + 1 > len(data):
                return False
            filter_type = data[offset]
            offset += 1
            if filter_type > 4:
                return False
            encoded = data[offset:offset + row_bytes]
            offset += row_bytes
            decoded = bytearray(row_bytes)
            for index, value in enumerate(encoded):
                left = decoded[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                above = previous[index]
                upper_left = (previous[index - bytes_per_pixel]
                              if index >= bytes_per_pixel else 0)
                if filter_type == 0:
                    predictor = 0
                elif filter_type == 1:
                    predictor = left
                elif filter_type == 2:
                    predictor = above
                elif filter_type == 3:
                    predictor = (left + above) // 2
                else:
                    estimate = left + above - upper_left
                    distances = (abs(estimate - left), abs(estimate - above),
                                 abs(estimate - upper_left))
                    predictor = (left if distances[0] <= distances[1]
                                 and distances[0] <= distances[2]
                                 else above if distances[1] <= distances[2]
                                 else upper_left)
                decoded[index] = (value + predictor) & 0xff

            if color_type == 3:
                if palette_entries is None:
                    return False
                mask = (1 << bit_depth) - 1
                for pixel in range(pass_width):
                    bit_offset = pixel * bit_depth
                    palette_index = (decoded[bit_offset // 8]
                                     >> (8 - bit_depth - bit_offset % 8)) & mask
                    if palette_index >= palette_entries:
                        return False
            previous = decoded

    return offset == len(data)


def _valid_png_image(png: bytes, expected_width: int, expected_height: int) -> bool:
    """Validate PNG chunks, CRCs, zlib pixels, and scanline filters with bounded output."""
    if not (0 < expected_width <= 2560 and 0 < expected_height <= 1440
            and expected_width * expected_height <= 2560 * 1440
            and len(png) <= MAX_PICKER_PNG_BYTES
            and png.startswith(PNG_SIGNATURE)):
        return False

    offset = len(PNG_SIGNATURE)
    saw_ihdr = False
    saw_plte = False
    saw_idat = False
    idat_closed = False
    saw_iend = False
    palette_entries = None
    decoder = None
    decoded = bytearray()
    expected_decoded_bytes = 0
    rows: list[tuple[int, int]] = []
    bit_depth = 0
    color_type = 0
    compressed_bytes = 0

    try:
        while offset < len(png):
            if len(png) - offset < 12:
                return False
            chunk_length = struct.unpack_from(">I", png, offset)[0]
            chunk_type = png[offset + 4:offset + 8]
            chunk_end = offset + 12 + chunk_length
            if chunk_end > len(png) or not all(
                    65 <= byte <= 90 or 97 <= byte <= 122 for byte in chunk_type) \
                    or chunk_type[2] & 0x20:
                return False
            chunk_data_start = offset + 8
            chunk_data_end = chunk_data_start + chunk_length
            chunk_data = png[chunk_data_start:chunk_data_end]
            actual_crc = zlib.crc32(chunk_type + chunk_data) & 0xffffffff
            expected_crc = struct.unpack_from(">I", png, chunk_data_end)[0]
            if actual_crc != expected_crc:
                return False

            if not saw_ihdr:
                if chunk_type != b"IHDR" or chunk_length != 13:
                    return False
            elif chunk_type == b"IHDR":
                return False

            if chunk_type == b"IHDR":
                width, height, bit_depth, color_type, compression, filtering, interlace = \
                    struct.unpack(">IIBBBBB", chunk_data)
                valid_depths = {
                    0: (1, 2, 4, 8, 16),
                    2: (8, 16),
                    3: (1, 2, 4, 8),
                    4: (8, 16),
                    6: (8, 16),
                }
                if width != expected_width or height != expected_height \
                        or color_type not in valid_depths \
                        or bit_depth not in valid_depths[color_type] \
                        or compression != 0 or filtering != 0 or interlace not in (0, 1):
                    return False
                channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
                if interlace == 0:
                    passes = ((0, 0, 1, 1),)
                else:
                    passes = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8),
                              (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2),
                              (0, 1, 1, 2))
                pixel_bytes = 0
                for start_x, start_y, step_x, step_y in passes:
                    pass_width = (max(0, (width - start_x + step_x - 1) // step_x))
                    pass_height = (max(0, (height - start_y + step_y - 1) // step_y))
                    if pass_width and pass_height:
                        row_bytes = (pass_width * channels * bit_depth + 7) // 8
                        rows.append((pass_width, pass_height))
                        pixel_bytes += row_bytes * pass_height
                        expected_decoded_bytes += (row_bytes + 1) * pass_height
                if pixel_bytes <= 0 \
                        or expected_decoded_bytes > MAX_PICKER_PNG_DECODED_BYTES:
                    return False
                decoder = zlib.decompressobj()
                saw_ihdr = True
            elif chunk_type == b"PLTE":
                if saw_idat or saw_plte or color_type in (0, 4) \
                        or chunk_length == 0 or chunk_length > 768 \
                        or chunk_length % 3:
                    return False
                palette_entries = chunk_length // 3
                if color_type == 3 and palette_entries > (1 << bit_depth):
                    return False
                saw_plte = True
            elif chunk_type == b"IDAT":
                if idat_closed or (color_type == 3 and not saw_plte) or decoder is None:
                    return False
                saw_idat = True
                compressed_bytes += chunk_length
                if compressed_bytes > MAX_PICKER_PNG_BYTES:
                    return False
                decoded.extend(decoder.decompress(
                    chunk_data, expected_decoded_bytes + 1 - len(decoded)))
                if len(decoded) > expected_decoded_bytes \
                        or decoder.unconsumed_tail or decoder.unused_data:
                    return False
            elif chunk_type == b"IEND":
                if not saw_idat or chunk_length != 0 or chunk_end != len(png):
                    return False
                saw_iend = True
            else:
                if saw_idat:
                    idat_closed = True
                if not (chunk_type[0] & 0x20):
                    return False

            if saw_idat and chunk_type != b"IDAT":
                idat_closed = True
            offset = chunk_end
            if chunk_type == b"IEND":
                break

        if not saw_ihdr or not saw_idat or not saw_iend or not compressed_bytes \
                or decoder is None or not decoder.eof or decoder.unused_data \
                or decoder.unconsumed_tail or len(decoded) != expected_decoded_bytes:
            return False
        return _decode_png_scanlines(decoded, rows, bit_depth, color_type, palette_entries)
    except (OverflowError, struct.error, zlib.error):
        return False


def require_picker_viewport_capture(receipt: dict, token: str, width: int,
                                    height: int, screenshot: Path) -> dict:
    """Validate a Development picker PNG and step-specific native UMG evidence."""
    rects = receipt.get("rects", {})
    texts = receipt.get("texts", {})
    states = receipt.get("stepStates", {})
    step1 = states.get("step1", {}) if isinstance(states, dict) else {}
    step2 = states.get("step2", {}) if isinstance(states, dict) else {}
    step3 = states.get("step3", {}) if isinstance(states, dict) else {}
    step1_rects = step1.get("rects", {}) if isinstance(step1, dict) else {}
    step2_rects = step2.get("rects", {}) if isinstance(step2, dict) else {}
    step3_rects = step3.get("rects", {}) if isinstance(step3, dict) else {}
    bottom_rects = step3.get("bottomRects", {}) if isinstance(step3, dict) else {}
    step1_texts = step1.get("texts", {}) if isinstance(step1, dict) else {}
    step2_texts = step2.get("texts", {}) if isinstance(step2, dict) else {}
    step3_texts = step3.get("texts", {}) if isinstance(step3, dict) else {}

    def valid_rect(value: object) -> bool:
        return (isinstance(value, list) and len(value) == 4
                and all(type(part) in (int, float) and math.isfinite(part) for part in value)
                and value[2] > 0 and value[3] > 0)

    def inside(inner: object, outer: object, tolerance: float = 1.0) -> bool:
        return (valid_rect(inner) and valid_rect(outer)
                and inner[0] >= outer[0] - tolerance
                and inner[1] >= outer[1] - tolerance
                and inner[0] + inner[2] <= outer[0] + outer[2] + tolerance
                and inner[1] + inner[3] <= outer[1] + outer[3] + tolerance)

    def scroll_is_at_end(offset: object, maximum: object) -> bool:
        return (type(offset) in (int, float) and type(maximum) in (int, float)
                and math.isfinite(offset) and math.isfinite(maximum)
                and maximum >= 0.0 and offset >= -1.0 and offset <= maximum + 1.0
                and (maximum <= 1.0 or offset >= maximum - 1.0))

    rect_names = ("panel", "map", "scroll", "heading", "subtitle", "steps")
    step1_names = ("locationControls", "locationHeading", "locationSearch",
                   "searchButton", "selectSiteButton")
    step2_names = ("boundaryHeading", "boundaryInstructions", "clearBoundary",
                   "boundaryStatus", "previewStatus")
    step3_names = ("boundaryControls", "boundaryHeading", "boundaryInstructions",
                   "nameControls", "resortNameHeading", "nameBox", "downloadButton",
                   "downloadLabel")
    bottom_names = ("nameControls", "resortNameHeading", "nameBox", "downloadButton",
                    "downloadLabel")
    step_items = rects.get("stepItems") if isinstance(rects, dict) else None
    step3_bottom_visible = (
        receipt.get("bottomContentReachable") is True
        and step3.get("locationVisible") is False
        and step3.get("boundaryVisible") is True
        and step3.get("nameVisible") is True
        and step3.get("selectionValid") is True
        and step3.get("downloadEnabled") is False
    )
    step3_bottom_rects_contained = (
        isinstance(step3_rects, dict)
        and all(valid_rect(step3_rects.get(name)) for name in bottom_names)
        and inside(step3_rects["resortNameHeading"], step3_rects["nameControls"])
        and inside(step3_rects["nameBox"], step3_rects["nameControls"])
        and inside(step3_rects["downloadButton"], step3_rects["nameControls"])
        and inside(step3_rects["downloadLabel"], step3_rects["downloadButton"])
        and isinstance(bottom_rects, dict)
        and all(valid_rect(bottom_rects.get(name)) for name in bottom_names)
        and isinstance(rects, dict) and valid_rect(rects.get("scroll"))
        and all(inside(bottom_rects[name], rects["scroll"]) for name in bottom_names)
        and inside(bottom_rects.get("resortNameHeading"), bottom_rects.get("nameControls"))
        and inside(bottom_rects.get("nameBox"), bottom_rects.get("nameControls"))
        and inside(bottom_rects.get("downloadButton"), bottom_rects.get("nameControls"))
        and inside(bottom_rects.get("downloadLabel"), bottom_rects.get("downloadButton"))
    )
    native_scroll_at_end = (
        receipt.get("capturedScrollPosition") == "end"
        and scroll_is_at_end(receipt.get("scrollAtEnd"), receipt.get("scrollMaximum"))
        and scroll_is_at_end(step3.get("scrollAtEnd"), step3.get("scrollMaximum"))
        and type(receipt.get("scrollAtEnd")) in (int, float)
        and type(step3.get("scrollAtEnd")) in (int, float)
        and type(receipt.get("scrollMaximum")) in (int, float)
        and type(step3.get("scrollMaximum")) in (int, float)
        and abs(step3["scrollAtEnd"] - receipt["scrollAtEnd"]) <= 1.0
        and abs(step3["scrollMaximum"] - receipt["scrollMaximum"]) <= 1.0
    )
    if not isinstance(rects, dict) or not isinstance(texts, dict) \
            or receipt.get("token") != token or receipt.get("scenario") != "picker-viewport" \
            or receipt.get("resolution") != [width, height] \
            or receipt.get("viewport") != [width, height] \
            or receipt.get("captureKind") != "rendered-viewport-png" \
            or receipt.get("capturedStep") != "name-resort" \
            or receipt.get("capturedScrollPosition") != "end" \
            or receipt.get("nativePickerVisible") is not True \
            or receipt.get("mapWidgetPresent") is not True \
            or receipt.get("networkDisabled") is not True \
            or receipt.get("topContentVisible") is not True \
            or receipt.get("bottomContentReachable") is not True \
            or receipt.get("visualReviewRequired") is not True \
            or not isinstance(texts.get("heading"), str) or "New resort" not in texts["heading"] \
            or texts.get("subtitle") != "Find the mountain you want to make your own." \
            or not isinstance(texts.get("steps"), list) or len(texts["steps"]) != 4 \
            or not all(isinstance(label, str) and label for label in texts["steps"]) \
            or not all(label in text for text, label in zip(
                texts["steps"], ("Choose location", "Define boundary", "Name resort", "Download"))) \
            or not isinstance(step_items, list) or len(step_items) != 4 \
            or not all(valid_rect(value) for value in step_items) \
            or not all(valid_rect(rects.get(name)) for name in rect_names) \
            or not isinstance(states, dict) or not all(isinstance(state, dict)
                                                        for state in (step1, step2, step3)) \
            or step1.get("locationVisible") is not True \
            or step1.get("boundaryVisible") is not False or step1.get("nameVisible") is not False \
            or step1.get("selectSiteInitiallyDisabled") is not True \
            or step1.get("selectSiteEnabledAfterSearch") is not True \
            or step1.get("locationQuery") != "47.25, -121.55" \
            or not isinstance(step1.get("searchStatus"), str) \
            or "Centered at" not in step1["searchStatus"] \
            or not isinstance(step1.get("activeLabel"), str) \
            or "Choose location" not in step1["activeLabel"] \
            or not isinstance(step1_texts, dict) \
            or not isinstance(step1_texts.get("locationHeading"), str) \
            or "Search a place" not in step1_texts["locationHeading"] \
            or not isinstance(step1_texts.get("searchButton"), str) \
            or "Search / go to coordinates" not in step1_texts["searchButton"] \
            or not isinstance(step1_texts.get("selectSiteButton"), str) \
            or "Select site" not in step1_texts["selectSiteButton"] \
            or not isinstance(step2_texts, dict) \
            or step2.get("locationVisible") is not False \
            or step2.get("boundaryVisible") is not True or step2.get("nameVisible") is not False \
            or not isinstance(step2.get("activeLabel"), str) \
            or "Define boundary" not in step2["activeLabel"] \
            or not isinstance(step2_texts.get("boundaryHeading"), str) \
            or "Define your boundary" not in step2_texts["boundaryHeading"] \
            or not isinstance(step2_texts.get("boundaryInstructions"), str) \
            or "Drag on the map" not in step2_texts["boundaryInstructions"] \
            or type(step2.get("scrollAtStart")) not in (int, float) \
            or step2["scrollAtStart"] > 1.0 \
            or not isinstance(step3_texts, dict) \
            or step3.get("locationVisible") is not False \
            or step3.get("boundaryVisible") is not True or step3.get("nameVisible") is not True \
            or step3.get("selectionValid") is not True or step3.get("downloadEnabled") is not False \
            or not isinstance(step3.get("activeLabel"), str) \
            or "Name resort" not in step3["activeLabel"] \
            or not isinstance(step3_texts.get("boundaryHeading"), str) \
            or "Define your boundary" not in step3_texts["boundaryHeading"] \
            or not isinstance(step3_texts.get("resortNameHeading"), str) \
            or "Name your resort" not in step3_texts["resortNameHeading"] \
            or not isinstance(step3_texts.get("downloadLabel"), str) \
            or "Download unavailable" not in step3_texts["downloadLabel"] \
            or not isinstance(step1_rects, dict) or not isinstance(step2_rects, dict) \
            or not isinstance(step3_rects, dict) or not isinstance(bottom_rects, dict) \
            or not all(valid_rect(rects.get(name)) for name in rect_names) \
            or not all(valid_rect(step1_rects.get(name)) for name in step1_names) \
            or not all(valid_rect(step2_rects.get(name)) for name in step2_names) \
            or not all(valid_rect(step3_rects.get(name)) for name in step3_names) \
            or not all(valid_rect(bottom_rects.get(name)) for name in bottom_names) \
            or type(receipt.get("scrollAtStart")) not in (int, float) \
            or not math.isfinite(receipt["scrollAtStart"]) \
            or receipt["scrollAtStart"] > 1.0 \
            or not native_scroll_at_end \
            or not step3_bottom_visible \
            or not step3_bottom_rects_contained:
        raise p0.Failed(f"Native picker viewport {width}x{height} receipt is invalid")

    if not inside(rects["panel"], rects["map"]) \
            or not all(inside(rects[name], rects["panel"])
                       for name in ("heading", "subtitle", "steps", "scroll")) \
            or any(not inside(value, rects["steps"]) for value in step_items) \
            or not inside(step1_rects["locationControls"], rects["scroll"]) \
            or any(not inside(step1_rects[name], step1_rects["locationControls"])
                   for name in step1_names[1:]) \
            or any(not inside(step2_rects[name], rects["scroll"]) for name in step2_names) \
            or any(not inside(bottom_rects[name], rects["scroll"]) for name in bottom_names) \
            or not inside(bottom_rects["downloadLabel"], bottom_rects["downloadButton"]):
        raise p0.Failed(f"Native picker viewport {width}x{height} has out-of-bounds content")
    if rects["map"][0] < -1 or rects["map"][1] < -1 \
            or rects["map"][0] + rects["map"][2] > width + 1 \
            or rects["map"][1] + rects["map"][3] > height + 1 \
            or rects["panel"][0] < -1 or rects["panel"][1] < -1 \
            or rects["panel"][0] + rects["panel"][2] > width + 1 \
            or rects["panel"][1] + rects["panel"][3] > height + 1:
        raise p0.Failed(f"Native picker viewport {width}x{height} leaves the viewport")

    supplied_screenshot = receipt.get("screenshotPath")
    if not isinstance(supplied_screenshot, str) \
            or Path(supplied_screenshot).resolve() != screenshot.resolve() \
            or not screenshot.is_file():
        raise p0.Failed(f"Native picker viewport {width}x{height} screenshot is missing")
    try:
        with screenshot.open("rb") as image:
            png = image.read(MAX_PICKER_PNG_BYTES + 1)
    except OSError as error:
        raise p0.Failed(
            f"Native picker viewport {width}x{height} screenshot is unreadable: {error}") from error
    screenshot_sha = hashlib.sha256(png).hexdigest()
    if len(png) > MAX_PICKER_PNG_BYTES \
            or not _valid_png_image(png, width, height) \
            or receipt.get("screenshotSha256") != screenshot_sha:
        raise p0.Failed(
            f"Native picker viewport {width}x{height} screenshot does not match its receipt or is not a complete, valid PNG")
    receipt["screenshot"] = str(screenshot)
    return receipt


def _windows_ipv4_tcp_rows(pid: int) -> list[dict]:
    """Return IPv4 TCP rows owned by pid without shelling out or requiring elevation."""
    if os.name != "nt":
        raise p0.Blocked("Process-attributed packaged TCP audit requires Windows")

    class TcpRow(ctypes.Structure):
        _fields_ = [("state", ctypes.c_ulong), ("local_addr", ctypes.c_ulong),
                    ("local_port", ctypes.c_ulong), ("remote_addr", ctypes.c_ulong),
                    ("remote_port", ctypes.c_ulong), ("pid", ctypes.c_ulong)]

    size = ctypes.c_ulong(0)
    api = ctypes.windll.iphlpapi.GetExtendedTcpTable
    api(None, ctypes.byref(size), False, socket.AF_INET, 5, 0)
    buffer = ctypes.create_string_buffer(size.value)
    status = api(buffer, ctypes.byref(size), False, socket.AF_INET, 5, 0)
    if status != 0:
        raise p0.Failed(f"GetExtendedTcpTable failed: {status}")
    count = ctypes.c_ulong.from_buffer(buffer).value
    rows = []
    offset = ctypes.sizeof(ctypes.c_ulong)
    for index in range(count):
        row = TcpRow.from_buffer_copy(buffer, offset + index * ctypes.sizeof(TcpRow))
        if row.pid != pid:
            continue
        local_port = socket.ntohs(row.local_port & 0xffff)
        remote_port = socket.ntohs(row.remote_port & 0xffff)
        local_addr = socket.inet_ntoa(struct.pack("<L", row.local_addr))
        remote_addr = socket.inet_ntoa(struct.pack("<L", row.remote_addr))
        rows.append({"state": int(row.state), "local": f"{local_addr}:{local_port}",
                     "remote": f"{remote_addr}:{remote_port}"})
    return rows


def _windows_ipv6_tcp_rows(pid: int) -> list[dict]:
    """Return IPv6 TCP rows owned by pid without shelling out or requiring elevation."""
    class Tcp6Row(ctypes.Structure):
        _fields_ = [("local_addr", ctypes.c_ubyte * 16),
                    ("local_scope", ctypes.c_ulong), ("local_port", ctypes.c_ulong),
                    ("remote_addr", ctypes.c_ubyte * 16),
                    ("remote_scope", ctypes.c_ulong), ("remote_port", ctypes.c_ulong),
                    ("state", ctypes.c_ulong), ("pid", ctypes.c_ulong)]

    size = ctypes.c_ulong(0)
    api = ctypes.windll.iphlpapi.GetExtendedTcpTable
    api(None, ctypes.byref(size), False, socket.AF_INET6, 5, 0)
    buffer = ctypes.create_string_buffer(size.value)
    status = api(buffer, ctypes.byref(size), False, socket.AF_INET6, 5, 0)
    if status != 0:
        raise p0.Failed(f"GetExtendedTcpTable IPv6 failed: {status}")
    count = ctypes.c_ulong.from_buffer(buffer).value
    rows = []
    offset = ctypes.sizeof(ctypes.c_ulong)
    for index in range(count):
        row = Tcp6Row.from_buffer_copy(buffer, offset + index * ctypes.sizeof(Tcp6Row))
        if row.pid != pid:
            continue
        local_port = socket.ntohs(row.local_port & 0xffff)
        remote_port = socket.ntohs(row.remote_port & 0xffff)
        local_addr = socket.inet_ntop(socket.AF_INET6, bytes(row.local_addr))
        remote_addr = socket.inet_ntop(socket.AF_INET6, bytes(row.remote_addr))
        rows.append({"state": int(row.state), "local": f"[{local_addr}]:{local_port}",
                     "remote": f"[{remote_addr}]:{remote_port}"})
    return rows


_PROCESS_IMAGES: dict[tuple[int, int], str] = {}


class _WindowsFileTime(ctypes.Structure):
    _fields_ = [("low", ctypes.c_ulong), ("high", ctypes.c_ulong)]


def _windows_process_creation_time_from_handle(handle) -> int:
    """Return a process object's creation FILETIME using its already-open handle."""
    kernel = ctypes.windll.kernel32
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p,
                                       ctypes.POINTER(_WindowsFileTime),
                                       ctypes.POINTER(_WindowsFileTime),
                                       ctypes.POINTER(_WindowsFileTime),
                                       ctypes.POINTER(_WindowsFileTime)]
    kernel.GetProcessTimes.restype = ctypes.c_int
    created = _WindowsFileTime()
    exited = _WindowsFileTime()
    kernel_time = _WindowsFileTime()
    user_time = _WindowsFileTime()
    if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                  ctypes.byref(kernel_time), ctypes.byref(user_time)):
        raise p0.Failed("GetProcessTimes failed during packaged TCP audit")
    return (int(created.high) << 32) | int(created.low)


def _windows_open_process_handle(pid: int, access: int):
    kernel = ctypes.windll.kernel32
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    return kernel.OpenProcess(access, False, pid)


def _windows_close_process_handle(handle) -> None:
    kernel = ctypes.windll.kernel32
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    kernel.CloseHandle(handle)


def _windows_process_creation_time(pid: int) -> int | None:
    """Return the current PID's creation FILETIME, or None if it has already exited."""
    handle = _windows_open_process_handle(pid, 0x1000)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        kernel = ctypes.windll.kernel32
        kernel.GetLastError.restype = ctypes.c_ulong
        error = int(kernel.GetLastError())
        if error == 87:  # ERROR_INVALID_PARAMETER: the PID exited after the process snapshot.
            return None
        raise p0.Failed(
            f"OpenProcess failed while identifying packaged descendant {pid}: {error}")
    try:
        return _windows_process_creation_time_from_handle(handle)
    finally:
        _windows_close_process_handle(handle)


def _windows_terminate_process_handle(handle) -> bool:
    kernel = ctypes.windll.kernel32
    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    kernel.TerminateProcess.restype = ctypes.c_int
    return bool(kernel.TerminateProcess(handle, 1))


def _windows_terminate_if_same_process(pid: int, creation_time: int) -> bool:
    """Terminate only the process object whose PID and creation time were audited."""
    handle = _windows_open_process_handle(
        pid, 0x0001 | 0x1000)  # PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        if _windows_process_creation_time_from_handle(handle) != creation_time:
            return False
        return _windows_terminate_process_handle(handle)
    finally:
        _windows_close_process_handle(handle)


def _windows_process_descendants(
        root_identity: tuple[int, int], known: set[tuple[int, int]]) \
        -> tuple[set[tuple[int, int]], set[tuple[int, int]]]:
    """Return observed (PID, creation-time) identities and live identities this snapshot."""
    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", ctypes.c_ulong), ("cntUsage", ctypes.c_ulong),
                    ("th32ProcessID", ctypes.c_ulong), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", ctypes.c_ulong), ("cntThreads", ctypes.c_ulong),
                    ("th32ParentProcessID", ctypes.c_ulong), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", ctypes.c_ulong), ("szExeFile", ctypes.c_wchar * 260)]

    kernel = ctypes.windll.kernel32
    kernel.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    snapshot = kernel.CreateToolhelp32Snapshot(0x00000002, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise p0.Failed("CreateToolhelp32Snapshot failed during packaged TCP audit")
    parents = {}
    images = {}
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(ProcessEntry)
        present = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while present:
            pid = int(entry.th32ProcessID)
            parents[pid] = int(entry.th32ParentProcessID)
            images[pid] = str(entry.szExeFile)
            present = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    tracked = set(known) | {root_identity}
    current_by_pid: dict[int, tuple[int, int]] = {}
    for pid in {tracked_pid for tracked_pid, _ in tracked}.intersection(parents):
        creation_time = _windows_process_creation_time(pid)
        identity = (pid, creation_time) if creation_time is not None else None
        if identity in tracked:
            current_by_pid[pid] = identity
            _PROCESS_IMAGES[identity] = images[pid]
    changed = True
    while changed:
        changed = False
        for candidate, parent in parents.items():
            if parent in current_by_pid and candidate not in current_by_pid:
                creation_time = _windows_process_creation_time(candidate)
                if creation_time is None:
                    continue
                identity = (candidate, creation_time)
                tracked.add(identity)
                current_by_pid[candidate] = identity
                _PROCESS_IMAGES[identity] = images[candidate]
                changed = True
    return tracked, set(current_by_pid.values())


def checked_with_tcp_audit(command: list[str], *, timeout: float, log: str,
                           include_listener_owners: bool = False,
                           allow_nonzero_exit: bool = False) -> dict:
    """Run one packaged process while independently polling its owned TCP table."""
    if os.name != "nt":
        raise p0.Blocked("Process-attributed packaged TCP audit requires Windows")
    log_path = p0.RUN_OUTPUT / log
    log_path.parent.mkdir(parents=True, exist_ok=True)
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    began = time.monotonic()
    snapshots = 0
    observed = set()
    tracked_processes: set[tuple[int, int]] = set()
    first_seen: dict[tuple[tuple[int, int], str], float] = {}
    _PROCESS_IMAGES.clear()
    with log_path.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, cwd=ROOT, stdout=stream,
                                   stderr=subprocess.STDOUT, text=True, **options)
        root_identity = None
        try:
            root_identity = (process.pid,
                             _windows_process_creation_time_from_handle(process._handle))
            tracked_processes.add(root_identity)
            while True:
                tracked_processes, live_processes = _windows_process_descendants(
                    root_identity, tracked_processes)
                for identity in live_processes:
                    tracked_pid, creation_time = identity
                    if _windows_process_creation_time(tracked_pid) != creation_time:
                        continue
                    rows = (_windows_ipv4_tcp_rows(tracked_pid)
                            + _windows_ipv6_tcp_rows(tracked_pid))
                    if _windows_process_creation_time(tracked_pid) != creation_time:
                        continue
                    for row in rows:
                        observed.add((identity, row["state"], row["local"], row["remote"]))
                        key = (identity, row["remote"])
                        if row["state"] != 2 and not row["remote"].endswith(":0") \
                                and key not in first_seen:
                            first_seen[key] = round((time.monotonic() - began) * 1000.0, 1)
                snapshots += 1
                if process.poll() is not None and not live_processes:
                    break
                if time.monotonic() - began > timeout:
                    raise p0.TimedOut(f"Timed out after {timeout}s: {command[0]}")
                time.sleep(0.01)
        except BaseException:
            for tracked_pid, creation_time in sorted(tracked_processes, reverse=True):
                if root_identity is not None \
                        and (tracked_pid, creation_time) == root_identity:
                    # Popen.kill uses its retained process handle, so it cannot hit a reused PID.
                    if process.poll() is None:
                        process.kill()
                    continue
                try:
                    _windows_terminate_if_same_process(tracked_pid, creation_time)
                except (OSError, p0.Failed):
                    pass
            if process.poll() is None:
                process.kill()
            process.wait(timeout=2)
            raise
    if process.returncode != 0 and not allow_nonzero_exit:
        output = log_path.read_text(encoding="utf-8", errors="replace")
        raise p0.Failed(f"Exit {process.returncode}: {command[0]}\n{output[-3000:]}")
    listen_ports = sorted({int(local.rsplit(":", 1)[1]) for _, state, local, _ in observed
                           if state == 2})
    remote_endpoints = sorted({remote for _, state, _, remote in observed
                               if state != 2 and not remote.endswith(":0")})
    observed_identities = sorted(tracked_processes)
    root_image = _PROCESS_IMAGES.get(root_identity, "unknown")
    process_images = {str(pid): image for (pid, _), image in
                      sorted((identity, _PROCESS_IMAGES.get(identity, "unknown"))
                             for identity in tracked_processes)}
    process_images[str(root_identity[0])] = root_image
    audit = {"method": "GetExtendedTcpTable process-attributed polling",
            "root_pid": process.pid, "root_process_creation_time": root_identity[1],
            "observed_pids": sorted({pid for pid, _ in observed_identities}),
            "observed_process_identities": [
                {"pid": pid, "creation_time_filetime": creation_time,
                 "image": _PROCESS_IMAGES.get((pid, creation_time), "unknown")}
                for pid, creation_time in observed_identities],
            "all_observed_pids_exited": True, "last_live_pids": [],
            "all_observed_processes_exited": True,
            "snapshots": snapshots,
            "poll_interval_milliseconds": 10, "sampling_mode": "sampled",
            "sampling_limitation": TCP_AUDIT_SAMPLING_LIMITATION,
            "listen_ports": listen_ports,
            "remote_endpoints": remote_endpoints,
            "endpoint_owners": [
                {"pid": identity[0], "process_creation_time_filetime": identity[1],
                 "image": _PROCESS_IMAGES.get(identity, "unknown"),
                 "remote": remote, "first_seen_ms": first_seen[(identity, remote)]}
                for identity, remote in sorted(first_seen)],
            "process_images": process_images,
            "process_return_code": process.returncode}
    if include_listener_owners:
        audit["listener_owners"] = [
            {"pid": identity[0], "process_creation_time_filetime": identity[1],
             "image": _PROCESS_IMAGES.get(identity, "unknown"), "local": local}
            for identity, state, local, _ in sorted(observed) if state == 2]
    log_path.with_suffix(".tcp-audit.json").write_text(
        json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    return audit


def automation(environment: dict, run_output: Path) -> dict:
    engine = p0.engine_path(environment)
    log_name = "p1-automation.log"
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=300, log=log_name)
    require_exact_automation(output, P1_TESTS, "MountainPlanner.P1")
    return {"status": "PASS", "kind": "unreal_automation", "filter": "MountainPlanner.P1",
            "tests": list(P1_TESTS)}


def focused_automation(environment: dict, run_output: Path, test_filter: str) -> dict:
    """Build and run one registered focused automation group with exact result checking."""
    expected = FOCUSED_AUTOMATION_TESTS.get(test_filter)
    if not expected:
        raise p0.Failed(f"Unknown or unregistered focused automation group: {test_filter}")
    build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    group_name = test_filter.rsplit(".", 1)[-1].lower()
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        f"-ExecCmds=Automation RunTests {test_filter};Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=300, log=f"focused-{group_name}-automation.log")
    require_exact_automation(output, expected, test_filter)
    return {"status": "PASS", "kind": "focused_automation", "filter": test_filter,
            "tests": list(expected), "build": build}


def tiff(environment: dict, run_output: Path) -> dict:
    build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.Preparation.GeoTiff;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=180, log="p1-tiff-automation.log")
    require_exact_automation(output, TIFF_TESTS, "MountainPlanner.P1.Preparation.GeoTiff")
    fixture = ROOT / "Content/P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64"
    import base64
    fixture_hash = hashlib.sha256(base64.b64decode(fixture.read_text(encoding="ascii"))).hexdigest()
    if fixture_hash != "4614b0cec77c843a6b00ec7d0a4b3b90511031ee9eb73f185c5656088fe599bf":
        raise p0.Failed("Immutable GeoTIFF fixture hash differs")
    return {"status": "PASS", "kind": "focused_geotiff", "tests": list(TIFF_TESTS),
            "fixture_sha256": fixture_hash, "build": build}


def acquisition(environment: dict, run_output: Path) -> dict:
    build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.Preparation.Acquisition;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=180, log="p1-acquisition-automation.log")
    require_exact_automation(output, ACQUISITION_TESTS, "MountainPlanner.P1.Preparation.Acquisition")
    return {"status": "PASS", "kind": "focused_acquisition", "tests": list(ACQUISITION_TESTS),
            "build": build}


def ui(environment: dict, run_output: Path) -> dict:
    build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.Presentation.UI;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=180, log="p1-ui-automation.log")
    require_exact_automation(output, UI_TESTS, "MountainPlanner.P1.Presentation.UI")
    return {"status": "PASS", "kind": "focused_ui", "tests": list(UI_TESTS), "build": build}


def terraincore(environment: dict, run_output: Path) -> dict:
    domain = p0.domain_checks(environment)
    cmake = environment["cmake"]
    build_root = OUTPUT / "domain-msvc"
    p0.checked([cmake, "--build", str(build_root), "--config", "Debug"],
               timeout=300, log="terraincore-domain-build.log")
    ctest_name = "ctest.exe" if os.name == "nt" else "ctest"
    ctest = Path(cmake).with_name(ctest_name)
    if not ctest.is_file():
        located = shutil.which(ctest_name)
        if not located:
            raise p0.Blocked("Exact TerrainCore native evidence requires CTest")
        ctest = Path(located)
    discovery = p0.checked([str(ctest), "--test-dir", str(build_root), "-C", "Debug",
                            "--show-only=json-v1"], timeout=30,
                           log="terraincore-ctest-discovery.json")
    junit_path = run_output / "terraincore-ctest-results.xml"
    p0.checked([str(ctest), "--test-dir", str(build_root), "-C", "Debug",
                "--output-on-failure", "--output-junit", str(junit_path)], timeout=60,
               log="terraincore-ctest-run.log")
    native_receipts = require_exact_ctest(
        discovery, junit_path, TERRAINCORE_NATIVE_TESTS)
    engine_build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.TerrainCore;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=300, log="p1-terraincore-automation.log")
    require_exact_automation(output, TERRAINCORE_TESTS, "MountainPlanner.P1.TerrainCore")
    return {"status": "PASS", "kind": "focused_terraincore",
            "native_tests": list(TERRAINCORE_NATIVE_TESTS),
            "native_receipts": native_receipts, "automation_tests": list(TERRAINCORE_TESTS),
            "domain": domain, "build": engine_build}


def p1_product(environment: dict, run_output: Path) -> dict:
    build = p0.native_build(environment, "Editor", "Development")
    engine = p0.engine_path(environment)
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.Product;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=300, log="p1-product-automation.log")
    require_exact_automation(output, P1_PRODUCT_TESTS, "MountainPlanner.P1.Product")
    return {"status": "PASS", "kind": "focused_p1_product",
            "tests": list(P1_PRODUCT_TESTS), "build": build}


def create_assets(environment: dict, before: dict) -> dict:
    engine = p0.engine_path(environment)
    command = [str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
               "-run=pythonscript", f"-script={ROOT / 'Tools/Editor/create_p1_assets.py'}",
               "-unattended", "-nop4", "-NullRHI", "-stdout", "-FullStdOutLogOutput"]
    p0.checked(command, timeout=300, log="p1-assets-first.log")
    middle = p0.source_snapshot()
    p0.checked(command, timeout=300, log="p1-assets-repeat.log")
    after = p0.source_snapshot()
    if middle["sha256"] != after["sha256"]:
        raise p0.Failed("Repeated P1 asset recipe changed source or asset bytes")
    sys.path.insert(0, str(ROOT / "Tools/Editor"))
    from p1_asset_ownership import ASSETS, RECEIPT, preflight, recipe_digest
    receipt = preflight(ROOT, recipe_digest(ROOT))
    allowed = set(ASSETS) | {RECEIPT}
    all_paths = set(before["files"]) | set(middle["files"])
    changed = {path for path in all_paths if before["files"].get(path) != middle["files"].get(path)}
    if not changed.issubset(allowed) or set(receipt["assets"]) != set(ASSETS):
        raise p0.Failed("P1 asset recipe changed files outside its ownership set or is incomplete")
    return {"status": "PASS", "kind": "p1_editor_asset_generation", "assets": receipt,
            "first_invocation": "PASS: owned update" if changed else "PASS: no-op",
            "second_invocation": "PASS: source and asset bytes unchanged"}


def verify_package_report(configuration: str, source_digest: str) -> tuple[dict, Path]:
    path = OUTPUT / f"package-{configuration}.json"
    if not path.is_file():
        raise p0.Blocked(f"Package {configuration} with p1.py before packaged smoke")
    report = json.loads(path.read_text(encoding="utf-8"))
    launcher = p0.verify_package_receipt(report, source_digest, configuration).resolve()
    if p0.sha(launcher) != report["result"]["launcher_sha256"]:
        raise p0.Failed("Recorded P1 packaged launcher changed")
    package_root = launcher.parent
    forbidden = ("modelcontextprotocol", "editortoolset", "automationtesttoolset",
                 "slateinspectortoolset", "umgtoolset")
    leaked = [path.relative_to(package_root).as_posix() for path in package_root.rglob("*")
              if path.is_file() and any(name in path.name.lower() for name in forbidden)]
    if leaked:
        raise p0.Failed("Editor-only MCP/toolset modules leaked into package: " + ", ".join(leaked[:10]))
    assert_no_browser_bundle(package_root)
    if configuration == "Shipping":
        recorded_proof = report["result"].get("shipping_mcp_proof")
        current_proof = shipping_target_module_proof()
        if recorded_proof != current_proof:
            raise p0.Failed("Shipping target/module MCP proof is missing, stale, or mismatched")
    return report, launcher


def assert_no_browser_bundle(package_root: Path) -> None:
    forbidden = ("cef3", "epicwebhelper", "unrealcefsubprocess", "p1selector", "maplibre-gl")
    leaked = []
    markers = tuple(name.encode("ascii") for name in forbidden)
    for path in package_root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(package_root).as_posix()
        if any(name in relative.lower() for name in forbidden):
            leaked.append(relative)
            continue
        if path.suffix.lower() not in (".pak", ".utoc"):
            continue
        # Archive indexes retain logical paths even when the payload is compressed.
        # Keep a suffix between reads so a marker split across chunks is detected.
        with path.open("rb") as stream:
            suffix = b""
            while chunk := stream.read(1024 * 1024):
                window = (suffix + chunk).lower()
                if any(marker in window for marker in markers):
                    leaked.append(relative)
                    break
                suffix = window[-32:]
    if leaked:
        raise p0.Failed("Browser helper or selector content leaked into native package: "
                        + ", ".join(leaked[:10]))


def parse_sha256_id(value: str) -> str:
    if not SHA256_ID_PATTERN.fullmatch(value):
        raise argparse.ArgumentTypeError("must be exactly 64 lowercase hexadecimal characters")
    return value


def smoke(configuration: str, source_digest: str, scenario: str, content_id: str | None,
          run_output: Path, edit_set_id: str | None = None) -> dict:
    if scenario == "picker-viewport" and configuration != "Development":
        raise p0.Failed("Native picker viewport screenshots are Development-only")
    package_report, launcher = verify_package_report(configuration, source_digest)
    token = str(uuid.uuid4())
    data_root = ((run_output / "isolated-data") if scenario in
                 ("frontend", "terraincore-regression", "medium-regression", "ui-layout",
                  "picker-viewport", "performance-regression")
                 else (OUTPUT / "isolated-data" / configuration)).resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    unreal_user_dir = data_root / "unreal-user" / token
    unreal_user_dir.mkdir(parents=True, exist_ok=True)
    receipt = data_root / f"{token}.receipt.json"
    smoke_flag = ("-SkiM1FrontEndSmoke" if scenario == "frontend" else
                  "-SkiP1UiLayoutSmoke" if scenario == "ui-layout" else
                  "-SkiP1PickerViewportSmoke" if scenario == "picker-viewport" else
                  "-SkiP1PerformanceSmoke" if scenario == "performance-regression"
                  else "-SkiP1Smoke")
    smoke_width, smoke_height = ((2560, 1440) if scenario == "performance-regression"
                                 else (1280, 720))
    command = [str(launcher), smoke_flag, f"-SkiP1Token={token}",
               f"-SkiP1Receipt={receipt}", f"-SkiP1DataRoot={data_root}",
               f"-SkiP1Scenario={scenario}", f"-UserDir={unreal_user_dir}",
               "-windowed", f"-ResX={smoke_width}", f"-ResY={smoke_height}",
               "-unattended", "-nosplash"]
    if scenario == "frontend":
        command.extend(("-RenderOffScreen", "-NullRHI"))
    if scenario == "offline-reopen":
        if not content_id or not SHA256_ID_PATTERN.fullmatch(content_id):
            raise p0.Failed("Offline reopen requires the exact contentId from an import receipt")
        if not edit_set_id or not SHA256_ID_PATTERN.fullmatch(edit_set_id):
            raise p0.Failed("Offline reopen requires the exact editSetId from an import receipt")
        command.extend((f"-SkiP1ContentId={content_id}",
                        f"-SkiP1EditSetId={edit_set_id}"))

    def collect_failure_context(reason: str, user_dir: Path | None = None) -> str:
        source_dir = user_dir if user_dir is not None else unreal_user_dir
        destination = run_output / "packaged-failure-context"
        destination.mkdir(parents=True, exist_ok=True)
        copied = []
        allowed_names = {"crashcontext.runtime-xml", "diagnostics.txt", "wermetadata.xml"}
        allowed_suffixes = {".log", ".dmp", ".xml"}
        candidates = sorted((path for path in source_dir.rglob("*")
                             if path.is_file()
                             and (path.name == "Network Persistent State"
                                  or path.name.lower() in allowed_names
                                  or path.suffix.lower() in allowed_suffixes)),
                            key=lambda path: path.stat().st_mtime, reverse=True)
        for source in candidates[:20]:
            if source.stat().st_size > 16 * 1024 * 1024:
                continue
            relative = source.relative_to(source_dir)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            copied.append(relative.as_posix())
        (destination / "failure.json").write_text(json.dumps({
            "scenario": scenario,
            "token": token,
            "reason": reason,
            "isolated_user_dir": str(source_dir),
            "copied_files": copied,
        }, indent=2) + "\n", encoding="utf-8")
        return str(destination)

    if scenario == "terraincore-regression":
        def run_terraincore_child(child_scenario: str, child_token: str,
                                  child_receipt: Path, child_data_root: Path,
                                  extra: list[str]) -> tuple[dict, dict]:
            child_user_dir = child_data_root / "unreal-user" / child_token
            child_user_dir.mkdir(parents=True, exist_ok=True)
            child_command = [str(launcher), "-SkiP1Smoke", f"-SkiP1Token={child_token}",
                             f"-SkiP1Receipt={child_receipt}", f"-SkiP1DataRoot={child_data_root}",
                             f"-SkiP1Scenario={child_scenario}", f"-UserDir={child_user_dir}",
                             "-windowed", "-ResX=1280", "-ResY=720", "-unattended", "-nosplash"]
            child_command.extend(extra)
            try:
                audit = checked_with_tcp_audit(
                    child_command, timeout=120,
                    log=f"p1-{child_scenario}-{child_token}.log")
                validate_tcp_audit(audit, require_no_connections=True,
                                   label=f"{scenario} child {child_scenario}")
            except p0.Failed as error:
                context = collect_failure_context(f"{child_scenario}: {error}", child_user_dir)
                raise p0.Failed(f"{error}; packaged failure context: {context}") from error
            if not child_receipt.is_file():
                context = collect_failure_context(
                    f"{child_scenario} exited without its tokened receipt", child_user_dir)
                raise p0.Failed(
                    f"{child_scenario} exited without its tokened receipt; context: {context}")
            return json.loads(child_receipt.read_text(encoding="utf-8-sig")), audit

        import_token = str(uuid.uuid4())
        import_receipt = data_root / f"{import_token}.receipt.json"
        imported, import_audit = run_terraincore_child(
            "terraincore-import-edit", import_token, import_receipt, data_root, [])
        content_id = imported.get("contentId", "")
        edit_set_id = imported.get("editSetId", "")
        if imported.get("token") != import_token \
                or imported.get("scenario") != "terraincore-import-edit" \
                or imported.get("schemaVersion") != 2 \
                or not re.fullmatch(r"[0-9a-f]{64}", content_id) \
                or not re.fullmatch(r"[0-9a-f]{64}", edit_set_id) \
                or imported.get("lodFactors") != [1, 2, 4, 8, 16] \
                or not imported.get("partialEdge") or not imported.get("sharedBorder") \
                or not imported.get("normalHalo") or not imported.get("baseImmutable") \
                or not imported.get("finestQuery") or not imported.get("editPersisted") \
                or not imported.get("staleBuildRejected") \
                or imported.get("defaultCacheBudgetBytes") != DEFAULT_TERRAINCORE_CACHE_BYTES:
            raise p0.Failed("Packaged TerrainCore import/edit child receipt is invalid")
        require_terraincore_renderer(imported, "TerrainCore import/edit child")
        first_trees = terraincore_contract_trees(data_root)

        repeat_root = (run_output / "isolated-data-repeat").resolve()
        repeat_root.mkdir(parents=True, exist_ok=False)
        repeat_token = str(uuid.uuid4())
        repeat_receipt = repeat_root / f"{repeat_token}.receipt.json"
        repeated, repeat_audit = run_terraincore_child(
            "terraincore-import-edit", repeat_token, repeat_receipt, repeat_root, [])
        if repeated.get("token") != repeat_token \
                or repeated.get("contentId") != content_id \
                or repeated.get("editSetId") != edit_set_id \
                or repeated.get("defaultCacheBudgetBytes") != DEFAULT_TERRAINCORE_CACHE_BYTES:
            raise p0.Failed(
                "Repeat-clean TerrainCore generation did not reproduce content/edit identities")
        require_terraincore_renderer(repeated, "Repeat-clean TerrainCore child")
        repeated_trees = terraincore_contract_trees(repeat_root)
        if repeated_trees != first_trees:
            raise p0.Failed(
                "Repeat-clean TerrainCore/TerrainEdits trees are not byte-for-byte deterministic")

        reopen_token = str(uuid.uuid4())
        reopen_receipt = data_root / f"{reopen_token}.receipt.json"
        reopened, reopen_audit = run_terraincore_child(
            "terraincore-offline-reopen", reopen_token, reopen_receipt, data_root,
            [f"-SkiP1ContentId={content_id}", f"-SkiP1EditSetId={edit_set_id}"])
        if reopened.get("token") != reopen_token \
                or reopened.get("scenario") != "terraincore-offline-reopen" \
                or reopened.get("contentId") != content_id \
                or reopened.get("editSetId") != edit_set_id \
                or not reopened.get("offlineReopen") or not reopened.get("finestQuery") \
                or not reopened.get("editDeltaReconstructed") \
                or not reopened.get("baseImmutable") \
                or reopened.get("reopenSeconds", 31) > 30:
            raise p0.Failed("Packaged TerrainCore offline-reopen child receipt is invalid")
        require_acquisition_port_guard(reopened)
        require_terraincore_renderer(reopened, "TerrainCore offline-reopen child")
        reopened_trees = terraincore_contract_trees(data_root)
        if reopened_trees != first_trees:
            raise p0.Failed("Offline reopen changed the TerrainCore/TerrainEdits contract trees")
        observed = {
            "token": token, "scenario": scenario, "schemaVersion": 2,
            "contentId": content_id, "editSetId": edit_set_id,
            "lodFactors": imported["lodFactors"],
            "partialEdge": imported["partialEdge"],
            "sharedBorder": imported["sharedBorder"],
            "normalHalo": imported["normalHalo"],
            "baseImmutable": imported["baseImmutable"] and reopened["baseImmutable"],
            "offlineReopen": reopened["offlineReopen"],
            "finestQuery": imported["finestQuery"] and reopened["finestQuery"],
            "staleBuildRejected": imported["staleBuildRejected"],
            "defaultCacheBudgetBytes": imported["defaultCacheBudgetBytes"],
            "residentBudgetBytes": imported["residentBudgetBytes"],
            "peakResidentBytes": imported["peakResidentBytes"],
            "evictionCount": imported.get("evictionCount", 0),
            "editDeltaReconstructed": reopened["editDeltaReconstructed"],
            "reopenSeconds": reopened["reopenSeconds"],
            "deterministicIds": True,
            "deterministicTrees": True,
            "contractTrees": first_trees,
            "contractTreeRuns": {"first": first_trees, "repeat": repeated_trees,
                                 "reopen": reopened_trees},
            "rendererPath": True,
            "renderedTileCount": min(child["renderedTileCount"]
                                     for child in (imported, repeated, reopened)),
            "revisionAligned": True,
            "actorPicked": True,
            "actorMutationObserved": True,
            "actorStaleMeshRejected": True,
            "acquisitionPortGuardInstalled": reopened["acquisitionPortGuardInstalled"],
            "acquisitionTransportCalls": reopened["acquisitionTransportCalls"],
            "processNetworkAudit": reopen_audit,
            "children": [imported, repeated, reopened],
            "childNetworkAudits": [import_audit, repeat_audit, reopen_audit],
        }
    elif scenario == "ui-layout":
        layouts = []
        audits = []
        resolutions = ((1280, 720), (1920, 1080), (2560, 1080),
                       (2560, 1440), (576, 1024))
        states = ("selecting", "preparing", "failed", "ready")
        for width, height in resolutions:
            for state in states:
                child_token = str(uuid.uuid4())
                child_receipt = data_root / f"{child_token}.receipt.json"
                child_user_dir = data_root / "unreal-user" / child_token
                child_user_dir.mkdir(parents=True, exist_ok=True)
                child_command = [str(launcher), "-SkiP1UiLayoutSmoke",
                                 f"-SkiP1Token={child_token}",
                                 f"-SkiP1Receipt={child_receipt}",
                                 f"-SkiP1DataRoot={data_root}",
                                 f"-SkiP1UiState={state}", f"-UserDir={child_user_dir}",
                                 "-windowed", f"-ResX={width}", f"-ResY={height}",
                                 "-unattended", "-nosplash"]
                if (width, height, state) == (1280, 720, "failed"):
                    child_command.append("-SkiP1UiInputIsolation")
                try:
                    audit = checked_with_tcp_audit(
                        child_command, timeout=120,
                        log=f"p1-ui-layout-{width}x{height}-{state}-{child_token}.log")
                    validate_tcp_audit(audit, require_no_connections=True,
                                       label=f"ui-layout {width}x{height} {state}")
                    require_no_browser_profile(
                        child_user_dir, f"ui-layout {width}x{height} {state}")
                except p0.Failed as error:
                    context = collect_failure_context(
                        f"ui-layout {width}x{height} {state}: {error}", child_user_dir)
                    raise p0.Failed(f"{error}; packaged failure context: {context}") from error
                if not child_receipt.is_file():
                    raise p0.Failed(
                        f"UI layout {width}x{height} {state} omitted its receipt")
                child = json.loads(child_receipt.read_text(encoding="utf-8-sig"))
                expected_input = True
                if child.get("token") != child_token or child.get("uiState") != state \
                        or child.get("resolution") != [width, height] \
                        or not child.get("layoutValid") \
                        or not child.get("recoveryActionsReachable") \
                        or child.get("inputIsolation") is not expected_input:
                    raise p0.Failed(
                        f"UI layout {width}x{height} {state} receipt is invalid")
                rect_name = "selectorRect" if state == "selecting" else "panelRect"
                rect = child.get(rect_name, [])
                if len(rect) != 4 or rect[2] <= 0 or rect[3] <= 0 \
                        or rect[0] < -1 or rect[1] < -1 \
                        or rect[0] + rect[2] > width + 1 \
                        or rect[1] + rect[3] > height + 1:
                    raise p0.Failed(
                        f"UI layout {width}x{height} {state} leaves the viewport")
                layouts.append(child)
                audits.append(audit)
        observed = {"token": token, "scenario": scenario, "layoutValid": True,
                    "inputIsolation": True, "recoveryActionsReachable": True,
                    "layouts": layouts, "processNetworkAudit": audits[-1],
                    "childNetworkAudits": audits}
    elif scenario == "picker-viewport":
        captures = []
        audits = []
        for width, height in PICKER_VIEWPORT_RESOLUTIONS:
            child_token = str(uuid.uuid4())
            child_receipt = data_root / f"{child_token}.receipt.json"
            child_screenshot = data_root / f"{child_token}.png"
            child_user_dir = data_root / "unreal-user" / child_token
            child_user_dir.mkdir(parents=True, exist_ok=True)
            child_command = [str(launcher), "-SkiP1PickerViewportSmoke",
                             f"-SkiP1Token={child_token}",
                             f"-SkiP1Receipt={child_receipt}",
                             f"-SkiP1Screenshot={child_screenshot}",
                             f"-SkiP1DataRoot={data_root}",
                             f"-UserDir={child_user_dir}", "-windowed",
                             f"-ResX={width}", f"-ResY={height}",
                             "-unattended", "-nosplash"]
            label = f"picker-viewport {width}x{height}"
            try:
                audit = checked_with_tcp_audit(
                    child_command, timeout=45,
                    log=f"p1-picker-viewport-{width}x{height}-{child_token}.log",
                    include_listener_owners=True)
                trace_policy = validate_development_picker_viewport_tcp_audit(
                    audit, configuration=configuration, scenario=scenario,
                    expected_image=launcher.name, label=label)
                audit.update(trace_policy)
            except p0.Failed as error:
                context = collect_failure_context(f"{label}: {error}", child_user_dir)
                raise p0.Failed(f"{error}; packaged failure context: {context}") from error
            if not child_receipt.is_file():
                raise p0.Failed(f"{label} omitted its tokened receipt")
            try:
                child = json.loads(child_receipt.read_text(encoding="utf-8-sig"))
            except (OSError, ValueError) as error:
                raise p0.Failed(f"{label} receipt is unreadable: {error}") from error
            child = require_picker_viewport_capture(
                child, child_token, width, height, child_screenshot)
            child.update(trace_policy)
            child["processNetworkAudit"] = audit
            captures.append(child)
            audits.append(audit)
        observed = {"token": token, "scenario": scenario,
                    "captureKind": "rendered-viewport-png",
                    "capturedStep": "name-resort",
                    "capturedScrollPosition": "end",
                    "visualReviewRequired": True,
                    "developmentTraceListenerObserved": all(
                        capture.get("developmentTraceListenerObserved") is True
                        for capture in captures),
                    "developmentTraceListenerPolicyException":
                        DEVELOPMENT_TRACE_LISTENER_POLICY,
                    "networkRemoteEndpoints": [], "networkRemoteZero": all(
                        capture.get("networkRemoteZero") is True for capture in captures),
                    "resolutions": [list(size) for size in PICKER_VIEWPORT_RESOLUTIONS],
                    "captures": captures, "processNetworkAudit": audits[-1],
                    "childNetworkAudits": audits}
    elif scenario == "medium-regression":
        def run_medium_child(child_scenario: str, child_token: str,
                             child_receipt: Path, child_data_root: Path,
                             extra: list[str]) -> tuple[dict, dict]:
            child_user_dir = child_data_root / "unreal-user" / child_token
            child_user_dir.mkdir(parents=True, exist_ok=True)
            child_command = [str(launcher), "-SkiP1Smoke", f"-SkiP1Token={child_token}",
                             f"-SkiP1Receipt={child_receipt}",
                             f"-SkiP1DataRoot={child_data_root}",
                             f"-SkiP1Scenario={child_scenario}", f"-UserDir={child_user_dir}",
                             "-windowed", "-ResX=1280", "-ResY=720",
                             "-unattended", "-nosplash"]
            child_command.extend(extra)
            try:
                audit = checked_with_tcp_audit(
                    child_command, timeout=180,
                    log=f"p1-medium-{child_scenario}-{child_token}.log")
                validate_tcp_audit(audit, require_no_connections=True,
                                   label=f"{scenario} child {child_scenario}")
            except p0.Failed as error:
                context = collect_failure_context(f"{child_scenario}: {error}", child_user_dir)
                raise p0.Failed(f"{error}; packaged failure context: {context}") from error
            if not child_receipt.is_file():
                raise p0.Failed(f"Medium {child_scenario} omitted its tokened receipt")
            return json.loads(child_receipt.read_text(encoding="utf-8-sig")), audit

        import_token = str(uuid.uuid4())
        imported, import_audit = run_medium_child(
            "import", import_token, data_root / f"{import_token}.receipt.json", data_root, [])
        content_id = imported.get("contentId", "")
        edit_set_id = imported.get("editSetId", "")
        base_height = imported.get("baseQueryHeightM")
        edited_height = imported.get("editedQueryHeightM")
        edit_delta = imported.get("editDeltaM")
        component_ids = (imported.get("terrainCoreId", ""),
                         imported.get("coverEcologyId", ""))
        if imported.get("token") != import_token or imported.get("qualityTier") != "medium" \
                or imported.get("schemaVersion") != 2 or not imported.get("nativeV2") \
                or not imported.get("ready") or not imported.get("picked") \
                or not imported.get("mutationObserved") or not imported.get("reopened") \
                or imported.get("optionalOutcomes") != 2 \
                or imported.get("editDeltaReconstructed") is not True \
                or imported.get("syntheticGuestMarkers") != 3000 \
                or imported.get("overlaySegments", 0) <= 0 \
                or not all(re.fullmatch(r"[0-9a-f]{64}", value or "")
                           for value in (content_id, edit_set_id) + component_ids) \
                or not all(type(value) in (int, float) and math.isfinite(value)
                           for value in (base_height, edited_height, edit_delta)) \
                or edit_delta <= 0.01 \
                or abs((edited_height - base_height) - edit_delta) > 1e-4:
            raise p0.Failed("Packaged Medium import/edit receipt is invalid")
        first_trees = installed_medium_contract_trees(data_root)

        repeat_root = (run_output / "isolated-data-repeat-medium").resolve()
        repeat_root.mkdir(parents=True, exist_ok=False)
        repeat_token = str(uuid.uuid4())
        repeated, repeat_audit = run_medium_child(
            "import", repeat_token, repeat_root / f"{repeat_token}.receipt.json",
            repeat_root, [])
        if repeated.get("contentId") != content_id \
                or repeated.get("editSetId") != edit_set_id \
                or repeated.get("terrainCoreId") != component_ids[0] \
                or repeated.get("coverEcologyId") != component_ids[1] \
                or installed_medium_contract_trees(repeat_root) != first_trees:
            raise p0.Failed("Repeat-clean packaged Medium installation is not deterministic")

        reopen_token = str(uuid.uuid4())
        reopened, reopen_audit = run_medium_child(
            "offline-reopen", reopen_token, data_root / f"{reopen_token}.receipt.json",
            data_root, [f"-SkiP1ContentId={content_id}",
                        f"-SkiP1EditSetId={edit_set_id}"])
        if reopened.get("token") != reopen_token or reopened.get("contentId") != content_id \
                or reopened.get("editSetId") != edit_set_id \
                or reopened.get("terrainCoreId") != component_ids[0] \
                or reopened.get("coverEcologyId") != component_ids[1] \
                or not reopened.get("offlineReopen") or not reopened.get("ready") \
                or not reopened.get("picked") or not reopened.get("reopened") \
                or reopened.get("editDeltaReconstructed") is not True \
                or type(reopened.get("editedQueryHeightM")) not in (int, float) \
                or not math.isfinite(reopened["editedQueryHeightM"]) \
                or abs(reopened["editedQueryHeightM"] - edited_height) > 1e-4:
            raise p0.Failed("Packaged Medium offline-reopen receipt is invalid")
        require_acquisition_port_guard(reopened)
        if installed_medium_contract_trees(data_root) != first_trees:
            raise p0.Failed("Offline Medium reopen changed immutable component trees")
        observed = {
            "token": token, "scenario": scenario, "schemaVersion": 2,
            "qualityTier": "medium", "contentId": content_id,
            "terrainCoreId": component_ids[0], "coverEcologyId": component_ids[1],
            "editSetId": edit_set_id, "baseQueryHeightM": base_height,
            "editedQueryHeightM": edited_height, "editDeltaM": edit_delta,
            "editDeltaReconstructed": True,
            "nativeV2": True, "ready": True, "picked": True,
            "mutationObserved": True, "offlineReopen": True,
            "reopened": True, "deterministicIds": True, "deterministicTrees": True,
            "syntheticGuestMarkers": imported["syntheticGuestMarkers"],
            "overlaySegments": imported["overlaySegments"],
            "acquisitionPortGuardInstalled": reopened["acquisitionPortGuardInstalled"],
            "acquisitionTransportCalls": reopened["acquisitionTransportCalls"],
            "contractTrees": first_trees,
            "processNetworkAudit": reopen_audit,
            "children": [imported, repeated, reopened],
            "childNetworkAudits": [import_audit, repeat_audit, reopen_audit],
        }
    else:
        observed = None
    receipt_path_escape_audit = None

    if observed is None:
        try:
            process_audit = checked_with_tcp_audit(
                command, timeout=120, log=f"p1-{scenario}-{token}.log")
            validate_tcp_audit(process_audit,
                               require_no_connections=scenario in ("frontend", "offline-reopen"),
                               label=scenario)
            if scenario in ("frontend", "geotiff-regression", "acquisition-regression",
                            "offline-reopen", "performance-regression"):
                require_no_browser_profile(unreal_user_dir, scenario)
        except p0.Failed as error:
            context = collect_failure_context(str(error))
            raise p0.Failed(f"{error}; packaged failure context: {context}") from error
        if not receipt.is_file():
            context = collect_failure_context("Packaged player exited without its tokened P1 receipt")
            raise p0.Failed(f"Packaged player exited without its tokened P1 receipt; context: {context}")
        observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
    if scenario == "frontend":
        if observed.get("token") != token or observed.get("scenario") != "frontend" \
                or not observed.get("passed") or not observed.get("nativeTitle") \
                or not observed.get("nativePickerPlaceholder") \
                or not observed.get("installedIdForwarded") \
                or not observed.get("browserWidgetAbsent") \
                or not observed.get("mountainTravel") \
                or not observed.get("offlineReopen") \
                or not re.fullmatch(r"[0-9a-f]{64}", observed.get("contentId", "")):
            raise p0.Failed("Packaged native frontend receipt is invalid")
        outside_receipt = run_output / f"{token}.receipt.json"
        escape_command = [f"-SkiP1Receipt={outside_receipt}"
                          if value.startswith("-SkiP1Receipt=") else value
                          for value in command]
        receipt_path_escape_audit = checked_with_tcp_audit(
            escape_command, timeout=30,
            log=f"p1-frontend-receipt-path-escape-{token}.log",
            allow_nonzero_exit=True)
        validate_tcp_audit(receipt_path_escape_audit, require_no_connections=True,
                           label="frontend receipt-path escape probe")
        # Unreal's Windows GUI launcher does not reliably propagate RequestExitWithStatus
        # through the outer launcher process. The security invariant is no outside write.
        if outside_receipt.exists():
            raise p0.Failed("Packaged frontend accepted a receipt outside its data root")
    elif scenario == "geotiff-regression":
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("decoded") \
                or observed.get("fixtureSha256") != "4614b0cec77c843a6b00ec7d0a4b3b90511031ee9eb73f185c5656088fe599bf" \
                or observed.get("dimensions") != [19, 18] or observed.get("nodata") != -9999.0 \
                or observed.get("organization") != "tiled":
            raise p0.Failed("Packaged GeoTIFF regression receipt is invalid")
    elif scenario == "acquisition-regression":
        dimensions = observed.get("dimensions", [])
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("passed") or observed.get("attempts") != 3 \
                or not observed.get("stitchedCentered") \
                or not observed.get("activationBlocked") \
                or not observed.get("networkCaps") or not observed.get("decodeCaps") \
                or observed.get("cancellationMilliseconds", 1000) > 250 \
                or observed.get("tileCount") != 4 or max(dimensions, default=0) != 2000 \
                or observed.get("activityTimeoutSeconds") != 90 \
                or observed.get("totalTimeoutSeconds") != 180:
            raise p0.Failed("Packaged acquisition-policy regression receipt is invalid")
    elif scenario == "ui-layout":
        layouts = observed.get("layouts", [])
        expected_pairs = {(width, height, state)
                          for width, height in ((1280, 720), (1920, 1080),
                                                (2560, 1080), (2560, 1440),
                                                (576, 1024))
                          for state in ("selecting", "preparing", "failed", "ready")}
        actual_pairs = {(entry.get("resolution", [None, None])[0],
                         entry.get("resolution", [None, None])[1], entry.get("uiState"))
                        for entry in layouts if len(entry.get("resolution", [])) == 2}
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("layoutValid") \
                or not observed.get("recoveryActionsReachable") \
                or not observed.get("inputIsolation") or actual_pairs != expected_pairs \
                or len(layouts) != len(expected_pairs):
            raise p0.Failed("Packaged UI-layout regression receipt is invalid")
    elif scenario == "picker-viewport":
        captures = observed.get("captures", [])
        actual_resolutions = [capture.get("resolution") for capture in captures]
        expected_resolutions = [list(size) for size in PICKER_VIEWPORT_RESOLUTIONS]
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or observed.get("captureKind") != "rendered-viewport-png" \
                or observed.get("visualReviewRequired") is not True \
                or actual_resolutions != expected_resolutions \
                or len(captures) != len(expected_resolutions):
            raise p0.Failed("Packaged native picker viewport matrix is invalid")
    elif scenario == "offline-reopen":
        require_offline_reopen_receipt(observed, content_id, edit_set_id)
    elif scenario == "terraincore-regression":
        content_id = observed.get("contentId", "")
        edit_set_id = observed.get("editSetId", "")
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or observed.get("schemaVersion") != 2 \
                or not re.fullmatch(r"[0-9a-f]{64}", content_id) \
                or not re.fullmatch(r"[0-9a-f]{64}", edit_set_id) \
                or observed.get("lodFactors") != [1, 2, 4, 8, 16] \
                or not observed.get("partialEdge") or not observed.get("sharedBorder") \
                or not observed.get("normalHalo") or not observed.get("baseImmutable") \
                or not observed.get("offlineReopen") or not observed.get("finestQuery") \
                or not observed.get("staleBuildRejected") \
                or not observed.get("editDeltaReconstructed") \
                or not observed.get("deterministicIds") \
                or not observed.get("deterministicTrees") \
                or observed.get("defaultCacheBudgetBytes") != DEFAULT_TERRAINCORE_CACHE_BYTES \
                or observed.get("residentBudgetBytes", 0) <= 0 \
                or observed.get("peakResidentBytes", 0) > observed.get("residentBudgetBytes", 0) \
                or observed.get("reopenSeconds", 31) > 30:
            raise p0.Failed("Packaged TerrainCore regression receipt is invalid")
        require_acquisition_port_guard(observed)
        require_terraincore_renderer(observed, "Packaged TerrainCore regression")
    elif scenario == "performance-regression":
        low = observed.get("low", {})
        reference = observed.get("reference", {})
        frames_name = observed.get("frameSamples", "")
        if frames_name != f"{token}.frames.json":
            raise p0.Failed("Packaged performance frame-sample name is invalid")
        frames_path = data_root / frames_name
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("passed") or low.get("frames") != 240 \
                or reference.get("frames") != 240 or low.get("p95Ms", 1e9) > 33.3 \
                or low.get("p99Ms", 1e9) > 50 or low.get("maxMs", 1e9) > 250 \
                or reference.get("p95Ms", 1e9) > 20 \
                or reference.get("p99Ms", 1e9) > 33.3 \
                or reference.get("maxMs", 1e9) > 250 \
                or observed.get("preparedReopenSeconds", 1e9) > 30 \
                or observed.get("cameraFramed") is not True \
                or type(observed.get("lowRenderedTiles")) is not int \
                or observed["lowRenderedTiles"] <= 0 \
                or type(observed.get("referenceRenderedTiles")) is not int \
                or observed["referenceRenderedTiles"] <= 0 \
                or observed.get("resolution") != [2560, 1440] \
                or observed.get("internalResolutionPercent") != 100 \
                or not all(isinstance(observed.get(field), str) and observed[field]
                           for field in ("rhi", "cpu", "gpu")) \
                or type(observed.get("availablePhysicalBytes")) is not int \
                or observed["availablePhysicalBytes"] <= 0 \
                or not frames_path.is_file() \
                or observed.get("frameSamplesSha256") != p0.sha(frames_path) \
                or observed.get("cacheBudgetBytes") != DEFAULT_TERRAINCORE_CACHE_BYTES:
            raise p0.Failed("Packaged performance-regression receipt is invalid")
        frames = json.loads(frames_path.read_text(encoding="utf-8-sig"))
        for field, summary in (("lowMs", low), ("referenceMs", reference)):
            values = frames.get(field)
            if not isinstance(values, list) or len(values) != 240 \
                    or not all(type(value) in (int, float) and math.isfinite(value)
                               and value >= 0 for value in values):
                raise p0.Failed(f"Packaged performance frame samples are invalid: {field}")
            ordered = sorted(values)
            for key, fraction in (("p95Ms", .95), ("p99Ms", .99)):
                index = math.ceil(fraction * len(ordered)) - 1
                if abs(ordered[index] - summary.get(key, -1)) > 1e-4:
                    raise p0.Failed(f"Packaged performance {field} {key} is inconsistent")
            if abs(ordered[-1] - summary.get("maxMs", -1)) > 1e-4 \
                    or any(summary.get(f"over{limit}") != sum(value > limit for value in values)
                           for limit in (50, 100, 250)):
                raise p0.Failed(f"Packaged performance {field} gap counts are inconsistent")
    elif scenario == "medium-regression":
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or observed.get("qualityTier") != "medium" \
                or observed.get("schemaVersion") != 2 or not observed.get("nativeV2") \
                or not observed.get("ready") or not observed.get("picked") \
                or not observed.get("mutationObserved") or not observed.get("offlineReopen") \
                or not observed.get("editDeltaReconstructed") \
                or not re.fullmatch(r"[0-9a-f]{64}", observed.get("editSetId", "")) \
                or not observed.get("deterministicIds") or not observed.get("deterministicTrees") \
                or observed.get("syntheticGuestMarkers") != 3000 \
                or observed.get("overlaySegments", 0) <= 0:
            raise p0.Failed("Packaged Medium composite regression receipt is invalid")
        require_acquisition_port_guard(observed)
    elif observed.get("token") != token or observed.get("scenario") != scenario \
            or not observed.get("ready") or not observed.get("picked") or not observed.get("reopened"):
        raise p0.Failed("Packaged P1 receipt is stale or qualification steps failed")
    verify_package_report(configuration, source_digest)
    return {"status": "PASS", "kind": "packaged_p1", "scenario": scenario,
            "receipt": observed, "package_invocation": package_report["invocation"],
            "receipt_path_escape_audit": receipt_path_escape_audit,
            "process_network_audit": (observed.get("processNetworkAudit")
                                      if scenario in ("terraincore-regression", "medium-regression",
                                                      "ui-layout", "picker-viewport")
                                      else process_audit),
            "network_policy": ("Frontend and receipt-path escape smoke runs require zero sampled "
                               "TCP listeners or remote endpoints at a nominal 10 ms interval; "
                               "shorter-lived endpoints may be missed"
                               if scenario == "frontend" else
                               "Development picker-viewport permits only the packaged app's "
                               "loopback Unreal Trace listener on port 1985; remote TCP endpoints: "
                               "zero; sampled at a nominal 10 ms, so shorter-lived endpoints may be missed"
                               if scenario == "picker-viewport" else
                               "production acquisition port denied; sampled process-attributed TCP "
                               "polling at a nominal 10 ms found no listeners or remote endpoints; "
                               "shorter-lived endpoints may be missed"
                               if scenario in ("offline-reopen", "terraincore-regression",
                                               "medium-regression")
                               else "fixture-only")}


def _picker_viewport_visual_network_findings(audit: dict, *, label: str,
                                              expected_image: str) -> list[str]:
    """Report the unchanged release audit result plus any extra observed endpoints."""
    findings = []
    try:
        validate_development_picker_viewport_tcp_audit(
            audit, configuration="Development", scenario="picker-viewport",
            expected_image=expected_image, label=label)
    except p0.Failed as error:
        findings.append(str(error))

    remotes = audit.get("remote_endpoints")
    if isinstance(remotes, list) and remotes:
        findings.append("Remote TCP endpoints observed: " + ", ".join(map(str, remotes)))

    owners = audit.get("listener_owners")
    if isinstance(owners, list):
        unique = {}
        for owner in owners:
            if not isinstance(owner, dict):
                findings.append("Malformed TCP listener owner was observed")
                continue
            local = owner.get("local")
            identity = (owner.get("pid"), owner.get("image"), local)
            unique[identity] = owner
        known_trace = [owner for owner in unique.values()
                       if _tcp_endpoint_host_port(owner.get("local"))
                       == ("0.0.0.0", DEVELOPMENT_TRACE_LISTENER_PORT)]
        unexpected = [owner for owner in unique.values() if owner not in known_trace]
        if len(known_trace) > 1:
            findings.append("More than one process owns the known 0.0.0.0:1985 Trace listener")
        if unexpected:
            detail = ", ".join(
                f"{owner.get('image')}#{owner.get('pid')}@{owner.get('local')}"
                for owner in unexpected)
            findings.append("Unexpected or additional TCP listener(s): " + detail)
    elif audit.get("listen_ports"):
        findings.append("TCP listener ports were observed without owner records: "
                        + str(audit["listen_ports"]))
    return findings


def picker_viewport_visual(configuration: str, source_digest: str,
                           run_output: Path) -> dict:
    """Capture picker visuals for diagnosis while retaining every failed release audit."""
    if configuration != "Development":
        raise p0.Failed("picker-viewport-visual is Development-only and cannot relax Shipping")
    package_report, launcher = verify_package_report(configuration, source_digest)
    data_root = (run_output / "picker-viewport-visual-data").resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    runs = []
    captures = []
    network_failures = []
    capture_failures = []

    for width, height in PICKER_VIEWPORT_RESOLUTIONS:
        token = str(uuid.uuid4())
        child_receipt = data_root / f"{token}.receipt.json"
        child_screenshot = data_root / f"{token}.png"
        child_user_dir = data_root / "unreal-user" / token
        child_user_dir.mkdir(parents=True, exist_ok=True)
        child_command = [str(launcher), "-SkiP1PickerViewportSmoke",
                         f"-SkiP1Token={token}", f"-SkiP1Receipt={child_receipt}",
                         f"-SkiP1Screenshot={child_screenshot}",
                         f"-SkiP1DataRoot={data_root}", f"-UserDir={child_user_dir}",
                         "-windowed", "-RenderOffScreen", "-ForceRes",
                         f"-ResX={width}", f"-ResY={height}",
                         "-unattended", "-nosplash"]
        label = f"picker-viewport-visual {width}x{height}"
        run = {"resolution": [width, height], "token": token,
               "processAttempted": True, "captureReceipt": None,
               "processNetworkAudit": None, "processAuditError": None,
               "networkAuditFindings": [], "captureError": None}

        # allow_nonzero_exit keeps the complete process-tree audit available for the
        # final report, even when the packaged process itself exits unsuccessfully.
        try:
            audit = checked_with_tcp_audit(
                child_command, timeout=45,
                log=f"p1-picker-viewport-visual-{width}x{height}-{token}.log",
                include_listener_owners=True, allow_nonzero_exit=True)
            run["processNetworkAudit"] = audit
            findings = _picker_viewport_visual_network_findings(
                audit, label=label, expected_image=launcher.name)
            run["networkAuditFindings"] = findings
            network_failures.extend({"resolution": [width, height], "finding": finding}
                                    for finding in findings)
            if audit.get("process_return_code") != 0:
                capture_failures.append({
                    "resolution": [width, height],
                    "reason": f"Packaged process exited with code {audit.get('process_return_code')}"})
        except Exception as error:
            run["processAuditError"] = str(error)
            network_failures.append({"resolution": [width, height],
                                     "finding": f"TCP audit unavailable: {error}"})
            capture_failures.append({"resolution": [width, height], "reason": str(error)})

        # Always inspect the tokened screenshot receipt after the process attempt. A
        # rejected network audit never short-circuits the remaining viewport captures.
        try:
            if not child_receipt.is_file():
                raise p0.Failed(f"{label} omitted its tokened receipt")
            child = json.loads(child_receipt.read_text(encoding="utf-8-sig"))
            run["captureReceipt"] = child
            validated = require_picker_viewport_capture(
                child, token, width, height, child_screenshot)
            run["captureReceipt"] = validated
            captures.append(validated)
        except Exception as error:
            run["captureError"] = str(error)
            capture_failures.append({"resolution": [width, height], "reason": str(error)})
        runs.append(run)

    visual_capture_pass = (len(runs) == len(PICKER_VIEWPORT_RESOLUTIONS)
                           and len(captures) == len(PICKER_VIEWPORT_RESOLUTIONS)
                           and not capture_failures)
    network_audit_status = "FAIL" if network_failures else "PASS"
    status = "FAIL" if network_failures or not visual_capture_pass else "DIAGNOSTIC"
    verify_package_report(configuration, source_digest)
    return {
        "status": status,
        "kind": "non_release_picker_viewport_visual_diagnostic",
        "releaseGatePass": False,
        "releaseAcceptanceEligible": False,
        "releaseAcceptanceScopes": [],
        "networkAuditStatus": network_audit_status,
        "networkAuditFailures": network_failures,
        "visualCapturePass": visual_capture_pass,
        "captureFailures": capture_failures,
        "attemptedResolutions": [list(size) for size in PICKER_VIEWPORT_RESOLUTIONS],
        "captures": captures,
        "runs": runs,
        "visualReviewRequired": True,
        "nonReleaseReason": "Diagnostic output is not an M2/M6 release acceptance receipt.",
        "package_invocation": package_report["invocation"],
    }


def visual(configuration: str, source_digest: str) -> dict:
    package_report, launcher = verify_package_report(configuration, source_digest)
    data_root = (OUTPUT / "visual" / configuration).resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    specifications = []
    for lighting in ("Midday", "LowAngle", "Overcast"):
        specifications.extend([("presentation", lighting, "full", 0, 2560, 1440),
                               ("presentation", lighting, "close", 0, 2560, 1440)])
    specifications.extend([
        ("elevation", "Midday", "full", 0, 2560, 1440),
        ("slope", "Midday", "full", 0, 2560, 1440),
        ("cover", "Midday", "full", 0, 2560, 1440),
        ("lod", "Midday", "full", 0, 2560, 1440),
        ("topology", "Midday", "close", 0, 2560, 1440),
        ("topology", "Midday", "close", 1, 2560, 1440),
        ("topology", "Midday", "close", 2, 2560, 1440),
        ("presentation", "Midday", "full", 0, 2560, 1080),
    ])
    receipts = []
    package_identity = None
    screenshot_hashes = set()
    for index, (mode, lighting, view, lod, width, height) in enumerate(specifications):
        token = str(uuid.uuid4())
        receipt = data_root / f"{token}.receipt.json"
        screenshot = data_root / f"{token}.png"
        user_dir = data_root / "unreal-user" / token
        user_dir.mkdir(parents=True, exist_ok=True)
        command = [str(launcher), "-SkiP1VisualCapture", f"-SkiP1Token={token}",
                   f"-SkiP1Receipt={receipt}", f"-SkiP1Screenshot={screenshot}",
                   f"-SkiP1DataRoot={data_root}", f"-SkiP1CaptureMode={mode}",
                   f"-SkiP1CaptureLighting={lighting}", f"-SkiP1CaptureView={view}",
                   f"-SkiP1CaptureLod={lod}", f"-SkiP1CaptureWidth={width}",
                   f"-SkiP1CaptureHeight={height}", f"-UserDir={user_dir}", "-windowed",
                   f"-ResX={width}", f"-ResY={height}", "-unattended", "-nosplash"]
        p0.checked(command, timeout=90, log=f"p1-visual-{index:02d}-{token}.log")
        if not receipt.is_file() or not screenshot.is_file():
            raise p0.Failed(f"Visual capture {index} did not create its tokened files")
        observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
        png = screenshot.read_bytes()
        actual_size = struct.unpack(">II", png[16:24]) if png.startswith(b"\x89PNG\r\n\x1a\n") else None
        identity = (observed.get("terrainCoreId"), observed.get("coverEcologyId"),
                    observed.get("installationId"), observed.get("packageHash"))
        revisions = observed.get("revisions", [])
        if observed.get("token") != token or observed.get("screenshotSha256") != p0.sha(screenshot) \
                or actual_size != (width, height) \
                or observed.get("resolution") != [width, height] \
                or observed.get("representation") != "terraincore-v2" \
                or observed.get("diagnosticMode") != mode \
                or observed.get("lighting") != lighting or observed.get("view") != view \
                or observed.get("lod") != lod or observed.get("verticalScale") != 1 \
                or observed.get("internalResolutionPercent") != 100 \
                or not observed.get("revisionAligned") or len(revisions) != 3 \
                or len(set(revisions)) != 1 or not revisions[0] \
                or observed.get("syntheticGuestMarkers") != 3000 \
                or observed.get("overlaySegments", 0) <= 0 \
                or not all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                           for value in identity) \
                or screenshot.stat().st_size < 64 * 1024:
            raise p0.Failed(f"Visual capture {index} receipt or screenshot hash is invalid")
        observed["screenshot"] = str(screenshot)
        if observed.get("contentId") != observed.get("terrainCoreId"):
            raise p0.Failed(f"Visual capture {index} has an ambiguous content identity")
        if package_identity is None:
            package_identity = identity
        elif identity != package_identity:
            raise p0.Failed(f"Visual capture {index} did not reproduce the immutable composite package")
        screenshot_hashes.add(observed["screenshotSha256"])
        receipts.append(observed)
    if len(screenshot_hashes) != len(specifications):
        raise p0.Failed("Visual capture matrix contains duplicate images and cannot prove its modes")
    verify_package_report(configuration, source_digest)
    return {"status": "PASS", "kind": "packaged_visual_capture", "captures": receipts,
            "fixture": "synthetic", "package_invocation": package_report["invocation"]}


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["freeze", "freeze-check", "doctor", "check", "domain", "build", "automation", "automation-focused", "tiff", "acquisition", "ui", "terraincore", "p1", "assets", "package", "smoke", "picker-viewport-visual", "visual"])
    parser.add_argument("--engine-root")
    parser.add_argument("--group", choices=tuple(FOCUSED_AUTOMATION_TESTS),
                        help="exact registered M3/M4/M5/M6 automation filter for automation-focused")
    parser.add_argument("--configuration", choices=["Development", "Shipping"], default="Development")
    parser.add_argument("--target", choices=["Editor", "Game"], default="Editor")
    parser.add_argument("--scenario", choices=["frontend", "import", "offline-reopen", "geotiff-regression", "acquisition-regression", "ui-layout", "picker-viewport", "terraincore-regression", "medium-regression", "performance-regression"], default="import")
    parser.add_argument("--content-id", type=parse_sha256_id)
    parser.add_argument("--edit-set-id", type=parse_sha256_id,
                        help="64-character lowercase editSetId from the import receipt; used by offline-reopen")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    invocation = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
    run_output = OUTPUT / "runs" / invocation
    run_output.mkdir(parents=True)
    print(f"P1 run output: {run_output}", flush=True)
    p0.OUTPUT = OUTPUT
    p0.RUN_OUTPUT = run_output
    environment = p0.doctor(args.engine_root)
    before = p0.source_snapshot()
    freeze_setting = os.environ.get("SKI_P1_RELEASE_FREEZE")
    freeze_path = Path(freeze_setting) if freeze_setting else OUTPUT / "release-freeze.json"
    if not freeze_path.is_absolute():
        freeze_path = (ROOT / freeze_path).resolve()
    freeze_error = None
    if args.command != "freeze" and freeze_setting:
        if not freeze_path.is_file():
            freeze_error = f"Release freeze receipt is missing: {freeze_path}"
        else:
            try:
                frozen = json.loads(freeze_path.read_text(encoding="utf-8"))
                if frozen.get("command") != "freeze" or frozen.get("result", {}).get("status") != "PASS" \
                        or frozen.get("source_before") != before["sha256"]:
                    freeze_error = "Source no longer matches the release-wide freeze receipt"
            except (OSError, ValueError) as error:
                freeze_error = f"Release freeze receipt is invalid: {error}"
    code = 0
    try:
        if freeze_error:
            raise p0.Failed(freeze_error)
        if args.command == "freeze":
            result = {"status": "PASS", "kind": "release_source_freeze", "source_digest": before["sha256"]}
        elif args.command == "freeze-check":
            result = {"status": "PASS", "kind": "release_source_freeze_check", "source_digest": before["sha256"]}
        elif args.command == "doctor":
            result = environment
            code = 2 if environment["missing"] else 0
        elif args.command == "check":
            result = {"status": "PASS", "retained": retained_checks(), "p1": required_inputs(),
                      "tooling": p0.tooling_checks()}
        elif args.command == "domain":
            result = p0.domain_checks(environment)
        elif args.command == "build":
            result = p0.native_build(environment, args.target, args.configuration)
        elif args.command == "automation":
            result = automation(environment, run_output)
        elif args.command == "automation-focused":
            if args.group is None:
                parser.error("automation-focused requires --group")
            result = focused_automation(environment, run_output, args.group)
        elif args.command == "tiff":
            result = tiff(environment, run_output)
        elif args.command == "acquisition":
            result = acquisition(environment, run_output)
        elif args.command == "ui":
            result = ui(environment, run_output)
        elif args.command == "terraincore":
            result = terraincore(environment, run_output)
        elif args.command == "p1":
            result = p1_product(environment, run_output)
        elif args.command == "assets":
            result = create_assets(environment, before)
        elif args.command == "package":
            required_inputs()
            sys.path.insert(0, str(ROOT / "Tools/Editor"))
            from p1_asset_ownership import ASSETS, preflight, recipe_digest
            receipt = preflight(ROOT, recipe_digest(ROOT))
            if set(receipt["assets"]) != set(ASSETS):
                raise p0.Blocked("Generate the separately owned P1 assets before packaging")
            result = p0.package(environment, args.configuration)
            assert_no_browser_bundle(Path(result["directory"]) / "Windows")
            if args.configuration == "Shipping":
                result["shipping_mcp_proof"] = shipping_target_module_proof()
        elif args.command == "smoke":
            result = smoke(args.configuration, before["sha256"], args.scenario,
                           args.content_id, run_output, edit_set_id=args.edit_set_id)
        elif args.command == "picker-viewport-visual":
            result = picker_viewport_visual(args.configuration, before["sha256"], run_output)
            if result.get("status") == "FAIL":
                code = 1
        else:
            result = visual(args.configuration, before["sha256"])
        after = p0.source_snapshot()
        if args.command != "assets":
            p0.verify_frozen(before, after)
    except p0.Blocked as error:
        result = {"status": "BLOCKED", "reason": str(error)}
        code = 2
    except (RuntimeError, OSError, ValueError) as error:
        result = {"status": "FAIL", "reason": str(error)}
        code = 1
    report = {"invocation": invocation, "run_output": str(run_output), "command": args.command, "configuration": args.configuration,
              "source_before": before["sha256"], "environment": environment, "result": result}
    serialized = json.dumps(report, indent=2) + "\n"
    (run_output / "report.json").write_text(serialized, encoding="utf-8")
    (run_output / "source-manifest.json").write_text(
        json.dumps(before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    latest = args.command + ("-" + args.configuration if args.command in (
        "package", "smoke", "picker-viewport-visual", "visual") else "")
    (OUTPUT / f"{latest}.json").write_text(serialized, encoding="utf-8")
    if args.command == "freeze":
        freeze_path.parent.mkdir(parents=True, exist_ok=True)
        freeze_path.write_text(serialized, encoding="utf-8")
    if args.command == "smoke":
        (OUTPUT / f"smoke-{args.configuration}-{args.scenario}.json").write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    return code


def main() -> int:
    previous_error_mode = None
    if os.name == "nt":
        # Harness-owned children inherit this and return a failing exit code instead of
        # blocking unattended qualification behind a Windows crash dialog.
        previous_error_mode = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    try:
        return _main()
    finally:
        if previous_error_mode is not None:
            ctypes.windll.kernel32.SetErrorMode(previous_error_mode)


if __name__ == "__main__":
    raise SystemExit(main())
