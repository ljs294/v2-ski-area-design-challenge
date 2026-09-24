"""Development packaged M0-D redirect-chain negative control.

Run: python Tools/Build/m0_gateway.py --launcher PATH_TO_PACKAGED_EXE
The packaged Bootstrap scenario `gateway-redirect` must accept SkiGatewayTestUrl and
SkiGatewayTestCA, then write requestStatus/httpStatus to SkiP1Receipt.
The `gateway-range` scenario must request bytes 4-7 and write bytesHex/httpStatus.
The `gateway-cog` scenario runs the fixture through the range-backed COG reader and
writes its FM0RasterProjectionReceipt JSON to SkiP1Receipt.
Stable COG ranges pin ETag "v1". A second fixture changes after the first range;
the packaged COG probe must reject the 412 without reaching the forbidden listener.
"""

import argparse
import http.server
import json
from pathlib import Path
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import re


OPENSSL = Path(r"C:\Program Files\Git\mingw64\bin\openssl.exe")
CASES = (
    "absolute", "relative", "http-to-https", "https-to-http",
    "alternate-port", "userinfo", "encoded-traversal", "odd-location", "loop",
)
GAME_FLAGS = ("-unattended", "-nop4", "-nosplash", "-RenderOffScreen", "-NullRHI")
NO_WINDOW = {"creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0)}


class ForbiddenListener:
    def __init__(self):
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen()
        self.socket.settimeout(0.2)
        self.port = self.socket.getsockname()[1]
        self.count = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.count += 1
            conn.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.socket.close()
        self.thread.join(timeout=2)


def make_handler(forbidden_port, cog_bytes):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path in ("/fixture/s1m-like.tif", "/fixture/mutating-s1m-like.tif"):
                match = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
                if match is None:
                    self.send_error(400, "Exact single byte range required")
                    return
                start, end = map(int, match.groups())
                if start > end or end >= len(cog_bytes):
                    self.send_error(416, "Range outside fixture")
                    return
                mutating = self.path.endswith("mutating-s1m-like.tif")
                with self.server.gateway_lock:
                    prior = self.server.mutating_range_requests if mutating else self.server.stable_range_requests
                    if mutating:
                        self.server.mutating_range_requests += 1
                    else:
                        self.server.stable_range_requests += 1
                expected_match = None if prior == 0 else '"v1"'
                if self.headers.get("If-Match") != expected_match:
                    self.send_error(400, "Missing or incorrect pinned If-Match")
                    return
                if mutating and prior > 0:
                    self.send_response(412)
                    self.send_header("ETag", '"v2"')
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                payload = cog_bytes[start:end + 1]
                self.server.gateway_range_requests += 1
                self.send_response(206)
                self.send_header("Content-Type", "image/tiff")
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(cog_bytes)}")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("ETag", '"v1"')
                self.end_headers()
                self.wfile.write(payload)
                return
            if self.path in ("/range/cog-header", "/range/bad-content-range"):
                body = bytes.fromhex("49492a0008000000")
                if self.headers.get("Range") != "bytes=4-7":
                    self.send_error(400)
                    return
                self.send_response(206)
                self.send_header("Content-Type", "image/tiff")
                self.send_header("Content-Range", "bytes 3-6/8" if self.path.endswith("bad-content-range") else "bytes 4-7/8")
                self.send_header("Content-Length", "4")
                self.end_headers()
                self.wfile.write(body[4:8])
                return
            kind = self.path.rsplit("/", 1)[-1]
            host = f"localhost:{self.server.server_port}"
            forbidden = f"localhost:{forbidden_port}"
            locations = {
                "absolute": f"https://{forbidden}/forbidden",
                "relative": f"//{forbidden}/forbidden",
                "http-to-https": f"http://{forbidden}/forbidden",
                "https-to-http": f"https://{forbidden}/forbidden",
                "alternate-port": f"https://localhost:{forbidden_port}/forbidden",
                "userinfo": f"https://user@{forbidden}/forbidden",
                "encoded-traversal": f"https://{host}/%2e%2e/forbidden",
                "odd-location": f" https://{forbidden}/forbidden ",
                "loop": self.path,
            }
            if kind not in locations:
                self.send_error(404)
                return
            self.send_response(302)
            self.send_header("Location", locations[kind])
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *_):
            pass

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, default=OPENSSL)
    fixture_root = Path(__file__).resolve().parents[2] / "Saved" / "M0RasterProjectionFixture"
    parser.add_argument("--cog", type=Path, default=fixture_root / "s1m-like.tif")
    parser.add_argument("--gpkg", type=Path, default=fixture_root / "s1m-trimmed.gpkg")
    args = parser.parse_args()
    if not args.launcher.is_file() or not args.openssl.is_file():
        parser.error("packaged launcher or OpenSSL executable missing")
    if not args.cog.is_file() or not args.gpkg.is_file():
        parser.error("M0 COG/GeoPackage fixture missing; run Ski.P1.Preparation.M0.RasterProjectionFixture first")
    results_root = Path(__file__).resolve().parents[2] / "test-results"
    results_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ski-m0-gateway-", dir=results_root) as temporary, ForbiddenListener() as forbidden:
        root = Path(temporary)
        key = root / "key.pem"
        cert = root / "cert.pem"
        subprocess.run([
            str(args.openssl), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost",
        ], check=True, capture_output=True, text=True)
        server = Server(("127.0.0.1", 0), make_handler(forbidden.port, args.cog.read_bytes()))
        server.gateway_range_requests = 0
        server.stable_range_requests = 0
        server.mutating_range_requests = 0
        server.gateway_lock = threading.Lock()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for kind in CASES:
                receipt = root / f"{kind}.json"
                url = f"https://localhost:{server.server_port}/redirect/{kind}"
                command = [
                    str(args.launcher), *GAME_FLAGS,
                    "-SkiP1Scenario=gateway-redirect",
                    f"-SkiP1Receipt={receipt}",
                    f"-SkiGatewayTestUrl={url}",
                    f"-SkiGatewayTestCA={cert}",
                ]
                subprocess.run(command, check=True, timeout=60, capture_output=True, text=True, **NO_WINDOW)
                if not receipt.is_file():
                    raise RuntimeError(f"No packaged receipt for {kind}")
                result = json.loads(receipt.read_text(encoding="utf-8-sig"))
                if result.get("requestStatus") != "REDIRECT_DENIED" or result.get("httpStatus") != 302:
                    raise RuntimeError(f"Redirect was not denied for {kind}: {result}")
                if forbidden.count != 0:
                    raise RuntimeError(f"Forbidden listener accepted {forbidden.count} connection(s) after {kind}")
            receipt = root / "range.json"
            subprocess.run([
                str(args.launcher), *GAME_FLAGS,
                "-SkiP1Scenario=gateway-range", f"-SkiP1Receipt={receipt}",
                f"-SkiGatewayTestUrl=https://localhost:{server.server_port}/range/cog-header",
                f"-SkiGatewayTestCA={cert}",
            ], check=True, timeout=60, capture_output=True, text=True, **NO_WINDOW)
            if not receipt.is_file():
                raise RuntimeError("No packaged range receipt")
            result = json.loads(receipt.read_text(encoding="utf-8-sig"))
            if result.get("httpStatus") != 206 or result.get("bytesHex") != "08000000":
                raise RuntimeError(f"Range probe failed: {result}")
            bad_receipt = root / "bad-range.json"
            subprocess.run([
                str(args.launcher), *GAME_FLAGS,
                "-SkiP1Scenario=gateway-range", f"-SkiP1Receipt={bad_receipt}",
                f"-SkiGatewayTestUrl=https://localhost:{server.server_port}/range/bad-content-range",
                f"-SkiGatewayTestCA={cert}",
            ], check=True, timeout=60, capture_output=True, text=True, **NO_WINDOW)
            bad = json.loads(bad_receipt.read_text(encoding="utf-8-sig"))
            if bad.get("requestStatus") != "InvalidContentRange":
                raise RuntimeError(f"Mismatched Content-Range was accepted: {bad}")
            cog_receipt = root / "gateway-cog.json"
            subprocess.run([
                str(args.launcher), *GAME_FLAGS,
                "-SkiP1Scenario=gateway-cog", f"-SkiP1Receipt={cog_receipt}",
                f"-SkiGatewayTestUrl=https://localhost:{server.server_port}/fixture/s1m-like.tif",
                f"-SkiGatewayTestCA={cert}", f"-SkiM0GeoPackage={args.gpkg}",
            ], check=True, timeout=60, capture_output=True, text=True, **NO_WINDOW)
            if not cog_receipt.is_file():
                raise RuntimeError("No packaged gateway COG receipt")
            cog = json.loads(cog_receipt.read_text(encoding="utf-8-sig"))
            probe = cog.get("probe", {})
            if cog.get("passed") is not True or probe.get("cogPassed") is not True \
                    or probe.get("gatewayRangeRequests", 0) <= 1:
                raise RuntimeError(f"Range-backed COG probe failed: {cog}")
            if server.gateway_range_requests <= 1:
                raise RuntimeError("HTTPS fixture saw fewer than two range requests")
            if server.stable_range_requests <= 1:
                raise RuntimeError("Stable COG did not send multiple pinned If-Match ranges")
            mutation_receipt = root / "gateway-cog-mutated.json"
            mutation_run = subprocess.run([
                str(args.launcher), *GAME_FLAGS,
                "-SkiP1Scenario=gateway-cog", f"-SkiP1Receipt={mutation_receipt}",
                f"-SkiGatewayTestUrl=https://localhost:{server.server_port}/fixture/mutating-s1m-like.tif",
                f"-SkiGatewayTestCA={cert}", f"-SkiM0GeoPackage={args.gpkg}",
            ], check=False, timeout=60, capture_output=True, text=True, **NO_WINDOW)
            if not mutation_receipt.is_file():
                raise RuntimeError(f"No packaged mutation receipt (exit {mutation_run.returncode})")
            mutated = json.loads(mutation_receipt.read_text(encoding="utf-8-sig"))
            mutation_probe = mutated.get("probe", {})
            if mutated.get("passed") is not False or mutation_probe.get("cogPassed") is not False \
                    or "GATEWAY_COG_ETAG_PRECONDITION_FAILED" not in mutation_probe.get("cogError", ""):
                raise RuntimeError(f"Changed COG object was not rejected: {mutated}")
            if server.mutating_range_requests < 2:
                raise RuntimeError("Mutation fixture did not receive an If-Match follow-up range")
            if forbidden.count != 0:
                raise RuntimeError(f"Forbidden listener accepted {forbidden.count} connection(s)")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        print(json.dumps({"gate": "M0-D", "result": "PASS", "redirectCases": len(CASES), "rangeProbe": "PASS",
                          "gatewayCogRanges": server.gateway_range_requests,
                          "stablePinnedRanges": server.stable_range_requests,
                          "mutationRequests": server.mutating_range_requests,
                          "mutationRejected": True,
                          "forbiddenAccepts": forbidden.count}))


if __name__ == "__main__":
    main()
