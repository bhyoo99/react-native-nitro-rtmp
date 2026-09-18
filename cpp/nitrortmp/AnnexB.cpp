#include "AnnexB.hpp"

namespace nitrortmp {

namespace {

constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
constexpr uint8_t kNaluIdr = 5;
constexpr uint8_t kNaluSps = 7;

void appendStartCode(std::vector<uint8_t>& out) {
  out.insert(out.end(), kStartCode, kStartCode + sizeof(kStartCode));
}

/// Calls `visit(naluType)` for every NALU header found after a 3- or 4-byte
/// start code. Stops when `visit` returns false.
template <typename Visit>
void forEachNalu(const uint8_t* data, size_t size, Visit visit) {
  if (data == nullptr) return;
  size_t zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t b = data[i];
    if (b == 0x00) {
      ++zeros;
      continue;
    }
    if (b == 0x01 && zeros >= 2 && i + 1 < size) {
      if (!visit(static_cast<uint8_t>(data[i + 1] & 0x1F))) return;
    }
    zeros = 0;
  }
}

}  // namespace

std::vector<uint8_t> avccToAnnexB(const uint8_t* avcc, size_t size, int lengthSize) {
  std::vector<uint8_t> out;
  if (avcc == nullptr || lengthSize < 1 || lengthSize > 4) return out;
  out.reserve(size + 8);
  size_t i = 0;
  const size_t prefix = static_cast<size_t>(lengthSize);
  while (i + prefix <= size) {
    size_t length = 0;
    for (size_t k = 0; k < prefix; ++k) {
      length = (length << 8) | avcc[i + k];
    }
    i += prefix;
    if (length == 0) continue;  // an empty NALU carries nothing; skip it
    if (length > size - i) break;  // truncated: keep what was complete
    appendStartCode(out);
    out.insert(out.end(), avcc + i, avcc + i + length);
    i += length;
  }
  return out;
}

std::vector<uint8_t> prependParameterSets(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                                          const uint8_t* annexb, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(sps.size() + pps.size() + size + 8);
  if (!sps.empty()) {
    appendStartCode(out);
    out.insert(out.end(), sps.begin(), sps.end());
  }
  if (!pps.empty()) {
    appendStartCode(out);
    out.insert(out.end(), pps.begin(), pps.end());
  }
  if (annexb != nullptr && size > 0) {
    out.insert(out.end(), annexb, annexb + size);
  }
  return out;
}

bool isKeyframe(const uint8_t* annexb, size_t size) {
  bool found = false;
  forEachNalu(annexb, size, [&](uint8_t type) {
    if (type == kNaluIdr) {
      found = true;
      return false;
    }
    return true;
  });
  return found;
}

bool hasParameterSets(const uint8_t* annexb, size_t size) {
  bool found = false;
  forEachNalu(annexb, size, [&](uint8_t type) {
    if (type == kNaluSps) {
      found = true;
      return false;
    }
    if (type >= 1 && type <= 5) return false;  // a slice came first
    return true;
  });
  return found;
}

}  // namespace nitrortmp
