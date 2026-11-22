#!/usr/bin/env node
// Generate the fixtures in test/data: tiny models, each covering one case listed in test/corpus.json.
//
//   node tools/fixtures/generate.mjs [NAME...]     all fixtures, or only the named ones
//
// Each fixture folder gets an `expected.json` with what is true by construction (counts, bounds,
// occupancy computed by hand below). Commit all of it; ctest reads these files and never needs Node.js.
// Output is deterministic: rerunning must not change any file. Requires `ktx` (KTX-Software) on PATH.
//
// Most fixtures are written as raw glTF JSON by the Builder below, for full control over the layout.
// Compressed ones (Draco, meshopt with quantization, quantization) go through gltf-transform, as real
// files do.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { Document } from '@gltf-transform/core';
import { KHRMeshQuantization } from '@gltf-transform/extensions';
import { draco, meshopt, quantize } from '@gltf-transform/functions';
import { MeshoptEncoder } from 'meshoptimizer';
import { getIO } from '../lib/io.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../..');
const dataDir = path.join(repo, 'test/data');

const FLOAT = 5126, USHORT = 5123, UINT = 5125, UBYTE = 5121;
const ARRAY_BUFFER = 34962, ELEMENT_ARRAY_BUFFER = 34963;
const SIZE = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
const NEAREST = 9728, LINEAR = 9729, REPEAT = 10497, CLAMP = 33071;

// ---------------------------------------------------------------------------------------------- images

/** RGBA test pattern: four colored quadrants (orientation is visible in renders) plus a gradient. */
function pattern(width, height, hue = 0) {
  const px = Buffer.alloc(width * height * 4);
  const colors = [[230, 60, 60], [60, 200, 60], [60, 90, 230], [240, 220, 60]].map((c) => c.map((v, i) => (v + hue * (i + 1) * 37) % 256));
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const q = (x < width / 2 ? 0 : 1) + (y < height / 2 ? 0 : 2);
    const g = Math.round((x / Math.max(1, width - 1)) * 40);
    const o = (y * width + x) * 4;
    px[o] = Math.min(255, colors[q][0] + g); px[o + 1] = colors[q][1]; px[o + 2] = colors[q][2]; px[o + 3] = 255;
  }
  return px;
}

const raw = (w, h, hue) => sharp(pattern(w, h, hue), { raw: { width: w, height: h, channels: 4 } });
const png = (w, h, hue) => raw(w, h, hue).png({ compressionLevel: 9 }).toBuffer();
const jpeg = (w, h, hue) => raw(w, h, hue).jpeg({ quality: 90 }).toBuffer();
const webp = (w, h, hue) => raw(w, h, hue).webp({ lossless: true }).toBuffer();

async function ktx2(w, h, hue) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'meshtinker-ktx-'));
  try {
    fs.writeFileSync(path.join(tmp, 'in.png'), await png(w, h, hue));
    execFileSync('ktx', ['create', '--format', 'R8G8B8A8_SRGB', '--encode', 'basis-lz', '--assign-tf', 'srgb',
      path.join(tmp, 'in.png'), path.join(tmp, 'out.ktx2')], { stdio: ['ignore', 'ignore', 'pipe'] });
    return fs.readFileSync(path.join(tmp, 'out.ktx2'));
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

// --------------------------------------------------------------------------------------------- builder

const encodeUri = (name) => name.split('/').map(encodeURIComponent).join('/');

/** Raw glTF writer. Buffers hold appended chunks; images are placed at write time (files, GLB views or data URIs). */
class Builder {
  constructor() {
    this.json = { asset: { version: '2.0', generator: 'meshtinker test fixtures' } };
    this.chunks = [[]];
    this.lengths = [0];
    this.virtual = {}; // buffer index -> extensions, for buffers without data (meshopt fallback)
    this.images = []; // {name, bytes, mimeType}
  }

  add(kind, obj) {
    (this.json[kind] ??= []).push(obj);
    return this.json[kind].length - 1;
  }

  newBuffer() {
    this.chunks.push([]);
    this.lengths.push(0);
    return this.chunks.length - 1;
  }

  virtualBuffer(byteLength, extensions) {
    const i = this.newBuffer();
    this.lengths[i] = byteLength;
    this.virtual[i] = extensions;
    return i;
  }

  /** Appends bytes to a buffer, aligned; returns their byte offset. */
  append(bytes, { buffer = 0, align = 4 } = {}) {
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let length = this.lengths[buffer];
    const pad = (align - (length % align)) % align;
    if (pad) this.chunks[buffer].push(Buffer.alloc(pad));
    length += pad;
    this.chunks[buffer].push(Buffer.from(u8));
    this.lengths[buffer] = length + u8.byteLength;
    return length;
  }

  view(bytes, { buffer = 0, stride, target, align = 4 } = {}) {
    const byteOffset = this.append(bytes, { buffer, align });
    const v = { buffer, byteOffset, byteLength: bytes.byteLength };
    if (stride) v.byteStride = stride;
    if (target) v.target = target;
    return this.add('bufferViews', v);
  }

  accessor(bufferView, componentType, type, count, extra = {}) {
    const a = { componentType, count, type, ...extra };
    if (bufferView !== undefined && bufferView !== null) a.bufferView = bufferView;
    return this.add('accessors', a);
  }

  /** Float accessor in its own view; min/max always written (POSITION requires them). */
  floats(values, type, { buffer = 0, target = ARRAY_BUFFER } = {}) {
    const f = Float32Array.from(values);
    const n = SIZE[type];
    const min = Array(n).fill(Infinity), max = Array(n).fill(-Infinity);
    for (let i = 0; i < f.length; i++) {
      min[i % n] = Math.min(min[i % n], f[i]);
      max[i % n] = Math.max(max[i % n], f[i]);
    }
    return this.accessor(this.view(f, { buffer, target }), FLOAT, type, f.length / n, { min, max });
  }

  indices(values, { buffer = 0, componentType = USHORT } = {}) {
    const a = componentType === UINT ? Uint32Array.from(values) : componentType === UBYTE ? Uint8Array.from(values) : Uint16Array.from(values);
    return this.accessor(this.view(a, { buffer, target: ELEMENT_ARRAY_BUFFER }), componentType, 'SCALAR', a.length);
  }

  image(name, bytes, mimeType) {
    this.images.push({ name, bytes, mimeType });
    return this.add('images', { name: name.replace(/\.[^.]+$/, '') });
  }

  texture(source, sampler, extra = {}) {
    const t = { ...extra };
    if (source !== null && source !== undefined) t.source = source;
    if (sampler !== undefined) t.sampler = sampler;
    return this.add('textures', t);
  }

  /** Mesh with one primitive per entry, and a node for it. Returns {mesh, node}. */
  meshNode(primitives, node = {}, name) {
    const mesh = this.add('meshes', { ...(name ? { name } : {}), primitives });
    return { mesh, node: this.add('nodes', { ...(name ? { name } : {}), ...node, mesh }) };
  }

  scene(nodes, extra = {}) {
    return this.add('scenes', { nodes, ...extra });
  }

  /**
   * Writes the model. mode: 'gltf' (external buffers and images), 'glb', or 'data-uri' (everything embedded).
   * bufferNames: file names (decoded; URIs are percent-encoded) for buffers in 'gltf' mode.
   */
  write(file, { mode = 'gltf', bufferNames = [], imageMode } = {}) {
    const dir = path.dirname(file);
    const stem = path.basename(file).replace(/\.(gltf|glb)$/, '');
    fs.mkdirSync(dir, { recursive: true });
    const json = structuredClone(this.json);
    const images = imageMode ?? (mode === 'gltf' ? 'files' : mode === 'glb' ? 'views' : 'data-uri');
    this.images.forEach((img, i) => {
      if (images === 'files') {
        fs.writeFileSync(path.join(dir, img.name), img.bytes);
        json.images[i].uri = encodeUri(img.name);
      } else if (images === 'views') {
        const offset = this.append(img.bytes, { buffer: 0 });
        json.bufferViews ??= [];
        json.bufferViews.push({ buffer: 0, byteOffset: offset, byteLength: img.bytes.byteLength });
        json.images[i].bufferView = json.bufferViews.length - 1;
        json.images[i].mimeType = img.mimeType;
      } else {
        json.images[i].uri = `data:${img.mimeType};base64,${img.bytes.toString('base64')}`;
      }
    });
    json.buffers = this.chunks.map((chunks, i) => {
      if (this.virtual[i]) return { byteLength: this.lengths[i], extensions: this.virtual[i] };
      const data = Buffer.concat(chunks);
      const b = { byteLength: data.byteLength };
      if (mode === 'glb' && i === 0) return b;
      if (mode === 'data-uri') return { ...b, uri: `data:application/octet-stream;base64,${data.toString('base64')}` };
      const name = bufferNames[i] ?? (i === 0 ? `${stem}.bin` : `${stem}_${i}.bin`);
      fs.writeFileSync(path.join(dir, name), data);
      return { ...b, uri: encodeUri(name) };
    }).filter((b) => b.byteLength > 0 || b.extensions);
    if (!json.buffers.length) delete json.buffers;
    if (mode === 'glb') {
      const bin = Buffer.concat(this.chunks[0]);
      const binPadded = Buffer.concat([bin, Buffer.alloc((4 - (bin.length % 4)) % 4)]);
      const text = Buffer.from(JSON.stringify(json));
      const jsonPadded = Buffer.concat([text, Buffer.alloc((4 - (text.length % 4)) % 4, 0x20)]);
      const header = Buffer.alloc(12);
      header.writeUInt32LE(0x46546c67, 0);
      header.writeUInt32LE(2, 4);
      header.writeUInt32LE(12 + 8 + jsonPadded.length + 8 + binPadded.length, 8);
      const chunk = (type, data) => {
        const h = Buffer.alloc(8);
        h.writeUInt32LE(data.length, 0);
        h.writeUInt32LE(type, 4);
        return Buffer.concat([h, data]);
      };
      fs.writeFileSync(file, Buffer.concat([header, chunk(0x4e4f534a, jsonPadded), chunk(0x004e4942, binPadded)]));
    } else {
      fs.writeFileSync(file, JSON.stringify(json, null, 2) + '\n');
    }
  }
}

/** Quad in the XY plane from (x0,y0) to (x1,y1), with UVs from (u0,v0) to (u1,v1). Two triangles. */
function quad(b, { x0 = 0, y0 = 0, x1 = 1, y1 = 1, z = 0, uv = [0, 0, 1, 1], uv1 = null, buffer = 0 } = {}) {
  const [u0, v0, u1, v1] = uv;
  const attributes = {
    POSITION: b.floats([x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z], 'VEC3', { buffer }),
    NORMAL: b.floats([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1], 'VEC3', { buffer }),
    // glTF UV origin is the top-left corner: v grows downward, so the quad's top edge (y1) gets v0.
    TEXCOORD_0: b.floats([u0, v1, u1, v1, u1, v0, u0, v0], 'VEC2', { buffer }),
  };
  if (uv1) {
    const [a0, b0, a1, b1] = uv1;
    attributes.TEXCOORD_1 = b.floats([a0, b1, a1, b1, a1, b0, a0, b0], 'VEC2', { buffer });
  }
  return { attributes, indices: b.indices([0, 1, 2, 0, 2, 3], { buffer }) };
}

function writeExpected(dir, expected) {
  fs.writeFileSync(path.join(dir, 'expected.json'), JSON.stringify(expected, null, 2) + '\n');
}

// -------------------------------------------------------------------------------------------- fixtures

const fixtures = {};

fixtures.triangle = (dir) => {
  const b = new Builder();
  const { node } = b.meshNode([{
    attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 0, 1, 0], 'VEC3'), NORMAL: b.floats([0, 0, 1, 0, 0, 1, 0, 0, 1], 'VEC3') },
    indices: b.indices([0, 1, 2]),
  }], {}, 'Triangle');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'triangle.gltf'));
  writeExpected(dir, { description: 'One indexed triangle, external .bin.', geometry: { primitives: 1, vertices: 3, faces: 1 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] }, firstNamedRootNode: 'Triangle' });
};

