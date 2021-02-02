/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * tree.c: Everything that primarily modifies the layout tree data structure.
 *
 */
#include "all.h"

struct Con *croot;
struct Con *focused;

struct all_cons_head all_cons = TAILQ_HEAD_INITIALIZER(all_cons);

/*
 * Create the pseudo-output __i3. Output-independent workspaces such as
 * __i3_scratch will live there.
 *
 */
static Con *_create___i3(void) {
    Con *__i3 = con_new(croot, NULL);
    FREE(__i3->name);
    __i3->name = sstrdup("__i3");
    __i3->type = CT_OUTPUT;
    __i3->layout = L_OUTPUT;
    con_fix_percent(croot);
    x_set_name(__i3, "[i3 con] pseudo-output __i3");
    /* For retaining the correct position/size of a scratchpad window, the
     * dimensions of the real outputs should be multiples of the __i3
     * pseudo-output. Ensuring that is the job of scratchpad_fix_resolution()
     * which gets called after this function and after detecting all the
     * outputs (or whenever an output changes). */
    __i3->rect.width = 1280;
    __i3->rect.height = 1024;

    /* Add a content container. */
    DLOG("adding main content container\n");
    Con *content = con_new(NULL, NULL);
    content->type = CT_CON;
    FREE(content->name);
    content->name = sstrdup("content");
    content->layout = L_SPLITH;

    x_set_name(content, "[i3 con] content __i3");
    con_attach(content, __i3, false);

    /* Attach the __i3_scratch workspace. */
    Con *ws = con_new(NULL, NULL);
    ws->type = CT_WORKSPACE;
    ws->num = -1;
    ws->name = sstrdup("__i3_scratch");
    ws->layout = L_SPLITH;
    con_attach(ws, content, false);
    x_set_name(ws, "[i3 con] workspace __i3_scratch");
    ws->fullscreen_mode = CF_OUTPUT;

    return __i3;
}

/*
 * Loads tree from 'path' (used for in-place restarts).
 *
 */
bool tree_restore(const char *path, xcb_get_geometry_reply_t *geometry) {
    bool result = false;
    char *globbed = resolve_tilde(path);
    char *buf = NULL;

    if (!path_exists(globbed)) {
        LOG("%s does not exist, not restoring tree\n", globbed);
        goto out;
    }

    ssize_t len;
    if ((len = slurp(globbed, &buf)) < 0) {
        /* slurp already logged an error. */
        goto out;
    }

    /* TODO: refactor the following */
    croot = con_new(NULL, NULL);
    croot->rect = (Rect){
        geometry->x,
        geometry->y,
        geometry->width,
        geometry->height};
    focused = croot;

    tree_append_json(focused, buf, len, NULL);

    DLOG("appended tree, using new root\n");
    croot = TAILQ_FIRST(&(croot->nodes_head));
    if (!croot) {
        /* tree_append_json failed. Continuing here would segfault. */
        goto out;
    }
    DLOG("new root = %p\n", croot);
    Con *out = TAILQ_FIRST(&(croot->nodes_head));
    DLOG("out = %p\n", out);
    Con *ws = TAILQ_FIRST(&(out->nodes_head));
    DLOG("ws = %p\n", ws);

    /* For in-place restarting into v4.2, we need to make sure the new
     * pseudo-output __i3 is present. */
    if (strcmp(out->name, "__i3") != 0) {
        DLOG("Adding pseudo-output __i3 during inplace restart\n");
        Con *__i3 = _create___i3();
        /* Ensure that it is the first output, other places in the code make
         * that assumption. */
        TAILQ_REMOVE(&(croot->nodes_head), __i3, nodes);
        TAILQ_INSERT_HEAD(&(croot->nodes_head), __i3, nodes);
    }

    restore_open_placeholder_windows(croot);
    result = true;

out:
    free(globbed);
    free(buf);
    return result;
}

/*
 * Initializes the tree by creating the root node. The CT_OUTPUT Cons below the
 * root node are created in randr.c for each Output.
 *
 */
void tree_init(xcb_get_geometry_reply_t *geometry) {
    croot = con_new(NULL, NULL);
    FREE(croot->name);
    croot->name = "root";
    croot->type = CT_ROOT;
    croot->layout = L_SPLITH;
    croot->rect = (Rect){
        geometry->x,
        geometry->y,
        geometry->width,
        geometry->height};

    _create___i3();
}

