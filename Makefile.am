# We are building a module for Asterisk.
# This is the name of the module file.
TARGET=chan_dongle_ng.so

# The source files for the module.
SOURCES=chan_dongle_ng.c

# Tell automake that this is a library to be built.
lib_LTLIBRARIES = $(TARGET)

# Automake will create chan_dongle_ng_la-chan_dongle_ng.lo from chan_dongle_ng.c
# The _la_SOURCES variable lists the source files for the library.
chan_dongle_ng_la_SOURCES = $(SOURCES)

# Linker flags: -module tells the linker this is a loadable module.
chan_dongle_ng_la_LDFLAGS = -module -avoid-version -lpthread

# This tells automake where to install the final module file.
chan_dongle_ng_ladir = $(AST_MODULE_DIR)

# Add Asterisk include path to the C compiler flags.
AM_CFLAGS = $(ASTERISK_INCLUDE)
