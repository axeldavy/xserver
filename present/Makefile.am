noinst_LTLIBRARIES = libpresent.la
libpresent_la_CFLAGS = \
	-DHAVE_XORG_CONFIG_H \
	@DIX_CFLAGS@ @XORG_CFLAGS@

libpresent_la_SOURCES = \
	present.h \
	present.c \
	present_event.c \
	present_fake.c \
	present_fence.c \
	present_notify.c \
	present_priv.h \
	present_request.c \
	present_screen.c

if WAYLAND
noinst_LTLIBRARIES += libpresentwayland.la

libpresentwayland_la_CFLAGS = \
	-DHAVE_XORG_CONFIG_H \
	-DPRESENT_WAYLAND\
	@DIX_CFLAGS@ @XORG_CFLAGS@

libpresentwayland_la_SOURCES = \
	present.h \
	present.c \
	present_event.c \
	present_fake.c \
	present_fence.c \
	present_notify.c \
	present_priv.h \
	present_request.c \
	present_screen.c \
	present_wayland.c
endif
sdk_HEADERS = present.h presentext.h
