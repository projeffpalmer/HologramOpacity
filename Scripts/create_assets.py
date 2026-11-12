"""Creates (or updates) the Hologram Opacity mod's content. Run inside the Unreal editor of the starter project:

    UnrealEditor-Cmd.exe <Project>/FactoryGame.uproject -run=pythonscript -script="<this file>" -unattended -nop4 -nosplash

Creates:
  /HologramOpacity/HologramOpacity                      FGGameFeatureData with the asset manager scan rules
  /HologramOpacity/Settings/US_HologramOpacity          FGUserSetting slider in Options > User Interface > Holograms
  /HologramOpacity/Settings/US_HologramScanlines        FGUserSetting "Show Scanlines" checkbox right under the slider
  /HologramOpacity/Materials/RT_HologramOpacityCapture  render target of the hologram capture (lit colour + depth)
  /HologramOpacity/Materials/M_HologramOpacityComposite post process material that draws the captured hologram
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

# Names shared with UHologramOpacitySubsystem
CAPTURE_TARGET = "RT_HologramOpacityCapture"
COMPOSITE_MATERIAL = "M_HologramOpacityComposite"


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


def enum_value(enum_cls, *names):
    for name in names:
        if hasattr(enum_cls, name):
            return getattr(enum_cls, name)
    fail("none of %s in %s (has %s)" % (names, enum_cls.__name__, [n for n in dir(enum_cls) if n.isupper()]))


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


def create_capture_target():
    # The capture resizes it to the main view at runtime: lit hologram colour in RGB, scene depth in A
    target = load_or_create(CAPTURE_TARGET, "/{}/Materials".format(MOD), unreal.TextureRenderTarget2D, unreal.TextureRenderTargetFactoryNew())
    target.set_editor_property("render_target_format", enum_value(unreal.TextureRenderTargetFormat, "RTF_RGBA16F"))
    target.set_editor_property("size_x", 64)
    target.set_editor_property("size_y", 64)
    target.set_editor_property("auto_generate_mips", False)
    save(target)
    return target


def expr(mat, cls, x, y, **props):
    node = mel.create_material_expression(mat, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(src, dst, dst_input, src_output=""):
    if not mel.connect_material_expressions(src, src_output, dst, dst_input):
        fail("could not connect %s.%s -> %s.%s" % (src.get_name(), src_output, dst.get_name(), dst_input))


def math(mat, cls, x, y, a, b, a_out="", b_out="", **props):
    node = expr(mat, cls, x, y, **props)
    link(a, node, "A", a_out)
    if b is not None:
        link(b, node, "B", b_out)
    return node


def mask(mat, x, y, src, src_output="", r=True, g=False, b=False, a=False):
    node = expr(mat, unreal.MaterialExpressionComponentMask, x, y, r=r, g=g, b=b, a=a)
    link(src, node, "", src_output)
    return node


def create_composite_material(capture_target):
    """Post process: draws the captured hologram bodies (lit, with their own materials, so textures, paint, decals,
    materials and animated parts stay) in the hologram colour, with or without scan lines, and blends them over the
    scene at the slider's opacity. Hidden where the world or a build gun helper marker (custom depth) is in front."""
    path = "/{}/Materials".format(MOD)
    full = "{}/{}".format(path, COMPOSITE_MATERIAL)
    # Built from scratch on every run: rebuilding the graph in place left the old nodes in the asset
    if unreal.load_asset("{}.{}".format(full, COMPOSITE_MATERIAL)) is not None and not eal.delete_asset(full):
        fail("could not delete " + full)
    log("creating " + full)
    mat = asset_tools.create_asset(COMPOSITE_MATERIAL, path, unreal.Material, unreal.MaterialFactoryNew())
    if mat is None:
        fail("could not create " + full)
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    # Same place in the frame as the game's own hologram post process (PPI_OutlineHologram)
    mat.set_editor_property("blendable_location", enum_value(unreal.BlendableLocation, "BL_SCENE_COLOR_AFTER_DOF", "BL_AFTER_DOF"))
    mat.set_editor_property("blendable_priority", 10)

    scalar = unreal.MaterialExpressionScalarParameter
    vector = unreal.MaterialExpressionVectorParameter
    scene_id = unreal.SceneTextureId
    linear_sampler = unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR

    # Parameters. Opacity, ShowScanlines and HoloColor come from the mod; the rest is tuned against the vanilla hologram
    holo_color = expr(mat, vector, -3000, -800, parameter_name="HoloColor", default_value=unreal.LinearColor(0.0, 0.653, 1.0, 1.0))
    opacity = expr(mat, scalar, -3000, -700, parameter_name="Opacity", default_value=1.0)
    show_lines = expr(mat, scalar, -3000, -620, parameter_name="ShowScanlines", default_value=1.0)
    emissive_strength = expr(mat, scalar, -3000, -540, parameter_name="EmissiveStrength", default_value=1.6)
    base_intensity = expr(mat, scalar, -3000, -460, parameter_name="BaseIntensity", default_value=0.85)
    line_intensity = expr(mat, scalar, -3000, -380, parameter_name="LineIntensity", default_value=0.18)
    line_period = expr(mat, scalar, -3000, -300, parameter_name="LinePeriod", default_value=12.5)
    line_duty = expr(mat, scalar, -3000, -220, parameter_name="LineDuty", default_value=0.5)
    line_opacity_boost = expr(mat, scalar, -3000, -140, parameter_name="LineOpacityBoost", default_value=0.1)
    detail_gain = expr(mat, scalar, -3000, -60, parameter_name="DetailGain", default_value=1.0)
    detail_floor = expr(mat, scalar, -3000, 20, parameter_name="DetailFloor", default_value=0.1)
    detail_scale = expr(mat, scalar, -3000, 100, parameter_name="DetailScale", default_value=1.8)
    color_saturation = expr(mat, scalar, -3000, 180, parameter_name="ColorSaturation", default_value=1.0)
    depth_bias = expr(mat, scalar, -3000, 260, parameter_name="DepthBias", default_value=2.0)
    depth_bias_scale = expr(mat, scalar, -3000, 340, parameter_name="DepthBiasScale", default_value=0.002)
    # Empty capture pixels hold the far depth, which the 16-bit target caps at 65504 cm: farther counts as no hologram
    max_depth = expr(mat, scalar, -3000, 420, parameter_name="MaxHoloDepth", default_value=60000.0)

    # The scene and its depth (the world, without the hidden hologram), and the helper markers' custom depth
    scene = expr(mat, unreal.MaterialExpressionSceneTexture, -2600, -900, scene_texture_id=scene_id.PPI_POST_PROCESS_INPUT0)
    scene_rgb = mask(mat, -2450, -900, scene, "Color", r=True, g=True, b=True)
    scene_depth = mask(mat, -2450, -820, expr(mat, unreal.MaterialExpressionSceneTexture, -2600, -820, scene_texture_id=scene_id.PPI_SCENE_DEPTH), "Color")
    custom_depth = mask(mat, -2450, -740, expr(mat, unreal.MaterialExpressionSceneTexture, -2600, -740, scene_texture_id=scene_id.PPI_CUSTOM_DEPTH), "Color")

    # The captured hologram. The capture fills the top-left ViewSize pixels of its target (internal resolution)
    capture_object = expr(mat, unreal.MaterialExpressionTextureObjectParameter, -2600, -600, parameter_name="HoloCapture", texture=capture_target, sampler_type=linear_sampler)
    capture_size = expr(mat, unreal.MaterialExpressionTextureProperty, -2450, -540,
                        property=enum_value(unreal.MaterialExposedTextureProperty, "TMTM_TEXTURE_SIZE"))
    link(capture_object, capture_size, "")
    screen = expr(mat, unreal.MaterialExpressionScreenPosition, -2600, -460)
    view_size = expr(mat, unreal.MaterialExpressionViewSize, -2600, -400)
    view_pixel = math(mat, unreal.MaterialExpressionMultiply, -2450, -460, screen, view_size, a_out="ViewportUV")
    capture_uv = math(mat, unreal.MaterialExpressionDivide, -2300, -460, view_pixel, capture_size)
    capture = expr(mat, unreal.MaterialExpressionTextureSample, -2150, -600, sampler_type=linear_sampler)
    link(capture_object, capture, "Tex")
    link(capture_uv, capture, "UVs")
    holo_rgb = mask(mat, -2000, -640, capture, "RGB", r=True, g=True, b=True)
    holo_depth = expr(mat, unreal.MaterialExpressionMin, -2000, -560, const_b=1.0e9)
    link(capture, holo_depth, "A", "A")

    # Visible where nothing (world or helper marker) is in front; empty capture pixels are infinitely far away
    occluder = math(mat, unreal.MaterialExpressionMin, -2300, -800, scene_depth, custom_depth)
    occluder_clamped = expr(mat, unreal.MaterialExpressionMin, -2150, -800, const_b=1.0e9)
    link(occluder, occluder_clamped, "A")
    gap = math(mat, unreal.MaterialExpressionSubtract, -1850, -760, occluder_clamped, holo_depth)
    relative_bias = math(mat, unreal.MaterialExpressionMultiply, -1850, -680, holo_depth, depth_bias_scale)
    bias = math(mat, unreal.MaterialExpressionAdd, -1700, -680, relative_bias, depth_bias)
    gap_biased = math(mat, unreal.MaterialExpressionSubtract, -1550, -720, gap, bias)
    in_front = expr(mat, unreal.MaterialExpressionSaturate, -1400, -720)
    link(gap_biased, in_front, "")
    headroom = math(mat, unreal.MaterialExpressionSubtract, -1700, -620, max_depth, holo_depth)
    present = expr(mat, unreal.MaterialExpressionSaturate, -1550, -620)
    link(headroom, present, "")
    visible = math(mat, unreal.MaterialExpressionMultiply, -1250, -680, in_front, present)

    # Screen-space scan lines like the game's: lines = frac(y / period) > duty
    pixel_y = mask(mat, -2450, -300, screen, "PixelPosition", r=False, g=True)
    y_scaled = math(mat, unreal.MaterialExpressionDivide, -2300, -300, pixel_y, line_period)
    y_frac = expr(mat, unreal.MaterialExpressionFrac, -2150, -300)
    link(y_scaled, y_frac, "")
    above = math(mat, unreal.MaterialExpressionSubtract, -2000, -300, y_frac, line_duty)
    steep = expr(mat, unreal.MaterialExpressionMultiply, -1850, -300, const_b=1000.0)
    link(above, steep, "A")
    lines = expr(mat, unreal.MaterialExpressionSaturate, -1700, -300)
    link(steep, lines, "")
    lines_shown = math(mat, unreal.MaterialExpressionMultiply, -1550, -300, lines, show_lines)

    # Brightness: base plus the lines; without lines the base takes the lines' average so the colour stays the same
    line_term = math(mat, unreal.MaterialExpressionMultiply, -1400, -360, lines_shown, line_intensity)
    lines_hidden = expr(mat, unreal.MaterialExpressionOneMinus, -1700, -220)
    link(show_lines, lines_hidden, "")
    duty_rest = expr(mat, unreal.MaterialExpressionOneMinus, -1700, -160)
    link(line_duty, duty_rest, "")
    average = math(mat, unreal.MaterialExpressionMultiply, -1550, -180, line_intensity, duty_rest)
    average_term = math(mat, unreal.MaterialExpressionMultiply, -1400, -200, average, lines_hidden)
    intensity = math(mat, unreal.MaterialExpressionAdd, -1250, -320, base_intensity, line_term)
    intensity = math(mat, unreal.MaterialExpressionAdd, -1100, -280, intensity, average_term)

    # Detail from the hologram's own lit colour (textures, paint, decals, shading), keeping part of its hue like the
    # game's hologram does, with a soft shoulder so bright surfaces keep their detail
    weights = expr(mat, unreal.MaterialExpressionConstant3Vector, -1850, -520, constant=unreal.LinearColor(0.299, 0.587, 0.114, 0.0))
    luminance = math(mat, unreal.MaterialExpressionDotProduct, -1700, -560, holo_rgb, weights)
    grey = expr(mat, unreal.MaterialExpressionConstant3Vector, -1700, -480, constant=unreal.LinearColor(1.0, 1.0, 1.0, 0.0))
    luminance_rgb = math(mat, unreal.MaterialExpressionMultiply, -1550, -520, grey, luminance)
    colored = expr(mat, unreal.MaterialExpressionLinearInterpolate, -1400, -560)
    link(luminance_rgb, colored, "A")
    link(holo_rgb, colored, "B")
    link(color_saturation, colored, "Alpha")
    exposed = math(mat, unreal.MaterialExpressionMultiply, -1250, -560, colored, detail_gain)
    exposed_plus_one = expr(mat, unreal.MaterialExpressionAdd, -1100, -520, const_b=1.0)
    link(exposed, exposed_plus_one, "A")
    shoulder = math(mat, unreal.MaterialExpressionDivide, -950, -560, exposed, exposed_plus_one)
    detail = math(mat, unreal.MaterialExpressionMultiply, -800, -560, shoulder, detail_scale)
    detail = math(mat, unreal.MaterialExpressionAdd, -650, -560, detail, detail_floor)
    detail_clamped = expr(mat, unreal.MaterialExpressionSaturate, -500, -560)
    link(detail, detail_clamped, "")

    # Hologram colour
    tinted = math(mat, unreal.MaterialExpressionMultiply, -950, -800, holo_color, emissive_strength)
    holo = math(mat, unreal.MaterialExpressionMultiply, -800, -700, tinted, intensity)
    holo = math(mat, unreal.MaterialExpressionMultiply, -350, -650, holo, detail_clamped)

    # Coverage: the slider's opacity, a bit more on the lines, only where the hologram is visible
    line_boost = math(mat, unreal.MaterialExpressionMultiply, -1250, -120, lines_shown, line_opacity_boost)
    one_plus_line = expr(mat, unreal.MaterialExpressionAdd, -1100, -120, const_b=1.0)
    link(line_boost, one_plus_line, "A")
    alpha = math(mat, unreal.MaterialExpressionMultiply, -950, -160, opacity, one_plus_line)
    alpha_clamped = expr(mat, unreal.MaterialExpressionSaturate, -800, -160)
    link(alpha, alpha_clamped, "")
    coverage = math(mat, unreal.MaterialExpressionMultiply, -650, -200, alpha_clamped, visible)

    result = expr(mat, unreal.MaterialExpressionLinearInterpolate, -200, -500)
    link(scene_rgb, result, "A")
    link(holo, result, "B")
    link(coverage, result, "Alpha")
    if not mel.connect_material_property(result, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        fail("emissive connection failed")
    mel.recompile_material(mat)
    save(mat)
    log("composite material: %d expressions" % mel.get_num_material_expressions(mat))


def main():
    if not eal.does_directory_exist("/" + MOD):
        log("content root /%s not listed yet (it is created with the first asset)" % MOD)
    create_game_feature_data()
    create_setting()
    create_scanlines_setting()
    create_composite_material(create_capture_target())
    log("done")


main()
