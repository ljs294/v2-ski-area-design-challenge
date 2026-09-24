import ast
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Editor"))
import p1_asset_ownership


class P1PhotoMaterialContractTests(unittest.TestCase):
    def test_recipe_is_unlit_vertex_color_to_emissive_without_tint_graph(self):
        recipe_path = ROOT / "Tools/Editor/create_p1_assets.py"
        tree = ast.parse(recipe_path.read_text(encoding="utf-8"))
        photo_recipe = next(
            node for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "photo_material")
        attributes = {
            node.attr for node in ast.walk(photo_recipe) if isinstance(node, ast.Attribute)
        }
        calls = {
            node.func.attr for node in ast.walk(photo_recipe)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
        }
        self.assertIn("MSM_UNLIT", attributes)
        self.assertIn("MaterialExpressionVertexColor", attributes)
        self.assertIn("MP_EMISSIVE_COLOR", attributes)
        self.assertIn("connect_material_property", calls)
        self.assertNotIn("MaterialExpressionMultiply", attributes)
        self.assertNotIn("MP_BASE_COLOR", attributes)

    def test_photo_material_has_owned_cooked_receipt_and_current_digest(self):
        relative = "Content/P1Generated/M_Photo.uasset"
        self.assertEqual("Material", p1_asset_ownership.ASSETS[relative])
        receipt = p1_asset_ownership.preflight(
            ROOT, p1_asset_ownership.recipe_digest(ROOT))
        self.assertIn(relative, receipt["assets"])
        entry = receipt["assets"][relative]
        self.assertEqual("Material", entry["class"])
        self.assertEqual({
            "role": "photo imagery vertex-color passthrough",
            "shading_model": "Unlit",
            "vertex_color_passthrough": True,
            "surface_tint": False,
        }, entry["properties"])

        game_config = (ROOT / "Config/DefaultGame.ini").read_text(encoding="utf-8")
        self.assertIn('+DirectoriesToAlwaysCook=(Path="/Game/P1Generated")', game_config)


if __name__ == "__main__":
    unittest.main()
