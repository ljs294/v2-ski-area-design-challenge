"""P0 entrypoint. Missing native prerequisites are BLOCKED (exit 2), never PASS."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unittest
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "test-results/p0"
RUN_OUTPUT = OUTPUT
PROJECT = ROOT / "SkiAreaDesignChallenge.uproject"


class Blocked(RuntimeError):
    pass


class Failed(RuntimeError):
    pass


class TimedOut(Failed):
    pass


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], *, timeout: float = 120, cwd: Path = ROOT, separate_stderr: bool = False,
        log_path: Path | None = None) -> subprocess.CompletedProcess:
    """No shell interpolation. Timeout terminates only this invocation's process tree."""
    options = {}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NO_WINDOW
    # Direct file output keeps long UBT/cook logs inspectable while a child runs,
    # and preserves its diagnostic output even if it must be terminated.
    with log_path.open("w", encoding="utf-8") if log_path else nullcontext(None) as log_stream:
        process = subprocess.Popen(command, cwd=cwd, stdout=log_stream or subprocess.PIPE,
                                   stderr=subprocess.PIPE if separate_stderr else subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace", **options)
        try:
            output, error_output = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            if os.name == "nt":
                try:
                    subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            # Our process handle remains usable if taskkill cannot enumerate.
            if process.poll() is None:
                process.kill()
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                raise TimedOut(f"Timed out; a descendant still holds the output pipe: {command[0]}") from error
            raise TimedOut(f"Timed out after {timeout}s: {command[0]}") from error
    if log_path:
        output = log_path.read_text(encoding="utf-8", errors="replace")
    return subprocess.CompletedProcess(command, process.returncode, output, error_output or "")


def checked(command: list[str], *, timeout: float = 120, log: str | None = None) -> str:
    if log:
        RUN_OUTPUT.mkdir(parents=True, exist_ok=True)
    result = run(command, timeout=timeout, log_path=RUN_OUTPUT / log if log else None)
    if result.returncode != 0:
        raise Failed(f"Exit {result.returncode}: {command[0]}\n{result.stdout[-3000:]}")
    return result.stdout


def parse_native_result(result: subprocess.CompletedProcess) -> dict:
    if result.returncode != 0:
        raise Failed(f"Native tests exited {result.returncode}")
    records = re.findall(r"^SKI_TEST_RESULT (.+)$", result.stdout, re.MULTILINE)
    if len(records) != 1:
        raise Failed("Expected exactly one native test report")
    try:
        report = json.loads(records[0])
        values = [report[key] for key in ("total", "passed", "failed")]
    except (ValueError, KeyError, TypeError) as error:
        raise Failed("Invalid native test report") from error
    if any(type(value) is not int or value < 0 for value in values):
        raise Failed("Invalid native test counts")
    total, passed, failed = values
    if total == 0 or failed != 0 or passed != total:
        raise Failed(f"Native tests did not pass: {report}")
    return report


def resolve_inside(root: Path, relative: str) -> Path:
    target = (root / relative).resolve()
    if not target.is_relative_to(root.resolve()):
        raise Failed(f"Path escapes root: {relative}")
    return target


def source_snapshot(root: Path = ROOT) -> dict:
    result = run(["git", "ls-files", "-c", "-o", "--exclude-standard", "-z"], cwd=root, separate_stderr=True)
    if result.returncode:
        raise Failed("Cannot inventory tracked and untracked inputs")
    files = {}
    for relative in sorted(set(result.stdout.split("\0")) - {""}):
        path = resolve_inside(root, relative)
        # Deletions are part of source identity too.
        files[relative.replace("\\", "/")] = sha(path) if path.is_file() else "MISSING"
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {"sha256": digest, "files": files}


def verify_frozen(before: dict, after: dict) -> None:
    if before != after:
        changed = sorted(key for key in set(before["files"]) | set(after["files"])
                         if before["files"].get(key) != after["files"].get(key))
        raise Failed(f"Source changed during verification: {changed}")


def verify_asset_changes(before: dict, after: dict) -> None:
    allowed = {"Content/P0Generated/Bootstrap.umap", "Content/P0Generated/M_Bootstrap.uasset",
               "Content/P0Generated/WBP_Bootstrap.uasset", "Content/P0Generated/ownership.json"}
    changed = {key for key in set(before["files"]) | set(after["files"])
               if before["files"].get(key) != after["files"].get(key)}
    if changed - allowed:
        raise Failed(f"Asset generation changed unowned source: {sorted(changed - allowed)}")


def package_manifest(directory: Path) -> dict:
    """Fingerprint shipped files; Unreal's runtime Saved directories are outputs."""
    files = {}
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory)
        if "Saved" in relative.parts:
            continue
        resolve_inside(directory, relative.as_posix())
        if path.is_file():
            files[relative.as_posix()] = sha(path)
    if not files:
        raise Failed("Packaged output is empty")
    return {"sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(), "files": files}


def verify_package_receipt(receipt: dict, source_digest: str, configuration: str) -> Path:
    result = receipt.get("result", {})
    if (receipt.get("command") != "package" or receipt.get("configuration") != configuration
            or receipt.get("source_before") != source_digest or result.get("status") != "PASS"):
        raise Failed("Package receipt does not match this source/configuration; package again")
    directory = Path(result["directory"])
    launcher = Path(result["launcher"])
    if not launcher.resolve().is_relative_to(directory.resolve()) or not launcher.is_file():
        raise Failed("Package launcher is missing or outside the recorded package")
    if package_manifest(directory) != result["manifest"]:
        raise Failed("Packaged files changed since the cook; package again")
    return launcher


def netfx_sdks() -> list[dict]:
    """Use the same 32-bit SDK registration queried by UnrealBuildTool."""
    if os.name != "nt":
        return []
    import winreg
    found = []
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Microsoft SDKs\NETFXSDK",
                            0, winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as sdk_key:
            for index in range(winreg.QueryInfoKey(sdk_key)[0]):
                version = winreg.EnumKey(sdk_key, index)
                try:
                    with winreg.OpenKey(sdk_key, version) as version_key:
                        directory = Path(winreg.QueryValueEx(version_key, "KitsInstallationFolder")[0])
                    if directory.is_dir():
                        found.append({"version": version, "root": str(directory)})
                except OSError:
                    continue
    except OSError:
        pass
    return found


def doctor(engine_root: str | None = None) -> dict:
    candidates = []
    explicit = engine_root or os.environ.get("UE_ROOT")
    if explicit:
        candidates.append(Path(explicit))
    else:
        launcher = Path(os.environ.get("PROGRAMDATA", "C:/ProgramData")) / "Epic/UnrealEngineLauncher/LauncherInstalled.dat"
        if launcher.is_file():
            for item in json.loads(launcher.read_text(encoding="utf-8-sig")).get("InstallationList", []):
                if item.get("AppName", "").startswith("UE_"):
                    candidates.append(Path(item["InstallLocation"]))
        candidates.extend(Path("C:/Program Files/Epic Games").glob("UE_5.8*"))
    engines = []
    for candidate in candidates:
        version_file = candidate / "Engine/Build/Build.version"
        if version_file.is_file():
            version = json.loads(version_file.read_text(encoding="utf-8-sig"))
            pending = any((candidate / ".egstore/Pending").glob("*.manifest"))
            complete = not pending and all((candidate / name).is_file() for name in (
                "Engine/Build/BatchFiles/Build.bat", "Engine/Build/BatchFiles/RunUAT.bat",
                "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"))
            engines.append({"root": str(candidate.resolve()), "version": version,
                            "installation_complete": complete,
                            "build_version_sha256": sha(version_file)})
    selected = next((engine for engine in engines if (engine["version"].get("MajorVersion"),
                                                     engine["version"].get("MinorVersion")) == (5, 8)), None)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    visual_studio = []
    if vswhere.is_file():
        result = run([str(vswhere), "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json"])
        if result.returncode == 0:
            visual_studio = json.loads(result.stdout)
    compilers = [str(path) for instance in visual_studio for path in
                 Path(instance["installationPath"]).glob("VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe")]
    path_compiler = shutil.which("cl")
    if path_compiler and path_compiler not in compilers:
        compilers.append(path_compiler)
    cmake = shutil.which("cmake")
    if not cmake:
        cmake = next((str(path) for instance in visual_studio for path in
                      Path(instance["installationPath"]).glob("Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")), None)
    sdk_root = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10"
    sdks = sorted(path.name for path in (sdk_root / "Include").glob("*") if (path / "um/Windows.h").is_file())
    framework_sdks = netfx_sdks()
    missing = []
    if not selected:
        missing.append("Unreal Engine 5.8: provide --engine-root or UE_ROOT if installed elsewhere")
    elif not selected["installation_complete"]:
        missing.append("Unreal Engine installation is still in progress or required files are missing")
    if not compilers:
        missing.append("MSVC C++ tools")
    if not sdks:
        missing.append("Windows SDK")
    if not cmake:
        missing.append("CMake >=3.24")
    if not framework_sdks:
        missing.append(".NET Framework SDK (editor-only prerequisite; install Microsoft.Net.Component.4.8.SDK)")
    return {"status": "BLOCKED" if missing else "AVAILABLE_NOT_QUALIFIED", "engine": selected,
            "cmake": cmake, "compilers": sorted(compilers), "windows_sdks": sdks, "netfx_sdks": framework_sdks,
            "missing": missing, "native_tests_executed": False}


def static_checks() -> dict:
    manifest = json.loads((ROOT / "Tools/Build/domain-sources.json").read_text())
    listed = set(manifest["sources"])
    actual = {path.relative_to(ROOT).as_posix() for path in (ROOT / "Source/SkiDomain/Private").rglob("*.cpp")
              if path.name != "SkiDomainModule.cpp"}
    if not listed or listed != actual:
        raise Failed("Pure C++ source manifest differs from UBT discovery")
    headers = {path.relative_to(ROOT).as_posix() for path in (ROOT / "Source/SkiDomain/Public").rglob("*.h")}
    if set(manifest["headers"]) != headers:
        raise Failed("Pure header manifest differs from public headers")
    for relative in manifest["sources"] + manifest["headers"]:
        if not resolve_inside(ROOT, relative).is_file():
            raise Failed(f"Missing pure input: {relative}")
    project = json.loads(PROJECT.read_text())
    kinds = {module["Name"]: module["Type"] for module in project["Modules"]}
    if kinds != {"SkiDomain": "Runtime", "SkiApplication": "Runtime", "SkiPresentation": "Runtime", "SkiEditor": "Editor"}:
        raise Failed("Unexpected module boundary")
    if any(plugin.get("Enabled", True) and plugin.get("TargetAllowList") != ["Editor"]
           for plugin in project["Plugins"]):
        raise Failed("Bootstrap development plugins must be Editor-only")
    checked(["node", "scripts/checkAgentDocs.mjs"])
    checked([sys.executable, "Tools/Preparation/check_inventory.py"])
    provenance = json.loads((ROOT / "docs/UnrealRebuild/provenance.json").read_text())
    archive = ROOT / "docs/UnrealRebuild/planning-pack-v0.9.zip"
    if sha(archive) != provenance["archive_sha256"]:
        raise Failed("Planning archive differs from the reviewed input")
    with zipfile.ZipFile(archive) as pack:
        if pack.testzip() is not None:
            raise Failed("Planning archive CRC failed")
        prefix = "unreal-port-plan-v0.9/"
        delivery = json.loads(pack.read(prefix + "MANIFEST.json"))
        expected = {entry["path"] for entry in delivery["files"]}
        actual = {name.removeprefix(prefix) for name in pack.namelist()} - {"MANIFEST.json"}
        if actual != expected or len(expected) != len(delivery["files"]):
            raise Failed("Planning delivery manifest coverage differs")
        for entry in delivery["files"]:
            data = pack.read(prefix + entry["path"])
            if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise Failed(f"Planning delivery hash failed: {entry['path']}")
    return {"status": "PASS", "kind": "static_only", "pure_sources": sorted(listed),
            "archive_entries": len(expected), "native_compile": "NOT_EXECUTED"}


def tooling_checks() -> dict:
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tests/Tooling"), pattern="test_*.py")
    count = suite.countTestCases()
    if count == 0:
        raise Failed("No tooling tests discovered")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful() or result.skipped:
        raise Failed("Tooling tests failed or skipped")
    return {"status": "PASS", "tests": count, "kind": "python_tooling_only"}


def domain_checks(environment: dict) -> dict:
    if not environment["cmake"] or not environment["compilers"] or not environment["windows_sdks"]:
        raise Blocked("Standalone C++ tests require CMake, MSVC and the Windows SDK")
    cmake = environment["cmake"]
    build = OUTPUT / "domain-msvc"
    checked([cmake, "--preset", "p0-msvc", "-S", str(ROOT), "-B", str(build)],
            timeout=180, log="domain-configure.log")
    checked([cmake, "--build", str(build), "--config", "Debug"], timeout=300, log="domain-build.log")
    toolchain = json.loads((build / "toolchain.json").read_text(encoding="utf-8"))
    toolchain["compiler_sha256"] = sha(Path(toolchain["compiler"]))
    toolchain["cmake_sha256"] = sha(Path(cmake))
    for fixture in ("ForbiddenDirect", "ForbiddenTransitive", "ForbiddenResolved"):
        shutil.copyfile(build / f"negative/{fixture}.log", RUN_OUTPUT / f"{fixture}.log")
    executable = build / "Debug/SkiDomainTests.exe"
    if not executable.is_file():
        executable = build / "SkiDomainTests.exe"
    native_result = run([str(executable)], timeout=10)
    (RUN_OUTPUT / "domain-tests.log").write_text(native_result.stdout, encoding="utf-8")
    report = parse_native_result(native_result)
    controls = []
    control_results = {}
    for mode in ("--fail", "--empty", "--crash", "--hang"):
        try:
            control = run([str(executable), mode], timeout=2)
        except TimedOut as error:
            if mode != "--hang":
                raise Failed(f"Control {mode} timed out instead of terminating") from error
            control_results[mode] = {"outcome": "timeout"}
            (RUN_OUTPUT / f"control{mode}.log").write_text(str(error), encoding="utf-8")
            controls.append(mode)
            continue
        if mode == "--hang":
            raise Failed("Hang control returned instead of exercising the timeout")
        if mode == "--crash" and (control.returncode == 0 or "SKI_TEST_RESULT" in control.stdout):
            raise Failed("Crash control did not terminate abnormally without a result")
        if mode == "--empty" and control.returncode != 0:
            raise Failed("Empty control must return zero so the report validator rejects it")
        if mode == "--fail" and control.returncode != 1:
            raise Failed("Failed assertion control must return exit 1")
        control_results[mode] = {"outcome": "terminated", "exit": control.returncode}
        try:
            (RUN_OUTPUT / f"control{mode}.log").write_text(
                f"exit={control.returncode}\n{control.stdout}", encoding="utf-8")
            parse_native_result(control)
        except Failed:
            controls.append(mode)
        else:
            raise Failed(f"Native negative control incorrectly passed: {mode}")
    return {"status": "PASS", "kind": "standalone_cpp", "report": report, "rejected_controls": controls,
            "control_results": control_results,
            "toolchain": toolchain, "executable_sha256": sha(executable),
            "forbidden_header_controls": ["ForbiddenDirect", "ForbiddenTransitive", "ForbiddenResolved"]}


def engine_path(environment: dict) -> Path:
    if not environment["engine"] or not environment["compilers"] or not environment["windows_sdks"]:
        raise Blocked("Native operations require Unreal 5.8, MSVC and Windows SDK; run doctor")
    if not environment["engine"]["installation_complete"]:
        raise Blocked("Wait for Unreal Engine installation and verification to finish")
    lock = json.loads((ROOT / "Tools/Build/toolchain-lock.json").read_text(encoding="utf-8"))
    if environment["engine"]["build_version_sha256"] != lock["engine_build_version_sha256"]:
        raise Blocked("Engine patch differs from toolchain-lock.json; requalify before changing the lock")
    if lock["windows_sdk"] not in environment["windows_sdks"] or not any(
            sha(Path(path)) == lock["msvc_compiler_sha256"] for path in environment["compilers"]):
        raise Blocked("Installed compiler/SDK differs from toolchain-lock.json")
    return Path(environment["engine"]["root"])


def native_build(environment: dict, target: str, configuration: str) -> dict:
    engine = engine_path(environment)
    if target == "Editor" and not environment["netfx_sdks"]:
        raise Blocked("The Unreal editor requires the .NET Framework SDK; install Microsoft.Net.Component.4.8.SDK")
    if target == "Editor" and configuration != "Development":
        raise Failed("The P0 editor target uses Development configuration")
    target_name = "SkiAreaDesignChallenge" + ("Editor" if target == "Editor" else "")
    checked([str(engine / "Engine/Build/BatchFiles/Build.bat"), target_name, "Win64", configuration,
             str(PROJECT), "-WaitMutex", "-NoHotReloadFromIDE", "-NoUBA"], timeout=1800,
            log=f"{target.lower()}-build.log")
    return {"status": "PASS", "kind": "native_build_only", "target": target_name,
            "configuration": configuration, "engine": environment["engine"]}


def create_assets(environment: dict) -> dict:
    engine = engine_path(environment)
    command = [str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(PROJECT), "-run=pythonscript",
               f"-script={ROOT / 'Tools/Editor/create_bootstrap_assets.py'}", "-unattended", "-nop4", "-NullRHI",
               "-stdout", "-FullStdOutLogOutput"]
    checked(command, timeout=300, log="bootstrap-assets-first.log")
    sys.path.insert(0, str(ROOT / "Tools/Editor"))
    from asset_ownership import preflight, recipe_digest, ASSETS
    receipt = preflight(ROOT, recipe_digest(ROOT))
    if set(receipt["assets"]) != set(ASSETS):
        raise Failed("Asset generator exited without all owned assets")
    before_repeat = source_snapshot()
    checked(command, timeout=300, log="bootstrap-assets-repeat.log")
    verify_frozen(before_repeat, source_snapshot())
    repeated_receipt = preflight(ROOT, recipe_digest(ROOT))
    if repeated_receipt != receipt:
        raise Failed("Second asset generation changed the ownership receipt")
    return {"status": "PASS", "kind": "editor_asset_generation_only", "assets": receipt,
            "second_invocation": "PASS: source and asset bytes unchanged"}


def package(environment: dict, configuration: str) -> dict:
    engine = engine_path(environment)
    sys.path.insert(0, str(ROOT / "Tools/Editor"))
    from asset_ownership import preflight, recipe_digest, ASSETS
    receipt = preflight(ROOT, recipe_digest(ROOT))
    if set(receipt["assets"]) != set(ASSETS):
        raise Blocked("Generate the owned bootstrap assets before cooking")
    # A new archive prevents a prior cook's stale files from entering evidence.
    destination = OUTPUT / "packages" / configuration / RUN_OUTPUT.name
    checked([str(engine / "Engine/Build/BatchFiles/RunUAT.bat"), "BuildCookRun", f"-project={PROJECT}",
             "-noP4", "-platform=Win64", f"-clientconfig={configuration}", "-build", "-cook", "-stage", "-pak",
             "-archive", f"-archivedirectory={destination}", "-unattended", "-utf8output", "-ubtargs=-NoUBA"], timeout=3600,
            log=f"package-{configuration}.log")
    launchers = list(destination.glob("*/SkiAreaDesignChallenge.exe"))
    if len(launchers) != 1:
        raise Failed("Cook did not produce exactly one packaged launcher")
    return {"status": "PASS", "kind": "package_only", "configuration": configuration,
            "directory": str(destination), "manifest": package_manifest(destination),
            "launcher": str(launchers[0]), "launcher_sha256": sha(launchers[0]), "engine": environment["engine"]}


def smoke(executable: str | None, configuration: str, source_digest: str) -> dict:
    package_receipt = OUTPUT / f"package-{configuration}.json"
    if not package_receipt.is_file():
        raise Blocked(f"Package {configuration} before its startup check")
    receipt_data = json.loads(package_receipt.read_text(encoding="utf-8"))
    launcher = verify_package_receipt(receipt_data, source_digest, configuration)
    if executable and Path(executable).resolve() != launcher.resolve():
        raise Failed("--executable differs from the recorded package launcher")
    token = str(uuid.uuid4())
    receipt = RUN_OUTPUT / f"smoke-{token}.json"
    checked([str(launcher.resolve()), "-SkiP0Smoke", f"-SkiP0Token={token}", f"-SkiP0Receipt={receipt}",
             "-windowed", "-ResX=800", "-ResY=600", "-unattended", "-nosplash"], timeout=90, log=f"smoke-{token}.log")
    if not receipt.is_file():
        raise Failed("Player exited without its unique readiness receipt")
    observed = json.loads(receipt.read_text(encoding="utf-8-sig"))
    if observed != {"token": token, "ready": True}:
        raise Failed("Player receipt is stale or bootstrap is not ready")
    verify_package_receipt(receipt_data, source_digest, configuration)
    return {"status": "PASS", "kind": "packaged_startup_only", "receipt": observed,
            "package_invocation": receipt_data["invocation"], "configuration": configuration,
            "visual_approval": "NOT_EXECUTED", "clean_machine": "NOT_EXECUTED", "offline": "NOT_EXECUTED"}


def main() -> int:
    global RUN_OUTPUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["doctor", "check", "domain", "build", "assets", "package", "smoke", "snapshot"])
    parser.add_argument("--engine-root")
    parser.add_argument("--configuration", choices=["Development", "Shipping"], default="Development")
    parser.add_argument("--target", choices=["Editor", "Game"], default="Editor")
    parser.add_argument("--executable")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    invocation = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
    RUN_OUTPUT = OUTPUT / "runs" / invocation
    RUN_OUTPUT.mkdir(parents=True)
    environment = doctor(args.engine_root)
    (OUTPUT / "environment.json").write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")
    before = source_snapshot()
    code = 0
    try:
        if args.command == "doctor":
            result = environment
            code = 2 if environment["missing"] else 0
        elif args.command == "snapshot":
            result = before
        elif args.command == "check":
            result = {"status": "PASS", "static": static_checks(), "tooling": tooling_checks(), "native": "NOT_EXECUTED"}
        elif args.command == "domain":
            result = domain_checks(environment)
        elif args.command == "build":
            result = native_build(environment, args.target, args.configuration)
        elif args.command == "assets":
            result = create_assets(environment)
        elif args.command == "package":
            result = package(environment, args.configuration)
        else:
            result = smoke(args.executable, args.configuration, before["sha256"])
        after = source_snapshot()
        if args.command == "assets":
            verify_asset_changes(before, after)
        else:
            verify_frozen(before, after)
    except Blocked as error:
        result = {"status": "BLOCKED", "reason": str(error)}
        code = 2
    except (RuntimeError, OSError, ValueError) as error:
        result = {"status": "FAIL", "reason": str(error)}
        code = 1
    report = {"invocation": invocation, "command": args.command, "configuration": args.configuration,
              "environment": environment, "source_before": before["sha256"], "result": result,
              "independent_review": "NOT_EXECUTED", "freeze": "before/after comparison; not enforced read-only isolation"}
    serialized = json.dumps(report, indent=2) + "\n"
    (RUN_OUTPUT / "report.json").write_text(serialized, encoding="utf-8")
    (RUN_OUTPUT / "source-manifest.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
    latest = args.command + ("-" + args.configuration if args.command in ("package", "smoke") else "")
    (OUTPUT / f"{latest}.json").write_text(serialized, encoding="utf-8")
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
