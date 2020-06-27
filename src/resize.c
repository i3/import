/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * resize.c: Interactive resizing.
 *
 */
#include "all.h"

struct callback_params_dim {
    /* Pair of containers to be resized by this axis or NULL. */
    Con *first, *second;
    /* Corresponding axis of mouse coordinates.
     * Used to track and to revert changes. */
    uint32_t prev;
    double orig;
};

struct callback_params {
    struct callback_params_dim horisontal;
    struct callback_params_dim vertical;
};

static void reset_percent(const struct callback_params_dim *d) {
    double sum = d->first->percent + d->second->percent;
    d->first->percent = sum * d->orig;
    d->second->percent = sum * (1 - d->orig);
    con_fix_percent(d->first->parent);
}

DRAGGING_CB(resize_callback) {
    const struct callback_params *params = extra;
    bool updated = false;
    DLOG("new x = %d, y = %d\n", new_x, new_y);

    if (params->horisontal.first != NULL) {
        reset_percent(&params->horisontal);
        updated |= resize_neighboring_cons(
            params->horisontal.first,
            params->horisontal.second,
            new_x - params->horisontal.prev, 0);
    }
    if (params->vertical.first != NULL) {
        reset_percent(&params->vertical);
        updated |= resize_neighboring_cons(
            params->vertical.first,
            params->vertical.second,
            new_y - params->vertical.prev, 0);
    }

    if (updated) {
        tree_render();
    }
}

bool resize_find_tiling_participants(Con **current, Con **other, direction_t direction, bool both_sides) {
    DLOG("Find two participants for resizing container=%p in direction=%i\n", other, direction);
    Con *first = *current;
    Con *second = NULL;
    if (first == NULL) {
        DLOG("Current container is NULL, aborting.\n");
        *current = *other = NULL;
        return false;
    }

    /* Go up in the tree and search for a container to resize */
    const orientation_t search_orientation = orientation_from_direction(direction);
    const bool dir_backwards = (direction == D_UP || direction == D_LEFT);
    while (first->type != CT_WORKSPACE &&
           first->type != CT_FLOATING_CON &&
           second == NULL) {
        /* get the appropriate first container with the matching
         * orientation (skip stacked/tabbed cons) */
        if ((con_orientation(first->parent) != search_orientation) ||
            (first->parent->layout == L_STACKED) ||
            (first->parent->layout == L_TABBED)) {
            first = first->parent;
            continue;
        }

        /* get the counterpart for this resizement */
        if (dir_backwards) {
            second = TAILQ_PREV(first, nodes_head, nodes);
            if (second == NULL && both_sides == true) {
                second = TAILQ_NEXT(first, nodes);
            }
        } else {
            second = TAILQ_NEXT(first, nodes);
            if (second == NULL && both_sides == true) {
                second = TAILQ_PREV(first, nodes_head, nodes);
            }
        }

        if (second == NULL) {
            DLOG("No second container in this direction found, trying to look further up in the tree...\n");
            first = first->parent;
        }
    }

    DLOG("Found participants: first=%p and second=%p.\n", first, second);
    *current = first;
    *other = second;
    if (first == NULL || second == NULL) {
        DLOG("Could not find two participants for this resize request.\n");
        *current = *other = NULL;
        return false;
    }

    return true;
}

resize_direction_t resize_find_tiling_participants_two_axes(
    Con *con, resize_direction_t dir, resize_params_t *p) {
    p->output = con_get_output(con);
    p->first_h = NULL;
    p->first_v = NULL;
    p->second_h = NULL;
    p->second_v = NULL;

    if (dir & RD_LEFT) {
        p->first_h = con;
        resize_find_tiling_participants(&p->first_h, &p->second_h, D_LEFT, false);
        /* The first container should always be in front of the second container */
        SWAP(p->first_h, p->second_h, Con *);
    } else if (dir & RD_RIGHT) {
        p->first_h = con;
        resize_find_tiling_participants(&p->first_h, &p->second_h, D_RIGHT, false);
    }
    if (dir & RD_UP) {
        p->first_v = con;
        resize_find_tiling_participants(&p->first_v, &p->second_v, D_UP, false);
        /* The first container should always be in front of the second container */
        SWAP(p->first_v, p->second_v, Con *);
    } else if (dir & RD_DOWN) {
        p->first_v = con;
        resize_find_tiling_participants(&p->first_v, &p->second_v, D_DOWN, false);
    }

    if (p->first_h != NULL && p->first_h->fullscreen_mode != p->second_h->fullscreen_mode) {
        DLOG("Avoiding horisontal resize between containers with different fullscreen modes, %d != %d\n",
             p->first_h->fullscreen_mode, p->second_h->fullscreen_mode);
        p->first_h = p->second_h = NULL;
    }
    if (p->first_v != NULL && p->first_v->fullscreen_mode != p->second_v->fullscreen_mode) {
        DLOG("Avoiding vertical resize between containers with different fullscreen modes, %d != %d\n",
             p->first_v->fullscreen_mode, p->second_v->fullscreen_mode);
        p->first_v = p->second_v = NULL;
    }

    /* Unset bits in the bitmask if corresponding direction is not found. */
    dir &= (p->first_v ? RD_UP | RD_DOWN : 0) | (p->first_h ? RD_LEFT | RD_RIGHT : 0);

    DLOG("Directions found: 0x%x\n", dir);

    return dir;
}

