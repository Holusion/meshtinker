#!/usr/bin/env node
// Semantic equivalence of two glTF models: what a renderer would see, plus the data meshtinker promises to keep.
//
//   node tools/compare.mjs A B [--geometry exact|triangles|world] [--tolerance T] [--attribute-tolerance T] [--max N]
//                              [--ignore ATTRIBUTE,...]
//
// Both models are read with gltf-transform (Draco and EXT_meshopt decoded, quantized values dequantized),
// and only the scene the profile keeps is compared (the default scene, or scene 0), as docs/normalized-profile.md says.
//
// - Node tree: names, local transforms, extras, known extensions, cameras, skins, mesh references.
// - Primitives: mode, material, morph targets, and vertex data, without the attributes --ignore names (NORMAL,TANGENT
//   to check `normals`):
//     exact      every accessor's decoded values in order, and the same indices (repackaging: `import`)
//     triangles  the multiset of triangles with all their corner attributes, rounded to the tolerance:
//                ignores vertex order, welding and index layout (reindexing, lossless meshopt). The tolerance
//                absorbs float noise only: lossy changes (Draco, quantization) are checked with renders.
//     world      for lossy geometry encodings (quantization, meshopt filters) that move data between
//                accessors and node transforms: the scene is compared as the list of primitives it draws, in
//                traversal order, and each triangle list (strips and fans expanded) as the multiset of its
//                triangles in world space.
//                Corners match when positions are within --tolerance (world units) and the other attributes
//                (normals in world space) within --attribute-tolerance. The node tree itself isn't compared.
// - Materials and textures: every factor, texture-info setting and known extension. Textures compare by
//   image bytes and MIME type, never by URI or file name.
// - Animations targeting kept nodes.
// - Unknown extensions and extras anywhere in the raw JSON of the kept scene (gltf-transform drops unknown
//   extensions on read), compared as multisets because indices may be renumbered.
//
// Prints {equal, differences[]}; exit 0 when equal, 1 when different, 2 on usage or read errors.
import crypto from 'node:crypto';
import { Accessor, Animation, Camera, Mesh, Node, Skin, Texture } from '@gltf-transform/core';
import { getIO, KNOWN_EXTENSIONS, readRawJSON, stderrLogger } from './lib/io.mjs';

function sha(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex').slice(0, 16);
}

function attributesOf(property) {
  const sym = Object.getOwnPropertySymbols(property).find((s) => s.description === 'attributes');
  return sym ? property[sym] : {};
}

const SKIP = { Texture: new Set(['uri']), Accessor: new Set(['buffer', 'array']), Buffer: new Set(['uri']) };

/** Canonical JSON-able form of a property's attributes. Nodes, meshes, skins and accessors are summarized. */
function canonical(value, depth = 0) {
  if (value === null || value === undefined) return null;
  if (depth > 32) return '<depth>';
  if (ArrayBuffer.isView(value)) return `bytes:${sha(Buffer.from(value.buffer, value.byteOffset, value.byteLength))}`;
  if (Array.isArray(value)) return value.map((v) => canonical(v, depth + 1));
  if (typeof value !== 'object') return value;
  const ctor = value.constructor?.name ?? '';
  if (/GraphEdge|^Edge$/.test(ctor) || typeof value.getChild === 'function') return canonical(value.getChild(), depth + 1);
  if (/RefSet|RefList/.test(ctor)) return [...value.values()].map((v) => canonical(v, depth + 1));
  if (/RefMap/.test(ctor)) {
    const out = {};
    for (const key of [...value.keys()].sort()) out[key] = canonical(value.get(key), depth + 1);
    return out;
  }
  if (value instanceof Node) return `node:${value.getName()}`;
  if (value instanceof Mesh) return `mesh:${value.getName()}`;
  if (value instanceof Skin) return `skin:${value.getName()}`;
  if (value instanceof Accessor) return `accessor:${value.getType()}x${value.getCount()}`;
  if (value instanceof Texture) return { mimeType: value.getMimeType(), image: value.getImage() ? sha(value.getImage()) : null };
  if (typeof value.propertyType === 'string') {
    const skip = SKIP[value.propertyType] ?? new Set();
    const attrs = attributesOf(value);
    const out = { type: value.propertyType };
    for (const key of Object.keys(attrs).sort()) if (!skip.has(key)) out[key] = canonical(attrs[key], depth + 1);
    return out;
  }
  const out = {};
  for (const key of Object.keys(value).sort()) out[key] = canonical(value[key], depth + 1);
  return out;
}

