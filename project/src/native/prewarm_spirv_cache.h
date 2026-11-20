// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include <xxhash.h>

namespace p2::native {

class PrewarmSpirvCache {
 public:
  static constexpr size_t kMaxBytes = 8u << 20;
  static constexpr uint32_t kMaxModuleBytes = 256u << 10;
  static constexpr size_t kMaxBindings = 512;

  using Identity = std::array<uint64_t, 4>;
  struct Record {
    std::span<const uint8_t> binary;
    std::span<const uint8_t> textures;
    std::span<const uint8_t> samplers;
  };

  static uint32_t Word(std::span<const uint8_t> words, size_t index) {
    uint32_t word;
    std::memcpy(&word, words.data() + index * sizeof(word), sizeof(word));
    return word;
  }

  explicit PrewarmSpirvCache(Identity identity) : identity_(identity) {}

  bool Load(FILE* file) {
    if (!file) return false;
    uint64_t header[kHeaderWords];
    if (fread(header, sizeof(header), 1, file) != 1 ||
        !std::equal(identity_.begin(), identity_.end(), header) ||
        !header[4] || header[4] > kMaxBytes) {
      return false;
    }
    bytes_.resize(size_t(header[4]));
    if (fread(bytes_.data(), bytes_.size(), 1, file) != 1 || fgetc(file) != EOF ||
        XXH3_64bits(bytes_.data(), bytes_.size()) != header[5]) {
      bytes_.clear();
      return false;
    }
    reading_ = true;
    return true;
  }

  bool Find(uint64_t hash, uint64_t modification, Record& out) {
    if (!reading_ || bytes_.size() - offset_ < sizeof(RecordHeader)) return false;
    RecordHeader header;
    std::memcpy(&header, bytes_.data() + offset_, sizeof(header));
    // Records are produced in the same order as the validated prewarm input.
    if (header.hash != hash || header.modification != modification) return false;
    if (header.bytes < 20 || header.bytes > kMaxModuleBytes || (header.bytes & 3) ||
        header.textures > kMaxBindings || header.samplers > kMaxBindings ||
        header.reserved != 0) {
      return false;
    }
    const size_t tex_bytes = size_t(header.textures) * 4;
    const size_t samp_bytes = size_t(header.samplers) * 4;
    const size_t size = header.bytes + tex_bytes + samp_bytes;
    const size_t body = offset_ + sizeof(header);
    if (size > bytes_.size() - body) return false;
    const auto bytes = std::span<const uint8_t>(bytes_).subspan(body, size);
    if (Word(bytes, 0) != 0x07230203u) return false;
    out = {bytes.first(header.bytes), bytes.subspan(header.bytes, tex_bytes),
           bytes.last(samp_bytes)};
    offset_ = body + size;
    return true;
  }

  void Add(uint64_t hash, uint64_t modification, std::span<const uint8_t> binary,
           std::span<const uint32_t> textures, std::span<const uint32_t> samplers) {
    if (reading_ || full_) return;
    if (binary.size() < 20 || binary.size() > kMaxModuleBytes || (binary.size() & 3) ||
        textures.size() > kMaxBindings || samplers.size() > kMaxBindings) {
      return;
    }
    const size_t size = sizeof(RecordHeader) + binary.size() + textures.size_bytes() +
                        samplers.size_bytes();
    if (size > kMaxBytes - bytes_.size()) {
      full_ = true;
      return;
    }
    if (bytes_.size() + size > bytes_.capacity()) {
      bytes_.reserve(std::min(kMaxBytes, std::max(bytes_.size() + size, bytes_.capacity() * 2)));
    }
    const RecordHeader header{hash, modification, uint32_t(binary.size()),
                              uint32_t(textures.size()), uint32_t(samplers.size()), 0};
    Append(&header, sizeof(header));
    Append(binary.data(), binary.size());
    Append(textures.data(), textures.size_bytes());
    Append(samplers.data(), samplers.size_bytes());
  }

  bool NeedsSave() const { return !reading_ && !bytes_.empty(); }

  bool Save(FILE* file) const {
    if (!file || !NeedsSave()) return false;
    uint64_t header[kHeaderWords];
    std::copy(identity_.begin(), identity_.end(), header);
    header[4] = bytes_.size();
    header[5] = XXH3_64bits(bytes_.data(), bytes_.size());
    return fwrite(header, sizeof(header), 1, file) == 1 &&
           fwrite(bytes_.data(), bytes_.size(), 1, file) == 1;
  }

 private:
  static constexpr size_t kHeaderWords = 6;
  struct RecordHeader {
    uint64_t hash, modification;
    uint32_t bytes, textures, samplers, reserved;
  };
  static_assert(sizeof(RecordHeader) == 32);

  void Append(const void* data, size_t size) {
    if (!size) return;
    const auto* first = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), first, first + size);
  }

  Identity identity_;
  std::vector<uint8_t> bytes_;
  size_t offset_ = 0;
  bool reading_ = false, full_ = false;
};

}  // namespace p2::native
