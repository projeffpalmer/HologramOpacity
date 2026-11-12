"""Creates (or updates) the Hologram Opacity mod's content. Run inside the Unreal editor of the starter project:

    UnrealEditor-Cmd.exe <Project>/FactoryGame.uproject -run=pythonscript -script="<this file>" -unattended -nop4 -nosplash

Creates:
  /HologramOpacity/HologramOpacity                    FGGameFeatureData with the asset manager scan rules
  /HologramOpacity/Settings/US_HologramOpacity        FGUserSetting slider in Options > User Interface > Holograms
  /HologramOpacity/Settings/US_HologramScanlines      FGUserSetting "Show Scanlines" checkbox right under the slider
  /HologramOpacity/Materials/M_HologramOpacity        translucent hologram material (custom depth writes on)
"""
import unreal

MOD = "HologramOpacity"
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
lib = unreal.HologramOpacityEditorLibrary

# Same scan rules the Alpakit "C++ & Blueprint" game feature template writes ($PluginName substituted)
SCAN_RULES = [
    '(PrimaryAssetType="FGSchematic",AssetBaseClass="/Script/FactoryGame.FGSchematic",bHasBlueprintClasses=True,Directories=((Path="/{0}/Schematics")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGResearchTree",AssetBaseClass="/Script/FactoryGame.FGResearchTree",bHasBlueprintClasses=True,Directories=((Path="/{0}/Schematics/Research")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGUserSetting",AssetBaseClass="/Script/FactoryGame.FGUserSetting",Directories=((Path="/{0}/Settings")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGMessage",AssetBaseClass="/Script/FactoryGame.FGMessage",Directories=((Path="/{0}/Narrative")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGRemoteCallObject",AssetBaseClass="/Script/FactoryGame.FGRemoteCallObject",bHasBlueprintClasses=True,Directories=((Path="/{0}/Buildable"),(Path="/{0}/-Shared/Blueprint")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGIconLibrary",AssetBaseClass="/Script/FactoryGame.FGIconLibrary",Directories=((Path="/{0}/-Shared/Blueprint")),Rules=(Priority=2,ChunkId=1))',
    '(PrimaryAssetType="FGChildInputMappingContext",AssetBaseClass="/Script/FactoryGame.FGChildInputMappingContext",Directories=((Path="/{0}/Inputs")),Rules=(Priority=2,ChunkId=1))',
]

UI_CATEGORY = "/Game/FactoryGame/Settings/OptionsMenu/UserInterface/USC_UI.USC_UI_C"
HOLOGRAM_SUBCATEGORY = "/Game/FactoryGame/Settings/OptionsMenu/UserInterface/USubC_UI_Holograms.USubC_UI_Holograms_C"
# After the four colour pickers (3-6) and before High Contrast Build Mode (7); scanlines right under the slider
MENU_PRIORITY = 6.5
SCANLINES_MENU_PRIORITY = 6.75


def log(msg):
    unreal.log("[HologramOpacity] " + msg)


def fail(msg):
    unreal.log_error("[HologramOpacity] " + msg)
    raise RuntimeError(msg)


def load_or_create(name, path, cls, factory):
    full = "{}/{}".format(path, name)
    # Load straight from disk: in a commandlet the asset registry may not have scanned plugin folders yet
    existing = unreal.load_asset("{}.{}".format(full, name))
    if existing is not None or eal.does_asset_exist(full):
        log("updating " + full)
        return existing if existing is not None else eal.load_asset(full)
    log("creating " + full)
    asset = asset_tools.create_asset(name, path, cls, factory)
    if asset is None:
        fail("could not create " + full)
    return asset


def data_asset_factory(cls):
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", cls)
    return factory


def save(asset):
    if not eal.save_loaded_asset(asset, only_if_is_dirty=False):
        fail("could not save " + asset.get_path_name())


def create_game_feature_data():
    gfd = load_or_create(MOD, "/" + MOD, unreal.FGGameFeatureData, data_asset_factory(unreal.FGGameFeatureData))
    for rule in SCAN_RULES:
        if not lib.add_primary_asset_type_to_scan(gfd, rule.format(MOD)):
            fail("bad scan rule " + rule)
    save(gfd)


