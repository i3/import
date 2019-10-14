/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * click.c: Button press (mouse click) events.
 *
 */
#include "all.h"

#include <time.h>

typedef enum { CLICK_BORDER = 0,
               CLICK_DECORATION = 1,
               CLICK_INSIDE = 2 } click_destination_t;

/*
 * Finds the correct pair of first/second cons between the resize will take
 * place according to the passed border position (top, left, right, bottom),
 * then calls resize_graphical_handler().
 *
 */
static bool tiling_resize_for_border(Con *con, border_t border, xcb_button_press_event_t *event, bool use_threshold) {
    DLOG("border = %d, con = %p\n", border, con);
    Con *second = NULL;
    Con *first = con;
    direction_t search_direction;
    switch (border) {
        case BORDER_LEFT:
            search_direction = D_LEFT;
            break;
        case BORDER_RIGHT:
            search_direction = D_RIGHT;
            break;
        case BORDER_TOP:
            search_direction = D_UP;
            break;
        case BORDER_BOTTOM:
            search_direction = D_DOWN;
            break;
    }

    bool res = resize_find_tiling_participants(&first, &second, search_direction, false);
    if (!res) {
        DLOG("No second container in this direction found.\n");
        return false;
    }
    if (first->fullscreen_mode != second->fullscreen_mode) {
        DLOG("Avoiding resize between containers with different fullscreen modes, %d != %d\n", first->fullscreen_mode, second->fullscreen_mode);
        return false;
    }

    assert(first != second);
    assert(first->parent == second->parent);

    /* The first container should always be in front of the second container */
    if (search_direction == D_UP || search_direction == D_LEFT) {
        Con *tmp = first;
        first = second;
        second = tmp;
    }

    const orientation_t orientation = ((border == BORDER_LEFT || border == BORDER_RIGHT) ? HORIZ : VERT);

    resize_graphical_handler(first, second, orientation, event, use_threshold);

    DLOG("After resize handler, rendering\n");
    tree_render();
    return true;
}

/*
 * Called when the user clicks using the floating_modifier, but the client is in
 * tiling layout.
 *
 * Returns false if it does not do anything (that is, the click should be sent
 * to the client).
 *
 */
static bool floating_mod_on_tiled_client(Con *con, xcb_button_press_event_t *event) {
    /* The client is in tiling layout. We can still initiate a resize with the
     * right mouse button, by chosing the border which is the most near one to
     * the position of the mouse pointer */
    int to_right = con->rect.width - event->event_x,
        to_left = event->event_x,
        to_top = event->event_y,
        to_bottom = con->rect.height - event->event_y;

    DLOG("click was %d px to the right, %d px to the left, %d px to top, %d px to bottom\n",
         to_right, to_left, to_top, to_bottom);

    if (to_right < to_left &&
        to_right < to_top &&
        to_right < to_bottom)
        return tiling_resize_for_border(con, BORDER_RIGHT, event, false);

    if (to_left < to_right &&
        to_left < to_top &&
        to_left < to_bottom)
        return tiling_resize_for_border(con, BORDER_LEFT, event, false);

    if (to_top < to_right &&
        to_top < to_left &&
        to_top < to_bottom)
        return tiling_resize_for_border(con, BORDER_TOP, event, false);

    if (to_bottom < to_right &&
        to_bottom < to_left &&
        to_bottom < to_top)
        return tiling_resize_for_border(con, BORDER_BOTTOM, event, false);

    return false;
}

/*
 * Finds out which border was clicked on and calls tiling_resize_for_border().
 *
 */
static bool tiling_resize(Con *con, xcb_button_press_event_t *event, const click_destination_t dest, bool use_threshold) {
    /* check if this was a click on the window border (and on which one) */
    Rect bsr = con_border_style_rect(con);
    DLOG("BORDER x = %d, y = %d for con %p, window 0x%08x\n",
         event->event_x, event->event_y, con, event->event);
    DLOG("checks for right >= %d\n", con->window_rect.x + con->window_rect.width);
    if (dest == CLICK_DECORATION) {
        return tiling_resize_for_border(con, BORDER_TOP, event, use_threshold);
    }

    if (event->event_x >= 0 && event->event_x <= (int32_t)bsr.x &&
        event->event_y >= (int32_t)bsr.y && event->event_y <= (int32_t)(con->rect.height + bsr.height))
        return tiling_resize_for_border(con, BORDER_LEFT, event, false);

    if (event->event_x >= (int32_t)(con->window_rect.x + con->window_rect.width) &&
        event->event_y >= (int32_t)bsr.y && event->event_y <= (int32_t)(con->rect.height + bsr.height))
        return tiling_resize_for_border(con, BORDER_RIGHT, event, false);

    if (event->event_y >= (int32_t)(con->window_rect.y + con->window_rect.height))
        return tiling_resize_for_border(con, BORDER_BOTTOM, event, false);

    return false;
}

/*
 * Being called by handle_button_press, this function calls the appropriate
 * functions for resizing/dragging.
 *
 */
