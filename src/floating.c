/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * floating.c: Floating windows.
 *
 */
#include "all.h"

#include <math.h>

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

/*
 * Calculates sum of heights and sum of widths of all currently active outputs
 *
 */
static Rect total_outputs_dimensions(void) {
    if (TAILQ_EMPTY(&outputs))
        return (Rect){0, 0, root_screen->width_in_pixels, root_screen->height_in_pixels};

    Output *output;
    /* Use Rect to encapsulate dimensions, ignoring x/y */
    Rect outputs_dimensions = {0, 0, 0, 0};
    TAILQ_FOREACH (output, &outputs, outputs) {
        outputs_dimensions.height += output->rect.height;
        outputs_dimensions.width += output->rect.width;
    }
    return outputs_dimensions;
}

/*
 * Updates I3_FLOATING_WINDOW by either setting or removing it on the con and
 * all its children.
 *
 */
static void floating_set_hint_atom(Con *con, bool floating) {
    if (!con_is_leaf(con)) {
        Con *child;
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            floating_set_hint_atom(child, floating);
        }
    }

    if (con->window == NULL) {
        return;
    }

    if (floating) {
        uint32_t val = 1;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, con->window->id,
                            A_I3_FLOATING_WINDOW, XCB_ATOM_CARDINAL, 32, 1, &val);
    } else {
        xcb_delete_property(conn, con->window->id, A_I3_FLOATING_WINDOW);
    }

    xcb_flush(conn);
}

/*
 * Called when a floating window is created or resized.  This function resizes
 * the window if its size is higher or lower than the configured maximum/minimum
 * size, respectively or when adjustments are needed to conform to the
 * configured size increments or aspect ratio limits.
 *
 * When prefer_height is true and the window needs to be resized because of the
 * configured aspect ratio, the width is adjusted first, preserving the previous
 * height.
 *
 */
