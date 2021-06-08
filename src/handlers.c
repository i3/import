/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * handlers.c: Small handlers for various events (keypresses, focus changes,
 *             …).
 *
 */
#include "all.h"

#include <sys/time.h>
#include <time.h>

#include <xcb/randr.h>
#define SN_API_NOT_YET_FROZEN 1
#include <libsn/sn-monitor.h>

int randr_base = -1;
int xkb_base = -1;
int xkb_current_group;
int shape_base = -1;

/* After mapping/unmapping windows, a notify event is generated. However, we don’t want it,
   since it’d trigger an infinite loop of switching between the different windows when
   changing workspaces */
static SLIST_HEAD(ignore_head, Ignore_Event) ignore_events;

/*
 * Adds the given sequence to the list of events which are ignored.
 * If this ignore should only affect a specific response_type, pass
 * response_type, otherwise, pass -1.
 *
 * Every ignored sequence number gets garbage collected after 5 seconds.
 *
 */
void add_ignore_event(const int sequence, const int response_type) {
    struct Ignore_Event *event = smalloc(sizeof(struct Ignore_Event));

    event->sequence = sequence;
    event->response_type = response_type;
    event->added = time(NULL);

    SLIST_INSERT_HEAD(&ignore_events, event, ignore_events);
}

/*
 * Checks if the given sequence is ignored and returns true if so.
 *
 */
bool event_is_ignored(const int sequence, const int response_type) {
    struct Ignore_Event *event;
    time_t now = time(NULL);
    for (event = SLIST_FIRST(&ignore_events); event != SLIST_END(&ignore_events);) {
        if ((now - event->added) > 5) {
            struct Ignore_Event *save = event;
            event = SLIST_NEXT(event, ignore_events);
            SLIST_REMOVE(&ignore_events, save, Ignore_Event, ignore_events);
            free(save);
        } else
            event = SLIST_NEXT(event, ignore_events);
    }

    SLIST_FOREACH (event, &ignore_events, ignore_events) {
        if (event->sequence != sequence)
            continue;

        if (event->response_type != -1 &&
            event->response_type != response_type)
            continue;

        /* instead of removing a sequence number we better wait until it gets
         * garbage collected. it may generate multiple events (there are multiple
         * enter_notifies for one configure_request, for example). */
        //SLIST_REMOVE(&ignore_events, event, Ignore_Event, ignore_events);
        //free(event);
        return true;
    }

    return false;
}

typedef enum {
    RG_DEFAULT = 0,
    RG_TRANSLATE = 1,
    RG_LOAD_KEYMAP_AND_TRANSLATE = 2,
    RG_UPDATE_KEY_SYMBOLS_AND_LOAD_KEYMAP_AND_TRANSLATE = 3,
} regrab_mode_t;

static regrab_mode_t pending_regrab = RG_DEFAULT;
static ev_idle regrab_watcher;
static bool regrab_watcher_initialised = false;
extern struct ev_loop *main_loop;

static void regrab_keys_cb(EV_P_ ev_idle *w, int revents) {
    ev_idle_stop(main_loop, w);

    ungrab_all_keys(conn);
    switch (pending_regrab) {
    case RG_DEFAULT:
        break;
    case RG_UPDATE_KEY_SYMBOLS_AND_LOAD_KEYMAP_AND_TRANSLATE:
        xcb_key_symbols_free(keysyms);
        keysyms = xcb_key_symbols_alloc(conn);
        /* fall through */
    case RG_LOAD_KEYMAP_AND_TRANSLATE:
        (void)load_keymap();
        /* fall through */
    case RG_TRANSLATE:
        translate_keysyms();
        break;
    }
    grab_all_keys(conn);

    pending_regrab = RG_DEFAULT;
}

/*
 * Called when the keyboard state changed and we need to re-grab keys.
 */
static void regrab_keys(regrab_mode_t mode) {
    pending_regrab = max(pending_regrab, mode);
    if (!regrab_watcher_initialised) {
        regrab_watcher_initialised = true;
        ev_idle_init(&regrab_watcher, regrab_keys_cb);
    }
    ev_idle_start(main_loop, &regrab_watcher);
}

/*
 * Called with coordinates of an enter_notify event or motion_notify event
 * to check if the user crossed virtual screen boundaries and adjust the
 * current workspace, if so.
 *
 */
static void check_crossing_screen_boundary(uint32_t x, uint32_t y) {
    Output *output;

    /* If the user disable focus follows mouse, we have nothing to do here */
    if (config.disable_focus_follows_mouse)
        return;

    if ((output = get_output_containing(x, y)) == NULL) {
        ELOG("ERROR: No such screen\n");
        return;
    }

    if (output->con == NULL) {
        ELOG("ERROR: The screen is not recognized by i3 (no container associated)\n");
        return;
    }

    /* Focus the output on which the user moved their cursor */
    Con *old_focused = focused;
    Con *next = con_descend_focused(output_get_content(output->con));
    /* Since we are switching outputs, this *must* be a different workspace, so
     * call workspace_show() */
    workspace_show(con_get_workspace(next));
    con_focus(next);

    /* If the focus changed, we re-render to get updated decorations */
    if (old_focused != focused)
        tree_render();
}

/*
 * When the user moves the mouse pointer onto a window, this callback gets called.
 *
 */
static void handle_enter_notify(xcb_enter_notify_event_t *event) {
    Con *con;

    last_timestamp = event->time;

    DLOG("enter_notify for %08x, mode = %d, detail %d, serial %d\n",
         event->event, event->mode, event->detail, event->sequence);
    DLOG("coordinates %d, %d\n", event->event_x, event->event_y);
    if (event->mode != XCB_NOTIFY_MODE_NORMAL) {
        DLOG("This was not a normal notify, ignoring\n");
        return;
    }
    /* Some events are not interesting, because they were not generated
     * actively by the user, but by reconfiguration of windows */
    if (event_is_ignored(event->sequence, XCB_ENTER_NOTIFY)) {
        DLOG("Event ignored\n");
        return;
    }

    bool enter_child = false;
    /* Get container by frame or by child window */
    if ((con = con_by_frame_id(event->event)) == NULL) {
        con = con_by_window_id(event->event);
        enter_child = true;
    }

    /* If we cannot find the container, the user moved their cursor to the root
     * window. In this case and if they used it to a dock, we need to focus the
     * workspace on the correct output. */
    if (con == NULL || con->parent->type == CT_DOCKAREA) {
        DLOG("Getting screen at %d x %d\n", event->root_x, event->root_y);
        check_crossing_screen_boundary(event->root_x, event->root_y);
        return;
    }

    /* see if the user entered the window on a certain window decoration */
    layout_t layout = (enter_child ? con->parent->layout : con->layout);
    if (layout == L_DEFAULT) {
        Con *child;
        TAILQ_FOREACH_REVERSE (child, &(con->nodes_head), nodes_head, nodes) {
            if (rect_contains(child->deco_rect, event->event_x, event->event_y)) {
                LOG("using child %p / %s instead!\n", child, child->name);
                con = child;
                break;
            }
        }
    }

    if (config.disable_focus_follows_mouse)
        return;

    /* if this container is already focused, there is nothing to do. */
    if (con == focused)
        return;

    /* Get the currently focused workspace to check if the focus change also
     * involves changing workspaces. If so, we need to call workspace_show() to
     * correctly update state and send the IPC event. */
    Con *ws = con_get_workspace(con);
    if (ws != con_get_workspace(focused))
        workspace_show(ws);

    focused_id = XCB_NONE;
    con_focus(con_descend_focused(con));
    tree_render();
}

