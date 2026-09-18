// flv_compare <actual.flv> <reference.flv>
//
// Compares the video and audio tags of two FLV files tag for tag (type,
// timestamp, body) with the rules of Golden.test.cpp: onMetaData, the empty
// AAC sequence header and the AVC end-of-sequence tag FFmpeg writes are left
// out. Exit code 0 when both streams match, 1 on a difference, 2 on a usage or
// file error. Used by scripts/verify-flv.sh on what `ffmpeg -listen` recorded.
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "Fixtures.hpp"

namespace {

int usage() {
  std::fprintf(stderr, "usage: flv_compare <actual.flv> <reference.flv>\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return usage();
  std::vector<test::FlvTag> actual;
  std::vector<test::FlvTag> reference;
  try {
    actual = test::parseFlv(test::readFile(argv[1]));
    reference = test::parseFlv(test::readFile(argv[2]));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "flv_compare: %s\n", e.what());
    return 2;
  }

  const std::vector<test::FlvTag> actualVideo = test::filterTags(actual, 9);
  const std::vector<test::FlvTag> actualAudio = test::filterTags(actual, 8);
  const std::vector<test::FlvTag> referenceVideo = test::filterTags(reference, 9);
  const std::vector<test::FlvTag> referenceAudio = test::filterTags(reference, 8);
  std::printf("video tags: %zu (reference %zu)\n", actualVideo.size(), referenceVideo.size());
  std::printf("audio tags: %zu (reference %zu)\n", actualAudio.size(), referenceAudio.size());

  const std::string videoDiff = test::diffTags(actualVideo, referenceVideo, "video");
  const std::string audioDiff = test::diffTags(actualAudio, referenceAudio, "audio");
  bool ok = true;
  if (!videoDiff.empty()) {
    std::printf("MISMATCH %s\n", videoDiff.c_str());
    ok = false;
  }
  if (!audioDiff.empty()) {
    std::printf("MISMATCH %s\n", audioDiff.c_str());
    ok = false;
  }
  if (ok) std::printf("MATCH: video and audio tags equal the reference\n");
  return ok ? 0 : 1;
}
