#include "TestHarness.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

namespace test {

namespace {
int g_failures = 0;
}

std::vector<Case>& cases() {
  static std::vector<Case> list;
  return list;
}

void recordFailure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::printf("    FAIL %s:%d: %s\n", file, line, message.c_str());
}

bool check(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    recordFailure(file, line, std::string("expected ") + expr);
  }
  return ok;
}

std::string hex(const uint8_t* data, size_t size, size_t max) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  const size_t n = size < max ? size : max;
  for (size_t i = 0; i < n; ++i) {
    if (i) out += ' ';
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  if (size > max) out += " ...";
  return out;
}

bool checkBytesEq(const uint8_t* a, size_t aSize, const uint8_t* b, size_t bSize, const char* label, const char* file, int line) {
  const size_t common = aSize < bSize ? aSize : bSize;
  size_t i = 0;
  while (i < common && a[i] == b[i]) ++i;
  if (i == common && aSize == bSize) {
    return true;
  }
  std::string message = std::string(label) + ": sizes " + std::to_string(aSize) + " vs " + std::to_string(bSize);
  if (i < common) {
    const size_t from = i >= 8 ? i - 8 : 0;
    message += ", first difference at byte " + std::to_string(i) + "\n      left : " + hex(a + from, aSize - from, 24) +
               "\n      right: " + hex(b + from, bSize - from, 24);
  } else {
    message += ", common prefix of " + std::to_string(common) + " bytes";
  }
  recordFailure(file, line, message);
  return false;
}

int runAll(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, passed = 0, skipped = 0, failed = 0;
  for (const Case& c : cases()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) {
      continue;
    }
    ++ran;
    const int before = g_failures;
    std::printf("[ RUN  ] %s\n", c.name);
    std::fflush(stdout);
    try {
      c.fn();
    } catch (const SkipTest& skip) {
      std::printf("[ SKIP ] %s: %s\n", c.name, skip.reason.c_str());
      ++skipped;
      continue;
    } catch (const Failure&) {
      // already recorded
    } catch (const std::exception& e) {
      recordFailure("?", 0, std::string("unexpected exception: ") + e.what());
    }
    if (g_failures == before) {
      std::printf("[  OK  ] %s\n", c.name);
      ++passed;
    } else {
      std::printf("[ FAIL ] %s\n", c.name);
      ++failed;
    }
  }
  std::printf("%d run, %d passed, %d skipped, %d failed\n", ran, passed, skipped, failed);
  if (failed > 0) return 1;
  if (ran > 0 && skipped == ran) return 77;
  return 0;
}

}  // namespace test