void floating_check_size(Con *floating_con, bool prefer_height) {
    /* Define reasonable minimal and maximal sizes for floating windows */
    const int floating_sane_min_height = 50;
    const int floating_sane_min_width = 75;
    Rect floating_sane_max_dimensions;
    Con *focused_con = con_descend_focused(floating_con);

    Rect border_rect = con_border_style_rect(focused_con);
    /* We have to do the opposite calculations that render_con() do
     * to get the exact size we want. */
    border_rect.width = -border_rect.width;
    border_rect.width += 2 * focused_con->border_width;
    border_rect.height = -border_rect.height;
    border_rect.height += 2 * focused_con->border_width;
    if (con_border_style(focused_con) == BS_NORMAL) {
        border_rect.height += render_deco_height();
    }

    i3Window *window = focused_con->window;
    if (window != NULL) {
        /* ICCCM says: If a base size is not provided, the minimum size is to be used in its place
         * and vice versa. */
        int min_width = (window->min_width ? window->min_width : window->base_width);
        int min_height = (window->min_height ? window->min_height : window->base_height);
        int base_width = (window->base_width ? window->base_width : window->min_width);
        int base_height = (window->base_height ? window->base_height : window->min_height);

        if (min_width) {
            floating_con->rect.width -= border_rect.width;
            floating_con->rect.width = max(floating_con->rect.width, min_width);
            floating_con->rect.width += border_rect.width;
        }

        if (min_height) {
            floating_con->rect.height -= border_rect.height;
            floating_con->rect.height = max(floating_con->rect.height, min_height);
            floating_con->rect.height += border_rect.height;
        }

        if (window->max_width) {
            floating_con->rect.width -= border_rect.width;
            floating_con->rect.width = min(floating_con->rect.width, window->max_width);
            floating_con->rect.width += border_rect.width;
        }

        if (window->max_height) {
            floating_con->rect.height -= border_rect.height;
            floating_con->rect.height = min(floating_con->rect.height, window->max_height);
            floating_con->rect.height += border_rect.height;
        }

        /* Obey the aspect ratio, if any, unless we are in fullscreen mode.
         *
         * The spec isn’t explicit on whether the aspect ratio hints should be
         * respected during fullscreen mode. Other WMs such as Openbox don’t do
         * that, and this post suggests that this is the correct way to do it:
         * https://mail.gnome.org/archives/wm-spec-list/2003-May/msg00007.html
         *
         * Ignoring aspect ratio during fullscreen was necessary to fix MPlayer
         * subtitle rendering, see https://bugs.i3wm.org/594 */
        const double min_ar = window->min_aspect_ratio;
        const double max_ar = window->max_aspect_ratio;
        if (floating_con->fullscreen_mode == CF_NONE && (min_ar > 0 || max_ar > 0)) {
            /* The ICCCM says to subtract the base size from the window size for
             * aspect ratio calculations. However, unlike determining the base
             * size itself we must not fall back to using the minimum size in
             * this case according to the ICCCM. */
            double width = floating_con->rect.width - window->base_width - border_rect.width;
            double height = floating_con->rect.height - window->base_height - border_rect.height;
            const double ar = (double)width / (double)height;
            double new_ar = -1;
            if (min_ar > 0 && ar < min_ar) {
                new_ar = min_ar;
            } else if (max_ar > 0 && ar > max_ar) {
                new_ar = max_ar;
            }
            if (new_ar > 0) {
                if (prefer_height) {
                    width = round(height * new_ar);
                    height = round(width / new_ar);
                } else {
                    height = round(width / new_ar);
                    width = round(height * new_ar);
                }
                floating_con->rect.width = width + window->base_width + border_rect.width;
                floating_con->rect.height = height + window->base_height + border_rect.height;
            }
        }

        if (window->height_increment &&
            floating_con->rect.height >= base_height + border_rect.height) {
            floating_con->rect.height -= base_height + border_rect.height;
            floating_con->rect.height -= floating_con->rect.height % window->height_increment;
            floating_con->rect.height += base_height + border_rect.height;
        }

        if (window->width_increment &&
            floating_con->rect.width >= base_width + border_rect.width) {
            floating_con->rect.width -= base_width + border_rect.width;
            floating_con->rect.width -= floating_con->rect.width % window->width_increment;
            floating_con->rect.width += base_width + border_rect.width;
        }
    }

    /* Unless user requests otherwise (-1), raise the width/height to
     * reasonable minimum dimensions */
    if (config.floating_minimum_height != -1) {
        floating_con->rect.height -= border_rect.height;
        if (config.floating_minimum_height == 0) {
            floating_con->rect.height = max(floating_con->rect.height, floating_sane_min_height);
        } else {
            floating_con->rect.height = max(floating_con->rect.height, config.floating_minimum_height);
        }
        floating_con->rect.height += border_rect.height;
    }

    if (config.floating_minimum_width != -1) {
        floating_con->rect.width -= border_rect.width;
        if (config.floating_minimum_width == 0) {
            floating_con->rect.width = max(floating_con->rect.width, floating_sane_min_width);
        } else {
            floating_con->rect.width = max(floating_con->rect.width, config.floating_minimum_width);
        }
        floating_con->rect.width += border_rect.width;
    }

    /* Unless user requests otherwise (-1), ensure width/height do not exceed
     * configured maxima or, if unconfigured, limit to combined width of all
     * outputs */
    floating_sane_max_dimensions = total_outputs_dimensions();
    if (config.floating_maximum_height != -1) {
        floating_con->rect.height -= border_rect.height;
        if (config.floating_maximum_height == 0) {
            floating_con->rect.height = min(floating_con->rect.height, floating_sane_max_dimensions.height);
        } else {
            floating_con->rect.height = min(floating_con->rect.height, config.floating_maximum_height);
        }
        floating_con->rect.height += border_rect.height;
    }

    if (config.floating_maximum_width != -1) {
        floating_con->rect.width -= border_rect.width;
        if (config.floating_maximum_width == 0) {
            floating_con->rect.width = min(floating_con->rect.width, floating_sane_max_dimensions.width);
        } else {
            floating_con->rect.width = min(floating_con->rect.width, config.floating_maximum_width);
        }
        floating_con->rect.width += border_rect.width;
    }
}

