/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3.h: global variables that are used all over i3.
 *
 */
#pragma once

#include <config.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <xcb/shape.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xkb.h>

#include <X11/XKBlib.h>

#define SN_API_NOT_YET_FROZEN 1
#include <libsn/sn-launcher.h>

#include "queue.h"
#include "data.h"
#include "xcb.h"

/** Git commit identifier, from version.c */
extern const char *i3_version;

/** The original value of RLIMIT_CORE when i3 was started. We need to restore
 * this before starting any other process, since we set RLIMIT_CORE to
 * RLIM_INFINITY for i3 debugging versions. */
extern struct rlimit original_rlimit_core;
/** Whether this version of i3 is a debug build or a release build. */
extern bool debug_build;
/** The number of file descriptors passed via socket activation. */
extern int listen_fds;
extern int conn_screen;
extern xcb_atom_t wm_sn;
/**
 * The EWMH support window that is used to indicate that an EWMH-compliant
 * window manager is present. This window is created when i3 starts and
 * kept alive until i3 exits.
 * We also use this window as the focused window if no other window is
 * available to be focused on the active workspace in order to prevent
 * keyboard focus issues (see #1378).
 */
extern xcb_window_t ewmh_window;
/** The last timestamp we got from X11 (timestamps are included in some events
 * and are used for some things, like determining a unique ID in startup
 * notification). */
extern xcb_timestamp_t last_timestamp;
extern SnDisplay *sndisplay;
extern xcb_key_symbols_t *keysyms;
extern char **start_argv;
extern Display *xlibdpy, *xkbdpy;
extern int xkb_current_group;
extern TAILQ_HEAD(bindings_head, Binding) *bindings;
extern const char *current_binding_mode;
extern TAILQ_HEAD(autostarts_head, Autostart) autostarts;
extern TAILQ_HEAD(autostarts_always_head, Autostart) autostarts_always;
extern TAILQ_HEAD(ws_assignments_head, Workspace_Assignment) ws_assignments;
extern TAILQ_HEAD(assignments_head, Assignment) assignments;
extern SLIST_HEAD(stack_wins_head, Stack_Window) stack_wins;

/* Color depth, visual id and colormap to use when creating windows and
 * pixmaps. Will use 32 bit depth and an appropriate visual, if available,
 * otherwise the root window’s default (usually 24 bit TrueColor). */
extern uint8_t root_depth;
extern xcb_visualid_t visual_id;
extern xcb_colormap_t colormap;

extern bool xkb_supported, shape_supported;
extern xcb_window_t root;
extern struct ev_loop *main_loop;
extern bool only_check_config;
extern bool force_xinerama;
