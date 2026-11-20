// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace p2::native {

// Decides only whether the CPU-side SystemConstants object must be recomputed.
// GPU upload and buffer-offset reuse remain governed by the existing submission path.
inline bool RefreshSystemConstants(bool cache_enabled, bool cpu_valid,
                                   bool same_submission, bool same_inputs) noexcept {
  if (!same_inputs) return true;
  if (same_submission) return false;
  return !(cache_enabled && cpu_valid);
}

}  // namespace p2::native
