// Surveys how normals are stored in real models, and how much welding would
// merge, for docs/experiments/normals.md.
//
//   node --max-old-space-size=24000 tools/experiments/normals/survey.mjs OUT.jsonl MODEL...
//
// One JSON line per model, appended to OUT.jsonl; models already in it are
// skipped. Read-only on the models.
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import draco3d from 'draco3dgltf';
import { MeshoptDecoder } from 'meshoptimizer';
import fs from 'node:fs';

const [out, ...models] = process.argv.slice(2);
const done = new Set(fs.existsSync(out) ? fs.readFileSync(out, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l).path) : []);

await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({
  'draco3d.decoder': await draco3d.createDecoderModule(),
  'meshopt.decoder': MeshoptDecoder,
});

// Attribute values as floats, normalized integers decoded.
function floats(accessor) {
  const a = accessor.getArray();
  if (a instanceof Float32Array) return a;
  const f = new Float32Array(a.length);
  if (!accessor.getNormalized()) { for (let i = 0; i < a.length; i++) f[i] = a[i]; return f; }
  const max = { 1: 127, 2: 32767, 3: 255, 4: 65535 }[a instanceof Int8Array ? 1 : a instanceof Int16Array ? 2 : a instanceof Uint8Array ? 3 : 4];
  for (let i = 0; i < a.length; i++) f[i] = Math.max(a[i] / max, -1);
  return f;
}

// Number of distinct keys among n keys of `words` 32-bit words each, and the
// id (first index) of each key's group.
function group(keys, words, n) {
  let size = 1; while (size < n * 1.5) size <<= 1;
  const table = new Int32Array(size).fill(-1);
  const id = new Int32Array(n);
  let count = 0;
  for (let i = 0; i < n; i++) {
    let h = 2166136261;
    for (let w = 0; w < words; w++) { h = Math.imul(h ^ keys[i * words + w], 16777619); h ^= h >>> 15; }
    let s = (h >>> 0) & (size - 1);
    for (;;) {
      const j = table[s];
      if (j < 0) { table[s] = i; id[i] = i; count++; break; }
      let eq = true;
      for (let w = 0; w < words; w++) if (keys[j * words + w] !== keys[i * words + w]) { eq = false; break; }
      if (eq) { id[i] = j; break; }
      s = (s + 1) & (size - 1);
    }
  }
  return { count, id };
}

// Packs the listed float arrays (components per vertex) into keys, bit for
// bit with −0 as 0, or snapped to a grid of `bits` over [min, max] per array.
function keysOf(n, parts) {
  const words = parts.reduce((s, p) => s + p.c, 0);
  const keys = new Uint32Array(n * words);
  const f = new Float32Array(1), u = new Uint32Array(f.buffer);
  for (let i = 0; i < n; i++) {
    let o = i * words;
    for (const p of parts) for (let c = 0; c < p.c; c++) {
      const v = p.a[i * p.c + c];
      if (p.step) keys[o++] = Math.round((v - p.min) / p.step) >>> 0;
      else { f[0] = v === 0 ? 0 : v; keys[o++] = u[0]; }
    }
  }
  return { keys, words };
}

// asset.generator as written: glTF-Transform replaces it on read.
function generator(path) {
  const b = fs.readFileSync(path);
  const json = b.readUInt32LE(0) === 0x46546c67 ? b.subarray(20, 20 + b.readUInt32LE(12)).toString('utf8') : b.toString('utf8');
  return JSON.parse(json).asset?.generator ?? null;
}

const angleBuckets = [1, 10, 30, 60, 180];
const bucket = (deg) => angleBuckets.findIndex((b) => deg <= b);
// Edges above 60°, for the crease angle: ≤80°, ≤90°, more.
const sharpBuckets = [80, 90, 180];
const sharpBucket = (deg) => sharpBuckets.findIndex((b) => deg <= b);

