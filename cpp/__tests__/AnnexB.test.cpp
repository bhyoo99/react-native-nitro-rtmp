// AVCC <-> Annex-B helpers (cpp/nitrortmp/AnnexB.hpp) against the test
// fixture: a round trip keeps every NALU, parameter sets put back in front
// of a stripped keyframe make the core accept it, and the C facade matches.
#include <cstring>
#include <string>
#include <vector>

#include "AnnexB.hpp"
#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "RtmpPublisher.hpp"
#include "TestHarness.hpp"
#include "nitrortmp_c.h"

namespace {

/// Raw NALU payloads (no start codes) in order.
std::vector<std::vector<uint8_t>> nalus(const std::vector<uint8_t>& annexb) {
  std::vector<std::vector<uint8_t>> out;
  size_t i = 0;
  size_t begin = std::string::npos;
  while (i + 3 <= annexb.size()) {
    if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1) {
      if (begin != std::string::npos) {
        size_t end = i;
        if (end > begin && annexb[end - 1] == 0) --end;  // 4-byte start code of the next NALU
        out.emplace_back(annexb.begin() + static_cast<long>(begin), annexb.begin() + static_cast<long>(end));
      }
      begin = i + 3;
      i += 3;
    } else {
      ++i;
    }
  }
  if (begin != std::string::npos) {
    out.emplace_back(annexb.begin() + static_cast<long>(begin), annexb.end());
  }
  return out;
}

/// Annex-B to AVCC with `lengthSize` byte lengths (what VideoToolbox emits).
std::vector<uint8_t> toAvcc(const std::vector<uint8_t>& annexb, int lengthSize) {
  std::vector<uint8_t> out;
  for (const std::vector<uint8_t>& n : nalus(annexb)) {
    for (int k = lengthSize - 1; k >= 0; --k) {
      out.push_back(static_cast<uint8_t>((n.size() >> (8 * k)) & 0xFF));
    }
    out.insert(out.end(), n.begin(), n.end());
  }
  return out;
}

struct Split {
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  std::vector<uint8_t> rest;  // Annex-B without the parameter sets
};

Split stripParameterSets(const std::vector<uint8_t>& annexb) {
  Split s;
  for (const std::vector<uint8_t>& n : nalus(annexb)) {
    const uint8_t type = n.empty() ? 0 : (n[0] & 0x1F);
    if (type == 7) {
      s.sps = n;
    } else if (type == 8) {
      s.pps = n;
    } else {
      const uint8_t start[4] = {0, 0, 0, 1};
      s.rest.insert(s.rest.end(), start, start + 4);
      s.rest.insert(s.rest.end(), n.begin(), n.end());
    }
  }
  return s;
}

std::vector<test::AccessUnit> fixtureUnits() {
  return test::splitAnnexB(test::readFile(test::fixturePath("video.h264")));
}

}  // namespace

TEST(avcc_round_trip_keeps_every_nalu) {
  const std::vector<test::AccessUnit> units = fixtureUnits();
  ASSERT_TRUE(units.size() > 10);
  for (int lengthSize = 1; lengthSize <= 4; ++lengthSize) {
    size_t checked = 0;
    for (const test::AccessUnit& au : units) {
      bool fits = true;
      for (const std::vector<uint8_t>& n : nalus(au.data)) {
        if (lengthSize < 4 && n.size() >= (static_cast<size_t>(1) << (8 * lengthSize))) fits = false;
      }
      if (!fits) continue;  // a 1-byte length cannot carry this NALU
      const std::vector<uint8_t> avcc = toAvcc(au.data, lengthSize);
      const std::vector<uint8_t> back = nitrortmp::avccToAnnexB(avcc.data(), avcc.size(), lengthSize);
      ASSERT_TRUE(nalus(back) == nalus(au.data));
      // The result uses 4-byte start codes only.
      EXPECT_EQ(back.size(), avcc.size() + nalus(au.data).size() * (4 - static_cast<size_t>(lengthSize)));
      ++checked;
    }
    // Every fixture NALU is longer than 255 bytes; the 1-byte case is covered below.
    if (lengthSize >= 2) EXPECT_TRUE(checked > 0);
  }
  for (const std::vector<uint8_t>& small : {test::syntheticIdr(true), test::syntheticPFrame()}) {
    const std::vector<uint8_t> avcc = toAvcc(small, 1);
    const std::vector<uint8_t> back = nitrortmp::avccToAnnexB(avcc.data(), avcc.size(), 1);
    EXPECT_TRUE(nalus(back) == nalus(small));
    EXPECT_BYTES_EQ(back, small);  // the synthetic frames use 4-byte start codes throughout
  }
}

