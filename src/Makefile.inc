#
# JALv2 Makefile
# Copyright (c) 2008, Kyle A. York
#               2018..2024, Rob Jansen
#
# All rights reserved
#
# Co-author: Matthew Schinkel, 2026 (jalv3compiler fork)
# Changes: fork extensions (build system) -- see fixes/ and git history
#
# Usage:
#   higher level makefiles define PROGRAM (program name) or LIBRARY
#   (jal emits bin/beta/jalv3-<stem>)
#   (library name) but not both. All sources in the directory are
#   assumed to belong. The result is left in the same directory
#   as the sources.
#   Object files go into obj
#   Dependency files are automatically generated and go into .dep
#
default: project

#COVERAGE=-fprofile-arcs -ftest-coverage
#PROFILE=-pg
DEBUG=-g

# Default ARCH on Linux/x86_64 so `make -C src` (no args) produces **`bin/beta/jalv3-x86-64`**
# from this tree — primary regression compiler (see **`instruction_compile.txt`**).
# Use **`make ARCH=native`** for host **`jalv3-original`** (`-march=native`).
ifeq ($(origin ARCH),undefined)
  ifeq ($(shell uname -s 2>/dev/null),Linux)
    ifeq ($(shell uname -m 2>/dev/null),x86_64)
      ARCH := x86-64
    endif
  endif
endif

# Compiler flags different for Window and macOS. Kept makefile as much in line as the original and added MacOS.
# Since MacOS also has a x86-64 architecture version we use osx.
ifeq ($(ARCH),x86-64)
   # 64-bit Windows and Linux
   MARCH = -m64
   # AddressSanitizer support for Linux debugging
   ifeq ($(ASAN),1)
     CFLAGS=-O1 -Werror -Wstringop-overflow=0 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -march=$(ARCH) $(MARCH) $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC -fsanitize=address -fno-omit-frame-pointer
     LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) $(MARCH) -march=$(ARCH) -lm -fsanitize=address -fno-omit-frame-pointer
   else
     CFLAGS=-O2 -Werror -Wstringop-overflow=0 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -march=$(ARCH) $(MARCH) $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC
     LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) $(MARCH) -march=$(ARCH) -lm -fPIC
   endif
else ifeq ($(ARCH),i686)
   MARCH = -m32
   # 32-bit Windows and Linux
   CFLAGS=-O2 -Werror -Wstringop-overflow=0 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -march=$(ARCH) $(MARCH) $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC
   LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) $(MARCH) -march=$(ARCH) -lm -static -fPIC
else ifeq ($(ARCH),win64)
   # 64-bit Windows, cross-compiled from Linux (packages:
   # gcc-mingw-w64-x86-64).  Own obj-dir/lib tags via ARCH=win64 so the
   # mingw objects never mix with the Linux build (mixed-object lesson).
   # Output stem `win64` -> bin/beta/jalv3-win64.exe is the ONLY Windows
   # binary (no duplicate: the old cp-to-x86-64.exe twin was removed
   # 2026-10-30 -- it was a manual copy under the jalv25r9 share name).
   MARCH = -m64
   CC = x86_64-w64-mingw32-gcc   # plain assignment: CC is a make builtin default, `?=` never fires
   CFLAGS=-O2 -Werror -Wstringop-overflow=0 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual $(MARCH) $(DEBUG) $(PROFILE) $(COVERAGE)
   LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) $(MARCH) -lm -static
else ifeq ($(ARCH),osx)
   #Darwin (MacOS), 64-bit
   # -Werror and  -static removed since MAC compiler on GitHub states that a variable is defined but not used but that is incorrect.
   # CFLAGS=-O2 -Werror -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -arch x86_64 $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC
   CFLAGS=-O2 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual -arch x86_64 $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC
   LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) -arch x86_64 -lm -fPIC
else
   # Native
   ARCH = native
   CFLAGS=-O2 -Werror -Wstringop-overflow=0 -ansi -pedantic -W -Wall -Wshadow -Wcast-qual  -march=$(ARCH) $(MARCH) $(DEBUG) $(PROFILE) $(COVERAGE) -fPIC
   LFLAGS=$(DEBUG) $(PROFILE) $(COVERAGE) $(MARCH) -march=$(ARCH) -lm -static -fPIC
endif

# Append experiment flags (e.g. ROM (A) `-DJALV2_ROM_A_STRING_LITERAL_CALL=1`) without
# replacing the platform `CFLAGS` line — see `fixes/fix_rom_a_jalv2_rom_a_string_literal_call_toggle.md`.
CFLAGS += $(EXTRA_CFLAGS)

# Build flavors (dev tooling, 2026-10-05 — fixes/fix_dev_tooling.md):
#   SAN=1 (or the historical ASAN=1) -> ASan+UBSan debug build,
#     bin/beta/jalv3-x86-64-san, objects in .obj-x86-64-san/
#   COV=1 -> gcov instrumentation, bin/beta/jalv3-x86-64-cov,
#     objects in .obj-x86-64-cov/ (.gcda written beside them)
# Flavors get their OWN object dirs and output names so instrumented and
# primary artifacts never mix (mixed-object lesson,
# fixes/fix_pic32_startup_silicon_cache.md). Library names carry the same
# tag via lazy $(LIBTAG) expansion in the per-part Makefiles.
FLAVOR :=
FLAVORFLAGS :=
ifeq ($(SAN),1)
  FLAVOR := -san
endif
ifeq ($(ASAN),1)
  FLAVOR := -san
