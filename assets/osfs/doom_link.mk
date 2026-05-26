# doom_link.mk -- chapter 133e: in-guest link of doomgeneric.elf.
#
# After /bin/make -f /bin/doom_full.mk has produced 80 .o files
# under /data/src/, this fixture invokes GNU /bin/ld (chapter 131f)
# to link them with the runtime archive /bin/libdoomrt.a (crt0 +
# osdev shim + setjmp + cstring + wmclient) into the final
# /data/doomgeneric.elf.
#
# The whole link line would be ~1.8 KiB after expansion (80 paths
# x ~22 chars).  Our sys_spawn argv buffer is THREAD_ARGS_MAX=128
# bytes, so we cannot pass all 80 paths to ld directly.  Instead
# we ship the path list as a separate file (/bin/doom_link.args,
# one path per line) and pass it via GNU binutils' standard
# `@file` response-file syntax.  The recipe argv stays well under
# 128 bytes; ld expands the @file internally via libiberty's
# expandargv().
#
# Invocation:
#   /bin/make -f /bin/doom_link.mk
#
# Output:
#   /data/doomgeneric.elf  --  static AArch64 ET_EXEC at VA
#                              0x1000100000, ENTRY=_user_start.

LD      = /bin/ld
LDFLAGS = -T /bin/osdev.ld
OUTPUT  = /data/doomgeneric.elf
RUNTIME = /bin/libdoomrt.a
ARGS    = /bin/doom_link.args

all: $(OUTPUT)

$(OUTPUT):
	$(LD) $(LDFLAGS) -o $(OUTPUT) @$(ARGS) $(RUNTIME)

clean:
	rm -f $(OUTPUT)
