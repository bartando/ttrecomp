#!/usr/bin/env python3
"""Build and render the app icon in Blender with the packed rubber normal map.

Blender --background --python tools/render_icon.py -- --preview
Blender --background --python tools/render_icon.py -- --final
"""

import argparse
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out" / "icon-blender-review"
ASSETS = ROOT / "assets" / "icon"


def material(name, color, roughness=0.4, metallic=0.0):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return mat


def grain(mat, scale, strength, distance):
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = scale
    noise.inputs["Detail"].default_value = 2
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = strength
    bump.inputs["Distance"].default_value = distance
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], nodes.get("Principled BSDF").inputs["Normal"])


def wood(name, dark, light):
    mat = material(name, light, 0.31)
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    tex = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeVectorMath")
    mapping.operation = "MULTIPLY"
    mapping.inputs[1].default_value = (80, 3, 15)
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 3
    noise.inputs["Detail"].default_value = 3
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.23
    ramp.color_ramp.elements[0].color = (*dark, 1)
    ramp.color_ramp.elements[1].position = 0.8
    ramp.color_ramp.elements[1].color = (*light, 1)
    links.new(tex.outputs["Generated"], mapping.inputs[0])
    links.new(mapping.outputs["Vector"], noise.inputs["Vector"])
    links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], nodes.get("Principled BSDF").inputs["Base Color"])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.12
    bump.inputs["Distance"].default_value = 0.0006
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], nodes.get("Principled BSDF").inputs["Normal"])
    return mat


def finish(obj, name, mat, bevel=0):
    obj.name = name
    obj.data.materials.append(mat)
    if bevel:
        modifier = obj.modifiers.new("Manufactured edge radius", "BEVEL")
        modifier.width = bevel
        modifier.segments = 4
        obj.modifiers.new("Weighted corner normals", "WEIGHTED_NORMAL")
    return obj


def box(name, location, dimensions, mat, bevel=0.005):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    obj = bpy.context.object
    obj.dimensions = dimensions
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(obj, name, mat, bevel)


def cylinder(name, a, b, radius, mat, vertices=32):
    direction = Vector(b) - Vector(a)
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                      depth=direction.length, location=(Vector(a) + Vector(b)) / 2)
    obj = bpy.context.object
    obj.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    for polygon in obj.data.polygons:
        polygon.use_smooth = len(polygon.vertices) == 4
    return finish(obj, name, mat, min(radius * 0.18, 0.002))


def curve_bundle(name, paths, radius, mat):
    data = bpy.data.curves.new(name, "CURVE")
    data.dimensions = "3D"
    data.bevel_depth = radius
    data.bevel_resolution = 2
    for path in paths:
        spline = data.splines.new("POLY")
        spline.points.add(len(path) - 1)
        for point, co in zip(spline.points, path):
            point.co = (*co, 1)
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def silhouette(name, outline, z, depth, mat, bevel=0.001):
    count = len(outline)
    vertices = [(x, y, z - depth / 2) for x, y in outline]
    vertices += [(x, y, z + depth / 2) for x, y in outline]
    faces = [tuple(reversed(range(count))), tuple(range(count, count * 2))]
    for i in range(count):
        j = (i + 1) % count
        faces.append((i, j, j + count, i + count))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    uv = mesh.uv_layers.new(name="Planar material UV")
    for loop in mesh.loops:
        vertex = mesh.vertices[loop.vertex_index].co
        uv.data[loop.index].uv = (vertex.x / 0.15 + 0.5, vertex.y / 0.16 + 0.5)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return finish(obj, name, mat, bevel)