fixtures['triangle-glb'] = (dir) => {
  const b = new Builder();
  const { node } = b.meshNode([{
    attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 0, 1, 0], 'VEC3') },
    indices: b.indices([0, 1, 2]),
  }], {}, 'Triangle');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'triangle.glb'), { mode: 'glb' });
  writeExpected(dir, { description: 'One indexed triangle without normals, as GLB.', geometry: { primitives: 1, vertices: 3, faces: 1 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] } });
};

fixtures['textured-glb'] = async (dir) => {
  const b = new Builder();
  const img = b.image('checker.png', await png(32, 32), 'image/png');
  const material = b.add('materials', { name: 'Textured', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(img) } } });
  const { node } = b.meshNode([{ ...quad(b), material }], {}, 'Quad');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'textured.glb'), { mode: 'glb' });
  writeExpected(dir, { description: 'Textured quad; the PNG is stored in the GLB binary chunk.', geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] }, images: [{ name: 'checker', mimeType: 'image/png', width: 32, height: 32, occupancy: 1 }] });
};

fixtures['data-uri'] = async (dir) => {
  const b = new Builder();
  const img = b.image('checker.png', await png(16, 16, 1), 'image/png');
  const material = b.add('materials', { pbrMetallicRoughness: { baseColorTexture: { index: b.texture(img) } } });
  const { node } = b.meshNode([{ ...quad(b), material }]);
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'data-uri.gltf'), { mode: 'data-uri' });
  writeExpected(dir, { description: 'Buffer and image embedded as base64 data URIs.', geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] }, images: [{ name: 'checker', mimeType: 'image/png', width: 16, height: 16, occupancy: 1 }] });
};

fixtures['uri-escaping'] = async (dir) => {
  const b = new Builder();
  const second = b.newBuffer();
  const jpg = b.image('base color ♻.jpg', await jpeg(16, 16, 2), 'image/jpeg');
  const pngImg = b.image('100% #1.png', await png(16, 16, 3), 'image/png');
  const m0 = b.add('materials', { pbrMetallicRoughness: { baseColorTexture: { index: b.texture(jpg) } } });
  const m1 = b.add('materials', { pbrMetallicRoughness: { baseColorTexture: { index: b.texture(pngImg) } } });
  const a = b.meshNode([{ ...quad(b), material: m0 }], {}, 'First');
  const c = b.meshNode([{ ...quad(b, { x0: 2, x1: 3, buffer: second }), material: m1 }], {}, 'Second');
  b.json.scene = b.scene([a.node, c.node]);
  b.write(path.join(dir, 'uri escaping.gltf'), { bufferNames: ['geo metry ❤.bin', 'second #2.bin'] });
  writeExpected(dir, {
    description: 'Two buffers and two images whose file names need percent-encoding (space, %, #, non-ASCII). The model file name has a space.',
    geometry: { primitives: 2, vertices: 8, faces: 4 }, bounds: { min: [0, 0, 0], max: [3, 1, 0] },
    images: [{ name: 'base color ♻', mimeType: 'image/jpeg', width: 16, height: 16, occupancy: 1 }, { name: '100% #1', mimeType: 'image/png', width: 16, height: 16, occupancy: 1 }],
  });
};

fixtures.shared = (dir) => {
  const b = new Builder();
  // Mesh "Square" (unit square in XY), used by two nodes.
  const square = b.add('meshes', { name: 'Square', primitives: [{
    attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], 'VEC3') }, indices: b.indices([0, 1, 2, 0, 2, 3]),
  }] });
  // Mesh "Split": two primitives sharing one POSITION accessor, each with its own indices and material.
  const position = b.floats([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], 'VEC3');
  const red = b.add('materials', { name: 'Red', pbrMetallicRoughness: { baseColorFactor: [1, 0, 0, 1] } });
  const blue = b.add('materials', { name: 'Blue', pbrMetallicRoughness: { baseColorFactor: [0, 0, 1, 1] } });
  const split = b.add('meshes', { name: 'Split', primitives: [
    { attributes: { POSITION: position }, indices: b.indices([0, 1, 2]), material: red },
    { attributes: { POSITION: position }, indices: b.indices([0, 2, 3]), material: blue },
  ] });
  const s = Math.SQRT1_2;
  const nodeA = b.add('nodes', { name: 'Translated', mesh: square, translation: [10, 0, 0] });
  const nodeB = b.add('nodes', { name: 'NegativeScale', mesh: square, scale: [-1, 2, 1] });
  const parent = b.add('nodes', { name: 'Rotated', rotation: [0, s, 0, s], children: [nodeB] });
  const nodeC = b.add('nodes', { name: 'SplitNode', mesh: split, translation: [0, -5, 0] });
  b.json.scene = b.scene([nodeA, parent, nodeC]);
  b.write(path.join(dir, 'shared.gltf'));
  writeExpected(dir, {
    description: 'Mesh "Square" used by two nodes (one under a 90° Y rotation with a negative scale); mesh "Split" has two primitives sharing a POSITION accessor.',
    geometry: { meshes: 2, primitives: 3, vertices: 8, faces: 4 },
    bounds: { min: [0, -5, 0], max: [11, 2, 1], tolerance: 1e-6 },
    firstNamedRootNode: 'Translated',
    notes: ['Each mesh counts once, and the shared POSITION accessor counts once: 4 + 4 vertices.',
      'Bounds: Translated gives x 10..11, y 0..1; Rotated(NegativeScale) maps x -1..0 to z 0..1 and y to 0..2; SplitNode gives y -5..-4.'],
  });
};

fixtures.modes = (dir) => {
  const b = new Builder();
  const pos = (n, y) => b.floats(Array.from({ length: n }, (_, i) => [i, y + (i % 2), 0]).flat(), 'VEC3');
  const { node } = b.meshNode([
    { mode: 0, attributes: { POSITION: pos(4, 0) } },
    { mode: 1, attributes: { POSITION: pos(4, 2) } },
    { mode: 2, attributes: { POSITION: pos(4, 4) } },
    { mode: 3, attributes: { POSITION: pos(4, 6) } },
    { mode: 4, attributes: { POSITION: pos(6, 8) } },
    { mode: 5, attributes: { POSITION: pos(5, 10) }, indices: b.indices([0, 1, 2, 3, 4]) },
    { mode: 6, attributes: { POSITION: pos(5, 12) } },
  ], {}, 'Modes');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'modes.gltf'));
  writeExpected(dir, {
    description: 'One primitive per mode: points, lines, line loop, line strip, non-indexed triangles, indexed strip, non-indexed fan.',
    geometry: { primitives: 7, vertices: 32, faces: 8 },
    bounds: { min: [0, 0, 0], max: [5, 13, 0] },
    notes: ['Faces: triangles 6/3 = 2, strip 5 − 2 = 3, fan 5 − 2 = 3; points and lines none.'],
  });
};

fixtures.scenes = (dir) => {
  const b = new Builder();
  const first = b.meshNode([{ attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 0, 1, 0], 'VEC3') } }], {}, 'InFirstScene');
  const second = b.meshNode([quad(b, { x0: 5, x1: 6 })], {}, 'InSecondScene');
  b.add('meshes', { name: 'Orphan', primitives: [{ attributes: { POSITION: b.floats([0, 0, 0, 9, 0, 0, 0, 9, 0], 'VEC3') } }] });
  b.add('nodes', { name: 'OrphanNode', mesh: 0 });
  b.scene([first.node], { name: 'First' });
  b.json.scene = b.scene([second.node], { name: 'Second' });
  b.write(path.join(dir, 'scenes.gltf'));
  writeExpected(dir, {
    description: 'Two scenes, the default being the second; plus a mesh and a node no scene uses.',
    scene: { index: 1, name: 'Second' },
    geometry: { meshes: 1, primitives: 1, vertices: 4, faces: 2 },
    bounds: { min: [5, 0, 0], max: [6, 1, 0] },
    notes: ['Only the default scene counts.'],
  });
};

fixtures['no-default-scene'] = (dir) => {
  const b = new Builder();
  const first = b.meshNode([quad(b)], {}, 'First');
  const second = b.meshNode([quad(b, { x0: 5, x1: 6 })], {}, 'Second');
  b.scene([first.node], { name: 'Zero' });
  b.scene([second.node], { name: 'One' });
  b.write(path.join(dir, 'no-default-scene.gltf'));
  writeExpected(dir, {
    description: 'Two scenes and no `scene` property: scene 0 is used, as three.js does.',
    scene: { index: 0, name: 'Zero' }, geometry: { meshes: 1, primitives: 1, vertices: 4, faces: 2 },
    bounds: { min: [0, 0, 0], max: [1, 1, 0] },
  });
};

fixtures.empty = (dir) => {
  const b = new Builder();
  const node = b.add('nodes', { name: 'Empty' });
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'empty.gltf'));
  writeExpected(dir, { description: 'A scene with one node and no geometry.', geometry: { meshes: 0, primitives: 0, vertices: 0, faces: 0 }, bounds: null });
};

fixtures.interleaved = (dir) => {
  const b = new Builder();
  // One interleaved view (stride 32): position, normal, uv for 8 vertices: two quads.
  const verts = [];
  for (const x of [0, 2]) {
    for (const [px, py, u, v] of [[0, 0, 0, 1], [1, 0, 1, 1], [1, 1, 1, 0], [0, 1, 0, 0]]) verts.push(px + x, py, 0, 0, 0, 1, u, v);
  }
  const view = b.view(Float32Array.from(verts), { stride: 32, target: ARRAY_BUFFER });
  const acc = (offset, type, count, first, extra) => b.accessor(view, FLOAT, type, count, { byteOffset: offset + first * 32, ...extra });
  const indices = b.indices([0, 1, 2, 0, 2, 3]);
  const { node } = b.meshNode([
    { attributes: { POSITION: acc(0, 'VEC3', 4, 0, { min: [0, 0, 0], max: [1, 1, 0] }), NORMAL: acc(12, 'VEC3', 4, 0), TEXCOORD_0: acc(24, 'VEC2', 4, 0) }, indices },
    { attributes: { POSITION: acc(0, 'VEC3', 4, 4, { min: [2, 0, 0], max: [3, 1, 0] }), NORMAL: acc(12, 'VEC3', 4, 4), TEXCOORD_0: acc(24, 'VEC2', 4, 4) }, indices },
  ], {}, 'Interleaved');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'interleaved.gltf'));
  writeExpected(dir, {
    description: 'Two primitives whose attributes interleave in one shared buffer view (stride 32), with a shared index accessor.',
    geometry: { primitives: 2, vertices: 8, faces: 4 }, bounds: { min: [0, 0, 0], max: [3, 1, 0] },
  });
};

