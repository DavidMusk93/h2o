#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace flowkit::auto_bench::rpc {

// response-v1 matches the Java, Rust and Python fixture byte for byte.
inline std::string responseBase(std::size_t size, const std::string &content) {
  std::string result(size, '\0');
  std::uint32_t state = 0x12345678;
  for (std::size_t index = 0; index < size; ++index) {
    if (content == "entropy") {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      result[index] = static_cast<char>(state & 255);
    } else {
      result[index] = static_cast<char>((index * 31 + 17) % 251 + 1);
    }
  }
  return result;
}

inline void mixResponseSequence(std::string *payload, std::uint64_t sequence,
                                std::size_t offset = 0) {
  for (std::size_t index = offset; index < payload->size(); ++index) {
    (*payload)[index] = static_cast<char>(
        static_cast<unsigned char>((*payload)[index]) ^
        ((sequence >> (((index - offset) % 8) * 8)) & 255));
  }
}

} // namespace flowkit::auto_bench::rpc
