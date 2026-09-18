#pragma once

#include <jni.h>

namespace nitrortmp {

/// Registers the natives of com.margelo.nitro.nitrortmp.NativePublisher and
/// caches its callback method ids. Called from JNI_OnLoad (cpp-adapter.cpp).
bool registerNativePublisher(JavaVM* vm, JNIEnv* env);

}  // namespace nitrortmp
