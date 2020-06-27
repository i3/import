/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * xcursor.c: libXcursor support for themed cursors.
 *
 */
#pragma once

#include <config.h>

#include <xcb/xcb_cursor.h>

enum xcursor_cursor_t {
    XCURSOR_CURSOR_POINTER = 0,
    XCURSOR_CURSOR_RESIZE_HORIZONTAL,
    XCURSOR_CURSOR_RESIZE_VERTICAL,
    XCURSOR_CURSOR_TOP_LEFT_CORNER,
    XCURSOR_CURSOR_TOP_RIGHT_CORNER,
    XCURSOR_CURSOR_BOTTOM_LEFT_CORNER,
    XCURSOR_CURSOR_BOTTOM_RIGHT_CORNER,
    XCURSOR_CURSOR_TOP_SIDE,
    XCURSOR_CURSOR_BOTTOM_SIDE,
    XCURSOR_CURSOR_LEFT_SIDE,
    XCURSOR_CURSOR_RIGHT_SIDE,
    XCURSOR_CURSOR_WATCH,
    XCURSOR_CURSOR_MOVE,
    XCURSOR_CURSOR_NOT_ALLOWED,
    XCURSOR_CURSOR_MAX
};

void xcursor_load_cursors(void);
xcb_cursor_t xcursor_get_cursor(enum xcursor_cursor_t c);

/**
 * Sets the cursor of the root window to the 'pointer' cursor.
 *
 * This function is called when i3 is initialized, because with some login
 * managers, the root window will not have a cursor otherwise.
 *
 * We have a separate xcursor function to use the same X11 connection as the
 * xcursor_load_cursors() function. If we mix the Xlib and the XCB connection,
 * races might occur (even though we flush the Xlib connection).
 *
 */
void xcursor_set_root_cursor(int cursor_id);