function diffJSON(a, b, path, report) {
  if (JSON.stringify(a) === JSON.stringify(b)) return;
  if (a && b && typeof a === 'object' && typeof b === 'object' && !Array.isArray(a) && !Array.isArray(b)) {
    for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) diffJSON(a[key], b[key], `${path}.${key}`, report);
    return;
  }
  report(path, `${JSON.stringify(a)?.slice(0, 120)} ≠ ${JSON.stringify(b)?.slice(0, 120)}`);
}

function keptScene(document) {
  const root = document.getRoot();
  return root.getDefaultScene() ?? root.listScenes()[0] ?? null;
}

function elements(accessor) {
  const size = accessor.getElementSize();
  const out = new Float64Array(accessor.getCount() * size);
  const el = new Array(size);
  for (let i = 0; i < accessor.getCount(); i++) {
    accessor.getElement(i, el);
    for (let j = 0; j < size; j++) out[i * size + j] = el[j];
  }
  return { size, values: out };
}

function compareAccessors(a, b, path, tolerance, report) {
  if (!a || !b) return void (a !== b && report(path, a ? 'missing in B' : 'missing in A'));
  if (a.getCount() !== b.getCount() || a.getType() !== b.getType()) {
    return report(path, `${a.getType()}x${a.getCount()} ≠ ${b.getType()}x${b.getCount()}`);
  }
  const [ea, eb] = [elements(a), elements(b)];
  for (let i = 0; i < ea.values.length; i++) {
    if (Math.abs(ea.values[i] - eb.values[i]) > tolerance) {
      return report(path, `element ${Math.floor(i / ea.size)} differs: ${ea.values[i]} ≠ ${eb.values[i]}`);
    }
  }
}

/** Each node of the scene, by its position in a depth-first traversal. */
function nodeOrder(scene, byName = false) {
  const order = new Map();
  const seen = new Map();
  scene?.traverse((n) => {
    // In world mode a command may add nodes (compress moves a mesh to a child): named nodes are matched by
    // name and occurrence instead of by position.
    if (byName && n.getName()) {
      const k = seen.get(n.getName()) ?? 0;
      seen.set(n.getName(), k + 1);
      order.set(n, `${n.getName()}#${k}`);
    } else order.set(n, order.size);
  });
  return order;
}

/** Channels in order: target node (by traversal position) and path, interpolation, decoded keyframes. */
function compareAnimation(a, b, path, orderA, orderB, options, report) {
  if (a.getName() !== b.getName()) report(`${path}.name`, `${a.getName()} ≠ ${b.getName()}`);
  diffJSON(canonical(a.getExtras()), canonical(b.getExtras()), `${path}.extras`, report);
  const [ca, cb] = [a.listChannels(), b.listChannels()];
  if (ca.length !== cb.length) return report(`${path}.channels`, `${ca.length} ≠ ${cb.length}`);
  ca.forEach((c, i) => {
    const d = cb[i];
    const p = `${path}.channels[${i}]`;
    if (orderA.get(c.getTargetNode()) !== orderB.get(d.getTargetNode())) report(`${p}.target.node`, 'differs');
    if (c.getTargetPath() !== d.getTargetPath()) report(`${p}.target.path`, `${c.getTargetPath()} ≠ ${d.getTargetPath()}`);
    const [sa, sb] = [c.getSampler(), d.getSampler()];
    if (sa?.getInterpolation() !== sb?.getInterpolation()) report(`${p}.sampler.interpolation`, 'differs');
    compareAccessors(sa?.getInput(), sb?.getInput(), `${p}.sampler.input`, options.tolerance, report);
    compareAccessors(sa?.getOutput(), sb?.getOutput(), `${p}.sampler.output`, options.tolerance, report);
  });
}