/*
 * Opens an empty container in the current container
 *
 */
Con *tree_open_con(Con *con, i3Window *window) {
    if (con == NULL) {
        /* every focusable Con has a parent (outputs have parent root) */
        con = focused->parent;
        /* If the parent is an output, we are on a workspace. In this case,
         * the new container needs to be opened as a leaf of the workspace. */
        if (con->parent->type == CT_OUTPUT && con->type != CT_DOCKAREA) {
            con = focused;
        }

        /* If the currently focused container is a floating container, we
         * attach the new container to the currently focused spot in its
         * workspace. */
        if (con->type == CT_FLOATING_CON) {
            con = con_descend_tiling_focused(con->parent);
            if (con->type != CT_WORKSPACE)
                con = con->parent;
        }
        DLOG("con = %p\n", con);
    }

    assert(con != NULL);

    /* 3. create the container and attach it to its parent */
    Con *new = con_new(con, window);
    new->layout = L_SPLITH;

    /* 4: re-calculate child->percent for each child */
    con_fix_percent(con);

    return new;
}

/*
 * Closes the given container including all children.
 * Returns true if the container was killed or false if just WM_DELETE was sent
 * and the window is expected to kill itself.
 *
 * The dont_kill_parent flag is specified when the function calls itself
 * recursively while deleting a containers children.
 *
 */
bool tree_close_internal(Con *con, kill_window_t kill_window, bool dont_kill_parent) {
    Con *parent = con->parent;

    /* remove the urgency hint of the workspace (if set) */
    if (con->urgent) {
        con_set_urgency(con, false);
        con_update_parents_urgency(con);
        workspace_update_urgent_flag(con_get_workspace(con));
    }

    DLOG("closing %p, kill_window = %d\n", con, kill_window);
    Con *child, *nextchild;
    bool abort_kill = false;
    /* We cannot use TAILQ_FOREACH because the children get deleted
     * in their parent’s nodes_head */
    for (child = TAILQ_FIRST(&(con->nodes_head)); child;) {
        nextchild = TAILQ_NEXT(child, nodes);
        DLOG("killing child=%p\n", child);
        if (!tree_close_internal(child, kill_window, true)) {
            abort_kill = true;
        }
        child = nextchild;
    }

    if (abort_kill) {
        DLOG("One of the children could not be killed immediately (WM_DELETE sent), aborting.\n");
        return false;
    }

    if (con->window != NULL) {
        if (kill_window != DONT_KILL_WINDOW) {
            x_window_kill(con->window->id, kill_window);
            return false;
        } else {
            xcb_void_cookie_t cookie;
            /* Ignore any further events by clearing the event mask,
             * unmap the window,
             * then reparent it to the root window. */
            xcb_change_window_attributes(conn, con->window->id,
                                         XCB_CW_EVENT_MASK, (uint32_t[]){XCB_NONE});
            xcb_unmap_window(conn, con->window->id);
            cookie = xcb_reparent_window(conn, con->window->id, root, con->rect.x, con->rect.y);

            /* Ignore X11 errors for the ReparentWindow request.
             * X11 Errors are returned when the window was already destroyed */
            add_ignore_event(cookie.sequence, 0);

            /* We are no longer handling this window, thus set WM_STATE to
             * WM_STATE_WITHDRAWN (see ICCCM 4.1.3.1) */
            long data[] = {XCB_ICCCM_WM_STATE_WITHDRAWN, XCB_NONE};
            cookie = xcb_change_property(conn, XCB_PROP_MODE_REPLACE,
                                         con->window->id, A_WM_STATE, A_WM_STATE, 32, 2, data);

            /* Remove the window from the save set. All windows in the save set
             * will be mapped when i3 closes its connection (e.g. when
             * restarting). This is not what we want, since some apps keep
             * unmapped windows around and don’t expect them to suddenly be
             * mapped. See https://bugs.i3wm.org/1617 */
            xcb_change_save_set(conn, XCB_SET_MODE_DELETE, con->window->id);

            /* Stop receiving ShapeNotify events. */
            if (shape_supported) {
                xcb_shape_select_input(conn, con->window->id, false);
            }

            /* Ignore X11 errors for the ReparentWindow request.
             * X11 Errors are returned when the window was already destroyed */
            add_ignore_event(cookie.sequence, 0);
        }
        ipc_send_window_event("close", con);
        window_free(con->window);
        con->window = NULL;
    }

    Con *ws = con_get_workspace(con);

    /* Figure out which container to focus next before detaching 'con'. */
    Con *next = (con == focused) ? con_next_focused(con) : NULL;
    DLOG("next = %p, focused = %p\n", next, focused);

    /* Detach the container so that it will not be rendered anymore. */
    con_detach(con);

    /* disable urgency timer, if needed */
    if (con->urgency_timer != NULL) {
        DLOG("Removing urgency timer of con %p\n", con);
        workspace_update_urgent_flag(ws);
        ev_timer_stop(main_loop, con->urgency_timer);
        FREE(con->urgency_timer);
    }

    if (con->type != CT_FLOATING_CON) {
        /* If the container is *not* floating, we might need to re-distribute
         * percentage values for the resized containers. */
        con_fix_percent(parent);
    }

    /* Render the tree so that the surrounding containers take up the space
     * which 'con' does no longer occupy. If we don’t render here, there will
     * be a gap in our containers and that could trigger an EnterNotify for an
     * underlying container, see ticket #660.
     *
     * Rendering has to be avoided when dont_kill_parent is set (when
     * tree_close_internal calls itself recursively) because the tree is in a
     * non-renderable state during that time. */
    if (!dont_kill_parent)
        tree_render();

    /* kill the X11 part of this container */
    x_con_kill(con);

    if (ws == con) {
        DLOG("Closing workspace container %s, updating EWMH atoms\n", ws->name);
        ewmh_update_desktop_properties();
    }

    con_free(con);

    if (next) {
        con_activate(next);
    } else {
        DLOG("not changing focus, the container was not focused before\n");
    }

    /* check if the parent container is empty now and close it */
    if (!dont_kill_parent)
        CALL(parent, on_remove_child);

    return true;
}

