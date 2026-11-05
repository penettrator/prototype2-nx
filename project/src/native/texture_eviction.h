// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace p2::native {
// A missing grading LUT blacks out the whole 3D composite, unlike a missing
// ordinary material. Keep this exception bounded to small RGBA cube LUTs.
inline bool IsSmallColorLut(bool volume, bool rgba8, uint32_t width, uint32_t height,
                           uint32_t depth, uint64_t bytes) {
  return volume && rgba8 && width && width==height && height==depth &&
         width<=32 && bytes && bytes<=256*1024;
}

// Video cutscenes (2026-11-04, green flash before every video): the decoder's planes are linear 8-bit textures
// (Y 1280x720, U/V 640x360) that the game converts to RGB in a shader. Streamed in like other new textures, they
// were drawn with the empty (all-zero) texture for their first frame or two, and Y=U=V=0 comes out green. Such
// planes are made on the spot (a few once per video). Kept narrow: linear, single-channel, one mip, one layer.
inline bool IsVideoPlane(bool volume, bool tiled, bool k8, uint32_t levels, uint32_t layers, uint32_t width,
                         uint32_t height, uint64_t bytes) {
  return !volume && !tiled && k8 && levels == 1 && layers == 1 && width >= 160 && height >= 90 &&
         width <= 1920 && height <= 1088 && bytes && bytes <= 2u * 1024 * 1024;
}

// A long map pause ages scene materials, but it must not destroy them unless
// the cache also exceeds its payload budget. Pending jobs cannot be retired.
inline bool IdleTextureExpired(uint64_t frame, uint64_t last_used, bool streaming) {
  return !streaming && frame > last_used && frame - last_used > 600;
}

// A completed upload that never reached a GPU submission still owns both an
// image and mapped staging. Age it by the last REQUEST, including pending draws.
inline bool DiscardUnusedStream(uint64_t frame, uint64_t last_request,
                                bool worker_done, bool gpu_image, bool has_views) {
  return worker_done && !gpu_image && !has_views &&
         IdleTextureExpired(frame, last_request, false);
}

// Invalidates CPU references before a retired image may be destroyed or reused.
// View handles alone are insufficient: a memo also owns a raw Texture pointer.
template <typename Memo, typename IsEvicted>
bool InvalidateEvictedTextureMemo(Memo& memo, IsEvicted evicted) {
  if (!memo.texture || !evicted(memo.texture)) return false;
  memo.view = {};
  memo.texture = nullptr;
  memo.valid_until = 0;
  return true;
}
}  // namespace p2::native
