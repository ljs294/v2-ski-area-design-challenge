"""Verify pinned P0 extraction anchors without acquiring data or changing reference code."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]


def main():
    manifest = json.loads((ROOT / "docs/UnrealRebuild/preparation-sources.json").read_text(encoding="utf-8"))
    failures = []
    for relative, expected in manifest["files"].items():
        path = (ROOT / relative).resolve()
        if not path.is_relative_to(ROOT) or not path.is_file():
            failures.append(relative + ": missing or outside repository")
        elif hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            failures.append(relative + ": source drift; review and recapture deliberately")
    print(json.dumps({"status": "FAIL" if failures else "PASS", "anchors": len(manifest["files"]),
                      "failures": failures, "kind": "source_inventory_only", "provider_tests": "NOT_EXECUTED"}, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
