# Renders a glTF model in gray with Blender's Workbench, to compare shading
# before and after `normals` (docs/normals.md#results). Read-only on the model.
#
#   blender -b -P tools/experiments/normals/render.py -- MODEL OUT.png [AZIMUTH ELEVATION ZOOM]
#
# The camera looks at the model's bounding box from AZIMUTH and ELEVATION
# degrees (default 35 and 25), ZOOM times closer than the whole box (default 1).
import math
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
model, out = args[0], args[1]
azimuth, elevation, zoom = (float(a) for a in (args[2:5] + ["35", "25", "1"][len(args[2:5]):]))

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=model)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
corners = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
low = Vector((min(c[i] for c in corners) for i in range(3)))
high = Vector((max(c[i] for c in corners) for i in range(3)))
center, radius = (low + high) / 2, (high - low).length / 2

a, e = math.radians(azimuth), math.radians(elevation)
direction = Vector((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))
camera = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
bpy.context.scene.collection.objects.link(camera)
camera.data.lens = 50
camera.location = center + direction * (radius * 2.4 / zoom)
camera.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()
camera.data.clip_end = radius * 10
bpy.context.scene.camera = camera

scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "SINGLE"
scene.display.shading.single_color = (0.8, 0.8, 0.8)
scene.render.resolution_x, scene.render.resolution_y = 1200, 900
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
