/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * manage.c: Initially managing new windows (or existing ones on restart).
 *
 */
#include "all.h"

/*
 * Match frame and window depth. This is needed because X will refuse to reparent a
 * window whose background is ParentRelative under a window with a different depth.
 *
 */
static xcb_window_t _match_depth(i3Window *win, Con *con) {
    xcb_window_t old_frame = XCB_NONE;
    if (con->depth != win->depth) {
        old_frame = con->frame.id;
        con->depth = win->depth;
        x_con_reframe(con);
    }
    return old_frame;
}

/*
 * Remove all match criteria, the first swallowed window wins.
 *
 */
static void _remove_matches(Con *con) {
    while (!TAILQ_EMPTY(&(con->swallow_head))) {
        Match *first = TAILQ_FIRST(&(con->swallow_head));
        TAILQ_REMOVE(&(con->swallow_head), first, matches);
        match_free(first);
        free(first);
    }
}

/*
 * Go through all existing windows (if the window manager is restarted) and manage them
 *
 */
void manage_existing_windows(xcb_window_t root) {
    xcb_query_tree_reply_t *reply;
    int i, len;
    xcb_window_t *children;
    xcb_get_window_attributes_cookie_t *cookies;

    /* Get the tree of windows whose parent is the root window (= all) */
    if ((reply = xcb_query_tree_reply(conn, xcb_query_tree(conn, root), 0)) == NULL)
        return;

    len = xcb_query_tree_children_length(reply);
    cookies = smalloc(len * sizeof(*cookies));

    /* Request the window attributes for every window */
    children = xcb_query_tree_children(reply);
    for (i = 0; i < len; ++i)
        cookies[i] = xcb_get_window_attributes(conn, children[i]);

    /* Call manage_window with the attributes for every window */
    for (i = 0; i < len; ++i)
        manage_window(children[i], cookies[i], true);

    free(reply);
    free(cookies);
}

/*
 * Restores the geometry of each window by reparenting it to the root window
 * at the position of its frame.
 *
 * This is to be called *only* before exiting/restarting i3 because of evil
 * side-effects which are to be expected when continuing to run i3.
 *
 */
void restore_geometry(void) {
    DLOG("Restoring geometry\n");

    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->window) {
            DLOG("Re-adding X11 border of %d px\n", con->border_width);
            con->window_rect.width += (2 * con->border_width);
            con->window_rect.height += (2 * con->border_width);
            xcb_set_window_rect(conn, con->window->id, con->window_rect);
            DLOG("placing window %08x at %d %d\n", con->window->id, con->rect.x, con->rect.y);
            xcb_reparent_window(conn, con->window->id, root,
                                con->rect.x, con->rect.y);
        }
    }

    /* Strictly speaking, this line doesn’t really belong here, but since we
     * are syncing, let’s un-register as a window manager first */
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT});

    /* Make sure our changes reach the X server, we restart/exit now */
    xcb_aux_sync(conn);
}

/*
 * Do some sanity checks and then reparent the window.
 *
 */