/*
 * Splits (horizontally or vertically) the given container by creating a new
 * container which contains the old one and the future ones.
 *
 */
void tree_split(Con *con, direction_t direction) {
#define SET_LAYOUT_AND_FILL_ORDER_FROM_DIRECTION(con, direction) \
    do {                                                         \
        Con *_con = con;                                         \
                                                                 \
        switch (direction) {                                     \
            case D_LEFT:                                         \
                _con->layout = L_SPLITH;                         \
                _con->layout_fill_order = LF_REVERSE;            \
                break;                                           \
            case D_RIGHT:                                        \
                _con->layout = L_SPLITH;                         \
                _con->layout_fill_order = LF_DEFAULT;            \
                break;                                           \
            case D_UP:                                           \
                _con->layout = L_SPLITV;                         \
                _con->layout_fill_order = LF_REVERSE;            \
                break;                                           \
            case D_DOWN:                                         \
                _con->layout = L_SPLITV;                         \
                _con->layout_fill_order = LF_DEFAULT;            \
                break;                                           \
        }                                                        \
    } while (0)

    if (con_is_floating(con)) {
        DLOG("Floating containers can't be split.\n");
        return;
    }

    if (con->type == CT_WORKSPACE) {
        if (con_num_children(con) < 2) {
            if (con_num_children(con) == 0) {
                DLOG("Changing workspace_layout to L_DEFAULT\n");
                con->workspace_layout = L_DEFAULT;
            }
            DLOG("Changing layout and fill order of workspace\n");
            SET_LAYOUT_AND_FILL_ORDER_FROM_DIRECTION(con, direction);
            return;
        } else {
            /* if there is more than one container on the workspace
             * move them into a new container and handle this instead */
            con = workspace_encapsulate(con);
        }
    }

    Con *parent = con->parent;

    /* Force re-rendering to make the indicator border visible. */
    con_force_split_parents_redraw(con);

    /* if we are in a container whose parent contains only one
     * child (its split functionality is unused so far), we just change the
     * orientation (more intuitive than splitting again) */
    if (con_num_children(parent) == 1 &&
        (parent->layout == L_SPLITH ||
         parent->layout == L_SPLITV)) {
        SET_LAYOUT_AND_FILL_ORDER_FROM_DIRECTION(parent, direction);
        DLOG("Just changing layout and fill order of existing container\n");
        return;
    }

    DLOG("Splitting in direction %d\n", direction);

    /* 2: replace it with a new Con */
    Con *new = con_new(NULL, NULL);
    TAILQ_REPLACE(&(parent->nodes_head), con, new, nodes);
    TAILQ_REPLACE(&(parent->focus_head), con, new, focused);
    new->parent = parent;
    SET_LAYOUT_AND_FILL_ORDER_FROM_DIRECTION(new, direction);

    /* 3: swap 'percent' (resize factor) */
    new->percent = con->percent;
    con->percent = 0.0;

    /* 4: add it as a child to the new Con */
    con_attach(con, new, false);
#undef SET_LAYOUT_AND_FILL_ORDER_FROM_DIRECTION
}

