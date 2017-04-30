NDK_TOOLCHAIN_VERSION=4.9

APP_CFLAGS += -Ofast \
	-funroll-loops \
	-fno-strict-aliasing

APP_ABI:=armeabi 
APP_PLATFORM := android-25