/*
 * When the user moves the mouse but does not change the active window
 * (e.g. when having no windows opened but moving mouse on the root screen
 * and crossing virtual screen boundaries), this callback gets called.
 *
 */
static void handle_motion_notify(xcb_motion_notify_event_t *event) {
    last_timestamp = event->time;

    /* Skip events where the pointer was over a child window, we are only
     * interested in events on the root window. */
    if (event->child != XCB_NONE)
        return;

    Con *con;
    if ((con = con_by_frame_id(event->event)) == NULL) {
        DLOG("MotionNotify for an unknown container, checking if it crosses screen boundaries.\n");
        check_crossing_screen_boundary(event->root_x, event->root_y);
        return;
    }

    if (config.disable_focus_follows_mouse)
        return;

    if (con->layout != L_DEFAULT && con->layout != L_SPLITV && con->layout != L_SPLITH)
        return;

    /* see over which rect the user is */
    Con *current;
    TAILQ_FOREACH_REVERSE (current, &(con->nodes_head), nodes_head, nodes) {
        if (!rect_contains(current->deco_rect, event->event_x, event->event_y))
            continue;

        /* We found the rect, let’s see if this window is focused */
        if (TAILQ_FIRST(&(con->focus_head)) == current)
            return;

        con_focus(current);
        x_push_changes(croot);
        return;
    }
}

/*
 * Called when the keyboard mapping changes (for example by using Xmodmap),
 * we need to update our key bindings then (re-translate symbols).
 *
 */
static void handle_mapping_notify(xcb_mapping_notify_event_t *event) {
    if (event->request != XCB_MAPPING_KEYBOARD &&
        event->request != XCB_MAPPING_MODIFIER)
        return;

    DLOG("Received mapping_notify for keyboard or modifier mapping, re-grabbing keys\n");
    xcb_refresh_keyboard_mapping(keysyms, event);

    xcb_numlock_mask = aio_get_mod_mask_for(XCB_NUM_LOCK, keysyms);

    regrab_keys(RG_TRANSLATE);
}

/*
 * A new window appeared on the screen (=was mapped), so let’s manage it.
 *
 */
static void handle_map_request(xcb_map_request_event_t *event) {
    xcb_get_window_attributes_cookie_t cookie;

    cookie = xcb_get_window_attributes_unchecked(conn, event->window);

    DLOG("window = 0x%08x, serial is %d.\n", event->window, event->sequence);
    add_ignore_event(event->sequence, -1);

    manage_window(event->window, cookie, false);
}

/*
 * Configure requests are received when the application wants to resize windows
 * on their own.
 *
 * We generate a synthethic configure notify event to signalize the client its
 * "new" position.
 *
 */
static void handle_configure_request(xcb_configure_request_event_t *event) {
    Con *con;

    DLOG("window 0x%08x wants to be at %dx%d with %dx%d\n",
         event->window, event->x, event->y, event->width, event->height);

    /* For unmanaged windows, we just execute the configure request. As soon as
     * it gets mapped, we will take over anyways. */
    if ((con = con_by_window_id(event->window)) == NULL) {
        DLOG("Configure request for unmanaged window, can do that.\n");

        uint32_t mask = 0;
        uint32_t values[7];
        int c = 0;
#define COPY_MASK_MEMBER(mask_member, event_member) \
    do {                                            \
        if (event->value_mask & mask_member) {      \
            mask |= mask_member;                    \
            values[c++] = event->event_member;      \
        }                                           \
    } while (0)

        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_X, x);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_Y, y);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_WIDTH, width);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_HEIGHT, height);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_BORDER_WIDTH, border_width);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_SIBLING, sibling);
        COPY_MASK_MEMBER(XCB_CONFIG_WINDOW_STACK_MODE, stack_mode);

        xcb_configure_window(conn, event->window, mask, values);
        xcb_flush(conn);

        return;
    }

    DLOG("Configure request!\n");

    Con *workspace = con_get_workspace(con);
    if (workspace && (strcmp(workspace->name, "__i3_scratch") == 0)) {
        DLOG("This is a scratchpad container, ignoring ConfigureRequest\n");
        goto out;
    }
    Con *fullscreen = con_get_fullscreen_covering_ws(workspace);

    if (fullscreen != con && con_is_floating(con) && con_is_leaf(con)) {
        /* find the height for the decorations */
        int deco_height = con->deco_rect.height;
        /* we actually need to apply the size/position changes to the *parent*
         * container */
        Rect bsr = con_border_style_rect(con);
        if (con->border_style == BS_NORMAL) {
            bsr.y += deco_height;
            bsr.height -= deco_height;
        }
        Con *floatingcon = con->parent;
        Rect newrect = floatingcon->rect;

        if (event->value_mask & XCB_CONFIG_WINDOW_X) {
            newrect.x = event->x + (-1) * bsr.x;
            DLOG("proposed x = %d, new x is %d\n", event->x, newrect.x);
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_Y) {
            newrect.y = event->y + (-1) * bsr.y;
            DLOG("proposed y = %d, new y is %d\n", event->y, newrect.y);
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            newrect.width = event->width + (-1) * bsr.width;
            newrect.width += con->border_width * 2;
            DLOG("proposed width = %d, new width is %d (x11 border %d)\n",
                 event->width, newrect.width, con->border_width);
        }
        if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            newrect.height = event->height + (-1) * bsr.height;
            newrect.height += con->border_width * 2;
            DLOG("proposed height = %d, new height is %d (x11 border %d)\n",
                 event->height, newrect.height, con->border_width);
        }

        DLOG("Container is a floating leaf node, will do that.\n");
        floating_reposition(floatingcon, newrect);
        return;
    }

    /* Dock windows can be reconfigured in their height and moved to another output. */
    if (con->parent && con->parent->type == CT_DOCKAREA) {
        DLOG("Reconfiguring dock window (con = %p).\n", con);
        if (event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            DLOG("Dock client wants to change height to %d, we can do that.\n", event->height);

            con->geometry.height = event->height;
            tree_render();
        }

        if (event->value_mask & XCB_CONFIG_WINDOW_X || event->value_mask & XCB_CONFIG_WINDOW_Y) {
            int16_t x = event->value_mask & XCB_CONFIG_WINDOW_X ? event->x : (int16_t)con->geometry.x;
            int16_t y = event->value_mask & XCB_CONFIG_WINDOW_Y ? event->y : (int16_t)con->geometry.y;

            Con *current_output = con_get_output(con);
            Output *target = get_output_containing(x, y);
            if (target != NULL && current_output != target->con) {
                DLOG("Dock client is requested to be moved to output %s, moving it there.\n", output_primary_name(target));
                Match *match;
                Con *nc = con_for_window(target->con, con->window, &match);
                DLOG("Dock client will be moved to container %p.\n", nc);
                con_detach(con);
                con_attach(con, nc, false);

                tree_render();
            } else {
                DLOG("Dock client will not be moved, we only support moving it to another output.\n");
            }
        }
        goto out;
    }

    if (event->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        DLOG("window 0x%08x wants to be stacked %d\n", event->window, event->stack_mode);

        /* Emacs and IntelliJ Idea “request focus” by stacking their window
         * above all others. */
        if (event->stack_mode != XCB_STACK_MODE_ABOVE) {
            DLOG("stack_mode != XCB_STACK_MODE_ABOVE, ignoring ConfigureRequest\n");
            goto out;
        }

        if (fullscreen || !con_is_leaf(con)) {
            DLOG("fullscreen or not a leaf, ignoring ConfigureRequest\n");
            goto out;
        }

        if (workspace == NULL) {
            DLOG("Window is not being managed, ignoring ConfigureRequest\n");
            goto out;
        }

        if (config.focus_on_window_activation == FOWA_FOCUS || (config.focus_on_window_activation == FOWA_SMART && workspace_is_visible(workspace))) {
            DLOG("Focusing con = %p\n", con);
            workspace_show(workspace);
            con_activate_unblock(con);
            tree_render();
        } else if (config.focus_on_window_activation == FOWA_URGENT || (config.focus_on_window_activation == FOWA_SMART && !workspace_is_visible(workspace))) {
            DLOG("Marking con = %p urgent\n", con);
            con_set_urgency(con, true);
            tree_render();
        } else {
            DLOG("Ignoring request for con = %p.\n", con);
        }
    }

