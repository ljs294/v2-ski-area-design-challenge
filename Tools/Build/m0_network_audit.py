"""Process-attributed Shipping M0 network audit; runs packaged probes offscreen.

Example:
  python Tools/Build/m0_network_audit.py --launcher <Shipping exe>

With no installed IDs, this creates a fresh packaged terrain fixture and reopens it
offline. Existing installs require --offline-content-id, --offline-edit-set-id,
and --data-root together.

This reuses p1.py's GetExtendedTcpTable process/descendant sampler. The IP audit is
paired with SkiNetGateway's exact-host validation because S3 hosts can share IPs.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import sys
import time
import uuid
from urllib.parse import urlsplit

import p1


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_COG = (
    "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/"
    "n26e19/n2620e1940/S1M_n2620e1940_20260102.tif"
)
ALLOWED_HOSTS = (
    "basemap.nationalmap.gov",
    "elevation-tiles-prod.s3.amazonaws.com",
    "nominatim.openstreetmap.org",
    "tnmaccess.nationalmap.gov",
    "prd-tnm.s3.amazonaws.com",
    "esa-worldcover.s3.eu-central-1.amazonaws.com",
    "overpass-api.de",
)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package_provenance(launcher):
    """Bind evidence to exact packaged bytes and a matching P1 package receipt."""
    binary = launcher.parent / "SkiAreaDesignChallenge" / "Binaries" / "Win64" / "SkiAreaDesignChallenge-Win64-Shipping.exe"
    if not binary.is_file():
        raise RuntimeError(f"Shipping binary missing beside launcher: {binary}")
    package_report = None
    candidates = [ROOT / "test-results" / "p1" / "package-Shipping.json"]
    candidates += sorted((ROOT / "test-results" / "p1" / "runs").glob("*/report.json"),
                         reverse=True)
    for path in candidates:
        if not path.is_file():
            continue
        try:
            report = json.loads(path.read_text(encoding="utf-8-sig"))
            recorded = report.get("result", {}).get("launcher")
            if report.get("command") == "package" and recorded \
                    and Path(recorded).resolve() == launcher:
                package_report = {"path": str(path.resolve()), "sha256": sha256_file(path),
                                  "invocation": report.get("invocation"),
                                  "sourceDigestAtPackage": report.get("source_before"),
                                  "recordedLauncherSha256": report.get("result", {}).get("launcher_sha256")}
                break
        except (OSError, ValueError, TypeError):
            continue
    return {"launcher": str(launcher), "launcherSha256": sha256_file(launcher),
            "shippingBinary": str(binary.resolve()), "shippingBinarySha256": sha256_file(binary),
            "packageReport": package_report,
            "packageReportStatus": "matched" if package_report else "no matching package report found"}


def resolve_requested_host(host):
    addresses = set()
    for family, _, _, _, sockaddr in socket.getaddrinfo(host, 443, proto=socket.IPPROTO_TCP):
        if family in (socket.AF_INET, socket.AF_INET6):
            addresses.add(sockaddr[0])
    if not addresses:
        raise RuntimeError(f"Requested-host DNS yielded no addresses: {host}")
    return sorted(addresses)


def remote_address(endpoint):
    host = endpoint.rsplit(":", 1)[0]
    return host[1:-1] if host.startswith("[") else host


def is_loopback(endpoint):
    return remote_address(endpoint) in ("127.0.0.1", "::1")


def classify_loopback(samples, audit):
    """Pair game socket rows to game/CEF peers by both TCP endpoints."""
    rows = {(pid, row["state"], row["local"], row["remote"])
            for _, pid, row in samples if is_loopback(row["remote"])
            and not row["remote"].endswith(":0")}
    connections = []
    for pid, state, local, remote in sorted(rows):
        image = audit["process_images"].get(str(pid), "unknown")
        if "SkiAreaDesignChallenge-Win64-Shipping" not in image:
            continue
        peers = [{"pid": other_pid,
                  "image": audit["process_images"].get(str(other_pid), "unknown")}
                 for other_pid, other_state, other_local, other_remote in rows
                 if other_local == remote and other_remote == local]
        connections.append({"pid": pid, "local": local, "remote": remote,
                            "peerCandidates": sorted(peers, key=lambda peer: peer["pid"]),
                            "cefPaired": bool(peers) and all("EpicWebHelper" in peer["image"]
                                                             for peer in peers),
                            "selfPaired": any(peer["pid"] == pid for peer in peers)})
    return connections


def run_audited(launcher, output, name, flags, timeout, receipt_dir=None):
    token = str(uuid.uuid4())
    receipt_path = (receipt_dir or output) / f"{token}.receipt.json"
    command = [str(launcher), *flags, f"-SkiP1Token={token}",
               f"-SkiP1Receipt={receipt_path}", "-RenderOffScreen", "-unattended",
               "-nosplash", "-nop4"]
    samples = []
    original_v4, original_v6 = p1._windows_ipv4_tcp_rows, p1._windows_ipv6_tcp_rows
    def capture(original):
        def wrapped(pid):
            rows = original(pid)
            timestamp = time.monotonic()
            samples.extend((timestamp, pid, row) for row in rows)
            return rows
        return wrapped
    p1._windows_ipv4_tcp_rows = capture(original_v4)
    p1._windows_ipv6_tcp_rows = capture(original_v6)
    try:
        audit = p1.checked_with_tcp_audit(command, timeout=timeout, log=f"{name}.log")
    finally:
        p1._windows_ipv4_tcp_rows, p1._windows_ipv6_tcp_rows = original_v4, original_v6
    audit["loopbackPairs"] = classify_loopback(samples, audit)
    (output / f"{name}.loopback-pairs.json").write_text(
        json.dumps(audit["loopbackPairs"], indent=2) + "\n", encoding="utf-8")
    if not receipt_path.is_file():
        raise RuntimeError(f"{name} exited without a receipt")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
    if receipt.get("token") != token or receipt.get("passed") is False:
        raise RuntimeError(f"{name} receipt invalid: {receipt}")
    return receipt, audit


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--cog-url", default=DEFAULT_COG)
    parser.add_argument("--offline-content-id")
    parser.add_argument("--offline-edit-set-id")
    parser.add_argument("--data-root", type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows GetExtendedTcpTable is required")
    launcher = args.launcher.resolve()
    if not launcher.is_file():
        parser.error("Shipping launcher missing")
    provenance = package_provenance(launcher)
    parsed_cog = urlsplit(args.cog_url)
    if parsed_cog.scheme != "https" or parsed_cog.hostname not in ALLOWED_HOSTS \
            or parsed_cog.port not in (None, 443) or parsed_cog.username or parsed_cog.password:
        parser.error("S1M URL must use an exact gateway allow-listed HTTPS host on port 443")
    if any((args.offline_content_id, args.offline_edit_set_id, args.data_root)) \
            and not all((args.offline_content_id, args.offline_edit_set_id, args.data_root)):
        parser.error("provide content id, edit set id, and data root together, or none to create a fresh fixture")
    if args.offline_content_id and not re.fullmatch(r"[0-9a-f]{64}", args.offline_content_id):
        parser.error("offline content id must be 64 lowercase hex digits")
    if args.offline_edit_set_id and not re.fullmatch(r"[0-9a-f]{64}", args.offline_edit_set_id):
        parser.error("offline edit set id must be 64 lowercase hex digits")
    output = ROOT / "test-results" / "m0" / f"network-audit-{uuid.uuid4()}"
    output.mkdir(parents=True)
    p1.p0.RUN_OUTPUT = output
    dns_before = resolve_requested_host(parsed_cog.hostname)
    real, real_audit = run_audited(launcher, output, "real-s1m", [
        "-SkiM0RealS1M", "-NullRHI", f"-SkiM0CogUrl={args.cog_url}",
    ], 120)
    probe = real.get("probe") or {}
    if real.get("scenario") != "m0-real-s1m" or not real.get("passed") \
            or not probe.get("baseTileDecoded") or not probe.get("overviewTileDecoded") \
            or probe.get("gatewayRangeRequests", 0) < 2:
        raise RuntimeError(f"Real S1M probe failed: {real}")
    failures = []
    try:
        p1.validate_tcp_audit(real_audit, require_no_connections=False, label="real S1M")
    except Exception as error:
        failures.append(str(error))
    dns_after = resolve_requested_host(parsed_cog.hostname)
    external = [remote for remote in real_audit["remote_endpoints"] if not is_loopback(remote)]
    loopback = [remote for remote in real_audit["remote_endpoints"] if is_loopback(remote)]
    if not external:
        failures.append("No external S1M endpoint observed; process audit is inconclusive")
    for remote in external:
        if not remote.endswith(":443"):
            failures.append(f"Shipping S1M external endpoint used a non-HTTPS port: {remote}")
    unpaired = [pair for pair in real_audit["loopbackPairs"]
                if not pair["cefPaired"] and not pair["selfPaired"]]
    if loopback and not real_audit["loopbackPairs"]:
        failures.append("Observed loopback endpoints lack socket-level peer evidence")
    if unpaired:
        failures.append(f"{len(unpaired)} game loopback sockets lacked a game/CEF reciprocal peer")
    dns_observations = [{"remote": remote, "address": remote_address(remote),
                         "seenInBefore": remote_address(remote) in dns_before,
                         "seenInAfter": remote_address(remote) in dns_after}
                        for remote in external]

    data_root = args.data_root.resolve() if args.data_root else output / "installed-data"
    data_root.mkdir(parents=True, exist_ok=True)
    content_id, edit_set_id = args.offline_content_id, args.offline_edit_set_id
    if content_id is None:
        imported, import_audit = run_audited(launcher, output, "fixture-install", [
            "-SkiP1Smoke", "-SkiP1Scenario=import",
            f"-SkiP1DataRoot={data_root}", f"-UserDir={output / 'install-user'}",
            "-windowed", "-ResX=1280", "-ResY=720",
        ], 120, data_root)
        content_id, edit_set_id = imported.get("contentId"), imported.get("editSetId")
        if not all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                   for value in (content_id, edit_set_id)):
            raise RuntimeError(f"Fresh installed fixture receipt invalid: {imported}")
    offline, offline_audit = run_audited(launcher, output, "mountain-offline", [
            "-SkiP1Smoke", "-SkiP1Scenario=offline-reopen",
            f"-SkiP1DataRoot={data_root}", f"-SkiP1ContentId={content_id}",
            f"-SkiP1EditSetId={edit_set_id}",
            f"-UserDir={output / 'offline-user'}",
            "-windowed", "-ResX=1280", "-ResY=720",
        ], 120, data_root)
    if offline.get("scenario") != "offline-reopen" or not offline.get("offlineReopen") \
            or not offline.get("acquisitionPortGuardInstalled") \
            or offline.get("acquisitionTransportCalls") != 0:
        raise RuntimeError(f"Offline Mountain receipt invalid: {offline}")
    try:
        p1.validate_tcp_audit(offline_audit, require_no_connections=True,
                              label="installed Mountain offline")
    except Exception as error:
        failures.append(str(error))

    result = {"gate": "M0-D Shipping network", "result": "FAIL" if failures else "PASS",
              "packageProvenance": provenance,
              "failures": failures,
              "realS1M": {"endpointCount": len(real_audit["remote_endpoints"]),
                           "externalEndpoints": external, "loopbackEndpoints": loopback,
                           "externalPortGate": "PASS" if external and all(e.endswith(":443") for e in external) else "FAIL",
                           "loopbackIpcGate": "PASS" if not loopback or (real_audit["loopbackPairs"] and not unpaired) else "UNPROVEN",
                           "loopbackPeerClassification": "game self-pair or CEF peer; no outside listener" if not unpaired else "unresolved",
                           "attribution": "SkiNetGateway validates the exact requested host; process TCP polling verifies endpoints and ports. DNS snapshots are advisory because S3 rotates addresses.",
                           "requestedUrl": args.cog_url, "dnsObservations": dns_observations,
                           "audit": real_audit, "probe": probe},
              "mountainOffline": {"audit": offline_audit, "fixtureSource":
                                  "existing prior install" if args.offline_content_id else "fresh packaged fixture"}}
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": result["result"], "output": str(output),
                      "realExternalEndpoints": external, "realLoopbackEndpoints": loopback,
                      "loopbackCefPaired": sum(pair["cefPaired"] for pair in real_audit["loopbackPairs"]),
                      "loopbackGameSelfPaired": sum(pair["selfPaired"] for pair in real_audit["loopbackPairs"]),
                      "mountainZeroEndpoints": not offline_audit["remote_endpoints"]}))
    if failures:
        raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"M0 network audit failed: {error}", file=sys.stderr)
        raise SystemExit(1)
