# doom_pilot.mk -- chapter 133c in-guest doom rebuild pilot.
#
# Compiles three small DoomGeneric vendor sources to .o files
# using /bin/gcc + /bin/make.  Runs after /bin/tar has
# extracted /bin/doomgeneric.tar onto /data/.
#
# Selected files (all small, low cross-file coupling):
#
#   m_random.c   — 65 LoC, zero #includes (just a 256-byte
#                  rnd table and 3 functions).  Zero
#                  external-symbol requirements -- the
#                  cleanest possible smoke test.
#   m_bbox.c     — 54 LoC, one local include (m_bbox.h ->
#                  <limits.h> + m_fixed.h).  Exercises
#                  cpp's same-dir include search.
#   m_fixed.c    — 62 LoC, includes "stdlib.h" (libc),
#                  "doomtype.h" (local) -> "strings.h",
#                  "inttypes.h", "limits.h" -- exercises
#                  the full cpp search path including
#                  <strings.h> from libc.
#
# Invocation:
#   /bin/tar xf /bin/doomgeneric.tar -C /data
#   /bin/make -f /bin/doom_pilot.mk
#
# Note: we use absolute paths everywhere because the
# in-guest sys_spawn does NOT propagate cwd to children
# (chapter-133c finding).  Once a future chapter adds
# cwd inheritance to spawn we can drop the /data/src
# prefix and just `cd /data/src && /bin/make`.
#
# Expected outcome: three .o files in /data/src/, each one
# an ELF AArch64 object.

CC = /bin/gcc
CFLAGS = -O0

DIR = /data/src

OBJS = $(DIR)/m_random.o \
       $(DIR)/m_bbox.o \
       $(DIR)/m_fixed.o

all: $(OBJS)

$(DIR)/%.o: $(DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