def create_setting():
    setting = load_or_create("US_HologramOpacity", "/{}/Settings".format(MOD), unreal.FGUserSetting, data_asset_factory(unreal.FGUserSetting))
    category = unreal.load_class(None, UI_CATEGORY)
    sub_category = unreal.load_class(None, HOLOGRAM_SUBCATEGORY)
    if category is None or sub_category is None:
        fail("options menu category classes not found")
    if not lib.setup_opacity_setting(setting, category, sub_category, MENU_PRIORITY):
        fail("setup_opacity_setting failed")
    save(setting)
    log("setting StrId=%s widgets=%s apply=%s" % (setting.get_editor_property("str_id"),
        len(setting.get_editor_property("widgets_to_create")), setting.get_editor_property("apply_type")))


def create_scanlines_setting():
    setting = load_or_create("US_HologramScanlines", "/{}/Settings".format(MOD), unreal.FGUserSetting, data_asset_factory(unreal.FGUserSetting))
    category = unreal.load_class(None, UI_CATEGORY)
    sub_category = unreal.load_class(None, HOLOGRAM_SUBCATEGORY)
    if category is None or sub_category is None:
        fail("options menu category classes not found")
    if not lib.setup_scanlines_setting(setting, category, sub_category, SCANLINES_MENU_PRIORITY):
        fail("setup_scanlines_setting failed")
    save(setting)
    log("setting StrId=%s selector=%s" % (setting.get_editor_property("str_id"),
        setting.get_editor_property("value_selector").get_class().get_name()))


