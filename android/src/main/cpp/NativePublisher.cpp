// JNI half of NativePublisher.kt: maps the Kotlin externals to nitrortmp_c.h
// and calls the three dispatch* methods back on the same (session) thread.
#include "NativePublisher.hpp"

#include <jni.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "nitrortmp_c.h"

namespace {

constexpr const char* kClassName = "com/margelo/nitro/nitrortmp/NativePublisher";

JavaVM* g_vm = nullptr;
jmethodID g_dispatchSend = nullptr;
jmethodID g_dispatchState = nullptr;
jmethodID g_dispatchError = nullptr;

/// One per NativePublisher. `self` is weak so a collected Kotlin object never
/// keeps the session alive; callbacks only fire inside calls the object makes.
struct Session {
  jweak self = nullptr;
  nitrortmp_publisher_t* publisher = nullptr;
};

JNIEnv* currentEnv() {
  JNIEnv* env = nullptr;
  if (g_vm == nullptr || g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return nullptr;
  }
  return env;
}

Session* session(jlong handle) {
  return reinterpret_cast<Session*>(handle);
}

nitrortmp_publisher_t* publisher(jlong handle) {
  Session* s = session(handle);
  return s != nullptr ? s->publisher : nullptr;
}

// --- core callbacks --------------------------------------------------------------

void onSend(void* ctx, const uint8_t* data, size_t size) {
  JNIEnv* env = currentEnv();
  auto* s = static_cast<Session*>(ctx);
  if (env == nullptr || s == nullptr || size == 0) return;
  jobject self = env->NewLocalRef(s->self);
  if (self == nullptr) return;
  jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
  if (array != nullptr) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
    env->CallVoidMethod(self, g_dispatchSend, array);
    env->DeleteLocalRef(array);
  }
  env->DeleteLocalRef(self);
}

void onState(void* ctx, int state) {
  JNIEnv* env = currentEnv();
  auto* s = static_cast<Session*>(ctx);
  if (env == nullptr || s == nullptr) return;
  jobject self = env->NewLocalRef(s->self);
  if (self == nullptr) return;
  env->CallVoidMethod(self, g_dispatchState, static_cast<jint>(state));
  env->DeleteLocalRef(self);
}

void onError(void* ctx, int code, const char* message) {
  JNIEnv* env = currentEnv();
  auto* s = static_cast<Session*>(ctx);
  if (env == nullptr || s == nullptr) return;
  jobject self = env->NewLocalRef(s->self);
  if (self == nullptr) return;
  jstring text = env->NewStringUTF(message != nullptr ? message : "");
  env->CallVoidMethod(self, g_dispatchError, static_cast<jint>(code), text);
  if (text != nullptr) env->DeleteLocalRef(text);
  env->DeleteLocalRef(self);
}

// --- natives ---------------------------------------------------------------------------

jlong nativeCreate(JNIEnv* env, jobject thiz) {
  auto* s = new Session();
  s->self = env->NewWeakGlobalRef(thiz);
  nitrortmp_callbacks_t callbacks{};
  callbacks.on_send = &onSend;
  callbacks.on_state = &onState;
  callbacks.on_error = &onError;
  s->publisher = nitrortmp_publisher_create(&callbacks, s);
  return reinterpret_cast<jlong>(s);
}

void nativeDestroy(JNIEnv* env, jobject, jlong handle) {
  Session* s = session(handle);
  if (s == nullptr) return;
  nitrortmp_publisher_destroy(s->publisher);
  env->DeleteWeakGlobalRef(s->self);
  delete s;
}

jboolean nativeStart(JNIEnv* env, jobject, jlong handle, jstring url) {
  nitrortmp_publisher_t* p = publisher(handle);
  if (p == nullptr || url == nullptr) return JNI_FALSE;
  const char* chars = env->GetStringUTFChars(url, nullptr);
  if (chars == nullptr) return JNI_FALSE;
  const int r = nitrortmp_publisher_start(p, chars);
  env->ReleaseStringUTFChars(url, chars);
  return r == 1 ? JNI_TRUE : JNI_FALSE;
}

