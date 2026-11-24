# T4's prototype (docs/experiments/many-textures.md): every UV island of every object packed into one shared
# page, each material's base color baked into it, and the model written with one material:
#   blender -b --factory-startup --python atlas.py -- MODEL.gltf OUT_DIR SIDE [FILL]
# SIDE is the page's side in texels. The islands are scaled to one texel density (Blender's Average Islands Scale,
# by 3D area) and packed without rotation; FILL (default 0.75) is only reported, as the share of the page the
# islands cover. Base colors are baked as emission from each material's own image and UVs, so no light is added.
# Only base color is kept: the three models have nothing else. Writes OUT_DIR/model.gltf with atlas.png.
import bpy, bmesh, sys, os, time
args = sys.argv[sys.argv.index('--') + 1:]
path, out, side = args[0], args[1], int(args[2])
start = time.time()
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
print(f"imported {len(meshes)} objects in {time.time() - start:.0f} s", flush=True)

# One object, so that the packer sees every island: joining keeps each face's material and its "UVMap".
bpy.ops.object.select_all(action='DESELECT')
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
ob = bpy.context.view_layer.objects.active
source_uv = ob.data.uv_layers[0].name
atlas_uv = ob.data.uv_layers.new(name="atlas")
ob.data.uv_layers.active = atlas_uv
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.select_all(action='SELECT')
bpy.ops.uv.average_islands_scale()
t = time.time()
bpy.ops.uv.pack_islands(rotate=False, margin=2.0 / side, margin_method='FRACTION')
print(f"packed in {time.time() - t:.0f} s", flush=True)
bpy.ops.object.mode_set(mode='OBJECT')

# Coverage of the page by the packed islands.
bm = bmesh.new(); bm.from_mesh(ob.data)
layer = bm.loops.layers.uv["atlas"]
covered = 0.0
for f in bm.faces:
    uv = [l[layer].uv for l in f.loops]
    for i in range(1, len(uv) - 1):
        covered += abs((uv[i].x - uv[0].x) * (uv[i + 1].y - uv[0].y) - (uv[i + 1].x - uv[0].x) * (uv[i].y - uv[0].y)) / 2
bm.free()

page = bpy.data.images.new("atlas", side, side, alpha=False)
for material in ob.data.materials:
    nodes, links = material.node_tree.nodes, material.node_tree.links
    image_node = next((n for n in nodes if n.type == 'TEX_IMAGE'), None)
    output = next(n for n in nodes if n.type == 'OUTPUT_MATERIAL')
    if image_node is not None:
        uv_node = nodes.new('ShaderNodeUVMap'); uv_node.uv_map = source_uv
        links.new(uv_node.outputs['UV'], image_node.inputs['Vector'])
        emission = nodes.new('ShaderNodeEmission')
        links.new(image_node.outputs['Color'], emission.inputs['Color'])
        links.new(emission.outputs['Emission'], output.inputs['Surface'])
    target = nodes.new('ShaderNodeTexImage'); target.image = page
    nodes.active = target

scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.samples = 1
scene.cycles.device = 'CPU'
scene.render.bake.margin = 4
t = time.time()
bpy.ops.object.bake(type='EMIT', uv_layer="atlas", margin=4)
print(f"baked {side}x{side} in {time.time() - t:.0f} s", flush=True)
os.makedirs(out, exist_ok=True)
page.filepath_raw = os.path.join(out, "atlas.png")
page.file_format = 'PNG'
page.save()

# One material on the atlas, and the atlas as the only UV set.
material = bpy.data.materials.new("atlas")
material.use_nodes = True
principled = material.node_tree.nodes.get("Principled BSDF")
texture = material.node_tree.nodes.new('ShaderNodeTexImage'); texture.image = page
material.node_tree.links.new(texture.outputs['Color'], principled.inputs['Base Color'])
principled.inputs['Roughness'].default_value = 1.0
principled.inputs['Metallic'].default_value = 0.0
ob.data.materials.clear()
ob.data.materials.append(material)
for poly in ob.data.polygons:
    poly.material_index = 0
ob.data.uv_layers.remove(ob.data.uv_layers[source_uv])
ob.data.uv_layers["atlas"].name = "UVMap"
bpy.ops.export_scene.gltf(filepath=os.path.join(out, "model.gltf"), export_format='GLTF_SEPARATE',
                          export_image_format='AUTO', export_materials='EXPORT')
print(f"RESULT side={side} islands_cover={covered:.3f} seconds={time.time() - start:.0f}", flush=True)
