bin_SCRIPTS = abrt-debuginfo-install abrt-handle-upload

sbin_PROGRAMS = abrtd

abrtd_SOURCES = \
    PluginManager.h PluginManager.cpp \
    rpm.h rpm.c \
    MiddleWare.h MiddleWare.cpp \
    CrashWatcher.h CrashWatcher.cpp \
    CommLayerServer.h CommLayerServer.cpp \
    CommLayerServerDBus.h CommLayerServerDBus.cpp \
    Daemon.h Daemon.cpp \
    Settings.h Settings.cpp \
    dumpsocket.h dumpsocket.cpp
abrtd_CPPFLAGS = \
    -I$(srcdir)/../../inc \
    -I$(srcdir)/../../lib/utils \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(DBUS_CFLAGS) \
    $(XMLRPC_CFLAGS) $(XMLRPC_CLIENT_CFLAGS) \
    $(ENABLE_SOCKET_OR_DBUS) \
    -D_GNU_SOURCE \
    -Wall
abrtd_LDADD = \
    ../../lib/utils/libABRTUtils.la \
    ../../lib/utils/libABRTdUtils.la \
    $(DL_LIBS) \
    $(DBUS_LIBS) \
    $(RPM_LIBS) \
    $(XMLRPC_LIBS) $(XMLRPC_CLIENT_LIBS)

dbusabrtconfdir = ${sysconfdir}/dbus-1/system.d/
dist_dbusabrtconf_DATA = dbus-abrt.conf

daemonconfdir = $(CONF_DIR)
dist_daemonconf_DATA = \
    abrt.conf \
    gpg_keys

polkitconfdir = ${datadir}/polkit-1/actions
dist_polkitconf_DATA = org.fedoraproject.abrt.policy

comredhatabrtservicedir = ${datadir}/dbus-1/system-services
dist_comredhatabrtservice_DATA = com.redhat.abrt.service

man_MANS = abrtd.8 abrt.conf.5

EXTRA_DIST = $(man_MANS) abrt-debuginfo-install abrt-handle-upload

if HAVE_SYSTEMD
dist_systemdsystemunit_DATA = \
        abrtd.service
endif

DEFS = -DLOCALEDIR=\"$(localedir)\" @DEFS@

install-data-local:
	$(mkdir_p) '$(DESTDIR)/$(VAR_RUN)'