/*
 * Moves focus one level up. Returns true if focus changed.
 *
 */
bool level_up(void) {
    /* Skip over floating containers and go directly to the grandparent
     * (which should always be a workspace) */
    if (focused->parent->type == CT_FLOATING_CON) {
        con_activate(focused->parent->parent);
        return true;
    }

    /* We can focus up to the workspace, but not any higher in the tree */
    if ((focused->parent->type != CT_CON &&
         focused->parent->type != CT_WORKSPACE) ||
        focused->type == CT_WORKSPACE) {
        ELOG("'focus parent': Focus is already on the workspace, cannot go higher than that.\n");
        return false;
    }
    con_activate(focused->parent);
    return true;
}

/*
 * Moves focus one level down. Returns true if focus changed.
 *
 */
bool level_down(void) {
    /* Go down the focus stack of the current node */
    Con *next = TAILQ_FIRST(&(focused->focus_head));
    if (next == TAILQ_END(&(focused->focus_head))) {
        DLOG("cannot go down\n");
        return false;
    } else if (next->type == CT_FLOATING_CON) {
        /* Floating cons shouldn't be directly focused; try immediately
         * going to the grandchild of the focused con. */
        Con *child = TAILQ_FIRST(&(next->focus_head));
        if (child == TAILQ_END(&(next->focus_head))) {
            DLOG("cannot go down\n");
            return false;
        } else
            next = TAILQ_FIRST(&(next->focus_head));
    }

    con_activate(next);
    return true;
}

static void mark_unmapped(Con *con) {
    Con *current;

    con->mapped = false;
    TAILQ_FOREACH (current, &(con->nodes_head), nodes) {
        mark_unmapped(current);
    }
    if (con->type == CT_WORKSPACE) {
        /* We need to call mark_unmapped on floating nodes as well since we can
         * make containers floating. */
        TAILQ_FOREACH (current, &(con->floating_head), floating_windows) {
            mark_unmapped(current);
        }
    }
}

/*
 * Renders the tree, that is rendering all outputs using render_con() and
 * pushing the changes to X11 using x_push_changes().
 *
 */
void tree_render(void) {
    if (croot == NULL)
        return;

    DLOG("-- BEGIN RENDERING --\n");
    /* Reset map state for all nodes in tree */
    /* TODO: a nicer method to walk all nodes would be good, maybe? */
    mark_unmapped(croot);
    croot->mapped = true;

    render_con(croot);

    x_push_changes(croot);
    DLOG("-- END RENDERING --\n");
}

static Con *get_tree_next_workspace(Con *con, direction_t direction) {
    if (con_get_fullscreen_con(con, CF_GLOBAL)) {
        DLOG("Cannot change workspace while in global fullscreen mode.\n");
        return NULL;
    }

    Output *current_output = get_output_containing(con->rect.x, con->rect.y);
    if (!current_output) {
        return NULL;
    }
    DLOG("Current output is %s\n", output_primary_name(current_output));

    Output *next_output = get_output_next(direction, current_output, CLOSEST_OUTPUT);
    if (!next_output) {
        return NULL;
    }
    DLOG("Next output is %s\n", output_primary_name(next_output));

    /* Find visible workspace on next output */
    Con *workspace = NULL;
    GREP_FIRST(workspace, output_get_content(next_output->con), workspace_is_visible(child));
    return workspace;
}

/*
 * Returns the next / previous container to focus in the given direction. Does
 * not modify focus and ensures focus restrictions for fullscreen containers
 * are respected.
 *
 */