def sculpted_grip(name, center_z, depth, mat):
    """Reference-shaped grip: broad shoulder, slim neck and gently flared butt."""
    sections = [(-0.0778, 0.035, 0.08), (-0.079, 0.033, 0.25),
                (-0.083, 0.026, 0.5), (-0.089, 0.020, 0.75),
                (-0.098, 0.015, 0.94), (-0.110, 0.014, 1),
                (-0.13, 0.014, 1), (-0.15, 0.015, 1),
                (-0.17, 0.017, 1), (-0.179, 0.018, 1),
                (-0.181, 0.018, 0.94), (-0.182, 0.018, 0.55),
                (-0.1825, 0.018, 0.01)]
    vertices, faces = [], []
    count = 48
    for y, width, cap in sections:
        for i in range(count):
            angle = 2 * math.pi * i / count
            c, s = math.cos(angle), math.sin(angle)
            x = math.copysign(abs(c) ** 0.55, c) * width * cap
            z = math.copysign(abs(s) ** 0.65, s) * depth * cap
            vertices.append((x, y, center_z + z))
    for ring in range(len(sections) - 1):
        for i in range(count):
            j = (i + 1) % count
            faces.append((ring * count + i, ring * count + j,
                          (ring + 1) * count + j, (ring + 1) * count + i))
    faces.extend([tuple(reversed(range(count))), tuple(range(len(vertices) - count, len(vertices)))])
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    finish(obj, name, mat)
    modifier = obj.modifiers.new("Smooth carved wood", "SUBSURF")
    modifier.levels = modifier.render_levels = 2
    return obj


def paddle_outline(with_handle=False):
    """Trace the supplied reference proportions with smooth cubic shoulders."""
    right = [(0, 0.081)]
    def cubic(end, a, b, count=28):
        start = right[-1]
        for i in range(1, count + 1):
            t = i / count
            right.append(tuple((1-t)**3 * start[j] + 3*(1-t)**2*t*a[j]
                               + 3*(1-t)*t*t*b[j] + t**3*end[j] for j in (0, 1)))
    cubic((0.073, 0.012), (0.040, 0.082), (0.072, 0.052))
    cubic((0.075, -0.027), (0.076, -0.004), (0.076, -0.014))
    cubic((0.038, -0.077), (0.075, -0.058), (0.060, -0.070))
    if with_handle:
        cubic((0.014, -0.11), (0.023, -0.084), (0.014, -0.094), 16)
        cubic((0.018, -0.180), (0.013, -0.14), (0.017, -0.168), 20)
        cubic((0, -0.183), (0.018, -0.1825), (0.006, -0.183), 12)
    # Counter-clockwise outline gives outward normals and a planar rubber face.
    return list(reversed(right + [(-x, y) for x, y in reversed(right[1:-1] if with_handle else right[1:])]))


