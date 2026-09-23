"""Isolated packaged CEF phase comparison; not a selector acceptance gate."""

from __future__ import annotations

import json
import uuid
from pathlib import Path

import p1


def main() -> None:
    report = json.loads((p1.OUTPUT / "package-Shipping.json").read_text(encoding="utf-8"))
    launcher = Path(report["result"]["launcher"]).resolve()
    if p1.p0.sha(launcher) != report["result"]["launcher_sha256"]:
        raise p1.p0.Failed("Recorded Shipping launcher changed")
    run_dir = (p1.OUTPUT / "selector-diagnostic" / str(uuid.uuid4())).resolve()
    run_dir.mkdir(parents=True)
    p1.p0.RUN_OUTPUT = run_dir
    rows = []
    for phase in ("blank", "no-tiles", "full"):
        user_dir = run_dir / f"{phase}-user"
        user_dir.mkdir()
        command = [str(launcher), f"-SkiP1SelectorDiagnostic={phase}",
                   f"-UserDir={user_dir}", "-windowed", "-ResX=1280", "-ResY=720",
                   "-unattended", "-nosplash"]
        audit = p1.checked_with_tcp_audit(command, timeout=30, log=f"{phase}.log")
        rows.append({"phase": phase, "endpoint_owners": audit["endpoint_owners"],
                     "cef_contacted_hosts": p1.cef_contacted_hosts(user_dir),
                     "audit": str(run_dir / f"{phase}.tcp-audit.json")})
    print(json.dumps({"run_dir": str(run_dir), "diagnostic_only": True,
                      "phases": rows}, indent=2))


if __name__ == "__main__":
    main()