fixtures.sparse = (dir) => {
  const b = new Builder();
  const base = b.view(Float32Array.from([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]), { target: ARRAY_BUFFER });
  const sparseIdx = b.view(Uint16Array.from([2]));
  const sparseVal = b.view(Float32Array.from([2, 2, 0]));
  const withBase = b.accessor(base, FLOAT, 'VEC3', 4, {
    min: [0, 0, 0], max: [2, 2, 0],
    sparse: { count: 1, indices: { bufferView: sparseIdx, componentType: USHORT }, values: { bufferView: sparseVal } },
  });
  const zeroIdx = b.view(Uint16Array.from([1, 2]));
  const zeroVal = b.view(Float32Array.from([5, 0, 0, 4, 1, 0]));
  const noBase = b.accessor(null, FLOAT, 'VEC3', 3, {
    min: [0, 0, 0], max: [5, 1, 0],
    sparse: { count: 2, indices: { bufferView: zeroIdx, componentType: USHORT }, values: { bufferView: zeroVal } },
  });
  const { node } = b.meshNode([
    { attributes: { POSITION: withBase }, indices: b.indices([0, 1, 2, 0, 2, 3]) },
    { attributes: { POSITION: noBase } },
  ], {}, 'Sparse');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'sparse.gltf'));
  writeExpected(dir, {
    description: 'A sparse POSITION accessor over a buffer view (vertex 2 moved to (2,2,0)), and one with no buffer view (zeros plus sparse values).',
    geometry: { primitives: 2, vertices: 7, faces: 3 }, bounds: { min: [0, 0, 0], max: [5, 2, 0] },
  });
};

fixtures.morph = (dir) => {
  const b = new Builder();
  const prim = quad(b);
  prim.targets = [{ POSITION: b.floats([0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 3], 'VEC3') }];
  const { node } = b.meshNode([prim], {}, 'Morph');
  b.json.meshes[0].weights = [0.5];
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'morph.gltf'));
  writeExpected(dir, {
    description: 'A quad with one POSITION morph target (lifts the top edge by 3 in z) and default weight 0.5.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    notes: ['Bounds ignore morph targets and weights, as gltf-transform getBounds does: z stays 0.'],
  });
};

fixtures.occupancy = async (dir) => {
  const b = new Builder();
  const nearest = b.add('samplers', { magFilter: NEAREST, minFilter: NEAREST, wrapS: CLAMP, wrapT: CLAMP });
  const linearClamp = b.add('samplers', { magFilter: LINEAR, minFilter: LINEAR, wrapS: CLAMP, wrapT: CLAMP });
  const linearRepeat = b.add('samplers', { magFilter: LINEAR, minFilter: LINEAR, wrapS: REPEAT, wrapT: REPEAT });
  const nodes = [];
  let x = 0;
  const place = (name, primitive) => {
    const { node } = b.meshNode([primitive], { translation: [x, 0, 0] }, name);
    nodes.push(node);
    x += 2;
  };
  const base = (name, image, sampler, uv, extra = {}) => {
    const material = b.add('materials', { name, pbrMetallicRoughness: { baseColorTexture: { index: b.texture(image, sampler), ...extra } } });
    place(name, { ...quad(b, { uv }), material });
  };
  base('Nearest', b.image('nearest.png', await png(64, 64, 0), 'image/png'), nearest, [0.1, 0.01, 0.4, 0.99]);
  base('Linear', b.image('linear.png', await png(64, 64, 1), 'image/png'), linearClamp, [0.1, 0.01, 0.4, 0.99]);
  base('Transform', b.image('transform.png', await png(16, 16, 2), 'image/png'), linearClamp, [0, 0, 1, 1],
    { extensions: { KHR_texture_transform: { offset: [0.25, 0.25], scale: [0.5, 0.5] } } });
  base('Repeat', b.image('repeat.png', await png(16, 16, 3), 'image/png'), linearRepeat, [0.9, 0.25, 1.1, 0.75]);
  // One image, two slots, two UV sets: base color through TEXCOORD_0, occlusion through TEXCOORD_1.
  const shared = b.image('shared.png', await png(32, 32, 4), 'image/png');
  const sharedTex = b.texture(shared, linearClamp);
  const sharedMat = b.add('materials', { name: 'TwoSlots', pbrMetallicRoughness: { baseColorTexture: { index: sharedTex } }, occlusionTexture: { index: sharedTex, texCoord: 1 } });
  place('TwoSlots', { ...quad(b, { uv: [0.05, 0.05, 0.2, 0.95], uv1: [0.55, 0.05, 0.7, 0.95] }), material: sharedMat });
  // Every core slot and one extension slot, each with its own 8×8 image covering the whole quad.
  const slot = async (name, hue) => ({ index: b.texture(b.image(`${name}.png`, await png(8, 8, hue), 'image/png'), linearClamp) });
  const allSlots = b.add('materials', {
    name: 'AllSlots',
    pbrMetallicRoughness: { baseColorTexture: await slot('slot-basecolor', 5), metallicRoughnessTexture: await slot('slot-metallicroughness', 6) },
    normalTexture: await slot('slot-normal', 7), occlusionTexture: await slot('slot-occlusion', 8), emissiveTexture: await slot('slot-emissive', 9),
    emissiveFactor: [1, 1, 1],
    extensions: { KHR_materials_clearcoat: { clearcoatFactor: 1, clearcoatTexture: await slot('slot-clearcoat', 10) } },
  });
  place('AllSlots', { ...quad(b), material: allSlots });
  b.image('unused.png', await png(8, 8, 11), 'image/png');
  b.json.extensionsUsed = ['KHR_texture_transform', 'KHR_materials_clearcoat'];
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'occupancy.gltf'));
  writeExpected(dir, {
    description: 'Texture occupancy cases with hand-computed answers (objtools definition: a texel counts when sampling some point reads it from the base level).',
    geometry: { primitives: 6, vertices: 24, faces: 12 }, bounds: { min: [0, 0, 0], max: [11, 1, 0] },
    images: [
      { name: 'nearest', width: 64, height: 64, occupancy: 0.3125, why: 'nearest: u in [6.4, 25.6] texels -> columns 6..25 (20 of 64); v in [0.64, 63.36] -> all rows' },
      { name: 'linear', width: 64, height: 64, occupancy: 0.34375, why: 'linear: texel i counts when |i + 0.5 - u| < 1 for some u -> columns 5..26 (22 of 64); all rows' },
      { name: 'transform', width: 16, height: 16, occupancy: 0.390625, why: 'KHR_texture_transform maps [0,1] to [0.25,0.75] -> [4,12] texels -> 3..12 (10×10 of 256)' },
      { name: 'repeat', width: 16, height: 16, occupancy: 0.234375, why: 'REPEAT: u in [14.4, 17.6] -> columns 13,14,15,0,1,2 (6); v in [4,12] -> rows 3..12 (10); 60 of 256' },
      { name: 'shared', width: 32, height: 32, occupancy: 0.3515625, why: 'union of two uses: TEXCOORD_0 u [1.6,6.4] -> columns 1..6, TEXCOORD_1 u [17.6,22.4] -> 17..22; v [1.6,30.4] -> rows 1..30; 12×30 of 1024' },
      ...['basecolor', 'metallicroughness', 'normal', 'occlusion', 'emissive', 'clearcoat'].map((s) => ({ name: `slot-${s}`, width: 8, height: 8, occupancy: 1 })),
      { name: 'unused', width: 8, height: 8, dropped: true, why: 'referenced by nothing: an orphan, which the profile drops' },
    ],
  });
};

// The image cases `ktx` and `uv` handle (docs/ktx.md#validation): alpha used, unused and ignored, a size
// that isn't a multiple of 4, a JPEG, a normal map, an ORM image in two data slots, a gray occlusion
// image, one image behind two color textures, and a 16-bit PNG.
fixtures.textures = async (dir) => {
  const b = new Builder();
  const withAlpha = (w, h, alpha) => {
    const px = pattern(w, h, 1);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) px[(y * w + x) * 4 + 3] = alpha(x, y);
    return sharp(px, { raw: { width: w, height: h, channels: 4 } }).png({ compressionLevel: 9 }).toBuffer();
  };
  const normals = () => {
    const px = Buffer.alloc(16 * 16 * 3);
    for (let i = 0; i < 256; i++) {
      const nx = ((i % 16) - 7.5) / 20, ny = (Math.floor(i / 16) - 7.5) / 20, nz = Math.sqrt(1 - nx * nx - ny * ny);
      [nx, ny, nz].forEach((v, c) => { px[i * 3 + c] = Math.round((v + 1) * 127.5); });
    }
    return sharp(px, { raw: { width: 16, height: 16, channels: 3 } }).png().toBuffer();
  };
  // Written by hand: sharp converts 16-bit raw input as a color space.
  const deep = () => {
    const chunk = (type, data) => {
      const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
      const out = Buffer.alloc(8 + data.length + 4);
      out.writeUInt32BE(data.length, 0);
      body.copy(out, 4);
      out.writeUInt32BE(zlib.crc32(body), 8 + data.length);
      return out;
    };
    const header = Buffer.alloc(13);
    header.writeUInt32BE(16, 0);
    header.writeUInt32BE(16, 4);
    header.set([16, 2, 0, 0, 0], 8); // 16 bits, RGB
    const rows = Buffer.alloc(16 * (1 + 16 * 6));
    // A smooth gradient, with low bytes that 8 bits drop.
    for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
      [x * 4096 + 1234, y * 4096 + 567, 40000].forEach((v, c) => rows.writeUInt16BE(v, y * (1 + 96) + 1 + x * 6 + c * 2));
    }
    return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', header),
      chunk('IDAT', zlib.deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]);
  };
  const images = [
    b.image('alpha.png', await withAlpha(30, 18, (x) => Math.round((x / 29) * 255)), 'image/png'),
    b.image('opaque.png', await withAlpha(16, 16, () => 255), 'image/png'),
    b.image('ignored-alpha.png', await withAlpha(16, 16, () => 128), 'image/png'),
    b.image('emissive.jpg', await jpeg(16, 16, 2), 'image/jpeg'),
    b.image('normal.png', await normals(), 'image/png'),
    b.image('orm.png', await png(16, 16, 3), 'image/png'),
    b.image('occlusion.png', await raw(16, 16, 4).removeAlpha().toColourspace('b-w').png().toBuffer(), 'image/png'),
    b.image('shared.png', await png(16, 16, 5), 'image/png'),
    b.image('deep.png', deep(), 'image/png'),
  ];
  const [alpha, opaque, ignored, emissive, normal, orm, occlusion, shared, sixteen] = images.map((image) => b.texture(image));
  const sharedAgain = b.texture(images[7]);
  const materials = [
    { name: 'Alpha', alphaMode: 'BLEND', pbrMetallicRoughness: { baseColorTexture: { index: alpha } } },
    { name: 'OpaqueAlpha', alphaMode: 'BLEND', pbrMetallicRoughness: { baseColorTexture: { index: opaque } } },
    { name: 'IgnoredAlpha', pbrMetallicRoughness: { baseColorTexture: { index: ignored } } },
    { name: 'Lit', emissiveFactor: [1, 1, 1], emissiveTexture: { index: emissive }, normalTexture: { index: normal },
      occlusionTexture: { index: orm }, pbrMetallicRoughness: { metallicRoughnessTexture: { index: orm } } },
    { name: 'Occluded', occlusionTexture: { index: occlusion } },
    { name: 'SharedBase', pbrMetallicRoughness: { baseColorTexture: { index: shared } } },
    { name: 'SharedEmissive', emissiveFactor: [1, 1, 1], emissiveTexture: { index: sharedAgain } },
    { name: 'Deep', pbrMetallicRoughness: { baseColorTexture: { index: sixteen } } },
  ];
  const nodes = materials.map((m, i) => b.meshNode([{ ...quad(b, { x0: 2 * i, x1: 2 * i + 1 }), material: b.add('materials', m) }], {}, m.name).node);
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'textures.gltf'));
  const image = (name, mimeType, width = 16, height = 16) => ({ name, mimeType, width, height, occupancy: 1 });
  writeExpected(dir, {
    description: 'Image cases for ktx and uv: alpha used, unused (all 255) and ignored (OPAQUE), a 30×18 size, a JPEG, a normal map, an ORM image in two data slots, a gray occlusion image, one image behind two color textures, a 16-bit PNG.',
    geometry: { primitives: 8, vertices: 32, faces: 16 },
    bounds: { min: [0, 0, 0], max: [15, 1, 0] },
    images: [image('alpha', 'image/png', 30, 18), image('opaque', 'image/png'), image('ignored-alpha', 'image/png'),
      image('emissive', 'image/jpeg'), image('normal', 'image/png'), image('orm', 'image/png'), image('occlusion', 'image/png'),
      image('shared', 'image/png'), image('deep', 'image/png')],
  });
};

