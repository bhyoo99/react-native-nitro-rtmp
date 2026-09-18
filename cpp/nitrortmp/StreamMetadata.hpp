#pragma once

#include <string>

namespace nitrortmp {

/// Values for the `onMetaData` script tag sent when publishing starts.
/// Zero means "unknown, leave the field out". Codec ids are fixed: H.264 (7)
/// for video and AAC (10) for audio, matching what pushVideo/pushAudio accept.
struct StreamMetadata {
  int width = 0;
  int height = 0;
  double frameRate = 0;         // frames per second
  double videoBitrateKbps = 0;  // kbit/s
  int audioSampleRate = 0;      // Hz, e.g. 48000
  int audioChannels = 0;        // 1 or 2
  double audioBitrateKbps = 0;  // kbit/s
  std::string encoder = "react-native-nitro-rtmp";
};

}  // namespace nitrortmp
