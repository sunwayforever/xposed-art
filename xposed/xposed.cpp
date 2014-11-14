/*
 * Xposed enables "god mode" for developers
 */

#define LOG_TAG "Xposed"

#include "xposed_internal.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>
#include <stdio.h>
#include <sys/mman.h>
#include <cutils/properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <entrypoints/entrypoint_utils.h>

#include "xposed_offsets.h"
namespace android {
    Class* objectArrayClass = NULL;
    jclass xposedClass = NULL;
    ArtMethod* xposedHandleHookedMethod = NULL;
    const char* startClassName = NULL;
    void* PTR_gDvmJit = NULL;
    size_t arrayContentsOffset = 0;

    bool addXposedToClasspath(bool zygote) {
        if (access(XPOSED_JAR, R_OK) == 0) {
            char* oldClassPath = getenv("CLASSPATH");
            if (oldClassPath == NULL) {
                setenv("CLASSPATH", XPOSED_JAR, 1);
            } else {
                char classPath[4096];
                int neededLength = snprintf(classPath, sizeof(classPath), "%s:%s", XPOSED_JAR, oldClassPath);
                if (neededLength >= (int)sizeof(classPath)) {
                    ALOGE("ERROR: CLASSPATH would exceed %d characters", sizeof(classPath));
                    return false;
                }
                setenv("CLASSPATH", classPath, 1);
            }
            ALOGI("Added Xposed (%s) to CLASSPATH.\n", XPOSED_JAR);
            return true;
        } else {
            ALOGE("ERROR: could not access Xposed jar '%s'\n", XPOSED_JAR);
            return false;
        }
    }

    bool xposedOnVmCreated(JNIEnv* env, const char* className) {
        ALOGE("xposed: xposedOnVmCreated: className: %s", className);
        startClassName = className;

        xposedClass = env->FindClass(XPOSED_CLASS);
        xposedClass = reinterpret_cast<jclass>(env->NewGlobalRef(xposedClass));
    
        if (xposedClass == NULL) {
            ALOGE("xposed: Error while loading Xposed class '%s':\n", XPOSED_CLASS);
            env->ExceptionClear();
            return false;
        }
    
        ALOGE("xposed: Found Xposed class '%s', now initializing\n", XPOSED_CLASS);
        if (register_de_robv_android_xposed_XposedBridge(env) != JNI_OK) {
            ALOGE("xposed: Could not register natives for '%s'\n", XPOSED_CLASS);
            env->ExceptionClear();
            return false;
        }
        return true;
    }

