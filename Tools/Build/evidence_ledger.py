"""Build a fail-closed ledger for the exact P1 release evidence runs."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


INVOCATION = re.compile(r"^[0-9]{8}T[0-9]{6}\.[0-9]{6}Z-[0-9a-f]{8}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
TERRAINCORE_CTESTS = (
    "SkiDomain.CoverEcology", "SkiDomain.ElevationSources",
    "SkiDomain.PlaceCoordinates", "SkiDomain.Revision",
    "SkiDomain.SiteSelection", "SkiDomain.Terrain",
    "SkiDomain.TerrainCore", "SkiDomain.TerrainQuality")
FORBIDDEN_SHIPPING_PLUGINS = {
    "modelcontextprotocol", "editortoolset", "automationtesttoolset",
    "slateinspectortoolset", "umgtoolset", "webbrowserwidget"}


class EvidenceError(RuntimeError):
    pass


def _sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _inside(root: Path, path: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _is_link(path: Path) -> bool:
    is_junction = getattr(path, "is_junction", lambda: False)
    return path.is_symlink() or is_junction()


def _relative(repo_root: Path, path: Path) -> str:
    if not _inside(repo_root, path):
        raise EvidenceError(f"Evidence path is outside the repository: {path}")
    return path.relative_to(repo_root).as_posix()


def _manifest_digest(files: dict[str, str]) -> str:
    """Match p0.source_snapshot/package_manifest's canonical file-map digest."""
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def _validated_file_map(value: object, label: str, *, allow_missing: bool) -> dict[str, str]:
    if not isinstance(value, dict) or not value:
        raise EvidenceError(f"{label} file map is missing or empty")
    result: dict[str, str] = {}
    for raw_path, raw_hash in value.items():
        if not isinstance(raw_path, str) or not raw_path or "\\" in raw_path \
                or raw_path.startswith("/") or any(
                    part in ("", ".", "..") for part in raw_path.split("/")):
            raise EvidenceError(f"{label} contains a non-canonical path")
        if not isinstance(raw_hash, str) or (
                not SHA256.fullmatch(raw_hash) and not (allow_missing and raw_hash == "MISSING")):
            raise EvidenceError(f"{label} contains an invalid file hash: {raw_path}")
        result[raw_path] = raw_hash
    return result


def _validate_source_manifest(value: object, source_digest: str, label: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise EvidenceError(f"Source manifest is malformed: {label}")
    files = _validated_file_map(value.get("files"), f"Source manifest {label}",
                                allow_missing=True)
    computed = _manifest_digest(files)
    if value.get("sha256") != computed or computed != source_digest:
        raise EvidenceError(f"Source manifest digest does not match its file map: {label}")
    return files


def _validate_exact_ctest(run: Path, receipt: dict, label: str) -> dict[str, object]:
    discovery_path = run / "terraincore-ctest-discovery.json"
    junit_path = run / "terraincore-ctest-results.xml"
    log_path = run / "terraincore-ctest-run.log"
    for path in (discovery_path, junit_path, log_path):
        if not path.is_file() or _is_link(path) or path.stat().st_size == 0:
            raise EvidenceError(f"Exact CTest evidence is missing or empty: {label}/{path.name}")
    try:
        discovery = json.loads(discovery_path.read_text(encoding="utf-8-sig"))
        configured = discovery["tests"]
        names = [entry["name"] for entry in configured]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise EvidenceError(f"CTest discovery is malformed: {label}") from error
    if sorted(names) != sorted(TERRAINCORE_CTESTS) or len(names) != len(TERRAINCORE_CTESTS):
        raise EvidenceError(f"CTest discovery identities are not exact: {label}")
    try:
        cases = list(ET.parse(junit_path).getroot().iter("testcase"))
    except (OSError, ET.ParseError) as error:
        raise EvidenceError(f"CTest JUnit is malformed: {label}") from error
    result_names = [case.attrib.get("name", "") for case in cases]
    failed = [case.attrib.get("name", "") for case in cases
              if any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))]
    if sorted(result_names) != sorted(TERRAINCORE_CTESTS) \
            or len(result_names) != len(TERRAINCORE_CTESTS) or failed:
        raise EvidenceError(f"CTest JUnit identities/results are not exact: {label}")
    result = receipt.get("result")
    native = result.get("native_receipts") if isinstance(result, dict) else None
    if receipt.get("command") != "terraincore" or not isinstance(result, dict) \
            or result.get("status") != "PASS" \
            or result.get("native_tests") != list(TERRAINCORE_CTESTS) \
            or not isinstance(native, list) or len(native) != len(TERRAINCORE_CTESTS):
        raise EvidenceError(f"CTest receipt bindings are missing: {label}")
    log = log_path.read_text(encoding="utf-8", errors="replace")
    if f"100% tests passed, 0 tests failed out of {len(TERRAINCORE_CTESTS)}" not in log \
            or any(name not in log for name in TERRAINCORE_CTESTS):
        raise EvidenceError(f"CTest run log does not prove the exact passing set: {label}")
    bound = {item.get("name"): item for item in native if isinstance(item, dict)}
    configured_by_name = {entry["name"]: entry for entry in configured}
    executable_hashes: dict[str, str] = {}
    for name in TERRAINCORE_CTESTS:
        item = bound.get(name)
        command = configured_by_name[name].get("command", [])
        if not item or item.get("result") != "PASS" or not isinstance(command, list) \
                or not command or not isinstance(command[0], str):
            raise EvidenceError(f"CTest identity is not bound to PASS: {label}/{name}")
        executable = Path(command[0]).resolve(strict=True)
        if not executable.is_file() or _is_link(executable) \
                or Path(item.get("executable", "")).resolve(strict=True) != executable:
            raise EvidenceError(f"CTest executable binding is invalid: {label}/{name}")
        actual_hash = _sha(executable)
        if item.get("executable_sha256") != actual_hash:
            raise EvidenceError(f"CTest executable hash binding is stale: {label}/{name}")
        executable_hashes[name] = actual_hash
    return {"tests": list(TERRAINCORE_CTESTS), "executables": executable_hashes,
            "discoverySha256": _sha(discovery_path), "junitSha256": _sha(junit_path),
            "runLogSha256": _sha(log_path)}