bool floating_enable(Con *con, bool automatic) {
    bool set_focus = (con == focused);

    if (con_is_docked(con)) {
        LOG("Container is a dock window, not enabling floating mode.\n");
        return false;
    }

    if (con_is_floating(con)) {
        LOG("Container is already in floating mode, not doing anything.\n");
        return false;
    }

    if (con->type == CT_WORKSPACE) {
        LOG("Container is a workspace, not enabling floating mode.\n");
        return false;
    }

    Con *focus_head_placeholder = NULL;
    bool focus_before_parent = true;
    if (!set_focus) {
        /* Find recursively the ancestor container which is a child of our workspace.
         * We need to reuse its focus position later. */
        Con *ancestor = con;
        while (ancestor->parent->type != CT_WORKSPACE) {
            focus_before_parent &= TAILQ_FIRST(&(ancestor->parent->focus_head)) == ancestor;
            ancestor = ancestor->parent;
        }
        /* Consider the part of the focus stack of our current workspace:
         * [ ... S_{i-1} S_{i} S_{i+1} ... ]
         * Where S_{x} is a container tree and the container 'con' that is beeing switched to
         * floating belongs in S_{i}. The new floating container, 'nc', will have the
         * workspace as its parent so it needs to be placed in this stack. If C was focused
         * we just need to call con_focus(). Otherwise, nc must be placed before or after S_{i}.
         * We should avoid using the S_{i} container for our operations since it might get
         * killed if it has no other children. So, the two possible positions are after S_{i-1}
         * or before S_{i+1}.
         */
        if (focus_before_parent) {
            focus_head_placeholder = TAILQ_PREV(ancestor, focus_head, focused);
        } else {
            focus_head_placeholder = TAILQ_NEXT(ancestor, focused);
        }
    }

    /* 1: detach the container from its parent */
    /* TODO: refactor this with tree_close_internal() */
    con_detach(con);
    con_fix_percent(con->parent);

    /* 2: create a new container to render the decoration on, add
     * it as a floating window to the workspace */
    Con *nc = con_new(NULL, NULL);
    /* we need to set the parent afterwards instead of passing it as an
     * argument to con_new() because nc would be inserted into the tiling layer
     * otherwise. */
    Con *ws = con_get_workspace(con);
    nc->parent = ws;
    nc->type = CT_FLOATING_CON;
    nc->layout = L_SPLITH;
    /* We insert nc already, even though its rect is not yet calculated. This
     * is necessary because otherwise the workspace might be empty (and get
     * closed in tree_close_internal()) even though it’s not. */
    TAILQ_INSERT_HEAD(&(ws->floating_head), nc, floating_windows);

    struct focus_head *fh = &(ws->focus_head);
    if (focus_before_parent) {
        if (focus_head_placeholder) {
            TAILQ_INSERT_AFTER(fh, focus_head_placeholder, nc, focused);
        } else {
            TAILQ_INSERT_HEAD(fh, nc, focused);
        }
    } else {
        if (focus_head_placeholder) {
            TAILQ_INSERT_BEFORE(focus_head_placeholder, nc, focused);
        } else {
            /* Also used for the set_focus case */
            TAILQ_INSERT_TAIL(fh, nc, focused);
        }
    }

    /* check if the parent container is empty and close it if so */
    if ((con->parent->type == CT_CON || con->parent->type == CT_FLOATING_CON) &&
        con_num_children(con->parent) == 0) {
        DLOG("Old container empty after setting this child to floating, closing\n");
        Con *parent = con->parent;
        /* clear the pointer before calling tree_close_internal in which the memory is freed */
        con->parent = NULL;
        tree_close_internal(parent, DONT_KILL_WINDOW, false);
    }

    char *name;
    sasprintf(&name, "[i3 con] floatingcon around %p", con);
    x_set_name(nc, name);
    free(name);

    /* find the height for the decorations */
    int deco_height = render_deco_height();

    DLOG("Original rect: (%d, %d) with %d x %d\n", con->rect.x, con->rect.y, con->rect.width, con->rect.height);
    DLOG("Geometry = (%d, %d) with %d x %d\n", con->geometry.x, con->geometry.y, con->geometry.width, con->geometry.height);
    nc->rect = con->geometry;
    /* If the geometry was not set (split containers), we need to determine a
     * sensible one by combining the geometry of all children */
    if (rect_equals(nc->rect, (Rect){0, 0, 0, 0})) {
        DLOG("Geometry not set, combining children\n");
        Con *child;
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            DLOG("child geometry: %d x %d\n", child->geometry.width, child->geometry.height);
            nc->rect.width += child->geometry.width;
            nc->rect.height = max(nc->rect.height, child->geometry.height);
        }
    }

    TAILQ_INSERT_TAIL(&(nc->nodes_head), con, nodes);
    TAILQ_INSERT_TAIL(&(nc->focus_head), con, focused);

    /* 3: attach the child to the new parent container. We need to do this
     * because con_border_style_rect() needs to access con->parent. */
    con->parent = nc;
    con->percent = 1.0;
    con->floating = FLOATING_USER_ON;

    /* 4: set the border style as specified with new_float */
    if (automatic)
        con->border_style = config.default_floating_border;

    /* Add pixels for the decoration. */
    Rect border_style_rect = con_border_style_rect(con);

    nc->rect.height -= border_style_rect.height;
    nc->rect.width -= border_style_rect.width;

    /* Add some more pixels for the title bar */
    if (con_border_style(con) == BS_NORMAL) {
        nc->rect.height += deco_height;
    }

    /* Honor the X11 border */
    nc->rect.height += con->border_width * 2;
    nc->rect.width += con->border_width * 2;

    floating_check_size(nc, false);

    /* Some clients (like GIMP’s color picker window) get mapped
     * to (0, 0), so we push them to a reasonable position
     * (centered over their leader) */
    if (nc->rect.x == 0 && nc->rect.y == 0) {
        Con *leader;
        if (con->window && con->window->leader != XCB_NONE &&
            con->window->id != con->window->leader &&
            (leader = con_by_window_id(con->window->leader)) != NULL) {
            DLOG("Centering above leader\n");
            floating_center(nc, leader->rect);
        } else {
            /* center the window on workspace as fallback */
            floating_center(nc, ws->rect);
        }
    }

    /* Sanity check: Are the coordinates on the appropriate output? If not, we
     * need to change them */
    Output *current_output = get_output_from_rect(nc->rect);
    Con *correct_output = con_get_output(ws);
    if (!current_output || current_output->con != correct_output) {
        DLOG("This floating window is on the wrong output, fixing coordinates (currently (%d, %d))\n",
             nc->rect.x, nc->rect.y);

        /* If moving from one output to another, keep the relative position
         * consistent (e.g. a centered dialog will remain centered). */
        if (current_output) {
            floating_fix_coordinates(nc, &current_output->con->rect, &correct_output->rect);
            /* Make sure that the result is in the correct output. */
            current_output = get_output_from_rect(nc->rect);
        }
        if (!current_output || current_output->con != correct_output) {
            floating_center(nc, ws->rect);
        }
    }

    DLOG("Floating rect: (%d, %d) with %d x %d\n", nc->rect.x, nc->rect.y, nc->rect.width, nc->rect.height);

    /* 5: Subtract the deco_height in order to make the floating window appear
     * at precisely the position it specified in its original geometry (which
     * is what applications might remember). */
    deco_height = (con->border_style == BS_NORMAL ? render_deco_height() : 0);
    nc->rect.y -= deco_height;

    DLOG("Corrected y = %d (deco_height = %d)\n", nc->rect.y, deco_height);

    /* render the cons to get initial window_rect correct */
    render_con(nc);

    if (set_focus)
        con_activate(con);

    floating_set_hint_atom(nc, true);
    ipc_send_window_event("floating", con);
    return true;
}

