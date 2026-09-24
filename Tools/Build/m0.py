"""Run the packaged M0 TerrainCore and native-map gates.

Usage: python Tools/Build/m0.py --launcher PATH_TO_PACKAGED_EXE
Receipts and a summary are retained under test-results/m0/<run-id>.
An incomplete measurement is deliberately a failing gate.
"""

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import uuid


ROOT = Path(__file__).resolve().parents[2]
RAM_LIMIT = 2 * 1024 ** 3


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def required_number(data, key, issues, label):
    value = data.get(key)
    if not number(value):
        issues.append(f"missing or invalid {label}.{key}")
        return None
    return value


def bound(value, limit, issues, label):
    if value is not None and value > limit:
        issues.append(f"{label}={value} exceeds {limit}")


def read_receipt(path, token, scenario, issues):
    if not path.is_file():
        issues.append("missing tokened receipt")
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        issues.append(f"invalid receipt JSON: {error}")
        return None
    if not isinstance(data, dict) or data.get("token") != token or data.get("scenario") != scenario:
        issues.append("receipt token or scenario mismatch")
        return None
    return data


def check_scale(receipt, side, issues):
    if receipt.get("side") != side or receipt.get("passed") is not True or receipt.get("error"):
        issues.append("scale scenario failed or identified the wrong side")
    try:
        report = json.loads(receipt["report"])
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        issues.append(f"missing or invalid scale report JSON: {error}")
        return
    if not isinstance(report, dict) or report.get("side") != side:
        issues.append("scale report side mismatch")
        return
    build = required_number(report, "buildAndActivateSeconds", issues, "scale")
    reopen = required_number(report, "reopenSeconds", issues, "scale")
    peak = required_number(report, "observedPeakPhysicalBytes", issues, "scale")
    generation = required_number(report, "generationSeconds", issues, "scale")
    verify = required_number(report, "verifySeconds", issues, "scale")
    required_number(report, "firstTileReadSeconds", issues, "scale")
    required_number(report, "installedBytes", issues, "scale")
    required_number(report, "scratchBytes", issues, "scale")
    if side == 10001:
        if all(value is not None for value in (generation, build, verify)):
            bound(generation + build + verify, 600, issues, "scale.nonNetworkBuildSeconds")
        bound(reopen, 30, issues, "scale.reopenSeconds")
        bound(peak, RAM_LIMIT, issues, "scale.observedPeakPhysicalBytes")
        cancel = required_number(report, "cancelReturnMs", issues, "scale")
        bound(cancel, 250, issues, "scale.cancelReturnMs")
        if report.get("cancelActivated") is not False:
            issues.append("scale cancellation activated a package or is unreported")
        for metric in ("lodSeconds", "shardWriteSeconds", "stagingBytes", "cleanupMs"):
            required_number(report, metric, issues, "scale")
    else:
        # The small run carries the bit-exact LOD check inside the native probe.
        bound(build, 600, issues, "scale.buildAndActivateSeconds")


def check_map(receipt, resolution, issues):
    if receipt.get("durationSeconds") != 300:
        issues.append("map duration is not 300 seconds")
    frames = required_number(receipt, "frames", issues, "map")
    if frames == 0:
        issues.append("map recorded no frames")
    frame_ms = receipt.get("frameMs")
    if not isinstance(frame_ms, dict):
        issues.append("missing map.frameMs")
    else:
        p50 = required_number(frame_ms, "p50", issues, "map.frameMs")
        p95 = required_number(frame_ms, "p95", issues, "map.frameMs")
        p99 = required_number(frame_ms, "p99", issues, "map.frameMs")
        if all(v is not None for v in (p50, p95, p99)) and not p50 <= p95 <= p99:
            issues.append("map frame percentiles are unordered")
        bound(p95, 16.7, issues, "map.frameMs.p95")
        bound(p99, 33.3, issues, "map.frameMs.p99")
    if receipt.get("teardownToBaseline") is not True:
        issues.append("map teardown did not return to baseline")
    memory = receipt.get("memory")
    if not isinstance(memory, dict):
        issues.append("missing map.memory")
    else:
        for metric in ("baselinePhysicalBytes", "peakPhysicalBytes", "afterPhysicalBytes",
                       "baselineTextures", "afterTextures", "baselineUObjects", "afterUObjects"):
            required_number(memory, metric, issues, "map.memory")
        if number(memory.get("afterTextures")) and number(memory.get("baselineTextures")) \
                and memory["afterTextures"] > memory["baselineTextures"]:
            issues.append("map texture count did not return to baseline")
        if number(memory.get("afterUObjects")) and number(memory.get("baselineUObjects")) \
                and memory["afterUObjects"] > memory["baselineUObjects"]:
            issues.append("map UObject count did not return to baseline")
    tiles = receipt.get("tiles")
    if not isinstance(tiles, dict):
        issues.append("missing map.tiles")
    else:
        for metric in ("peakResident", "hits", "misses", "evictions", "uploads"):
            required_number(tiles, metric, issues, "map.tiles")
    for metric in ("gameThreadMs", "renderThreadMs", "gpuMs", "peakCpuBytes", "peakTextureBytes"):
        if not number(receipt.get(metric)):
            issues.append(f"missing map.{metric}")
    # The native receipt has no viewport field yet. Keep the requested size in
    # the summary, but require native corroboration before qualifying it.
    if receipt.get("resolution") != resolution:
        issues.append("map receipt does not corroborate requested resolution")


