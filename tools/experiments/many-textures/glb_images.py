# Writes images of a GLB to OUT_DIR, by index, without reading the rest of the file:
#   python3 -I glb_images.py MODEL.glb OUT_DIR 0,18,50
import json, struct, sys
p, out, idx = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',')]
f = open(p, 'rb'); h = f.read(20)
jl = struct.unpack_from('<I', h, 12)[0]
j = json.loads(f.read(jl)); bo = 20 + jl + 8
for i in idx:
    im = j['images'][i]; bv = j['bufferViews'][im['bufferView']]
    ext = {'image/ktx2':'ktx2','image/png':'png'}.get(im['mimeType'],'jpg')
    f.seek(bo + bv.get('byteOffset', 0)); name = f"{out}/{i}.{ext}"
    open(name, 'wb').write(f.read(bv['byteLength'])); print(name)
