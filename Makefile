# sloperinaGZ (source: src/ootgz): the native (in-game) build. See docs/BUILDING.md.
#
#   make            build bin/subsdk9 and one bin/<build id>.ips per supported game
#   make dist       stage install trees in dist/ (atmosphere/, lichii/, exefs/)
#   make clean
#
# Needs devkitA64 (DEVKITPRO set) and a host C compiler; ./docker-make runs this Makefile in
# the devkitpro/devkita64 image instead.

# the version: the VERSION file (the release) and git's commit, e.g. "0.1.0 (6d8cdba)"
PACKAGE_VERSION      ?= $(shell cat VERSION 2>/dev/null || echo 0) ($(shell git describe --tags --always --dirty 2>/dev/null || echo unknown))
DEVKITPRO            ?= /opt/devkitpro
TOOLS                 = $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CC                    = $(TOOLS)gcc
ELF2NSO               = $(DEVKITPRO)/tools/bin/elf2nso
HOSTCC               ?= gcc

PROGRAM_ID            = 0400486024DEC000
BUILD_IDS             = 33FA6C2C23848282A77E8B88A0A93F4887C55920 E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348
SUBSDK                = subsdk9
NAME                  = sloperinaGZ

SRCDIR                = src/ootgz
HOSTDIR               = src/host
LDSCRIPT              = lib/ootgz.ld
OBJDIR                = obj
BINDIR                = bin
DISTDIR               = dist

# Extra -D defines: make OPTIONS=...
OPTIONS              ?=

ARCHFLAGS             = -march=armv8-a+crc+crypto -mtune=cortex-a78 -mtp=soft -fPIC
CFLAGS                = -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter $(ARCHFLAGS) \
                        -fvisibility=hidden -ffunction-sections -fdata-sections \
                        -fno-asynchronous-unwind-tables -fno-unwind-tables \
                        -fno-stack-protector -fno-plt -fno-common \
                        -DPACKAGE_VERSION='"$(PACKAGE_VERSION)"' $(addprefix -D,$(OPTIONS))
ASFLAGS               = $(ARCHFLAGS)
LDFLAGS               = -shared -nostdlib -nostartfiles -T $(LDSCRIPT) \
                        -Wl,--gc-sections -Wl,-z,text -Wl,-z,max-page-size=0x1000 \
                        -Wl,--hash-style=sysv -Wl,-Bsymbolic -Wl,--build-id=sha1 \
                        -Wl,-init=ootgz_module_init -Wl,--exclude-libs=ALL
LDLIBS                = -lgcc

CSRC                  = $(wildcard $(SRCDIR)/*.c)
SSRC                  = $(wildcard $(SRCDIR)/*.S)
HDRS                  = $(wildcard $(SRCDIR)/*.h) $(wildcard $(SRCDIR)/third_party/*) # third_party/*.c are #included

PATCHES               = $(addprefix $(BINDIR)/,$(addsuffix .ips,$(BUILD_IDS)))

all: $(BINDIR)/$(SUBSDK) $(PATCHES)

# The about menu's build info: the version and the day it was built ("9th November, 2026"). Made every
# time; menus.c is rebuilt when it changes.
$(OBJDIR)/build_info.h: VERSION FORCE
	mkdir -p $(@D)
	d=$$(LC_ALL=C date +%-d); \
	case $$d in 11|12|13) o=th;; *1) o=st;; *2) o=nd;; *3) o=rd;; *) o=th;; esac; \
	{ printf '#define BUILD_WHEN "%s%s %s"\n' "$$d" "$$o" "$$(LC_ALL=C date +'%B, %Y')"; \
	  printf '#define BUILD_VERSION "%s"\n' "$(PACKAGE_VERSION)"; } > $@.tmp
	cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
FORCE:

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(OBJDIR) -c $< -o $@
$(OBJDIR)/menus.o: $(OBJDIR)/build_info.h
$(OBJDIR)/%.o: $(SRCDIR)/%.S | $(OBJDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(BINDIR)/ootgz.elf: $(patsubst $(SRCDIR)/%,$(OBJDIR)/%.o,$(basename $(SSRC) $(CSRC))) $(LDSCRIPT) | $(BINDIR)
	$(CC) $(ARCHFLAGS) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)
	@# Reject undefined symbols the game's loader cannot resolve.
	@missing=$$($(TOOLS)readelf --dyn-syms -W $@ | awk '$$7=="UND" && $$8!="" {print $$8}' | LC_ALL=C sort -u | LC_ALL=C comm -23 - lib/imports.txt); \
	if [ -n "$$missing" ]; then echo "error: not defined, and not a known import (lib/imports.txt):"; echo "$$missing"; rm -f $@; exit 1; fi
$(BINDIR)/$(SUBSDK): $(BINDIR)/ootgz.elf
	$(ELF2NSO) $< $@
$(PATCHES): $(OBJDIR)/host/mkips | $(BINDIR)
	$(OBJDIR)/host/mkips $@
$(OBJDIR) $(BINDIR):
	mkdir -p $@

$(OBJDIR)/host/mkips: $(HOSTDIR)/mkips.c $(SRCDIR)/z64hd.h $(SRCDIR)/z64hd_builds.h $(SRCDIR)/ootgz_version.h
	mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -o $@ $<

# Install trees. Atmosphère convention (also what a Switch 2 CFW with LayeredFS would need):
#   atmosphere/contents/<program id>/exefs/subsdk9
#   atmosphere/exefs_patches/$(NAME)/<build id>.ips
# Lichii / Eden: load/<program id>/sloperinaGZ/exefs/{subsdk9,<build id>.ips}
# exefs/: add subsdk9 to exefs and apply the matching .ips to main.
dist: all
	rm -rf $(DISTDIR)
	mkdir -p $(DISTDIR)/atmosphere/contents/$(PROGRAM_ID)/exefs \
	         $(DISTDIR)/atmosphere/exefs_patches/$(NAME) \
	         $(DISTDIR)/lichii/load/$(PROGRAM_ID)/$(NAME)/exefs $(DISTDIR)/exefs
	cp $(BINDIR)/$(SUBSDK) $(DISTDIR)/atmosphere/contents/$(PROGRAM_ID)/exefs/
	cp $(PATCHES) $(DISTDIR)/atmosphere/exefs_patches/$(NAME)/
	cp $(BINDIR)/$(SUBSDK) $(PATCHES) $(DISTDIR)/lichii/load/$(PROGRAM_ID)/$(NAME)/exefs/
	cp $(BINDIR)/$(SUBSDK) $(PATCHES) $(DISTDIR)/exefs/
	# the SD card: the translation template (tools/lang_template.py), sd:/sloperinaGZ/lang/
	mkdir -p $(DISTDIR)/sd/$(NAME)/lang && cp lang/template.json $(DISTDIR)/sd/$(NAME)/lang/

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(DISTDIR)

.PHONY: all dist clean FORCE
