#!/usr/bin/env python3
"""Create the separate RECOMP-ball variant and render only a quick Eevee preview.

Run make_recomp_ball_print.py first, then run this script in Blender.
The existing app icon and its scene are left untouched.
"""

import sys
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from render_icon import compositor, point_at, save_portable

OUT = ROOT / "assets/icon/variants/recomp-ball"


def main():
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / "assets/icon/tabletennis_icon.blend"))
    scene = bpy.context.scene
    ball = bpy.data.objects["HERO BALL | seamless matte polymer"]
    camera = scene.camera
    center = ball.matrix_world.translation.copy()
    # Tighter crop from the same low side angle, with the print in the focal plane.
    camera.data.lens = 120
    point_at(camera, center + Vector((0, 0.004, 0.006)))
    normal = (camera.location - center).normalized()
    focus = bpy.data.objects["FOCUS | blade and supporting ball"]
    focus.location = center + normal * 0.018
    camera.data.dof.aperture_fstop = 11

    projection = bpy.data.objects.new("Ball print | camera-facing projection", None)
    bpy.context.collection.objects.link(projection)
    projection.location = center
    projection.rotation_euler = normal.to_track_quat("Z", "Y").to_euler()
    projection.empty_display_type = "ARROWS"
    projection.empty_display_size = 0.025

    material = ball.active_material.copy()
    material.name = "Matte ivory ball | original RECOMP print"
    ball.data.materials[0] = material
    nodes, links = material.node_tree.nodes, material.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    base = tuple(bsdf.inputs["Base Color"].default_value)
    coords = nodes.new("ShaderNodeTexCoord")
    coords.object = projection
    mapping = nodes.new("ShaderNodeVectorMath")
    mapping.operation = "MULTIPLY_ADD"
    mapping.inputs[1].default_value = (1/0.032, 1/0.032, 1)
    mapping.inputs[2].default_value = (0.5, 0.5, 0)
    image = bpy.data.images.load(str(OUT / "recomp_ball_print.png"), check_existing=True)
    image.pack()
    texture = nodes.new("ShaderNodeTexImage")
    texture.name = "Original RECOMP ink | transparent artwork"
    texture.image = image
    texture.extension = "CLIP"
    front = nodes.new("ShaderNodeSeparateXYZ")
    gate = nodes.new("ShaderNodeMath")
    gate.operation = "GREATER_THAN"
    gate.inputs[1].default_value = 0
    alpha = nodes.new("ShaderNodeMath")
    alpha.operation = "MULTIPLY"
    mix = nodes.new("ShaderNodeMixRGB")
    mix.inputs[1].default_value = base
    links.new(coords.outputs["Object"], mapping.inputs[0])
    links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
    links.new(coords.outputs["Object"], front.inputs[0])
    links.new(front.outputs["Z"], gate.inputs[0])
    links.new(gate.outputs[0], alpha.inputs[0])
    links.new(texture.outputs["Alpha"], alpha.inputs[1])
    links.new(alpha.outputs[0], mix.inputs[0])
    links.new(texture.outputs["Color"], mix.inputs[2])
    links.new(mix.outputs[0], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.56
    bsdf.inputs["Specular IOR Level"].default_value = 0.22

    scene.render.engine = "BLENDER_EEVEE"
    scene.eevee.taa_render_samples = 64
    scene.eevee.use_raytracing = True
    scene.eevee.ray_tracing_options.resolution_scale = "2"
    scene.eevee.use_bokeh_jittered = True
    scene.render.resolution_x = scene.render.resolution_y = 1024
    scene.render.resolution_percentage = 100
    compositor()
    scene.render.filepath = "//preview_eevee.png"
    OUT.mkdir(parents=True, exist_ok=True)
    save_portable(OUT / "recomp_ball_preview.blend")
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
