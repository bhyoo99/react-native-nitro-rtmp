// Publishes the fixture over a real TCP socket to `ffmpeg -listen 1`, an
// independent RTMP implementation, and checks the FLV it recorded. Skipped
// when ffmpeg is not on PATH (or $FFMPEG).
#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Fixtures.hpp"
#include "RtmpPublisher.hpp"
#include "TestHarness.hpp"

extern char** environ;

using namespace nitrortmp;

namespace {

std::string findFfmpeg() {
  if (const char* env = std::getenv("FFMPEG"); env != nullptr && *env != '\0') {
    return access(env, X_OK) == 0 ? std::string(env) : std::string();
  }
  const char* path = std::getenv("PATH");
  if (path == nullptr) return {};
  std::string p(path);
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find(':', start);
    if (end == std::string::npos) end = p.size();
    const std::string candidate = p.substr(start, end - start) + "/ffmpeg";
    if (end > start && access(candidate.c_str(), X_OK) == 0) return candidate;
    start = end + 1;
  }
  return {};
}

int freePort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) port = ntohs(addr.sin_port);
  }
  close(fd);
  return port;
}

int connectWithRetry(int port, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
#ifdef SO_NOSIGPIPE
      int one = 1;
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      return fd;
    }
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return -1;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
  while (size > 0) {
    const ssize_t n = ::send(fd, data, size, 0);
    if (n <= 0) return false;
    data += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

/// Reads whatever is available within timeoutMs and feeds it to the publisher.
bool pumpSocket(int fd, RtmpPublisher& pub, int timeoutMs) {
  pollfd pfd{fd, POLLIN, 0};
  if (poll(&pfd, 1, timeoutMs) <= 0) return false;
  uint8_t buffer[16 * 1024];
  const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
  if (n <= 0) return false;
  pub.onReceive(buffer, static_cast<size_t>(n));
  return true;
}

bool waitForExit(pid_t pid, int timeoutMs, int& status) {
  for (int waited = 0; waited < timeoutMs; waited += 50) {
    if (waitpid(pid, &status, WNOHANG) == pid) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return false;
}

}  // namespace

TEST(ffmpeg_listen_receives_the_publish_and_records_matching_flv) {
  const std::string ffmpeg = findFfmpeg();
  if (ffmpeg.empty()) SKIP_TEST("ffmpeg not found on PATH (set FFMPEG to override)");
  signal(SIGPIPE, SIG_IGN);

  const int port = freePort();
  ASSERT_TRUE(port > 0);
  const char* tmp = std::getenv("TMPDIR");
  const std::string outPath = std::string(tmp != nullptr ? tmp : "/tmp") + "/nitrortmp-interop-" + std::to_string(getpid()) + ".flv";
  const std::string url = "rtmp://127.0.0.1:" + std::to_string(port) + "/live/interop";

  std::vector<std::string> args = {ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-listen", "1",
                                   "-i", url, "-c", "copy", "-f", "flv", outPath};
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  ASSERT_EQ(posix_spawnp(&pid, ffmpeg.c_str(), nullptr, nullptr, argv.data(), environ), 0);

  const int fd = connectWithRetry(port, 100);
  if (fd < 0) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    FAIL_TEST("could not connect to ffmpeg -listen on port " + std::to_string(port));
  }

  std::vector<std::pair<PublisherError, std::string>> errors;
  bool sendFailed = false;
  PublisherCallbacks cb;
  cb.onSend = [&](const uint8_t* data, size_t size) { if (!sendAll(fd, data, size)) sendFailed = true; };
  cb.onError = [&](PublisherError e, const std::string& m) { errors.emplace_back(e, m); };
  RtmpPublisher pub(std::move(cb));
  StreamMetadata md;
  md.width = 320;
  md.height = 240;
  md.frameRate = 30;
  md.audioSampleRate = 48000;
  md.audioChannels = 2;
  pub.setMetadata(md);

  ASSERT_TRUE(pub.start(std::string_view(url)));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (pub.state() == PublisherState::Connecting || pub.state() == PublisherState::Connected) {
    if (std::chrono::steady_clock::now() > deadline || sendFailed) break;
    pumpSocket(fd, pub, 100);
  }
  if (pub.state() != PublisherState::Publishing) {
    close(fd);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    std::string why = errors.empty() ? "no error reported" : errors[0].second;
    FAIL_TEST("did not reach Publishing (state " + std::string(toString(pub.state())) + "): " + why);
  }

  const std::vector<test::MediaFrame> frames = test::loadFixtureFrames();
  size_t accepted = 0;
  for (const test::MediaFrame& f : frames) {
    const bool ok = f.video ? pub.pushVideo(f.data.data(), f.data.size(), f.pts, f.dts)
                            : pub.pushAudio(f.data.data(), f.data.size(), f.pts);
    if (ok) ++accepted;
    pumpSocket(fd, pub, 0);
    if (sendFailed) break;
  }
  EXPECT_EQ(accepted, frames.size());
  EXPECT_FALSE(sendFailed);
  EXPECT_TRUE(errors.empty());

  pub.stop();
  pumpSocket(fd, pub, 200);
  close(fd);

  int status = 0;
  const bool exited = waitForExit(pid, 15000, status);
  EXPECT_TRUE(exited);
  if (exited) EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  const std::vector<test::FlvTag> recorded = test::parseFlv(test::readFile(outPath));
  unlink(outPath.c_str());
  const std::vector<test::FlvTag> expected = test::muxWithVendor(frames);
  EXPECT_EQ(test::diffTags(test::filterTags(recorded, 9), test::filterTags(expected, 9), "ffmpeg video"), "");
  EXPECT_EQ(test::diffTags(test::filterTags(recorded, 8), test::filterTags(expected, 8), "ffmpeg audio"), "");
}
