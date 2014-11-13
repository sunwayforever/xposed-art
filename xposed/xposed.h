#ifndef XPOSED_H_
#define XPOSED_H_

#include <jni.h>

#define XPOSED_CLASS_DOTS "de.robv.android.xposed.XposedBridge"
namespace android {
    bool addXposedToClasspath(bool zygote);
    bool xposedOnVmCreated(JNIEnv* env, const char* className);
} 

#endif  // XPOSED_H_