static Con *get_tree_next(Con *con, direction_t direction) {
    const bool previous = position_from_direction(direction) == BEFORE;
    const orientation_t orientation = orientation_from_direction(direction);

    Con *first_wrap = NULL;

    if (con->type == CT_WORKSPACE) {
        /* Special case for FOCUS_WRAPPING_WORKSPACE: allow the focus to leave
         * the workspace only when a workspace is selected. */
        goto handle_workspace;
    }

    while (con->type != CT_WORKSPACE) {
        if (con->fullscreen_mode == CF_OUTPUT) {
            /* We've reached a fullscreen container. Directional focus should
             * now operate on the workspace level. */
            con = con_get_workspace(con);
            break;
        } else if (con->fullscreen_mode == CF_GLOBAL) {
            /* Focus changes should happen only inside the children of a global
             * fullscreen container. */
            return first_wrap;
        }

        Con *const parent = con->parent;
        if (con->type == CT_FLOATING_CON) {
            if (orientation != HORIZ) {
                /* up/down does not change floating containers */
                return NULL;
            }

            /* left/right focuses the previous/next floating container */
            Con *next = previous ? TAILQ_PREV(con, floating_head, floating_windows)
                                 : TAILQ_NEXT(con, floating_windows);
            /* If there is no next/previous container, wrap */
            if (!next) {
                next = previous ? TAILQ_LAST(&(parent->floating_head), floating_head)
                                : TAILQ_FIRST(&(parent->floating_head));
            }
            /* Our parent does not list us in floating heads? */
            assert(next);

            return next;
        }

        if (con_num_children(parent) > 1 && con_orientation(parent) == orientation) {
            Con *const next = previous ? TAILQ_PREV(con, nodes_head, nodes)
                                       : TAILQ_NEXT(con, nodes);
            if (next && con_fullscreen_permits_focusing(next)) {
                return next;
            }

            Con *const wrap = previous ? TAILQ_LAST(&(parent->nodes_head), nodes_head)
                                       : TAILQ_FIRST(&(parent->nodes_head));
            switch (config.focus_wrapping) {
                case FOCUS_WRAPPING_OFF:
                    break;
                case FOCUS_WRAPPING_WORKSPACE:
                case FOCUS_WRAPPING_ON:
                    if (!first_wrap && con_fullscreen_permits_focusing(wrap)) {
                        first_wrap = wrap;
                    }
                    break;
                case FOCUS_WRAPPING_FORCE:
                    /* 'force' should always return to ensure focus doesn't
                     * leave the parent. */
                    if (next) {
                        return NULL; /* blocked by fullscreen */
                    }
                    return con_fullscreen_permits_focusing(wrap) ? wrap : NULL;
            }
        }

        con = parent;
    }

    assert(con->type == CT_WORKSPACE);
    if (config.focus_wrapping == FOCUS_WRAPPING_WORKSPACE) {
        return first_wrap;
    }

handle_workspace:;
    Con *workspace = get_tree_next_workspace(con, direction);
    return workspace ? workspace : first_wrap;
}

/*
 * Changes focus in the given direction
 *
 */
void tree_next(Con *con, direction_t direction) {
    Con *next = get_tree_next(con, direction);
    if (!next) {
        return;
    }
    if (next->type == CT_WORKSPACE) {
        /* Show next workspace and focus appropriate container if possible. */
        /* Use descend_focused first to give higher priority to floating or
         * tiling fullscreen containers. */
        Con *focus = con_descend_focused(next);
        if (focus->fullscreen_mode == CF_NONE) {
            Con *focus_tiling = con_descend_tiling_focused(next);
            /* If descend_tiling returned a workspace then focus is either a
             * floating container or the same workspace. */
            if (focus_tiling != next) {
                focus = focus_tiling;
            }
        }

        workspace_show(next);
        con_activate(focus);
        x_set_warp_to(&(focus->rect));
        return;
    } else if (next->type == CT_FLOATING_CON) {
        /* Raise the floating window on top of other windows preserving relative
         * stack order */
        Con *parent = next->parent;
        while (TAILQ_LAST(&(parent->floating_head), floating_head) != next) {
            Con *last = TAILQ_LAST(&(parent->floating_head), floating_head);
            TAILQ_REMOVE(&(parent->floating_head), last, floating_windows);
            TAILQ_INSERT_HEAD(&(parent->floating_head), last, floating_windows);
        }
    }

    workspace_show(con_get_workspace(next));
    con_activate(con_descend_focused(next));
}

/*
 * Get the previous / next sibling
 *
 */
