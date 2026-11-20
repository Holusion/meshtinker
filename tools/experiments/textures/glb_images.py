import json, struct, sys
p, out = sys.argv[1], sys.argv[2]
d = open(p, 'rb').read()
jl = struct.unpack_from('<I', d, 12)[0]
j = json.loads(d[20:20+jl]); bo = 20 + jl + 8
slots = {}
for m in j.get('materials', []):
    for k, v in list(m.get('pbrMetallicRoughness', {}).items()) + list(m.items()):
        if isinstance(v, dict) and 'index' in v:
            slots[j['textures'][v['index']]['source']] = k
for i, im in enumerate(j['images']):
    bv = j['bufferViews'][im['bufferView']]
    ext = 'png' if im['mimeType'] == 'image/png' else 'jpg'
    name = f"{out}/helmet-{slots.get(i, 'img'+str(i))}.{ext}"
    open(name, 'wb').write(d[bo + bv.get('byteOffset', 0): bo + bv.get('byteOffset', 0) + bv['byteLength']])
    print(name)