    int64_t xposedCallHandler(ArtMethod* original_method, Object* thiz, int r1, int r2, int32_t sp) {
        LOG(ERROR) << "xposed: >>> xposedCallHandler for " << art::PrettyMethod(original_method);
        art::Thread* self = art::Thread::Current();
        ScopedObjectAccess soa(self);

        Object* this_object = nullptr;
        if (! original_method->IsStatic()) {
            this_object = thiz;
        }
        uint32_t shorty_len = 0;
        const char * shorty = original_method->GetShorty(&shorty_len);

        LOG(ERROR) << "xposed: shorty: " << shorty << " len: " << shorty_len;
        char return_type = *shorty;
        shorty++;
        shorty_len--;

        int32_t args[shorty_len+3];
        memset(args, shorty_len+3, 0);
        int tmp=0;
        if (this_object == nullptr) {
            args[tmp++]=(int)thiz;
        }
        args[tmp++]=r1;
        args[tmp++]=r2;
        if (shorty_len > tmp) {
            memcpy(args+tmp, (&sp)+4, (shorty_len-tmp)*4);
        }

        LOG(ERROR) << "xposed: dump args";
        LOG(ERROR) << "xposed: this:" << this_object;        
        for (int i=0; i<shorty_len; i++) {
            LOG(ERROR) << "arg:" << i << args[i];
        }
       
        Class* object_array_class = Runtime::Current()->GetClassLinker()->FindSystemClass(self,"[Ljava/lang/Object;");
        ObjectArray<Object>* args_array = ObjectArray<Object>::Alloc(self,object_array_class,shorty_len);

        for (int i=0; i<shorty_len; i++) {
            char desc = shorty[i];
            Object* obj;
            JValue value;
            switch (desc) {
                case 'Z':case 'C':case 'F':case 'B':case 'S':case 'I':
                    value.SetI(args[i]);
                    obj = BoxPrimitive(Primitive::GetType(desc), value);
                    break;
                case 'D': case 'J': {
                    long j;
                    memcpy(&j, &args[i++], sizeof(j));
                    value.SetJ(j);
                    obj = BoxPrimitive(Primitive::Type::kPrimLong, value);
                    break;
                }
                case '[':case 'L':
                    obj  = (Object*) args[i];
                    ALOGE("get object for arg: class is %s", obj->GetClass()->GetName()->ToModifiedUtf8().c_str());
                    break;
                default:
                    ALOGE("xposed: Unknown method signature description character: %c\n", desc);
                    obj = NULL;
            }
            args_array->Set(i, obj);
        }
        LOG(ERROR) << "xposed: before InvokeXposedWithVarArgs";
        uint32_t arguments[4]={0};
        XposedHookInfo* hookInfo = (XposedHookInfo*)original_method->GetEntryPointFromInterpreter();
        
        arguments[0]=(uint32_t)(soa.Decode<Object*> (hookInfo->reflectedMethod));
        arguments[1]=(uint32_t)(soa.Decode<Object*> (hookInfo->additionalInfo));
        arguments[2]=(uint32_t)this_object;
        arguments[3]=(uint32_t)args_array;

        JValue ret_value;
        xposedHandleHookedMethod->Invoke(self, arguments, 16, &ret_value, xposedHandleHookedMethod->GetShorty());
        LOG(ERROR) << "xposed: after InvokeXposedWithVarArgs";

        // need check exception

        if (return_type == 'V') {
            return 0;
        }

        if (return_type != 'L' || return_type != '[') {
            StackHandleScope<1> hs(self);
            MethodHelper mh_interface_method(hs.NewHandle(original_method));
            art::ThrowLocation throw_location;
            UnboxPrimitiveForResult(throw_location, ret_value.GetL(), mh_interface_method.GetReturnType(), &ret_value);
        }

        return ret_value.GetJ();
    }

    static jboolean de_robv_android_xposed_XposedBridge_initNative(JNIEnv* env, jclass clazz) {
        xposedHandleHookedMethod = (ArtMethod*) env->GetStaticMethodID(
            xposedClass,
            "handleHookedMethod",
            "(Ljava/lang/reflect/Member;Ljava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        
        if (xposedHandleHookedMethod == NULL) {
            ALOGE("ERROR: could not find method %s.handleHookedMethod(Member, Object, Object, Object[])\n", XPOSED_CLASS);
            env->ExceptionClear();
            return false;
        }
        return true;
    }

    static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject javaMethod, jobject additionalInfoIndirect) {
        ALOGE("xposed: >>>de_robv_android_xposed_XposedBridge_hookMethodNative");
        ScopedObjectAccess soa(env);
        art::Thread* self = art::Thread::Current();
        ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
        if (method == NULL) {
            return;
        }
        if (method->IsStatic()) {
            StackHandleScope<2> shs(art::Thread::Current());
            Runtime::Current()->GetClassLinker()->EnsureInitialized(shs.NewHandle(method->GetDeclaringClass()), true, true);
        }
        if (xposedIsHooked(method)) {
            return;
        }
        LOG(ERROR) << "xposed: hookMethodNative for " << art::PrettyMethod(method);
        
        XposedHookInfo* hookInfo = (XposedHookInfo*) calloc(1, sizeof(XposedHookInfo));

        hookInfo->reflectedMethod = env->NewGlobalRef(javaMethod);
        hookInfo->additionalInfo = env->NewGlobalRef(additionalInfoIndirect);
        
        method->SetEntryPointFromQuickCompiledCode((void*)&xposedCallHandler);
        method->SetEntryPointFromInterpreter((art::mirror::EntryPointFromInterpreter*) hookInfo);

        ALOGE("xposed: <<<de_robv_android_xposed_XposedBridge_hookMethodNative");
    }