for (const path of models) {
  if (done.has(path)) continue;
  const t0 = Date.now();
  let doc;
  try { doc = await io.read(path); } catch (e) { fs.appendFileSync(out, JSON.stringify({ path, error: String(e.message).slice(0, 200) }) + '\n'); continue; }
  const root = doc.getRoot();
  const prims = root.listMeshes().flatMap((m) => m.listPrimitives());

  // The scene's position grid, as compress builds it: 14 bits on the longest side.
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const p of prims) {
    const a = p.getAttribute('POSITION'); if (!a) continue;
    const mn = a.getMin([]), mx = a.getMax([]);
    for (let c = 0; c < 3; c++) { lo[c] = Math.min(lo[c], mn[c]); hi[c] = Math.max(hi[c], mx[c]); }
  }
  const posStep = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) / ((1 << 14) - 1);

  const r = {
    path, generator: generator(path), primitives: prims.length, nonTriangle: 0,
    classes: {}, // primitive count per class
    triangles: 0, vertices: 0, corners: 0,
    weld: { exact: 0, withoutNormal: 0, position: 0, quantized: 0 },
    flatTriangles: 0, normalTriangles: 0,
    // Positions whose wedges carry more than one normal, by largest angle between them (≤1°, ≤10°, ≤30°, ≤60°, more).
    splitPositions: [0, 0, 0, 0, 0], positions: 0,
    // Dihedral angles of edges shared by two triangles (by position), same buckets.
    dihedral: [0, 0, 0, 0, 0],
    // The last dihedral bucket (above 60°), split into ≤80°, ≤90° and more.
    dihedralSharp: [0, 0, 0],
    // Stored normal against the area-weighted normal of the position's triangles, same buckets.
    vsSmooth: [0, 0, 0, 0, 0],
    normalMap: 0, tangents: 0, morphNormals: 0,
  };

  for (const p of prims) {
    const mode = p.getMode();
    if (mode !== 4 || !p.getAttribute('POSITION')) { r.nonTriangle++; continue; }
    const P = floats(p.getAttribute('POSITION'));
    const n = P.length / 3;
    const idx = p.getIndices() ? p.getIndices().getArray() : null;
    const corners = idx ? idx.length : n;
    const tris = Math.floor(corners / 3);
    const I = (k) => (idx ? idx[k] : k);
    const nAcc = p.getAttribute('NORMAL');
    const N = nAcc ? floats(nAcc) : null;
    r.triangles += tris; r.vertices += n; r.corners += corners;
    if (p.getAttribute('TANGENT')) r.tangents++;
    if (p.getMaterial()?.getNormalTexture()) r.normalMap++;
    if (p.listTargets().some((t) => t.getAttribute('NORMAL'))) r.morphNormals++;

    // Welds: every attribute bit for bit; all but NORMAL; POSITION alone;
    // every attribute on compress's grids (14-bit scene positions, 8-bit
    // normals, 12-bit texture coordinates over their range).
    const sem = p.listSemantics();
    const part = (s, step) => {
      const acc = p.getAttribute(s), a = floats(acc), c = acc.getElementSize();
      if (!step) return { a, c };
      if (s === 'POSITION') return { a, c, min: Math.min(...lo), step: posStep };
      if (s === 'NORMAL' || s === 'TANGENT') return { a, c, min: -1, step: 2 / 254 };
      let mn = Infinity, mx = -Infinity; for (const v of a) { mn = Math.min(mn, v); mx = Math.max(mx, v); }
      return { a, c, min: mn, step: (mx - mn || 1) / 4095 };
    };
    const weld = (list, step) => { const k = keysOf(n, list.map((s) => part(s, step))); return group(k.keys, k.words, n).count; };
    r.weld.exact += weld(sem, false);
    r.weld.withoutNormal += weld(sem.filter((s) => s !== 'NORMAL'), false);
    r.weld.quantized += weld(sem, true);
    const pk = keysOf(n, [{ a: P, c: 3 }]);
    const pos = group(pk.keys, 3, n);
    r.weld.position += pos.count;

    // Face normals, unnormalized (area-weighted), and per-position sums.
    const face = new Float64Array(tris * 3);
    const smooth = new Float64Array(n * 3);
    for (let t = 0; t < tris; t++) {
      const a = I(3 * t), b = I(3 * t + 1), c = I(3 * t + 2);
      const ux = P[3 * b] - P[3 * a], uy = P[3 * b + 1] - P[3 * a + 1], uz = P[3 * b + 2] - P[3 * a + 2];
      const vx = P[3 * c] - P[3 * a], vy = P[3 * c + 1] - P[3 * a + 1], vz = P[3 * c + 2] - P[3 * a + 2];
      const x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
      face[3 * t] = x; face[3 * t + 1] = y; face[3 * t + 2] = z;
      for (const v of [a, b, c]) { const g = pos.id[v]; smooth[3 * g] += x; smooth[3 * g + 1] += y; smooth[3 * g + 2] += z; }
    }
    const angle = (ax, ay, az, bx, by, bz) => {
      const l = Math.hypot(ax, ay, az) * Math.hypot(bx, by, bz);
      return l ? (Math.acos(Math.min(1, Math.max(-1, (ax * bx + ay * by + az * bz) / l))) * 180) / Math.PI : 0;
    };

    // Dihedral angles, on up to 2 million triangles per primitive.
    const edges = new Map();
    for (let t = 0; t < Math.min(tris, 2e6); t++) {
      const v = [pos.id[I(3 * t)], pos.id[I(3 * t + 1)], pos.id[I(3 * t + 2)]];
      for (let e = 0; e < 3; e++) {
        const a = v[e], b = v[(e + 1) % 3]; if (a === b) continue;
        const key = a < b ? a * 2 ** 26 + b : b * 2 ** 26 + a;
        const o = edges.get(key);
        if (o === undefined) edges.set(key, t);
        else if (o >= 0) {
          const d = angle(face[3 * o], face[3 * o + 1], face[3 * o + 2], face[3 * t], face[3 * t + 1], face[3 * t + 2]);
          r.dihedral[bucket(d)]++;
          if (d > 60) r.dihedralSharp[sharpBucket(d)]++;
          edges.set(key, -1);
        }
      }
    }

    let cls;
    if (!N) cls = idx ? 'no-normals' : 'no-normals-unindexed';
    else {
      let flat = 0, normalTris = 0;
      for (let t = 0; t < tris; t++) {
        const fx = face[3 * t], fy = face[3 * t + 1], fz = face[3 * t + 2];
        if (!fx && !fy && !fz) continue;
        normalTris++;
        let ok = true;
        for (let k = 0; k < 3 && ok; k++) { const v = I(3 * t + k); ok = angle(fx, fy, fz, N[3 * v], N[3 * v + 1], N[3 * v + 2]) < 1; }
        if (ok) flat++;
      }
      r.flatTriangles += flat; r.normalTriangles += normalTris;
      // Split positions: largest angle between the normals of one position's wedges.
      const first = new Int32Array(n).fill(-1), worst = new Float32Array(n);
      for (let v = 0; v < n; v++) {
        const g = pos.id[v];
        if (first[g] < 0) { first[g] = v; continue; }
        const f = first[g];
        if (N[3 * f] === N[3 * v] && N[3 * f + 1] === N[3 * v + 1] && N[3 * f + 2] === N[3 * v + 2]) { worst[g] = Math.max(worst[g], 1e-6); continue; }
        worst[g] = Math.max(worst[g], angle(N[3 * f], N[3 * f + 1], N[3 * f + 2], N[3 * v], N[3 * v + 1], N[3 * v + 2]) + 1e-6);
      }
      let split = 0;
      for (let v = 0; v < n; v++) if (pos.id[v] === v) {
        r.positions++;
        if (worst[v] > 1e-5) { r.splitPositions[bucket(worst[v])]++; split++; }
      }
      for (let v = 0; v < n; v++) {
        const g = pos.id[v];
        r.vsSmooth[bucket(angle(N[3 * v], N[3 * v + 1], N[3 * v + 2], smooth[3 * g], smooth[3 * g + 1], smooth[3 * g + 2]))]++;
      }
      cls = flat === normalTris && normalTris ? 'flat' : flat > normalTris * 0.5 ? 'mostly-flat' : split === 0 ? 'smooth' : 'split';
    }
    r.classes[cls] = (r.classes[cls] ?? 0) + 1;
  }
  r.seconds = (Date.now() - t0) / 1000;
  fs.appendFileSync(out, JSON.stringify(r) + '\n');
  console.error(`${path.split('/').slice(-2).join('/')}: ${JSON.stringify(r.classes)} ${r.seconds}s`);
}