out:
    fake_absolute_configure_notify(con);
}

/*
 * Gets triggered upon a RandR screen change event, that is when the user
 * changes the screen configuration in any way (mode, position, …)
 *
 */
static void handle_screen_change(xcb_generic_event_t *e) {
    DLOG("RandR screen change\n");

    /* The geometry of the root window is used for “fullscreen global” and
     * changes when new outputs are added. */
    xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, root);
    xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(conn, cookie, NULL);
    if (reply == NULL) {
        ELOG("Could not get geometry of the root window, exiting\n");
        exit(EXIT_FAILURE);
    }
    DLOG("root geometry reply: (%d, %d) %d x %d\n", reply->x, reply->y, reply->width, reply->height);

    croot->rect.width = reply->width;
    croot->rect.height = reply->height;

    randr_query_outputs();

    scratchpad_fix_resolution();

    ipc_send_event("output", I3_IPC_EVENT_OUTPUT, "{\"change\":\"unspecified\"}");
}

/*
 * Our window decorations were unmapped. That means, the window will be killed
 * now, so we better clean up before.
 *
 */
static void handle_unmap_notify_event(xcb_unmap_notify_event_t *event) {
    DLOG("UnmapNotify for 0x%08x (received from 0x%08x), serial %d\n", event->window, event->event, event->sequence);
    xcb_get_input_focus_cookie_t cookie;
    Con *con = con_by_window_id(event->window);
    if (con == NULL) {
        /* This could also be an UnmapNotify for the frame. We need to
         * decrement the ignore_unmap counter. */
        con = con_by_frame_id(event->window);
        if (con == NULL) {
            LOG("Not a managed window, ignoring UnmapNotify event\n");
            return;
        }

        if (con->ignore_unmap > 0)
            con->ignore_unmap--;
        /* See the end of this function. */
        cookie = xcb_get_input_focus(conn);
        DLOG("ignore_unmap = %d for frame of container %p\n", con->ignore_unmap, con);
        goto ignore_end;
    }

    /* See the end of this function. */
    cookie = xcb_get_input_focus(conn);

    if (con->ignore_unmap > 0) {
        DLOG("ignore_unmap = %d, dec\n", con->ignore_unmap);
        con->ignore_unmap--;
        goto ignore_end;
    }

    /* Since we close the container, we need to unset _NET_WM_DESKTOP and
     * _NET_WM_STATE according to the spec. */
    xcb_delete_property(conn, event->window, A__NET_WM_DESKTOP);
    xcb_delete_property(conn, event->window, A__NET_WM_STATE);

    tree_close_internal(con, DONT_KILL_WINDOW, false);
    tree_render();

ignore_end:
    /* If the client (as opposed to i3) destroyed or unmapped a window, an
     * EnterNotify event will follow (indistinguishable from an EnterNotify
     * event caused by moving your mouse), causing i3 to set focus to whichever
     * window is now visible.
     *
     * In a complex stacked or tabbed layout (take two v-split containers in a
     * tabbed container), when the bottom window in tab2 is closed, the bottom
     * window of tab1 is visible instead. X11 will thus send an EnterNotify
     * event for the bottom window of tab1, while the focus should be set to
     * the remaining window of tab2.
     *
     * Therefore, we ignore all EnterNotify events which have the same sequence
     * as an UnmapNotify event. */
    add_ignore_event(event->sequence, XCB_ENTER_NOTIFY);

    /* Since we just ignored the sequence of this UnmapNotify, we want to make
     * sure that following events use a different sequence. When putting xterm
     * into fullscreen and moving the pointer to a different window, without
     * using GetInputFocus, subsequent (legitimate) EnterNotify events arrived
     * with the same sequence and thus were ignored (see ticket #609). */
    free(xcb_get_input_focus_reply(conn, cookie, NULL));
}