void nativeOnReceive(JNIEnv* env, jobject, jlong handle, jbyteArray data, jint length) {
  nitrortmp_publisher_t* p = publisher(handle);
  if (p == nullptr || data == nullptr || length <= 0) return;
  jbyte* bytes = env->GetByteArrayElements(data, nullptr);
  if (bytes == nullptr) return;
  const jsize available = env->GetArrayLength(data);
  const size_t size = static_cast<size_t>(length < available ? length : available);
  nitrortmp_publisher_on_receive(p, reinterpret_cast<const uint8_t*>(bytes), size);
  env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

void nativeStop(JNIEnv*, jobject, jlong handle) {
  nitrortmp_publisher_stop(publisher(handle));
}

jboolean nativePushVideo(JNIEnv* env, jobject, jlong handle, jbyteArray frame, jint pts, jint dts) {
  nitrortmp_publisher_t* p = publisher(handle);
  if (p == nullptr || frame == nullptr) return JNI_FALSE;
  jbyte* bytes = env->GetByteArrayElements(frame, nullptr);
  if (bytes == nullptr) return JNI_FALSE;
  const int r = nitrortmp_publisher_push_video(p, reinterpret_cast<const uint8_t*>(bytes),
                                               static_cast<size_t>(env->GetArrayLength(frame)),
                                               static_cast<uint32_t>(pts), static_cast<uint32_t>(dts));
  env->ReleaseByteArrayElements(frame, bytes, JNI_ABORT);
  return r == 1 ? JNI_TRUE : JNI_FALSE;
}

jboolean nativePushAudio(JNIEnv* env, jobject, jlong handle, jbyteArray frame, jint pts) {
  nitrortmp_publisher_t* p = publisher(handle);
  if (p == nullptr || frame == nullptr) return JNI_FALSE;
  jbyte* bytes = env->GetByteArrayElements(frame, nullptr);
  if (bytes == nullptr) return JNI_FALSE;
  const int r = nitrortmp_publisher_push_audio(p, reinterpret_cast<const uint8_t*>(bytes),
                                               static_cast<size_t>(env->GetArrayLength(frame)),
                                               static_cast<uint32_t>(pts));
  env->ReleaseByteArrayElements(frame, bytes, JNI_ABORT);
  return r == 1 ? JNI_TRUE : JNI_FALSE;
}

void nativeSetMetadata(JNIEnv* env, jobject, jlong handle, jint width, jint height, jdouble frameRate,
                       jdouble videoBitrateKbps, jint audioSampleRate, jint audioChannels,
                       jdouble audioBitrateKbps, jstring encoder) {
  nitrortmp_publisher_t* p = publisher(handle);
  if (p == nullptr) return;
  nitrortmp_metadata_t m{};
  m.width = width;
  m.height = height;
  m.frame_rate = frameRate;
  m.video_bitrate_kbps = videoBitrateKbps;
  m.audio_sample_rate = audioSampleRate;
  m.audio_channels = audioChannels;
  m.audio_bitrate_kbps = audioBitrateKbps;
  const char* chars = encoder != nullptr ? env->GetStringUTFChars(encoder, nullptr) : nullptr;
  m.encoder = chars;
  nitrortmp_publisher_set_metadata(p, &m);
  if (chars != nullptr) env->ReleaseStringUTFChars(encoder, chars);
}

jint nativeState(JNIEnv*, jobject, jlong handle) {
  return nitrortmp_publisher_state(publisher(handle));
}

jlongArray nativeStats(JNIEnv* env, jobject, jlong handle) {
  nitrortmp_stats_t s{};
  nitrortmp_publisher_stats(publisher(handle), &s);
  const jlong values[10] = {
      static_cast<jlong>(s.bytes_sent),        static_cast<jlong>(s.video_tags),
      static_cast<jlong>(s.audio_tags),        static_cast<jlong>(s.script_tags),
      static_cast<jlong>(s.rejected_frames),   static_cast<jlong>(s.dropped_before_keyframe),
      static_cast<jlong>(s.invalid_frames),    static_cast<jlong>(s.timestamp_clamps),
      static_cast<jlong>(s.last_video_timestamp), static_cast<jlong>(s.last_audio_timestamp),
  };
  jlongArray array = env->NewLongArray(10);
  if (array != nullptr) env->SetLongArrayRegion(array, 0, 10, values);
  return array;
}

jobjectArray urlEndpoint(JNIEnv* env, jclass, jstring url) {
  if (url == nullptr) return nullptr;
  const char* chars = env->GetStringUTFChars(url, nullptr);
  if (chars == nullptr) return nullptr;
  nitrortmp_endpoint_t endpoint{};
  const int ok = nitrortmp_url_endpoint(chars, &endpoint);
  env->ReleaseStringUTFChars(url, chars);
  if (ok != 1) return nullptr;
  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray result = env->NewObjectArray(3, stringClass, nullptr);
  if (result == nullptr) return nullptr;
  env->SetObjectArrayElement(result, 0, env->NewStringUTF(endpoint.host));
  env->SetObjectArrayElement(result, 1, env->NewStringUTF(std::to_string(endpoint.port).c_str()));
  env->SetObjectArrayElement(result, 2, env->NewStringUTF(endpoint.use_tls != 0 ? "1" : "0"));
  return result;
}

jstring stateName(JNIEnv* env, jclass, jint state) {
  const char* name = nitrortmp_state_name(state);
  return name != nullptr ? env->NewStringUTF(name) : nullptr;
}

jstring errorName(JNIEnv* env, jclass, jint code) {
  const char* name = nitrortmp_error_name(code);
  return name != nullptr ? env->NewStringUTF(name) : nullptr;
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "()J", reinterpret_cast<void*>(&nativeCreate)},
    {"nativeDestroy", "(J)V", reinterpret_cast<void*>(&nativeDestroy)},
    {"nativeStart", "(JLjava/lang/String;)Z", reinterpret_cast<void*>(&nativeStart)},
    {"nativeOnReceive", "(J[BI)V", reinterpret_cast<void*>(&nativeOnReceive)},
    {"nativeStop", "(J)V", reinterpret_cast<void*>(&nativeStop)},
    {"nativePushVideo", "(J[BII)Z", reinterpret_cast<void*>(&nativePushVideo)},
    {"nativePushAudio", "(J[BI)Z", reinterpret_cast<void*>(&nativePushAudio)},
    {"nativeSetMetadata", "(JIIDDIIDLjava/lang/String;)V", reinterpret_cast<void*>(&nativeSetMetadata)},
    {"nativeState", "(J)I", reinterpret_cast<void*>(&nativeState)},
    {"nativeStats", "(J)[J", reinterpret_cast<void*>(&nativeStats)},
    {"urlEndpoint", "(Ljava/lang/String;)[Ljava/lang/String;", reinterpret_cast<void*>(&urlEndpoint)},
    {"stateName", "(I)Ljava/lang/String;", reinterpret_cast<void*>(&stateName)},
    {"errorName", "(I)Ljava/lang/String;", reinterpret_cast<void*>(&errorName)},
};

}  // namespace

namespace nitrortmp {

bool registerNativePublisher(JavaVM* vm, JNIEnv* env) {
  g_vm = vm;
  jclass clazz = env->FindClass(kClassName);
  if (clazz == nullptr) return false;
  g_dispatchSend = env->GetMethodID(clazz, "dispatchSend", "([B)V");
  g_dispatchState = env->GetMethodID(clazz, "dispatchState", "(I)V");
  g_dispatchError = env->GetMethodID(clazz, "dispatchError", "(ILjava/lang/String;)V");
  if (g_dispatchSend == nullptr || g_dispatchState == nullptr || g_dispatchError == nullptr) return false;
  const jint count = static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0]));
  return env->RegisterNatives(clazz, kMethods, count) == JNI_OK;
}

}  // namespace nitrortmp
