LOCAL_PATH := $(QFUSION_PATH)/libsrcs/src/libjpeg
include $(CLEAR_VARS)
LOCAL_MODULE := jpeg

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

LOCAL_SRC_FILES := \
  jaricom.c \
  jcapimin.c \
  jcapistd.c \
  jcarith.c \
  jccoefct.c \
  jccolor.c \
  jcdctmgr.c \
  jchuff.c \
  jcinit.c \
  jcmainct.c \
  jcmarker.c \
  jcmaster.c \
  jcomapi.c \
  jcparam.c \
  jcprepct.c \
  jcsample.c \
  jctrans.c \
  jdapimin.c \
  jdapistd.c \
  jdarith.c \
  jdatadst.c \
  jdatasrc.c \
  jdcoefct.c \
  jdcolor.c \
  jddctmgr.c \
  jdhuff.c \
  jdinput.c \
  jdmainct.c \
  jdmarker.c \
  jdmaster.c \
  jdmerge.c \
  jdpostct.c \
  jdsample.c \
  jdtrans.c \
  jerror.c \
  jfdctflt.c \
  jfdctfst.c \
  jfdctint.c \
  jidctflt.c \
  jidctfst.c \
  jidctint.c \
  jmemmgr.c \
  jmemnobs.c \
  jquant1.c \
  jquant2.c \
  jutils.c

include $(BUILD_STATIC_LIBRARY)