/** Multiset of triangles: each corner is every attribute's value, rounded; corners rotated, winding kept. */
function triangleSet(prim, tolerance, ignore) {
  const semantics = prim.listSemantics().filter((s) => !ignore.has(s)).sort();
  const data = semantics.map((s) => elements(prim.getAttribute(s)));
  const indices = prim.getIndices() ? elements(prim.getIndices()).values : null;
  const count = indices ? indices.length : prim.getAttribute('POSITION').getCount();
  const step = tolerance > 0 ? tolerance : 1e-9;
  const corner = (v) =>
    data.map(({ size, values }) => Array.from(values.subarray(v * size, v * size + size), (x) => Math.round(x / step)).join(',')).join('|');
  const set = new Map();
  for (let t = 0; t + 2 < count + 0; t += 3) {
    const c = [0, 1, 2].map((k) => corner(indices ? indices[t + k] : t + k));
    const first = c.indexOf([...c].sort()[0]);
    const key = [c[first], c[(first + 1) % 3], c[(first + 2) % 3]].join(' / ');
    set.set(key, (set.get(key) ?? 0) + 1);
  }
  return { semantics, set };
}

function comparePrimitive(pa, pb, path, options, report) {
  if (pa.getMode() !== pb.getMode()) return report(`${path}.mode`, `${pa.getMode()} ≠ ${pb.getMode()}`);
  diffJSON(canonical(pa.getMaterial()), canonical(pb.getMaterial()), `${path}.material`, report);
  diffJSON(canonical(pa.getExtras()), canonical(pb.getExtras()), `${path}.extras`, report);
  const kept = (p) => p.listSemantics().filter((s) => !options.ignore.has(s)).sort();
  const [sa, sb] = [kept(pa), kept(pb)];
  if (sa.join() !== sb.join()) return report(`${path}.attributes`, `${sa} ≠ ${sb}`);
  if (options.geometry === 'triangles' && pa.getMode() === 4) {
    const [ta, tb] = [triangleSet(pa, options.tolerance, options.ignore), triangleSet(pb, options.tolerance, options.ignore)];
    let missing = 0;
    let added = 0;
    for (const [k, n] of ta.set) missing += Math.max(0, n - (tb.set.get(k) ?? 0));
    for (const [k, n] of tb.set) added += Math.max(0, n - (ta.set.get(k) ?? 0));
    if (missing || added) report(`${path}.triangles`, `${missing} triangles of A missing from B, ${added} in B not in A`);
  } else {
    for (const s of sa) compareAccessors(pa.getAttribute(s), pb.getAttribute(s), `${path}.attributes.${s}`, options.tolerance, report);
    compareAccessors(pa.getIndices(), pb.getIndices(), `${path}.indices`, 0, report);
  }
  const [ta, tb] = [pa.listTargets(), pb.listTargets()];
  if (ta.length !== tb.length) return report(`${path}.targets`, `${ta.length} ≠ ${tb.length}`);
  ta.forEach((t, i) => {
    for (const s of new Set([...t.listSemantics(), ...tb[i].listSemantics()])) {
      compareAccessors(t.getAttribute(s), tb[i].getAttribute(s), `${path}.targets[${i}].${s}`, options.tolerance, report);
    }
  });
}

