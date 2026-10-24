#include "head_model.hpp"

#include "sof/lip.hpp"

#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace sof
{
  namespace
  {
    using Vec = std::array<float, 3>;

    const cgltf_accessor* attribute(const cgltf_attribute* attributes, cgltf_size count,
      cgltf_attribute_type type)
    {
      for (cgltf_size i = 0; i < count; ++i)
        if (attributes[i].type == type)
          return attributes[i].data;
      return nullptr;
    }

    std::vector<float> unpack(const cgltf_accessor* accessor, std::size_t count, bool required)
    {
      if (!accessor)
      {
        if (required)
          throw std::runtime_error("Head mesh requires positions and normals");
        return std::vector<float>(count * 3);
      }
      if (accessor->type != cgltf_type_vec3 || accessor->count != count)
        throw std::runtime_error("Invalid head vertex attribute");
      std::vector<float> result(count * 3);
      if (cgltf_accessor_unpack_floats(accessor, result.data(), result.size()) != result.size() || !std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("Cannot read head vertex attribute");
      return result;
    }

    Vec transform(const float* matrix, const float* v)
    {
      return { matrix[0] * v[0] + matrix[4] * v[1] + matrix[8] * v[2],
        matrix[1] * v[0] + matrix[5] * v[1] + matrix[9] * v[2],
        matrix[2] * v[0] + matrix[6] * v[1] + matrix[10] * v[2] };
    }

    std::array<float, 16> normal_matrix(const float* m)
    {
      // Inverse transpose of the node's linear transform, including nonuniform scale.
      const Vec a { m[0], m[1], m[2] }, b { m[4], m[5], m[6] }, c { m[8], m[9], m[10] };
      const auto cross = [](Vec u, Vec v) -> Vec {
        return { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
      };
      const auto x = cross(b, c), y = cross(c, a), z = cross(a, b);
      const float determinant = a[0] * x[0] + a[1] * x[1] + a[2] * x[2];
      if (std::abs(determinant) < 1e-12F)
        throw std::runtime_error("Head node has a singular transform");
      std::array<float, 16> result {};
      for (std::size_t i = 0; i < 3; ++i)
      {
        result[i] = x[i] / determinant;
        result[4 + i] = y[i] / determinant;
        result[8 + i] = z[i] / determinant;
      }
      return result;
    }
  }  // namespace

  HeadModel::HeadModel(const std::filesystem::path& path)
  {
    cgltf_options options {};
    cgltf_data* raw {};
    if (cgltf_parse_file(&options, path.string().c_str(), &raw) != cgltf_result_success)
      throw std::runtime_error("Cannot load head model: " + path.string());
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    if (cgltf_load_buffers(&options, data.get(), path.string().c_str()) != cgltf_result_success || cgltf_validate(data.get()) != cgltf_result_success)
      throw std::runtime_error("Invalid head model buffers");
    if (!data->scene || data->scene->nodes_count != 1 || data->nodes_count != 1 || data->meshes_count != 1 || data->meshes[0].primitives_count != 1)
      throw std::runtime_error("Head model requires one scene node, mesh and primitive");
    const auto& node = *data->scene->nodes[0];
    if (node.skin || node.children_count || node.mesh != &data->meshes[0])
      throw std::runtime_error("Head model requires a single unskinned mesh node");
    float matrix[16];
    cgltf_node_transform_world(&node, matrix);
    const auto normals = normal_matrix(matrix);
    const auto& primitive = node.mesh->primitives[0];
    if (primitive.type != cgltf_primitive_type_triangles)
      throw std::runtime_error("Head mesh must use triangles");
    const auto* positions =
      attribute(primitive.attributes, primitive.attributes_count, cgltf_attribute_type_position);
    if (!positions)
      throw std::runtime_error("Head mesh is missing positions");
    const auto count = positions->count;
    if (count > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("Head mesh is too large");
    const auto pos = unpack(positions, count, true);
    const auto norm =
      unpack(attribute(primitive.attributes, primitive.attributes_count, cgltf_attribute_type_normal),
        count, true);
    for (std::size_t i = 0; i < count; ++i)
    {
      auto v = HeadVertex { transform(matrix, &pos[i * 3]), transform(normals.data(), &norm[i * 3]) };
      for (std::size_t axis = 0; axis < 3; ++axis)
        v.position[axis] += matrix[12 + axis];
      base_.push_back(v);
    }
    const auto index_count = primitive.indices ? primitive.indices->count : count;
    if (index_count % 3)
      throw std::runtime_error("Invalid head triangle indices");
    for (std::size_t i = 0; i < index_count; ++i)
    {
      const auto index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
      if (index >= count)
        throw std::runtime_error("Head triangle index is out of range");
      indices_.push_back(static_cast<std::uint32_t>(index));
    }
    if (primitive.targets_count != lip_parameter_count)
      throw std::runtime_error("Head mesh requires 37 ordered lip blendshapes");
    for (std::size_t t = 0; t < primitive.targets_count; ++t)
    {
      const auto& source = primitive.targets[t];
      auto& target = targets_[t];
      const auto delta_pos =
        unpack(attribute(source.attributes, source.attributes_count, cgltf_attribute_type_position),
          count, false);
      const auto delta_norm = unpack(
        attribute(source.attributes, source.attributes_count, cgltf_attribute_type_normal), count, false
      );
      target.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
        target.push_back(
          { transform(matrix, &delta_pos[i * 3]), transform(normals.data(), &delta_norm[i * 3]) }
        );
    }
    if (base_.empty() || indices_.empty())
      throw std::runtime_error("Head model has no triangles");

    Vec minimum = base_.front().position, maximum = minimum;
    for (const auto& vertex : base_)
      for (std::size_t axis = 0; axis < 3; ++axis)
      {
        minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
        maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
      }
    const float extent =
      std::max({ maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2] });
    if (!(extent > 0.0F))
      throw std::runtime_error("Head model has empty bounds");
    const float scale = 1.6F / extent;
    // Preserve the model's Y=0 plane for top-aligned preview framing.
    for (auto& vertex : base_)
      for (std::size_t axis = 0; axis < 3; ++axis)
        vertex.position[axis] =
          (vertex.position[axis] - (axis == 1 ? 0.0F : (minimum[axis] + maximum[axis]) * 0.5F)) * scale;
    for (auto& target : targets_)
      for (auto& delta : target)
        for (auto& component : delta.position)
          component *= scale;
    deform({});
  }

  void HeadModel::deform(std::span<const Parameter> parameters)
  {
    deformed_ = base_;
    for (std::size_t shape = 0; shape < std::min(parameters.size(), targets_.size()); ++shape)
    {
      const auto& target = targets_[shape];
      const float value = parameters[shape].value;
      const float weight = std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
      if (weight == 0.0F)
        continue;
      for (std::size_t i = 0; i < target.size(); ++i)
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
          deformed_[i].position[axis] += weight * target[i].position[axis];
          deformed_[i].normal[axis] += weight * target[i].normal[axis];
        }
    }
    for (auto& vertex : deformed_)
    {
      float length {};
      for (const float component : vertex.normal)
        length += component * component;
      if (length > 0.0F)
        for (auto& component : vertex.normal)
          component /= std::sqrt(length);
    }
  }
}  // namespace sof
