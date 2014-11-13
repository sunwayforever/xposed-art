#ifndef XPOSED_INTERNAL_H
#define XPOSED_INTERNAL_H

#define ANDROID_SMP 0
#include "xposed.h"
#include <mirror/art_method.h>
#include <mirror/object.h>
#include <mirror/object-inl.h>
#include <mirror/array.h>
#include <mirror/class.h>
#include <well_known_classes.h>
#include <class_linker.h>
#include <primitive.h>
#include <reflection.h>
#include <runtime.h>
#include <scoped_thread_state_change.h>
#include <method_helper.h>
#include <method_helper-inl.h>
#include <handle.h>
#include <util.h>
#include <throw_location.h>
#include <stack.h>

using art::mirror::ArtMethod;
using art::mirror::Array;
using art::mirror::ObjectArray;
using art::mirror::Class;
using art::mirror::Object;
using art::Runtime;
using art::WellKnownClasses;
using art::Primitive;
using art::BoxPrimitive;
using art::UnboxPrimitiveForResult;
using art::JValue;
using art::ScopedObjectAccess;
using art::StackHandleScope;
using art::Handle;
using art::MethodHelper;
using art::StackReference;

#define XPOSED_JAR "system/framework/XposedBridge.jar"
#define XPOSED_CLASS "de/robv/android/xposed/XposedBridge"
#define XPOSED_CLASS_DOTS "de.robv.android.xposed.XposedBridge"

namespace android {
    struct XposedHookInfo {
        jobject reflectedMethod;
        jobject additionalInfo;
        jobject original_method;
    };

    static int xposedReadIntConfig(const char* fileName, int defaultValue);
    bool xposedShouldIgnoreCommand(const char* className, int argc, const char* const argv[]);
    bool addXposedToClasspath(bool zygote);
    bool xposedOnVmCreated(JNIEnv* env, const char* className);

    // fixme
    extern "C" int xposedCallHandler(ArtMethod* original, Object* here, int a, int b, int32_t sp);
    // extern "C" void xposedCallHandler(ArtMethod* original, uint32_t* args, JValue* pResult);
    // static void xposedCallHandler(const u4* args, JValue* pResult, const ArtMethod* method, ::Thread* self);
    static bool xposedIsHooked(ArtMethod* method);

    static jboolean de_robv_android_xposed_XposedBridge_initNative(JNIEnv* env, jclass clazz);
    static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject javaMethod, jobject additionalInfoIndirect);
    // fixme
    // static void de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult, const ArtMethod* method, ::Thread* self);
    static void android_content_res_XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz, jint parserPtr, jobject origRes, jobject repRes);
    static jobject de_robv_android_xposed_XposedBridge_getStartClassName(JNIEnv* env, jclass clazz);

    static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env);

} // namespace android

#endif  // XPOSED_INTERNAL_H