endif
ifeq ($(FLAVOR),-san)
  # The sanitizer flavor also carries the value lifetime detector: a value
  # used after its release is reported (with the release stack) instead of
  # silently reading stale data. Values are region-scoped, so this is the
  # deliberate replacement for the poisoning refcounting used to do.
  FLAVORFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
      -DJALV2_VALUE_DEBUG
  # -O1 for readable traces; no -Werror (lower opt levels surface format
  # warnings on intentional null-format pfile_log calls that -O2 hides)
  CFLAGS := $(filter-out -Werror,$(subst -O2,-O1,$(CFLAGS)))
endif
ifeq ($(COV),1)
  FLAVOR := -cov
  FLAVORFLAGS := --coverage
endif
CFLAGS += $(FLAVORFLAGS)
LFLAGS += $(FLAVORFLAGS)

export ARCH

# Stems `bin/beta/jalv3-*` (jal only): `native` -> `original` to avoid clashing with gcc `-march=native` and with JAL "native" language features.
JAL2_BIN_STEM := $(if $(filter native,$(ARCH)),original,$(ARCH))$(FLAVOR)

LIBTAG := $(ARCH)$(FLAVOR)
OBJ_DIR=.obj-$(LIBTAG)

DEP_DIR=.dep
SRCS=$(wildcard *.c)
OBJS=$(addprefix $(OBJ_DIR)/, $(SRCS:.c=.o))

%.asm : %.jal
	../bin/beta/jalv3-x86-64 -pcode -no-fuse -s lib $<

%.a : %.jal
	../bin/beta/jalv3-x86-64 -pcode -no-fuse -s lib $<

%.d : %.jal
	../bin/beta/jalv3-x86-64 -debug -pcode -no-fuse -s lib $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)
	mkdir -p $(DEP_DIR)

# `make clean` removes .dep; the -MMD write needs it back before the first
# compile (rebuild-after-clean footgun, hit 2026-10-10)
$(DEP_DIR):
	mkdir -p $(DEP_DIR)

#
# splint note -- unless I plan to annotate the source
# (I don't) I need to use '-weak'
#
DEP=$(DEP_DIR)/$(*F)
# `$(DEP_DIR)` is an ORDER-ONLY prerequisite, not just a rule: the recipe
# below writes $(DEP)/.d with -MMD, so a build where the object dir
# exists but .dep was removed (any per-flavor `make clean`, which drops
# the SHARED .dep) would fail with "opening dependency file .dep/x.d".
# The $(DEP_DIR) rule alone never fired; hit again 2026-11-01 on a
# win64 build over a stale .obj-win64.
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR) $(DEP_DIR)
	#splint -weak -bufferoverflowhigh -pred-bool -cast-fcn-ptr -unrecog -skipisoheaders $<
	$(CC) -MMD -MF $(DEP).d -c -o $@ $(CFLAGS) $<

%.o : $(OBJ_DIR)/%.o

#
# Program rules
#
ifneq ($(PROGRAM),)

project: $(PROGRAM)

# Order-only bin/ prerequisite: a fresh clone has neither bin/ nor bin/beta/
# and the link would abort ('cannot open output file .../bin/beta/...').
#
# LAYOUT: every build lands in bin/beta/ (this rule creates it).  bin/release/
# holds CUT releases only -- nothing writes there, a human copies an artifact
# in when a release is made.  bin/old_versions/ holds the RELEASED jalv2 line
# and never receives a jalv3 build (jALv3 is beta; see .gitignore).
BIN_DIR = $(dir $(PROGRAM))

$(PROGRAM): $(OBJS) $(LIBS) | $(BIN_DIR)
	$(CC) -o $@ $(OBJS) $(LIBS) $(LFLAGS)
#	strip $(PROGRAM)  # Commented out to preserve debug symbols for AddressSanitizer debugging

$(BIN_DIR):
	mkdir -p $@
	
# The toolchain appends .exe for the win64 cross and Windows-native builds,
# so clean must remove BOTH spellings.  The old `x86-64Linux -> EXE_RM empty`
# special case existed because a Linux clean sat beside the published Windows
# artifact under the SAME name; with builds confined to bin/beta that artifact
# is bin/beta/jalv3-win64.exe, so the clash (and the carve-out) is gone.
EXE_RM := $(PROGRAM).exe

clean::
	-rm -f $(PROGRAM) $(EXE_RM)
	-rm -rf $(OBJ_DIR)
	-rm -rf $(DEP_DIR)

endif

#
# Library rules
#
ifneq ($(LIBRARY),)

project: $(LIBRARY)

$(LIBRARY): $(OBJS)
	$(AR) -r $@ $(OBJS)

clean::
	-rm -f $(LIBRARY)
	-rm -rf $(OBJ_DIR)
	-rm -rf $(DEP_DIR)

endif

#
# Parts rules (subdirectories)
#
ifneq ($(PARTS),)

project:
	@for dir in $(PARTS) ; do \
		( make -C $$dir )       \
	done

clean::
	@for dir in $(PARTS) ; do \
		( make -C $$dir clean ) \
	done

endif

ifeq ($(strip $(filter clean%, $(MAKECMDGOALS))),)
# column 0 is load-bearing: a TAB-initial -include here is silently eaten
# as recipe text, the .d rules never load, and header edits go unnoticed
# until stale objects segfault the binary (mixed-object lesson, 2026-10-06)
-include $(addprefix $(DEP_DIR)/,$(SRCS:.c=.d))
endif


clean::
	-rm -rf $(OBJ_DIR) $(DEP_DIR) 

