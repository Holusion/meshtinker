# Renders a glTF model with several sets of textures, for comparing texture encodings on the
# rendered model rather than on the textures (docs/experiments/webp-avif-rendered.md).
# Read-only on the model.
#
#   blender -b -P tools/experiments/textures/render_variants.py -- MODEL VARIANTS_DIR OUT_DIR [VARIANT...]
#
# VARIANTS_DIR holds one directory per variant, each with a PNG per image of the model, named
# after the image's URI (`baseColor.jpg` -> `baseColor.jpg.png`). A variant missing an image
# keeps the model's own. The variant "original" renders the model's own images.
# Writes OUT_DIR/VARIANT/VIEW.png, with a transparent background, for the views below.
#
# EEVEE, the glTF importer's materials (base color, normal, metallic-roughness, occlusion is not
# rendered by Blender), a sun and an even world light, "Standard" view transform so that texture
# colours come out as stored. Views look at the bounding box's center: the whole model, and
# close-ups made with a longer lens from the same place, where a texel covers about a pixel.
import math
import os
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
model, variants_dir, out_dir = args[0], args[1], args[2]
variants = args[3:] or ["original"] + sorted(os.listdir(variants_dir))

VIEWS = [  # name, azimuth, elevation, zoom (focal length multiplier)
    ("full", 35, 25, 1),
    ("near", 35, 25, 3),
    ("close", 35, 25, 6),
    ("back-near", 215, 15, 3),
]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=model)
scene = bpy.context.scene
meshes = [o for o in scene.objects if o.type == "MESH"]
corners = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
low = Vector([min(c[i] for c in corners) for i in range(3)])
high = Vector([max(c[i] for c in corners) for i in range(3)])
center, radius = (low + high) / 2, (high - low).length / 2

camera = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
scene.collection.objects.link(camera)
scene.camera = camera
camera.data.clip_start = radius * 0.01
camera.data.clip_end = radius * 10

sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.data.energy = 4
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(sun)
world = bpy.data.worlds.new("world")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (1, 1, 1, 1)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1
scene.world = world

scene.render.engine = "BLENDER_EEVEE"
scene.eevee.taa_render_samples = 16
scene.render.film_transparent = True
scene.view_settings.view_transform = "Standard"
scene.render.resolution_x, scene.render.resolution_y = 1024, 768
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"

# The importer packs images, so a variant's images are loaded as new images and put in the
# texture nodes in place of the model's, which stay untouched.
nodes = {}
for material in bpy.data.materials:
    for node in material.node_tree.nodes if material.node_tree else []:
        if node.type == "TEX_IMAGE" and node.image and node.image.filepath:
            nodes.setdefault(node.image, []).append(node)
loaded = []
for variant in variants:
    for image in loaded:
        bpy.data.images.remove(image)
    loaded = []
    for image, users in nodes.items():
        path = os.path.join(variants_dir, variant, os.path.basename(bpy.path.abspath(image.filepath)) + ".png")
        replacement = image
        if variant != "original" and os.path.exists(path):
            replacement = bpy.data.images.load(path)
            replacement.colorspace_settings.name = image.colorspace_settings.name
            replacement.alpha_mode = image.alpha_mode
            loaded.append(replacement)
        for node in users:
            node.image = replacement
    for name, azimuth, elevation, zoom in VIEWS:
        a, e = math.radians(azimuth), math.radians(elevation)
        direction = Vector((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))
        camera.location = center + direction * radius * 2.4
        camera.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()
        camera.data.lens = 50 * zoom
        scene.render.filepath = os.path.join(out_dir, variant, name + ".png")
        bpy.ops.render.render(write_still=True)