void floating_disable(Con *con) {
    if (!con_is_floating(con)) {
        LOG("Container isn't floating, not doing anything.\n");
        return;
    }

    Con *ws = con_get_workspace(con);
    if (con_is_internal(ws)) {
        LOG("Can't disable floating for container in internal workspace.\n");
        return;
    }
    Con *tiling_focused = con_descend_tiling_focused(ws);

    if (tiling_focused->type == CT_WORKSPACE) {
        Con *parent = con->parent;
        con_detach(con);
        con->parent = NULL;
        tree_close_internal(parent, DONT_KILL_WINDOW, true);
        con_attach(con, tiling_focused, false);
        con->percent = 0.0;
        con_fix_percent(con->parent);
    } else {
        insert_con_into(con, tiling_focused,
                        (tiling_focused->parent->layout_fill_order == LF_DEFAULT)
                            ? AFTER
                            : BEFORE);
    }

    con->floating = FLOATING_USER_OFF;
    floating_set_hint_atom(con, false);
    ipc_send_window_event("floating", con);
}

/*
 * Toggles floating mode for the given container.
 *
 * If the automatic flag is set to true, this was an automatic update by a change of the
 * window class from the application which can be overwritten by the user.
 *
 */
void toggle_floating_mode(Con *con, bool automatic) {
    /* forbid the command to toggle floating on a CT_FLOATING_CON */
    if (con->type == CT_FLOATING_CON) {
        ELOG("Cannot toggle floating mode on con = %p because it is of type CT_FLOATING_CON.\n", con);
        return;
    }

    /* see if the client is already floating */
    if (con_is_floating(con)) {
        LOG("already floating, re-setting to tiling\n");

        floating_disable(con);
        return;
    }

    floating_enable(con, automatic);
}