function compareNode(na, nb, path, options, report) {
  const plain = (n) => {
    const c = canonical(n);
    for (const k of ['children', 'mesh', 'skin']) delete c[k];
    return c;
  };
  diffJSON(plain(na), plain(nb), path, report);
  diffJSON(canonical(na.getSkin()), canonical(nb.getSkin()), `${path}.skin`, report);
  const [ma, mb] = [na.getMesh(), nb.getMesh()];
  if (!!ma !== !!mb) report(`${path}.mesh`, ma ? 'missing in B' : 'missing in A');
  if (ma && mb) {
    diffJSON(canonical(ma.getExtras()), canonical(mb.getExtras()), `${path}.mesh.extras`, report);
    if (ma.getName() !== mb.getName()) report(`${path}.mesh.name`, `${ma.getName()} ≠ ${mb.getName()}`);
    diffJSON(ma.getWeights(), mb.getWeights(), `${path}.mesh.weights`, report);
    const [pa, pb] = [ma.listPrimitives(), mb.listPrimitives()];
    if (pa.length !== pb.length) report(`${path}.mesh.primitives`, `${pa.length} ≠ ${pb.length}`);
    else pa.forEach((p, i) => comparePrimitive(p, pb[i], `${path}.mesh.primitives[${i}]`, options, report));
  }
  const [ca, cb] = [na.listChildren(), nb.listChildren()];
  if (ca.length !== cb.length) return report(`${path}.children`, `${ca.length} ≠ ${cb.length}`);
  ca.forEach((c, i) => compareNode(c, cb[i], `${path}.children[${i}]${c.getName() ? `(${c.getName()})` : ''}`, options, report));
}

/** Indices of raw JSON objects reachable from the kept scene. */
function reachable(json) {
  const sceneIndex = json.scene ?? 0;
  const r = { nodes: new Set(), meshes: new Set(), materials: new Set(), textures: new Set(), images: new Set(), samplers: new Set(), accessors: new Set(), skins: new Set(), cameras: new Set(), animations: new Set() };
  const stack = [...(json.scenes?.[sceneIndex]?.nodes ?? [])];
  while (stack.length) {
    const n = stack.pop();
    if (r.nodes.has(n)) continue;
    r.nodes.add(n);
    const node = json.nodes[n];
    stack.push(...(node.children ?? []));
    if (node.mesh !== undefined) r.meshes.add(node.mesh);
    if (node.skin !== undefined) r.skins.add(node.skin);
    if (node.camera !== undefined) r.cameras.add(node.camera);
  }
  for (const m of r.meshes) {
    for (const p of json.meshes[m].primitives) {
      if (p.material !== undefined) r.materials.add(p.material);
      for (const a of Object.values(p.attributes ?? {})) r.accessors.add(a);
      if (p.indices !== undefined) r.accessors.add(p.indices);
      for (const t of p.targets ?? []) for (const a of Object.values(t)) r.accessors.add(a);
    }
  }
  for (const s of r.skins) if (json.skins[s].inverseBindMatrices !== undefined) r.accessors.add(json.skins[s].inverseBindMatrices);
  (json.animations ?? []).forEach((anim, i) => {
    if (anim.channels.some((c) => r.nodes.has(c.target?.node))) {
      r.animations.add(i);
      for (const s of anim.samplers) r.accessors.add(s.input).add(s.output);
    }
  });
  const findTextures = (o, key) => {
    if (!o || typeof o !== 'object') return;
    if (/Texture$/.test(key ?? '') && typeof o.index === 'number') r.textures.add(o.index);
    for (const [k, v] of Object.entries(o)) findTextures(v, k);
  };
  for (const m of r.materials) findTextures(json.materials[m]);
  for (const t of r.textures) {
    const tex = json.textures[t];
    if (tex.source !== undefined) r.images.add(tex.source);
    for (const e of Object.values(tex.extensions ?? {})) if (typeof e?.source === 'number') r.images.add(e.source);
    if (tex.sampler !== undefined) r.samplers.add(tex.sampler);
  }
  return r;
}