/*
 * A destroy notify event is sent when the window is not unmapped, but
 * immediately destroyed (for example when starting a window and immediately
 * killing the program which started it).
 *
 * We just pass on the event to the unmap notify handler (by copying the
 * important fields in the event data structure).
 *
 */
static void handle_destroy_notify_event(xcb_destroy_notify_event_t *event) {
    DLOG("destroy notify for 0x%08x, 0x%08x\n", event->event, event->window);

    xcb_unmap_notify_event_t unmap;
    unmap.sequence = event->sequence;
    unmap.event = event->event;
    unmap.window = event->window;

    handle_unmap_notify_event(&unmap);
}

static bool window_name_changed(i3Window *window, char *old_name) {
    if ((old_name == NULL) && (window->name == NULL))
        return false;

    /* Either the old or the new one is NULL, but not both. */
    if ((old_name == NULL) ^ (window->name == NULL))
        return true;

    return (strcmp(old_name, i3string_as_utf8(window->name)) != 0);
}

/*
 * Called when a window changes its title
 *
 */
static bool handle_windowname_change(Con *con, xcb_get_property_reply_t *prop) {
    char *old_name = (con->window->name != NULL ? sstrdup(i3string_as_utf8(con->window->name)) : NULL);

    window_update_name(con->window, prop);

    con = remanage_window(con);

    x_push_changes(croot);

    if (window_name_changed(con->window, old_name))
        ipc_send_window_event("title", con);

    FREE(old_name);

    return true;
}

/*
 * Handles legacy window name updates (WM_NAME), see also src/window.c,
 * window_update_name_legacy().
 *
 */
static bool handle_windowname_change_legacy(Con *con, xcb_get_property_reply_t *prop) {
    char *old_name = (con->window->name != NULL ? sstrdup(i3string_as_utf8(con->window->name)) : NULL);

    window_update_name_legacy(con->window, prop);

    con = remanage_window(con);

    x_push_changes(croot);

    if (window_name_changed(con->window, old_name))
        ipc_send_window_event("title", con);

    FREE(old_name);

    return true;
}

/*
 * Called when a window changes its WM_WINDOW_ROLE.
 *
 */
static bool handle_windowrole_change(Con *con, xcb_get_property_reply_t *prop) {
    window_update_role(con->window, prop);

    con = remanage_window(con);

    return true;
}

/*
 * Expose event means we should redraw our windows (= title bar)
 *
 */
static void handle_expose_event(xcb_expose_event_t *event) {
    Con *parent;

    DLOG("window = %08x\n", event->window);

    if ((parent = con_by_frame_id(event->window)) == NULL) {
        LOG("expose event for unknown window, ignoring\n");
        return;
    }

    /* Since we render to our surface on every change anyways, expose events
     * only tell us that the X server lost (parts of) the window contents. */
    draw_util_copy_surface(&(parent->frame_buffer), &(parent->frame),
                           0, 0, 0, 0, parent->rect.width, parent->rect.height);
    xcb_flush(conn);
}

#define _NET_WM_MOVERESIZE_SIZE_TOPLEFT 0
#define _NET_WM_MOVERESIZE_SIZE_TOP 1
#define _NET_WM_MOVERESIZE_SIZE_TOPRIGHT 2
#define _NET_WM_MOVERESIZE_SIZE_RIGHT 3
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT 4
#define _NET_WM_MOVERESIZE_SIZE_BOTTOM 5
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT 6
#define _NET_WM_MOVERESIZE_SIZE_LEFT 7
#define _NET_WM_MOVERESIZE_MOVE 8           /* movement only */
#define _NET_WM_MOVERESIZE_SIZE_KEYBOARD 9  /* size via keyboard */
#define _NET_WM_MOVERESIZE_MOVE_KEYBOARD 10 /* move via keyboard */
#define _NET_WM_MOVERESIZE_CANCEL 11        /* cancel operation */

#define _NET_MOVERESIZE_WINDOW_X (1 << 8)
#define _NET_MOVERESIZE_WINDOW_Y (1 << 9)
#define _NET_MOVERESIZE_WINDOW_WIDTH (1 << 10)
#define _NET_MOVERESIZE_WINDOW_HEIGHT (1 << 11)

/*
 * Handle client messages (EWMH)
 *
 */
