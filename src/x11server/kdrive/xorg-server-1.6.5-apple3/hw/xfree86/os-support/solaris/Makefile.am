if SOLARIS_USL_CONSOLE
VTSW_SRC = $(srcdir)/../shared/VTsw_usl.c
else
VTSW_SRC = $(srcdir)/../shared/VTsw_noop.c
endif

# TODO: Don't build agpgart on SPARC
#if defined(i386Architecture) || defined(AMD64Architecture)
AGP_SRC = sun_agp.c
#else
#AGP_SRC = $(srcdir)/../shared/agp_noop.c
#endif

SOLARIS_INOUT_SRC = solaris-@SOLARIS_INOUT_ARCH@.S
DISTCLEANFILES = solaris-@SOLARIS_INOUT_ARCH@.il

solaris-@SOLARIS_INOUT_ARCH@.il: solaris-@SOLARIS_INOUT_ARCH@.S
	$(CPP) -P -DINLINE_ASM solaris-@SOLARIS_INOUT_ARCH@.S > $@

noinst_LTLIBRARIES = libsolaris.la
libsolaris_la_SOURCES = sun_init.c \
	sun_vid.c sun_bell.c $(AGP_SRC) sun_apm.c \
	$(srcdir)/../shared/kmod_noop.c \
	$(srcdir)/../shared/posix_tty.c $(srcdir)/../shared/sigiostubs.c \
	$(srcdir)/../shared/stdResource.c \
	$(srcdir)/../shared/vidmem.c \
	$(VTSW_SRC)
nodist_libsolaris_la_SOURCES = $(SOLARIS_INOUT_SRC)

sdk_HEADERS = agpgart.h
nodist_sdk_HEADERS = solaris-@SOLARIS_INOUT_ARCH@.il

AM_CFLAGS = -DUSESTDRES -DHAVE_SYSV_IPC $(XORG_CFLAGS) $(DIX_CFLAGS)

INCLUDES = $(XORG_INCS) 

EXTRA_DIST = solaris-amd64.S solaris-ia32.S solaris-sparcv8plus.S \
	apSolaris.shar sun_inout.s
