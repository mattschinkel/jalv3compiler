#
# JALv2 Makefile
# Copyright (c) 2008, Kyle A. York
# All rights reserved
#
# Original Author: Kyle A. York
# Author: Matthew Schinkel, 2026
# Changes: fork extensions (32-bit back-end libs) -- see docs/fixes/ and git history
#
LIBS=../libpic12/libpic12-$(LIBTAG).a ../libpic32/libpic32-$(LIBTAG).a ../libesp32/libesp32-$(LIBTAG).a ../libcore/libcore-$(LIBTAG).a ../libutils/libutils-$(LIBTAG).a
# Must follow Makefile.inc: JAL2_BIN_STEM is `original` for ARCH=native, else same as ARCH.
PROGRAM=../../bin/beta/jalv3-$(JAL2_BIN_STEM)

.PHONY: dummy

jaldefault: dummy project

dummy:
	./build.sh
	-rm -f $(OBJ_DIR)/jal_main.o

include ../Makefile.inc

