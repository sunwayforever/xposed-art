LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	xposed.cpp \
	xposed_quick_code_stub.S

LOCAL_CFLAGS += -std=c++0x
LOCAL_C_INCLUDES := \
	$(JNI_H_INCLUDE) \
	art/runtime/ \
	dalvik/ \
	$(ART_C_INCLUDES) \
	external/gtest/include \
	external/valgrind/main/include \
	external/valgrind/main/ \

include external/libcxx/libcxx.mk


LOCAL_SHARED_LIBRARIES := libutils liblog libart libc++ libcutils

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE := libxposed

include $(BUILD_SHARED_LIBRARY)