static void handle_client_message(xcb_client_message_event_t *event) {
    /* If this is a startup notification ClientMessage, the library will handle
     * it and call our monitor_event() callback. */
    if (sn_xcb_display_process_event(sndisplay, (xcb_generic_event_t *)event))
        return;

    LOG("ClientMessage for window 0x%08x\n", event->window);
    if (event->type == A__NET_WM_STATE) {
        if (event->format != 32 ||
            (event->data.data32[1] != A__NET_WM_STATE_FULLSCREEN &&
             event->data.data32[1] != A__NET_WM_STATE_DEMANDS_ATTENTION &&
             event->data.data32[1] != A__NET_WM_STATE_STICKY)) {
            DLOG("Unknown atom in clientmessage of type %d\n", event->data.data32[1]);
            return;
        }

        Con *con = con_by_window_id(event->window);
        if (con == NULL) {
            DLOG("Could not get window for client message\n");
            return;
        }

        if (event->data.data32[1] == A__NET_WM_STATE_FULLSCREEN) {
            /* Check if the fullscreen state should be toggled */
            if ((con->fullscreen_mode != CF_NONE &&
                 (event->data.data32[0] == _NET_WM_STATE_REMOVE ||
                  event->data.data32[0] == _NET_WM_STATE_TOGGLE)) ||
                (con->fullscreen_mode == CF_NONE &&
                 (event->data.data32[0] == _NET_WM_STATE_ADD ||
                  event->data.data32[0] == _NET_WM_STATE_TOGGLE))) {
                DLOG("toggling fullscreen\n");
                con_toggle_fullscreen(con, CF_OUTPUT);
            }
        } else if (event->data.data32[1] == A__NET_WM_STATE_DEMANDS_ATTENTION) {
            /* Check if the urgent flag must be set or not */
            if (event->data.data32[0] == _NET_WM_STATE_ADD)
                con_set_urgency(con, true);
            else if (event->data.data32[0] == _NET_WM_STATE_REMOVE)
                con_set_urgency(con, false);
            else if (event->data.data32[0] == _NET_WM_STATE_TOGGLE)
                con_set_urgency(con, !con->urgent);
        } else if (event->data.data32[1] == A__NET_WM_STATE_STICKY) {
            DLOG("Received a client message to modify _NET_WM_STATE_STICKY.\n");
            if (event->data.data32[0] == _NET_WM_STATE_ADD)
                con->sticky = true;
            else if (event->data.data32[0] == _NET_WM_STATE_REMOVE)
                con->sticky = false;
            else if (event->data.data32[0] == _NET_WM_STATE_TOGGLE)
                con->sticky = !con->sticky;

            DLOG("New sticky status for con = %p is %i.\n", con, con->sticky);
            ewmh_update_sticky(con->window->id, con->sticky);
            output_push_sticky_windows(focused);
            ewmh_update_wm_desktop();
        }

        tree_render();
    } else if (event->type == A__NET_ACTIVE_WINDOW) {
        if (event->format != 32)
            return;

        DLOG("_NET_ACTIVE_WINDOW: Window 0x%08x should be activated\n", event->window);

        Con *con = con_by_window_id(event->window);
        if (con == NULL) {
            DLOG("Could not get window for client message\n");
            return;
        }

        Con *ws = con_get_workspace(con);
        if (ws == NULL) {
            DLOG("Window is not being managed, ignoring _NET_ACTIVE_WINDOW\n");
            return;
        }

        if (con_is_internal(ws) && ws != workspace_get("__i3_scratch")) {
            DLOG("Workspace is internal but not scratchpad, ignoring _NET_ACTIVE_WINDOW\n");
            return;
        }

        /* data32[0] indicates the source of the request (application or pager) */
        if (event->data.data32[0] == 2) {
            /* Always focus the con if it is from a pager, because this is most
             * likely from some user action */
            DLOG("This request came from a pager. Focusing con = %p\n", con);

            if (con_is_internal(ws)) {
                scratchpad_show(con);
            } else {
                workspace_show(ws);
                /* Re-set focus, even if unchanged from i3’s perspective. */
                focused_id = XCB_NONE;
                con_activate_unblock(con);
            }
        } else {
            /* Request is from an application. */
            if (con_is_internal(ws)) {
                DLOG("Ignoring request to make con = %p active because it's on an internal workspace.\n", con);
                return;
            }

            if (config.focus_on_window_activation == FOWA_FOCUS || (config.focus_on_window_activation == FOWA_SMART && workspace_is_visible(ws))) {
                DLOG("Focusing con = %p\n", con);
                con_activate_unblock(con);
            } else if (config.focus_on_window_activation == FOWA_URGENT || (config.focus_on_window_activation == FOWA_SMART && !workspace_is_visible(ws))) {
                DLOG("Marking con = %p urgent\n", con);
                con_set_urgency(con, true);
            } else
                DLOG("Ignoring request for con = %p.\n", con);
        }

        tree_render();
    } else if (event->type == A_I3_SYNC) {
        xcb_window_t window = event->data.data32[0];
        uint32_t rnd = event->data.data32[1];
        sync_respond(window, rnd);
    } else if (event->type == A__NET_REQUEST_FRAME_EXTENTS) {
        /*
         * A client can request an estimate for the frame size which the window
         * manager will put around it before actually mapping its window. Java
         * does this (as of openjdk-7).
         *
         * Note that the calculation below is not entirely accurate — once you
         * set a different border type, it’s off. We _could_ request all the
         * window properties (which have to be set up at this point according
         * to EWMH), but that seems rather elaborate. The standard explicitly
         * says the application must cope with an estimate that is not entirely
         * accurate.
         */
        DLOG("_NET_REQUEST_FRAME_EXTENTS for window 0x%08x\n", event->window);

        /* The reply data: approximate frame size */
        Rect r = {
            config.default_border_width, /* left */
            config.default_border_width, /* right */
            render_deco_height(),        /* top */
            config.default_border_width  /* bottom */
        };
        xcb_change_property(
            conn,
            XCB_PROP_MODE_REPLACE,
            event->window,
            A__NET_FRAME_EXTENTS,
            XCB_ATOM_CARDINAL, 32, 4,
            &r);
        xcb_flush(conn);
    } else if (event->type == A_WM_CHANGE_STATE) {
        /* http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.4 */
        if (event->data.data32[0] == XCB_ICCCM_WM_STATE_ICONIC) {
            /* For compatiblity reasons, Wine will request iconic state and cannot ensure that the WM has agreed on it;
             * immediately revert to normal to avoid being stuck in a paused state. */
            DLOG("Client has requested iconic state, rejecting. (window = %d)\n", event->window);
            long data[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, event->window,
                                A_WM_STATE, A_WM_STATE, 32, 2, data);
        } else {
            DLOG("Not handling WM_CHANGE_STATE request. (window = %d, state = %d)\n", event->window, event->data.data32[0]);
        }
    } else if (event->type == A__NET_CURRENT_DESKTOP) {
        /* This request is used by pagers and bars to change the current
         * desktop likely as a result of some user action. We interpret this as
         * a request to focus the given workspace. See
         * https://standards.freedesktop.org/wm-spec/latest/ar01s03.html#idm140251368135008
         * */
        DLOG("Request to change current desktop to index %d\n", event->data.data32[0]);
        Con *ws = ewmh_get_workspace_by_index(event->data.data32[0]);
        if (ws == NULL) {
            ELOG("Could not determine workspace for this index, ignoring request.\n");
            return;
        }

        DLOG("Handling request to focus workspace %s\n", ws->name);
        workspace_show(ws);
        tree_render();
    } else if (event->type == A__NET_WM_DESKTOP) {
        uint32_t index = event->data.data32[0];
        DLOG("Request to move window %d to EWMH desktop index %d\n", event->window, index);

        Con *con = con_by_window_id(event->window);
        if (con == NULL) {
            DLOG("Couldn't find con for window %d, ignoring the request.\n", event->window);
            return;
        }

        if (index == NET_WM_DESKTOP_ALL) {
            /* The window is requesting to be visible on all workspaces, so
             * let's float it and make it sticky. */
            DLOG("The window was requested to be visible on all workspaces, making it sticky and floating.\n");

            if (floating_enable(con, false)) {
                con->floating = FLOATING_AUTO_ON;

                con->sticky = true;
                ewmh_update_sticky(con->window->id, true);
                output_push_sticky_windows(focused);
            }
        } else {
            Con *ws = ewmh_get_workspace_by_index(index);
            if (ws == NULL) {
                ELOG("Could not determine workspace for this index, ignoring request.\n");
                return;
            }

            con_move_to_workspace(con, ws, true, false, false);
        }

        tree_render();
        ewmh_update_wm_desktop();
    } else if (event->type == A__NET_CLOSE_WINDOW) {
        /*
         * Pagers wanting to close a window MUST send a _NET_CLOSE_WINDOW
         * client message request to the root window.
         * https://standards.freedesktop.org/wm-spec/wm-spec-latest.html#idm140200472668896
         */
        Con *con = con_by_window_id(event->window);
        if (con) {
            DLOG("Handling _NET_CLOSE_WINDOW request (con = %p)\n", con);

            if (event->data.data32[0])
                last_timestamp = event->data.data32[0];

            tree_close_internal(con, KILL_WINDOW, false);
            tree_render();
        } else {
            DLOG("Couldn't find con for _NET_CLOSE_WINDOW request. (window = %d)\n", event->window);
        }
    } else if (event->type == A__NET_WM_MOVERESIZE) {
        /*
         * Client-side decorated Gtk3 windows emit this signal when being
         * dragged by their GtkHeaderBar
         */
        Con *con = con_by_window_id(event->window);
        if (!con || !con_is_floating(con)) {
            DLOG("Couldn't find con for _NET_WM_MOVERESIZE request, or con not floating (window = %d)\n", event->window);
            return;
        }
        DLOG("Handling _NET_WM_MOVERESIZE request (con = %p)\n", con);
        uint32_t direction = event->data.data32[2];
        uint32_t x_root = event->data.data32[0];
        uint32_t y_root = event->data.data32[1];
        /* construct fake xcb_button_press_event_t */
        xcb_button_press_event_t fake = {
            .root_x = x_root,
            .root_y = y_root,
            .event_x = x_root - (con->rect.x),
            .event_y = y_root - (con->rect.y)};
        switch (direction) {
            case _NET_WM_MOVERESIZE_MOVE:
                floating_drag_window(con->parent, &fake, false);
                break;
            case _NET_WM_MOVERESIZE_SIZE_TOPLEFT ... _NET_WM_MOVERESIZE_SIZE_LEFT:
                floating_resize_window(con->parent, false, &fake);
                break;
            default:
                DLOG("_NET_WM_MOVERESIZE direction %d not implemented\n", direction);
                break;
        }
    } else if (event->type == A__NET_MOVERESIZE_WINDOW) {
        DLOG("Received _NET_MOVE_RESIZE_WINDOW. Handling by faking a configure request.\n");

        void *_generated_event = scalloc(32, 1);
        xcb_configure_request_event_t *generated_event = _generated_event;

        generated_event->window = event->window;
        generated_event->response_type = XCB_CONFIGURE_REQUEST;

        generated_event->value_mask = 0;
        if (event->data.data32[0] & _NET_MOVERESIZE_WINDOW_X) {
            generated_event->value_mask |= XCB_CONFIG_WINDOW_X;
            generated_event->x = event->data.data32[1];
        }
        if (event->data.data32[0] & _NET_MOVERESIZE_WINDOW_Y) {
            generated_event->value_mask |= XCB_CONFIG_WINDOW_Y;
            generated_event->y = event->data.data32[2];
        }
        if (event->data.data32[0] & _NET_MOVERESIZE_WINDOW_WIDTH) {
            generated_event->value_mask |= XCB_CONFIG_WINDOW_WIDTH;
            generated_event->width = event->data.data32[3];
        }
        if (event->data.data32[0] & _NET_MOVERESIZE_WINDOW_HEIGHT) {
            generated_event->value_mask |= XCB_CONFIG_WINDOW_HEIGHT;
            generated_event->height = event->data.data32[4];
        }

        handle_configure_request(generated_event);
        FREE(generated_event);
    } else {
        DLOG("Skipping client message for unhandled type %d\n", event->type);
    }
}

