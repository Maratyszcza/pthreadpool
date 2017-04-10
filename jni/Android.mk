LOCAL_PATH := $(call my-dir)/..

include $(CLEAR_VARS)
LOCAL_MODULE := gtest
LOCAL_SRC_FILES := $(LOCAL_PATH)/deps/googletest/googletest/src/gtest-all.cc
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/deps/googletest/googletest/include
LOCAL_C_INCLUDES := $(LOCAL_EXPORT_C_INCLUDES) $(LOCAL_PATH)/deps/googletest/googletest
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := pthreadpool
LOCAL_SRC_FILES := $(LOCAL_PATH)/src/threadpool-pthreads.c
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_C_INCLUDES := $(LOCAL_EXPORT_C_INCLUDES) $(LOCAL_PATH)/deps/fxdiv/include
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := pthreadpool-test
LOCAL_SRC_FILES := $(LOCAL_PATH)/test/pthreadpool.cc
LOCAL_C_INCLUDES := 
LOCAL_CFLAGS := -D__STDC_CONSTANT_MACROS=1
LOCAL_LDLIBS := -latomic
LOCAL_STATIC_LIBRARIES := pthreadpool gtest
include $(BUILD_EXECUTABLE)