/*
 * Calculate the minimum percent needed for the given container to be at least 1
 * pixel.
 *
 */
double percent_for_1px(Con *con) {
    const int parent_size = con_rect_size_in_orientation(con->parent);
    /* deco_rect.height is subtracted from each child in render_con_split */
    const int min_size = (con_orientation(con->parent) == HORIZ ? 1 : 1 + con->deco_rect.height);
    return ((double)min_size / (double)parent_size);
}

/*
 * Resize the two given containers using the given amount of pixels or
 * percentage points. One of the two needs to be 0. A positive amount means
 * growing the first container while a negative means shrinking it.
 * Return false when resize is not performed due to container minimum size
 * constraints.
 *
 */
bool resize_neighboring_cons(Con *first, Con *second, int px, int ppt) {
    assert(px * ppt == 0);

    Con *parent = first->parent;

    const double min_first =
        parent->layout == L_SPLITH
            ? (double)first->min_size.w / parent->rect.width
            : (double)first->min_size.h / parent->rect.height;
    const double min_second =
        parent->layout == L_SPLITH
            ? (double)second->min_size.w / parent->rect.width
            : (double)second->min_size.h / parent->rect.height;

    /* Refuse to shrink a con if it's already below its minimum size. */
    if ((first->percent < min_first && (px + ppt) < 0) ||
        (second->percent < min_second && (px + ppt) > 0)) {
        return false;
    }

    /* Convert to change in percentages. */
    const double pct =
        ppt ? (double)ppt / 100.0
            : (double)px / (double)con_rect_size_in_orientation(parent);

    first->percent += pct;
    second->percent -= pct;

    /* If we can't resize while keeping minimum size, do the best what we can. */
    if (first->percent < min_first && pct < 0) {
        second->percent -= min_first - first->percent;
        first->percent = min_first;
    }
    if (second->percent < min_second && pct > 0) {
        first->percent -= min_second - second->percent;
        second->percent = min_second;
    }

    con_fix_percent(parent);
    return true;
}

void resize_graphical_handler(const xcb_button_press_event_t *event,
                              enum xcursor_cursor_t cursor,
                              bool use_threshold,
                              resize_params_t *p) {
    x_mask_event_mask(~XCB_EVENT_MASK_ENTER_WINDOW);
    xcb_flush(conn);

    uint32_t mask = 0;
    uint32_t values[2];

    mask = XCB_CW_OVERRIDE_REDIRECT;
    values[0] = 1;

    /* Open a new window, the resizebar. Grab the pointer and move the window
     * around as the user moves the pointer. */
    xcb_window_t grabwin = create_window(conn, p->output->rect, XCB_COPY_FROM_PARENT, XCB_COPY_FROM_PARENT,
                                         XCB_WINDOW_CLASS_INPUT_ONLY, XCURSOR_CURSOR_POINTER, true, mask, values);

    xcb_flush(conn);

    const struct callback_params params = {
        .horisontal = {
            p->first_h,
            p->second_h,
            event->root_x,
            p->first_h ? p->first_h->percent / (p->first_h->percent + p->second_h->percent) : 0,
        },
        .vertical = {
            p->first_v,
            p->second_v,
            event->root_y,
            p->first_v ? p->first_v->percent / (p->first_v->percent + p->second_v->percent) : 0,
        },
    };

    /* Re-render the tree before returning to the event loop (drag_pointer()
     * runs its own event-loop) in case if there are unrendered updates. */
    tree_render();

    /* `drag_pointer' blocks until the drag is completed. */
    drag_result_t drag_result = drag_pointer(NULL, event, grabwin, cursor, use_threshold, resize_callback, &params);

    xcb_destroy_window(conn, grabwin);
    xcb_flush(conn);

    /* Undo resize if user cancelled the drag. */
    if (drag_result == DRAG_REVERT) {
        if (p->first_h != NULL) {
            reset_percent(&params.horisontal);
        }
        if (p->first_v != NULL) {
            reset_percent(&params.vertical);
        }
        tree_render();
    }
}