static bool handle_window_type(Con *con, xcb_get_property_reply_t *reply) {
    window_update_type(con->window, reply);
    return true;
}

/*
 * Handles the size hints set by a window, but currently only the part necessary for displaying
 * clients proportionally inside their frames (mplayer for example)
 *
 * See ICCCM 4.1.2.3 for more details
 *
 */
static bool handle_normal_hints(Con *con, xcb_get_property_reply_t *reply) {
    bool changed = window_update_normal_hints(con->window, reply, NULL);

    if (changed) {
        Con *floating = con_inside_floating(con);
        if (floating) {
            floating_check_size(con, false);
            tree_render();
        }
    }

    FREE(reply);
    return true;
}

/*
 * Handles the WM_HINTS property for extracting the urgency state of the window.
 *
 */
static bool handle_hints(Con *con, xcb_get_property_reply_t *reply) {
    bool urgency_hint;
    window_update_hints(con->window, reply, &urgency_hint);
    con_set_urgency(con, urgency_hint);
    tree_render();
    return true;
}

/*
 * Handles the transient for hints set by a window, signalizing that this window is a popup window
 * for some other window.
 *
 * See ICCCM 4.1.2.6 for more details
 *
 */
static bool handle_transient_for(Con *con, xcb_get_property_reply_t *prop) {
    window_update_transient_for(con->window, prop);
    return true;
}

/*
 * Handles changes of the WM_CLIENT_LEADER atom which specifies if this is a
 * toolwindow (or similar) and to which window it belongs (logical parent).
 *
 */
static bool handle_clientleader_change(Con *con, xcb_get_property_reply_t *prop) {
    window_update_leader(con->window, prop);
    return true;
}

/*
 * Handles FocusIn events which are generated by clients (i3’s focus changes
 * don’t generate FocusIn events due to a different EventMask) and updates the
 * decorations accordingly.
 *
 */
static void handle_focus_in(xcb_focus_in_event_t *event) {
    DLOG("focus change in, for window 0x%08x\n", event->event);

    if (event->event == root) {
        DLOG("Received focus in for root window, refocusing the focused window.\n");
        con_focus(focused);
        focused_id = XCB_NONE;
        x_push_changes(croot);
    }

    Con *con;
    if ((con = con_by_window_id(event->event)) == NULL || con->window == NULL)
        return;
    DLOG("That is con %p / %s\n", con, con->name);

    if (event->mode == XCB_NOTIFY_MODE_GRAB ||
        event->mode == XCB_NOTIFY_MODE_UNGRAB) {
        DLOG("FocusIn event for grab/ungrab, ignoring\n");
        return;
    }

    if (event->detail == XCB_NOTIFY_DETAIL_POINTER) {
        DLOG("notify detail is pointer, ignoring this event\n");
        return;
    }

    /* Floating windows should be refocused to ensure that they are on top of
     * other windows. */
    if (focused_id == event->event && !con_inside_floating(con)) {
        DLOG("focus matches the currently focused window, not doing anything\n");
        return;
    }

    /* Skip dock clients, they cannot get the i3 focus. */
    if (con->parent->type == CT_DOCKAREA) {
        DLOG("This is a dock client, not focusing.\n");
        return;
    }

    DLOG("focus is different / refocusing floating window: updating decorations\n");

    con_activate_unblock(con);

    /* We update focused_id because we don’t need to set focus again */
    focused_id = event->event;
    tree_render();
}

