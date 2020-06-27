/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * resize.c: Interactive resizing.
 *
 */
#pragma once

#include <config.h>

typedef enum { CLICK_BORDER = 0,
               CLICK_DECORATION = 1,
               CLICK_INSIDE = 2 } click_destination_t;

typedef enum {
    RD_NONE = 0,

    RD_UP = (1 << 0),
    RD_DOWN = (1 << 1),
    RD_LEFT = (1 << 2),
    RD_RIGHT = (1 << 3),

    RD_UP_LEFT = RD_UP | RD_LEFT,
    RD_UP_RIGHT = RD_UP | RD_RIGHT,
    RD_DOWN_LEFT = RD_DOWN | RD_LEFT,
    RD_DOWN_RIGHT = RD_DOWN | RD_RIGHT,
} resize_direction_t;

typedef struct {
    Con *output;
    Con *first_h;
    Con *first_v;
    Con *second_h;
    Con *second_v;
} resize_params_t;

bool resize_find_tiling_participants(Con **current, Con **other, direction_t direction, bool both_sides);

resize_direction_t resize_find_tiling_participants_two_axes(
    Con *con, resize_direction_t dir, resize_params_t *p);

void resize_graphical_handler(const xcb_button_press_event_t *event,
                              enum xcursor_cursor_t cursor,
                              bool use_threshold,
                              resize_params_t *p);

/**
 * Resize the two given containers using the given amount of pixels or
 * percentage points. One of the two needs to be 0. A positive amount means
 * growing the first container while a negative means shrinking it.
 * Return false when resize is not performed due to container minimum size
 * constraints.
 *
 */
bool resize_neighboring_cons(Con *first, Con *second, int px, int ppt);

/**
 * Calculate the minimum percent needed for the given container to be at least 1
 * pixel.
 *
 */
double percent_for_1px(Con *con);

/**
 * Return cursor which should be used during the resize process.
 *
 * unidirectional_arrow should be true when it is clear from the user
 * perspective which container is the main in the resizing process. In this
 * case, a single-headed arrow cursor is returned. Сontrariwise, e.g. in a case
 * of drag by the border between two windows, a two-headed arrow cursor is
 * returned.
 *
 */
enum xcursor_cursor_t xcursor_type_for_resize_direction(
    resize_direction_t dir, bool unidirectional_arrow);

/**
 * Return resize direction for a container based on the click coordinates and
 * destination.
 *
 */
resize_direction_t get_resize_direction(Con *con, int x, int y, click_destination_t dest);
