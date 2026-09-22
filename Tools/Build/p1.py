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
import sys
import uuid
import zipfile
import struct

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
P1_TESTS = tuple(sorted(TIFF_TESTS + ACQUISITION_TESTS + UI_TESTS + (
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
        "Source/SkiPreparation/Public/SkiPreparation/TerrainAcquisition.h",
        "Source/SkiPreparation/Public/SkiPreparation/SelectorProtocol.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/SkiTerrainActor.h",
        "Content/P1Selector/index.html",
        "Content/P1Selector/maplibre-gl.js",
        "Content/P1Selector/maplibre-gl.css",
        "Content/P1Selector/MAPLIBRE-LICENSE.txt",
        "Content/P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64",
        "Tools/Preparation/generate_tiff_fixture.py",
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
    return report, launcher


def smoke(configuration: str, source_digest: str, scenario: str, content_id: str | None,
          run_output: Path) -> dict:
    package_report, launcher = verify_package_report(configuration, source_digest)
    token = str(uuid.uuid4())
    data_root = (OUTPUT / "isolated-data" / configuration).resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    unreal_user_dir = data_root / "unreal-user" / token
    unreal_user_dir.mkdir(parents=True, exist_ok=True)
    receipt = data_root / f"{token}.receipt.json"
    smoke_flag = ("-SkiP1SelectorSmoke" if scenario == "selector" else
                  "-SkiP1UiLayoutSmoke" if scenario == "ui-layout" else "-SkiP1Smoke")
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

    try:
        p0.checked(command, timeout=120, log=f"p1-{scenario}-{token}.log")
    except p0.Failed as error:
        context = collect_failure_context(str(error))
        raise p0.Failed(f"{error}; packaged failure context: {context}") from error
    if not receipt.is_file():
        context = collect_failure_context("Packaged player exited without its tokened P1 receipt")
        raise p0.Failed(f"Packaged player exited without its tokened P1 receipt; context: {context}")
    observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
    if scenario == "selector":
        if observed != {"token": token, "selector": True, "profile": "standard"}:
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
        if observed.get("token") != token or observed.get("scenario") != scenario \
                or not observed.get("recoveryActionsReachable") \
                or not observed.get("inputIsolation") \
                or observed.get("resolution") != [1280, 720] or observed.get("rightInset", 0) <= 0:
            raise p0.Failed("Packaged UI-layout regression receipt is invalid")
    elif observed.get("token") != token or observed.get("scenario") != scenario \
            or not observed.get("ready") or not observed.get("picked") or not observed.get("reopened"):
        raise p0.Failed("Packaged P1 receipt is stale or qualification steps failed")
    verify_package_report(configuration, source_digest)
    return {"status": "PASS", "kind": "packaged_p1", "scenario": scenario,
            "receipt": observed, "package_invocation": package_report["invocation"],
            "network_policy": "offline-reopen code path performs package-store reads only" if scenario == "offline-reopen" else "fixture-only"}


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
    content_id = None
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
        if content_id:
            command.append(f"-SkiP1ContentId={content_id}")
        p0.checked(command, timeout=90, log=f"p1-visual-{index:02d}-{token}.log")
        if not receipt.is_file() or not screenshot.is_file():
            raise p0.Failed(f"Visual capture {index} did not create its tokened files")
        observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
        png = screenshot.read_bytes()
        actual_size = struct.unpack(">II", png[16:24]) if png.startswith(b"\x89PNG\r\n\x1a\n") else None
        if observed.get("token") != token or observed.get("screenshotSha256") != p0.sha(screenshot) \
                or actual_size != (width, height) \
                or observed.get("resolution") != [width, height]:
            raise p0.Failed(f"Visual capture {index} receipt or screenshot hash is invalid")
        observed["screenshot"] = str(screenshot)
        if content_id is None:
            content_id = observed.get("contentId")
        elif observed.get("contentId") != content_id:
            raise p0.Failed(f"Visual capture {index} did not reuse the recorded immutable package")
        receipts.append(observed)
    verify_package_report(configuration, source_digest)
    return {"status": "PASS", "kind": "packaged_visual_capture", "captures": receipts,
            "fixture": "synthetic", "package_invocation": package_report["invocation"]}


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["freeze", "freeze-check", "doctor", "check", "domain", "build", "automation", "tiff", "acquisition", "ui", "assets", "package", "smoke", "visual"])
    parser.add_argument("--engine-root")
    parser.add_argument("--configuration", choices=["Development", "Shipping"], default="Development")
    parser.add_argument("--target", choices=["Editor", "Game"], default="Editor")
    parser.add_argument("--scenario", choices=["selector", "import", "offline-reopen", "geotiff-regression", "acquisition-regression", "ui-layout"], default="import")
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