/*
 * Handles ConfigureNotify events for the root window, which are generated when
 * the monitor configuration changed.
 *
 */
static void handle_configure_notify(xcb_configure_notify_event_t *event) {
    if (event->event != root) {
        DLOG("ConfigureNotify for non-root window 0x%08x, ignoring\n", event->event);
        return;
    }
    DLOG("ConfigureNotify for root window 0x%08x\n", event->event);

    if (force_xinerama) {
        return;
    }
    randr_query_outputs();
}

/*
 * Handles the WM_CLASS property for assignments and criteria selection.
 *
 */
static bool handle_class_change(Con *con, xcb_get_property_reply_t *prop) {
    window_update_class(con->window, prop);
    con = remanage_window(con);
    return true;
}

/*
 * Handles the WM_CLIENT_MACHINE property for assignments and criteria selection.
 *
 */
static bool handle_machine_change(Con *con, xcb_get_property_reply_t *prop) {
    window_update_machine(con->window, prop);
    con = remanage_window(con);
    return true;
}

/*
 * Handles the _MOTIF_WM_HINTS property of specifing window deocration settings.
 *
 */
static bool handle_motif_hints_change(Con *con, xcb_get_property_reply_t *prop) {
    border_style_t motif_border_style;
    window_update_motif_hints(con->window, prop, &motif_border_style);

    if (motif_border_style != con->border_style && motif_border_style != BS_NORMAL) {
        DLOG("Update border style of con %p to %d\n", con, motif_border_style);
        con_set_border_style(con, motif_border_style, con->current_border_width);

        x_push_changes(croot);
    }

    return true;
}

/*
 * Handles the _NET_WM_STRUT_PARTIAL property for allocating space for dock clients.
 *
 */
static bool handle_strut_partial_change(Con *con, xcb_get_property_reply_t *prop) {
    window_update_strut_partial(con->window, prop);

    /* we only handle this change for dock clients */
    if (con->parent == NULL || con->parent->type != CT_DOCKAREA) {
        return true;
    }

    Con *search_at = croot;
    Con *output = con_get_output(con);
    if (output != NULL) {
        DLOG("Starting search at output %s\n", output->name);
        search_at = output;
    }

    /* find out the desired position of this dock window */
    if (con->window->reserved.top > 0 && con->window->reserved.bottom == 0) {
        DLOG("Top dock client\n");
        con->window->dock = W_DOCK_TOP;
    } else if (con->window->reserved.top == 0 && con->window->reserved.bottom > 0) {
        DLOG("Bottom dock client\n");
        con->window->dock = W_DOCK_BOTTOM;
    } else {
        DLOG("Ignoring invalid reserved edges (_NET_WM_STRUT_PARTIAL), using position as fallback:\n");
        if (con->geometry.y < (search_at->rect.height / 2)) {
            DLOG("geom->y = %d < rect.height / 2 = %d, it is a top dock client\n",
                 con->geometry.y, (search_at->rect.height / 2));
            con->window->dock = W_DOCK_TOP;
        } else {
            DLOG("geom->y = %d >= rect.height / 2 = %d, it is a bottom dock client\n",
                 con->geometry.y, (search_at->rect.height / 2));
            con->window->dock = W_DOCK_BOTTOM;
        }
    }

    /* find the dockarea */
    Con *dockarea = con_for_window(search_at, con->window, NULL);
    assert(dockarea != NULL);

    /* attach the dock to the dock area */
    con_detach(con);
    con_attach(con, dockarea, true);

    tree_render();

    return true;
}

/*
 * Handles the _I3_FLOATING_WINDOW property to properly run assignments for
 * floating window changes.
 *
 * This is needed to correctly run the assignments after changes in floating
 * windows which are triggered by user commands (floating enable | disable). In
 * that case, we can't call run_assignments because it will modify the parser
 * state when it needs to parse the user-specified action, breaking the parser
 * state for the original command.
 *
 */
static bool handle_i3_floating(Con *con, xcb_get_property_reply_t *prop) {
    DLOG("floating change for con %p\n", con);

    remanage_window(con);

    return true;
}

/* Returns false if the event could not be processed (e.g. the window could not
 * be found), true otherwise */
typedef bool (*cb_property_handler_t)(Con *con, xcb_get_property_reply_t *property);

struct property_handler_t {
    xcb_atom_t atom;
    uint32_t long_len;
    cb_property_handler_t cb;
};

static struct property_handler_t property_handlers[] = {
    {0, 128, handle_windowname_change},
    {0, UINT_MAX, handle_hints},
    {0, 128, handle_windowname_change_legacy},
    {0, UINT_MAX, handle_normal_hints},
    {0, UINT_MAX, handle_clientleader_change},
    {0, UINT_MAX, handle_transient_for},
    {0, 128, handle_windowrole_change},
    {0, 128, handle_class_change},
    {0, UINT_MAX, handle_strut_partial_change},
    {0, UINT_MAX, handle_window_type},
    {0, UINT_MAX, handle_i3_floating},
    {0, 128, handle_machine_change},
    {0, 5 * sizeof(uint64_t), handle_motif_hints_change}};
#define NUM_HANDLERS (sizeof(property_handlers) / sizeof(struct property_handler_t))

/*
 * Sets the appropriate atoms for the property handlers after the atoms were
 * received from X11
 *
 */
void property_handlers_init(void) {
    sn_monitor_context_new(sndisplay, conn_screen, startup_monitor_event, NULL, NULL);

    property_handlers[0].atom = A__NET_WM_NAME;
    property_handlers[1].atom = XCB_ATOM_WM_HINTS;
    property_handlers[2].atom = XCB_ATOM_WM_NAME;
    property_handlers[3].atom = XCB_ATOM_WM_NORMAL_HINTS;
    property_handlers[4].atom = A_WM_CLIENT_LEADER;
    property_handlers[5].atom = XCB_ATOM_WM_TRANSIENT_FOR;
    property_handlers[6].atom = A_WM_WINDOW_ROLE;
    property_handlers[7].atom = XCB_ATOM_WM_CLASS;
    property_handlers[8].atom = A__NET_WM_STRUT_PARTIAL;
    property_handlers[9].atom = A__NET_WM_WINDOW_TYPE;
    property_handlers[10].atom = A_I3_FLOATING_WINDOW;
    property_handlers[11].atom = XCB_ATOM_WM_CLIENT_MACHINE;
    property_handlers[12].atom = A__MOTIF_WM_HINTS;
}