def expr(mat, cls, x, y, **props):
    node = mel.create_material_expression(mat, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(src, dst, dst_input, src_output=""):
    if not mel.connect_material_expressions(src, src_output, dst, dst_input):
        fail("could not connect %s.%s -> %s.%s" % (src.get_name(), src_output, dst.get_name(), dst_input))


def create_material():
    mat = load_or_create("M_HologramOpacity", "/{}/Materials".format(MOD), unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    # One-sided: a closed mesh then blends the background once, so 50% really looks half see-through
    # (two-sided showed front and back faces, which doubled the coverage)
    mat.set_editor_property("two_sided", False)
    # Keep writing custom depth/stencil while see-through, so the game's hologram outline post process still works
    mat.set_editor_property("allow_translucent_custom_depth_writes", True)
    mat.set_editor_property("opacity_mask_clip_value", 0.001)
    # Every mesh type a hologram can use (same set as the vanilla HologramColor material)
    for flag in ("used_with_skeletal_mesh", "used_with_spline_meshes", "used_with_instanced_static_meshes",
                 "used_with_instanced_spline_meshes", "used_with_niagara_mesh_particles"):
        try:
            mat.set_editor_property(flag, True)
        except Exception as ex:  # flag missing in this engine build
            unreal.log_warning("[HologramOpacity] usage flag %s not set: %s" % (flag, ex))

    mel.delete_all_material_expressions(mat)
    scalar = unreal.MaterialExpressionScalarParameter
    color = expr(mat, unreal.MaterialExpressionVectorParameter, -1400, -400,
                 parameter_name="HoloColor", default_value=unreal.LinearColor(0.0, 0.653, 1.0, 1.0))
    # Defaults tuned in game against the vanilla post process look (12.5 px soft lines, matching colour at ~100%)
    emissive_strength = expr(mat, scalar, -1400, -250, parameter_name="EmissiveStrength", default_value=0.8)
    base_intensity = expr(mat, scalar, -1400, -150, parameter_name="BaseIntensity", default_value=0.85)
    line_intensity = expr(mat, scalar, -1400, -50, parameter_name="LineIntensity", default_value=0.18)
    line_period = expr(mat, scalar, -1400, 50, parameter_name="LinePeriod", default_value=12.5)
    line_duty = expr(mat, scalar, -1400, 150, parameter_name="LineDuty", default_value=0.5)
    scan_speed = expr(mat, scalar, -1400, 250, parameter_name="ScanSpeed", default_value=0.0)
    opacity = expr(mat, scalar, -1400, 400, parameter_name="Opacity", default_value=0.5)
    line_opacity_boost = expr(mat, scalar, -1400, 500, parameter_name="LineOpacityBoost", default_value=0.1)
    min_opacity = expr(mat, scalar, -1400, 600, parameter_name="MinOpacity", default_value=0.002)
    fresnel_exponent = expr(mat, scalar, -1400, 700, parameter_name="FresnelExponent", default_value=3.0)
    fresnel_boost = expr(mat, scalar, -1400, 800, parameter_name="FresnelBoost", default_value=0.35)

    # Screen-space scan lines, like the game's hologram post process: lines = frac(y / period + t * speed) > duty
    screen = expr(mat, unreal.MaterialExpressionScreenPosition, -1150, 0)
    pixel_y = expr(mat, unreal.MaterialExpressionComponentMask, -1000, 0, r=False, g=True, b=False, a=False)
    link(screen, pixel_y, "", "PixelPosition")
    y_scaled = expr(mat, unreal.MaterialExpressionDivide, -850, 0)
    link(pixel_y, y_scaled, "A")
    link(line_period, y_scaled, "B")
    time = expr(mat, unreal.MaterialExpressionTime, -1000, 250)
    scroll = expr(mat, unreal.MaterialExpressionMultiply, -850, 250)
    link(time, scroll, "A")
    link(scan_speed, scroll, "B")
    y_scrolled = expr(mat, unreal.MaterialExpressionAdd, -700, 50)
    link(y_scaled, y_scrolled, "A")
    link(scroll, y_scrolled, "B")
    y_frac = expr(mat, unreal.MaterialExpressionFrac, -580, 50)
    link(y_scrolled, y_frac, "")
    above = expr(mat, unreal.MaterialExpressionSubtract, -460, 80)
    link(y_frac, above, "A")
    link(line_duty, above, "B")
    steep = expr(mat, unreal.MaterialExpressionMultiply, -340, 80, const_b=1000.0)
    link(above, steep, "A")
    lines = expr(mat, unreal.MaterialExpressionSaturate, -220, 80)
    link(steep, lines, "")

    # Brightness: base + lines, brighter towards silhouette edges (fresnel)
    line_term = expr(mat, unreal.MaterialExpressionMultiply, -100, -40)
    link(lines, line_term, "A")
    link(line_intensity, line_term, "B")
    intensity = expr(mat, unreal.MaterialExpressionAdd, 20, -100)
    link(base_intensity, intensity, "A")
    link(line_term, intensity, "B")
    fresnel = expr(mat, unreal.MaterialExpressionFresnel, -1000, 650)
    link(fresnel_exponent, fresnel, "ExponentIn")
    edge = expr(mat, unreal.MaterialExpressionMultiply, -850, 700)
    link(fresnel, edge, "A")
    link(fresnel_boost, edge, "B")
    one_plus_edge = expr(mat, unreal.MaterialExpressionAdd, -700, 700, const_b=1.0)
    link(edge, one_plus_edge, "A")

    tinted = expr(mat, unreal.MaterialExpressionMultiply, -1000, -330)
    link(color, tinted, "A")
    link(emissive_strength, tinted, "B")
    lit = expr(mat, unreal.MaterialExpressionMultiply, 150, -250)
    link(tinted, lit, "A")
    link(intensity, lit, "B")
    emissive = expr(mat, unreal.MaterialExpressionMultiply, 300, -200)
    link(lit, emissive, "A")
    link(one_plus_edge, emissive, "B")

    # Opacity: slider value, a bit more on edges and lines, never fully zero so custom depth/clicks keep working
    line_op = expr(mat, unreal.MaterialExpressionMultiply, -100, 450)
    link(lines, line_op, "A")
    link(line_opacity_boost, line_op, "B")
    one_plus_line = expr(mat, unreal.MaterialExpressionAdd, 20, 450, const_b=1.0)
    link(line_op, one_plus_line, "A")
    edged_opacity = expr(mat, unreal.MaterialExpressionMultiply, 20, 600)
    link(opacity, edged_opacity, "A")
    link(one_plus_edge, edged_opacity, "B")
    lined_opacity = expr(mat, unreal.MaterialExpressionMultiply, 150, 520)
    link(edged_opacity, lined_opacity, "A")
    link(one_plus_line, lined_opacity, "B")
    clamped = expr(mat, unreal.MaterialExpressionSaturate, 280, 520)
    link(lined_opacity, clamped, "")
    floored = expr(mat, unreal.MaterialExpressionMax, 400, 540)
    link(clamped, floored, "A")
    link(min_opacity, floored, "B")

    if not mel.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        fail("emissive connection failed")
    if not mel.connect_material_property(floored, "", unreal.MaterialProperty.MP_OPACITY):
        fail("opacity connection failed")
    mel.recompile_material(mat)
    save(mat)


def main():
    if not eal.does_directory_exist("/" + MOD):
        log("content root /%s not listed yet (it is created with the first asset)" % MOD)
    create_game_feature_data()
    create_setting()
    create_scanlines_setting()
    create_material()
    log("done")


main()