/** Multiset of extras values and unknown extension objects in the kept part of the raw JSON. */
function rawKeptData(json) {
  const r = reachable(json);
  const items = new Map();
  const add = (key) => items.set(key, (items.get(key) ?? 0) + 1);
  const walk = (o) => {
    if (!o || typeof o !== 'object') return;
    if (Array.isArray(o)) return o.forEach(walk);
    for (const [k, v] of Object.entries(o)) {
      if (k === 'extras') add(`extras ${JSON.stringify(canonical(v))}`);
      else if (k === 'extensions' && v && typeof v === 'object') {
        for (const [name, ext] of Object.entries(v)) {
          if (KNOWN_EXTENSIONS.has(name)) walk(ext);
          else add(`extension ${name} ${JSON.stringify(canonical(ext))}`);
        }
      } else walk(v);
    }
  };
  walk({ asset: json.asset, extras: json.extras, extensions: json.extensions });
  walk(json.scenes?.[json.scene ?? 0]);
  for (const [kind, set] of Object.entries(r)) for (const i of set) walk(json[kind]?.[i]);
  return items;
}

function multiply(m, p, w) {
  return [0, 1, 2].map((i) => m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i] * w);
}

/** The inverse transpose of a 4×4 matrix's upper 3×3, for normals. */
function normalMatrix(m) {
  const [a, b, c, d, e, f, g, h, i] = [m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]];
  const det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const inv = [(e * i - f * h), -(b * i - c * h), (b * f - c * e), -(d * i - f * g), (a * i - c * g), -(a * f - c * d), (d * h - e * g), -(a * h - b * g), (a * e - b * d)].map((x) => x / det);
  // Transposed, as a column-major 4×4 without translation.
  return [inv[0], inv[1], inv[2], 0, inv[3], inv[4], inv[5], 0, inv[6], inv[7], inv[8], 0, 0, 0, 0, 1];
}

function normalize(v) {
  const l = Math.hypot(...v);
  return l > 0 ? v.map((x) => x / l) : v;
}

/** Every primitive the scene draws, in traversal order, with its corners in world space. */
function worldDraws(scene) {
  const draws = [];
  scene?.traverse((node) => {
    const mesh = node.getMesh();
    if (!mesh) return;
    const world = node.getWorldMatrix();
    const normals = normalMatrix(world);
    mesh.listPrimitives().forEach((prim, i) => {
      const semantics = prim.listSemantics().filter((s) => !options.ignore.has(s)).sort();
      const data = Object.fromEntries(semantics.map((s) => [s, elements(prim.getAttribute(s))]));
      const count = prim.getAttribute('POSITION').getCount();
      const corner = (v) => {
        const at = (s) => Array.from(data[s].values.subarray(v * data[s].size, (v + 1) * data[s].size));
        const position = multiply(world, at('POSITION'), 1);
        const attributes = [];
        for (const s of semantics) {
          if (s === 'POSITION') continue;
          if (s === 'NORMAL') attributes.push(...normalize(multiply(normals, at(s), 0)));
          else if (s === 'TANGENT') attributes.push(...normalize(multiply(world, at(s).slice(0, 3), 0)), at(s)[3]);
          else attributes.push(...at(s));
        }
        return { position, attributes };
      };
      const indices = prim.getIndices() ? elements(prim.getIndices()).values : null;
      let corners = indices ? Array.from(indices, corner) : Array.from({ length: count }, (_, v) => corner(v));
      // Strips and fans as the triangle lists they draw, with their winding.
      let mode = prim.getMode();
      if (mode === 5 || mode === 6) {
        const list = [];
        for (let t = 0; t + 2 < corners.length; t++) {
          const c = mode === 6 ? [0, t + 1, t + 2] : t % 2 ? [t + 1, t, t + 2] : [t, t + 1, t + 2];
          list.push(...c.map((k) => corners[k]));
        }
        corners = list;
        mode = 4;
      }
      draws.push({ path: `${node.getName() || 'node'}.mesh.primitives[${i}]`, prim, mode, semantics, corners });
    });
  });
  return draws;
}