static void route_click(Con *con, xcb_button_press_event_t *event, const bool mod_pressed, const click_destination_t dest) {
    DLOG("--> click properties: mod = %d, destination = %d\n", mod_pressed, dest);
    DLOG("--> OUTCOME = %p\n", con);
    DLOG("type = %d, name = %s\n", con->type, con->name);

    /* don’t handle dockarea cons, they must not be focused */
    if (con->parent->type == CT_DOCKAREA)
        goto done;

    /* if the user has bound an action to this click, it should override the
     * default behavior. */
    Binding *bind = get_binding_from_xcb_event((xcb_generic_event_t *)event);
    if (bind && ((dest == CLICK_DECORATION && !bind->exclude_titlebar) ||
                 (dest == CLICK_INSIDE && bind->whole_window) ||
                 (dest == CLICK_BORDER && bind->border))) {
        CommandResult *result = run_binding(bind, con);

        /* ASYNC_POINTER eats the event */
        xcb_allow_events(conn, XCB_ALLOW_ASYNC_POINTER, event->time);
        xcb_flush(conn);

        command_result_free(result);
        return;
    }

    /* There is no default behavior for button release events so we are done. */
    if (event->response_type == XCB_BUTTON_RELEASE) {
        goto done;
    }

    /* Any click in a workspace should focus that workspace. If the
     * workspace is on another output we need to do a workspace_show in
     * order for i3bar (and others) to notice the change in workspace. */
    Con *ws = con_get_workspace(con);
    Con *focused_workspace = con_get_workspace(focused);

    if (!ws) {
        ws = TAILQ_FIRST(&(output_get_content(con_get_output(con))->focus_head));
        if (!ws)
            goto done;
    }

    /* get the floating con */
    Con *floatingcon = con_inside_floating(con);
    const bool proportional = (event->state & XCB_KEY_BUT_MASK_SHIFT) == XCB_KEY_BUT_MASK_SHIFT;
    const bool in_stacked = (con->parent->layout == L_STACKED || con->parent->layout == L_TABBED);
    const bool was_focused = focused == con;
    const bool is_left_click = (event->detail == XCB_BUTTON_CLICK_LEFT);
    const bool is_right_click = (event->detail == XCB_BUTTON_CLICK_RIGHT);
    const bool is_left_or_right_click = (is_left_click || is_right_click);
    const bool is_scroll = (event->detail == XCB_BUTTON_SCROLL_UP ||
                            event->detail == XCB_BUTTON_SCROLL_DOWN ||
                            event->detail == XCB_BUTTON_SCROLL_LEFT ||
                            event->detail == XCB_BUTTON_SCROLL_RIGHT);

    /* 1: see if the user scrolled on the decoration of a stacked/tabbed con */
    if (in_stacked && dest == CLICK_DECORATION && is_scroll) {
        DLOG("Scrolling on a window decoration\n");
        /* Use the focused child of the tabbed / stacked container, not the
         * container the user scrolled on. */
        Con *current = TAILQ_FIRST(&(con->parent->focus_head));
        const position_t direction =
            (event->detail == XCB_BUTTON_SCROLL_UP || event->detail == XCB_BUTTON_SCROLL_LEFT) ? BEFORE : AFTER;
        Con *next = get_tree_next_sibling(current, direction);
        con_activate(con_descend_focused(next ? next : current));

        goto done;
    }

    /* 2: floating modifier pressed, initiate a drag */
    if (!config.disable_tiling_drag && mod_pressed && event->detail == XCB_BUTTON_INDEX_1 && !floatingcon) {
        tiling_drag(con, event);
        goto done;
    }

    /* 3: focus this con or one of its children. */
    Con *con_to_focus = con;
    if (in_stacked && dest == CLICK_DECORATION) {
        /* If the container is a tab/stacked container and the click happened
         * on a tab, switch to the tab. If the tab contents were already
         * focused, focus the tab container itself. If the tab container was
         * already focused, cycle back to focusing the tab contents. */
        if (was_focused || !con_has_parent(focused, con)) {
            while (!TAILQ_EMPTY(&(con_to_focus->focus_head))) {
                con_to_focus = TAILQ_FIRST(&(con_to_focus->focus_head));
            }
        }
    }
    if (ws != focused_workspace) {
        workspace_show(ws);
    }
    con_activate(con_to_focus);

    /* 4: For floating containers, we also want to raise them on click.
     * We will skip handling events on floating cons in fullscreen mode */
    Con *fs = con_get_fullscreen_covering_ws(ws);
    if (floatingcon != NULL && fs != con) {
        /* 5: floating_modifier plus left mouse button drags */
        if (mod_pressed && is_left_click) {
            floating_drag_window(floatingcon, event, false);
            return;
        }

        /*  6: resize (floating) if this was a (left or right) click on the
         * left/right/bottom border, or a right click on the decoration.
         * also try resizing (tiling) if possible */
        if (mod_pressed && is_right_click) {
            DLOG("floating resize due to floatingmodifier\n");
            floating_resize_window(floatingcon, proportional, event);
            return;
        }

        if ((dest == CLICK_BORDER || dest == CLICK_DECORATION) &&
            is_left_or_right_click) {
            /* try tiling resize, but continue if it doesn’t work */
            DLOG("tiling resize with fallback\n");
            if (tiling_resize(con, event, dest, dest == CLICK_DECORATION && !was_focused))
                goto done;
        }

        if (dest == CLICK_DECORATION && is_right_click) {
            DLOG("floating resize due to decoration right click\n");
            floating_resize_window(floatingcon, proportional, event);
            return;
        }

        if (dest == CLICK_BORDER && is_left_or_right_click) {
            DLOG("floating resize due to border click\n");
            floating_resize_window(floatingcon, proportional, event);
            return;
        }

        /* 7: dragging, if this was a click on a decoration (which did not lead
         * to a resize) */
        if (dest == CLICK_DECORATION && is_left_click) {
            floating_drag_window(floatingcon, event, !was_focused);
            return;
        }

        goto done;
    }

    /* 8: floating modifier pressed, initiate a drag */
    if (!config.disable_tiling_drag && mod_pressed && event->detail == XCB_BUTTON_INDEX_1) {
        DLOG("Trying to drag (tiling)\n");
        tiling_drag(con, event);
        goto done;
    }

    /* 9: floating modifier pressed, initiate a resize */
    if (dest == CLICK_INSIDE && mod_pressed && is_right_click) {
        if (floating_mod_on_tiled_client(con, event)) {
            return;
        }
        /* Avoid propagating events to clients, since the user expects
         * $mod+click to be handled by i3. */
        xcb_allow_events(conn, XCB_ALLOW_ASYNC_POINTER, event->time);
        xcb_flush(conn);
        return;
    }
    /* 10: otherwise, check for border/decoration clicks and resize */
    if ((dest == CLICK_BORDER || dest == CLICK_DECORATION) &&
        is_left_or_right_click) {
        DLOG("Trying to resize (tiling)\n");
        tiling_resize(con, event, dest, dest == CLICK_DECORATION && !was_focused);
    }

done:
    xcb_allow_events(conn, XCB_ALLOW_REPLAY_POINTER, event->time);
    xcb_flush(conn);
    tree_render();
}

