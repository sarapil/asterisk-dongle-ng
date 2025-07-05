AC_PREREQ([2.69])
AC_INIT([asterisk-dongle-ng], [0.1], [moataz.sarapil@gmail.com])
AC_CONFIG_SRCDIR([chan_dongle_ng.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
LT_INIT

# Find Asterisk headers and libraries
# This macro is provided by the asterisk.m4 file we copied
AST_EXT_MODULE_CHECK_ASTERISK_DEPS

# Set the destination for the module installation
AC_SUBST(AST_MODULE_DIR)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
