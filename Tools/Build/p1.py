"""Failure-propagating P1 Unreal build, package, automation, and packaged-smoke harness."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import importlib.util
import hashlib
import json
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
UI_TESTS = (
    "MountainPlanner.P1.Presentation.UI.ResponsiveLayout",
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
TERRAINCORE_NATIVE_TESTS = (
    "SkiDomain.CoverEcology",
    "SkiDomain.Revision",
    "SkiDomain.Terrain",
    "SkiDomain.TerrainCore",
)
P1_PRODUCT_TESTS = (
    "MountainPlanner.P1.Product.CoverEcology.CompositeActivation",
    "MountainPlanner.P1.Product.CoverEcology.ContractAndStore",
    "MountainPlanner.P1.Product.Medium.ProfileContract",
    "MountainPlanner.P1.Product.WorldCoverCog.AnalyticalClasses",
)
DEFAULT_TERRAINCORE_CACHE_BYTES = 512 * 1024 * 1024
FORBIDDEN_SHIPPING_PLUGINS = (
    "ModelContextProtocol", "EditorToolset", "AutomationTestToolset",
    "SlateInspectorToolset", "UMGToolSet",
)
P1_TESTS = tuple(sorted(TIFF_TESTS + ACQUISITION_TESTS + UI_TESTS + TERRAINCORE_TESTS + P1_PRODUCT_TESTS + (
    "MountainPlanner.P1.Preparation.PackageAndProtocol",
    "MountainPlanner.P1.Preparation.ProviderDiagnostics",
)))

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
        "Source/SkiPreparation/Public/SkiPreparation/SelectorProtocol.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/SkiTerrainActor.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreTileCache.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreLodController.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/TerrainCoreMesh.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreRepository.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreEditedRepository.h",
        "Source/SkiApplication/Public/SkiApplication/TerrainCoreSession.h",
        "Source/SkiDomain/Public/SkiDomain/TerrainCore.h",
        "Source/SkiDomain/Public/SkiDomain/CoverEcology.h",
        "Content/P1Selector/index.html",
        "Content/P1Selector/maplibre-gl.js",
        "Content/P1Selector/maplibre-gl.css",
        "Content/P1Selector/MAPLIBRE-LICENSE.txt",
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
    selector = (ROOT / "Content/P1Selector/index.html").read_text(encoding="utf-8")
    if 'src="maplibre-gl.js"' not in selector or "Content-Security-Policy" not in selector:
        raise p0.Failed("Selector is not pinned to the staged local MapLibre bundle")
    return {"status": "PASS", "required_inputs": required,
            "selector_bundle_bytes": sum((ROOT / value).stat().st_size for value in required if value.startswith("Content/P1Selector/"))}


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
    present = sorted(set(plugins).intersection(FORBIDDEN_SHIPPING_PLUGINS))
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


def validate_tcp_audit(audit: dict, *, require_no_connections: bool) -> None:
    if audit.get("method") != "GetExtendedTcpTable process-attributed polling" \
            or audit.get("snapshots", 0) < 1:
        raise p0.Failed("Packaged process TCP audit is missing or was not observed")
    if audit.get("listen_ports"):
        raise p0.Failed(f"Packaged process opened TCP listeners: {audit['listen_ports']}")
    if require_no_connections and audit.get("remote_endpoints"):
        raise p0.Failed(
            f"Offline reopen attempted TCP connections: {audit['remote_endpoints']}")


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
            for name in ("TerrainCore", "CoverEcology", "InstalledTerrain")}


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


def _windows_process_descendants(root_pid: int, known: set[int]) -> set[int]:
    """Track a packaged bootstrap process and any child game/helper processes it creates."""
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
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(ProcessEntry)
        present = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while present:
            parents[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
            present = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    tracked = set(known) | {root_pid}
    changed = True
    while changed:
        changed = False
        for candidate, parent in parents.items():
            if parent in tracked and candidate not in tracked:
                tracked.add(candidate)
                changed = True
    return tracked


def checked_with_tcp_audit(command: list[str], *, timeout: float, log: str) -> dict:
    """Run one packaged process while independently polling its owned TCP table."""
    log_path = p0.RUN_OUTPUT / log
    log_path.parent.mkdir(parents=True, exist_ok=True)
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    began = time.monotonic()
    snapshots = 0
    observed = set()
    tracked_pids = set()
    with log_path.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, cwd=ROOT, stdout=stream,
                                   stderr=subprocess.STDOUT, text=True, **options)
        try:
            while process.poll() is None:
                tracked_pids = _windows_process_descendants(process.pid, tracked_pids)
                for tracked_pid in tracked_pids:
                    for row in (_windows_ipv4_tcp_rows(tracked_pid)
                                + _windows_ipv6_tcp_rows(tracked_pid)):
                        observed.add((tracked_pid, row["state"], row["local"], row["remote"]))
                snapshots += 1
                if time.monotonic() - began > timeout:
                    raise p0.TimedOut(f"Timed out after {timeout}s: {command[0]}")
                time.sleep(0.01)
        except BaseException:
            if os.name == "nt":
                try:
                    subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=2, creationflags=subprocess.CREATE_NO_WINDOW)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
            elif process.poll() is None:
                process.kill()
            process.wait(timeout=2)
            raise
    if process.returncode != 0:
        output = log_path.read_text(encoding="utf-8", errors="replace")
        raise p0.Failed(f"Exit {process.returncode}: {command[0]}\n{output[-3000:]}")
    listen_ports = sorted({int(local.rsplit(":", 1)[1]) for _, state, local, _ in observed
                           if state == 2})
    remote_endpoints = sorted({remote for _, state, _, remote in observed
                               if state != 2 and not remote.endswith(":0")})
    return {"method": "GetExtendedTcpTable process-attributed polling",
            "root_pid": process.pid, "observed_pids": sorted(tracked_pids),
            "snapshots": snapshots,
            "poll_interval_milliseconds": 10, "listen_ports": listen_ports,
            "remote_endpoints": remote_endpoints}


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
    if configuration == "Shipping":
        recorded_proof = report["result"].get("shipping_mcp_proof")
        current_proof = shipping_target_module_proof()
        if recorded_proof != current_proof:
            raise p0.Failed("Shipping target/module MCP proof is missing, stale, or mismatched")
    return report, launcher


def smoke(configuration: str, source_digest: str, scenario: str, content_id: str | None,
          run_output: Path) -> dict:
    package_report, launcher = verify_package_report(configuration, source_digest)
    token = str(uuid.uuid4())
    data_root = ((run_output / "isolated-data") if scenario in
                 ("terraincore-regression", "medium-regression", "ui-layout",
                  "performance-regression")
                 else (OUTPUT / "isolated-data" / configuration)).resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    unreal_user_dir = data_root / "unreal-user" / token
    unreal_user_dir.mkdir(parents=True, exist_ok=True)
    receipt = data_root / f"{token}.receipt.json"
    smoke_flag = ("-SkiP1SelectorSmoke" if scenario == "selector" else
                  "-SkiP1UiLayoutSmoke" if scenario == "ui-layout" else
                  "-SkiP1PerformanceSmoke" if scenario == "performance-regression"
                  else "-SkiP1Smoke")
    command = [str(launcher), smoke_flag, f"-SkiP1Token={token}",
               f"-SkiP1Receipt={receipt}", f"-SkiP1DataRoot={data_root}",
               f"-SkiP1Scenario={scenario}", f"-UserDir={unreal_user_dir}",
               "-windowed", "-ResX=1280", "-ResY=720",
               "-unattended", "-nosplash"]
    if scenario == "offline-reopen":
        if not content_id or len(content_id) != 64:
            raise p0.Failed("Offline reopen requires the exact contentId from an import receipt")
        command.append(f"-SkiP1ContentId={content_id}")

    def collect_failure_context(reason: str) -> str:
        destination = run_output / "packaged-failure-context"
        destination.mkdir(parents=True, exist_ok=True)
        copied = []
        allowed_names = {"crashcontext.runtime-xml", "diagnostics.txt", "wermetadata.xml"}
        allowed_suffixes = {".log", ".dmp", ".xml"}
        candidates = sorted((path for path in unreal_user_dir.rglob("*")
                             if path.is_file()
                             and (path.name.lower() in allowed_names
                                  or path.suffix.lower() in allowed_suffixes)),
                            key=lambda path: path.stat().st_mtime, reverse=True)
        for source in candidates[:20]:
            if source.stat().st_size > 16 * 1024 * 1024:
                continue
            relative = source.relative_to(unreal_user_dir)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            copied.append(relative.as_posix())
        (destination / "failure.json").write_text(json.dumps({
            "scenario": scenario,
            "token": token,
            "reason": reason,
            "isolated_user_dir": str(unreal_user_dir),
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
                validate_tcp_audit(audit, require_no_connections=True)
            except p0.Failed as error:
                context = collect_failure_context(f"{child_scenario}: {error}")
                raise p0.Failed(f"{error}; packaged failure context: {context}") from error
            if not child_receipt.is_file():
                context = collect_failure_context(
                    f"{child_scenario} exited without its tokened receipt")
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
                    validate_tcp_audit(audit, require_no_connections=True)
                except p0.Failed as error:
                    context = collect_failure_context(
                        f"ui-layout {width}x{height} {state}: {error}")
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
                validate_tcp_audit(audit, require_no_connections=True)
            except p0.Failed as error:
                context = collect_failure_context(f"{child_scenario}: {error}")
                raise p0.Failed(f"{error}; packaged failure context: {context}") from error
            if not child_receipt.is_file():
                raise p0.Failed(f"Medium {child_scenario} omitted its tokened receipt")
            return json.loads(child_receipt.read_text(encoding="utf-8-sig")), audit

        import_token = str(uuid.uuid4())
        imported, import_audit = run_medium_child(
            "import", import_token, data_root / f"{import_token}.receipt.json", data_root, [])
        content_id = imported.get("contentId", "")
        component_ids = (imported.get("terrainCoreId", ""),
                         imported.get("coverEcologyId", ""))
        if imported.get("token") != import_token or imported.get("qualityTier") != "medium" \
                or imported.get("schemaVersion") != 2 or not imported.get("nativeV2") \
                or not imported.get("ready") or not imported.get("picked") \
                or not imported.get("mutationObserved") or not imported.get("reopened") \
                or imported.get("optionalOutcomes") != 2 \
                or imported.get("syntheticGuestMarkers") != 3000 \
                or imported.get("overlaySegments", 0) <= 0 \
                or not all(re.fullmatch(r"[0-9a-f]{64}", value or "")
                           for value in (content_id,) + component_ids):
            raise p0.Failed("Packaged Medium import/edit receipt is invalid")
        first_trees = installed_medium_contract_trees(data_root)

        repeat_root = (run_output / "isolated-data-repeat-medium").resolve()
        repeat_root.mkdir(parents=True, exist_ok=False)
        repeat_token = str(uuid.uuid4())
        repeated, repeat_audit = run_medium_child(
            "import", repeat_token, repeat_root / f"{repeat_token}.receipt.json",
            repeat_root, [])
        if repeated.get("contentId") != content_id \
                or repeated.get("terrainCoreId") != component_ids[0] \
                or repeated.get("coverEcologyId") != component_ids[1] \
                or installed_medium_contract_trees(repeat_root) != first_trees:
            raise p0.Failed("Repeat-clean packaged Medium installation is not deterministic")

        reopen_token = str(uuid.uuid4())
        reopened, reopen_audit = run_medium_child(
            "offline-reopen", reopen_token, data_root / f"{reopen_token}.receipt.json",
            data_root, [f"-SkiP1ContentId={content_id}"])
        if reopened.get("token") != reopen_token or reopened.get("contentId") != content_id \
                or reopened.get("terrainCoreId") != component_ids[0] \
                or reopened.get("coverEcologyId") != component_ids[1] \
                or not reopened.get("offlineReopen") or not reopened.get("ready") \
                or not reopened.get("picked") or not reopened.get("reopened"):
            raise p0.Failed("Packaged Medium offline-reopen receipt is invalid")
        require_acquisition_port_guard(reopened)
        if installed_medium_contract_trees(data_root) != first_trees:
            raise p0.Failed("Offline Medium reopen changed immutable component trees")
        observed = {
            "token": token, "scenario": scenario, "schemaVersion": 2,
            "qualityTier": "medium", "contentId": content_id,
            "terrainCoreId": component_ids[0], "coverEcologyId": component_ids[1],
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

    if observed is None:
        try:
            process_audit = checked_with_tcp_audit(
                command, timeout=120, log=f"p1-{scenario}-{token}.log")
            validate_tcp_audit(process_audit, require_no_connections=scenario == "offline-reopen")
        except p0.Failed as error:
            context = collect_failure_context(str(error))
            raise p0.Failed(f"{error}; packaged failure context: {context}") from error
        if not receipt.is_file():
            context = collect_failure_context("Packaged player exited without its tokened P1 receipt")
            raise p0.Failed(f"Packaged player exited without its tokened P1 receipt; context: {context}")
        observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
    if scenario == "selector":
        if observed.get("token") != token or observed.get("selector") is not True \
                or observed.get("profile") != "medium" \
                or observed.get("closedBeforeAcceptance") is not True \
                or observed.get("blockedNavigation", 0) < 1 \
                or observed.get("blockedPopup", 0) < 1:
            raise p0.Failed("Packaged selector receipt is stale or CEF/WebGL/bridge validation failed")
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
        frames_path = data_root / frames_name
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("passed") or low.get("frames") != 240 \
                or reference.get("frames") != 240 or low.get("p95Ms", 1e9) > 33.3 \
                or low.get("p99Ms", 1e9) > 50 or low.get("maxMs", 1e9) > 250 \
                or reference.get("p95Ms", 1e9) > 20 \
                or reference.get("p99Ms", 1e9) > 33.3 \
                or reference.get("maxMs", 1e9) > 250 \
                or observed.get("preparedReopenSeconds", 1e9) > 30 \
                or observed.get("internalResolutionPercent") != 100 \
                or not frames_path.is_file() \
                or observed.get("frameSamplesSha256") != p0.sha(frames_path) \
                or observed.get("cacheBudgetBytes") != DEFAULT_TERRAINCORE_CACHE_BYTES:
            raise p0.Failed("Packaged performance-regression receipt is invalid")
    elif scenario == "medium-regression":
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or observed.get("qualityTier") != "medium" \
                or observed.get("schemaVersion") != 2 or not observed.get("nativeV2") \
                or not observed.get("ready") or not observed.get("picked") \
                or not observed.get("mutationObserved") or not observed.get("offlineReopen") \
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
            "process_network_audit": (observed.get("processNetworkAudit")
                                      if scenario in ("terraincore-regression", "medium-regression",
                                                      "ui-layout")
                                      else process_audit),
            "network_policy": ("production acquisition port denied; process-attributed TCP "
                               "observation found no listeners or remote endpoints"
                               if scenario in ("offline-reopen", "terraincore-regression",
                                               "medium-regression")
                               else "fixture-only")}


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
    parser.add_argument("command", choices=["freeze", "freeze-check", "doctor", "check", "domain", "build", "automation", "tiff", "acquisition", "ui", "terraincore", "p1", "assets", "package", "smoke", "visual"])
    parser.add_argument("--engine-root")
    parser.add_argument("--configuration", choices=["Development", "Shipping"], default="Development")
    parser.add_argument("--target", choices=["Editor", "Game"], default="Editor")
    parser.add_argument("--scenario", choices=["selector", "import", "offline-reopen", "geotiff-regression", "acquisition-regression", "ui-layout", "terraincore-regression", "medium-regression", "performance-regression"], default="import")
    parser.add_argument("--content-id")
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
            if args.configuration == "Shipping":
                result["shipping_mcp_proof"] = shipping_target_module_proof()
        elif args.command == "smoke":
            result = smoke(args.configuration, before["sha256"], args.scenario, args.content_id, run_output)
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
    latest = args.command + ("-" + args.configuration if args.command in ("package", "smoke", "visual") else "")
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
