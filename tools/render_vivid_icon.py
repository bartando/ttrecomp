#!/usr/bin/env python3
"""Render the vivid app icon in Eevee: a rounded desktop master and a full-bleed square.

The PS5 home screen rounds its tiles itself, so it needs the square render.
"""

import sys
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from render_icon import compositor, save_portable

OUT = ROOT / "assets/icon/variants/vivid"
ASSETS = ROOT / "assets/icon"


def surface(name):
    return bpy.data.materials[name].node_tree.nodes.get("Principled BSDF")


def main():
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / "assets/icon/tabletennis_icon.blend"))
    scene = bpy.context.scene
    # Reduce the pale softbox reflection and let the green paint read more clearly.
    table = surface("Tournament green | satin painted top")
    table.inputs["Base Color"].default_value = (0.006, 0.09, 0.026, 1)
    table.inputs["Roughness"].default_value = 0.5
    table.inputs["Specular IOR Level"].default_value = 0.10
    # A material mask lets the color grade target the green paint, not the ball,
    # white table lines, paddle or warm backdrop.
    aov = bpy.context.view_layer.aovs.add()
    aov.name = "Green tabletop"
    aov.type = "VALUE"
    table_material = bpy.data.materials["Tournament green | satin painted top"]
    mask_output = table_material.node_tree.nodes.new("ShaderNodeOutputAOV")
    mask_output.aov_name = aov.name
    mask_output.inputs["Value"].default_value = 1
    surface("Crimson inverted competition rubber").inputs["Base Color"].default_value = (0.40, 0.004, 0.008, 1)
    surface("Dark warm arena wall").inputs["Base Color"].default_value = (0.065, 0.030, 0.009, 1)
    bpy.data.objects["Amber wall wash"].data.energy = 85
    bpy.data.objects["Rim | narrow softbox"].data.energy = 30
    bpy.data.objects["Low strip bounce | black rubber detail"].data.energy = 0.28
    scene.view_settings.look = "AgX - High Contrast"
    scene.view_settings.exposure = 0.15

    scene.render.engine = "BLENDER_EEVEE"
    scene.eevee.taa_render_samples = 64
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_options.resolution_scale = "2"
    scene.eevee.use_bokeh_jittered = True
    scene.render.resolution_x = scene.render.resolution_y = 2048
    scene.render.resolution_percentage = 100
    compositor()
    tree = scene.compositing_node_group
    source = next(node for node in tree.nodes if node.type == "R_LAYERS")
    transform = next(node for node in tree.nodes if node.type == "TRANSFORM")
    saturation = tree.nodes.new("CompositorNodeHueSat")
    saturation.label = "Selective tabletop saturation"
    saturation.inputs["Saturation"].default_value = 1.3
    tree.links.new(source.outputs["Image"], saturation.inputs["Image"])
    tree.links.new(source.outputs[aov.name], saturation.inputs["Factor"])
    tree.links.new(saturation.outputs["Image"], transform.inputs["Image"])
    OUT.mkdir(parents=True, exist_ok=True)
    save_portable(OUT / "vivid_preview.blend")
    scene.render.filepath = str(ASSETS / "tabletennis_icon_vivid_master.png")
    bpy.ops.render.render(write_still=True)
    # Skip the inset and rounded mask; keep the selective saturation.
    output = next(node for node in tree.nodes if node.type == "GROUP_OUTPUT")
    tree.links.new(saturation.outputs["Image"], output.inputs["Image"])
    scene.render.filepath = str(ASSETS / "tabletennis_icon_vivid_square.png")
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