// Charts spread over their textures, for uv: three charts in a 64×64 base color and a 32×32 normal map
// (one image half the other's size), and a second material sharing the base color image.
fixtures.atlas = async (dir) => {
  const b = new Builder();
  const color = b.image('atlas-color.png', await png(128, 128, 6), 'image/png');
  const normals = Buffer.alloc(64 * 64 * 3);
  for (let i = 0; i < 64 * 64; i++) {
    const nx = ((i % 64) - 31.5) / 80, ny = (Math.floor(i / 64) - 31.5) / 80, nz = Math.sqrt(1 - nx * nx - ny * ny);
    [nx, ny, nz].forEach((v, c) => { normals[i * 3 + c] = Math.round((v + 1) * 127.5); });
  }
  const normal = b.image('atlas-normal.png', await sharp(normals, { raw: { width: 64, height: 64, channels: 3 } }).png().toBuffer(), 'image/png');
  const charted = b.add('materials', { name: 'Charted', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(color) } },
    normalTexture: { index: b.texture(normal) } });
  const sharing = b.add('materials', { name: 'Sharing', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(color) } } });
  const charts = [[0.05, 0.05, 0.25, 0.3], [0.6, 0.1, 0.9, 0.35], [0.3, 0.7, 0.55, 0.95]];
  const primitives = charts.map((uv, i) => ({ ...quad(b, { x0: 2 * i, x1: 2 * i + 1, uv }), material: charted }));
  const { node: a } = b.meshNode(primitives, {}, 'Charted');
  const { node: c } = b.meshNode([{ ...quad(b, { x0: 6, x1: 7, uv: [0.7, 0.6, 0.95, 0.9] }), material: sharing }], {}, 'Sharing');
  b.json.scene = b.scene([a, c]);
  b.write(path.join(dir, 'atlas.gltf'));
  writeExpected(dir, {
    description: 'Three charts spread over a 128×128 base color and a 64×64 normal map, and a second material sharing the base color, for uv.',
    geometry: { primitives: 4, vertices: 16, faces: 8 },
    bounds: { min: [0, 0, 0], max: [7, 1, 0] },
    images: [{ name: 'atlas-color', mimeType: 'image/png', width: 128, height: 128 }, { name: 'atlas-normal', mimeType: 'image/png', width: 64, height: 64 }],
  });
};

fixtures.avif = async (dir) => {
  // Noisy 48×48 images, which PNG stores poorly and AVIF well, so that `avif`
  // encodes them; and a 4×4 one, which stays a smaller PNG.
  const b = new Builder();
  const random = prng(7);
  const noisy = (w, h, hue, channels = 4, alpha = () => 255) => {
    const px = pattern(w, h, hue);
    for (let i = 0; i < w * h; i++) {
      for (let c = 0; c < 3; c++) px[i * 4 + c] = Math.max(0, Math.min(255, px[i * 4 + c] + Math.round((random() - 0.5) * 60)));
      px[i * 4 + 3] = alpha(i % w, Math.floor(i / w));
    }
    let image = sharp(px, { raw: { width: w, height: h, channels: 4 } });
    if (channels === 1) image = image.removeAlpha().toColourspace('b-w');
    else if (channels === 3) image = image.removeAlpha();
    return image.png({ compressionLevel: 9 }).toBuffer();
  };
  const normals = () => {
    const px = Buffer.alloc(48 * 48 * 3);
    for (let i = 0; i < 48 * 48; i++) {
      const nx = ((i % 48) - 23.5) / 60 + (random() - 0.5) * 0.2, ny = (Math.floor(i / 48) - 23.5) / 60 + (random() - 0.5) * 0.2;
      const nz = Math.sqrt(Math.max(0, 1 - nx * nx - ny * ny));
      [nx, ny, nz].forEach((v, c) => { px[i * 3 + c] = Math.round((v + 1) * 127.5); });
    }
    return sharp(px, { raw: { width: 48, height: 48, channels: 3 } }).png({ compressionLevel: 9 }).toBuffer();
  };
  const existing = await raw(32, 32, 6).avif({ quality: 50 }).toBuffer();
  const images = [
    b.image('color.png', await noisy(48, 48, 1, 3), 'image/png'),
    b.image('alpha.png', await noisy(48, 48, 2, 4, (x) => Math.round((x / 47) * 255)), 'image/png'),
    b.image('normal.png', await normals(), 'image/png'),
    b.image('orm.png', await noisy(48, 48, 3, 3), 'image/png'),
    b.image('gray.png', await noisy(48, 48, 4, 1), 'image/png'),
    b.image('mixed.png', await noisy(48, 48, 5, 3), 'image/png'),
    b.image('tiny.png', await png(4, 4, 0), 'image/png'),
    b.image('fallback.png', await noisy(48, 48, 6, 3), 'image/png'),
    b.image('alternative.webp', await webp(32, 32, 0), 'image/webp'),
    b.image('ktx-only.ktx2', await ktx2(16, 16, 1), 'image/ktx2'),
    b.image('webp-only.webp', await webp(8, 8, 2), 'image/webp'),
    b.image('existing.avif', existing, 'image/avif'),
  ];
  const [color, alpha, normal, orm, gray, mixed, tiny, fallback, alternative, ktxOnly, webpOnly, avifOnly] = images;
  const t = (image, extensions) => b.texture(image, undefined, extensions ? { extensions } : undefined);
  const materials = [
    { name: 'Color', pbrMetallicRoughness: { baseColorTexture: { index: t(color) } } },
    { name: 'Alpha', alphaMode: 'BLEND', pbrMetallicRoughness: { baseColorTexture: { index: t(alpha) } } },
    { name: 'Data', normalTexture: { index: t(normal) }, occlusionTexture: { index: t(orm) },
      pbrMetallicRoughness: { metallicRoughnessTexture: { index: t(orm) } } },
    { name: 'Gray', occlusionTexture: { index: t(gray) } },
    { name: 'Mixed', pbrMetallicRoughness: { baseColorTexture: { index: t(mixed) }, metallicRoughnessTexture: { index: t(mixed) } } },
    { name: 'Tiny', pbrMetallicRoughness: { baseColorTexture: { index: t(tiny) } } },
    { name: 'PngWithWebp', pbrMetallicRoughness: { baseColorTexture: { index: t(fallback, { EXT_texture_webp: { source: alternative } }) } } },
    { name: 'KtxOnly', pbrMetallicRoughness: { baseColorTexture: { index: t(null, { KHR_texture_basisu: { source: ktxOnly } }) } } },
    { name: 'WebpOnly', pbrMetallicRoughness: { baseColorTexture: { index: t(null, { EXT_texture_webp: { source: webpOnly } }) } } },
    { name: 'AvifOnly', pbrMetallicRoughness: { baseColorTexture: { index: t(null, { EXT_texture_avif: { source: avifOnly } }) } } },
  ];
  // Two rows of five quads, so that the scene isn't a thin strip (placeholder's resolution).
  const nodes = materials.map((m, i) => {
    const [x0, y0] = [2 * (i % 5), 2 * Math.floor(i / 5)];
    return b.meshNode([{ ...quad(b, { x0, x1: x0 + 1, y0, y1: y0 + 1 }), material: b.add('materials', m) }], {}, m.name).node;
  });
  b.json.extensionsUsed = ['EXT_texture_webp', 'KHR_texture_basisu', 'EXT_texture_avif'];
  b.json.extensionsRequired = ['EXT_texture_webp', 'KHR_texture_basisu', 'EXT_texture_avif'];
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'avif.gltf'));
  const image = (name, mimeType, width = 48, height = 48) => ({ name, mimeType, width, height, used: true });
  writeExpected(dir, {
    description: 'Image cases for avif: noisy 48×48 color, alpha, normal, ORM, gray and color-and-data images that AVIF stores smaller than PNG; a 4×4 PNG it does not; a PNG with a WebP alternative, and KTX2-, WebP- and AVIF-only textures.',
    geometry: { primitives: 10, vertices: 40, faces: 20 },
    bounds: { min: [0, 0, 0], max: [9, 3, 0] },
    images: [image('color', 'image/png'), image('alpha', 'image/png'), image('normal', 'image/png'), image('orm', 'image/png'),
      image('gray', 'image/png'), image('mixed', 'image/png'), image('tiny', 'image/png', 4, 4), image('fallback', 'image/png'),
      { ...image('alternative', 'image/webp', 32, 32), used: false, why: 'the core source wins, with a texture-alternative-source diagnostic' },
      image('ktx-only', 'image/ktx2', 16, 16), image('webp-only', 'image/webp', 8, 8), image('existing', 'image/avif', 32, 32)],
    avif: {
      encoded: { 'color.png': '4:2:0', 'alpha.png': '4:2:0', 'normal.png': '4:4:4', 'orm.png': '4:4:4', 'gray.png': '4:0:0', 'mixed.png': '4:4:4', 'fallback.png': '4:2:0' },
      alpha: ['alpha.png'],
      notSmaller: ['tiny.png'],
      why: 'Color in 4:2:0, data and color-and-data in 4:4:4, gray in 4:0:0; only the BLEND material samples alpha; the 4×4 PNG is smaller than its AVIF.',
    },
    diagnostics: ['texture-alternative-source'],
  });
};