def _validate_shipping_package(receipt: dict, package_invocation: str,
                               label: str) -> dict[str, object]:
    result = receipt.get("result")
    if receipt.get("command") != "package" or receipt.get("configuration") != "Shipping" \
            or receipt.get("invocation") != package_invocation or not isinstance(result, dict) \
            or result.get("status") != "PASS":
        raise EvidenceError(f"Shipping package receipt is not exact: {label}")
    manifest = result.get("manifest")
    if not isinstance(manifest, dict):
        raise EvidenceError(f"Shipping package manifest is missing: {label}")
    files = _validated_file_map(manifest.get("files"), "Shipping package manifest",
                                allow_missing=False)
    manifest_digest = _manifest_digest(files)
    if manifest.get("sha256") != manifest_digest:
        raise EvidenceError(f"Shipping package manifest digest is fabricated: {label}")
    package_root = Path(result.get("directory", "")).resolve(strict=True)
    actual: dict[str, str] = {}
    for path in sorted(package_root.rglob("*")):
        if _is_link(path):
            raise EvidenceError(f"Shipping package contains a symlink/reparse point: {path}")
        if path.is_file():
            actual[path.relative_to(package_root).as_posix()] = _sha(path)
    if actual != files:
        raise EvidenceError(f"Shipping package files differ from their manifest: {label}")
    proof = result.get("shipping_mcp_proof")
    if not isinstance(proof, dict) or proof.get("status") != "PASS":
        raise EvidenceError(f"Shipping target proof is missing: {label}")
    bindings = {}
    for path_key, hash_key in (("target_receipt", "target_receipt_sha256"),):
        target = Path(proof.get(path_key, "")).resolve(strict=True)
        if not target.is_file() or _is_link(target) or proof.get(hash_key) != _sha(target):
            raise EvidenceError(f"Shipping target receipt hash is stale: {label}")
        bindings[hash_key] = proof[hash_key]
        target_receipt = target
    try:
        target_value = json.loads(target_receipt.read_text(encoding="utf-8-sig"))
        plugins = target_value["BuildPlugins"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise EvidenceError(f"Shipping target receipt is malformed: {label}") from error
    if not isinstance(target_value, dict) \
            or target_value.get("TargetName") != "SkiAreaDesignChallenge" \
            or target_value.get("Platform") != "Win64" \
            or target_value.get("Configuration") != "Shipping" \
            or target_value.get("TargetType") != "Game" \
            or target_value.get("IsTestTarget") is not False \
            or target_value.get("Project") != "../../SkiAreaDesignChallenge.uproject" \
            or target_value.get("Launch") != \
                "$(ProjectDir)/Binaries/Win64/SkiAreaDesignChallenge-Win64-Shipping.exe" \
            or not isinstance(plugins, list) \
            or any(not isinstance(plugin, str) or not plugin for plugin in plugins):
        raise EvidenceError(f"Shipping target receipt identity is not exact: {label}")
    forbidden_present = sorted(
        plugin for plugin in plugins if plugin.lower() in FORBIDDEN_SHIPPING_PLUGINS)
    if forbidden_present:
        raise EvidenceError(
            f"Forbidden MCP/toolset plugins entered the Shipping target: {forbidden_present}")
    # Receipts normally point into the repository, not the package. Resolve the rules from
    # the target receipt's repository root so the recorded rules digest is independently bound.
    target_receipt = Path(proof["target_receipt"]).resolve(strict=True)
    candidates = [parent / "Source/SkiAreaDesignChallenge.Target.cs"
                  for parent in target_receipt.parents]
    rules = next((candidate for candidate in candidates if candidate.is_file()), None)
    if rules is None or _is_link(rules) or proof.get("target_rules_sha256") != _sha(rules):
        raise EvidenceError(f"Shipping target-rules hash is stale: {label}")
    rules_text = rules.read_text(encoding="utf-8-sig")
    root_modules = re.findall(r'ExtraModuleNames\.Add\(\s*"([^"]+)"\s*\)', rules_text)
    root_mutators = re.findall(r'ExtraModuleNames\s*\.\s*(Add|AddRange)\s*\(', rules_text)
    if root_modules != ["SkiPresentation"] or root_mutators != ["Add"] \
            or len(re.findall(r'\bType\s*=\s*TargetType\.Game\s*;', rules_text)) != 1:
        raise EvidenceError(f"Shipping root module is not exactly SkiPresentation: {label}")
    if proof.get("target") != "SkiAreaDesignChallenge" \
            or proof.get("platform") != "Win64" \
            or proof.get("configuration") != "Shipping" \
            or proof.get("target_type") != "Game" \
            or proof.get("root_module") != "SkiPresentation":
        raise EvidenceError(f"Shipping proof fields disagree with parsed target evidence: {label}")
    bindings["target_rules_sha256"] = proof["target_rules_sha256"]
    bindings["package_manifest_sha256"] = manifest_digest
    bindings["target_identity"] = {
        "target": "SkiAreaDesignChallenge", "platform": "Win64",
        "configuration": "Shipping", "type": "Game", "isTestTarget": False,
        "rootModule": "SkiPresentation", "forbiddenPluginsPresent": []}
    return bindings


def collect_evidence(
    repo_root: Path,
    runs_root: Path,
    source_digest: str,
    package_invocation: str,
    receipt_specs: list[tuple[str, Path]],
    required_artifacts: dict[str, tuple[str, ...]] | None = None,
) -> dict:
    """Validate receipts and hash every regular file retained by their exact runs."""
    repo_root = repo_root.resolve(strict=True)
    runs_root = runs_root.resolve(strict=True)
    if not _inside(repo_root, runs_root):
        raise EvidenceError("P1 runs root is outside the repository")
    if not re.fullmatch(r"[0-9a-f]{64}", source_digest):
        raise EvidenceError("Evidence source digest is malformed")
    if not INVOCATION.fullmatch(package_invocation):
        raise EvidenceError("Package invocation is malformed")
    if not receipt_specs:
        raise EvidenceError("No release evidence receipts were supplied")
    required_artifacts = required_artifacts or {}
    files: dict[str, dict[str, int | str]] = {}
    receipts: dict[str, dict[str, object]] = {}
    bindings: dict[str, object] = {}
    seen_runs: set[Path] = set()
    canonical_source_files: dict[str, str] | None = None

    for label, supplied_receipt_path in receipt_specs:
        if label in receipts:
            raise EvidenceError(f"Duplicate evidence label: {label}")
        if _is_link(supplied_receipt_path):
            raise EvidenceError(f"Evidence receipt is not a regular file: {label}")
        receipt_path = supplied_receipt_path.resolve(strict=True)
        if not receipt_path.is_file():
            raise EvidenceError(f"Evidence receipt is not a regular file: {label}")
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
            invocation = receipt["invocation"]
            recorded_run_output = Path(receipt["run_output"])
            receipt_source = receipt["source_before"]
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise EvidenceError(f"Evidence receipt is malformed: {label}") from error
        if not isinstance(invocation, str) or not INVOCATION.fullmatch(invocation):
            raise EvidenceError(f"Evidence invocation is malformed: {label}")
        run_candidate = runs_root / invocation
        if _is_link(run_candidate):
            raise EvidenceError(f"Evidence run is not a regular directory: {label}")
        expected_run = run_candidate.resolve(strict=True)
        if expected_run.parent != runs_root or expected_run in seen_runs:
            raise EvidenceError(f"Evidence invocation is duplicated or not a direct run: {label}")
        seen_runs.add(expected_run)
        if not expected_run.is_dir():
            raise EvidenceError(f"Evidence run is not a regular directory: {label}")
        if not recorded_run_output.is_absolute():
            raise EvidenceError(f"Evidence run output is not absolute: {label}")
        try:
            recorded_run = recorded_run_output.resolve(strict=True)
        except OSError as error:
            raise EvidenceError(f"Evidence run output is missing: {label}") from error
        if recorded_run != expected_run:
            raise EvidenceError(f"Evidence run output does not match its invocation: {label}")
        if receipt_source != source_digest:
            raise EvidenceError(f"Evidence receipt belongs to another source: {label}")

        report_path = expected_run / "report.json"
        source_manifest_path = expected_run / "source-manifest.json"
        for required in (report_path, source_manifest_path):
            if not required.is_file() or _is_link(required):
                raise EvidenceError(f"Required run evidence is missing: {label}/{required.name}")
        if receipt_path.read_bytes() != report_path.read_bytes():
            raise EvidenceError(f"Evidence receipt is not the exact retained run report: {label}")
        try:
            source_manifest = json.loads(source_manifest_path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as error:
            raise EvidenceError(f"Source manifest is malformed: {label}") from error
        source_files = _validate_source_manifest(source_manifest, source_digest, label)
        if canonical_source_files is None:
            canonical_source_files = source_files
        elif source_files != canonical_source_files:
            raise EvidenceError(f"Source manifest file map differs between runs: {label}")
        if label == "editor-terraincore":
            bindings["terraincoreCTest"] = _validate_exact_ctest(expected_run, receipt, label)
        if label == "shipping-package":
            bindings["shippingPackage"] = _validate_shipping_package(
                receipt, package_invocation, label)
        elif label.startswith("shipping-"):
            result = receipt.get("result")
            if not isinstance(result, dict) or result.get("package_invocation") != package_invocation:
                raise EvidenceError(f"Shipping evidence is not bound to the package: {label}")
        for relative in required_artifacts.get(label, ()):
            candidate = expected_run / relative
            if (candidate.parent != expected_run or not candidate.is_file()
                    or _is_link(candidate)):
                raise EvidenceError(f"Required run artifact is missing: {label}/{relative}")

        receipt_relative = _relative(repo_root, receipt_path)
        files[receipt_relative] = {
            "bytes": receipt_path.stat().st_size, "sha256": _sha(receipt_path)}
        run_files: list[str] = []
        for path in sorted(expected_run.rglob("*")):
            if _is_link(path):
                raise EvidenceError(f"Symlink/reparse evidence is not allowed: {path}")
            if not path.is_file():
                continue
            resolved = path.resolve(strict=True)
            if not _inside(expected_run, resolved):
                raise EvidenceError(f"Run evidence escaped its invocation directory: {path}")
            relative = _relative(repo_root, resolved)
            if relative in files:
                raise EvidenceError(f"Evidence file is retained by multiple runs: {relative}")
            files[relative] = {
                "bytes": resolved.stat().st_size, "sha256": _sha(resolved)}
            run_files.append(relative)
        receipts[label] = {
            "invocation": invocation,
            "receipt": receipt_relative,
            "runOutput": _relative(repo_root, expected_run),
            "files": run_files,
        }

    unknown_requirements = set(required_artifacts) - set(receipts)
    if unknown_requirements:
        raise EvidenceError(
            f"Required artifacts name unknown receipts: {sorted(unknown_requirements)}")

    return {
        "schemaVersion": 2,
        "sourceDigest": source_digest,
        "packageInvocation": package_invocation,
        "bindings": bindings,
        "receipts": receipts,
        "files": dict(sorted(files.items())),
    }


def _parse_pair(value: str, option: str) -> tuple[str, str]:
    if "=" not in value:
        raise EvidenceError(f"{option} must use LABEL=VALUE")
    label, item = value.split("=", 1)
    if not label or not item:
        raise EvidenceError(f"{option} must use non-empty LABEL=VALUE")
    return label, item


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--runs-root", required=True, type=Path)
    parser.add_argument("--source-digest", required=True)
    parser.add_argument("--package-invocation", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--receipt", action="append", default=[])
    parser.add_argument("--require", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        receipts = [(label, Path(path)) for label, path in
                    (_parse_pair(value, "--receipt") for value in args.receipt)]
        required: dict[str, list[str]] = {}
        for label, relative in (_parse_pair(value, "--require") for value in args.require):
            required.setdefault(label, []).append(relative)
        ledger = collect_evidence(
            args.repo_root, args.runs_root, args.source_digest,
            args.package_invocation, receipts,
            {label: tuple(values) for label, values in required.items()})
        args.output.write_text(
            json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (EvidenceError, OSError) as error:
        print(f"Evidence ledger failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
