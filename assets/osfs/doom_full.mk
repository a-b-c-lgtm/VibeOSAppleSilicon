# doom_full.mk -- chapter 194 in-guest doom full vendor compile.
#
# Compiles the 81 canonical DoomGeneric vendor sources (plus
# dummy.o) to .o files using /bin/gcc + /bin/make, producing
# the same 82-object set the host cross-build produces and
# /bin/doom_link.args expects.  The set is the host Makefile's
# DOOM_VENDOR_SRCS minus the SDL/X11/Allegro/etc. platform
# backends (we replace them with our own osdev shim, shipped
# separately as libdoomrt.a).
#
# Like chapter 193's doom_pilot.mk, this fixture uses
# ABSOLUTE paths everywhere because /bin/sys_spawn does not
# propagate cwd to children.
#
# Prereq:
#   /bin/tar xf /bin/doomgeneric.tar -C /data
#
# Invocation:
#   /bin/make -f /bin/doom_full.mk

CC = /bin/gcc
# -DOSDEV_LIBC_NO_GLOBAL_DEFS mirrors the host
# DOOM_VENDOR_CFLAGS: without it, every vendor TU that
# transitively includes userspace/libc/atexit.h or env.h emits
# its own copy of __cxa_finalize / environ, and the subsequent
# /bin/make -f /bin/doom_link.mk drowns in "multiple definition
# of `__cxa_finalize'" / "multiple definition of `environ'"
# errors.  cstring.o inside /bin/libdoomrt.a provides the
# canonical single defs the suppressed TUs link against.
CFLAGS = -O0 -DNORMALUNIX -DOSDEV_LIBC_NO_GLOBAL_DEFS -I /data/src

DIR = /data/src

OBJS = $(DIR)/dummy.o \
       $(DIR)/am_map.o \
       $(DIR)/doomdef.o \
       $(DIR)/doomstat.o \
       $(DIR)/dstrings.o \
       $(DIR)/d_event.o \
       $(DIR)/d_items.o \
       $(DIR)/d_iwad.o \
       $(DIR)/d_loop.o \
       $(DIR)/d_main.o \
       $(DIR)/d_mode.o \
       $(DIR)/d_net.o \
       $(DIR)/f_finale.o \
       $(DIR)/f_wipe.o \
       $(DIR)/g_game.o \
       $(DIR)/gusconf.o \
       $(DIR)/hu_lib.o \
       $(DIR)/hu_stuff.o \
       $(DIR)/icon.o \
       $(DIR)/info.o \
       $(DIR)/i_cdmus.o \
       $(DIR)/i_endoom.o \
       $(DIR)/i_joystick.o \
       $(DIR)/i_scale.o \
       $(DIR)/i_sound.o \
       $(DIR)/i_system.o \
       $(DIR)/i_timer.o \
       $(DIR)/memio.o \
       $(DIR)/m_argv.o \
       $(DIR)/m_bbox.o \
       $(DIR)/m_cheat.o \
       $(DIR)/m_config.o \
       $(DIR)/m_controls.o \
       $(DIR)/m_fixed.o \
       $(DIR)/m_menu.o \
       $(DIR)/m_misc.o \
       $(DIR)/m_random.o \
       $(DIR)/p_ceilng.o \
       $(DIR)/p_doors.o \
       $(DIR)/p_enemy.o \
       $(DIR)/p_floor.o \
       $(DIR)/p_inter.o \
       $(DIR)/p_lights.o \
       $(DIR)/p_map.o \
       $(DIR)/p_maputl.o \
       $(DIR)/p_mobj.o \
       $(DIR)/p_plats.o \
       $(DIR)/p_pspr.o \
       $(DIR)/p_saveg.o \
       $(DIR)/p_setup.o \
       $(DIR)/p_sight.o \
       $(DIR)/p_spec.o \
       $(DIR)/p_switch.o \
       $(DIR)/p_telept.o \
       $(DIR)/p_tick.o \
       $(DIR)/p_user.o \
       $(DIR)/r_bsp.o \
       $(DIR)/r_data.o \
       $(DIR)/r_draw.o \
       $(DIR)/r_main.o \
       $(DIR)/r_plane.o \
       $(DIR)/r_segs.o \
       $(DIR)/r_sky.o \
       $(DIR)/r_things.o \
       $(DIR)/sha1.o \
       $(DIR)/sounds.o \
       $(DIR)/statdump.o \
       $(DIR)/st_lib.o \
       $(DIR)/st_stuff.o \
       $(DIR)/s_sound.o \
       $(DIR)/tables.o \
       $(DIR)/v_video.o \
       $(DIR)/wi_stuff.o \
       $(DIR)/w_checksum.o \
       $(DIR)/w_file.o \
       $(DIR)/w_main.o \
       $(DIR)/w_wad.o \
       $(DIR)/w_file_stdc.o \
       $(DIR)/z_zone.o \
       $(DIR)/i_input.o \
       $(DIR)/i_video.o \
       $(DIR)/doomgeneric.o

all: $(OBJS)

$(DIR)/%.o: $(DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