fixtures['alt-sources'] = async (dir) => {
  const b = new Builder();
  const fallbackPng = b.image('fallback.png', await png(16, 16, 0), 'image/png');
  const altWebp = b.image('alternative.webp', await webp(32, 32, 0), 'image/webp');
  const ktxOnly = b.image('ktx-only.ktx2', await ktx2(64, 64, 1), 'image/ktx2');
  const webpOnly = b.image('webp-only.webp', await webp(8, 8, 2), 'image/webp');
  const mat = (name, texture) => b.add('materials', { name, pbrMetallicRoughness: { baseColorTexture: { index: texture } } });
  const nodes = [
    b.meshNode([{ ...quad(b), material: mat('PngWithWebp', b.texture(fallbackPng, undefined, { extensions: { EXT_texture_webp: { source: altWebp } } })) }], {}, 'PngWithWebp').node,
    b.meshNode([{ ...quad(b, { x0: 2, x1: 3 }), material: mat('KtxOnly', b.texture(null, undefined, { extensions: { KHR_texture_basisu: { source: ktxOnly } } })) }], {}, 'KtxOnly').node,
    b.meshNode([{ ...quad(b, { x0: 4, x1: 5 }), material: mat('WebpOnly', b.texture(null, undefined, { extensions: { EXT_texture_webp: { source: webpOnly } } })) }], {}, 'WebpOnly').node,
  ];
  b.json.extensionsUsed = ['EXT_texture_webp', 'KHR_texture_basisu'];
  b.json.extensionsRequired = ['EXT_texture_webp', 'KHR_texture_basisu'];
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'alt-sources.gltf'));
  writeExpected(dir, {
    description: 'A PNG texture with a WebP alternative of a different size, a KTX2-only texture and a WebP-only texture.',
    geometry: { primitives: 3, vertices: 12, faces: 6 }, bounds: { min: [0, 0, 0], max: [5, 1, 0] },
    images: [
      { name: 'fallback', mimeType: 'image/png', width: 16, height: 16, used: true },
      { name: 'alternative', mimeType: 'image/webp', width: 32, height: 32, used: false, why: 'the core source wins, with a texture-alternative-source diagnostic' },
      { name: 'ktx-only', mimeType: 'image/ktx2', width: 64, height: 64, used: true },
      { name: 'webp-only', mimeType: 'image/webp', width: 8, height: 8, used: true },
    ],
    diagnostics: ['texture-alternative-source'],
  });
};

fixtures.udim = async (dir) => {
  // As photogrammetry and sculpting tools export UDIM tiles to glTF: one primitive and
  // one material per tile, the tile's UVs offset by whole units, REPEAT wrapping them onto the tile's images.
  const b = new Builder();
  const linearRepeat = b.add('samplers', { magFilter: LINEAR, minFilter: LINEAR, wrapS: REPEAT, wrapT: REPEAT });
  const tiles = [[1001, 0, 0], [1002, 1, 0], [1011, 0, 1]];
  const nodes = [];
  for (const [k, [tile, u, v]] of tiles.entries()) {
    const texture = async (slot, hue) => ({ index: b.texture(b.image(`${slot}.${tile}.png`, await png(16, 16, hue), 'image/png'), linearRepeat) });
    const material = b.add('materials', { name: `Tile.${tile}`, pbrMetallicRoughness: { baseColorTexture: await texture('basecolor', k) }, normalTexture: await texture('normal', k + 3) });
    nodes.push(b.meshNode([{ ...quad(b, { x0: 2 * k, x1: 2 * k + 1, uv: [u + 0.25, v + 0.25, u + 0.75, v + 0.75] }), material }], {}, `Tile.${tile}`).node);
  }
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'udim.gltf'));
  const why = 'REPEAT wraps the tile onto [0.25, 0.75] -> [4, 12] texels; linear -> columns and rows 3..12 (10×10 of 256)';
  writeExpected(dir, {
    description: 'Three UDIM tiles (1001, 1002, 1011): one quad and one material each, with UVs offset by whole units onto their own base color and normal images.',
    geometry: { primitives: 3, vertices: 12, faces: 6 }, bounds: { min: [0, 0, 0], max: [5, 1, 0] },
    images: tiles.flatMap(([tile]) => ['basecolor', 'normal'].map((slot) => ({ name: `${slot}.${tile}`, mimeType: 'image/png', width: 16, height: 16, occupancy: 0.390625, why }))),
  });
};

fixtures['spec-gloss'] = async (dir) => {
  // KHR_materials_pbrSpecularGlossiness is archived but still exported: its
  // textures are extension slots, and the material has no pbrMetallicRoughness.
  const b = new Builder();
  const linearClamp = b.add('samplers', { magFilter: LINEAR, minFilter: LINEAR, wrapS: CLAMP, wrapT: CLAMP });
  const diffuse = b.texture(b.image('diffuse.png', await png(16, 16, 0), 'image/png'), linearClamp);
  const specGloss = b.texture(b.image('specgloss.png', await png(8, 8, 1), 'image/png'), linearClamp);
  const material = b.add('materials', { name: 'SpecGloss', extensions: { KHR_materials_pbrSpecularGlossiness: {
    diffuseFactor: [1, 1, 1, 1], diffuseTexture: { index: diffuse }, specularFactor: [0.5, 0.5, 0.5], glossinessFactor: 0.8, specularGlossinessTexture: { index: specGloss },
  } } });
  const { node } = b.meshNode([{ ...quad(b), material }], {}, 'SpecGloss');
  b.json.extensionsUsed = ['KHR_materials_pbrSpecularGlossiness'];
  b.json.extensionsRequired = ['KHR_materials_pbrSpecularGlossiness'];
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'spec-gloss.gltf'));
  writeExpected(dir, {
    description: 'A quad with a required KHR_materials_pbrSpecularGlossiness material: diffuse and specular-glossiness textures, no metallic-roughness.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    images: [{ name: 'diffuse', mimeType: 'image/png', width: 16, height: 16, occupancy: 1 }, { name: 'specgloss', mimeType: 'image/png', width: 8, height: 8, occupancy: 1 }],
  });
};

fixtures.invalid = (dir) => {
  // Real files carry validator errors that readers ignore (non-normalized skin weights,
  // in a real scan). meshtinker must read them, take bounds from the data, and add no error of its own.
  const b = new Builder();
  const q = quad(b);
  b.json.accessors[q.attributes.POSITION].max = [2, 2, 0]; // ACCESSOR_MAX_MISMATCH: the data's max is [1, 1, 0]
  const joints = b.accessor(b.view(Uint8Array.from([0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0]), { target: ARRAY_BUFFER }), UBYTE, 'VEC4', 4);
  // ACCESSOR_JOINTS_WEIGHTS_NON_NORMALIZED: every vertex's weights add up to 0.8.
  const weights = b.accessor(b.view(Float32Array.from([0.5, 0.3, 0, 0, 0.5, 0.3, 0, 0, 0.4, 0.4, 0, 0, 0.4, 0.4, 0, 0]), { target: ARRAY_BUFFER }), FLOAT, 'VEC4', 4);
  const root = b.add('nodes', { name: 'Root' });
  const tip = b.add('nodes', { name: 'Tip', translation: [0, 1, 0] });
  b.json.nodes[root].children = [tip];
  const skin = b.add('skins', { joints: [root, tip] });
  const { node } = b.meshNode([{ attributes: { ...q.attributes, JOINTS_0: joints, WEIGHTS_0: weights }, indices: q.indices }], { skin }, 'Skinned');
  b.json.scene = b.scene([node, root]);
  b.write(path.join(dir, 'invalid.gltf'));
  writeExpected(dir, {
    description: 'A skinned quad whose POSITION max is wrong and whose skin weights add up to 0.8: two validator errors that readers ignore.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    notes: ['Bounds come from the vertex data in the rest pose, not from the accessor max [2, 2, 0].'],
  });
};

fixtures.skinned = (dir) => {
  // A Z-up skeleton under a root that turns it Y-up, as Blender exports it (BrainStem): a renderer places the skinned
  // quad by its joint's world matrix times its inverse bind matrix, which carries the root's rotation.
  const b = new Builder();
  const q = quad(b);
  const joints = b.accessor(b.view(Uint8Array.from(Array(16).fill(0)), { target: ARRAY_BUFFER }), UBYTE, 'VEC4', 4);
  const weights = b.accessor(b.view(Float32Array.from([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]), { target: ARRAY_BUFFER }), FLOAT, 'VEC4', 4);
  const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const inverseBindMatrices = b.accessor(b.view(Float32Array.from(identity)), FLOAT, 'MAT4', 1);
  const root = b.add('nodes', { name: 'Root', rotation: [-Math.SQRT1_2, 0, 0, Math.SQRT1_2] });
  const joint = b.add('nodes', { name: 'Joint' });
  const skin = b.add('skins', { joints: [joint], inverseBindMatrices });
  const { node } = b.meshNode([{ attributes: { ...q.attributes, JOINTS_0: joints, WEIGHTS_0: weights }, indices: q.indices }], { skin }, 'Skinned');
  b.json.nodes[root].children = [node, joint];
  b.json.scene = b.scene([root]);
  b.write(path.join(dir, 'skinned.gltf'));
  writeExpected(dir, {
    description: 'A skinned quad in the XY plane, its joint and its mesh node under a root rotated -90° about X: drawn in the XZ plane.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, -1], max: [1, 0, 0], tolerance: 1e-6 },
    notes: ['The joint has no transform and an identity inverse bind matrix, so the quad is drawn through the root\'s rotation, which the mesh node\'s world matrix also gives.',
      'placeholder: a skinned node is never the frame, so the placeholder is built in world coordinates, in the XZ plane.'],
  });
};

fixtures['rigid-hierarchy'] = (dir) => {
  // Three rigid mesh nodes: one with a child, one whose translation an animation sets, one alone. A command that
  // moves a mesh's dequantization into its node (compress) can fold it into the last one only.
  const b = new Builder();
  const mesh = (name) => b.add('meshes', { name, primitives: [quad(b)] });
  const child = b.add('nodes', { name: 'Animated', mesh: mesh('ChildQuad'), translation: [2, 0, 0] });
  const parent = b.add('nodes', { name: 'Parent', mesh: mesh('ParentQuad'), children: [child] });
  const plain = b.add('nodes', { name: 'Plain', mesh: mesh('PlainQuad'), translation: [0, 3, 0], scale: [2, 2, 2] });
  const times = b.accessor(b.view(Float32Array.from([0, 1])), FLOAT, 'SCALAR', 2);
  b.json.accessors[times].min = [0];
  b.json.accessors[times].max = [1];
  const values = b.accessor(b.view(Float32Array.from([2, 0, 0, 2, 1, 0])), FLOAT, 'VEC3', 2);
  b.add('animations', { name: 'Lift', samplers: [{ input: times, output: values }], channels: [{ sampler: 0, target: { node: child, path: 'translation' } }] });
  b.json.scene = b.scene([parent, plain]);
  b.write(path.join(dir, 'rigid-hierarchy.gltf'));
  writeExpected(dir, {
    description: 'Three unit quads on rigid nodes: Parent (with a child), Animated (its translation animated, from x = 2 up by 1) and Plain (translated to y = 3, scaled by 2).',
    geometry: { meshes: 3, primitives: 3, vertices: 12, faces: 6 }, bounds: { min: [0, 0, 0], max: [3, 5, 0] },
    notes: ['Bounds are in the rest pose: Animated at x = 2..3, Plain at x = 0..2, y = 3..5.'],
  });
};

