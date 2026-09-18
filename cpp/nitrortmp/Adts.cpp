#include "Adts.hpp"

namespace nitrortmp {

int adtsSampleRateIndex(int sampleRate) {
  static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
  for (int i = 0; i < 13; ++i) {
    if (kRates[i] == sampleRate) return i;
  }
  return -1;
}

bool writeAdtsHeader(uint8_t out[kAdtsHeaderSize], int profile, int sampleRate, int channels, size_t payloadSize) {
  const int rateIndex = adtsSampleRateIndex(sampleRate);
  const size_t frameLength = payloadSize + kAdtsHeaderSize;
  if (out == nullptr || profile < 0 || profile > 3 || rateIndex < 0 || channels < 1 || channels > 7 ||
      frameLength > 0x1FFF) {
    return false;
  }
  // syncword (12) | id=0 MPEG-4 (1) | layer=00 (2) | protection_absent=1 (1)
  out[0] = 0xFF;
  out[1] = 0xF1;
  // profile (2) | sampling_frequency_index (4) | private_bit=0 (1) | channel_configuration high bit (1)
  out[2] = static_cast<uint8_t>((profile << 6) | (rateIndex << 2) | ((channels >> 2) & 0x01));
  // channel_configuration low bits (2) | original=0 | home=0 | copyright_id_bit=0 | copyright_id_start=0 | frame_length high bits (2)
  out[3] = static_cast<uint8_t>(((channels & 0x03) << 6) | ((frameLength >> 11) & 0x03));
  out[4] = static_cast<uint8_t>((frameLength >> 3) & 0xFF);
  // frame_length low bits (3) | adts_buffer_fullness high bits (5) = 0x7FF (VBR)
  out[5] = static_cast<uint8_t>(((frameLength & 0x07) << 5) | 0x1F);
  // adts_buffer_fullness low bits (6) | number_of_raw_data_blocks_in_frame=0 (2)
  out[6] = 0xFC;
  return true;
}

}  // namespace nitrortmp
