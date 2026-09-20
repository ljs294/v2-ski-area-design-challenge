"""Ownership guard for P1-generated Unreal assets; P0 receipts remain immutable."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ASSETS = {
    "Content/P1Generated/P1Terrain.umap": "World",
    "Content/P1Generated/M_Terrain_ClearMidday.uasset": "Material",
    "Content/P1Generated/M_Terrain_LowAngle.uasset": "Material",
    "Content/P1Generated/M_Terrain_Overcast.uasset": "Material",
    "Content/P1Generated/M_Overlay.uasset": "Material",
}
RECEIPT = "Content/P1Generated/ownership.json"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recipe_digest(root: Path) -> str:
    inputs = [root / "Tools/Editor/create_p1_assets.py", root / "Tools/Editor/p1_asset_ownership.py"]
    return hashlib.sha256("\n".join(digest(path) for path in inputs).encode()).hexdigest()


def preflight(root: Path, recipe_hash: str) -> dict:
    path = root / RECEIPT
    receipt = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"recipe_sha256": recipe_hash, "assets": {}}
    if not isinstance(receipt, dict) or not isinstance(receipt.get("assets"), dict):
        raise RuntimeError("Malformed P1 asset ownership receipt")
    if receipt.get("recipe_sha256") != recipe_hash:
        raise RuntimeError("P1 recipe changed; review regeneration as a separate section")
    if set(receipt["assets"]) - set(ASSETS):
        raise RuntimeError("P1 receipt claims unowned assets")
    for relative, class_name in ASSETS.items():
        file = root / relative
        prior = receipt["assets"].get(relative)
        if file.is_file() and (not prior or prior.get("sha256") != digest(file) or prior.get("class") != class_name):
            raise RuntimeError(f"Refusing to overwrite unowned or modified P1 asset: {relative}")
        if prior and not file.is_file():
            raise RuntimeError(f"Owned P1 asset disappeared: {relative}")
    return receipt


def record_asset(root: Path, receipt: dict, relative: str, properties: dict) -> None:
    if relative not in ASSETS:
        raise RuntimeError("Cannot record an unowned P1 asset")
    receipt["assets"][relative] = {"class": ASSETS[relative], "sha256": digest(root / relative), "properties": properties}
    path = root / RECEIPT
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
