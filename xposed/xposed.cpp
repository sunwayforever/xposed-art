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

    int xposedCallHandler(ArtMethod* original_method, Object* thiz, int r1, int r2, int32_t sp) {
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
        if (return_type == 'L' || return_type == '[') {
            return (long)ret_value.GetL();
        }
        
        StackHandleScope<1> hs(self);
        MethodHelper mh_interface_method(hs.NewHandle(original_method));
        art::ThrowLocation throw_location;
        UnboxPrimitiveForResult(throw_location, ret_value.GetL(), mh_interface_method.GetReturnType(), &ret_value);
            
        long ret=0;
        switch (return_type) {
            case 'Z':
                ret=(long)ret_value.GetZ();
                break;
            case 'C':
                ret=(long)ret_value.GetC();
                break;
            case 'F':
                ret=(long)ret_value.GetC();
                break;
            case 'B':
                ret=(long)ret_value.GetB();
                break;
            case 'S':
                ret=(long)ret_value.GetS();
                break;
            case 'I':
                ret=(long)ret_value.GetI();
                break;
            case 'D':
                ret=(long)ret_value.GetD();
                break;
            case 'J': 
                ret=(long)ret_value.GetJ();
                break;
            default:
                break;
        }        
        ALOGE("xposed: <<<xposedCallHandler: ret is %ld", ret);
        return ret;
    }

    static jboolean de_robv_android_xposed_XposedBridge_initNative(JNIEnv* env, jclass clazz) {
        xposedHandleHookedMethod = (ArtMethod*) env->GetStaticMethodID(xposedClass, "handleHookedMethod",
                                                                      "(Ljava/lang/reflect/Member;Ljava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        if (xposedHandleHookedMethod == NULL) {
            ALOGE("ERROR: could not find method %s.handleHookedMethod(Member, Object, Object, Object[])\n", XPOSED_CLASS);
            env->ExceptionClear();
            return false;
        }

        ArtMethod* xposedInvokeOriginalMethodNative = (ArtMethod*) env->GetStaticMethodID(xposedClass, "invokeOriginalMethodNative",
                                                                                         "(Ljava/lang/reflect/Member;[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        if (xposedInvokeOriginalMethodNative == NULL) {
            ALOGE("ERROR: could not find method %s.invokeOriginalMethodNative(Member, Class[], Class, Object, Object[])\n", XPOSED_CLASS);
            env->ExceptionClear();
            return false;
        }
        // fixme
        // dvmSetNativeFunc(xposedInvokeOriginalMethodNative, de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative, NULL);

        // objectArrayClass = dvmFindArrayClass("[Ljava/lang/Object;", NULL);
        // if (objectArrayClass == NULL) {
        //     ALOGE("Error while loading Object[] class");
        //     env->ExceptionClear();
        //     return false;
        // }

        // xresourcesClass = env->FindClass(XRESOURCES_CLASS);
        // xresourcesClass = reinterpret_cast<jclass>(env->NewGlobalRef(xresourcesClass));
        // if (xresourcesClass == NULL) {
        //     ALOGE("Error while loading XResources class '%s':\n", XRESOURCES_CLASS);
        //     env->ExceptionClear();
        //     return false;
        // }
        // if (register_android_content_res_XResources(env) != JNI_OK) {
        //     ALOGE("Could not register natives for '%s'\n", XRESOURCES_CLASS);
        //     env->ExceptionClear();
        //     return false;
        // }

        // xresourcesTranslateResId = env->GetStaticMethodID(xresourcesClass, "translateResId",
        //                                                   "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)I");
        // if (xresourcesTranslateResId == NULL) {
        //     ALOGE("ERROR: could not find method %s.translateResId(int, Resources, Resources)\n", XRESOURCES_CLASS);
        //     env->ExceptionClear();
        //     return false;
        // }

        // xresourcesTranslateAttrId = env->GetStaticMethodID(xresourcesClass, "translateAttrId",
        //                                                    "(Ljava/lang/String;Landroid/content/res/XResources;)I");
        // if (xresourcesTranslateAttrId == NULL) {
        //     ALOGE("ERROR: could not find method %s.findAttrId(String, Resources, Resources)\n", XRESOURCES_CLASS);
        //     env->ExceptionClear();
        //     return false;
        // }

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
        // hookInfo->reflectedMethod = soa.Decode<Object*>(env->NewGlobalRef(javaMethod));
        // hookInfo->additionalInfo = soa.Decode<Object*> (env->NewGlobalRef(additionalInfoIndirect));

        hookInfo->reflectedMethod = env->NewGlobalRef(javaMethod);
        hookInfo->additionalInfo = env->NewGlobalRef(additionalInfoIndirect);

        ALOGE("xposed: hookInfo: %p, %p", soa.Decode<Object*> (hookInfo->reflectedMethod), soa.Decode<Object*> (hookInfo->additionalInfo));
        
        method->SetEntryPointFromQuickCompiledCode((void*)&xposedCallHandler);
        method->SetEntryPointFromInterpreter((art::mirror::EntryPointFromInterpreter*) hookInfo);

        ALOGE("xposed: <<<de_robv_android_xposed_XposedBridge_hookMethodNative");
    }

    static bool xposedIsHooked(ArtMethod* method) {
        return (method->GetEntryPointFromQuickCompiledCode()) == (void *)xposedCallHandler;
    }

    // fixme
    // static void de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult,
    //                                                                            const Method* method, ::Thread* self) {
    //     Method* meth = (Method*) args[1];
    //     if (meth == NULL) {
    //         meth = dvmGetMethodFromReflectObj((Object*) args[0]);
    //         if (xposedIsHooked(meth)) {
    //             meth = (Method*) meth->insns;
    //         }
    //     }
    //     ArrayObject* params = (ArrayObject*) args[2];
    //     ClassObject* returnType = (ClassObject*) args[3];
    //     Object* thisObject = (Object*) args[4]; // null for static methods
    //     ArrayObject* argList = (ArrayObject*) args[5];

    //     // invoke the method
    //     pResult->l = dvmInvokeMethod(thisObject, meth, argList, params, returnType, true);
    //     return;
    // }

    static jobject de_robv_android_xposed_XposedBridge_getStartClassName(JNIEnv* env, jclass clazz) {
        return env->NewStringUTF(startClassName);
    }

    static const JNINativeMethod xposedMethods[] = {
        {"getStartClassName", "()Ljava/lang/String;", (void*)de_robv_android_xposed_XposedBridge_getStartClassName},
        {"initNative", "()Z", (void*)de_robv_android_xposed_XposedBridge_initNative},
        {"hookMethodNative", "(Ljava/lang/reflect/Member;Ljava/lang/Object;)V", (void*)de_robv_android_xposed_XposedBridge_hookMethodNative},
    };

    static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env) {
        return env->RegisterNatives(xposedClass, xposedMethods, sizeof(xposedMethods)/sizeof(xposedMethods[0]));
    }

}