/** How far two corners are, relative to the tolerances (1 at the limit), or Infinity when they don't match. */
function cornerDistance(a, b, options) {
  let d = 0;
  for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.position[k] - b.position[k]) / (options.tolerance || 1e-12));
  if (a.attributes.length !== b.attributes.length) return Infinity;
  for (let k = 0; k < a.attributes.length; k++) d = Math.max(d, Math.abs(a.attributes[k] - b.attributes[k]) / (options.attributeTolerance || 1e-12));
  return d <= 1 ? d : Infinity;
}

function cornersMatch(a, b, options) {
  return cornerDistance(a, b, options) <= 1;
}

/** Matches B's triangles to A's, each once, with the winding kept. Returns the counts left unmatched. */
function matchTriangles(ca, cb, options) {
  const cell = Math.max(4 * options.tolerance, 1e-9);
  const key = (t) => [0, 1, 2].map((k) => Math.floor((t[0].position[k] + t[1].position[k] + t[2].position[k]) / 3 / cell));
  const triangles = (c) => Array.from({ length: Math.floor(c.length / 3) }, (_, t) => [c[3 * t], c[3 * t + 1], c[3 * t + 2]]);
  const [ta, tb] = [triangles(ca), triangles(cb)];
  const bins = new Map();
  ta.forEach((t, i) => {
    const k = key(t).join(',');
    if (!bins.has(k)) bins.set(k, []);
    bins.get(k).push(i);
  });
  const used = new Uint8Array(ta.length);
  let added = 0;
  for (const t of tb) {
    const [x, y, z] = key(t);
    // The closest unused candidate: on dense meshes several triangles can be within the tolerance, and taking the
    // first one would use up another triangle's own match.
    let best = -1;
    let bestDistance = Infinity;
    for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) {
      for (const i of bins.get(`${x + dx},${y + dy},${z + dz}`) ?? []) {
        if (used[i]) continue;
        const a = ta[i];
        for (let r = 0; r < 3; r++) {
          const d = Math.max(...[0, 1, 2].map((k) => cornerDistance(a[(k + r) % 3], t[k], options)));
          if (d < bestDistance) [best, bestDistance] = [i, d];
        }
      }
    }
    if (best >= 0) used[best] = 1;
    else added++;
  }
  return { missing: used.reduce((n, u) => n + (u ? 0 : 1), 0), added };
}

function compareWorld(sa, sb, options, report) {
  const [da, db] = [worldDraws(sa), worldDraws(sb)];
  if (da.length !== db.length) return report('scene.draws', `${da.length} ≠ ${db.length}`);
  da.forEach((a, i) => {
    const b = db[i];
    const path = `draws[${i}](${a.path})`;
    if (a.mode !== b.mode) return report(`${path}.mode`, `${a.prim.getMode()} ≠ ${b.prim.getMode()}`);
    diffJSON(canonical(a.prim.getMaterial()), canonical(b.prim.getMaterial()), `${path}.material`, report);
    if (a.semantics.join() !== b.semantics.join()) return report(`${path}.attributes`, `${a.semantics} ≠ ${b.semantics}`);
    if (a.mode === 4) {
      const { missing, added } = matchTriangles(a.corners, b.corners, options);
      if (missing || added) report(`${path}.triangles`, `${missing} triangles of A missing from B, ${added} in B not in A`);
    } else if (a.corners.length !== b.corners.length || a.corners.some((c, k) => !cornersMatch(c, b.corners[k], options))) {
      report(`${path}.corners`, 'differ');
    }
  });
}