/*
 * The button press X callback. This function determines whether the floating
 * modifier is pressed and where the user clicked (decoration, border, inside
 * the window).
 *
 * Then, route_click is called on the appropriate con.
 *
 */
void handle_button_press(xcb_button_press_event_t *event) {
    Con *con;
    DLOG("Button %d (state %d) %s on window 0x%08x (child 0x%08x) at (%d, %d) (root %d, %d)\n",
         event->detail, event->state, (event->response_type == XCB_BUTTON_PRESS ? "press" : "release"),
         event->event, event->child, event->event_x, event->event_y, event->root_x,
         event->root_y);

    last_timestamp = event->time;

    const uint32_t mod = (config.floating_modifier & 0xFFFF);
    const bool mod_pressed = (mod != 0 && (event->state & mod) == mod);
    DLOG("floating_mod = %d, detail = %d\n", mod_pressed, event->detail);
    if ((con = con_by_window_id(event->event))) {
        route_click(con, event, mod_pressed, CLICK_INSIDE);
        return;
    }

    if (!(con = con_by_frame_id(event->event))) {
        /* Run bindings on the root window as well, see #2097. We only run it
         * if --whole-window was set as that's the equivalent for a normal
         * window. */
        if (event->event == root) {
            Binding *bind = get_binding_from_xcb_event((xcb_generic_event_t *)event);
            if (bind != NULL && bind->whole_window) {
                CommandResult *result = run_binding(bind, NULL);
                command_result_free(result);
            }
        }

        /* If the root window is clicked, find the relevant output from the
         * click coordinates and focus the output's active workspace. */
        if (event->event == root && event->response_type == XCB_BUTTON_PRESS) {
            Con *output, *ws;
            TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
                if (con_is_internal(output) ||
                    !rect_contains(output->rect, event->event_x, event->event_y))
                    continue;

                ws = TAILQ_FIRST(&(output_get_content(output)->focus_head));
                if (ws != con_get_workspace(focused)) {
                    workspace_show(ws);
                    tree_render();
                }
                return;
            }
            return;
        }

        ELOG("Clicked into unknown window?!\n");
        xcb_allow_events(conn, XCB_ALLOW_REPLAY_POINTER, event->time);
        xcb_flush(conn);
        return;
    }

    /* Check if the click was on the decoration of a child */
    Con *child;
    TAILQ_FOREACH_REVERSE (child, &(con->nodes_head), nodes_head, nodes) {
        if (!rect_contains(child->deco_rect, event->event_x, event->event_y))
            continue;

        route_click(child, event, mod_pressed, CLICK_DECORATION);
        return;
    }

    if (event->child != XCB_NONE) {
        DLOG("event->child not XCB_NONE, so this is an event which originated from a click into the application, but the application did not handle it.\n");
        route_click(con, event, mod_pressed, CLICK_INSIDE);
        return;
    }

    route_click(con, event, mod_pressed, CLICK_BORDER);
}
