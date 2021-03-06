SUBDIRS = pages

BUILT_SOURCES =

uidir = $(datadir)/gnome-initial-setup

AM_CPPFLAGS = \
	$(INITIAL_SETUP_CFLAGS) \
	-DSECRET_API_SUBJECT_TO_CHANGE \
	-DGCR_API_SUBJECT_TO_CHANGE \
	-DVENDOR_CONF_FILE="\"$(VENDOR_CONF_FILE)\"" \
	-DUIDIR="\"$(uidir)\"" \
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\" \
	-DLIBLOCALEDIR=\""$(prefix)/lib/locale"\" \
	-DCONFIGDIR=\"$(sysconfdir)/$(PACKAGE)\" \
	-DDATADIR=\""$(datadir)"\" \
	-DLOCALSTATEDIR=\""$(localstatedir)"\" \
	-DLIBEXECDIR=\""$(libexecdir)"\"

libexec_PROGRAMS = gnome-initial-setup gnome-initial-setup-copy-worker

assistant_resource_files = $(shell glib-compile-resources --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/gis-assistant.gresource.xml)
gis-assistant-resources.c: gis-assistant.gresource.xml $(assistant_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-source $<
gis-assistant-resources.h: gis-assistant.gresource.xml $(assistant_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-header $<
BUILT_SOURCES += gis-assistant-resources.c gis-assistant-resources.h

page_util_resource_files = $(shell glib-compile-resources --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/gis-page-util.gresource.xml)
gis-page-util-resources.c: gis-page-util.gresource.xml $(page_util_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-source $<
gis-page-util-resources.h: gis-page-util.gresource.xml $(page_util_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-header $<
BUILT_SOURCES += gis-page-util-resources.c gis-page-util-resources.h


fbe-remote-generated.c: fbe-remote-generated.h
fbe-remote-generated.h: $(srcdir)/com.endlessm.Tutorial.FBERemote.xml
	$(AM_V_GEN) gdbus-codegen		\
	--interface-prefix=com.endlessm.Tutorial \
	--generate-c-code=fbe-remote-generated $<
BUILT_SOURCES += fbe-remote-generated.c fbe-remote-generated.h

gnome_initial_setup_SOURCES = \
	$(BUILT_SOURCES) \
	gnome-initial-setup.c gnome-initial-setup.h \
	gis-assistant.c gis-assistant.h \
	gis-page.c gis-page.h \
	gis-page-util.c gis-page-util.h \
	gis-pkexec.c gis-pkexec.h \
	gis-driver.c gis-driver.h \
	gis-keyring.c gis-keyring.h \
	gis-window.c gis-window.h

gnome_initial_setup_LDADD =	\
	pages/branding-welcome/libgisbrandingwelcome.la \
	pages/language/libgislanguage.la \
	pages/keyboard/libgiskeyboard.la \
	pages/display/libgisdisplay.la \
	pages/endless-eula/libgisendlesseula.la \
	pages/eulas/libgiseulas.la \
	pages/network/libgisnetwork.la \
	pages/live-chooser/libgislivechooser.la \
	pages/location/libgislocation.la \
	pages/account/libgisaccount.la \
	pages/goa/libgisgoa.la \
	pages/summary/libgissummary.la \
	$(INITIAL_SETUP_LIBS) \
	-lm

gnome_initial_setup_copy_worker_SOURCES =		\
	gnome-initial-setup-copy-worker.c

gnome_initial_setup_copy_worker_CFLAGS = \
	$(COPY_WORKER_CFLAGS)

gnome_initial_setup_copy_worker_LDADD = \
	$(COPY_WORKER_LIBS)

EXTRA_DIST = \
	com.endlessm.Tutorial.FBERemote.xml \
	gis-assistant.gresource.xml \
	gis-page-util.gresource.xml \
	$(assistant_resource_files) \
	$(page_util_resource_files)
