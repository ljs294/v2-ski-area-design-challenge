"""File-only ownership guards. No Unreal imports, deletion or overwriting of unknown assets."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ASSETS = {
    "Content/P0Generated/Bootstrap.umap": "World",
    "Content/P0Generated/M_Bootstrap.uasset": "Material",
    "Content/P0Generated/WBP_Bootstrap.uasset": "WidgetBlueprint",
}
RECEIPT = "Content/P0Generated/ownership.json"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recipe_digest(root: Path) -> str:
    inputs = [root / "Tools/Editor/create_bootstrap_assets.py", root / "Tools/Editor/asset_ownership.py"]
    return hashlib.sha256("\n".join(digest(path) for path in inputs).encode()).hexdigest()


def preflight(root: Path, recipe_hash: str) -> dict:
    path = root / RECEIPT
    receipt = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"recipe_sha256": recipe_hash, "assets": {}}
    if not isinstance(receipt, dict) or not isinstance(receipt.get("assets"), dict):
        raise RuntimeError("Malformed asset ownership receipt")
    if receipt.get("recipe_sha256") != recipe_hash:
        raise RuntimeError("Recipe changed; review asset regeneration as a separate task")
    if set(receipt.get("assets", {})) - set(ASSETS):
        raise RuntimeError("Receipt claims unowned assets")
    for relative, class_name in ASSETS.items():
        file = root / relative
        prior = receipt["assets"].get(relative)
        if file.is_file() and (not prior or prior.get("sha256") != digest(file) or prior.get("class") != class_name):
            raise RuntimeError(f"Refusing to overwrite unowned or modified asset: {relative}")
        if prior and not file.is_file():
            raise RuntimeError(f"Owned asset disappeared: {relative}")
    return receipt


def record_asset(root: Path, receipt: dict, relative: str, properties: dict) -> None:
    if relative not in ASSETS:
        raise RuntimeError("Cannot record an unowned asset")
    receipt["assets"][relative] = {"class": ASSETS[relative], "sha256": digest(root / relative), "properties": properties}
    path = root / RECEIPT
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
