"""Repeatable editor recipe for separately owned P1 map and terrain presentation assets."""
from pathlib import Path
import sys
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
sys.path.insert(0, str(ROOT / "Tools/Editor"))
from p1_asset_ownership import ASSETS, preflight, recipe_digest, record_asset


def material(tools, receipt, name, low, high, roughness, role):
    relative = f"Content/P1Generated/{name}.uasset"
    if relative in receipt["assets"]:
        return
    asset = tools.create_asset(name, "/Game/P1Generated", unreal.Material, unreal.MaterialFactoryNew())
    if not asset:
        raise RuntimeError(f"Could not create {name}")
    low_node = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant3Vector, -500, -100)
    low_node.set_editor_property("constant", unreal.LinearColor(*low, 1.0))
    high_node = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant3Vector, -500, 100)
    high_node.set_editor_property("constant", unreal.LinearColor(*high, 1.0))
    normal = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionVertexNormalWS, -500, 300)
    mask = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionComponentMask, -300, 300)
    mask.set_editor_property("b", True)
    inverse = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionOneMinus, -100, 300)
    world_position = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionWorldPosition, -500, 500)
    altitude_mask = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionComponentMask, -300, 500)
    altitude_mask.set_editor_property("b", True)
    altitude_scale = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant, -300, 650)
    altitude_scale.set_editor_property("r", 0.00001)
    altitude_multiply = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionMultiply, -100, 500)
    altitude_bias = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant, -100, 650)
    altitude_bias.set_editor_property("r", 0.5)
    altitude_add = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionAdd, 100, 500)
    altitude = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionSaturate, 300, 500)
    terrain_factor_add = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionAdd, 100, 300)
    factor_scale = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant, 100, 650)
    factor_scale.set_editor_property("r", 0.5)
    terrain_factor_multiply = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionMultiply, 300, 300)
    terrain_factor = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionSaturate, 500, 300)
    blend = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionLinearInterpolate, 100, 0)
    vertex_color = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionVertexColor, 300, -150)
    cover_modulate = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionMultiply, 500, 0)
    rough = unreal.MaterialEditingLibrary.create_material_expression(asset, unreal.MaterialExpressionConstant, 100, 300)
    rough.set_editor_property("r", roughness)
    unreal.MaterialEditingLibrary.connect_material_expressions(normal, "", mask, "")
    unreal.MaterialEditingLibrary.connect_material_expressions(mask, "", inverse, "")
    unreal.MaterialEditingLibrary.connect_material_expressions(world_position, "", altitude_mask, "")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude_mask, "", altitude_multiply, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude_scale, "", altitude_multiply, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude_multiply, "", altitude_add, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude_bias, "", altitude_add, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude_add, "", altitude, "")
    unreal.MaterialEditingLibrary.connect_material_expressions(inverse, "", terrain_factor_add, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(altitude, "", terrain_factor_add, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(terrain_factor_add, "", terrain_factor_multiply, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(factor_scale, "", terrain_factor_multiply, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(terrain_factor_multiply, "", terrain_factor, "")
    unreal.MaterialEditingLibrary.connect_material_expressions(low_node, "", blend, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(high_node, "", blend, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(terrain_factor, "", blend, "Alpha")
    unreal.MaterialEditingLibrary.connect_material_expressions(blend, "", cover_modulate, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(vertex_color, "", cover_modulate, "B")
    unreal.MaterialEditingLibrary.connect_material_property(cover_modulate, "", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(asset)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError(f"Could not save {name}")
    record_asset(ROOT, receipt, relative, {"role": role, "slope_aware": True,
        "altitude_aware": True, "cover_aware": True, "topology_profile_independent": True})


def photo_material(tools, receipt):
    relative = "Content/P1Generated/M_Photo.uasset"
    if relative in receipt["assets"]:
        return
    asset = tools.create_asset("M_Photo", "/Game/P1Generated", unreal.Material,
                               unreal.MaterialFactoryNew())
    if not asset:
        raise RuntimeError("Could not create M_Photo")
    asset.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    vertex_color = unreal.MaterialEditingLibrary.create_material_expression(
        asset, unreal.MaterialExpressionVertexColor, 0, 0)
    unreal.MaterialEditingLibrary.connect_material_property(
        vertex_color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(asset)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError("Could not save M_Photo")
    record_asset(ROOT, receipt, relative, {
        "role": "photo imagery vertex-color passthrough",
        "shading_model": "Unlit",
        "vertex_color_passthrough": True,
        "surface_tint": False,
    })


def main():
    receipt = preflight(ROOT, recipe_digest(ROOT))
    dirty = list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
    dirty += list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
    if dirty:
        raise RuntimeError("Dirty editor packages exist; refusing P1 asset generation")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material(tools, receipt, "M_Terrain_ClearMidday", (0.13, 0.28, 0.12), (0.58, 0.55, 0.48), 0.82, "clear-midday terrain")
    material(tools, receipt, "M_Terrain_LowAngle", (0.12, 0.20, 0.11), (0.48, 0.31, 0.22), 0.88, "low-angle terrain")
    material(tools, receipt, "M_Terrain_Overcast", (0.15, 0.23, 0.18), (0.46, 0.49, 0.50), 0.94, "overcast terrain")
    material(tools, receipt, "M_Overlay", (0.90, 0.22, 0.08), (1.0, 0.74, 0.20), 0.55, "batched overlay and guest dots")
    photo_material(tools, receipt)
    map_file = "Content/P1Generated/P1Terrain.umap"
    if map_file not in receipt["assets"]:
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not levels.new_level("/Game/P1Generated/P1Terrain") or not levels.save_current_level():
            raise RuntimeError("P1 runtime map creation/save failed")
        record_asset(ROOT, receipt, map_file, {"world": "Runtime-spawned editable terrain", "beauty_scene": False,
            "game_mode": "/Script/SkiPresentation.SkiBootstrapGameMode"})
    if set(receipt["assets"]) != set(ASSETS):
        raise RuntimeError("P1 asset recipe is incomplete")
    unreal.log("SKI_P1_ASSETS_READY: six owned P1 assets; repeated run is a no-op")


if __name__ == "__main__":
    main()