def check_render(receipt, issues):
    if receipt.get("passed") is not True or receipt.get("error"):
        issues.append("synthetic TerrainCore render failed")
    first_visible = required_number(receipt, "firstVisibleTerrainSeconds", issues, "render")
    if first_visible == 0:
        issues.append("render did not measure elapsed visibility")
    tiles = required_number(receipt, "renderedTiles", issues, "render")
    if tiles == 0:
        issues.append("render published no TerrainCore tiles")
    digest = receipt.get("screenshotSha256")
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
        issues.append("missing or invalid render screenshot digest")
    screenshot = receipt.get("screenshotPath")
    if not isinstance(screenshot, str) or not Path(screenshot).is_file():
        issues.append("missing render screenshot")


def run_case(launcher, output, name, flags, scenario, timeout, check):
    token = str(uuid.uuid4())
    receipt_path = output / f"{name}.receipt.json"
    log_path = output / f"{name}.log.txt"
    command = [str(launcher), "-unattended", "-nop4", "-nosplash", *flags,
               f"-SkiP1Token={token}"]
    issues = []
    try:
        with log_path.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=launcher.parent, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout, check=False,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        if result.returncode != 0:
            issues.append(f"packaged process exited {result.returncode}")
    except subprocess.TimeoutExpired:
        issues.append(f"packaged process exceeded {timeout}s timeout")
    except OSError as error:
        issues.append(f"could not launch packaged process: {error}")
    receipt = read_receipt(receipt_path, token, scenario, issues)
    if receipt is not None:
        check(receipt, issues)
    failed = any(not issue.startswith(("missing ", "map receipt does not corroborate"))
                 for issue in issues)
    status = "FAIL" if failed else "INCOMPLETE" if issues else "PASS"
    return {"case": name, "status": status,
            "issues": issues, "receipt": str(receipt_path), "log": str(log_path),
            "command": command}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="output directory; defaults to a new test-results/m0 run")
    parser.add_argument("--cases", choices=("all", "scale", "map"), default="all")
    args = parser.parse_args()
    launcher = args.launcher.resolve()
    if not launcher.is_file():
        parser.error(f"packaged launcher does not exist: {launcher}")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    output = (args.output or ROOT / "test-results" / "m0" / run_id).resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for side in ((513, 10001) if args.cases in ("all", "scale") else ()):
        name = f"terrain-{side}"
        receipt = output / f"{name}.receipt.json"
        flags = ["-RenderOffScreen", "-NullRHI", "-SkiM0TerrainCoreScale",
                 f"-SkiM0Side={side}", f"-SkiP1Receipt={receipt}"]
        if side == 10001:
            flags.append("-SkiM0KeepPackage")
        results.append(run_case(launcher, output, name, flags,
            "m0-terraincore-scale", 1200 if side == 10001 else 180,
            lambda data, issues, side=side: check_scale(data, side, issues)))
        if side == 10001 and receipt.is_file():
            try:
                scale = json.loads(receipt.read_text(encoding="utf-8-sig"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                scale = {}
            package_root = scale.get("packageRoot")
            content_id = scale.get("contentId")
            if scale.get("passed") is True and isinstance(package_root, str) and package_root \
                    and isinstance(content_id, str) and content_id:
                render_receipt = output / "terrain-render.receipt.json"
                results.append(run_case(launcher, output, "terrain-render",
                    ["-RenderOffScreen", "-Windowed", "-ForceRes", "-ResX=1280", "-ResY=720",
                     "-SkiM0TerrainCoreRender", f"-SkiM0RenderRoot={package_root}",
                     f"-SkiM0ContentId={content_id}", f"-SkiP1Receipt={render_receipt}"],
                    "m0-terraincore-render", 120, check_render))
            else:
                results.append({"case": "terrain-render", "status": "FAIL",
                                "issues": ["scale receipt has no retained package for render"]})
    for width, height in (((1920, 1080), (2560, 1440)) if args.cases in ("all", "map") else ()):
        resolution = f"{width}x{height}"
        name = f"map-{resolution}"
        receipt = output / f"{name}.receipt.json"
        results.append(run_case(launcher, output, name,
            ["-SkiM0MapSpike", "-RenderOffScreen", "-Windowed", "-ForceRes",
             f"-ResX={width}", f"-ResY={height}",
             f"-SkiM0MapReceipt={receipt}"], "M0-A-native-map", 390,
            lambda data, issues, resolution=resolution: check_map(data, resolution, issues)))
    status = ("FAIL" if any(case["status"] == "FAIL" for case in results)
              else "INCOMPLETE" if any(case["status"] == "INCOMPLETE" for case in results)
              else "PASS")
    summary = {"gate": "M0-A/F", "status": status, "launcher": str(launcher),
               "selectedCases": args.cases, "cases": results}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": status, "summary": str(output / "summary.json")}, indent=2))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
