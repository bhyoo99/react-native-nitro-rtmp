// Compiled with only cpp/nitrortmp on the include path: proves the public
// headers do not pull in ireader/media-server headers.
#include "RtmpPublisher.hpp"
#include "RtmpUrl.hpp"
#include "StreamMetadata.hpp"
#include "nitrortmp_c.h"

namespace {
[[maybe_unused]] void touch() {
  nitrortmp::PublisherCallbacks callbacks;
  nitrortmp::RtmpPublisher publisher(std::move(callbacks));
  (void)publisher.state();
  (void)nitrortmp_state_name(NITRORTMP_STATE_IDLE);
}
}  // namespace