export async function compare(fileA, fileB, options = {}) {
  options = { geometry: 'exact', tolerance: 0, attributeTolerance: 0.02, max: 50, ...options };
  options.ignore = new Set(options.ignore ?? []);
  const differences = [];
  let truncated = 0;
  const report = (path, message) => (differences.length < options.max ? differences.push({ path, message }) : truncated++);

  const io = await getIO();
  const [da, db] = await Promise.all([io.read(fileA), io.read(fileB)]);
  da.setLogger(stderrLogger);
  db.setLogger(stderrLogger);
  const [sa, sb] = [keptScene(da), keptScene(db)];
  if (!sa || !sb) {
    if (!!sa !== !!sb) report('scene', sa ? 'no scene in B' : 'no scene in A');
  } else if (options.geometry === 'world') {
    compareWorld(sa, sb, options, report);
  } else {
    diffJSON(canonical(sa.getExtras()), canonical(sb.getExtras()), 'scene.extras', report);
    if (sa.getName() !== sb.getName()) report('scene.name', `${sa.getName()} ≠ ${sb.getName()}`);
    const [ca, cb] = [sa.listChildren(), sb.listChildren()];
    if (ca.length !== cb.length) report('scene.nodes', `${ca.length} ≠ ${cb.length}`);
    else ca.forEach((n, i) => compareNode(n, cb[i], `scene.nodes[${i}]${n.getName() ? `(${n.getName()})` : ''}`, options, report));
  }
  const animations = (doc, scene) => doc.getRoot().listAnimations().filter((a) => {
    const kept = new Set();
    scene?.traverse((n) => kept.add(n));
    return a.listChannels().some((c) => kept.has(c.getTargetNode()));
  });
  const [aa, ab] = [animations(da, sa), animations(db, sb)];
  if (aa.length !== ab.length) report('animations', `${aa.length} ≠ ${ab.length}`);
  else {
    // Not Animation.equals: it follows accessors to their Buffer, whose uri
    // changes whenever files are repackaged.
    const byName = options.geometry === 'world';
    const [ia, ib] = [nodeOrder(sa, byName), nodeOrder(sb, byName)];
    aa.forEach((a, i) => compareAnimation(a, ab[i], `animations[${i}]${a.getName() ? `(${a.getName()})` : ''}`, ia, ib, options, report));
  }

  const [ra, rb] = [rawKeptData(await readRawJSON(fileA)), rawKeptData(await readRawJSON(fileB))];
  for (const [k, n] of ra) if ((rb.get(k) ?? 0) < n) report('raw', `lost: ${k.slice(0, 200)}`);
  for (const [k, n] of rb) if ((ra.get(k) ?? 0) < n) report('raw', `added: ${k.slice(0, 200)}`);

  return { a: fileA, b: fileB, geometry: options.geometry, tolerance: options.tolerance, equal: differences.length === 0, differences, truncated };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const args = process.argv.slice(2);
  const opt = (name, def) => {
    const i = args.indexOf(name);
    return i >= 0 ? args.splice(i, 2)[1] : def;
  };
  const geometry = opt('--geometry', 'exact');
  const tolerance = Number(opt('--tolerance', geometry === 'exact' ? '0' : '1e-5'));
  const attributeTolerance = Number(opt('--attribute-tolerance', '0.02'));
  const max = Number(opt('--max', '50'));
  const ignore = opt('--ignore', '').split(',').filter(Boolean);
  if (args.length !== 2 || !['exact', 'triangles', 'world'].includes(geometry)) {
    console.error('Usage: node tools/compare.mjs A B [--geometry exact|triangles|world] [--tolerance T] [--attribute-tolerance T] [--max N] [--ignore ATTRIBUTE,...]');
    process.exit(2);
  }
  try {
    const result = await compare(args[0], args[1], { geometry, tolerance, attributeTolerance, max, ignore });
    console.log(JSON.stringify(result, null, 2));
    process.exitCode = result.equal ? 0 : 1;
  } catch (e) {
    console.error(e?.stack ?? e);
    process.exitCode = 2;
  }
}