Con *get_tree_next_sibling(Con *con, position_t direction) {
    Con *to_focus = (direction == BEFORE ? TAILQ_PREV(con, nodes_head, nodes)
                                         : TAILQ_NEXT(con, nodes));
    if (to_focus && con_fullscreen_permits_focusing(to_focus)) {
        return to_focus;
    }
    return NULL;
}

/*
 * tree_flatten() removes pairs of redundant split containers, e.g.:
 *       [workspace, horizontal]
 *   [v-split]           [child3]
 *   [h-split]
 * [child1] [child2]
 * In this example, the v-split and h-split container are redundant.
 * Such a situation can be created by moving containers in a direction which is
 * not the orientation of their parent container. i3 needs to create a new
 * split container then and if you move containers this way multiple times,
 * redundant chains of split-containers can be the result.
 *
 */
void tree_flatten(Con *con) {
    Con *current, *child, *parent = con->parent;
    DLOG("Checking if I can flatten con = %p / %s\n", con, con->name);

    /* We only consider normal containers without windows */
    if (con->type != CT_CON ||
        parent->layout == L_OUTPUT || /* con == "content" */
        con->window != NULL)
        goto recurse;

    /* Ensure it got only one child */
    child = TAILQ_FIRST(&(con->nodes_head));
    if (child == NULL || TAILQ_NEXT(child, nodes) != NULL)
        goto recurse;

    DLOG("child = %p, con = %p, parent = %p\n", child, con, parent);

    /* The child must have a different orientation than the con but the same as
     * the con’s parent to be redundant */
    if (!con_is_split(con) ||
        !con_is_split(child) ||
        (con->layout != L_SPLITH && con->layout != L_SPLITV) ||
        (child->layout != L_SPLITH && child->layout != L_SPLITV) ||
        con_orientation(con) == con_orientation(child) ||
        con_orientation(child) != con_orientation(parent))
        goto recurse;

    DLOG("Alright, I have to flatten this situation now. Stay calm.\n");
    /* 1: save focus */
    Con *focus_next = TAILQ_FIRST(&(child->focus_head));

    DLOG("detaching...\n");
    /* 2: re-attach the children to the parent before con */
    while (!TAILQ_EMPTY(&(child->nodes_head))) {
        current = TAILQ_FIRST(&(child->nodes_head));
        DLOG("detaching current=%p / %s\n", current, current->name);
        con_detach(current);
        DLOG("re-attaching\n");
        /* We don’t use con_attach() here because for a CT_CON, the special
         * case handling of con_attach() does not trigger. So all it would do
         * is calling TAILQ_INSERT_AFTER, but with the wrong container. So we
         * directly use the TAILQ macros. */
        current->parent = parent;
        TAILQ_INSERT_BEFORE(con, current, nodes);
        DLOG("attaching to focus list\n");
        TAILQ_INSERT_TAIL(&(parent->focus_head), current, focused);
        current->percent = con->percent;
    }
    DLOG("re-attached all\n");

    /* 3: restore focus, if con was focused */
    if (focus_next != NULL &&
        TAILQ_FIRST(&(parent->focus_head)) == con) {
        DLOG("restoring focus to focus_next=%p\n", focus_next);
        TAILQ_REMOVE(&(parent->focus_head), focus_next, focused);
        TAILQ_INSERT_HEAD(&(parent->focus_head), focus_next, focused);
        DLOG("restored focus.\n");
    }

    /* 4: close the redundant cons */
    DLOG("closing redundant cons\n");
    tree_close_internal(con, DONT_KILL_WINDOW, true);

    /* Well, we got to abort the recursion here because we destroyed the
     * container. However, if tree_flatten() is called sufficiently often,
     * there can’t be the situation of having two pairs of redundant containers
     * at once. Therefore, we can safely abort the recursion on this level
     * after flattening. */
    return;

recurse:
    /* We cannot use normal foreach here because tree_flatten might close the
     * current container. */
    current = TAILQ_FIRST(&(con->nodes_head));
    while (current != NULL) {
        Con *next = TAILQ_NEXT(current, nodes);
        tree_flatten(current);
        current = next;
    }

    current = TAILQ_FIRST(&(con->floating_head));
    while (current != NULL) {
        Con *next = TAILQ_NEXT(current, floating_windows);
        tree_flatten(current);
        current = next;
    }
}
