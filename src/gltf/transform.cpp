#include "meshtinker/gltf/transform.hpp"

#include <algorithm>

#include <cmath>
#include <string>

namespace meshtinker::gltf {

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double sum = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }
            result[column * 4 + row] = sum;
        }
    }
    return result;
}

Matrix trsMatrix(const double t[3], const double q[4], const double scale[3]) {
    Matrix m{};
    const double tx = t[0], ty = t[1], tz = t[2];
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const double sx = scale[0], sy = scale[1], sz = scale[2];
    m[0] = (1 - 2 * (qy * qy + qz * qz)) * sx;
    m[1] = (2 * (qx * qy + qz * qw)) * sx;
    m[2] = (2 * (qx * qz - qy * qw)) * sx;
    m[4] = (2 * (qx * qy - qz * qw)) * sy;
    m[5] = (1 - 2 * (qx * qx + qz * qz)) * sy;
    m[6] = (2 * (qy * qz + qx * qw)) * sy;
    m[8] = (2 * (qx * qz + qy * qw)) * sz;
    m[9] = (2 * (qy * qz - qx * qw)) * sz;
    m[10] = (1 - 2 * (qx * qx + qy * qy)) * sz;
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
    m[15] = 1;
    return m;
}

Matrix worldMatrix(const cgltf_node& node) {
    Matrix world = localMatrix(node);
    for (const cgltf_node* parent = node.parent; parent != nullptr; parent = parent->parent) {
        world = multiply(localMatrix(*parent), world);
    }
    return world;
}

std::vector<std::size_t> meshSpaces(const cgltf_data& data) {
    // Nodes drawing each mesh, and whether one can share its space.
    std::vector<std::size_t> draws(data.meshes_count, 0);
    std::vector<const cgltf_node*> drawer(data.meshes_count, nullptr);
    std::vector<bool> alone(data.meshes_count, false);
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        const auto m = static_cast<std::size_t>(node.mesh - data.meshes);
        ++draws[m];
        drawer[m] = &node;
        alone[m] = alone[m] || node.skin != nullptr || node.has_mesh_gpu_instancing != 0;
    }
    std::vector<std::size_t> spaces(data.meshes_count, 0);
    std::vector<std::pair<Matrix, std::size_t>> shared;
    std::size_t next = 0;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        if (draws[m] != 1 || alone[m]) {
            spaces[m] = next++;
            continue;
        }
        const Matrix world = worldMatrix(*drawer[m]);
        const auto found = std::find_if(shared.begin(), shared.end(),
                                        [&](const std::pair<Matrix, std::size_t>& s) { return s.first == world; });
        if (found != shared.end()) {
            spaces[m] = found->second;
        } else {
            spaces[m] = next;
            shared.emplace_back(world, next++);
        }
    }
    return spaces;
}

Matrix localMatrix(const cgltf_node& node) {
    if (node.has_matrix) {
        Matrix m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = node.matrix[i];
        }
        return m;
    }
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};
    double scale[3] = {1, 1, 1};
    for (std::size_t i = 0; i < 4; ++i) {
        if (i < 3 && node.has_translation) {
            t[i] = node.translation[i];
        }
        if (node.has_rotation) {
            q[i] = node.rotation[i];
        }
        if (i < 3 && node.has_scale) {
            scale[i] = node.scale[i];
        }
    }
    return trsMatrix(t, q, scale);
}


Instancing instancing(const cgltf_node& node) {
    Instancing result;
    if (!node.has_mesh_gpu_instancing) {
        return result;
    }
    for (std::size_t a = 0; a < node.mesh_gpu_instancing.attributes_count; ++a) {
        const cgltf_attribute& attribute = node.mesh_gpu_instancing.attributes[a];
        const std::string name = attribute.name == nullptr ? "" : attribute.name;
        if (name == "TRANSLATION") {
            result.translation = attribute.data;
        } else if (name == "ROTATION") {
            result.rotation = attribute.data;
        } else if (name == "SCALE") {
            result.scale = attribute.data;
        }
    }
    // Every attribute has one element per instance; the extension requires them equal.
    for (const cgltf_accessor* accessor : {result.translation, result.rotation, result.scale}) {
        if (accessor != nullptr) {
            result.count = accessor->count;
        }
    }
    return result;
}

Matrix instanceMatrix(const Instancing& instances, std::size_t i) {
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};
    double scale[3] = {1, 1, 1};
    float values[4] = {0, 0, 0, 0};
    if (instances.translation != nullptr && cgltf_accessor_read_float(instances.translation, i, values, 3)) {
        t[0] = values[0], t[1] = values[1], t[2] = values[2];
    }
    if (instances.rotation != nullptr && cgltf_accessor_read_float(instances.rotation, i, values, 4)) {
        q[0] = values[0], q[1] = values[1], q[2] = values[2], q[3] = values[3];
    }
    if (instances.scale != nullptr && cgltf_accessor_read_float(instances.scale, i, values, 3)) {
        scale[0] = values[0], scale[1] = values[1], scale[2] = values[2];
    }
    return trsMatrix(t, q, scale);
}

std::array<double, 3> apply(const Matrix& m, const std::array<double, 3>& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

std::optional<Matrix> invertAffine(const Matrix& m) {
    // The linear part's cofactors, by row and column of the 3x3 block.
    const auto at = [&](std::size_t row, std::size_t column) { return m[column * 4 + row]; };
    const double c00 = at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1);
    const double c01 = at(1, 2) * at(2, 0) - at(1, 0) * at(2, 2);
    const double c02 = at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0);
    const double determinant = at(0, 0) * c00 + at(0, 1) * c01 + at(0, 2) * c02;
    // Relative to the columns' lengths, so that a uniformly tiny scale isn't flattening.
    double scale = 1;
    for (std::size_t column = 0; column < 3; ++column) {
        scale *= std::sqrt(at(0, column) * at(0, column) + at(1, column) * at(1, column) + at(2, column) * at(2, column));
    }
    if (!(scale > 0) || !(std::abs(determinant) > 1e-12 * scale)) {
        return std::nullopt;
    }
    const double d = 1 / determinant;
    Matrix inverse{};
    const auto set = [&](std::size_t row, std::size_t column, double value) { inverse[column * 4 + row] = value; };
    set(0, 0, c00 * d);
    set(1, 0, c01 * d);
    set(2, 0, c02 * d);
    set(0, 1, (at(0, 2) * at(2, 1) - at(0, 1) * at(2, 2)) * d);
    set(1, 1, (at(0, 0) * at(2, 2) - at(0, 2) * at(2, 0)) * d);
    set(2, 1, (at(0, 1) * at(2, 0) - at(0, 0) * at(2, 1)) * d);
    set(0, 2, (at(0, 1) * at(1, 2) - at(0, 2) * at(1, 1)) * d);
    set(1, 2, (at(0, 2) * at(1, 0) - at(0, 0) * at(1, 2)) * d);
    set(2, 2, (at(0, 0) * at(1, 1) - at(0, 1) * at(1, 0)) * d);
    for (std::size_t row = 0; row < 3; ++row) {
        set(row, 3, -(inverse[row] * m[12] + inverse[4 + row] * m[13] + inverse[8 + row] * m[14]));
    }
    set(3, 3, 1);
    return inverse;
}

} // namespace meshtinker::gltf