/*
 * Raises the given container in the list of floating containers
 *
 */
void floating_raise_con(Con *con) {
    DLOG("Raising floating con %p / %s\n", con, con->name);
    TAILQ_REMOVE(&(con->parent->floating_head), con, floating_windows);
    TAILQ_INSERT_TAIL(&(con->parent->floating_head), con, floating_windows);
}

/*
 * Checks if con’s coordinates are within its workspace and re-assigns it to
 * the actual workspace if not.
 *
 */
bool floating_maybe_reassign_ws(Con *con) {
    if (con_is_internal(con_get_workspace(con))) {
        DLOG("Con in an internal workspace\n");
        return false;
    }

    Output *output = get_output_from_rect(con->rect);

    if (!output) {
        ELOG("No output found at destination coordinates?\n");
        return false;
    }

    if (con_get_output(con) == output->con) {
        DLOG("still the same ws\n");
        return false;
    }

    DLOG("Need to re-assign!\n");

    Con *content = output_get_content(output->con);
    Con *ws = TAILQ_FIRST(&(content->focus_head));
    DLOG("Moving con %p / %s to workspace %p / %s\n", con, con->name, ws, ws->name);
    Con *needs_focus = con_descend_focused(con);
    if (!con_inside_focused(needs_focus)) {
        needs_focus = NULL;
    }
    con_move_to_workspace(con, ws, false, true, false);
    if (needs_focus) {
        workspace_show(ws);
        con_activate(needs_focus);
    }
    return true;
}

/*
 * Centers a floating con above the specified rect.
 *
 */
void floating_center(Con *con, Rect rect) {
    con->rect.x = rect.x + (rect.width / 2) - (con->rect.width / 2);
    con->rect.y = rect.y + (rect.height / 2) - (con->rect.height / 2);
}

/*
 * Moves the given floating con to the current pointer position.
 *
 */
void floating_move_to_pointer(Con *con) {
    assert(con->type == CT_FLOATING_CON);

    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    if (reply == NULL) {
        ELOG("could not query pointer position, not moving this container\n");
        return;
    }

    Output *output = get_output_containing(reply->root_x, reply->root_y);
    if (output == NULL) {
        ELOG("The pointer is not on any output, cannot move the container here.\n");
        return;
    }

    /* Determine where to put the window. */
    int32_t x = reply->root_x - con->rect.width / 2;
    int32_t y = reply->root_y - con->rect.height / 2;
    FREE(reply);

    /* Correct target coordinates to be in-bounds. */
    x = MAX(x, (int32_t)output->rect.x);
    y = MAX(y, (int32_t)output->rect.y);
    if (x + con->rect.width > output->rect.x + output->rect.width)
        x = output->rect.x + output->rect.width - con->rect.width;
    if (y + con->rect.height > output->rect.y + output->rect.height)
        y = output->rect.y + output->rect.height - con->rect.height;

    /* Update container's coordinates to position it correctly. */
    floating_reposition(con, (Rect){x, y, con->rect.width, con->rect.height});
}

DRAGGING_CB(drag_window_callback) {
    /* Reposition the client correctly while moving */
    con->rect.x = old_rect->x + (new_x - event->root_x);
    con->rect.y = old_rect->y + (new_y - event->root_y);

    render_con(con);
    x_push_node(con);
    xcb_flush(conn);

    /* Check if we cross workspace boundaries while moving */
    if (!floating_maybe_reassign_ws(con))
        return;
    /* Ensure not to warp the pointer while dragging */
    x_set_warp_to(NULL);
    tree_render();
}

/*
 * Called when the user clicked on the titlebar of a floating window.
 * Calls the drag_pointer function with the drag_window callback
 *
 */
