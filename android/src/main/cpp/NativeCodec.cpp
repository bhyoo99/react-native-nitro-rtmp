// JNI half of NativeCodec.kt: stateless wrappers over the encoder output
// helpers of nitrortmp_c.h (AVCC/Annex-B and ADTS).
#include "NativeCodec.hpp"

#include <jni.h>

#include <cstdint>
#include <vector>

#include "nitrortmp_c.h"

namespace {

constexpr const char* kClassName = "com/margelo/nitro/nitrortmp/NativeCodec";

/// Copies `array[0, length)` (clamped to the array size) into a vector.
std::vector<uint8_t> copyIn(JNIEnv* env, jbyteArray array, jint length) {
  std::vector<uint8_t> out;
  if (array == nullptr || length <= 0) return out;
  const jsize available = env->GetArrayLength(array);
  const jsize n = length < available ? length : available;
  out.resize(static_cast<size_t>(n));
  env->GetByteArrayRegion(array, 0, n, reinterpret_cast<jbyte*>(out.data()));
  return out;
}

jbyteArray copyOut(JNIEnv* env, const std::vector<uint8_t>& bytes) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (array != nullptr && !bytes.empty()) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return array;
}

jbyteArray writeAdtsHeader(JNIEnv* env, jclass, jint profile, jint sampleRate, jint channels, jint payloadSize) {
  uint8_t header[7];
  if (payloadSize < 0) return nullptr;
  if (nitrortmp_write_adts_header(header, profile, sampleRate, channels, static_cast<size_t>(payloadSize)) != 1) {
    return nullptr;
  }
  jbyteArray array = env->NewByteArray(7);
  if (array != nullptr) env->SetByteArrayRegion(array, 0, 7, reinterpret_cast<const jbyte*>(header));
  return array;
}

jboolean isKeyframe(JNIEnv* env, jclass, jbyteArray annexb, jint length) {
  const std::vector<uint8_t> bytes = copyIn(env, annexb, length);
  return nitrortmp_annexb_is_keyframe(bytes.data(), bytes.size()) == 1 ? JNI_TRUE : JNI_FALSE;
}

jboolean hasParameterSets(JNIEnv* env, jclass, jbyteArray annexb, jint length) {
  const std::vector<uint8_t> bytes = copyIn(env, annexb, length);
  return nitrortmp_annexb_has_parameter_sets(bytes.data(), bytes.size()) == 1 ? JNI_TRUE : JNI_FALSE;
}

jbyteArray prependParameterSets(JNIEnv* env, jclass, jbyteArray sps, jbyteArray pps, jbyteArray annexb, jint length) {
  const std::vector<uint8_t> s = copyIn(env, sps, sps != nullptr ? env->GetArrayLength(sps) : 0);
  const std::vector<uint8_t> p = copyIn(env, pps, pps != nullptr ? env->GetArrayLength(pps) : 0);
  const std::vector<uint8_t> frame = copyIn(env, annexb, length);
  const size_t needed = nitrortmp_prepend_parameter_sets(s.data(), s.size(), p.data(), p.size(), frame.data(),
                                                         frame.size(), nullptr, 0);
  std::vector<uint8_t> out(needed);
  nitrortmp_prepend_parameter_sets(s.data(), s.size(), p.data(), p.size(), frame.data(), frame.size(), out.data(),
                                   out.size());
  return copyOut(env, out);
}

const JNINativeMethod kMethods[] = {
    {"writeAdtsHeader", "(IIII)[B", reinterpret_cast<void*>(&writeAdtsHeader)},
    {"isKeyframe", "([BI)Z", reinterpret_cast<void*>(&isKeyframe)},
    {"hasParameterSets", "([BI)Z", reinterpret_cast<void*>(&hasParameterSets)},
    {"prependParameterSets", "([B[B[BI)[B", reinterpret_cast<void*>(&prependParameterSets)},
};

}  // namespace

namespace nitrortmp {

bool registerNativeCodec(JNIEnv* env) {
  jclass clazz = env->FindClass(kClassName);
  if (clazz == nullptr) return false;
  const jint count = static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0]));
  return env->RegisterNatives(clazz, kMethods, count) == JNI_OK;
}

}  // namespace nitrortmp
