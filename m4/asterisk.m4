dnl
dnl $Id$
dnl
dnl Copyright (C) 2011-2013, Digium, Inc.
dnl
dnl See http://www.asterisk.org for more information about
dnl the Asterisk project. Please do not directly contact
dnl any of the maintainers of this project for assistance;
dnl the asterisk-dev mailing list is a better place for questions.
dnl
dnl This program is free software, distributed under the terms of
dnl the GNU General Public License Version 2. See the LICENSE file
dnl at the top of the source tree.
dnl
dnl A set of autoconf macros for building external Asterisk modules.
dnl
dnl To use this in your project, you must have a copy of this file
dnl in your m4/ directory.
dnl
dnl In your configure.ac, you must have the following line:
dnl
dnl   AST_EXT_MODULE_CHECK_ASTERISK_DEPS
dnl
dnl This will check for the presence of the Asterisk development
dnl headers and libraries. It will also set the following output
dnl variables:
dnl
dnl   ASTERISK_INCLUDE --- C compiler flags to find the Asterisk headers
dnl   ASTERISK_LIBS    --- Linker flags to link against Asterisk
dnl   AST_MODULE_DIR   --- The directory where Asterisk modules are installed
dnl

AC_DEFUN([AST_EXT_MODULE_CHECK_ASTERISK_DEPS], [
	AC_MSG_CHECKING([for Asterisk development headers])

	AST_SAVED_CPPFLAGS=$CPPFLAGS
	AST_SAVED_LDFLAGS=$LDFLAGS
	AST_SAVED_LIBS=$LIBS

	AC_ARG_WITH(asterisk,
		AS_HELP_STRING([--with-asterisk=DIR], [Look for Asterisk headers in DIR/include]),
		[
			if test -d "$withval/include/asterisk"; then
				CPPFLAGS="$CPPFLAGS -I$withval/include"
			elif test -d "$withval/include"; then
				CPPFLAGS="$CPPFLAGS -I$withval/include"
			else
				CPPFLAGS="$CPPFLAGS -I$withval"
			fi
		],
		[
			#
			# If the user did not specify a location for Asterisk,
			# let's try to find it with pkg-config.
			#
			AC_PATH_PROG(PKG_CONFIG, pkg-config)
			if test -n "$PKG_CONFIG"; then
				AST_PKG_CONFIG_DEPS="asterisk"
				$PKG_CONFIG --exists "$AST_PKG_CONFIG_DEPS"
				if test $? -eq 0; then
					AC_MSG_RESULT([found via pkg-config])
					CPPFLAGS="$CPPFLAGS `eval $PKG_CONFIG --cflags-only-I $AST_PKG_CONFIG_DEPS`"
					AST_MODULE_DIR=`eval $PKG_CONFIG --variable=astmoddir asterisk`
				else
					AC_MSG_RESULT([not found])
					AC_MSG_ERROR([
***
*** Asterisk development headers not found.
*** Please install the Asterisk development package.
***])
				fi
			else
				AC_MSG_RESULT([pkg-config not found])
				AC_MSG_ERROR([
***
*** Pkg-config not found.
*** Please install the pkg-config package.
***])
			fi
		])

	#
	# Now that we have the include path, let's verify that we can
	# actually find the headers.
	#
	AC_MSG_CHECKING([for asterisk.h])
	AC_PREPROC_IFELSE(
		[AC_LANG_PROGRAM([[
/* Defeat the name collision between our package and Asterisk's */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_URL
#undef PACKAGE_VERSION
#include "asterisk.h"
		]])],
		[AC_MSG_RESULT([yes])],
		[
			AC_MSG_RESULT([no])
			AC_MSG_ERROR([
***
*** Asterisk development headers not found.
***
*** Please install the Asterisk development package.
*** You may also need to use --with-asterisk=DIR to specify
*** the location of your Asterisk source.
***])
		])

	AC_MSG_CHECKING([for ast_version.h])
	AC_PREPROC_IFELSE(
		[AC_LANG_PROGRAM([[#include "asterisk/version.h"]])],
		[AC_MSG_RESULT([yes])],
		[
			AC_MSG_RESULT([no])
			AC_MSG_ERROR([
***
*** Asterisk development headers not found.
***
*** Please install the Asterisk development package.
*** You may also need to use --with-asterisk=DIR to specify
*** the location of your Asterisk source.
***])
		])

	AC_MSG_CHECKING([for AST_MODULE_DIR])
	if test -z "$AST_MODULE_DIR"; then
		AST_MODULE_DIR_SAVED_CPPFLAGS=$CPPFLAGS
		CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
		AC_RUN_IFELSE(
			[AC_LANG_PROGRAM(
				[[
#include <stdio.h>
#include "asterisk/paths.h"
				]],
				[[
					FILE *f = fopen("conftest.dir", "w");
					if (f) {
						fprintf(f, "%s", AST_MODULE_DIR);
						fclose(f);
					}
				]])],
			[
				AST_MODULE_DIR=`cat conftest.dir`
				AC_MSG_RESULT([$AST_MODULE_DIR])
			],
			[AC_MSG_RESULT([not found])],
			[AC_MSG_RESULT([cross-compiling])])
		CPPFLAGS=$AST_MODULE_DIR_SAVED_CPPFLAGS
	else
		AC_MSG_RESULT([$AST_MODULE_DIR])
	fi

	if test -z "$AST_MODULE_DIR"; then
		AC_MSG_WARN([Could not determine AST_MODULE_DIR])
	fi

	ASTERISK_INCLUDE=$CPPFLAGS

	CPPFLAGS=$AST_SAVED_CPPFLAGS
	LDFLAGS=$AST_SAVED_LDFLAGS
	LIBS=$AST_SAVED_LIBS

	AC_SUBST(ASTERISK_INCLUDE)
	AC_SUBST(ASTERISK_LIBS)
])
