#
# JALv2 Makefile
# Copyright (c) 2008, Kyle A. York
# All rights reserved
#
# Original Author: Kyle A. York
# Author: Matthew Schinkel, 2026
# Changes: minor fork edits -- see fixes/ and git history
#
PARTS = libutils libcore libpic12 libpic32 libesp32 jal

include Makefile.inc

win32: libutils/libutils.a libcore/libcore.a libpic12/libpic12.a
	wine nmake /f "jal/jal.mak" "CFG=jal - Win32 Release"
	wine nmake /f "jal/jal.mak" "CFG=jal - Win32 Debug"

ctags_list_file:
	find . -name *.[ch] > ctags_list_file

ctags: ctags_list_file
	rm -f tags
	ctags -L ctags_list_file --extra=+f -f tags  
