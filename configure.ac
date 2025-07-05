AC_PREREQ([2.69])
AC_INIT([asterisk-dongle-ng], [0.1], [""])
AC_CONFIG_SRCDIR([chan_dongle_ng.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

# Find Asterisk headers and libraries
# This macro is provided by Asterisk development packages
AST_EXT_MODULE_CHECK_ASTERISK_DEPS

# Set the destination for the module installation
AC_SUBST(AST_MODULE_DIR)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
