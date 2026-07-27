#include "native/tabletennis_camera.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace tabletennis::native {
namespace {

constexpr size_t kConstantRowBytes = sizeof(uint32_t) * 4;
constexpr size_t kWorldFirstRow = 0;
constexpr size_t kViewFirstRow = 8;
constexpr size_t kViewProjectionFirstRow = 12;
constexpr size_t kInverseViewFirstRow = 16;
constexpr size_t kContextWorldOffset = 0x00;
constexpr size_t kContextWorldViewOffset = 0x80;
constexpr size_t kContextWvpOffset = 0xC0;
constexpr size_t kContextInverseViewOffset = 0x100;
constexpr size_t kContextViewOffset = 0x140;
constexpr size_t kContextProjectionOffset = 0x1C0;
constexpr float kMatrixTolerance = 0.0001f;

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

bool ReadMatrix(std::span<const std::byte> bank, size_t first_row,
                CapturedCamera::Matrix& matrix) {
  const size_t offset = first_row * kConstantRowBytes;
  if (offset + matrix.size() * sizeof(float) > bank.size()) {
    return false;
  }

  for (size_t index = 0; index < matrix.size(); ++index) {
    matrix[index] = LoadBeF32(bank.data() + offset + index * sizeof(float));
    if (!std::isfinite(matrix[index]) ||
        std::abs(matrix[index]) > 1000000.0f) {
      return false;
    }
  }
  return true;
}

bool ReadMatrixAtByteOffset(std::span<const std::byte> bytes, size_t offset,
                            CapturedCamera::Matrix& matrix) {
  if (offset + matrix.size() * sizeof(float) > bytes.size()) {
    return false;
  }
  for (size_t index = 0; index < matrix.size(); ++index) {
    matrix[index] =
        LoadBeF32(bytes.data() + offset + index * sizeof(float));
    if (!std::isfinite(matrix[index]) ||
        std::abs(matrix[index]) > 1000000.0f) {
      return false;
    }
  }
  return true;
}

CapturedCamera::Matrix Multiply(const CapturedCamera::Matrix& left,
                                const CapturedCamera::Matrix& right) {
  CapturedCamera::Matrix product{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      for (size_t inner = 0; inner < 4; ++inner) {
        product[row * 4 + column] +=
            left[row * 4 + inner] * right[inner * 4 + column];
      }
    }
  }
  return product;
}

bool NearlyEqual(float left, float right,
                 float tolerance = kMatrixTolerance) {
  const float scale = std::max({1.0f, std::abs(left), std::abs(right)});
  return std::abs(left - right) <= tolerance * scale;
}

bool IsIdentity(const CapturedCamera::Matrix& matrix) {
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float expected = row == column ? 1.0f : 0.0f;
      if (!NearlyEqual(matrix[row * 4 + column], expected)) {
        return false;
      }
    }
  }
  return true;
}

bool IsIdentityExactValue(const CapturedCamera::Matrix& matrix) {
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float expected = row == column ? 1.0f : 0.0f;
      if (matrix[row * 4 + column] != expected) {
        return false;
      }
    }
  }
  return true;
}

