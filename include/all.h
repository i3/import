/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * This header file includes all relevant files of i3 and the most often used
 * system header files. This reduces boilerplate (the amount of code duplicated
 * at the beginning of each source file) and is not significantly slower at
 * compile-time.
 *
 */
#pragma once

#include <config.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#include "libi3.h"
#include "data.h"
#include "util.h"
#include "ipc.h"
#include "tree.h"
#include "log.h"
#include "xcb.h"
#include "manage.h"
#include "workspace.h"
#include "i3.h"
#include "x.h"
#include "click.h"
#include "key_press.h"
#include "floating.h"
#include "drag.h"
#include "configuration.h"
#include "handlers.h"
#include "randr.h"
#include "xinerama.h"
#include "con.h"
#include "load_layout.h"
#include "render.h"
#include "window.h"
#include "match.h"
#include "xcursor.h"
#include "resize.h"
#include "tiling_drag.h"
#include "sighandler.h"
#include "move.h"
#include "output.h"
#include "ewmh.h"
#include "assignments.h"
#include "regex.h"
#include "startup.h"
#include "scratchpad.h"
#include "commands.h"
#include "commands_parser.h"
#include "bindings.h"
#include "config_directives.h"
#include "config_parser.h"
#include "fake_outputs.h"
#include "display_version.h"
#include "restore_layout.h"
#include "sync.h"
#include "main.h"
