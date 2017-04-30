LOCAL_PATH:= $(call my-dir)
FULL_PATH:=$(realpath .)
include $(CLEAR_VARS)

LIBVNCSERVER_DIR:=${FULL_PATH}/../../libvncserver
COMMON_DIR:=${FULL_PATH}/../../common

LIBVNCSERVER_SOURCES:= \
    ${LIBVNCSERVER_DIR}/main.c \
    ${LIBVNCSERVER_DIR}/rfbserver.c \
    ${LIBVNCSERVER_DIR}/rfbregion.c \
    ${LIBVNCSERVER_DIR}/auth.c \
    ${LIBVNCSERVER_DIR}/sockets.c \
    ${LIBVNCSERVER_DIR}/stats.c \
    ${LIBVNCSERVER_DIR}/corre.c \
    ${LIBVNCSERVER_DIR}/hextile.c \
    ${LIBVNCSERVER_DIR}/rre.c \
    ${LIBVNCSERVER_DIR}/translate.c \
    ${LIBVNCSERVER_DIR}/cutpaste.c \
    ${LIBVNCSERVER_DIR}/httpd.c \
    ${LIBVNCSERVER_DIR}/cursor.c \
    ${LIBVNCSERVER_DIR}/font.c \
    ${LIBVNCSERVER_DIR}/draw.c \
    ${LIBVNCSERVER_DIR}/selbox.c \
    ${COMMON_DIR}/d3des.c \
    ${COMMON_DIR}/vncauth.c \
    ${LIBVNCSERVER_DIR}/cargs.c \
    ${COMMON_DIR}/minilzo.c \
    ${LIBVNCSERVER_DIR}/ultra.c \
    ${LIBVNCSERVER_DIR}/scale.c \
    $(LIBVNCSERVER_DIR)/zlib.c \
    $(LIBVNCSERVER_DIR)/zrle.c \
    $(LIBVNCSERVER_DIR)/zrleoutstream.c \
    $(LIBVNCSERVER_DIR)/zrlepalettehelper.c

LOCAL_CFLAGS += \
  -Wall \
  -Wno-unused-variable \
  -Wno-maybe-uninitialized \
  -Wno-unused-but-set-variable

LOCAL_LDLIBS +=  -llog -lz -ldl

LOCAL_SRC_FILES += \
	$(LIBVNCSERVER_SOURCES) \
	androidvncserver.c

LOCAL_C_INCLUDES += \
	$(FULL_PATH) \
	$(COMMON_DIR) \
	$(LIBVNCSERVER_DIR) \
	${FULL_PATH}/../..

LOCAL_MODULE := androidvncserver

include $(BUILD_EXECUTABLE)