void floating_drag_window(Con *con, const xcb_button_press_event_t *event, bool use_threshold) {
    DLOG("floating_drag_window\n");

    /* Push changes before dragging, so that the window gets raised now and not
     * after the user releases the mouse button */
    tree_render();

    /* Store the initial rect in case of user revert/cancel */
    Rect initial_rect = con->rect;

    /* Drag the window */
    drag_result_t drag_result = drag_pointer(con, event, XCB_NONE, XCURSOR_CURSOR_MOVE, use_threshold, drag_window_callback, NULL);

    if (!con_exists(con)) {
        DLOG("The container has been closed in the meantime.\n");
        return;
    }

    /* If the user cancelled, undo the changes. */
    if (drag_result == DRAG_REVERT) {
        floating_reposition(con, initial_rect);
        return;
    }

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (con->scratchpad_state == SCRATCHPAD_FRESH)
        con->scratchpad_state = SCRATCHPAD_CHANGED;

    tree_render();
}

/*
 * This is an ugly data structure which we need because there is no standard
 * way of having nested functions (only available as a gcc extension at the
 * moment, clang doesn’t support it) or blocks (only available as a clang
 * extension and only on Mac OS X systems at the moment).
 *
 */
struct resize_window_callback_params {
    const border_t corner;
    const bool proportional;
};

DRAGGING_CB(resize_window_callback) {
    const struct resize_window_callback_params *params = extra;
    border_t corner = params->corner;

    int32_t dest_x = con->rect.x;
    int32_t dest_y = con->rect.y;
    uint32_t dest_width;
    uint32_t dest_height;

    double ratio = (double)old_rect->width / old_rect->height;

    /* First guess: We resize by exactly the amount the mouse moved,
     * taking into account in which corner the client was grabbed */
    if (corner & BORDER_LEFT)
        dest_width = old_rect->width - (new_x - event->root_x);
    else
        dest_width = old_rect->width + (new_x - event->root_x);

    if (corner & BORDER_TOP)
        dest_height = old_rect->height - (new_y - event->root_y);
    else
        dest_height = old_rect->height + (new_y - event->root_y);

    /* User wants to keep proportions, so we may have to adjust our values */
    if (params->proportional) {
        dest_width = max(dest_width, (int)(dest_height * ratio));
        dest_height = max(dest_height, (int)(dest_width / ratio));
    }

    con->rect = (Rect){dest_x, dest_y, dest_width, dest_height};

    /* Obey window size */
    floating_check_size(con, false);

    /* If not the lower right corner is grabbed, we must also reposition
     * the client by exactly the amount we resized it */
    if (corner & BORDER_LEFT)
        dest_x = old_rect->x + (old_rect->width - con->rect.width);

    if (corner & BORDER_TOP)
        dest_y = old_rect->y + (old_rect->height - con->rect.height);

    con->rect.x = dest_x;
    con->rect.y = dest_y;

    render_con(con);
    x_push_changes(croot);
}

/*
 * Called when the user clicked on a floating window while holding the
 * floating_modifier and the right mouse button.
 * Calls the drag_pointer function with the resize_window callback
 *
 */
void floating_resize_window(Con *con, const bool proportional,
                            const xcb_button_press_event_t *event) {
    DLOG("floating_resize_window\n");

    /* corner saves the nearest corner to the original click. It contains
     * a bitmask of the nearest borders (BORDER_LEFT, BORDER_RIGHT, …) */
    border_t corner = 0;

    if (event->event_x <= (int16_t)(con->rect.width / 2))
        corner |= BORDER_LEFT;
    else
        corner |= BORDER_RIGHT;

    int cursor = 0;
    if (event->event_y <= (int16_t)(con->rect.height / 2)) {
        corner |= BORDER_TOP;
        cursor = (corner & BORDER_LEFT) ? XCURSOR_CURSOR_TOP_LEFT_CORNER : XCURSOR_CURSOR_TOP_RIGHT_CORNER;
    } else {
        corner |= BORDER_BOTTOM;
        cursor = (corner & BORDER_LEFT) ? XCURSOR_CURSOR_BOTTOM_LEFT_CORNER : XCURSOR_CURSOR_BOTTOM_RIGHT_CORNER;
    }

    struct resize_window_callback_params params = {corner, proportional};

    /* get the initial rect in case of revert/cancel */
    Rect initial_rect = con->rect;

    drag_result_t drag_result = drag_pointer(con, event, XCB_NONE, cursor, false, resize_window_callback, &params);

    if (!con_exists(con)) {
        DLOG("The container has been closed in the meantime.\n");
        return;
    }

    /* If the user cancels, undo the resize */
    if (drag_result == DRAG_REVERT)
        floating_reposition(con, initial_rect);

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (con->scratchpad_state == SCRATCHPAD_FRESH)
        con->scratchpad_state = SCRATCHPAD_CHANGED;
}

