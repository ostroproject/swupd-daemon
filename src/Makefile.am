INTERFACES_DIR = $(top_srcdir)/interfaces

bin_PROGRAMS = swupdd

DBUS_SOURCES = swupdd-interface.c swupdd-interface.h
BUILT_SOURCES = $(DBUS_SOURCES)

$(DBUS_SOURCES) : $(INTERFACES_DIR)/org.O1.swupdd.Client.xml
	gdbus-codegen \
		--interface-prefix=org.O1.swupdd.Client \
		--c-namespace=ClrSoftwareUpdate \
		--generate-c-code=swupdd-interface \
		$<

swupdd_SOURCES = \
	$(DBUS_SOURCES) \
	swupdd-main.c \
	$(NULL)

swupdd_CFLAGS = \
	-I$(top_builddir) \
	-I$(top_builddir)/src \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src \
	$(SWUPDD_CFLAGS) \
	$(NULL)

swupdd_LDADD = \
	$(SWUPDD_LIBS) \
	$(NULL)

CLEANFILES = $(BUILT_SOURCES)

