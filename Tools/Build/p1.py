"""Failure-propagating P1 Unreal build, package, automation, and packaged-smoke harness."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import sys
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "test-results/p1"
PROJECT = ROOT / "SkiAreaDesignChallenge.uproject"

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
        "Source/SkiPreparation/Public/SkiPreparation/SelectorProtocol.h",
        "Source/SkiTerrainRuntime/Public/SkiTerrainRuntime/SkiTerrainActor.h",
        "Content/P1Selector/index.html",
        "Content/P1Selector/maplibre-gl.js",
        "Content/P1Selector/maplibre-gl.css",
        "Content/P1Selector/MAPLIBRE-LICENSE.txt",
        "docs/UnrealRebuild/P1-runbook.md",
    ]
    missing = [value for value in required if not (ROOT / value).is_file()]
    if missing:
        raise p0.Failed("P1 inputs missing: " + ", ".join(missing))
    project = json.loads(PROJECT.read_text(encoding="utf-8"))
    modules = {entry["Name"] for entry in project["Modules"]}
    if not {"SkiPreparation", "SkiTerrainRuntime"}.issubset(modules):
        raise p0.Failed("P1 runtime modules are absent from the project descriptor")
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


def automation(environment: dict, run_output: Path) -> dict:
    engine = p0.engine_path(environment)
    log_name = "p1-automation.log"
    output = p0.checked([
        str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT),
        "-ExecCmds=Automation RunTests MountainPlanner.P1.Preparation;Quit",
        "-TestExit=Automation Test Queue Empty", "-unattended", "-nop4", "-NullRHI",
        "-stdout", "-FullStdOutLogOutput",
    ], timeout=300, log=log_name)
    if "Test Completed. Result={Success}" not in output or "Automation Test Queue Empty" not in output:
        raise p0.Failed("P1 automation did not report a successful, completed test queue")
    return {"status": "PASS", "kind": "unreal_automation", "filter": "MountainPlanner.P1.Preparation"}


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
    if report.get("source_before") != source_digest or report.get("result", {}).get("status") != "PASS":
        raise p0.Failed("P1 package receipt is stale or failed")
    launcher = Path(report["result"]["launcher"]).resolve()
    if not launcher.is_file() or p0.sha(launcher) != report["result"]["launcher_sha256"]:
        raise p0.Failed("Recorded P1 packaged launcher is missing or changed")
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
    smoke_flag = "-SkiP1SelectorSmoke" if scenario == "selector" else "-SkiP1Smoke"
    command = [str(launcher), smoke_flag, f"-SkiP1Token={token}",
               f"-SkiP1Receipt={receipt}", f"-SkiP1DataRoot={data_root}",
               f"-SkiP1Scenario={scenario}", f"-UserDir={unreal_user_dir}",
               "-windowed", "-ResX=1280", "-ResY=720",
               "-unattended", "-nosplash"]
    if scenario == "offline-reopen":
        if not content_id or len(content_id) != 64:
            raise p0.Failed("Offline reopen requires the exact contentId from an import receipt")
        command.append(f"-SkiP1ContentId={content_id}")
    p0.checked(command, timeout=120, log=f"p1-{scenario}-{token}.log")
    if not receipt.is_file():
        raise p0.Failed("Packaged player exited without its tokened P1 receipt")
    observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
    if scenario == "selector":
        if observed != {"token": token, "selector": True, "profile": "standard"}:
            raise p0.Failed("Packaged selector receipt is stale or CEF/WebGL/bridge validation failed")
    elif observed.get("token") != token or observed.get("scenario") != scenario \
            or not observed.get("ready") or not observed.get("picked") or not observed.get("reopened"):
        raise p0.Failed("Packaged P1 receipt is stale or qualification steps failed")
    verify_package_report(configuration, source_digest)
    return {"status": "PASS", "kind": "packaged_p1", "scenario": scenario,
            "receipt": observed, "package_invocation": package_report["invocation"],
            "network_policy": "offline-reopen code path performs package-store reads only" if scenario == "offline-reopen" else "fixture-only"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["doctor", "check", "domain", "build", "automation", "assets", "package", "smoke"])
    parser.add_argument("--engine-root")
    parser.add_argument("--configuration", choices=["Development", "Shipping"], default="Development")
    parser.add_argument("--target", choices=["Editor", "Game"], default="Editor")
    parser.add_argument("--scenario", choices=["selector", "import", "offline-reopen"], default="import")
    parser.add_argument("--content-id")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    invocation = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
    run_output = OUTPUT / "runs" / invocation
    run_output.mkdir(parents=True)
    p0.OUTPUT = OUTPUT
    p0.RUN_OUTPUT = run_output
    environment = p0.doctor(args.engine_root)
    before = p0.source_snapshot()
    code = 0
    try:
        if args.command == "doctor":
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
        else:
            result = smoke(args.configuration, before["sha256"], args.scenario, args.content_id, run_output)
        after = p0.source_snapshot()
        if args.command != "assets":
            p0.verify_frozen(before, after)
    except p0.Blocked as error:
        result = {"status": "BLOCKED", "reason": str(error)}
        code = 2
    except (RuntimeError, OSError, ValueError) as error:
        result = {"status": "FAIL", "reason": str(error)}
        code = 1
    report = {"invocation": invocation, "command": args.command, "configuration": args.configuration,
              "source_before": before["sha256"], "environment": environment, "result": result}
    serialized = json.dumps(report, indent=2) + "\n"
    (run_output / "report.json").write_text(serialized, encoding="utf-8")
    latest = args.command + ("-" + args.configuration if args.command in ("package", "smoke") else "")
    (OUTPUT / f"{latest}.json").write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
