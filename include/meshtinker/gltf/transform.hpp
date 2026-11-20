#pragma once

#include <cgltf.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace meshtinker::gltf {

// A 4x4 column-major matrix in double precision, as glTF stores `matrix`.
using Matrix = std::array<double, 16>;

constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

Matrix multiply(const Matrix& a, const Matrix& b);

// T * R * S, as the glTF spec defines it, from a translation, a unit
// quaternion (x, y, z, w) and a scale.
Matrix trsMatrix(const double t[3], const double q[4], const double scale[3]);

// A node's local transform: its `matrix`, or T * R * S.
Matrix localMatrix(const cgltf_node& node);

// The point `p` through `m`.
std::array<double, 3> apply(const Matrix& m, const std::array<double, 3>& p);

// The inverse of an affine matrix, or nothing when it flattens space (its
// linear part's determinant is zero, relative to its scale).
std::optional<Matrix> invertAffine(const Matrix& m);

// A node's world matrix: its local matrix through every parent's.
Matrix worldMatrix(const cgltf_node& node);

// Meshes that are drawn in one space, by mesh index: the same number for
// meshes whose positions compare directly, because each is drawn once, by a
// node with the same world matrix as the others', neither skinned nor
// instanced. Tiled scans are often written as one mesh per tile, all in the
// same space, with borders the tiles share. Every other mesh is alone in its
// space. Numbers run from 0, in the order of each space's first mesh.
std::vector<std::size_t> meshSpaces(const cgltf_data& data);

// The EXT_mesh_gpu_instancing attributes of a node, and how many instances they give.
struct Instancing {
    const cgltf_accessor* translation = nullptr;
    const cgltf_accessor* rotation = nullptr;
    const cgltf_accessor* scale = nullptr;
    std::size_t count = 1;
};

Instancing instancing(const cgltf_node& node);

// Instance `i`'s matrix: T * R * S from the instancing attributes.
Matrix instanceMatrix(const Instancing& instances, std::size_t i);

} // namespace meshtinker::gltf
