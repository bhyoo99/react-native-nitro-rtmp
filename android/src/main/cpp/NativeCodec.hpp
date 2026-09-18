#pragma once

#include <jni.h>

namespace nitrortmp {

/// Registers the natives of com.margelo.nitro.nitrortmp.NativeCodec (the
/// encoder output helpers of nitrortmp_c.h). Called from JNI_OnLoad.
bool registerNativeCodec(JNIEnv* env);

}  // namespace nitrortmp
