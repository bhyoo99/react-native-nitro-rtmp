#pragma once

// Minimal test harness: TEST(name) registers a case, EXPECT_* record failures
// without aborting, ASSERT_* abort the current case, SKIP_TEST marks it skipped.
// A binary whose cases were all skipped exits with 77 (ctest SKIP_RETURN_CODE).

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace test {

struct SkipTest {
  std::string reason;
};
struct Failure {};

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& cases();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

void recordFailure(const char* file, int line, const std::string& message);
bool check(bool ok, const char* expr, const char* file, int line);
bool checkBytesEq(const uint8_t* a, size_t aSize, const uint8_t* b, size_t bSize, const char* label, const char* file, int line);
std::string hex(const uint8_t* data, size_t size, size_t max = 48);

template <typename T>
std::string show(const T& value) {
  std::ostringstream os;
  if constexpr (std::is_enum_v<T>) {
    os << static_cast<long long>(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    os << (value ? "true" : "false");
  } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>) {
    os << static_cast<int>(value);
  } else {
    os << value;
  }
  return os.str();
}

template <typename A, typename B>
bool checkEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (a == b) {
    return true;
  }
  recordFailure(file, line, std::string(ea) + " == " + eb + " failed: " + show(a) + " vs " + show(b));
  return false;
}

inline bool checkBytesEq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const char* label, const char* file, int line) {
  return checkBytesEq(a.data(), a.size(), b.data(), b.size(), label, file, line);
}

int runAll(int argc, char** argv);

}  // namespace test

#define TEST(name)                                          \
  static void name();                                       \
  static ::test::Registrar registrar_##name(#name, &name); \
  static void name()

#define EXPECT_TRUE(x) ::test::check(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) ::test::check(!static_cast<bool>(x), "!(" #x ")", __FILE__, __LINE__)
#define EXPECT_EQ(a, b) ::test::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
#define EXPECT_BYTES_EQ(a, b) ::test::checkBytesEq((a), (b), #a " vs " #b, __FILE__, __LINE__)
#define ASSERT_TRUE(x)                                    \
  do {                                                    \
    if (!EXPECT_TRUE(x)) throw ::test::Failure{};        \
  } while (0)
#define ASSERT_EQ(a, b)                                   \
  do {                                                    \
    if (!EXPECT_EQ(a, b)) throw ::test::Failure{};       \
  } while (0)
#define ASSERT_BYTES_EQ(a, b)                             \
  do {                                                    \
    if (!EXPECT_BYTES_EQ(a, b)) throw ::test::Failure{}; \
  } while (0)
#define FAIL_TEST(message)                                            \
  do {                                                                \
    ::test::recordFailure(__FILE__, __LINE__, (message));            \
    throw ::test::Failure{};                                          \
  } while (0)
#define SKIP_TEST(reason) throw ::test::SkipTest{(reason)}