def point_at(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


def light(name, location, target, energy, size, color, shape="DISK", size_y=None):
    data = bpy.data.lights.new(name, "AREA")
    data.energy, data.shape, data.size, data.color = energy, shape, size, color
    if size_y is not None:
        data.size_y = size_y
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    point_at(obj, target)


def camera(name, location, target, scale):
    data = bpy.data.cameras.new(name)
    data.type = "ORTHO"
    data.ortho_scale = scale
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    point_at(obj, target)
    return obj


def build_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    green = material("Tournament green | satin painted top", (0.008, 0.055, 0.027), 0.43)
    green.node_tree.nodes.get("Principled BSDF").inputs["Specular IOR Level"].default_value = 0.16
    grain(green, 1800, 0.17, 0.000045)
    ivory = material("Warm white enamel | regulation lines", (0.85, 0.86, 0.79), 0.36)
    metal = material("Graphite powder-coated steel", (0.023, 0.031, 0.034), 0.32, 0.65)
    chrome = material("Machined stainless hardware", (0.35, 0.4, 0.42), 0.24, 0.9)
    black = material("Rubber feet", (0.009, 0.012, 0.013), 0.65)
    net_mat = material("Woven charcoal net thread", (0.024, 0.037, 0.032), 0.72)
    tape_mat = material("Ivory woven net binding", (0.42, 0.46, 0.41), 0.7)
    grain(tape_mat, 350, 0.25, 0.0004)
    red = material("Crimson inverted competition rubber", (0.24, 0.003, 0.009), 0.43)
    red.node_tree.nodes.get("Principled BSDF").inputs["Specular IOR Level"].default_value = 0.19
    normal_image = bpy.data.images.load(str(ASSETS / "textures" / "rubber_normal.png"), check_existing=True)
    normal_image.colorspace_settings.name = "Non-Color"
    normal_image.pack()
    normal_texture = red.node_tree.nodes.new("ShaderNodeTexImage")
    normal_texture.name = "Imagegen rubber microstructure | normal data only"
    normal_texture.image = normal_image
    normal_uv = red.node_tree.nodes.new("ShaderNodeTexCoord")
    normal_scale = red.node_tree.nodes.new("ShaderNodeVectorMath")
    normal_scale.operation = "SCALE"
    normal_scale.inputs[3].default_value = 2.2
    red.node_tree.links.new(normal_uv.outputs["UV"], normal_scale.inputs[0])
    red.node_tree.links.new(normal_scale.outputs["Vector"], normal_texture.inputs["Vector"])
    normal_node = red.node_tree.nodes.new("ShaderNodeNormalMap")
    normal_node.inputs["Strength"].default_value = 0.085
    normal_node.uv_map = "Planar material UV"
    red.node_tree.links.new(normal_texture.outputs["Color"], normal_node.inputs["Color"])
    red.node_tree.links.new(normal_node.outputs["Normal"], red.node_tree.nodes.get("Principled BSDF").inputs["Normal"])
    sponge = material("Red rubber sponge edge", (0.32, 0.034, 0.015), 0.7)
    maple = wood("Pale limba blade plies", (0.30, 0.24, 0.16), (0.64, 0.54, 0.39))
    grip_mat = wood("Pale ash grip with charcoal diagonal accents", (0.26, 0.24, 0.20), (0.66, 0.62, 0.52))
    grip_mat.node_tree.nodes.get("Principled BSDF").inputs["Roughness"].default_value = 0.43
    nodes, links = grip_mat.node_tree.nodes, grip_mat.node_tree.links
    wood_color = nodes.get("Principled BSDF").inputs["Base Color"].links[0].from_socket
    coordinates = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    slope = nodes.new("ShaderNodeMath")
    slope.operation = "MULTIPLY_ADD"
    slope.inputs[1].default_value = 0.23
    slope.inputs[2].default_value = 0
    stripe_coord = nodes.new("ShaderNodeMath")
    stripe_coord.operation = "ADD"
    links.new(coordinates.outputs["Generated"], separate.inputs[0])
    links.new(separate.outputs["X"], slope.inputs[0])
    links.new(slope.outputs[0], stripe_coord.inputs[0])
    links.new(separate.outputs["Y"], stripe_coord.inputs[1])
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.interpolation = "CONSTANT"
    for element in list(ramp.color_ramp.elements)[1:]:
        ramp.color_ramp.elements.remove(element)
    ramp.color_ramp.elements[0].color = (1, 1, 1, 1)
    for position, value in [(0.31, 0), (0.36, 1), (0.43, 0)]:
        element = ramp.color_ramp.elements.new(position)
        element.color = (value, value, value, 1)
    mix = nodes.new("ShaderNodeMixRGB")
    mix.inputs[2].default_value = (0.013, 0.018, 0.019, 1)
    links.new(stripe_coord.outputs[0], ramp.inputs[0])
    links.new(ramp.outputs[0], mix.inputs[0])
    links.new(wood_color, mix.inputs[1])
    links.new(mix.outputs[0], nodes.get("Principled BSDF").inputs["Base Color"])
    dark_ply = material("Carbon reinforcement plies", (0.021, 0.024, 0.025), 0.5)
    base_mat = material("Burgundy arena floor", (0.035, 0.005, 0.011), 0.5)
    underside = red.copy()
    underside.name = "Black inverted rubber | same packed micro-normal"
    underside.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = (0.009, 0.017, 0.019, 1)
    underside.node_tree.nodes.get("Principled BSDF").inputs["Roughness"].default_value = 0.47

    # Full, physically built table; the hero camera takes a close DSLR-style shot.
    box("Dark burgundy arena floor", (0, 0, -0.03), (200, 200, 0.05), base_mat)

    # Regulation table dimensions: 2.74 m long, 1.525 m wide, 0.76 m high.
    width, length, top = 1.525, 2.74, 0.76
    for sign in (-1, 1):
        box(f"Playing surface half {sign}", (0, sign * (length / 4 + 0.001), top - 0.013),
            (width, length / 2 - 0.002, 0.026), green, 0.004)
    for x in (-width / 2 + 0.055, width / 2 - 0.055):
        box("Side apron | steel", (x, 0, top - 0.065), (0.045, length - 0.08, 0.08), metal)
    for y in (-length / 2 + 0.055, length / 2 - 0.055):
        box("End apron | steel", (0, y, top - 0.065), (width - 0.065, 0.045, 0.08), metal)
    for x in (-width / 2 + 0.02, width / 2 - 0.02):
        box("20 mm sideline", (x, 0, top + 0.00035), (0.02, length - 0.024, 0.0006), ivory, 0)
    for y in (-length / 2 + 0.02, length / 2 - 0.02):
        box("20 mm end line", (0, y, top + 0.0004), (width - 0.024, 0.02, 0.0006), ivory, 0)
    box("3 mm doubles centre line", (0, 0, top + 0.0004), (0.003, length - 0.024, 0.0006), ivory, 0)

    for y in (-0.92, 0.92):
        for x in (-0.57, 0.57):
            box("Steel square-tube leg", (x, y, 0.365), (0.05, 0.05, 0.66), metal, 0.008)
            box("Protective adjustable foot", (x, y, 0.039), (0.072, 0.075, 0.057), black, 0.012)
            cylinder("Height adjustment collar", (x, y, 0.06), (x, y, 0.11), 0.023, chrome)
        box("Cross-member", (0, y, 0.245), (1.15, 0.035, 0.035), metal)
        for sign in (-1, 1):
            cylinder("Diagonal undercarriage brace", (sign * 0.55, y, 0.26),
                     (sign * 0.14, y, 0.68), 0.013, metal)
    for x in (-0.57, 0.57):
        box("Longitudinal frame beam", (x, 0, 0.585), (0.025, 1.85, 0.03), metal)

    # Net is across y=0; both poles are OUTSIDE the 1.525 m-wide playing surface.
    post_x, net_top, net_bottom = width / 2 + 0.105, top + 0.1525, top + 0.003
    for sign in (-1, 1):
        x = sign * post_x
        box("External net clamp upper jaw", (sign * (width / 2 + 0.022), 0, top - 0.018),
            (0.18, 0.062, 0.026), metal, 0.006)
        box("External net clamp bracket", (x, 0, top - 0.055), (0.035, 0.06, 0.1), metal)
        box("External net clamp lower jaw", (sign * (width / 2 + 0.022), 0, top - 0.093),
            (0.18, 0.054, 0.018), metal)
        cylinder("Clamping screw", (sign * (width / 2 - 0.028), 0, top - 0.123),
                 (sign * (width / 2 - 0.028), 0, top - 0.074), 0.006, chrome)
        box("Clamp thumb screw", (sign * (width / 2 - 0.028), 0, top - 0.123),
            (0.037, 0.014, 0.012), black)
        cylinder("Net support post outside sideline", (x, 0, top - 0.065),
                 (x, 0, net_top + 0.012), 0.014, metal)
        cylinder("Silver net post cap", (x, 0, net_top + 0.009),
                 (x, 0, net_top + 0.015), 0.0145, chrome)

    span = post_x - 0.014
    sag = lambda x: 0.0035 * (1 - (x / span) ** 2)
    threads = []
    for i in range(174):
        x = -span + 2 * span * i / 173
        threads.append([(x, 0, net_bottom), (x, 0, net_top - sag(x))])
    for i in range(16):
        z = net_bottom + (net_top - net_bottom) * i / 15
        threads.append([(x, 0.0006, z - sag(x) * i / 15)
                        for x in [-span + 2 * span * j / 64 for j in range(65)]])
    curve_bundle("Physical woven net | 10 mm mesh", threads, 0.00065, net_mat)
    vertices, faces = [], []
    for i in range(65):
        x = -span + 2 * span * i / 64
        z = net_top - sag(x)
        vertices.extend([(x, -0.0018, z - 0.012), (x, -0.0018, z)])
        if i:
            j = 2 * i
            faces.append((j - 2, j, j + 1, j - 1))
    mesh = bpy.data.meshes.new("Net tape ribbon")
    mesh.from_pydata(vertices, [], faces)
    ribbon = bpy.data.objects.new("Continuous ivory top binding | joins both posts", mesh)
    bpy.context.collection.objects.link(ribbon)
    ribbon.data.materials.append(tape_mat)
    solid = ribbon.modifiers.new("Woven fabric thickness", "SOLIDIFY")
    solid.thickness = 0.0036
    curve_bundle("Bottom net cord", [[(-span, 0, net_bottom), (span, 0, net_bottom)]], 0.001, tape_mat)

    # Life-size equipment. The ball props up the blade; the grip rests on the table.
    paddle_objects = []
    def keep(obj):
        paddle_objects.append(obj)
        return obj
    face = paddle_outline()
    blade = paddle_outline(with_handle=True)
    for ply in range(7):
        keep(silhouette(f"Continuous blade and neck ply {ply + 1}", blade,
                        0.004 + ply * 0.0008, 0.0008, maple, 0.0001))
    for z in (0.0052, 0.0076):
        keep(silhouette("Thin carbon reinforcement", blade, z, 0.00013, dark_ply, 0.00003))
    keep(silhouette("Lower black inverted rubber", face, 0.0018, 0.0028, underside, 0.0004))
    keep(silhouette("Upper sponge layer", face, 0.0101, 0.0014, sponge, 0.00025))
    keep(silhouette("Red inverted rubber face", face, 0.0115, 0.0014, red, 0.0004))
    keep(sculpted_grip("Pale ash upper grip | integrated shoulder", 0.012, 0.006, grip_mat))
    keep(sculpted_grip("Pale ash lower grip | integrated shoulder", -0.0015, 0.0055, grip_mat))
    parent = bpy.data.objects.new("PADDLE | ball-supported blade, grounded handle", None)
    bpy.context.collection.objects.link(parent)
    parent.location = (-0.135, -0.77, top)
    # Solve the blade/ball tangency and handle/table contact together.
    # The ball touches the underside just behind the front edge of the blade.
    radius = 0.02
    contact = Vector((0.060, 0.006, 0.0004))
    roll = math.radians(-15)
    points = [obj.matrix_basis @ vertex.co for obj in paddle_objects for vertex in obj.data.vertices]
    def support(angle):
        rotation = Matrix.Rotation(roll, 3, "Y") @ Matrix.Rotation(angle, 3, "X")
        height = -min((rotation @ point).z for point in points) + 0.00012
        gap = height + (rotation @ contact).z - radius * (1 + math.cos(roll) * math.cos(angle))
        return gap, height
    low, high = 0, math.radians(35)
    assert support(low)[0] < 0 < support(high)[0], "Ball support angle must have a physical solution"
    for _ in range(60):
        middle = (low + high) / 2
        if support(middle)[0] > 0:
            high = middle
        else:
            low = middle
    tilt = (low + high) / 2
    parent.location.z += support(tilt)[1]
    parent.rotation_euler = (tilt, roll, math.radians(-96))
    for obj in paddle_objects:
        obj.parent = parent

    bpy.context.view_layer.update()
    normal = parent.rotation_euler.to_matrix() @ Vector((0, 0, 1))
    ball_center = parent.matrix_world @ contact - radius * normal
    assert abs(ball_center.z - (top + radius)) < 0.00001
    print(f"Blade tilt {math.degrees(tilt):.2f} degrees; ball/table and ball/blade tangency solved", flush=True)
    ball_mat = material("Ivory matte ABS ball", (0.88, 0.85, 0.76), 0.42)
    ball_mat.node_tree.nodes.get("Principled BSDF").inputs["Subsurface Weight"].default_value = 0.035
    grain(ball_mat, 220, 0.06, 0.00008)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=96, ring_count=64, radius=radius,
                                       location=ball_center)
    ball = finish(bpy.context.object, "HERO BALL | seamless matte polymer", ball_mat)
    for polygon in ball.data.polygons:
        polygon.use_smooth = True

    light("Key | warm photographic softbox", (-0.8, -1.15, 1.8), (0, -0.75, 0.79), 34, 0.65, (1, 0.85, 0.72))
    light("Fill | cool photographic card", (0.85, -0.95, 1.3), (0, -0.75, 0.79), 6, 0.75, (0.7, 0.85, 1))
    light("Rim | narrow softbox", (-0.5, -0.1, 1.5), (0, -0.7, 0.8), 24, 0.65, (0.87, 1, 0.95), "RECTANGLE", 0.14)
    light("Warm arena bounce", (-1.1, 0.2, 1), (0, 0, 0.75), 6, 1.0, (1, 0.19, 0.09))
    light("Low white bounce | ball under blade", (-0.20, -1.10, 0.82), ball_center, 1.4, 0.3, (0.88, 0.94, 1))
    light("Low strip bounce | black rubber detail", (-0.14, -1.08, 0.772),
          (-0.135, -0.78, 0.79), 0.18, 0.22, (0.77, 0.88, 1), "RECTANGLE", 0.012)

    # A real arena backdrop, with soft lighting and no decorative lamp spheres.
    wall_mat = material("Dark warm arena wall", (0.045, 0.025, 0.016), 0.75)
    box("Arena backdrop", (0, 2.0, 1.8), (10, 0.1, 4), wall_mat)
    panel_mat = wood("Warm arena timber panels", (0.026, 0.01, 0.006), (0.13, 0.055, 0.025))
    for x in (0.45, 0.62, 0.79, 0.96, 1.13):
        box("Defocused timber wall panel", (x, 1.93, 1.3), (0.15, 0.04, 1.8), panel_mat)
    light("Amber wall wash", (0.8, 1.0, 1.35), (0.75, 2.0, 1.2), 65, 1.0, (1, 0.54, 0.24))

    hero = camera("01 HERO | low DSLR close-up", (0.0495, -1.3605, 0.793), (-0.172, -0.77, 0.812), 0.31)
    hero.data.type = "PERSP"
    hero.data.lens = 75
    hero.data.sensor_width = 36
    focus = bpy.data.objects.new("FOCUS | blade and supporting ball", None)
    bpy.context.collection.objects.link(focus)
    focus.location = (-0.142, -0.80, 0.794)
    hero.data.dof.use_dof = True
    hero.data.dof.focus_object = focus
    hero.data.dof.aperture_fstop = 6.3
    hero.data.dof.aperture_blades = 9
    camera("02 CHECK | opposite side", (-3.8, 5.5, 4.6), (0, 0, 0.45), 4.7)
    camera("03 CHECK | plan view", (0, 0, 7), (0, 0, 0), 4.15)
    camera("04 CHECK | blade and ball contact", (-0.48, -1.05, 0.82), (-0.20, -0.79, 0.79), 0.32)
    top_target = parent.matrix_world @ Vector((0, -0.048, 0))
    top_position = top_target + parent.rotation_euler.to_matrix() @ Vector((0, 0, 0.6))
    check = camera("05 CHECK | reference paddle silhouette", top_position, top_target, 0.31)
    check.rotation_euler = parent.rotation_euler
    bpy.context.scene.camera = hero