static void property_notify(uint8_t state, xcb_window_t window, xcb_atom_t atom) {
    struct property_handler_t *handler = NULL;
    xcb_get_property_reply_t *propr = NULL;
    xcb_generic_error_t *err = NULL;
    Con *con;

    for (size_t c = 0; c < NUM_HANDLERS; c++) {
        if (property_handlers[c].atom != atom)
            continue;

        handler = &property_handlers[c];
        break;
    }

    if (handler == NULL) {
        //DLOG("Unhandled property notify for atom %d (0x%08x)\n", atom, atom);
        return;
    }

    if ((con = con_by_window_id(window)) == NULL || con->window == NULL) {
        DLOG("Received property for atom %d for unknown client\n", atom);
        return;
    }

    if (state != XCB_PROPERTY_DELETE) {
        xcb_get_property_cookie_t cookie = xcb_get_property(conn, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, handler->long_len);
        propr = xcb_get_property_reply(conn, cookie, &err);
        if (err != NULL) {
            DLOG("got error %d when getting property of atom %d\n", err->error_code, atom);
            FREE(err);
            return;
        }
    }

    /* the handler will free() the reply unless it returns false */
    if (!handler->cb(con, propr))
        FREE(propr);
}

/*
 * Takes an xcb_generic_event_t and calls the appropriate handler, based on the
 * event type.
 *
 */
void handle_event(int type, xcb_generic_event_t *event) {
    if (type != XCB_MOTION_NOTIFY)
        DLOG("event type %d, xkb_base %d\n", type, xkb_base);

    if (randr_base > -1 &&
        type == randr_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        handle_screen_change(event);
        return;
    }

    if (xkb_base > -1 && type == xkb_base) {
        DLOG("xkb event, need to handle it.\n");

        xcb_xkb_state_notify_event_t *state = (xcb_xkb_state_notify_event_t *)event;
        if (state->xkbType == XCB_XKB_NEW_KEYBOARD_NOTIFY) {
            DLOG("xkb new keyboard notify, sequence %d, time %d\n", state->sequence, state->time);
            xcb_key_symbols_free(keysyms);
            keysyms = xcb_key_symbols_alloc(conn);
            if (((xcb_xkb_new_keyboard_notify_event_t *)event)->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
                regrab_keys(RG_LOAD_KEYMAP_AND_TRANSLATE);
            else
                regrab_keys(RG_TRANSLATE);
        } else if (state->xkbType == XCB_XKB_MAP_NOTIFY) {
            if (event_is_ignored(event->sequence, type)) {
                DLOG("Ignoring map notify event for sequence %d.\n", state->sequence);
            } else {
                DLOG("xkb map notify, sequence %d, time %d\n", state->sequence, state->time);
                add_ignore_event(event->sequence, type);
                regrab_keys(RG_UPDATE_KEY_SYMBOLS_AND_LOAD_KEYMAP_AND_TRANSLATE);
            }
        } else if (state->xkbType == XCB_XKB_STATE_NOTIFY) {
            DLOG("xkb state group = %d\n", state->group);
            if (xkb_current_group == state->group)
                return;
            xkb_current_group = state->group;
            regrab_keys(RG_DEFAULT);
        }

        return;
    }

    if (shape_supported && type == shape_base + XCB_SHAPE_NOTIFY) {
        xcb_shape_notify_event_t *shape = (xcb_shape_notify_event_t *)event;

        DLOG("shape_notify_event for window 0x%08x, shape_kind = %d, shaped = %d\n",
             shape->affected_window, shape->shape_kind, shape->shaped);

        Con *con = con_by_window_id(shape->affected_window);
        if (con == NULL) {
            LOG("Not a managed window 0x%08x, ignoring shape_notify_event\n",
                shape->affected_window);
            return;
        }

        if (shape->shape_kind == XCB_SHAPE_SK_BOUNDING ||
            shape->shape_kind == XCB_SHAPE_SK_INPUT) {
            x_set_shape(con, shape->shape_kind, shape->shaped);
        }

        return;
    }

    switch (type) {
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
            handle_key_press((xcb_key_press_event_t *)event);
            break;

        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
            handle_button_press((xcb_button_press_event_t *)event);
            break;

        case XCB_MAP_REQUEST:
            handle_map_request((xcb_map_request_event_t *)event);
            break;

        case XCB_UNMAP_NOTIFY:
            handle_unmap_notify_event((xcb_unmap_notify_event_t *)event);
            break;

        case XCB_DESTROY_NOTIFY:
            handle_destroy_notify_event((xcb_destroy_notify_event_t *)event);
            break;

        case XCB_EXPOSE:
            if (((xcb_expose_event_t *)event)->count == 0) {
                handle_expose_event((xcb_expose_event_t *)event);
            }

            break;

        case XCB_MOTION_NOTIFY:
            handle_motion_notify((xcb_motion_notify_event_t *)event);
            break;

        /* Enter window = user moved their mouse over the window */
        case XCB_ENTER_NOTIFY:
            handle_enter_notify((xcb_enter_notify_event_t *)event);
            break;

        /* Client message are sent to the root window. The only interesting
         * client message for us is _NET_WM_STATE, we honour
         * _NET_WM_STATE_FULLSCREEN and _NET_WM_STATE_DEMANDS_ATTENTION */
        case XCB_CLIENT_MESSAGE:
            handle_client_message((xcb_client_message_event_t *)event);
            break;

        /* Configure request = window tried to change size on its own */
        case XCB_CONFIGURE_REQUEST:
            handle_configure_request((xcb_configure_request_event_t *)event);
            break;

        /* Mapping notify = keyboard mapping changed (Xmodmap), re-grab bindings */
        case XCB_MAPPING_NOTIFY:
            handle_mapping_notify((xcb_mapping_notify_event_t *)event);
            break;

        case XCB_FOCUS_IN:
            handle_focus_in((xcb_focus_in_event_t *)event);
            break;

        case XCB_PROPERTY_NOTIFY: {
            xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)event;
            last_timestamp = e->time;
            property_notify(e->state, e->window, e->atom);
            break;
        }

        case XCB_CONFIGURE_NOTIFY:
            handle_configure_notify((xcb_configure_notify_event_t *)event);
            break;

        default:
            //DLOG("Unhandled event of type %d\n", type);
            break;
    }
}