void manage_window(xcb_window_t window, xcb_get_window_attributes_cookie_t cookie,
                   bool needs_to_be_mapped) {
    DLOG("window 0x%08x\n", window);

    xcb_drawable_t d = {window};
    xcb_get_geometry_cookie_t geomc;
    xcb_get_geometry_reply_t *geom;
    xcb_get_window_attributes_reply_t *attr = NULL;

    xcb_get_property_cookie_t wm_type_cookie, strut_cookie, state_cookie,
        utf8_title_cookie, title_cookie,
        class_cookie, leader_cookie, transient_cookie,
        role_cookie, startup_id_cookie, wm_hints_cookie,
        wm_normal_hints_cookie, motif_wm_hints_cookie, wm_user_time_cookie, wm_desktop_cookie,
        wm_machine_cookie;

    xcb_get_property_cookie_t wm_icon_cookie;

    geomc = xcb_get_geometry(conn, d);

    /* Check if the window is mapped (it could be not mapped when intializing and
       calling manage_window() for every window) */
    if ((attr = xcb_get_window_attributes_reply(conn, cookie, 0)) == NULL) {
        DLOG("Could not get attributes\n");
        xcb_discard_reply(conn, geomc.sequence);
        return;
    }

    if (needs_to_be_mapped && attr->map_state != XCB_MAP_STATE_VIEWABLE) {
        xcb_discard_reply(conn, geomc.sequence);
        goto out;
    }

    /* Don’t manage clients with the override_redirect flag */
    if (attr->override_redirect) {
        xcb_discard_reply(conn, geomc.sequence);
        goto out;
    }

    /* Check if the window is already managed */
    if (con_by_window_id(window) != NULL) {
        DLOG("already managed (by con %p)\n", con_by_window_id(window));
        xcb_discard_reply(conn, geomc.sequence);
        goto out;
    }

    /* Get the initial geometry (position, size, …) */
    if ((geom = xcb_get_geometry_reply(conn, geomc, 0)) == NULL) {
        DLOG("could not get geometry\n");
        goto out;
    }

    uint32_t values[1];

    /* Set a temporary event mask for the new window, consisting only of
     * PropertyChange and StructureNotify. We need to be notified of
     * PropertyChanges because the client can change its properties *after* we
     * requested them but *before* we actually reparented it and have set our
     * final event mask.
     * We need StructureNotify because the client may unmap the window before
     * we get to re-parent it.
     * If this request fails, we assume the client has already unmapped the
     * window between the MapRequest and our event mask change. */
    values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_void_cookie_t event_mask_cookie =
        xcb_change_window_attributes_checked(conn, window, XCB_CW_EVENT_MASK, values);
    if (xcb_request_check(conn, event_mask_cookie) != NULL) {
        LOG("Could not change event mask, the window probably already disappeared.\n");
        goto out;
    }

#define GET_PROPERTY(atom, len) xcb_get_property(conn, false, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, len)

    wm_type_cookie = GET_PROPERTY(A__NET_WM_WINDOW_TYPE, UINT32_MAX);
    strut_cookie = GET_PROPERTY(A__NET_WM_STRUT_PARTIAL, UINT32_MAX);
    state_cookie = GET_PROPERTY(A__NET_WM_STATE, UINT32_MAX);
    utf8_title_cookie = GET_PROPERTY(A__NET_WM_NAME, 128);
    leader_cookie = GET_PROPERTY(A_WM_CLIENT_LEADER, UINT32_MAX);
    transient_cookie = GET_PROPERTY(XCB_ATOM_WM_TRANSIENT_FOR, UINT32_MAX);
    title_cookie = GET_PROPERTY(XCB_ATOM_WM_NAME, 128);
    class_cookie = GET_PROPERTY(XCB_ATOM_WM_CLASS, 128);
    role_cookie = GET_PROPERTY(A_WM_WINDOW_ROLE, 128);
    startup_id_cookie = GET_PROPERTY(A__NET_STARTUP_ID, 512);
    wm_hints_cookie = xcb_icccm_get_wm_hints(conn, window);
    wm_normal_hints_cookie = xcb_icccm_get_wm_normal_hints(conn, window);
    motif_wm_hints_cookie = GET_PROPERTY(A__MOTIF_WM_HINTS, 5 * sizeof(uint64_t));
    wm_user_time_cookie = GET_PROPERTY(A__NET_WM_USER_TIME, UINT32_MAX);
    wm_desktop_cookie = GET_PROPERTY(A__NET_WM_DESKTOP, UINT32_MAX);
    wm_machine_cookie = GET_PROPERTY(XCB_ATOM_WM_CLIENT_MACHINE, UINT32_MAX);
    wm_icon_cookie = GET_PROPERTY(A__NET_WM_ICON, UINT32_MAX);

    i3Window *cwindow = scalloc(1, sizeof(i3Window));
    cwindow->id = window;
    cwindow->depth = get_visual_depth(attr->visual);

    int *buttons = bindings_get_buttons_to_grab();
    xcb_grab_buttons(conn, window, buttons);
    FREE(buttons);

    /* update as much information as possible so far (some replies may be NULL) */
    window_update_class(cwindow, xcb_get_property_reply(conn, class_cookie, NULL));
    window_update_name_legacy(cwindow, xcb_get_property_reply(conn, title_cookie, NULL));
    window_update_name(cwindow, xcb_get_property_reply(conn, utf8_title_cookie, NULL));
    window_update_icon(cwindow, xcb_get_property_reply(conn, wm_icon_cookie, NULL));
    window_update_leader(cwindow, xcb_get_property_reply(conn, leader_cookie, NULL));
    window_update_transient_for(cwindow, xcb_get_property_reply(conn, transient_cookie, NULL));
    window_update_strut_partial(cwindow, xcb_get_property_reply(conn, strut_cookie, NULL));
    window_update_role(cwindow, xcb_get_property_reply(conn, role_cookie, NULL));
    bool urgency_hint;
    window_update_hints(cwindow, xcb_get_property_reply(conn, wm_hints_cookie, NULL), &urgency_hint);
    border_style_t motif_border_style = BS_NORMAL;
    window_update_motif_hints(cwindow, xcb_get_property_reply(conn, motif_wm_hints_cookie, NULL), &motif_border_style);
    window_update_normal_hints(cwindow, xcb_get_property_reply(conn, wm_normal_hints_cookie, NULL), geom);
    window_update_machine(cwindow, xcb_get_property_reply(conn, wm_machine_cookie, NULL));
    xcb_get_property_reply_t *type_reply = xcb_get_property_reply(conn, wm_type_cookie, NULL);
    xcb_get_property_reply_t *state_reply = xcb_get_property_reply(conn, state_cookie, NULL);

    xcb_get_property_reply_t *startup_id_reply;
    startup_id_reply = xcb_get_property_reply(conn, startup_id_cookie, NULL);
    char *startup_ws = startup_workspace_for_window(cwindow, startup_id_reply);
    DLOG("startup workspace = %s\n", startup_ws);

    /* Get _NET_WM_DESKTOP if it was set. */
    xcb_get_property_reply_t *wm_desktop_reply;
    wm_desktop_reply = xcb_get_property_reply(conn, wm_desktop_cookie, NULL);
    cwindow->wm_desktop = NET_WM_DESKTOP_NONE;
    if (wm_desktop_reply != NULL && xcb_get_property_value_length(wm_desktop_reply) != 0) {
        uint32_t *wm_desktops = xcb_get_property_value(wm_desktop_reply);
        cwindow->wm_desktop = (int32_t)wm_desktops[0];
    }
    FREE(wm_desktop_reply);

    /* check if the window needs WM_TAKE_FOCUS */
    cwindow->needs_take_focus = window_supports_protocol(cwindow->id, A_WM_TAKE_FOCUS);

    /* read the preferred _NET_WM_WINDOW_TYPE atom */
    cwindow->window_type = xcb_get_preferred_window_type(type_reply);

    /* Where to start searching for a container that swallows the new one? */
    Con *search_at = croot;

    if (xcb_reply_contains_atom(type_reply, A__NET_WM_WINDOW_TYPE_DOCK)) {
        LOG("This window is of type dock\n");
        Output *output = get_output_containing(geom->x, geom->y);
        if (output != NULL) {
            DLOG("Starting search at output %s\n", output_primary_name(output));
            search_at = output->con;
        }

        /* find out the desired position of this dock window */
        if (cwindow->reserved.top > 0 && cwindow->reserved.bottom == 0) {
            DLOG("Top dock client\n");
            cwindow->dock = W_DOCK_TOP;
        } else if (cwindow->reserved.top == 0 && cwindow->reserved.bottom > 0) {
            DLOG("Bottom dock client\n");
            cwindow->dock = W_DOCK_BOTTOM;
        } else {
            DLOG("Ignoring invalid reserved edges (_NET_WM_STRUT_PARTIAL), using position as fallback:\n");
            if (geom->y < (int16_t)(search_at->rect.height / 2)) {
                DLOG("geom->y = %d < rect.height / 2 = %d, it is a top dock client\n",
                     geom->y, (search_at->rect.height / 2));
                cwindow->dock = W_DOCK_TOP;
            } else {
                DLOG("geom->y = %d >= rect.height / 2 = %d, it is a bottom dock client\n",
                     geom->y, (search_at->rect.height / 2));
                cwindow->dock = W_DOCK_BOTTOM;
            }
        }
    }

    DLOG("Initial geometry: (%d, %d, %d, %d)\n", geom->x, geom->y, geom->width, geom->height);

    /* See if any container swallows this new window */
    cwindow->swallowed = false;
    Match *match = NULL;
    Con *nc = con_for_window(search_at, cwindow, &match);
    const bool match_from_restart_mode = (match && match->restart_mode);
    if (nc == NULL) {
        Con *wm_desktop_ws = NULL;
        Assignment *assignment;

        /* If not, check if it is assigned to a specific workspace */
        if ((assignment = assignment_for(cwindow, A_TO_WORKSPACE)) ||
            (assignment = assignment_for(cwindow, A_TO_WORKSPACE_NUMBER))) {
            DLOG("Assignment matches (%p)\n", match);

            Con *assigned_ws = NULL;
            if (assignment->type == A_TO_WORKSPACE_NUMBER) {
                long parsed_num = ws_name_to_number(assignment->dest.workspace);

                assigned_ws = get_existing_workspace_by_num(parsed_num);
            }
            /* A_TO_WORKSPACE type assignment or fallback from A_TO_WORKSPACE_NUMBER
             * when the target workspace number does not exist yet. */
            if (!assigned_ws) {
                assigned_ws = workspace_get(assignment->dest.workspace);
            }

            nc = con_descend_tiling_focused(assigned_ws);
            DLOG("focused on ws %s: %p / %s\n", assigned_ws->name, nc, nc->name);
            if (nc->type == CT_WORKSPACE)
                nc = tree_open_con(nc, cwindow);
            else
                nc = tree_open_con(nc->parent, cwindow);

            /* set the urgency hint on the window if the workspace is not visible */
            if (!workspace_is_visible(assigned_ws))
                urgency_hint = true;
        } else if (cwindow->wm_desktop != NET_WM_DESKTOP_NONE &&
                   cwindow->wm_desktop != NET_WM_DESKTOP_ALL &&
                   (wm_desktop_ws = ewmh_get_workspace_by_index(cwindow->wm_desktop)) != NULL) {
            /* If _NET_WM_DESKTOP is set to a specific desktop, we open it
             * there. Note that we ignore the special value 0xFFFFFFFF here
             * since such a window will be made sticky anyway. */

            DLOG("Using workspace %p / %s because _NET_WM_DESKTOP = %d.\n",
                 wm_desktop_ws, wm_desktop_ws->name, cwindow->wm_desktop);

            nc = con_descend_tiling_focused(wm_desktop_ws);
            if (nc->type == CT_WORKSPACE)
                nc = tree_open_con(nc, cwindow);
            else
                nc = tree_open_con(nc->parent, cwindow);
        } else if (startup_ws) {
            /* If it was started on a specific workspace, we want to open it there. */
            DLOG("Using workspace on which this application was started (%s)\n", startup_ws);
            nc = con_descend_tiling_focused(workspace_get(startup_ws));
            DLOG("focused on ws %s: %p / %s\n", startup_ws, nc, nc->name);
            if (nc->type == CT_WORKSPACE)
                nc = tree_open_con(nc, cwindow);
            else
                nc = tree_open_con(nc->parent, cwindow);
        } else {
            /* If not, insert it at the currently focused position */
            if (focused->type == CT_CON && con_accepts_window(focused)) {
                LOG("using current container, focused = %p, focused->name = %s\n",
                    focused, focused->name);
                nc = focused;
            } else
                nc = tree_open_con(NULL, cwindow);
        }

        if ((assignment = assignment_for(cwindow, A_TO_OUTPUT))) {
            con_move_to_output_name(nc, assignment->dest.output, true);
        }
    } else {
        /* M_BELOW inserts the new window as a child of the one which was
         * matched (e.g. dock areas) */
        if (match != NULL && match->insert_where == M_BELOW) {
            nc = tree_open_con(nc, cwindow);
        }

        /* If M_BELOW is not used, the container is replaced. This happens with
         * "swallows" criteria that are used for stored layouts, in which case
         * we need to remove that criterion, because they should only be valid
         * once. */
        if (match != NULL && match->insert_where != M_BELOW) {
            DLOG("Removing match %p from container %p\n", match, nc);
            TAILQ_REMOVE(&(nc->swallow_head), match, matches);
            match_free(match);
            FREE(match);
        }

        cwindow->swallowed = true;
    }

    DLOG("new container = %p\n", nc);
    if (nc->window != NULL && nc->window != cwindow) {
        if (!restore_kill_placeholder(nc->window->id)) {
            DLOG("Uh?! Container without a placeholder, but with a window, has swallowed this to-be-managed window?!\n");
        } else {
            /* Remove remaining criteria, the first swallowed window wins. */
            _remove_matches(nc);
        }
    }
    xcb_window_t old_frame = XCB_NONE;
    if (nc->window != cwindow && nc->window != NULL) {
        window_free(nc->window);
        old_frame = _match_depth(cwindow, nc);
    }
    nc->window = cwindow;
    x_reinit(nc);

    nc->border_width = geom->border_width;

    char *name;
    sasprintf(&name, "[i3 con] container around %p", cwindow);
    x_set_name(nc, name);
    free(name);

    /* handle fullscreen containers */
    Con *ws = con_get_workspace(nc);
    Con *fs = con_get_fullscreen_covering_ws(ws);

    if (xcb_reply_contains_atom(state_reply, A__NET_WM_STATE_FULLSCREEN)) {
        /* If this window is already fullscreen (after restarting!), skip
         * toggling fullscreen, that would drop it out of fullscreen mode. */
        if (fs != nc) {
            Output *output = get_output_with_dimensions((Rect){geom->x, geom->y, geom->width, geom->height});
            /* If the requested window geometry spans the whole area
             * of an output, move the window to that output. This is
             * needed e.g. for LibreOffice Impress multi-monitor
             * presentations to work out of the box. */
            if (output != NULL)
                con_move_to_output(nc, output, false);
            con_toggle_fullscreen(nc, CF_OUTPUT);
        }
        fs = NULL;
    }

    bool set_focus = false;

    if (fs == NULL) {
        DLOG("Not in fullscreen mode, focusing\n");
        if (!cwindow->dock) {
            /* Check that the workspace is visible and on the same output as
             * the current focused container. If the window was assigned to an
             * invisible workspace, we should not steal focus. */
            Con *current_output = con_get_output(focused);
            Con *target_output = con_get_output(ws);

            if (workspace_is_visible(ws) && current_output == target_output) {
                if (!match_from_restart_mode) {
                    set_focus = true;
                } else {
                    DLOG("not focusing, matched with restart_mode == true\n");
                }
            } else {
                DLOG("workspace not visible, not focusing\n");
            }
        } else {
            DLOG("dock, not focusing\n");
        }
    } else {
        DLOG("fs = %p, ws = %p, not focusing\n", fs, ws);
        /* Insert the new container in focus stack *after* the currently
         * focused (fullscreen) con. This way, the new container will be
         * focused after we return from fullscreen mode */
        Con *first = TAILQ_FIRST(&(nc->parent->focus_head));
        if (first != nc) {
            /* We only modify the focus stack if the container is not already
             * the first one. This can happen when existing containers swallow
             * new windows, for example when restarting. */
            TAILQ_REMOVE(&(nc->parent->focus_head), nc, focused);
            TAILQ_INSERT_AFTER(&(nc->parent->focus_head), first, nc, focused);
        }
    }

    /* set floating if necessary */
    bool want_floating = false;
    if (xcb_reply_contains_atom(type_reply, A__NET_WM_WINDOW_TYPE_DIALOG) ||
        xcb_reply_contains_atom(type_reply, A__NET_WM_WINDOW_TYPE_UTILITY) ||
        xcb_reply_contains_atom(type_reply, A__NET_WM_WINDOW_TYPE_TOOLBAR) ||
        xcb_reply_contains_atom(type_reply, A__NET_WM_WINDOW_TYPE_SPLASH) ||
        xcb_reply_contains_atom(state_reply, A__NET_WM_STATE_MODAL) ||
        (cwindow->max_width > 0 && cwindow->max_height > 0 &&
         cwindow->min_height == cwindow->max_height &&
         cwindow->min_width == cwindow->max_width)) {
        LOG("This window is a dialog window, setting floating\n");
        want_floating = true;
    }

    if (xcb_reply_contains_atom(state_reply, A__NET_WM_STATE_STICKY))
        nc->sticky = true;

    /* We ignore the hint for an internal workspace because windows in the
     * scratchpad also have this value, but upon restarting i3 we don't want
     * them to become sticky windows. */
    if (cwindow->wm_desktop == NET_WM_DESKTOP_ALL && (ws == NULL || !con_is_internal(ws))) {
        DLOG("This window has _NET_WM_DESKTOP = 0xFFFFFFFF. Will float it and make it sticky.\n");
        nc->sticky = true;
        want_floating = true;
    }

    FREE(state_reply);
    FREE(type_reply);

    if (cwindow->transient_for != XCB_NONE ||
        (cwindow->leader != XCB_NONE &&
         cwindow->leader != cwindow->id &&
         con_by_window_id(cwindow->leader) != NULL)) {
        LOG("This window is transient for another window, setting floating\n");
        want_floating = true;

        if (config.popup_during_fullscreen == PDF_LEAVE_FULLSCREEN &&
            fs != NULL) {
            LOG("There is a fullscreen window, leaving fullscreen mode\n");
            con_toggle_fullscreen(fs, CF_OUTPUT);
        } else if (config.popup_during_fullscreen == PDF_SMART &&
                   fs != NULL &&
                   fs->window != NULL) {
            i3Window *transient_win = cwindow;
            while (transient_win != NULL &&
                   transient_win->transient_for != XCB_NONE) {
                if (transient_win->transient_for == fs->window->id) {
                    LOG("This floating window belongs to the fullscreen window (popup_during_fullscreen == smart)\n");
                    set_focus = true;
                    break;
                }
                Con *next_transient = con_by_window_id(transient_win->transient_for);
                if (next_transient == NULL)
                    break;
                /* Some clients (e.g. x11-ssh-askpass) actually set
                 * WM_TRANSIENT_FOR to their own window id, so break instead of
                 * looping endlessly. */
                if (transient_win == next_transient->window)
                    break;
                transient_win = next_transient->window;
            }
        }
    }

    /* dock clients cannot be floating, that makes no sense */
    if (cwindow->dock)
        want_floating = false;

    /* Store the requested geometry. The width/height gets raised to at least
     * 75x50 when entering floating mode, which is the minimum size for a
     * window to be useful (smaller windows are usually overlays/toolbars/…
     * which are not managed by the wm anyways). We store the original geometry
     * here because it’s used for dock clients. */
    if (nc->geometry.width == 0)
        nc->geometry = (Rect){geom->x, geom->y, geom->width, geom->height};

    if (motif_border_style != BS_NORMAL) {
        DLOG("MOTIF_WM_HINTS specifies decorations (border_style = %d)\n", motif_border_style);
        if (want_floating) {
            con_set_border_style(nc, motif_border_style, config.default_floating_border_width);
        } else {
            con_set_border_style(nc, motif_border_style, config.default_border_width);
        }
    }

    if (want_floating) {
        DLOG("geometry = %d x %d\n", nc->geometry.width, nc->geometry.height);
        /* automatically set the border to the default value if a motif border
         * was not specified */
        bool automatic_border = (motif_border_style == BS_NORMAL);

        if (floating_enable(nc, automatic_border)) {
            nc->floating = FLOATING_AUTO_ON;
        }
    }

    /* explicitly set the border width to the default */
    if (nc->current_border_width == -1) {
        nc->current_border_width = (want_floating ? config.default_floating_border_width : config.default_border_width);
    }

    /* to avoid getting an UnmapNotify event due to reparenting, we temporarily
     * declare no interest in any state change event of this window */
    values[0] = XCB_NONE;
    xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK, values);

    xcb_void_cookie_t rcookie = xcb_reparent_window_checked(conn, window, nc->frame.id, 0, 0);
    if (xcb_request_check(conn, rcookie) != NULL) {
        LOG("Could not reparent the window, aborting\n");
        goto geom_out;
    }

    values[0] = CHILD_EVENT_MASK & ~XCB_EVENT_MASK_ENTER_WINDOW;
    xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK, values);
    xcb_flush(conn);

    /* Put the client inside the save set. Upon termination (whether killed or
     * normal exit does not matter) of the window manager, these clients will
     * be correctly reparented to their most closest living ancestor (=
     * cleanup) */
    xcb_change_save_set(conn, XCB_SET_MODE_INSERT, window);

    if (shape_supported) {
        /* Receive ShapeNotify events whenever the client altered its window
         * shape. */
        xcb_shape_select_input(conn, window, true);

        /* Check if the window is shaped. Sadly, we can check only for the
         * bounding shape, not for the input shape. */
        xcb_shape_query_extents_cookie_t cookie =
            xcb_shape_query_extents(conn, window);
        xcb_shape_query_extents_reply_t *reply =
            xcb_shape_query_extents_reply(conn, cookie, NULL);
        if (reply != NULL && reply->bounding_shaped) {
            cwindow->shaped = true;
        }
        FREE(reply);
    }

    /* Check if any assignments match */
    run_assignments(cwindow);

    /* 'ws' may be invalid because of the assignments, e.g. when the user uses
     * "move window to workspace 1", but had it assigned to workspace 2. */
    ws = con_get_workspace(nc);

    /* If this window was put onto an invisible workspace (via assignments), we
     * render this workspace. It wouldn’t be rendered in our normal code path
     * because only the visible workspaces get rendered.
     *
     * By rendering the workspace, we assign proper coordinates (read: not
     * width=0, height=0) to the window, which is important for windows who
     * actually use them to position their GUI elements, e.g. rhythmbox. */
    if (ws && !workspace_is_visible(ws)) {
        /* This is a bit hackish: we need to copy the content container’s rect
         * to the workspace, because calling render_con() on the content
         * container would also take the shortcut and not render the invisible
         * workspace at all. However, just calling render_con() on the
         * workspace isn’t enough either — it needs the rect. */
        ws->rect = ws->parent->rect;
        render_con(ws);
        /* Disable setting focus, otherwise we’d move focus to an invisible
         * workspace, which we generally prevent (e.g. in
         * con_move_to_workspace). */
        set_focus = false;
    }
    render_con(croot);

    cwindow->managed_since = time(NULL);

    /* Send an event about window creation */
    ipc_send_window_event("new", nc);

    if (set_focus && assignment_for(cwindow, A_NO_FOCUS) != NULL) {
        /* The first window on a workspace should always be focused. We have to
         * compare with == 1 because the container has already been inserted at
         * this point. */
        if (con_num_windows(ws) == 1) {
            DLOG("This is the first window on this workspace, ignoring no_focus.\n");
        } else {
            DLOG("no_focus was set for con = %p, not setting focus.\n", nc);
            set_focus = false;
        }
    }

    if (set_focus) {
        DLOG("Checking con = %p for _NET_WM_USER_TIME.\n", nc);

        uint32_t *wm_user_time;
        xcb_get_property_reply_t *wm_user_time_reply = xcb_get_property_reply(conn, wm_user_time_cookie, NULL);
        if (wm_user_time_reply != NULL && xcb_get_property_value_length(wm_user_time_reply) != 0 &&
            (wm_user_time = xcb_get_property_value(wm_user_time_reply)) &&
            wm_user_time[0] == 0) {
            DLOG("_NET_WM_USER_TIME set to 0, not focusing con = %p.\n", nc);
            set_focus = false;
        }

        FREE(wm_user_time_reply);
    } else {
        xcb_discard_reply(conn, wm_user_time_cookie.sequence);
    }

    if (set_focus) {
        /* Even if the client doesn't want focus, we still need to focus the
         * container to not break focus workflows. Our handling towards X will
         * take care of not setting the input focus. However, one exception to
         * this are clients using the globally active input model which we
         * don't want to focus at all. */
        if (nc->window->doesnt_accept_focus && !nc->window->needs_take_focus) {
            set_focus = false;
        }
    }

    /* Defer setting focus after the 'new' event has been sent to ensure the
     * proper window event sequence. */
    if (set_focus && nc->mapped) {
        DLOG("Now setting focus.\n");
        con_activate(nc);
    }

    tree_render();

    /* Destroy the old frame if we had to reframe the container. This needs to be done
     * after rendering in order to prevent the background from flickering in its place. */
    if (old_frame != XCB_NONE) {
        xcb_destroy_window(conn, old_frame);
    }

    /* Windows might get managed with the urgency hint already set (Pidgin is
     * known to do that), so check for that and handle the hint accordingly.
     * This code needs to be in this part of manage_window() because the window
     * needs to be on the final workspace first. */
    con_set_urgency(nc, urgency_hint);

    /* Update _NET_WM_DESKTOP. We invalidate the cached value first to force an update. */
    cwindow->wm_desktop = NET_WM_DESKTOP_NONE;
    ewmh_update_wm_desktop();

    /* If a sticky window was mapped onto another workspace, make sure to pop it to the front. */
    output_push_sticky_windows(focused);

