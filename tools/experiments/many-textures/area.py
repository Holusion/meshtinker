# Prints the surface area of a model in world units squared, node transforms applied:
#   blender -b --factory-startup --python area.py -- MODEL.gltf
# Blender 5.2 refuses KHR_texture_basisu: measure a copy without its textures (docs/experiments/many-textures.md).
import bpy, sys, bmesh
path = sys.argv[sys.argv.index('--') + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
area = 0.0
for ob in bpy.context.scene.objects:
    if ob.type != 'MESH': continue
    bm = bmesh.new(); bm.from_mesh(ob.data); bm.transform(ob.matrix_world)
    a = sum(f.calc_area() for f in bm.faces); area += a
    bm.free()
print(f"RESULT area_m2={area:.4f} objects={sum(1 for o in bpy.context.scene.objects if o.type=='MESH')}")