/** Deterministic PRNG (mulberry32), so generated models are identical on every run. */
function prng(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * A synthetic photogrammetry scan: a bumpy open band around a sphere (scans have holes where the camera
 * didn't reach), cut into charts that each get a cell of one texture atlas, vertices duplicated along chart
 * seams, uint32 indices, and a node transform. Returns the builder and the world-space bounds of the data.
 * With `split`, each chart is its own primitive (`primitives`), as tiled scan exports write them: the
 * seams between charts become borders that two primitives share.
 */
function scanBuilder({ rings, segments, charts, seed, scale = 0.5, translation = [0, 1, 0], split = false, normals = true }) {
  const random = prng(seed);
  const waves = Array.from({ length: 6 }, () => ({ a: 0.02 + 0.04 * random(), m: 1 + Math.floor(7 * random()), n: 1 + Math.floor(7 * random()), p: 2 * Math.PI * random() }));
  const radius = (theta, phi) => 1 + waves.reduce((r, w) => r + w.a * Math.sin(w.m * theta + w.p) * Math.cos(w.n * phi), 0);
  const columns = Math.ceil(Math.sqrt(charts));
  const rows = Math.ceil(charts / columns);
  const perChart = segments / charts;
  const pos = [], nor = [], uv = [], idx = [];
  const chartRanges = [];
  for (let c = 0; c < charts; c++) {
    const first = pos.length / 3;
    const firstIndex = idx.length;
    const [cellU, cellV] = [(c % columns) / columns, Math.floor(c / columns) / rows];
    for (let r = 0; r <= rings; r++) for (let s = 0; s <= perChart; s++) {
      const theta = Math.PI * (0.15 + (0.7 * r) / rings); // 27° to 153° from the pole: an open band
      const phi = (2 * Math.PI * (c * perChart + s)) / segments;
      const d = [Math.sin(theta) * Math.cos(phi), Math.cos(theta), Math.sin(theta) * Math.sin(phi)];
      const k = radius(theta, phi);
      pos.push(...d.map((x) => Math.fround(k * x)));
      nor.push(...d);
      // Each chart fills 80% of its cell, leaving a gutter, as atlas packers do.
      uv.push(cellU + (0.1 + (0.8 * s) / perChart) / columns, cellV + (0.1 + (0.8 * r) / rings) / rows);
    }
    for (let r = 0; r < rings; r++) for (let s = 0; s < perChart; s++) {
      const a = first + r * (perChart + 1) + s;
      const below = a + perChart + 1;
      idx.push(a, below, a + 1, a + 1, below, below + 1);
    }
    chartRanges.push({ first, end: pos.length / 3, firstIndex, endIndex: idx.length });
  }
  const b = new Builder();
  const world = (i, axis) => scale * pos[3 * i + axis] + translation[axis];
  const count = pos.length / 3;
  const min = [0, 1, 2].map((axis) => Math.min(...Array.from({ length: count }, (_, i) => world(i, axis))));
  const max = [0, 1, 2].map((axis) => Math.max(...Array.from({ length: count }, (_, i) => world(i, axis))));
  const primitive = split ? null : {
    attributes: { POSITION: b.floats(pos, 'VEC3'), ...(normals ? { NORMAL: b.floats(nor, 'VEC3') } : {}), TEXCOORD_0: b.floats(uv, 'VEC2') },
    indices: b.indices(idx, { componentType: UINT }),
  };
  const primitives = !split ? null : chartRanges.map(({ first, end, firstIndex, endIndex }) => ({
    attributes: {
      POSITION: b.floats(pos.slice(3 * first, 3 * end), 'VEC3'),
      NORMAL: b.floats(nor.slice(3 * first, 3 * end), 'VEC3'),
      TEXCOORD_0: b.floats(uv.slice(2 * first, 2 * end), 'VEC2'),
    },
    indices: b.indices(idx.slice(firstIndex, endIndex).map((i) => i - first), { componentType: UINT }),
  }));
  return { b, primitive, primitives, transform: { scale: [scale, scale, scale], translation }, bounds: { min, max }, vertices: count, faces: idx.length / 3 };
}

fixtures.scan = async (dir) => {
  const { b, primitive, transform, bounds, vertices, faces } = scanBuilder({ rings: 16, segments: 32, charts: 4, seed: 1 });
  const atlas = b.image('atlas.jpg', await jpeg(128, 128, 0), 'image/jpeg');
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(atlas) }, metallicFactor: 0 } });
  const { node } = b.meshNode([{ ...primitive, material }], transform, 'Scan');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'scan.glb'), { mode: 'glb' });
  writeExpected(dir, {
    description: 'A small synthetic photogrammetry scan: a bumpy open band cut into 4 charts on one JPEG atlas, seams duplicated, uint32 indices, a node transform, as a GLB.',
    geometry: { meshes: 1, primitives: 1, vertices, faces }, bounds: { ...bounds, tolerance: 1e-6 },
    images: [{ name: 'atlas', mimeType: 'image/jpeg', width: 128, height: 128, occupancy: 0.7119140625,
      why: 'not computed by hand: objtools\' figure (11,664 of 16,384 texels), which meshtinker matched independently' }],
    notes: ['Bounds are the generator\'s float32 positions through the node transform.'],
  });
};

fixtures['scan-tiles'] = async (dir) => {
  const { b, primitives, transform, bounds, vertices, faces } = scanBuilder({ rings: 48, segments: 96, charts: 4, seed: 2, split: true });
  const atlas = b.image('atlas.jpg', await jpeg(128, 128, 0), 'image/jpeg');
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(atlas) }, metallicFactor: 0 } });
  const { node } = b.meshNode(primitives.map((p) => ({ ...p, material })), transform, 'Scan');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'scan-tiles.gltf'));
  writeExpected(dir, {
    description: 'The synthetic scan cut into 4 charts, each its own primitive with its own vertices, as tiled scan exports write them: the seams between charts are borders two primitives share, at the same positions.',
    geometry: { meshes: 1, primitives: 4, vertices, faces }, bounds: { ...bounds, tolerance: 1e-6 },
    images: [{ name: 'atlas', mimeType: 'image/jpeg', width: 128, height: 128 }],
    notes: ['Bounds are the generator\'s float32 positions through the node transform.', 'Occupancy is not stated: this fixture is for geometry.'],
  });
};

fixtures['scan-meshes'] = async (dir) => {
  // The tiles of scan-tiles as one mesh and one node each, all with the same transform, as some scan exports write
  // UDIM tiles: the borders are shared between meshes in one space.
  const { b, primitives, transform, bounds, vertices, faces } = scanBuilder({ rings: 48, segments: 96, charts: 4, seed: 2, split: true });
  const atlas = b.image('atlas.jpg', await jpeg(128, 128, 0), 'image/jpeg');
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(atlas) }, metallicFactor: 0 } });
  const nodes = primitives.map((p, i) => b.meshNode([{ ...p, material }], transform, `Tile${i}`).node);
  b.json.scene = b.scene(nodes);
  b.write(path.join(dir, 'scan-meshes.gltf'));
  writeExpected(dir, {
    description: 'scan-tiles with each tile its own mesh and node, every node with the same transform: borders shared between meshes drawn in one space.',
    geometry: { meshes: 4, primitives: 4, vertices, faces }, bounds: { ...bounds, tolerance: 1e-6 },
    images: [{ name: 'atlas', mimeType: 'image/jpeg', width: 128, height: 128 }],
    notes: ['Bounds are the generator\'s float32 positions through the node transform.', 'Occupancy is not stated: this fixture is for geometry.'],
  });
};

// The values of accessor `accessor` that `builder` wrote, as uint32 indices or floats.
function readBuilt(builder, accessor) {
  const a = builder.json.accessors[accessor];
  const view = builder.json.bufferViews[a.bufferView];
  const bytes = Buffer.concat(builder.chunks[view.buffer]).subarray(view.byteOffset, view.byteOffset + view.byteLength);
  const Type = a.componentType === UINT ? Uint32Array : Float32Array;
  return new Type(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
}

fixtures.soup = async (dir) => {
  // The synthetic scan written three vertices per triangle, without indices, as some scan exports write it, and a
  // second mesh for the edge cases of an exact weld: −0 against 0, and a morph target that keeps two otherwise
  // equal vertices apart.
  const { b: scan, primitive, transform, bounds, vertices, faces } = scanBuilder({ rings: 8, segments: 16, charts: 2, seed: 3 });
  const b = new Builder();
  const idx = readBuilt(scan, primitive.indices);
  const corners = (accessor, n) => {
    const values = readBuilt(scan, accessor);
    return Array.from(idx).flatMap((i) => Array.from(values.subarray(n * i, n * i + n)));
  };
  const atlas = b.image('atlas.jpg', await jpeg(64, 64, 0), 'image/jpeg');
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { baseColorTexture: { index: b.texture(atlas) }, metallicFactor: 0 } });
  const soup = b.meshNode([{
    attributes: { POSITION: b.floats(corners(primitive.attributes.POSITION, 3), 'VEC3'), NORMAL: b.floats(corners(primitive.attributes.NORMAL, 3), 'VEC3'), TEXCOORD_0: b.floats(corners(primitive.attributes.TEXCOORD_0, 2), 'VEC2') },
    material,
  }], transform, 'Soup');
  // Two triangles sharing an edge, six corners: 1 and 3 are equal but for the morph target, 2 and 5 only differ by
  // the sign of a zero.
  const quadPositions = [0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, -0, 1, 0];
  const quads = b.meshNode([{
    attributes: { POSITION: b.floats(quadPositions, 'VEC3'), NORMAL: b.floats(Array(6).fill([0, 0, 1]).flat(), 'VEC3') },
    targets: [{ POSITION: b.floats([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0, 0], 'VEC3') }],
    indices: b.indices([0, 1, 2, 3, 4, 5]),
  }], { translation: [3, 0, 0] }, 'Quads');
  b.json.meshes[quads.mesh].weights = [0];
  b.json.scene = b.scene([soup.node, quads.node]);
  b.write(path.join(dir, 'soup.gltf'));
  writeExpected(dir, {
    description: 'The synthetic scan as a triangle soup (no indices, three vertices per triangle, 2 charts), and two indexed triangles whose corners only an exact weld can tell apart: one pair differs by the sign of a zero, another only in a morph target.',
    geometry: { meshes: 2, primitives: 2, vertices: 3 * faces + 6, faces: faces + 2 },
    bounds: { min: bounds.min.map((v, i) => Math.min(v, [3, 0, 0][i])), max: bounds.max.map((v, i) => Math.max(v, [4, 1, 0][i])), tolerance: 1e-6 },
    weld: { vertices: { Soup: vertices, Quads: 5 }, why: 'Soup: the indexed scan it was unrolled from, whose seam vertices differ in UV. Quads: corners 2 and 5 merge (−0 is 0), 1 and 3 stay apart (the target moves 3).' },
    images: [{ name: 'atlas', mimeType: 'image/jpeg', width: 64, height: 64 }],
    notes: ['Bounds are the generator\'s float32 positions through the node transforms.', 'Occupancy is not stated: this fixture is for geometry.'],
  });
};