TEST(avcc_conversion_muxes_like_the_original) {
  // The vendored muxer must see the same frames after the round trip.
  std::vector<test::MediaFrame> original = test::loadFixtureFrames();
  std::vector<test::MediaFrame> converted = original;
  for (test::MediaFrame& f : converted) {
    if (!f.video) continue;
    const std::vector<uint8_t> avcc = toAvcc(f.data, 4);
    f.data = nitrortmp::avccToAnnexB(avcc.data(), avcc.size(), 4);
  }
  const std::vector<test::FlvTag> a = test::muxWithVendor(original);
  const std::vector<test::FlvTag> b = test::muxWithVendor(converted);
  EXPECT_EQ(test::diffTags(test::filterTags(b, 9), test::filterTags(a, 9), "video"), "");
}

TEST(avcc_conversion_tolerates_damage) {
  EXPECT_TRUE(nitrortmp::avccToAnnexB(nullptr, 10, 4).empty());
  const uint8_t nalu[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0xAA};
  EXPECT_TRUE(nitrortmp::avccToAnnexB(nalu, sizeof(nalu), 0).empty());
  EXPECT_TRUE(nitrortmp::avccToAnnexB(nalu, sizeof(nalu), 5).empty());
  // A truncated second NALU keeps the first one.
  const uint8_t truncated[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0xAA, 0x00, 0x00, 0x00, 0x09, 0x41};
  const std::vector<uint8_t> out = nitrortmp::avccToAnnexB(truncated, sizeof(truncated), 4);
  const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA};
  EXPECT_BYTES_EQ(out, expected);
  // Empty NALUs are skipped.
  const uint8_t empty[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x41};
  const std::vector<uint8_t> expected2 = {0x00, 0x00, 0x00, 0x01, 0x41};
  EXPECT_BYTES_EQ(nitrortmp::avccToAnnexB(empty, sizeof(empty), 4), expected2);
}

TEST(keyframe_detection_matches_the_fixture) {
  for (const test::AccessUnit& au : fixtureUnits()) {
    EXPECT_EQ(nitrortmp::isKeyframe(au.data.data(), au.data.size()), au.keyframe);
  }
  const std::vector<uint8_t> p = test::syntheticPFrame();
  EXPECT_FALSE(nitrortmp::isKeyframe(p.data(), p.size()));
  const std::vector<uint8_t> idr = test::syntheticIdr(false);
  EXPECT_TRUE(nitrortmp::isKeyframe(idr.data(), idr.size()));
  // 3-byte start codes count too.
  const uint8_t shortCode[] = {0x00, 0x00, 0x01, 0x65, 0x88};
  EXPECT_TRUE(nitrortmp::isKeyframe(shortCode, sizeof(shortCode)));
  EXPECT_FALSE(nitrortmp::isKeyframe(nullptr, 0));
  // No start code at all: not a keyframe (the core rejects it as well).
  const uint8_t bare[] = {0x65, 0x88, 0x84};
  EXPECT_FALSE(nitrortmp::isKeyframe(bare, sizeof(bare)));
}

TEST(parameter_set_detection) {
  const std::vector<test::AccessUnit> units = fixtureUnits();
  ASSERT_TRUE(units[0].keyframe);
  EXPECT_TRUE(nitrortmp::hasParameterSets(units[0].data.data(), units[0].data.size()));
  const Split s = stripParameterSets(units[0].data);
  EXPECT_FALSE(nitrortmp::hasParameterSets(s.rest.data(), s.rest.size()));
  EXPECT_FALSE(s.sps.empty());
  EXPECT_FALSE(s.pps.empty());
  // SPS after the slice does not count: the muxer needs it in front.
  std::vector<uint8_t> late = s.rest;
  const uint8_t start[4] = {0, 0, 0, 1};
  late.insert(late.end(), start, start + 4);
  late.insert(late.end(), s.sps.begin(), s.sps.end());
  EXPECT_FALSE(nitrortmp::hasParameterSets(late.data(), late.size()));
}

TEST(prepending_parameter_sets_restores_the_keyframe) {
  const std::vector<test::AccessUnit> units = fixtureUnits();
  const Split s = stripParameterSets(units[0].data);
  const std::vector<uint8_t> restored = nitrortmp::prependParameterSets(s.sps, s.pps, s.rest.data(), s.rest.size());
  EXPECT_TRUE(nalus(restored) == nalus(units[0].data));
  EXPECT_TRUE(nitrortmp::hasParameterSets(restored.data(), restored.size()));
  EXPECT_TRUE(nitrortmp::isKeyframe(restored.data(), restored.size()));

  // Empty sets are skipped, an empty frame gives just the parameter sets.
  const std::vector<uint8_t> onlyPps = nitrortmp::prependParameterSets({}, s.pps, s.rest.data(), s.rest.size());
  EXPECT_EQ(nalus(onlyPps).size(), nalus(s.rest).size() + 1);
  const std::vector<uint8_t> none = nitrortmp::prependParameterSets(s.sps, s.pps, nullptr, 0);
  EXPECT_EQ(nalus(none).size(), 2u);
}