geom_out:
    free(geom);
out:
    free(attr);
}

/*
 * Remanages a window: performs a swallow check and runs assignments.
 * Returns con for the window regardless if it updated.
 *
 */
Con *remanage_window(Con *con) {
    /* Make sure this windows hasn't already been swallowed. */
    if (con->window->swallowed) {
        run_assignments(con->window);
        return con;
    }
    Match *match;
    Con *nc = con_for_window(croot, con->window, &match);
    if (nc == NULL || nc->window == NULL || nc->window == con->window) {
        run_assignments(con->window);
        return con;
    }
    /* Make sure the placeholder that wants to swallow this window didn't spawn
     * after the window to follow current behavior: adding a placeholder won't
     * swallow windows currently managed. */
    if (nc->window->managed_since > con->window->managed_since) {
        run_assignments(con->window);
        return con;
    }

    if (!restore_kill_placeholder(nc->window->id)) {
        DLOG("Uh?! Container without a placeholder, but with a window, has swallowed this managed window?!\n");
    } else {
        _remove_matches(nc);
    }
    window_free(nc->window);

    xcb_window_t old_frame = _match_depth(con->window, nc);

    x_reparent_child2(nc, con);

    bool moved_workpaces = (con_get_workspace(nc) != con_get_workspace(con));

    con_merge_into(con, nc);

    /* Destroy the old frame if we had to reframe the container. This needs to be done
     * after rendering in order to prevent the background from flickering in its place. */
    if (old_frame != XCB_NONE) {
        xcb_destroy_window(conn, old_frame);
    }

    run_assignments(nc->window);

    if (moved_workpaces) {
        /* If the window is associated with a startup sequence, delete it so
         * child windows won't be created on the old workspace. */
        startup_sequence_delete_by_window(nc->window);

        ewmh_update_wm_desktop();
    }

    nc->window->swallowed = true;
    return nc;
}