fixtures['flat-tiles'] = async (dir) => {
  // The synthetic scan cut into 4 tiles, stored as the campus scan stores them: three vertices per triangle, each
  // with the triangle's face normal, indexed. The tangents follow the faces too.
  const { b: scan, primitives, transform, bounds, faces } = scanBuilder({ rings: 16, segments: 48, charts: 4, seed: 8, split: true });
  const b = new Builder();
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { metallicFactor: 0 } });
  const tiles = primitives.map((tile) => {
    const idx = readBuilt(scan, tile.indices);
    const pos = readBuilt(scan, tile.attributes.POSITION);
    const uv = readBuilt(scan, tile.attributes.TEXCOORD_0);
    const out = { pos: [], nor: [], tan: [], uv: [] };
    for (let t = 0; t < idx.length; t += 3) {
      const [a, c, d] = [idx[t], idx[t + 1], idx[t + 2]].map((i) => [pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]]);
      const e1 = c.map((x, k) => x - a[k]), e2 = d.map((x, k) => x - a[k]);
      const n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
      const nl = Math.hypot(...n), tl = Math.hypot(...e1);
      for (const i of [idx[t], idx[t + 1], idx[t + 2]]) {
        out.pos.push(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
        out.nor.push(...n.map((x) => x / nl));
        out.tan.push(...e1.map((x) => x / tl), 1);
        out.uv.push(uv[2 * i], uv[2 * i + 1]);
      }
    }
    return {
      attributes: { POSITION: b.floats(out.pos, 'VEC3'), NORMAL: b.floats(out.nor, 'VEC3'), TANGENT: b.floats(out.tan, 'VEC4'), TEXCOORD_0: b.floats(out.uv, 'VEC2') },
      indices: b.indices(Array.from({ length: idx.length }, (_, i) => i)),
      material,
      vertices: pos.length / 3,
    };
  });
  const { node } = b.meshNode(tiles.map(({ vertices, ...p }) => p), transform, 'Tiles');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'flat-tiles.gltf'));
  writeExpected(dir, {
    description: 'The synthetic scan cut into 4 tiles, one primitive each, with three vertices per triangle carrying its face normal and a tangent along its first edge, as the campus scan stores its normals.',
    geometry: { meshes: 1, primitives: 4, vertices: 3 * faces, faces }, bounds: { ...bounds, tolerance: 1e-6 },
    normals: { weldedVertices: tiles.map((t) => t.vertices), why: 'Once smooth, a tile\'s vertices at one position are equal in every attribute, so weld brings each tile back to the indexed tile it was unrolled from.' },
    notes: ['Bounds are the generator\'s float32 positions through the node transform.'],
  });
};

fixtures.bare = (dir) => {
  // Primitives without normals: an indexed scan with UV seams, an unlit quad, and two triangles with a morph target.
  const { b, primitive, transform, bounds, vertices, faces } = scanBuilder({ rings: 32, segments: 96, charts: 2, seed: 5, normals: false });
  const material = b.add('materials', { name: 'Scan', pbrMetallicRoughness: { metallicFactor: 0 } });
  const scan = b.meshNode([{ ...primitive, material }], transform, 'Scan');
  const unlit = b.add('materials', { name: 'Unlit', extensions: { KHR_materials_unlit: {} } });
  const quad = b.meshNode([{
    attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], 'VEC3') },
    indices: b.indices([0, 1, 2, 0, 2, 3]),
    material: unlit,
  }], { translation: [3, 0, 0] }, 'Unlit');
  const morph = b.meshNode([{
    attributes: { POSITION: b.floats([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], 'VEC3') },
    targets: [{ POSITION: b.floats([0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0], 'VEC3') }],
    indices: b.indices([0, 1, 2, 0, 2, 3]),
  }], { translation: [5, 0, 0] }, 'Morph');
  b.json.meshes[morph.mesh].weights = [0];
  b.json.extensionsUsed = ['KHR_materials_unlit'];
  b.json.scene = b.scene([scan.node, quad.node, morph.node]);
  b.write(path.join(dir, 'bare.gltf'));
  writeExpected(dir, {
    description: 'Primitives without normals: the synthetic scan (indexed, 2 charts, seam vertices duplicated), a quad with an unlit material, and a quad with a morph target.',
    geometry: { meshes: 3, primitives: 3, vertices: vertices + 8, faces: faces + 4 },
    bounds: { min: bounds.min.map((v) => Math.min(v, 0)), max: bounds.max.map((v, i) => Math.max(v, [6, 1, 0][i])), tolerance: 1e-6 },
    normals: { vertices: { Scan: vertices }, why: 'No edge of the scan is sharper than the crease angle, so no vertex is split; seam vertices keep their own UV.' },
    notes: ['Bounds are the generator\'s float32 positions through the node transforms.', 'The scan\'s triangles are wound clockwise seen from outside, as in every fixture scanBuilder makes: normals computed from the winding point inward.'],
  });
};

fixtures.meshopt = (dir) => {
  // EXT_meshopt_compression with float32 positions and no quantization (gltfpack always quantizes, so
  // this isolates the decoder), and a fallback buffer that holds no data.
  const b = new Builder();
  const positions = Float32Array.from([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 2, 0, 0, 2, 1, 0]);
  const indices = Uint16Array.from([0, 1, 2, 0, 2, 3, 1, 4, 5, 1, 5, 2]);
  const fallback = b.virtualBuffer(0, { EXT_meshopt_compression: { fallback: true } });
  const compressedView = (data, count, stride, mode, target) => {
    const encoded = MeshoptEncoder.encodeGltfBuffer(new Uint8Array(data.buffer), count, stride, mode, 0);
    const byteOffset = b.append(encoded, { buffer: 0 });
    const fallbackOffset = b.lengths[fallback];
    b.lengths[fallback] += Math.ceil(data.byteLength / 4) * 4;
    return b.add('bufferViews', {
      buffer: fallback, byteOffset: fallbackOffset, byteLength: data.byteLength, ...(mode === 'ATTRIBUTES' ? { byteStride: stride } : {}), target,
      extensions: { EXT_meshopt_compression: { buffer: 0, byteOffset, byteLength: encoded.byteLength, byteStride: stride, mode, count } },
    });
  };
  const pv = compressedView(positions, 6, 12, 'ATTRIBUTES', ARRAY_BUFFER);
  const iv = compressedView(indices, 12, 2, 'TRIANGLES', ELEMENT_ARRAY_BUFFER);
  const { node } = b.meshNode([{
    attributes: { POSITION: b.accessor(pv, FLOAT, 'VEC3', 6, { min: [0, 0, 0], max: [2, 1, 0] }) },
    indices: b.accessor(iv, USHORT, 'SCALAR', 12),
  }], {}, 'Meshopt');
  b.json.extensionsUsed = ['EXT_meshopt_compression'];
  b.json.extensionsRequired = ['EXT_meshopt_compression'];
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'meshopt.gltf'));
  writeExpected(dir, {
    description: 'EXT_meshopt_compression on float32 positions and indices, without quantization; the fallback buffer has no data.',
    geometry: { primitives: 1, vertices: 6, faces: 4 }, bounds: { min: [0, 0, 0], max: [2, 1, 0] },
  });
};

/** A wavy n×n grid with normals, UVs and a base-color texture, as a gltf-transform Document. */
async function gridDocument(n = 8) {
  const doc = new Document();
  const buffer = doc.createBuffer();
  const pos = [], nor = [], uv = [], idx = [];
  for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) {
    const [x, y] = [i / (n - 1), j / (n - 1)];
    pos.push(x, y, 0.1 * Math.sin(x * Math.PI));
    nor.push(0, 0, 1);
    uv.push(x, 1 - y);
  }
  for (let j = 0; j < n - 1; j++) for (let i = 0; i < n - 1; i++) {
    const a = j * n + i;
    idx.push(a, a + 1, a + n + 1, a, a + n + 1, a + n);
  }
  const acc = (type, arr) => doc.createAccessor().setType(type).setArray(arr).setBuffer(buffer);
  const texture = doc.createTexture('grid').setImage(await png(32, 32, 6)).setMimeType('image/png').setURI('grid.png');
  const material = doc.createMaterial('Grid').setBaseColorTexture(texture);
  const prim = doc.createPrimitive()
    .setAttribute('POSITION', acc('VEC3', new Float32Array(pos)))
    .setAttribute('NORMAL', acc('VEC3', new Float32Array(nor)))
    .setAttribute('TEXCOORD_0', acc('VEC2', new Float32Array(uv)))
    .setIndices(acc('SCALAR', new Uint16Array(idx)))
    .setMaterial(material);
  const mesh = doc.createMesh('Grid').addPrimitive(prim);
  doc.createScene('Scene').addChild(doc.createNode('Grid').setMesh(mesh));
  doc.getRoot().setDefaultScene(doc.getRoot().listScenes()[0]);
  return doc;
}

const gridExpected = (n = 8) => ({
  geometry: { primitives: 1, vertices: n * n, faces: 2 * (n - 1) * (n - 1) },
  bounds: { min: [0, 0, 0], max: [1, 1, 0.1 * Math.max(...Array.from({ length: n }, (_, i) => Math.sin((i / (n - 1)) * Math.PI)))], tolerance: 1e-3 },
  images: [{ name: 'grid', mimeType: 'image/png', width: 32, height: 32, occupancy: 1 }],
});

fixtures['meshopt-quantized'] = async (dir) => {
  const doc = await gridDocument();
  const io = await getIO();
  await doc.transform(meshopt({ encoder: MeshoptEncoder, level: 'medium' }));
  fs.mkdirSync(dir, { recursive: true });
  await io.write(path.join(dir, 'meshopt-quantized.glb'), doc);
  writeExpected(dir, { description: 'The 8×8 grid through gltf-transform meshopt(): KHR_mesh_quantization plus EXT_meshopt_compression, as gltfpack-style files are.', ...gridExpected(),
    notes: ['Quantization moves positions slightly: bounds within 1e-3. meshtinker decodes it.'] });
};

fixtures.quantized = async (dir) => {
  const doc = await gridDocument();
  const io = await getIO();
  doc.createExtension(KHRMeshQuantization).setRequired(true);
  await doc.transform(quantize());
  fs.mkdirSync(dir, { recursive: true });
  await io.write(path.join(dir, 'quantized.gltf'), doc);
  writeExpected(dir, { description: 'The 8×8 grid through gltf-transform quantize(): KHR_mesh_quantization without compression.', ...gridExpected(),
    notes: ['The dequantization scale and offset go into the node transform. meshtinker decodes it.'] });
};

fixtures.draco = async (dir) => {
  const doc = await gridDocument();
  const io = await getIO();
  await doc.transform(draco({ method: 'edgebreaker' }));
  fs.mkdirSync(dir, { recursive: true });
  await io.write(path.join(dir, 'draco.glb'), doc);
  writeExpected(dir, { description: 'The 8×8 grid through gltf-transform draco().', ...gridExpected(),
    notes: ['Counts come from the JSON accessors; bounds and occupancy from the decoded Draco data, within 1e-3 of the source because Draco quantizes.'] });
};

fixtures['unknown-ext'] = (dir) => {
  const b = new Builder();
  const ext = (where) => ({ MESHTINKER_test_unknown: { where, nested: { list: [1, 2, 3] } } });
  const prim = quad(b);
  prim.extensions = ext('primitive');
  prim.extras = { from: 'primitive' };
  b.json.accessors[prim.attributes.POSITION].extras = { from: 'accessor' };
  b.json.bufferViews[0].extras = { from: 'bufferView' };
  const material = b.add('materials', { name: 'Extended', extensions: ext('material'), extras: { from: 'material' } });
  const { mesh, node } = b.meshNode([{ ...prim, material }], { extensions: ext('node'), extras: { from: 'node' } }, 'Extended');
  b.json.meshes[mesh].extras = { from: 'mesh' };
  // An unknown extension that refers to another object by index: renumbering would silently break it.
  b.json.nodes[node].extensions.MESHTINKER_test_reference = { accessor: prim.attributes.NORMAL };
  b.json.scene = b.scene([node], { extras: { from: 'scene' } });
  b.json.asset.extras = { from: 'asset' };
  b.json.extras = { from: 'root' };
  b.json.extensions = ext('root');
  b.json.extensionsUsed = ['MESHTINKER_test_unknown', 'MESHTINKER_test_reference'];
  b.write(path.join(dir, 'unknown-ext.gltf'));
  writeExpected(dir, {
    description: 'Unknown (non-required) extensions and extras on the root, asset, scene, node, mesh, primitive, material, accessor and buffer view.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    notes: ['All of it must survive import, except possibly the buffer view extras: import rewrites buffer views.',
      'MESHTINKER_test_reference holds an accessor index; an import that renumbers accessors has to decide what to do with it.'],
  });
};

