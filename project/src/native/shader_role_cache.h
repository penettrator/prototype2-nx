// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace p2::native {

enum PixelShaderRole : uint8_t {
  kReflectionRole = 1u << 0,
  kEdgeBlurRole = 1u << 1,
  kCompositeRole = 1u << 2,
  kWaterIndexRole = 1u << 3,
};

inline constexpr uint64_t kReflectionPixelShaderHash = UINT64_C(0x9AD799B034BB706F);
inline constexpr uint64_t kEdgeBlurPixelShaderHash = UINT64_C(0x3E0AB1A6A7CD8CFB);
inline constexpr uint64_t kCompositePixelShaderHash = UINT64_C(0xC45C07BB16BF2E70);

inline uint8_t ClassifyPixelShaderRoles(bool present, uint64_t hash,
                                        uint64_t water_index_hash) noexcept {
  if (!present) return 0;
  uint8_t roles = 0;
  if (hash == kReflectionPixelShaderHash) roles |= kReflectionRole;
  if (hash == kEdgeBlurPixelShaderHash) roles |= kEdgeBlurRole;
  if (hash == kCompositePixelShaderHash) roles |= kCompositeRole;
  if (water_index_hash != 0 && hash == water_index_hash) roles |= kWaterIndexRole;
  return roles;
}

}  // namespace p2::native