/*
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
    resize_direction_t dir, bool unidirectional_arrow) {
    if (unidirectional_arrow) {
        switch (dir) {
            case RD_NONE:
                return XCURSOR_CURSOR_NOT_ALLOWED;
            case RD_UP:
                return XCURSOR_CURSOR_TOP_SIDE;
            case RD_DOWN:
                return XCURSOR_CURSOR_BOTTOM_SIDE;
            case RD_LEFT:
                return XCURSOR_CURSOR_LEFT_SIDE;
            case RD_RIGHT:
                return XCURSOR_CURSOR_RIGHT_SIDE;
            case RD_UP_LEFT:
                return XCURSOR_CURSOR_TOP_LEFT_CORNER;
            case RD_UP_RIGHT:
                return XCURSOR_CURSOR_TOP_RIGHT_CORNER;
            case RD_DOWN_LEFT:
                return XCURSOR_CURSOR_BOTTOM_LEFT_CORNER;
            case RD_DOWN_RIGHT:
                return XCURSOR_CURSOR_BOTTOM_RIGHT_CORNER;
        }
    } else {
        switch (dir) {
            case RD_NONE:
                return XCURSOR_CURSOR_NOT_ALLOWED;
            case RD_UP:
            case RD_DOWN:
                return XCURSOR_CURSOR_RESIZE_VERTICAL;
            case RD_LEFT:
            case RD_RIGHT:
                return XCURSOR_CURSOR_RESIZE_HORIZONTAL;
            case RD_UP_LEFT:
            case RD_DOWN_RIGHT:
            case RD_UP_RIGHT:
            case RD_DOWN_LEFT:
                return XCURSOR_CURSOR_MOVE;
        }
    }
    /* Should never happen */
    return 0;
}

/*
 * Return resize direction for a container based on the click coordinates and
 * destination.
 *
 */
resize_direction_t get_resize_direction(Con *con, int x, int y, click_destination_t dest) {
    if (dest == CLICK_INSIDE) {
        /* Return a corresponding direction according to the following figure:
         *
         * +---+---+---+
         * |   |   |   |
         * | 2 |   | 2 |
         * |   | 1 |   |
         * +---+   +---+
         * |    \ /    |
         * |  1  X  1  |
         * |    / \    |
         * +---+   +---+
         * |   | 1 |   |
         * | 2 |   | 2 |
         * |   |   |   |
         * +---+---+---+
         *
         * 1 - sides, one-axis drag
         * 2 - corners, two-axis drag
         */

        /* Relative to inner rectangle. */
        const Rect r = rect_add(con->rect, con_border_style_rect(con));
        x -= r.x;
        y -= r.y;

        /* xb and yb are coordinates of a cell in a 3x3 grid */
        const int xb = MIN(2, MAX(0, 3 * x / (int)r.width));
        const int yb = MIN(2, MAX(0, 3 * y / (int)r.height));

        if (xb == 1 && yb == 1) {
            /* Central grid cell */
            const bool under_major_diagonal = x * r.height > y * r.width;
            const bool under_minor_diagonal = x * r.height > (r.height - y) * r.width;
            if (under_major_diagonal) {
                return under_minor_diagonal ? RD_UP : RD_RIGHT;
            } else {
                return under_minor_diagonal ? RD_LEFT : RD_DOWN;
            }
        } else {
            /* Either a side or a corner cell */
            return (xb == 0 ? RD_LEFT : xb == 2 ? RD_RIGHT : 0) |
                   (yb == 0 ? RD_UP : yb == 2 ? RD_DOWN : 0);
        }
    } else {
        const int corner_size = logical_px(24);
        const Rect bsr = con_border_style_rect(con);

        /* Make coordinates relative to con->rect */
        x -= con->rect.x;
        y -= con->rect.y;

        /* How deep inside each corner the click was */
        const int x0 = corner_size - x + bsr.x;
        const int x1 = corner_size + x - bsr.x - con->rect.width + 1 - bsr.width;
        const int y0 = corner_size - y + bsr.y;
        const int y1 = corner_size + y - bsr.y - con->rect.height + 1 - bsr.height;

        resize_direction_t dir = RD_NONE;

        if (x0 > 0 && x1 > 0) {
            dir |= (x0 > x1 ? RD_LEFT : RD_RIGHT);
        } else if (x0 > 0) {
            dir |= RD_LEFT;
        } else if (x1 > 0) {
            dir |= RD_RIGHT;
        }

        if (dest == CLICK_DECORATION) {
            dir |= RD_UP;
        } else if (y0 > 0 && y1 > 0) {
            dir |= (y0 > y1 ? RD_UP : RD_DOWN);
        } else if (y0 > 0) {
            dir |= RD_UP;
        } else if (y1 > 0) {
            dir |= RD_DOWN;
        }

        return dir;
    }
}
