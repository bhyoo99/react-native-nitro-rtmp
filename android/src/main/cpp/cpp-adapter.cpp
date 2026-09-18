#include <jni.h>
#include "nitrortmpOnLoad.hpp"
#include "NativeCodec.hpp"
#include "NativePublisher.hpp"

#include <fbjni/fbjni.h>


JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::jni::initialize(vm, [vm]() {
    margelo::nitro::nitrortmp::registerAllNatives();
    // The JNI bridge to the RTMP core (NativePublisher.kt <-> nitrortmp_c.h).
    if (!nitrortmp::registerNativePublisher(vm, facebook::jni::Environment::current())) {
      throw std::runtime_error("nitrortmp: failed to register NativePublisher natives");
    }
    // The encoder output helpers (NativeCodec.kt <-> nitrortmp_c.h).
    if (!nitrortmp::registerNativeCodec(facebook::jni::Environment::current())) {
      throw std::runtime_error("nitrortmp: failed to register NativeCodec natives");
    }
  });
}