/*
 * Repositions the CT_FLOATING_CON to have the coordinates specified by
 * newrect, but only if the coordinates are not out-of-bounds. Also reassigns
 * the floating con to a different workspace if this move was across different
 * outputs.
 *
 */
bool floating_reposition(Con *con, Rect newrect) {
    /* Sanity check: Are the new coordinates on any output? If not, we
     * ignore that request. */
    if (!output_containing_rect(newrect)) {
        ELOG("No output found at destination coordinates. Not repositioning.\n");
        return false;
    }

    con->rect = newrect;

    floating_maybe_reassign_ws(con);

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (con->scratchpad_state == SCRATCHPAD_FRESH)
        con->scratchpad_state = SCRATCHPAD_CHANGED;

    tree_render();
    return true;
}

/*
 * Sets size of the CT_FLOATING_CON to specified dimensions. Might limit the
 * actual size with regard to size constraints taken from user settings.
 * Additionally, the dimensions may be upscaled until they're divisible by the
 * window's size hints.
 *
 */
void floating_resize(Con *floating_con, uint32_t x, uint32_t y) {
    DLOG("floating resize to %dx%d px\n", x, y);
    Rect *rect = &floating_con->rect;
    Con *focused_con = con_descend_focused(floating_con);
    if (focused_con->window == NULL) {
        DLOG("No window is focused. Not resizing.\n");
        return;
    }
    int wi = focused_con->window->width_increment;
    int hi = focused_con->window->height_increment;
    bool prefer_height = (rect->width == x);
    rect->width = x;
    rect->height = y;
    if (wi)
        rect->width += (wi - 1 - rect->width) % wi;
    if (hi)
        rect->height += (hi - 1 - rect->height) % hi;

    floating_check_size(floating_con, prefer_height);

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (floating_con->scratchpad_state == SCRATCHPAD_FRESH)
        floating_con->scratchpad_state = SCRATCHPAD_CHANGED;
}

/*
 * Fixes the coordinates of the floating window whenever the window gets
 * reassigned to a different output (or when the output’s rect changes).
 *
 */
void floating_fix_coordinates(Con *con, Rect *old_rect, Rect *new_rect) {
    DLOG("Fixing coordinates of floating window %p (rect (%d, %d), %d x %d)\n",
         con, con->rect.x, con->rect.y, con->rect.width, con->rect.height);
    DLOG("old_rect = (%d, %d), %d x %d\n",
         old_rect->x, old_rect->y, old_rect->width, old_rect->height);
    DLOG("new_rect = (%d, %d), %d x %d\n",
         new_rect->x, new_rect->y, new_rect->width, new_rect->height);
    /* First we get the x/y coordinates relative to the x/y coordinates
     * of the output on which the window is on */
    int32_t rel_x = con->rect.x - old_rect->x + (int32_t)(con->rect.width / 2);
    int32_t rel_y = con->rect.y - old_rect->y + (int32_t)(con->rect.height / 2);
    /* Then we calculate a fraction, for example 0.63 for a window
     * which is at y = 1212 of a 1920 px high output */
    DLOG("rel_x = %d, rel_y = %d, fraction_x = %f, fraction_y = %f, output->w = %d, output->h = %d\n",
         rel_x, rel_y, (double)rel_x / old_rect->width, (double)rel_y / old_rect->height,
         old_rect->width, old_rect->height);
    /* Here we have to multiply at first. Or we will lose precision when not compiled with -msse2 */
    con->rect.x = (int32_t)new_rect->x + (double)(rel_x * (int32_t)new_rect->width) / (int32_t)old_rect->width - (int32_t)(con->rect.width / 2);
    con->rect.y = (int32_t)new_rect->y + (double)(rel_y * (int32_t)new_rect->height) / (int32_t)old_rect->height - (int32_t)(con->rect.height / 2);
    DLOG("Resulting coordinates: x = %d, y = %d\n", con->rect.x, con->rect.y);
}
