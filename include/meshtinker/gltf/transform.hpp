#pragma once

#include <cgltf.h>

#include <array>
#include <cstddef>
#include <optional>

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