bool MatricesNearlyEqual(const CapturedCamera::Matrix& left,
                         const CapturedCamera::Matrix& right) {
  for (size_t index = 0; index < left.size(); ++index) {
    if (!NearlyEqual(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

bool MatricesBitEqual(const CapturedCamera::Matrix& left,
                      const CapturedCamera::Matrix& right) {
  for (size_t index = 0; index < left.size(); ++index) {
    if (std::bit_cast<uint32_t>(left[index]) !=
        std::bit_cast<uint32_t>(right[index])) {
      return false;
    }
  }
  return true;
}

bool IsPerspectiveProjection(const CapturedCamera::Matrix& projection) {
  // The title uses a right-handed row-vector D3D projection. Keep this gate
  // tolerant enough for alternate gameplay cameras while rejecting shadow and
  // orthographic passes.
  return std::abs(std::abs(projection[11]) - 1.0f) <= 0.02f &&
         std::abs(projection[15]) <= 0.02f &&
         std::abs(projection[5]) >= std::abs(projection[0]) * 1.2f;
}

}  // namespace

bool DecodeCameraConstantBank(std::span<const std::byte> bank,
                              uint32_t source_model,
                              uint32_t source_shader_group,
                              uint32_t source_index_count,
                              CapturedCamera& camera) {
  CapturedCamera decoded;
  CapturedCamera::Matrix world;
  if (!ReadMatrix(bank, kWorldFirstRow, world) ||
      !ReadMatrix(bank, kViewFirstRow, decoded.view) ||
      !ReadMatrix(bank, kViewProjectionFirstRow, decoded.view_projection) ||
      !ReadMatrix(bank, kInverseViewFirstRow, decoded.inverse_view)) {
    return false;
  }

  // c12..c15 is WorldViewProjection, not a global camera constant. The first
  // proven table/net draw currently has identity World, making those rows the
  // camera ViewProjection exactly. Refuse any other draw until arbitrary world
  // inversion is deliberately implemented and validated.
  if (!IsIdentityExactValue(world) ||
      !IsIdentity(Multiply(decoded.view, decoded.inverse_view)) ||
      !IsIdentity(Multiply(decoded.inverse_view, decoded.view))) {
    return false;
  }

  decoded.projection =
      Multiply(decoded.inverse_view, decoded.view_projection);
  if (!MatricesNearlyEqual(Multiply(decoded.view, decoded.projection),
                           decoded.view_projection) ||
      !IsPerspectiveProjection(decoded.projection)) {
    return false;
  }

  decoded.position = {
      decoded.inverse_view[12],
      decoded.inverse_view[13],
      decoded.inverse_view[14],
  };
  decoded.source_model = source_model;
  decoded.source_shader_group = source_shader_group;
  decoded.source_index_count = source_index_count;
  decoded.valid = true;
  camera = decoded;
  return true;
}

bool DecodeCameraRenderContext(std::span<const std::byte> context_block,
                               uint32_t source_render_context,
                               CapturedCamera& camera) {
  CapturedCamera decoded;
  CapturedCamera::Matrix world;
  CapturedCamera::Matrix world_view;
  CapturedCamera::Matrix world_view_projection;
  if (!ReadMatrixAtByteOffset(context_block, kContextWorldOffset, world) ||
      !ReadMatrixAtByteOffset(context_block, kContextWorldViewOffset,
                              world_view) ||
      !ReadMatrixAtByteOffset(context_block, kContextWvpOffset,
                              world_view_projection) ||
      !ReadMatrixAtByteOffset(context_block, kContextInverseViewOffset,
                              decoded.inverse_view) ||
      !ReadMatrixAtByteOffset(context_block, kContextViewOffset,
                              decoded.view) ||
      !ReadMatrixAtByteOffset(context_block, kContextProjectionOffset,
                              decoded.projection)) {
    return false;
  }

  if (!MatricesNearlyEqual(Multiply(world, decoded.view), world_view) ||
      !MatricesNearlyEqual(Multiply(world_view, decoded.projection),
                           world_view_projection) ||
      !IsIdentity(Multiply(decoded.view, decoded.inverse_view)) ||
      !IsIdentity(Multiply(decoded.inverse_view, decoded.view)) ||
      !IsPerspectiveProjection(decoded.projection)) {
    return false;
  }

  decoded.view_projection =
      Multiply(decoded.view, decoded.projection);
  decoded.position = {
      decoded.inverse_view[12],
      decoded.inverse_view[13],
      decoded.inverse_view[14],
  };
  decoded.source_render_context = source_render_context;
  decoded.valid = true;
  camera = decoded;
  return true;
}

bool CameraMatchesRenderContext(const CapturedCamera& draw_camera,
                                const CapturedCamera& context_camera) {
  if (!draw_camera.valid || !context_camera.valid) {
    return false;
  }

  // ViewInverse is copied directly into c16-c19 and must retain its bits.
  // WorldView and WVP are calculated by guest vector instructions, while the
  // observer derives pure ViewProjection with host scalar math. Compare those
  // numerically to avoid treating harmless last-bit evaluation differences as
  // camera divergence.
  if (!MatricesBitEqual(draw_camera.inverse_view,
                        context_camera.inverse_view) ||
      !MatricesNearlyEqual(draw_camera.view, context_camera.view) ||
      !MatricesNearlyEqual(draw_camera.view_projection,
                           context_camera.view_projection)) {
    return false;
  }

  // The draw observer derives Projection as ViewInverse * WVP, which adds one
  // floating-point round trip compared with the producer's stored matrix.
  return MatricesNearlyEqual(draw_camera.projection,
                             context_camera.projection);
}

}  // namespace tabletennis::native