    static bool xposedIsHooked(ArtMethod* method) {
        return (method->GetEntryPointFromQuickCompiledCode()) == (void *)xposedCallHandler;
    }

    extern "C" void xposed_quick_invoke_stub(ArtMethod*, uint32_t*, uint32_t, art::Thread*, const void*, JValue *);
    
    extern "C" jobject de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative (
        JNIEnv* env, jclass clazz, jobject java_method, jobject thiz, jobject args) {
        ScopedObjectAccess soa(env);
        art::Thread* self = art::Thread::Current();

        LOG(ERROR) << "xposed: >>> invokeOriginalMethodNative";
        
        ArtMethod* method = ArtMethod::FromReflectedMethod(soa, java_method);
        XposedHookInfo* hookInfo = (XposedHookInfo*)method->GetEntryPointFromInterpreter();

        Object* this_object = NULL;
        if (! method->IsStatic()) {
            this_object = soa.Decode<Object*> (thiz);
        }
        const void* quick_code = Runtime::Current()->GetClassLinker()->GetQuickOatCodeFor(method);

        // todo: convert this_object+jobject[] to args
        uint32_t shorty_len = 0;
        const char * shorty = method->GetShorty(&shorty_len);
        char return_type = *shorty;
        shorty++;
        shorty_len--;

        uint32_t* arg_array = (uint32_t*) malloc(shorty_len*2*sizeof(uint32_t));
        memset(arg_array, 0, sizeof(arg_array));
        int num_bytes = 0;
        ClassLinker* linker = Runtime::Current()->GetClassLinker();
        if (this_object != NULL) {
            arg_array[0] = (uint32_t)this_object;
            num_bytes += 4;
        }
        ObjectArray<Object>* object_array = soa.Decode< ObjectArray<Object>* >(args);
        for (int i=0;i<shorty_len; i++) {
            char desc = shorty[i];
            if (desc == 'L' || desc == '[') {
                arg_array[num_bytes/4] = (uint32_t) (object_array->Get(i));
                num_bytes += 4;
            } else {
                JValue value;
                UnboxPrimitiveForField(object_array->Get(i), linker->FindPrimitiveClass(desc), nullptr, &value);
                switch (desc) {
                    case 'Z': case 'B': case 'C': case 'S': case 'I': case 'F':
                        arg_array[num_bytes/4] = value.GetI();
                        num_bytes += 4;
                        break;
                    case 'D': case 'J':
                        uint64_t dvalue = value.GetJ();
                        arg_array[num_bytes / 4] = dvalue;
                        arg_array[(num_bytes / 4) + 1] = dvalue >> 32;
                        num_bytes += 8;
                        break;
                }
            }
        }        

        JValue result;
        ALOGE("xposed: xposed_quick_invoke_stub: quick_code: %p, result: %p, num_bytes: %d, arg_array: %p", quick_code, &result, num_bytes, arg_array);
        (*xposed_quick_invoke_stub)(method, arg_array, num_bytes, self, quick_code, &result);
        free(arg_array);
        
        if (return_type == '[' || return_type == 'L') {
            return soa.AddLocalReference<jobject> (result.GetL());
        } else {
            return soa.AddLocalReference<jobject> (BoxPrimitive(Primitive::GetType(return_type), result));
        }
    }

    static jobject de_robv_android_xposed_XposedBridge_getStartClassName(JNIEnv* env, jclass clazz) {
        return env->NewStringUTF(startClassName);
    }

    static const JNINativeMethod xposedMethods[] = {
        {"getStartClassName", "()Ljava/lang/String;", (void*)de_robv_android_xposed_XposedBridge_getStartClassName},
        {"initNative", "()Z", (void*)de_robv_android_xposed_XposedBridge_initNative},
        {"hookMethodNative", "(Ljava/lang/reflect/Member;Ljava/lang/Object;)V", (void*)de_robv_android_xposed_XposedBridge_hookMethodNative},
        {"invokeOriginalMethod", "(Ljava/lang/reflect/Member;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
         (void*)de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative},
    };

    static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env) {
        return env->RegisterNatives(xposedClass, xposedMethods, sizeof(xposedMethods)/sizeof(xposedMethods[0]));
    }

}

