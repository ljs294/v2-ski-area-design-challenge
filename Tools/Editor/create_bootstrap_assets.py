"""Run inside the built Unreal Editor only. Creates three explicitly owned P0 assets."""
from pathlib import Path
import sys
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
sys.path.insert(0, str(ROOT / "Tools/Editor"))
from asset_ownership import ASSETS, recipe_digest, preflight, record_asset


def main():
    receipt = preflight(ROOT, recipe_digest(ROOT))
    dirty = list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
    dirty += list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
    if dirty:
        raise RuntimeError("Dirty editor packages exist; do not save or discard unrelated editor state")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    folder = "/Game/P0Generated"
    material_file = "Content/P0Generated/M_Bootstrap.uasset"
    if material_file not in receipt["assets"]:
        material = tools.create_asset("M_Bootstrap", folder, unreal.Material, unreal.MaterialFactoryNew())
        if not material:
            raise RuntimeError("Material creation failed")
        expression = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
        expression.set_editor_property("constant", unreal.LinearColor(0.06, 0.12, 0.18, 1.0))
        unreal.MaterialEditingLibrary.connect_material_property(expression, "", unreal.MaterialProperty.MP_BASE_COLOR)
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
            raise RuntimeError("Material save failed")
        record_asset(ROOT, receipt, material_file, {"base_color": [0.06, 0.12, 0.18], "role": "P0 sample only"})
    widget_file = "Content/P0Generated/WBP_Bootstrap.uasset"
    if widget_file not in receipt["assets"]:
        factory = unreal.WidgetBlueprintFactory()
        parent = unreal.load_class(None, "/Script/SkiPresentation.SkiBootstrapWidget")
        if not parent:
            raise RuntimeError("Build the native editor before generating the widget")
        factory.set_editor_property("parent_class", parent)
        widget = tools.create_asset("WBP_Bootstrap", folder, unreal.WidgetBlueprint, factory)
        if not widget or not unreal.EditorAssetLibrary.save_loaded_asset(widget, only_if_is_dirty=False):
            raise RuntimeError("Widget creation/save failed")
        record_asset(ROOT, receipt, widget_file, {"parent": "/Script/SkiPresentation.SkiBootstrapWidget", "graphs": "No authored gameplay logic"})
    map_file = "Content/P0Generated/Bootstrap.umap"
    if map_file not in receipt["assets"]:
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not levels.new_level(folder + "/Bootstrap") or not levels.save_current_level():
            raise RuntimeError("Bootstrap map creation/save failed")
        record_asset(ROOT, receipt, map_file, {"world": "Empty bootstrap", "game_mode": "/Script/SkiPresentation.SkiBootstrapGameMode"})
    if set(receipt["assets"]) != set(ASSETS):
        raise RuntimeError("Bootstrap recipe is incomplete")
    unreal.log("SKI_P0_ASSETS_READY: three owned assets; repeated run is a no-op")


if __name__ == "__main__":
    main()