TEST(core_accepts_a_restored_keyframe_and_rejects_a_stripped_one) {
  // What the VideoToolbox path does: the encoder's keyframe has no SPS/PPS,
  // the helper puts them back, the core takes it as the first frame.
  const std::vector<test::AccessUnit> units = fixtureUnits();
  const Split s = stripParameterSets(units[0].data);

  std::vector<uint8_t> sent;
  nitrortmp::PublisherCallbacks cb;
  cb.onSend = [&](const uint8_t* d, size_t n) { sent.insert(sent.end(), d, d + n); };
  nitrortmp::RtmpPublisher pub(std::move(cb));
  test::LoopbackServer server;
  ASSERT_TRUE(pub.start("rtmp://127.0.0.1/live/test"));
  auto pump = [&] {
    for (;;) {
      bool moved = false;
      if (!sent.empty()) {
        std::vector<uint8_t> b;
        b.swap(sent);
        server.input(b.data(), b.size());
        moved = true;
      }
      if (!server.output.empty()) {
        std::vector<uint8_t> b;
        b.swap(server.output);
        pub.onReceive(b.data(), b.size());
        moved = true;
      }
      if (!moved) return;
    }
  };
  pump();
  ASSERT_EQ(pub.state(), nitrortmp::PublisherState::Publishing);

  // Stripped: an IDR without parameter sets has nothing to build a sequence header from.
  EXPECT_FALSE(pub.pushVideo(s.rest.data(), s.rest.size(), 0, 0));
  EXPECT_EQ(pub.stats().videoTags, 0u);

  const std::vector<uint8_t> restored = nitrortmp::prependParameterSets(s.sps, s.pps, s.rest.data(), s.rest.size());
  EXPECT_TRUE(pub.pushVideo(restored.data(), restored.size(), 0, 0));
  EXPECT_TRUE(pub.pushVideo(units[1].data.data(), units[1].data.size(), 33, 33));
  EXPECT_EQ(pub.stats().videoTags, 3u);  // sequence header + 2 frames
  pump();
  EXPECT_EQ(test::filterTags(server.tags, 9).size(), 3u);
}

TEST(c_facade_matches_the_cpp_helpers) {
  const std::vector<test::AccessUnit> units = fixtureUnits();
  const std::vector<uint8_t> avcc = toAvcc(units[0].data, 4);
  const std::vector<uint8_t> expected = nitrortmp::avccToAnnexB(avcc.data(), avcc.size(), 4);

  // Sizing call, then the copy.
  const size_t needed = nitrortmp_avcc_to_annexb(avcc.data(), avcc.size(), 4, nullptr, 0);
  EXPECT_EQ(needed, expected.size());
  std::vector<uint8_t> out(needed);
  EXPECT_EQ(nitrortmp_avcc_to_annexb(avcc.data(), avcc.size(), 4, out.data(), out.size()), needed);
  EXPECT_BYTES_EQ(out, expected);
  // Too small: nothing is written, the size is still reported.
  std::vector<uint8_t> small(4, 0xEE);
  EXPECT_EQ(nitrortmp_avcc_to_annexb(avcc.data(), avcc.size(), 4, small.data(), small.size()), needed);
  EXPECT_EQ(small[0], static_cast<uint8_t>(0xEE));

  const Split s = stripParameterSets(units[0].data);
  const std::vector<uint8_t> restored = nitrortmp::prependParameterSets(s.sps, s.pps, s.rest.data(), s.rest.size());
  const size_t n2 = nitrortmp_prepend_parameter_sets(s.sps.data(), s.sps.size(), s.pps.data(), s.pps.size(),
                                                     s.rest.data(), s.rest.size(), nullptr, 0);
  EXPECT_EQ(n2, restored.size());
  std::vector<uint8_t> out2(n2);
  nitrortmp_prepend_parameter_sets(s.sps.data(), s.sps.size(), s.pps.data(), s.pps.size(), s.rest.data(),
                                   s.rest.size(), out2.data(), out2.size());
  EXPECT_BYTES_EQ(out2, restored);

  EXPECT_EQ(nitrortmp_annexb_is_keyframe(restored.data(), restored.size()), 1);
  EXPECT_EQ(nitrortmp_annexb_is_keyframe(units[1].data.data(), units[1].data.size()), 0);
  EXPECT_EQ(nitrortmp_annexb_has_parameter_sets(restored.data(), restored.size()), 1);
  EXPECT_EQ(nitrortmp_annexb_has_parameter_sets(s.rest.data(), s.rest.size()), 0);
  EXPECT_EQ(nitrortmp_annexb_is_keyframe(nullptr, 0), 0);
}