fixtures['missing-image'] = async (dir) => {
  const b = new Builder();
  const color = b.image('color.png', await png(16, 16, 4), 'image/png');
  const normal = b.image('normal.png', await png(16, 16, 5), 'image/png');
  const material = b.add('materials', {
    pbrMetallicRoughness: { baseColorTexture: { index: b.texture(color) } },
    normalTexture: { index: b.texture(normal) },
  });
  const { node } = b.meshNode([{ ...quad(b), material }], {}, 'Quad');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'missing-image.gltf'));
  fs.rmSync(path.join(dir, 'normal.png'));
  writeExpected(dir, {
    description: 'The normal map\'s file is missing; the base color is there.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    images: [
      { name: 'color', mimeType: 'image/png', width: 16, height: 16, occupancy: 1 },
      { name: 'normal', occupancy: null, why: 'missing: a warning, since only a base color is required' },
    ],
    diagnostics: ['missing-image'],
  });
};

fixtures['missing-base-color'] = async (dir) => {
  const b = new Builder();
  const color = b.image('color.png', await png(16, 16, 6), 'image/png');
  const material = b.add('materials', { pbrMetallicRoughness: { baseColorTexture: { index: b.texture(color) } } });
  const { node } = b.meshNode([{ ...quad(b), material }], {}, 'Quad');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'missing-base-color.gltf'));
  fs.rmSync(path.join(dir, 'color.png'));
  writeExpected(dir, {
    description: 'The base color image\'s file is missing.',
    error: 'missing-base-color-image',
    expect: 'rejected by inspect and import',
  });
};

fixtures.instancing = (dir) => {
  const b = new Builder();
  const { node } = b.meshNode([quad(b)], {}, 'Instanced');
  const s = Math.SQRT1_2;
  b.json.nodes[node].translation = [0, 0, 1];
  b.json.nodes[node].extensions = {
    EXT_mesh_gpu_instancing: {
      attributes: {
        TRANSLATION: b.floats([0, 0, 0, 5, 0, 0, 0, -3, 0], 'VEC3'),
        ROTATION: b.floats([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, s, s], 'VEC4'),
        SCALE: b.floats([1, 1, 1, 2, 2, 2, 1, 1, 1], 'VEC3'),
      },
    },
  };
  (b.json.extensionsUsed ??= []).push('EXT_mesh_gpu_instancing');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'instancing.gltf'));
  writeExpected(dir, {
    description: 'One quad drawn three times with EXT_mesh_gpu_instancing, under a node translated by z = 1.',
    geometry: { meshes: 1, primitives: 1, vertices: 4, faces: 2 },
    bounds: { min: [-1, -3, 1], max: [7, 2, 1], tolerance: 1e-6 },
    notes: [
      'Instances: as is, x 0..1 y 0..1; translated by x = 5 and scaled by 2, x 5..7 y 0..2; rotated 90° about z (x, y → −y, x), then translated by y = −3, x −1..0 y −3..−2. All at z = 1.',
      'The mesh counts once, however many instances draw it.',
    ],
  });
};

fixtures['unknown-required'] = (dir) => {
  const b = new Builder();
  const prim = quad(b);
  prim.extensions = { MESHTINKER_test_required: { mode: 'magic' } };
  const { node } = b.meshNode([prim], {}, 'Required');
  b.json.extensionsUsed = ['MESHTINKER_test_required'];
  b.json.extensionsRequired = ['MESHTINKER_test_required'];
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'unknown-required.gltf'));
  writeExpected(dir, {
    description: 'An unknown extension listed in extensionsRequired.',
    geometry: { primitives: 1, vertices: 4, faces: 2 }, bounds: { min: [0, 0, 0], max: [1, 1, 0] },
    notes: ['Whether inspect and import accept this is not decided yet; they must agree.'],
  });
};

/** An axis-aligned cube of side `size` around `center`, each side an n×n grid of quads, wound to face outward. */
function cubeSurface(n, center, size) {
  const positions = [];
  const indices = [];
  const h = size / 2;
  // Each side: the axis it faces, its sign, and two in-plane axes (u, v) with u × v along the outward normal.
  for (const [axis, sign] of [[0, 1], [0, -1], [1, 1], [1, -1], [2, 1], [2, -1]]) {
    const [u, v] = sign > 0 ? [(axis + 1) % 3, (axis + 2) % 3] : [(axis + 2) % 3, (axis + 1) % 3];
    const base = positions.length / 3;
    for (let j = 0; j <= n; j++) {
      for (let i = 0; i <= n; i++) {
        const p = [0, 0, 0];
        p[axis] = center[axis] + sign * h;
        p[u] = center[u] - h + (size * i) / n;
        p[v] = center[v] - h + (size * j) / n;
        positions.push(...p);
      }
    }
    for (let j = 0; j < n; j++) {
      for (let i = 0; i < n; i++) {
        const a = base + j * (n + 1) + i;
        indices.push(a, a + 1, a + n + 2, a, a + n + 2, a + n + 1);
      }
    }
  }
  return { positions, indices };
}

fixtures.cube = (dir) => {
  const b = new Builder();
  const outer = cubeSurface(8, [0, 0, 0], 2);
  const inner = cubeSurface(2, [0.2, 0, 0], 0.5);
  const primitive = (s) => ({ attributes: { POSITION: b.floats(s.positions, 'VEC3') }, indices: b.indices(s.indices) });
  const { node } = b.meshNode([primitive(outer), primitive(inner)], {}, 'Cube');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'cube.gltf'));
  writeExpected(dir, {
    description: 'A closed 2×2×2 cube around the origin, each side an 8×8 grid wound outward, hiding a 0.5 cube inside it as a second primitive.',
    geometry: { primitives: 2, vertices: 6 * 81 + 6 * 9, faces: 6 * 128 + 6 * 8 }, bounds: { min: [-1, -1, -1], max: [1, 1, 1] },
    notes: ['placeholder: closed, facing outward, within a cell of the outer cube, and nothing from the inner cube.'],
  });
};

fixtures.floater = (dir) => {
  const b = new Builder();
  const main = cubeSurface(4, [0, 0, 0], 2);
  const floater = cubeSurface(1, [40, 0, 0], 0.05);
  const { node } = b.meshNode([{ attributes: { POSITION: b.floats(main.positions, 'VEC3') }, indices: b.indices(main.indices) },
    { attributes: { POSITION: b.floats(floater.positions, 'VEC3') }, indices: b.indices(floater.indices) }], {}, 'Floater');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'floater.gltf'));
  writeExpected(dir, {
    description: 'A 2×2×2 cube around the origin and a 0.05 cube 40 units away, as scan noise far off.',
    geometry: { primitives: 2, vertices: 6 * 25 + 6 * 4, faces: 6 * 32 + 6 * 2 }, bounds: { min: [-1, -1, -1], max: [40.025, 1, 1], tolerance: 1e-5 },
    notes: ['40.025 is not a float32: bounds within 1e-5.', 'placeholder: the floater is left out (small-parts-left-out) and the grid refitted to the cube, within a cell of it.'],
  });
};

fixtures.noise = (dir) => {
  // Scan noise in the same primitive as the model: a 0.05 cube and a 0.001 cube beside a 2×2×2 cube. At simplify's
  // default error (0.001 of the scene's 2.5-unit size) only the 0.001 cube is smaller than the error.
  const b = new Builder();
  const parts = [cubeSurface(4, [0, 0, 0], 2), cubeSurface(1, [1.5, 0, 0], 0.05), cubeSurface(1, [0, 1.5, 0], 0.001)];
  const positions = [];
  const indices = [];
  for (const part of parts) {
    const base = positions.length / 3;
    positions.push(...part.positions);
    indices.push(...part.indices.map((i) => i + base));
  }
  const { node } = b.meshNode([{ attributes: { POSITION: b.floats(positions, 'VEC3') }, indices: b.indices(indices) }], {}, 'Noise');
  b.json.scene = b.scene([node]);
  b.write(path.join(dir, 'noise.gltf'));
  writeExpected(dir, {
    description: 'One primitive: a 2×2×2 cube around the origin, a 0.05 cube at x = 1.5 and a 0.001 cube at y = 1.5, as scan noise beside a model.',
    geometry: { primitives: 1, vertices: 6 * 25 + 2 * 6 * 4, faces: 6 * 32 + 2 * 6 * 2 }, bounds: { min: [-1, -1, -1], max: [1.525, 1.5005, 1], tolerance: 1e-5 },
    notes: ['Bounds within 1e-5: 1.525 and 1.5005 are not float32 values.'],
  });
};

const confinement = (name, buffersFor) => (dir) => {
  const b = new Builder();
  const { node } = b.meshNode([quad(b)], {}, name);
  b.json.scene = b.scene([node]);
  buffersFor(b, dir);
};

fixtures['refs-outside'] = confinement('Outside', (b, dir) => {
  b.write(path.join(dir, 'model', 'refs-outside.gltf'), { bufferNames: ['../outside.bin'] });
  writeExpected(dir, { description: 'The buffer URI is ../outside.bin: outside the model folder.', expect: 'rejected without --trusted; with --trusted, as fixture/triangle-like quad: 4 vertices, 2 faces' });
});

fixtures['refs-absolute'] = confinement('Absolute', (b, dir) => {
  b.write(path.join(dir, 'refs-absolute.gltf'));
  const file = path.join(dir, 'refs-absolute.gltf');
  const json = JSON.parse(fs.readFileSync(file, 'utf8'));
  json.buffers[0].uri = '/nonexistent/meshtinker/refs-absolute.bin';
  fs.writeFileSync(file, JSON.stringify(json, null, 2) + '\n');
  writeExpected(dir, { description: 'The buffer URI is an absolute path that does not exist; a file of the same name sits next to the model.', expect: 'rejected: absolute paths are refused (AGENTS.md)' });
});

fixtures['refs-symlink'] = confinement('Symlink', (b, dir) => {
  b.write(path.join(dir, 'model', 'refs-symlink.gltf'), { bufferNames: ['../real.bin'] });
  const file = path.join(dir, 'model', 'refs-symlink.gltf');
  const json = JSON.parse(fs.readFileSync(file, 'utf8'));
  json.buffers[0].uri = 'linked.bin';
  fs.writeFileSync(file, JSON.stringify(json, null, 2) + '\n');
  fs.symlinkSync('../real.bin', path.join(dir, 'model', 'linked.bin'));
  writeExpected(dir, { description: 'The buffer URI is linked.bin, a symlink to ../real.bin outside the model folder.', expect: 'rejected without --trusted' });
});

// ------------------------------------------------------------------------------------------------ main

const names = process.argv.slice(2);
for (const name of names.length ? names : Object.keys(fixtures)) {
  if (!fixtures[name]) {
    console.error(`unknown fixture: ${name}`);
    process.exit(2);
  }
  const dir = path.join(dataDir, name);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  await fixtures[name](dir);
  console.error(`generated ${path.relative(repo, dir)}`);
}