def configure(final):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 512 if final else 64
    scene.cycles.use_denoising = True
    scene.cycles.adaptive_threshold = 0.006 if final else 0.035
    scene.cycles.max_bounces = 8
    scene.render.resolution_x = scene.render.resolution_y = 2048 if final else 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.film_transparent = True
    scene.world.color = (0.16, 0.16, 0.16)
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.4, 0.48, 0.58, 1)
    background.inputs["Strength"].default_value = 0.07
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Medium High Contrast"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    try:
        prefs.compute_device_type = "METAL"
        prefs.get_devices()
        gpu = [device for device in prefs.devices if device.type == "METAL"]
        if gpu:
            for device in prefs.devices:
                device.use = device.type == "METAL"
            scene.cycles.device = "GPU"
            print("Rendering with Metal:", [device.name for device in gpu], flush=True)
    except (TypeError, RuntimeError):
        print("Metal unavailable; using Cycles CPU", flush=True)


def compositor():
    """Native Blender finishing: inset the photograph and add rounded alpha corners."""
    scene = bpy.context.scene
    size = scene.render.resolution_x
    yy, xx = np.mgrid[:size, :size].astype(np.float32)
    x = (xx + 0.5) / size - 0.5
    y = (yy + 0.5) / size - 0.5
    radius, half_width = 0.15, 0.455
    qx, qy = np.abs(x) - (half_width - radius), np.abs(y) - (half_width - radius)
    distance = np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2)
    distance += np.minimum(np.maximum(qx, qy), 0) - radius
    pixels = np.ones((size, size, 4), dtype=np.float32)
    pixels[:, :, 3] = np.clip(0.5 - distance * size, 0, 1)
    mask = bpy.data.images.new("Rounded icon alpha | generated geometric mask", size, size, alpha=True)
    mask.pixels.foreach_set(pixels.ravel())
    mask.pack()
    tree = bpy.data.node_groups.new("Icon finishing | rounded crop and transparent margin", "CompositorNodeTree")
    tree.interface.new_socket(name="Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    scene.compositing_node_group = tree
    source = tree.nodes.new("CompositorNodeRLayers")
    source.location = (-400, 120)
    transform = tree.nodes.new("CompositorNodeTransform")
    transform.inputs["Scale"].default_value = 0.91
    transform.location = (-180, 120)
    image = tree.nodes.new("CompositorNodeImage")
    image.image = mask
    image.location = (-180, -120)
    alpha = tree.nodes.new("CompositorNodeSetAlpha")
    alpha.inputs["Type"].default_value = "Apply Mask"
    alpha.location = (50, 120)
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (270, 120)
    tree.links.new(source.outputs["Image"], transform.inputs["Image"])
    tree.links.new(transform.outputs["Image"], alpha.inputs["Image"])
    tree.links.new(image.outputs["Alpha"], alpha.inputs["Alpha"])
    tree.links.new(alpha.outputs["Image"], output.inputs["Image"])


def save_portable(path):
    """Save the scene, and set up later renders, without the author's absolute paths.

    Blender stamps the .blend path into PNG metadata, keeps an absolute source path
    on packed images even after make_paths_relative, and remembers the last file
    browser directory in a fixed buffer that a shorter path only partly overwrites.
    """
    scene = bpy.context.scene
    for name in dir(scene.render):
        if name.startswith("use_stamp"):
            setattr(scene.render, name, False)
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    bpy.ops.file.make_paths_relative()
    for image in bpy.data.images:
        if image.packed_file and image.filepath:
            data = image.packed_file.data
            image.unpack(method="REMOVE")
            image.pack(data=data, data_len=len(data))
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == "FILE_BROWSER":
                area.spaces.active.params.directory = b"/" * 1000
                area.spaces.active.params.directory = b"//"
    bpy.ops.wm.save_mainfile()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--hero-only", action="store_true", help="Render only the hero camera during previews")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    OUT.mkdir(parents=True, exist_ok=True)
    build_scene()
    configure(args.final)
    compositor()
    scene = bpy.context.scene
    hero = scene.camera
    scene.render.filepath = "//tabletennis_icon_3d_master.png"
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.region_3d.view_perspective = "CAMERA"
    save_portable(ASSETS / "tabletennis_icon.blend")
    if args.final:
        bpy.ops.render.render(write_still=True)
    else:
        for name, filename in [(hero.name, "hero-preview.png"),
                               ("02 CHECK | opposite side", "opposite-preview.png"),
                               ("03 CHECK | plan view", "plan-preview.png"),
                               ("04 CHECK | blade and ball contact", "contact-preview.png"),
                               ("05 CHECK | reference paddle silhouette", "paddle-preview.png")]:
            if args.hero_only and name != hero.name:
                continue
            scene.camera = bpy.data.objects[name]
            scene.render.filepath = str(OUT / filename)
            bpy.ops.render.render(write_still=True)
        scene.camera = hero


if __name__ == "__main__":
    main()
