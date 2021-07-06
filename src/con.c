/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * con.c: Functions which deal with containers directly (creating containers,
 *        searching containers, getting specific properties from containers,
 *        …).
 *
 */
#include "all.h"
#include "yajl_utils.h"

static void con_on_remove_child(Con *con);

/*
 * force parent split containers to be redrawn
 *
 */
void con_force_split_parents_redraw(Con *con) {
    Con *parent = con;

    while (parent != NULL && parent->type != CT_WORKSPACE && parent->type != CT_DOCKAREA) {
        if (!con_is_leaf(parent)) {
            FREE(parent->deco_render_params);
        }

        parent = parent->parent;
    }
}

/*
 * Create a new container (and attach it to the given parent, if not NULL).
 * This function only initializes the data structures.
 *
 */
Con *con_new_skeleton(Con *parent, i3Window *window) {
    Con *new = scalloc(1, sizeof(Con));
    new->on_remove_child = con_on_remove_child;
    TAILQ_INSERT_TAIL(&all_cons, new, all_cons);
    new->type = CT_CON;
    new->window = window;
    new->border_style = config.default_border;
    new->current_border_width = -1;
    new->window_icon_padding = -1;
    new->window_icon_position = ICON_POSITION_TITLE;
    if (window) {
        new->depth = window->depth;
    } else {
        new->depth = root_depth;
    }
    DLOG("opening window\n");

    TAILQ_INIT(&(new->floating_head));
    TAILQ_INIT(&(new->nodes_head));
    TAILQ_INIT(&(new->focus_head));
    TAILQ_INIT(&(new->swallow_head));
    TAILQ_INIT(&(new->marks_head));

    if (parent != NULL)
        con_attach(new, parent, false);

    return new;
}

/* A wrapper for con_new_skeleton, to retain the old con_new behaviour
 *
 */
Con *con_new(Con *parent, i3Window *window) {
    Con *new = con_new_skeleton(parent, window);
    x_con_init(new);
    return new;
}

/*
 * Frees the specified container.
 *
 */
void con_free(Con *con) {
    free(con->name);
    FREE(con->deco_render_params);
    TAILQ_REMOVE(&all_cons, con, all_cons);
    while (!TAILQ_EMPTY(&(con->swallow_head))) {
        Match *match = TAILQ_FIRST(&(con->swallow_head));
        TAILQ_REMOVE(&(con->swallow_head), match, matches);
        match_free(match);
        free(match);
    }
    while (!TAILQ_EMPTY(&(con->marks_head))) {
        mark_t *mark = TAILQ_FIRST(&(con->marks_head));
        TAILQ_REMOVE(&(con->marks_head), mark, marks);
        FREE(mark->name);
        FREE(mark);
    }
    DLOG("con %p freed\n", con);
    free(con);
}

static void _con_attach(Con *con, Con *parent, Con *previous, bool ignore_focus) {
    con->parent = parent;
    Con *loop;
    Con *current = previous;
    struct nodes_head *nodes_head = &(parent->nodes_head);
    struct focus_head *focus_head = &(parent->focus_head);

    /* Workspaces are handled differently: they need to be inserted at the
     * right position. */
    if (con->type == CT_WORKSPACE) {
        DLOG("it's a workspace. num = %d\n", con->num);
        if (con->num == -1 || TAILQ_EMPTY(nodes_head)) {
            TAILQ_INSERT_TAIL(nodes_head, con, nodes);
        } else {
            current = TAILQ_FIRST(nodes_head);
            if (con->num < current->num) {
                /* we need to insert the container at the beginning */
                TAILQ_INSERT_HEAD(nodes_head, con, nodes);
            } else {
                while (current->num != -1 && con->num > current->num) {
                    current = TAILQ_NEXT(current, nodes);
                    if (current == TAILQ_END(nodes_head)) {
                        current = NULL;
                        break;
                    }
                }
                /* we need to insert con after current, if current is not NULL */
                if (current)
                    TAILQ_INSERT_BEFORE(current, con, nodes);
                else
                    TAILQ_INSERT_TAIL(nodes_head, con, nodes);
            }
        }
        goto add_to_focus_head;
    }

    if (parent->type == CT_DOCKAREA) {
        /* Insert dock client, sorting alphanumerically by class and then
         * instance name. This makes dock client order deterministic. As a side
         * effect, bars without a custom bar id will be sorted according to
         * their declaration order in the config file. See #3491. */
        current = NULL;
        TAILQ_FOREACH (loop, nodes_head, nodes) {
            int result = strcasecmp_nullable(con->window->class_class, loop->window->class_class);
            if (result == 0) {
                result = strcasecmp_nullable(con->window->class_instance, loop->window->class_instance);
            }
            if (result < 0) {
                current = loop;
                break;
            }
        }
        if (current) {
            TAILQ_INSERT_BEFORE(loop, con, nodes);
        } else {
            TAILQ_INSERT_TAIL(nodes_head, con, nodes);
        }
        goto add_to_focus_head;
    }

    if (con->type == CT_FLOATING_CON) {
        DLOG("Inserting into floating containers\n");
        TAILQ_INSERT_TAIL(&(parent->floating_head), con, floating_windows);
    } else {
        if (!ignore_focus) {
            /* Get the first tiling container in focus stack */
            TAILQ_FOREACH (loop, &(parent->focus_head), focused) {
                if (loop->type == CT_FLOATING_CON)
                    continue;
                current = loop;
                break;
            }
        }

        /* When the container is not a split container (but contains a window)
         * and is attached to a workspace, we check if the user configured a
         * workspace_layout. This is done in workspace_attach_to, which will
         * provide us with the container to which we should attach (either the
         * workspace or a new split container with the configured
         * workspace_layout).
         */
        if (con->window != NULL &&
            parent->type == CT_WORKSPACE &&
            parent->workspace_layout != L_DEFAULT) {
            DLOG("Parent is a workspace. Applying default layout...\n");
            Con *target = workspace_attach_to(parent);

            /* Attach the original con to this new split con instead */
            nodes_head = &(target->nodes_head);
            focus_head = &(target->focus_head);
            con->parent = target;
            current = NULL;

            DLOG("done\n");
        }

        /* Insert the container after the tiling container, if found.
         * When adding to a CT_OUTPUT, just append one after another. */
        if (current != NULL && parent->type != CT_OUTPUT) {
            DLOG("Inserting con = %p after con %p\n", con, current);
            TAILQ_INSERT_AFTER(nodes_head, current, con, nodes);
        } else
            TAILQ_INSERT_TAIL(nodes_head, con, nodes);
    }

add_to_focus_head:
    /* We insert to the TAIL because con_focus() will correct this.
     * This way, we have the option to insert Cons without having
     * to focus them. */
    TAILQ_INSERT_TAIL(focus_head, con, focused);
    con_force_split_parents_redraw(con);
}

/*
 * Attaches the given container to the given parent. This happens when moving
 * a container or when inserting a new container at a specific place in the
 * tree.
 *
 * ignore_focus is to just insert the Con at the end (useful when creating a
 * new split container *around* some containers, that is, detaching and
 * attaching them in order without wanting to mess with the focus in between).
 *
 */
void con_attach(Con *con, Con *parent, bool ignore_focus) {
    _con_attach(con, parent, NULL, ignore_focus);
}

/*
 * Detaches the given container from its current parent
 *
 */
void con_detach(Con *con) {
    con_force_split_parents_redraw(con);
    if (con->type == CT_FLOATING_CON) {
        TAILQ_REMOVE(&(con->parent->floating_head), con, floating_windows);
        TAILQ_REMOVE(&(con->parent->focus_head), con, focused);
    } else {
        TAILQ_REMOVE(&(con->parent->nodes_head), con, nodes);
        TAILQ_REMOVE(&(con->parent->focus_head), con, focused);
    }
}

/*
 * Sets input focus to the given container. Will be updated in X11 in the next
 * run of x_push_changes().
 *
 */
void con_focus(Con *con) {
    assert(con != NULL);
    DLOG("con_focus = %p\n", con);

    /* 1: set focused-pointer to the new con */
    /* 2: exchange the position of the container in focus stack of the parent all the way up */
    TAILQ_REMOVE(&(con->parent->focus_head), con, focused);
    TAILQ_INSERT_HEAD(&(con->parent->focus_head), con, focused);
    if (con->parent->parent != NULL)
        con_focus(con->parent);

    focused = con;
    /* We can't blindly reset non-leaf containers since they might have
     * other urgent children. Therefore we only reset leafs and propagate
     * the changes upwards via con_update_parents_urgency() which does proper
     * checks before resetting the urgency.
     */
    if (con->urgent && con_is_leaf(con)) {
        con_set_urgency(con, false);
        con_update_parents_urgency(con);
        workspace_update_urgent_flag(con_get_workspace(con));
        ipc_send_window_event("urgent", con);
    }
}

/*
 * Raise container to the top if it is floating or inside some floating
 * container.
 *
 */
static void con_raise(Con *con) {
    Con *floating = con_inside_floating(con);
    if (floating) {
        floating_raise_con(floating);
    }
}

/*
 * Sets input focus to the given container and raises it to the top.
 *
 */
void con_activate(Con *con) {
    con_focus(con);
    con_raise(con);
}

/*
 * Activates the container like in con_activate but removes fullscreen
 * restrictions and properly warps the pointer if needed.
 *
 */
void con_activate_unblock(Con *con) {
    Con *ws = con_get_workspace(con);
    Con *previous_focus = focused;
    Con *fullscreen_on_ws = con_get_fullscreen_covering_ws(ws);

    if (fullscreen_on_ws && fullscreen_on_ws != con && !con_has_parent(con, fullscreen_on_ws)) {
        con_disable_fullscreen(fullscreen_on_ws);
    }

    con_activate(con);

    /* If the container is not on the current workspace, workspace_show() will
     * switch to a different workspace and (if enabled) trigger a mouse pointer
     * warp to the currently focused container (!) on the target workspace.
     *
     * Therefore, before calling workspace_show(), we make sure that 'con' will
     * be focused on the workspace. However, we cannot just con_focus(con)
     * because then the pointer will not be warped at all (the code thinks we
     * are already there).
     *
     * So we focus 'con' to make it the currently focused window of the target
     * workspace, then revert focus. */
    if (ws != con_get_workspace(previous_focus)) {
        con_activate(previous_focus);
        /* Now switch to the workspace, then focus */
        workspace_show(ws);
        con_activate(con);
    }
}

/*
 * Closes the given container.
 *
 */
void con_close(Con *con, kill_window_t kill_window) {
    assert(con != NULL);
    DLOG("Closing con = %p.\n", con);

    /* We never close output or root containers. */
    if (con->type == CT_OUTPUT || con->type == CT_ROOT) {
        DLOG("con = %p is of type %d, not closing anything.\n", con, con->type);
        return;
    }

    if (con->type == CT_WORKSPACE) {
        DLOG("con = %p is a workspace, closing all children instead.\n", con);
        Con *child, *nextchild;
        for (child = TAILQ_FIRST(&(con->focus_head)); child;) {
            nextchild = TAILQ_NEXT(child, focused);
            DLOG("killing child = %p.\n", child);
            tree_close_internal(child, kill_window, false);
            child = nextchild;
        }

        return;
    }

    tree_close_internal(con, kill_window, false);
}

/*
 * Returns true when this node is a leaf node (has no children)
 *
 */
bool con_is_leaf(Con *con) {
    return TAILQ_EMPTY(&(con->nodes_head));
}

/*
 * Returns true when this con is a leaf node with a managed X11 window (e.g.,
 * excluding dock containers)
 */
bool con_has_managed_window(Con *con) {
    return (con != NULL && con->window != NULL && con->window->id != XCB_WINDOW_NONE && con_get_workspace(con) != NULL);
}

/*
 * Returns true if this node has regular or floating children.
 *
 */
bool con_has_children(Con *con) {
    return (!con_is_leaf(con) || !TAILQ_EMPTY(&(con->floating_head)));
}

/*
 * Returns true if a container should be considered split.
 *
 */
bool con_is_split(Con *con) {
    if (con_is_leaf(con))
        return false;

    switch (con->layout) {
        case L_DOCKAREA:
        case L_OUTPUT:
            return false;

        default:
            return true;
    }
}

/*
 * This will only return true for containers which have some parent with
 * a tabbed / stacked parent of which they are not the currently focused child.
 *
 */
bool con_is_hidden(Con *con) {
    Con *current = con;

    /* ascend to the workspace level and memorize the highest-up container
     * which is stacked or tabbed. */
    while (current != NULL && current->type != CT_WORKSPACE) {
        Con *parent = current->parent;
        if (parent != NULL && (parent->layout == L_TABBED || parent->layout == L_STACKED)) {
            if (TAILQ_FIRST(&(parent->focus_head)) != current)
                return true;
        }

        current = parent;
    }

    return false;
}

/*
 * Returns whether the container or any of its children is sticky.
 *
 */
bool con_is_sticky(Con *con) {
    if (con->sticky)
        return true;

    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        if (con_is_sticky(child))
            return true;
    }

    return false;
}

/*
 * Returns true if this node accepts a window (if the node swallows windows,
 * it might already have swallowed enough and cannot hold any more).
 *
 */
bool con_accepts_window(Con *con) {
    /* 1: workspaces never accept direct windows */
    if (con->type == CT_WORKSPACE)
        return false;

    if (con_is_split(con)) {
        DLOG("container %p does not accept windows, it is a split container.\n", con);
        return false;
    }

    /* TODO: if this is a swallowing container, we need to check its max_clients */
    return (con->window == NULL);
}

/*
 * Gets the output container (first container with CT_OUTPUT in hierarchy) this
 * node is on.
 *
 */
Con *con_get_output(Con *con) {
    Con *result = con;
    while (result != NULL && result->type != CT_OUTPUT)
        result = result->parent;
    /* We must be able to get an output because focus can never be set higher
     * in the tree (root node cannot be focused). */
    assert(result != NULL);
    return result;
}

/*
 * Gets the workspace container this node is on.
 *
 */
Con *con_get_workspace(Con *con) {
    Con *result = con;
    while (result != NULL && result->type != CT_WORKSPACE)
        result = result->parent;
    return result;
}

/*
 * Searches parents of the given 'con' until it reaches one with the specified
 * 'orientation'. Aborts when it comes across a floating_con.
 *
 */
Con *con_parent_with_orientation(Con *con, orientation_t orientation) {
    DLOG("Searching for parent of Con %p with orientation %d\n", con, orientation);
    Con *parent = con->parent;
    if (parent->type == CT_FLOATING_CON)
        return NULL;
    while (con_orientation(parent) != orientation) {
        DLOG("Need to go one level further up\n");
        parent = parent->parent;
        /* Abort when we reach a floating con, or an output con */
        if (parent &&
            (parent->type == CT_FLOATING_CON ||
             parent->type == CT_OUTPUT ||
             (parent->parent && parent->parent->type == CT_OUTPUT)))
            parent = NULL;
        if (parent == NULL)
            break;
    }
    DLOG("Result: %p\n", parent);
    return parent;
}

/*
 * helper data structure for the breadth-first-search in
 * con_get_fullscreen_con()
 *
 */
struct bfs_entry {
    Con *con;

    TAILQ_ENTRY(bfs_entry) entries;
};

/*
 * Returns the first fullscreen node below this node.
 *
 */
Con *con_get_fullscreen_con(Con *con, fullscreen_mode_t fullscreen_mode) {
    Con *current, *child;

    /* TODO: is breadth-first-search really appropriate? (check as soon as
     * fullscreen levels and fullscreen for containers is implemented) */
    TAILQ_HEAD(bfs_head, bfs_entry) bfs_head = TAILQ_HEAD_INITIALIZER(bfs_head);
    struct bfs_entry *entry = smalloc(sizeof(struct bfs_entry));
    entry->con = con;
    TAILQ_INSERT_TAIL(&bfs_head, entry, entries);

    while (!TAILQ_EMPTY(&bfs_head)) {
        entry = TAILQ_FIRST(&bfs_head);
        current = entry->con;
        if (current != con && current->fullscreen_mode == fullscreen_mode) {
            /* empty the queue */
            while (!TAILQ_EMPTY(&bfs_head)) {
                entry = TAILQ_FIRST(&bfs_head);
                TAILQ_REMOVE(&bfs_head, entry, entries);
                free(entry);
            }
            return current;
        }

        TAILQ_REMOVE(&bfs_head, entry, entries);
        free(entry);

        TAILQ_FOREACH (child, &(current->nodes_head), nodes) {
            entry = smalloc(sizeof(struct bfs_entry));
            entry->con = child;
            TAILQ_INSERT_TAIL(&bfs_head, entry, entries);
        }

        TAILQ_FOREACH (child, &(current->floating_head), floating_windows) {
            entry = smalloc(sizeof(struct bfs_entry));
            entry->con = child;
            TAILQ_INSERT_TAIL(&bfs_head, entry, entries);
        }
    }

    return NULL;
}

/*
 * Returns the fullscreen node that covers the given workspace if it exists.
 * This is either a CF_GLOBAL fullscreen container anywhere or a CF_OUTPUT
 * fullscreen container in the workspace.
 *
 */
Con *con_get_fullscreen_covering_ws(Con *ws) {
    if (!ws) {
        return NULL;
    }
    Con *fs = con_get_fullscreen_con(croot, CF_GLOBAL);
    if (!fs) {
        return con_get_fullscreen_con(ws, CF_OUTPUT);
    }
    return fs;
}

/*
 * Returns true if the container is internal, such as __i3_scratch
 *
 */
bool con_is_internal(Con *con) {
    return (con->name[0] == '_' && con->name[1] == '_');
}

/*
 * Returns true if the node is floating.
 *
 */
bool con_is_floating(Con *con) {
    assert(con != NULL);
    return (con->floating >= FLOATING_AUTO_ON);
}

/*
 * Returns true if the container is a docked container.
 *
 */
bool con_is_docked(Con *con) {
    if (con->parent == NULL)
        return false;

    if (con->parent->type == CT_DOCKAREA)
        return true;

    return con_is_docked(con->parent);
}

/*
 * Checks if the given container is either floating or inside some floating
 * container. It returns the FLOATING_CON container.
 *
 */
Con *con_inside_floating(Con *con) {
    assert(con != NULL);
    if (con->type == CT_FLOATING_CON)
        return con;

    if (con->floating >= FLOATING_AUTO_ON)
        return con->parent;

    if (con->type == CT_WORKSPACE || con->type == CT_OUTPUT)
        return NULL;

    return con_inside_floating(con->parent);
}

/*
 * Checks if the given container is inside a focused container.
 *
 */
bool con_inside_focused(Con *con) {
    if (con == focused)
        return true;
    if (!con->parent)
        return false;
    return con_inside_focused(con->parent);
}

/*
 * Checks if the container has the given parent as an actual parent.
 *
 */
bool con_has_parent(Con *con, Con *parent) {
    Con *current = con->parent;
    if (current == NULL) {
        return false;
    }

    if (current == parent) {
        return true;
    }

    return con_has_parent(current, parent);
}

/*
 * Returns the container with the given client window ID or NULL if no such
 * container exists.
 *
 */
Con *con_by_window_id(xcb_window_t window) {
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->window != NULL && con->window->id == window) {
            return con;
        }
    }
    return NULL;
}

/*
 * Returns the container with the given container ID or NULL if no such
 * container exists.
 *
 */
Con *con_by_con_id(long target) {
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con == (Con *)target) {
            return con;
        }
    }

    return NULL;
}

/*
 * Returns true if the given container (still) exists.
 * This can be used, e.g., to make sure a container hasn't been closed in the meantime.
 *
 */
bool con_exists(Con *con) {
    return con_by_con_id((long)con) != NULL;
}

/*
 * Returns the container with the given frame ID or NULL if no such container
 * exists.
 *
 */
Con *con_by_frame_id(xcb_window_t frame) {
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->frame.id == frame) {
            return con;
        }
    }
    return NULL;
}

/*
 * Returns the container with the given mark or NULL if no such container
 * exists.
 *
 */
Con *con_by_mark(const char *mark) {
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con_has_mark(con, mark))
            return con;
    }

    return NULL;
}

/*
 * Returns true if and only if the given containers holds the mark.
 *
 */
bool con_has_mark(Con *con, const char *mark) {
    mark_t *current;
    TAILQ_FOREACH (current, &(con->marks_head), marks) {
        if (strcmp(current->name, mark) == 0)
            return true;
    }

    return false;
}

/*
 * Toggles the mark on a container.
 * If the container already has this mark, the mark is removed.
 * Otherwise, the mark is assigned to the container.
 *
 */
void con_mark_toggle(Con *con, const char *mark, mark_mode_t mode) {
    assert(con != NULL);
    DLOG("Toggling mark \"%s\" on con = %p.\n", mark, con);

    if (con_has_mark(con, mark)) {
        con_unmark(con, mark);
    } else {
        con_mark(con, mark, mode);
    }
}

/*
 * Assigns a mark to the container.
 *
 */
void con_mark(Con *con, const char *mark, mark_mode_t mode) {
    assert(con != NULL);
    DLOG("Setting mark \"%s\" on con = %p.\n", mark, con);

    con_unmark(NULL, mark);
    if (mode == MM_REPLACE) {
        DLOG("Removing all existing marks on con = %p.\n", con);

        mark_t *current;
        while (!TAILQ_EMPTY(&(con->marks_head))) {
            current = TAILQ_FIRST(&(con->marks_head));
            con_unmark(con, current->name);
        }
    }

    mark_t *new = scalloc(1, sizeof(mark_t));
    new->name = sstrdup(mark);
    TAILQ_INSERT_TAIL(&(con->marks_head), new, marks);
    ipc_send_window_event("mark", con);

    con->mark_changed = true;
}

/*
 * Removes marks from containers.
 * If con is NULL, all containers are considered.
 * If name is NULL, this removes all existing marks.
 * Otherwise, it will only remove the given mark (if it is present).
 *
 */
void con_unmark(Con *con, const char *name) {
    Con *current;
    if (name == NULL) {
        DLOG("Unmarking all containers.\n");
        TAILQ_FOREACH (current, &all_cons, all_cons) {
            if (con != NULL && current != con)
                continue;

            if (TAILQ_EMPTY(&(current->marks_head)))
                continue;

            mark_t *mark;
            while (!TAILQ_EMPTY(&(current->marks_head))) {
                mark = TAILQ_FIRST(&(current->marks_head));
                FREE(mark->name);
                TAILQ_REMOVE(&(current->marks_head), mark, marks);
                FREE(mark);

                ipc_send_window_event("mark", current);
            }

            current->mark_changed = true;
        }
    } else {
        DLOG("Removing mark \"%s\".\n", name);
        current = (con == NULL) ? con_by_mark(name) : con;
        if (current == NULL) {
            DLOG("No container found with this mark, so there is nothing to do.\n");
            return;
        }

        DLOG("Found mark on con = %p. Removing it now.\n", current);
        current->mark_changed = true;

        mark_t *mark;
        TAILQ_FOREACH (mark, &(current->marks_head), marks) {
            if (strcmp(mark->name, name) != 0)
                continue;

            FREE(mark->name);
            TAILQ_REMOVE(&(current->marks_head), mark, marks);
            FREE(mark);

            ipc_send_window_event("mark", current);
            break;
        }
    }
}

/*
 * Returns the first container below 'con' which wants to swallow this window
 * TODO: priority
 *
 */
Con *con_for_window(Con *con, i3Window *window, Match **store_match) {
    Con *child;
    Match *match;
    //DLOG("searching con for window %p starting at con %p\n", window, con);
    //DLOG("class == %s\n", window->class_class);

    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        TAILQ_FOREACH (match, &(child->swallow_head), matches) {
            if (!match_matches_window(match, window))
                continue;
            if (store_match != NULL)
                *store_match = match;
            return child;
        }
        Con *result = con_for_window(child, window, store_match);
        if (result != NULL)
            return result;
    }

    TAILQ_FOREACH (child, &(con->floating_head), floating_windows) {
        TAILQ_FOREACH (match, &(child->swallow_head), matches) {
            if (!match_matches_window(match, window))
                continue;
            if (store_match != NULL)
                *store_match = match;
            return child;
        }
        Con *result = con_for_window(child, window, store_match);
        if (result != NULL)
            return result;
    }

    return NULL;
}

static int num_focus_heads(Con *con) {
    int focus_heads = 0;

    Con *current;
    TAILQ_FOREACH (current, &(con->focus_head), focused) {
        focus_heads++;
    }

    return focus_heads;
}

/*
 * Iterate over the container's focus stack and return an array with the
 * containers inside it, ordered from higher focus order to lowest.
 *
 */
Con **get_focus_order(Con *con) {
    const int focus_heads = num_focus_heads(con);
    Con **focus_order = smalloc(focus_heads * sizeof(Con *));
    Con *current;
    int idx = 0;
    TAILQ_FOREACH (current, &(con->focus_head), focused) {
        assert(idx < focus_heads);
        focus_order[idx++] = current;
    }

    return focus_order;
}

/*
 * Clear the container's focus stack and re-add it using the provided container
 * array. The function doesn't check if the provided array contains the same
 * containers with the previous focus stack but will not add floating containers
 * in the new focus stack if container is not a workspace.
 *
 */
void set_focus_order(Con *con, Con **focus_order) {
    int focus_heads = 0;
    while (!TAILQ_EMPTY(&(con->focus_head))) {
        Con *current = TAILQ_FIRST(&(con->focus_head));

        TAILQ_REMOVE(&(con->focus_head), current, focused);
        focus_heads++;
    }

    for (int idx = 0; idx < focus_heads; idx++) {
        /* Useful when encapsulating a workspace. */
        if (con->type != CT_WORKSPACE && con_inside_floating(focus_order[idx])) {
            focus_heads++;
            continue;
        }

        TAILQ_INSERT_TAIL(&(con->focus_head), focus_order[idx], focused);
    }
}

/*
 * Returns the number of children of this container.
 *
 */
int con_num_children(Con *con) {
    Con *child;
    int children = 0;

    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        children++;
    }

    return children;
}

/*
 * Returns the number of visible non-floating children of this container.
 * For example, if the container contains a hsplit which has two children,
 * this will return 2 instead of 1.
 */
int con_num_visible_children(Con *con) {
    if (con == NULL)
        return 0;

    int children = 0;
    Con *current = NULL;
    TAILQ_FOREACH (current, &(con->nodes_head), nodes) {
        /* Visible leaf nodes are a child. */
        if (!con_is_hidden(current) && con_is_leaf(current))
            children++;
        /* All other containers need to be recursed. */
        else
            children += con_num_visible_children(current);
    }

    return children;
}

/*
 * Count the number of windows (i.e., leaf containers).
 *
 */
int con_num_windows(Con *con) {
    if (con == NULL)
        return 0;

    if (con_has_managed_window(con))
        return 1;

    int num = 0;
    Con *current = NULL;
    TAILQ_FOREACH (current, &(con->nodes_head), nodes) {
        num += con_num_windows(current);
    }

    TAILQ_FOREACH (current, &(con->floating_head), floating_windows) {
        num += con_num_windows(current);
    }

    return num;
}

/*
 * Updates the percent attribute of the children of the given container. This
 * function needs to be called when a window is added or removed from a
 * container.
 *
 */
void con_fix_percent(Con *con) {
    Con *child;
    int children = con_num_children(con);

    // calculate how much we have distributed and how many containers
    // with a percentage set we have
    double total = 0.0;
    int children_with_percent = 0;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        if (child->percent > 0.0) {
            total += child->percent;
            ++children_with_percent;
        }
    }

    // if there were children without a percentage set, set to a value that
    // will make those children proportional to all others
    if (children_with_percent != children) {
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            if (child->percent <= 0.0) {
                if (children_with_percent == 0) {
                    total += (child->percent = 1.0);
                } else {
                    total += (child->percent = total / children_with_percent);
                }
            }
        }
    }

    // if we got a zero, just distribute the space equally, otherwise
    // distribute according to the proportions we got
    if (total == 0.0) {
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            child->percent = 1.0 / children;
        }
    } else if (total != 1.0) {
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            child->percent /= total;
        }
    }
}

/*
 * Toggles fullscreen mode for the given container. If there already is a
 * fullscreen container on this workspace, fullscreen will be disabled and then
 * enabled for the container the user wants to have in fullscreen mode.
 *
 */
void con_toggle_fullscreen(Con *con, int fullscreen_mode) {
    if (con->type == CT_WORKSPACE) {
        DLOG("You cannot make a workspace fullscreen.\n");
        return;
    }

    DLOG("toggling fullscreen for %p / %s\n", con, con->name);

    if (con->fullscreen_mode == CF_NONE)
        con_enable_fullscreen(con, fullscreen_mode);
    else
        con_disable_fullscreen(con);
}

/*
 * Sets the specified fullscreen mode for the given container, sends the
 * “fullscreen_mode” event and changes the XCB fullscreen property of the
 * container’s window, if any.
 *
 */
static void con_set_fullscreen_mode(Con *con, fullscreen_mode_t fullscreen_mode) {
    con->fullscreen_mode = fullscreen_mode;

    DLOG("mode now: %d\n", con->fullscreen_mode);

    /* Send an ipc window "fullscreen_mode" event */
    ipc_send_window_event("fullscreen_mode", con);

    /* update _NET_WM_STATE if this container has a window */
    /* TODO: when a window is assigned to a container which is already
     * fullscreened, this state needs to be pushed to the client, too */
    if (con->window == NULL)
        return;

    if (con->fullscreen_mode != CF_NONE) {
        DLOG("Setting _NET_WM_STATE_FULLSCREEN for con = %p / window = %d.\n", con, con->window->id);
        xcb_add_property_atom(conn, con->window->id, A__NET_WM_STATE, A__NET_WM_STATE_FULLSCREEN);
    } else {
        DLOG("Removing _NET_WM_STATE_FULLSCREEN for con = %p / window = %d.\n", con, con->window->id);
        xcb_remove_property_atom(conn, con->window->id, A__NET_WM_STATE, A__NET_WM_STATE_FULLSCREEN);
    }
}

/*
 * Enables fullscreen mode for the given container, if necessary.
 *
 * If the container’s mode is already CF_OUTPUT or CF_GLOBAL, the container is
 * kept fullscreen but its mode is set to CF_GLOBAL and CF_OUTPUT,
 * respectively.
 *
 * Other fullscreen containers will be disabled first, if they hide the new
 * one.
 *
 */
void con_enable_fullscreen(Con *con, fullscreen_mode_t fullscreen_mode) {
    if (con->type == CT_WORKSPACE) {
        DLOG("You cannot make a workspace fullscreen.\n");
        return;
    }

    assert(fullscreen_mode == CF_GLOBAL || fullscreen_mode == CF_OUTPUT);

    if (fullscreen_mode == CF_GLOBAL)
        DLOG("enabling global fullscreen for %p / %s\n", con, con->name);
    else
        DLOG("enabling fullscreen for %p / %s\n", con, con->name);

    if (con->fullscreen_mode == fullscreen_mode) {
        DLOG("fullscreen already enabled for %p / %s\n", con, con->name);
        return;
    }

    Con *con_ws = con_get_workspace(con);

    /* Disable any fullscreen container that would conflict the new one. */
    Con *fullscreen = con_get_fullscreen_con(croot, CF_GLOBAL);
    if (fullscreen == NULL)
        fullscreen = con_get_fullscreen_con(con_ws, CF_OUTPUT);
    if (fullscreen != NULL)
        con_disable_fullscreen(fullscreen);

    /* Set focus to new fullscreen container. Unless in global fullscreen mode
     * and on another workspace restore focus afterwards.
     * Switch to the container’s workspace if mode is global. */
    Con *cur_ws = con_get_workspace(focused);
    Con *old_focused = focused;
    if (fullscreen_mode == CF_GLOBAL && cur_ws != con_ws)
        workspace_show(con_ws);
    con_activate(con);
    if (fullscreen_mode != CF_GLOBAL && cur_ws != con_ws)
        con_activate(old_focused);

    con_set_fullscreen_mode(con, fullscreen_mode);
}

/*
 * Disables fullscreen mode for the given container regardless of the mode, if
 * necessary.
 *
 */
void con_disable_fullscreen(Con *con) {
    if (con->type == CT_WORKSPACE) {
        DLOG("You cannot make a workspace fullscreen.\n");
        return;
    }

    DLOG("disabling fullscreen for %p / %s\n", con, con->name);

    if (con->fullscreen_mode == CF_NONE) {
        DLOG("fullscreen already disabled for %p / %s\n", con, con->name);
        return;
    }

    con_set_fullscreen_mode(con, CF_NONE);
}

static bool _con_move_to_con(Con *con, Con *target, bool behind_focused, bool fix_coordinates, bool dont_warp, bool ignore_focus, bool fix_percentage) {
    Con *orig_target = target;

    /* Prevent moving if this would violate the fullscreen focus restrictions. */
    Con *target_ws = con_get_workspace(target);
    if (!ignore_focus && !con_fullscreen_permits_focusing(target_ws)) {
        LOG("Cannot move out of a fullscreen container.\n");
        return false;
    }

    if (con_is_floating(con)) {
        DLOG("Container is floating, using parent instead.\n");
        con = con->parent;
    }

    Con *source_ws = con_get_workspace(con);

    if (con->type == CT_WORKSPACE) {
        /* Re-parent all of the old workspace's floating windows. */
        Con *child;
        while (!TAILQ_EMPTY(&(source_ws->floating_head))) {
            child = TAILQ_FIRST(&(source_ws->floating_head));
            con_move_to_workspace(child, target_ws, true, true, false);
        }

        /* If there are no non-floating children, ignore the workspace. */
        if (con_is_leaf(con))
            return false;

        con = workspace_encapsulate(con);
        if (con == NULL) {
            ELOG("Workspace failed to move its contents into a container!\n");
            return false;
        }
    }

    /* Save the urgency state so that we can restore it. */
    bool urgent = con->urgent;

    /* Save the current workspace. So we can call workspace_show() by the end
     * of this function. */
    Con *current_ws = con_get_workspace(focused);

    Con *source_output = con_get_output(con),
        *dest_output = con_get_output(target_ws);

    /* 1: save the container which is going to be focused after the current
     * container is moved away */
    Con *focus_next = NULL;
    if (!ignore_focus && source_ws == current_ws && target_ws != source_ws) {
        focus_next = con_descend_focused(source_ws);
        if (focus_next == con || con_has_parent(focus_next, con)) {
            focus_next = con_next_focused(con);
        }
    }

    /* 2: we go up one level, but only when target is a normal container */
    if (target->type != CT_WORKSPACE) {
        DLOG("target originally = %p / %s / type %d\n", target, target->name, target->type);
        target = target->parent;
    }

    /* 3: if the original target is the direct child of a floating container, we
     * can't move con next to it - floating containers have only one child - so
     * we get the workspace instead. */
    if (target->type == CT_FLOATING_CON) {
        DLOG("floatingcon, going up even further\n");
        orig_target = target;
        target = target->parent;
    }

    if (con->type == CT_FLOATING_CON) {
        Con *ws = con_get_workspace(target);
        DLOG("This is a floating window, using workspace %p / %s\n", ws, ws->name);
        target = ws;
    }

    if (source_output != dest_output) {
        /* Take the relative coordinates of the current output, then add them
         * to the coordinate space of the correct output */
        if (fix_coordinates && con->type == CT_FLOATING_CON) {
            floating_fix_coordinates(con, &(source_output->rect), &(dest_output->rect));
        } else
            DLOG("Not fixing coordinates, fix_coordinates flag = %d\n", fix_coordinates);
    }

    /* If moving a fullscreen container and the destination already has a
     * fullscreen window on it, un-fullscreen the target's fullscreen con.
     * con->fullscreen_mode is not enough in some edge cases:
     * 1. con is CT_FLOATING_CON, child is fullscreen.
     * 2. con is the parent of a fullscreen container, can be triggered by
     * moving the parent with command criteria.
     */
    Con *fullscreen = con_get_fullscreen_con(target_ws, CF_OUTPUT);
    const bool con_has_fullscreen = con->fullscreen_mode != CF_NONE ||
                                    con_get_fullscreen_con(con, CF_GLOBAL) ||
                                    con_get_fullscreen_con(con, CF_OUTPUT);
    if (con_has_fullscreen && fullscreen != NULL) {
        con_toggle_fullscreen(fullscreen, CF_OUTPUT);
        fullscreen = NULL;
    }

    DLOG("Re-attaching container to %p / %s\n", target, target->name);
    /* 4: re-attach the con to the parent of this focused container */
    Con *parent = con->parent;
    con_detach(con);
    _con_attach(con, target, behind_focused ? NULL : orig_target, !behind_focused);

    /* 5: fix the percentages */
    if (fix_percentage) {
        con_fix_percent(parent);
        con->percent = 0.0;
        con_fix_percent(target);
    }

    /* 6: focus the con on the target workspace, but only within that
     * workspace, that is, don’t move focus away if the target workspace is
     * invisible.
     * We don’t focus the con for i3 pseudo workspaces like __i3_scratch and
     * we don’t focus when there is a fullscreen con on that workspace. We
     * also don't do it if the caller requested to ignore focus. */
    if (!ignore_focus && !con_is_internal(target_ws) && !fullscreen) {
        /* We need to save the focused workspace on the output in case the
         * new workspace is hidden and it's necessary to immediately switch
         * back to the originally-focused workspace. */
        Con *old_focus_ws = TAILQ_FIRST(&(output_get_content(dest_output)->focus_head));
        Con *old_focus = focused;
        con_activate(con_descend_focused(con));

        if (old_focus_ws == current_ws && old_focus->type != CT_WORKSPACE) {
            /* Restore focus to the currently focused container. */
            con_activate(old_focus);
        } else if (con_get_workspace(focused) != old_focus_ws) {
            /* Restore focus if the output's focused workspace has changed. */
            con_focus(con_descend_focused(old_focus_ws));
        }
    }

    /* 7: when moving to another workspace, we leave the focus on the current
     * workspace. (see also #809) */
    if (!ignore_focus) {
        workspace_show(current_ws);
        if (dont_warp) {
            DLOG("x_set_warp_to(NULL) because dont_warp is set\n");
            x_set_warp_to(NULL);
        }
    }

    /* Set focus only if con was on current workspace before moving.
     * Otherwise we would give focus to some window on different workspace. */
    if (focus_next)
        con_activate(con_descend_focused(focus_next));

    /* 8. If anything within the container is associated with a startup sequence,
     * delete it so child windows won't be created on the old workspace. */
    if (!con_is_leaf(con)) {
        Con *child;
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            if (!child->window)
                continue;
            startup_sequence_delete_by_window(child->window);
        }
    }

    if (con->window) {
        startup_sequence_delete_by_window(con->window);
    }

    /* 9. If the container was marked urgent, move the urgency hint. */
    if (urgent) {
        workspace_update_urgent_flag(source_ws);
        con_set_urgency(con, true);
    }

    /* Ensure the container will be redrawn. */
    FREE(con->deco_render_params);

    CALL(parent, on_remove_child);

    ipc_send_window_event("move", con);
    ewmh_update_wm_desktop();
    return true;
}

/*
 * Moves the given container to the given mark.
 *
 */
bool con_move_to_mark(Con *con, const char *mark) {
    Con *target = con_by_mark(mark);
    if (target == NULL) {
        DLOG("found no container with mark \"%s\"\n", mark);
        return false;
    }

    /* For target containers in the scratchpad, we just send the window to the scratchpad. */
    if (con_get_workspace(target) == workspace_get("__i3_scratch")) {
        DLOG("target container is in the scratchpad, moving container to scratchpad.\n");
        scratchpad_move(con);
        return true;
    }

    /* For floating target containers, we just send the window to the same workspace. */
    if (con_is_floating(target)) {
        DLOG("target container is floating, moving container to target's workspace.\n");
        con_move_to_workspace(con, con_get_workspace(target), true, false, false);
        return true;
    }

    if (target->type == CT_WORKSPACE && con_is_leaf(target)) {
        DLOG("target container is an empty workspace, simply moving the container there.\n");
        con_move_to_workspace(con, target, true, false, false);
        return true;
    }

    /* For split containers, we use the currently focused container within it.
     * This allows setting marks on, e.g., tabbed containers which will move
     * con to a new tab behind the focused tab. */
    if (con_is_split(target)) {
        DLOG("target is a split container, descending to the currently focused child.\n");
        target = TAILQ_FIRST(&(target->focus_head));
    }

    if (con == target || con_has_parent(target, con)) {
        DLOG("cannot move the container to or inside itself, aborting.\n");
        return false;
    }

    return _con_move_to_con(con, target, false, true, false, false, true);
}

/*
 * Moves the given container to the currently focused container on the given
 * workspace.
 *
 * The fix_coordinates flag will translate the current coordinates (offset from
 * the monitor position basically) to appropriate coordinates on the
 * destination workspace.
 * Not enabling this behaviour comes in handy when this function gets called by
 * floating_maybe_reassign_ws, which will only "move" a floating window when it
 * *already* changed its coordinates to a different output.
 *
 * The dont_warp flag disables pointer warping and will be set when this
 * function is called while dragging a floating window.
 *
 * If ignore_focus is set, the container will be moved without modifying focus
 * at all.
 *
 * TODO: is there a better place for this function?
 *
 */
void con_move_to_workspace(Con *con, Con *workspace, bool fix_coordinates, bool dont_warp, bool ignore_focus) {
    assert(workspace->type == CT_WORKSPACE);

    Con *source_ws = con_get_workspace(con);
    if (workspace == source_ws) {
        DLOG("Not moving, already there\n");
        return;
    }

    Con *target = con_descend_focused(workspace);
    _con_move_to_con(con, target, true, fix_coordinates, dont_warp, ignore_focus, true);
}

/*
 * Moves the given container to the currently focused container on the
 * visible workspace on the given output.
 *
 */
void con_move_to_output(Con *con, Output *output, bool fix_coordinates) {
    Con *ws = NULL;
    GREP_FIRST(ws, output_get_content(output->con), workspace_is_visible(child));
    assert(ws != NULL);
    DLOG("Moving con %p to output %s\n", con, output_primary_name(output));
    con_move_to_workspace(con, ws, fix_coordinates, false, false);
}

/*
 * Moves the given container to the currently focused container on the
 * visible workspace on the output specified by the given name.
 * The current output for the container is used to resolve relative names
 * such as left, right, up, down.
 *
 */
bool con_move_to_output_name(Con *con, const char *name, bool fix_coordinates) {
    Output *current_output = get_output_for_con(con);
    Output *output = get_output_from_string(current_output, name);
    if (output == NULL) {
        ELOG("Could not find output \"%s\"\n", name);
        return false;
    }

    con_move_to_output(con, output, fix_coordinates);
    return true;
}

/*
 * Returns the orientation of the given container (for stacked containers,
 * vertical orientation is used regardless of the actual orientation of the
 * container).
 *
 */
orientation_t con_orientation(Con *con) {
    switch (con->layout) {
        case L_SPLITV:
        /* stacking containers behave like they are in vertical orientation */
        case L_STACKED:
            return VERT;

        case L_SPLITH:
        /* tabbed containers behave like they are in vertical orientation */
        case L_TABBED:
            return HORIZ;

        case L_DEFAULT:
            ELOG("Someone called con_orientation() on a con with L_DEFAULT, this is a bug in the code.\n");
            assert(false);

        case L_DOCKAREA:
        case L_OUTPUT:
            ELOG("con_orientation() called on dockarea/output (%d) container %p\n", con->layout, con);
            assert(false);
    }
    /* should not be reached */
    assert(false);
}

/*
 * Returns the container which will be focused next when the given container
 * is not available anymore. Called in tree_close_internal and con_move_to_workspace
 * to properly restore focus.
 *
 */
Con *con_next_focused(Con *con) {
    /* dock clients cannot be focused, so we focus the workspace instead */
    if (con->parent->type == CT_DOCKAREA) {
        DLOG("selecting workspace for dock client\n");
        return con_descend_focused(output_get_content(con->parent->parent));
    }
    if (con_is_floating(con)) {
        con = con->parent;
    }

    /* if 'con' is not the first entry in the focus stack, use the first one as
     * it’s currently focused already */
    Con *next = TAILQ_FIRST(&(con->parent->focus_head));
    if (next != con) {
        DLOG("Using first entry %p\n", next);
    } else {
        /* try to focus the next container on the same level as this one or fall
         * back to its parent */
        if (!(next = TAILQ_NEXT(con, focused))) {
            next = con->parent;
        }
    }

    /* now go down the focus stack as far as
     * possible, excluding the current container */
    while (!TAILQ_EMPTY(&(next->focus_head)) && TAILQ_FIRST(&(next->focus_head)) != con) {
        next = TAILQ_FIRST(&(next->focus_head));
    }

    if (con->type == CT_FLOATING_CON && next != con->parent) {
        next = con_descend_focused(next);
    }

    return next;
}

/*
 * Returns the focused con inside this client, descending the tree as far as
 * possible. This comes in handy when attaching a con to a workspace at the
 * currently focused position, for example.
 *
 */
Con *con_descend_focused(Con *con) {
    Con *next = con;
    while (next != focused && !TAILQ_EMPTY(&(next->focus_head)))
        next = TAILQ_FIRST(&(next->focus_head));
    return next;
}

/*
 * Returns the focused con inside this client, descending the tree as far as
 * possible. This comes in handy when attaching a con to a workspace at the
 * currently focused position, for example.
 *
 * Works like con_descend_focused but considers only tiling cons.
 *
 */
Con *con_descend_tiling_focused(Con *con) {
    Con *next = con;
    Con *before;
    Con *child;
    if (next == focused)
        return next;
    do {
        before = next;
        TAILQ_FOREACH (child, &(next->focus_head), focused) {
            if (child->type == CT_FLOATING_CON)
                continue;

            next = child;
            break;
        }
    } while (before != next && next != focused);
    return next;
}

/*
 * Returns the leftmost, rightmost, etc. container in sub-tree. For example, if
 * direction is D_LEFT, then we return the rightmost container and if direction
 * is D_RIGHT, we return the leftmost container.  This is because if we are
 * moving D_LEFT, and thus want the rightmost container.
 *
 */
Con *con_descend_direction(Con *con, direction_t direction) {
    Con *most = NULL;
    Con *current;
    int orientation = con_orientation(con);
    DLOG("con_descend_direction(%p, orientation %d, direction %d)\n", con, orientation, direction);
    if (direction == D_LEFT || direction == D_RIGHT) {
        if (orientation == HORIZ) {
            /* If the direction is horizontal, we can use either the first
             * (D_RIGHT) or the last con (D_LEFT) */
            if (direction == D_RIGHT)
                most = TAILQ_FIRST(&(con->nodes_head));
            else
                most = TAILQ_LAST(&(con->nodes_head), nodes_head);
        } else if (orientation == VERT) {
            /* Wrong orientation. We use the last focused con. Within that con,
             * we recurse to chose the left/right con or at least the last
             * focused one. */
            TAILQ_FOREACH (current, &(con->focus_head), focused) {
                if (current->type != CT_FLOATING_CON) {
                    most = current;
                    break;
                }
            }
        } else {
            /* If the con has no orientation set, it’s not a split container
             * but a container with a client window, so stop recursing */
            return con;
        }
    }

    if (direction == D_UP || direction == D_DOWN) {
        if (orientation == VERT) {
            /* If the direction is vertical, we can use either the first
             * (D_DOWN) or the last con (D_UP) */
            if (direction == D_UP)
                most = TAILQ_LAST(&(con->nodes_head), nodes_head);
            else
                most = TAILQ_FIRST(&(con->nodes_head));
        } else if (orientation == HORIZ) {
            /* Wrong orientation. We use the last focused con. Within that con,
             * we recurse to chose the top/bottom con or at least the last
             * focused one. */
            TAILQ_FOREACH (current, &(con->focus_head), focused) {
                if (current->type != CT_FLOATING_CON) {
                    most = current;
                    break;
                }
            }
        } else {
            /* If the con has no orientation set, it’s not a split container
             * but a container with a client window, so stop recursing */
            return con;
        }
    }

    if (!most)
        return con;
    return con_descend_direction(most, direction);
}

/*
 * Returns a "relative" Rect which contains the amount of pixels that need to
 * be added to the original Rect to get the final position (obviously the
 * amount of pixels for normal, 1pixel and borderless are different).
 *
 */
Rect con_border_style_rect(Con *con) {
    if (config.hide_edge_borders == HEBM_SMART && con_num_visible_children(con_get_workspace(con)) <= 1) {
        if (!con_is_floating(con)) {
            return (Rect){0, 0, 0, 0};
        }
    }

    adjacent_t borders_to_hide = ADJ_NONE;
    int border_width = con->current_border_width;
    DLOG("The border width for con is set to: %d\n", con->current_border_width);
    Rect result;
    if (con->current_border_width < 0) {
        if (con_is_floating(con)) {
            border_width = config.default_floating_border_width;
        } else {
            border_width = config.default_border_width;
        }
    }
    DLOG("Effective border width is set to: %d\n", border_width);
    /* Shortcut to avoid calling con_adjacent_borders() on dock containers. */
    int border_style = con_border_style(con);
    if (border_style == BS_NONE)
        return (Rect){0, 0, 0, 0};
    if (border_style == BS_NORMAL) {
        result = (Rect){border_width, 0, -(2 * border_width), -(border_width)};
    } else {
        result = (Rect){border_width, border_width, -(2 * border_width), -(2 * border_width)};
    }

    borders_to_hide = con_adjacent_borders(con) & config.hide_edge_borders;
    if (borders_to_hide & ADJ_LEFT_SCREEN_EDGE) {
        result.x -= border_width;
        result.width += border_width;
    }
    if (borders_to_hide & ADJ_RIGHT_SCREEN_EDGE) {
        result.width += border_width;
    }
    if (borders_to_hide & ADJ_UPPER_SCREEN_EDGE && (border_style != BS_NORMAL)) {
        result.y -= border_width;
        result.height += border_width;
    }
    if (borders_to_hide & ADJ_LOWER_SCREEN_EDGE) {
        result.height += border_width;
    }
    return result;
}

/*
 * Returns adjacent borders of the window. We need this if hide_edge_borders is
 * enabled.
 */
adjacent_t con_adjacent_borders(Con *con) {
    adjacent_t result = ADJ_NONE;
    /* Floating windows are never adjacent to any other window, so
       don’t hide their border(s). This prevents bug #998. */
    if (con_is_floating(con))
        return result;

    Con *workspace = con_get_workspace(con);
    if (con->rect.x == workspace->rect.x)
        result |= ADJ_LEFT_SCREEN_EDGE;
    if (con->rect.x + con->rect.width == workspace->rect.x + workspace->rect.width)
        result |= ADJ_RIGHT_SCREEN_EDGE;
    if (con->rect.y == workspace->rect.y)
        result |= ADJ_UPPER_SCREEN_EDGE;
    if (con->rect.y + con->rect.height == workspace->rect.y + workspace->rect.height)
        result |= ADJ_LOWER_SCREEN_EDGE;
    return result;
}

/*
 * Use this function to get a container’s border style. This is important
 * because when inside a stack, the border style is always BS_NORMAL.
 * For tabbed mode, the same applies, with one exception: when the container is
 * borderless and the only element in the tabbed container, the border is not
 * rendered.
 *
 * For children of a CT_DOCKAREA, the border style is always none.
 *
 */
int con_border_style(Con *con) {
    if (con->fullscreen_mode == CF_OUTPUT || con->fullscreen_mode == CF_GLOBAL) {
        DLOG("this one is fullscreen! overriding BS_NONE\n");
        return BS_NONE;
    }

    if (con->parent->layout == L_STACKED)
        return (con_num_children(con->parent) == 1 ? con->border_style : BS_NORMAL);

    if (con->parent->layout == L_TABBED && con->border_style != BS_NORMAL)
        return (con_num_children(con->parent) == 1 ? con->border_style : BS_NORMAL);

    if (con->parent->type == CT_DOCKAREA)
        return BS_NONE;

    return con->border_style;
}

/*
 * Sets the given border style on con, correctly keeping the position/size of a
 * floating window.
 *
 */
void con_set_border_style(Con *con, int border_style, int border_width) {
    /* Handle the simple case: non-floating containerns */
    if (!con_is_floating(con)) {
        con->border_style = border_style;
        con->current_border_width = border_width;
        return;
    }

    /* For floating containers, we want to keep the position/size of the
     * *window* itself. We first add the border pixels to con->rect to make
     * con->rect represent the absolute position of the window (same for
     * parent). Then, we change the border style and subtract the new border
     * pixels. For the parent, we do the same also for the decoration. */
    DLOG("This is a floating container\n");

    Con *parent = con->parent;
    Rect bsr = con_border_style_rect(con);
    int deco_height = (con->border_style == BS_NORMAL ? render_deco_height() : 0);

    con->rect = rect_add(con->rect, bsr);
    parent->rect = rect_add(parent->rect, bsr);
    parent->rect.y += deco_height;
    parent->rect.height -= deco_height;

    /* Change the border style, get new border/decoration values. */
    con->border_style = border_style;
    con->current_border_width = border_width;
    bsr = con_border_style_rect(con);
    deco_height = (con->border_style == BS_NORMAL ? render_deco_height() : 0);

    con->rect = rect_sub(con->rect, bsr);
    parent->rect = rect_sub(parent->rect, bsr);
    parent->rect.y -= deco_height;
    parent->rect.height += deco_height;
}

/*
 * This function changes the layout of a given container. Use it to handle
 * special cases like changing a whole workspace to stacked/tabbed (creates a
 * new split container before).
 *
 */
void con_set_layout(Con *con, layout_t layout) {
    DLOG("con_set_layout(%p, %d), con->type = %d\n",
         con, layout, con->type);

    /* Users can focus workspaces, but not any higher in the hierarchy.
     * Focus on the workspace is a special case, since in every other case, the
     * user means "change the layout of the parent split container". */
    if (con->type != CT_WORKSPACE)
        con = con->parent;

    /* We fill in last_split_layout when switching to a different layout
     * since there are many places in the code that don’t use
     * con_set_layout(). */
    if (con->layout == L_SPLITH || con->layout == L_SPLITV)
        con->last_split_layout = con->layout;

    /* When the container type is CT_WORKSPACE, the user wants to change the
     * whole workspace into stacked/tabbed mode. To do this and still allow
     * intuitive operations (like level-up and then opening a new window), we
     * need to create a new split container. */
    if (con->type == CT_WORKSPACE) {
        if (con_num_children(con) == 0) {
            layout_t ws_layout = (layout == L_STACKED || layout == L_TABBED) ? layout : L_DEFAULT;
            DLOG("Setting workspace_layout to %d\n", ws_layout);
            con->workspace_layout = ws_layout;
            DLOG("Setting layout to %d\n", layout);
            con->layout = layout;
        } else if (layout == L_STACKED || layout == L_TABBED || layout == L_SPLITV || layout == L_SPLITH) {
            DLOG("Creating new split container\n");
            /* 1: create a new split container */
            Con *new = con_new(NULL, NULL);
            new->parent = con;

            /* 2: Set the requested layout on the split container and mark it as
             * split. */
            new->layout = layout;
            new->last_split_layout = con->last_split_layout;

            /* 3: move the existing cons of this workspace below the new con */
            Con **focus_order = get_focus_order(con);

            DLOG("Moving cons\n");
            Con *child;
            while (!TAILQ_EMPTY(&(con->nodes_head))) {
                child = TAILQ_FIRST(&(con->nodes_head));
                con_detach(child);
                con_attach(child, new, true);
            }

            set_focus_order(new, focus_order);
            free(focus_order);

            /* 4: attach the new split container to the workspace */
            DLOG("Attaching new split to ws\n");
            con_attach(new, con, false);

            tree_flatten(croot);
            con_force_split_parents_redraw(con);
            return;
        }
    }

    if (layout == L_DEFAULT) {
        /* Special case: the layout formerly known as "default" (in combination
         * with an orientation). Since we switched to splith/splitv layouts,
         * using the "default" layout (which "only" should happen when using
         * legacy configs) is using the last split layout (either splith or
         * splitv) in order to still do the same thing. */
        con->layout = con->last_split_layout;
        /* In case last_split_layout was not initialized… */
        if (con->layout == L_DEFAULT)
            con->layout = L_SPLITH;
    } else {
        con->layout = layout;
    }
    con_force_split_parents_redraw(con);
}

/*
 * This function toggles the layout of a given container. toggle_mode can be
 * either 'default' (toggle only between stacked/tabbed/last_split_layout),
 * 'split' (toggle only between splitv/splith) or 'all' (toggle between all
 * layouts).
 *
 */
void con_toggle_layout(Con *con, const char *toggle_mode) {
    Con *parent = con;
    /* Users can focus workspaces, but not any higher in the hierarchy.
     * Focus on the workspace is a special case, since in every other case, the
     * user means "change the layout of the parent split container". */
    if (con->type != CT_WORKSPACE)
        parent = con->parent;
    DLOG("con_toggle_layout(%p, %s), parent = %p\n", con, toggle_mode, parent);

    const char delim[] = " ";

    if (strcasecmp(toggle_mode, "split") == 0 || strstr(toggle_mode, delim)) {
        /* L_DEFAULT is used as a placeholder value to distinguish if
         * the first layout has already been saved. (it can never be L_DEFAULT) */
        layout_t new_layout = L_DEFAULT;
        bool current_layout_found = false;
        char *tm_dup = sstrdup(toggle_mode);
        char *cur_tok = strtok(tm_dup, delim);

        for (layout_t layout; cur_tok != NULL; cur_tok = strtok(NULL, delim)) {
            if (strcasecmp(cur_tok, "split") == 0) {
                /* Toggle between splits. When the current layout is not a split
                 * layout, we just switch back to last_split_layout. Otherwise, we
                 * change to the opposite split layout. */
                if (parent->layout != L_SPLITH && parent->layout != L_SPLITV) {
                    layout = parent->last_split_layout;
                    /* In case last_split_layout was not initialized… */
                    if (layout == L_DEFAULT) {
                        layout = L_SPLITH;
                    }
                } else {
                    layout = (parent->layout == L_SPLITH) ? L_SPLITV : L_SPLITH;
                }
            } else {
                bool success = layout_from_name(cur_tok, &layout);
                if (!success || layout == L_DEFAULT) {
                    ELOG("The token '%s' was not recognized and has been skipped.\n", cur_tok);
                    continue;
                }
            }

            /* If none of the specified layouts match the current,
             * fall back to the first layout in the list */
            if (new_layout == L_DEFAULT) {
                new_layout = layout;
            }

            /* We found the active layout in the last iteration, so
             * now let's activate the current layout (next in list) */
            if (current_layout_found) {
                new_layout = layout;
                break;
            }

            if (parent->layout == layout) {
                current_layout_found = true;
            }
        }
        free(tm_dup);

        if (new_layout != L_DEFAULT) {
            con_set_layout(con, new_layout);
        }
    } else if (strcasecmp(toggle_mode, "all") == 0 || strcasecmp(toggle_mode, "default") == 0) {
        if (parent->layout == L_STACKED)
            con_set_layout(con, L_TABBED);
        else if (parent->layout == L_TABBED) {
            if (strcasecmp(toggle_mode, "all") == 0)
                con_set_layout(con, L_SPLITH);
            else
                con_set_layout(con, parent->last_split_layout);
        } else if (parent->layout == L_SPLITH || parent->layout == L_SPLITV) {
            if (strcasecmp(toggle_mode, "all") == 0) {
                /* When toggling through all modes, we toggle between
                 * splith/splitv, whereas normally we just directly jump to
                 * stacked. */
                if (parent->layout == L_SPLITH)
                    con_set_layout(con, L_SPLITV);
                else
                    con_set_layout(con, L_STACKED);
            } else {
                con_set_layout(con, L_STACKED);
            }
        }
    }
}

/*
 * Callback which will be called when removing a child from the given con.
 * Kills the container if it is empty and replaces it with the child if there
 * is exactly one child.
 *
 */
static void con_on_remove_child(Con *con) {
    DLOG("on_remove_child\n");

    /* Every container 'above' (in the hierarchy) the workspace content should
     * not be closed when the last child was removed */
    if (con->type == CT_OUTPUT ||
        con->type == CT_ROOT ||
        con->type == CT_DOCKAREA ||
        (con->parent != NULL && con->parent->type == CT_OUTPUT)) {
        DLOG("not handling, type = %d, name = %s\n", con->type, con->name);
        return;
    }

    /* For workspaces, close them only if they're not visible anymore */
    if (con->type == CT_WORKSPACE) {
        if (TAILQ_EMPTY(&(con->focus_head)) && !workspace_is_visible(con)) {
            LOG("Closing old workspace (%p / %s), it is empty\n", con, con->name);
            yajl_gen gen = ipc_marshal_workspace_event("empty", con, NULL);
            tree_close_internal(con, DONT_KILL_WINDOW, false);

            const unsigned char *payload;
            ylength length;
            y(get_buf, &payload, &length);
            ipc_send_event("workspace", I3_IPC_EVENT_WORKSPACE, (const char *)payload);

            y(free);
        }
        return;
    }

    con_force_split_parents_redraw(con);
    con->urgent = con_has_urgent_child(con);
    con_update_parents_urgency(con);

    /* TODO: check if this container would swallow any other client and
     * don’t close it automatically. */
    int children = con_num_children(con);
    if (children == 0) {
        DLOG("Container empty, closing\n");
        tree_close_internal(con, DONT_KILL_WINDOW, false);
        return;
    }
}

/*
 * Determines the minimum size of the given con by looking at its children (for
 * split/stacked/tabbed cons). Will be called when resizing floating cons
 *
 */
Rect con_minimum_size(Con *con) {
    DLOG("Determining minimum size for con %p\n", con);

    if (con_is_leaf(con)) {
        DLOG("leaf node, returning 75x50\n");
        return (Rect){0, 0, 75, 50};
    }

    if (con->type == CT_FLOATING_CON) {
        DLOG("floating con\n");
        Con *child = TAILQ_FIRST(&(con->nodes_head));
        return con_minimum_size(child);
    }

    if (con->layout == L_STACKED || con->layout == L_TABBED) {
        uint32_t max_width = 0, max_height = 0, deco_height = 0;
        Con *child;
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            Rect min = con_minimum_size(child);
            deco_height += child->deco_rect.height;
            max_width = max(max_width, min.width);
            max_height = max(max_height, min.height);
        }
        DLOG("stacked/tabbed now, returning %d x %d + deco_rect = %d\n",
             max_width, max_height, deco_height);
        return (Rect){0, 0, max_width, max_height + deco_height};
    }

    /* For horizontal/vertical split containers we sum up the width (h-split)
     * or height (v-split) and use the maximum of the height (h-split) or width
     * (v-split) as minimum size. */
    if (con_is_split(con)) {
        uint32_t width = 0, height = 0;
        Con *child;
        TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
            Rect min = con_minimum_size(child);
            if (con->layout == L_SPLITH) {
                width += min.width;
                height = max(height, min.height);
            } else {
                height += min.height;
                width = max(width, min.width);
            }
        }
        DLOG("split container, returning width = %d x height = %d\n", width, height);
        return (Rect){0, 0, width, height};
    }

    ELOG("Unhandled case, type = %d, layout = %d, split = %d\n",
         con->type, con->layout, con_is_split(con));
    assert(false);
}

/*
 * Returns true if changing the focus to con would be allowed considering
 * the fullscreen focus constraints. Specifically, if a fullscreen container or
 * any of its descendants is focused, this function returns true if and only if
 * focusing con would mean that focus would still be visible on screen, i.e.,
 * the newly focused container would not be obscured by a fullscreen container.
 *
 * In the simplest case, if a fullscreen container or any of its descendants is
 * fullscreen, this functions returns true if con is the fullscreen container
 * itself or any of its descendants, as this means focus wouldn't escape the
 * boundaries of the fullscreen container.
 *
 * In case the fullscreen container is of type CF_OUTPUT, this function returns
 * true if con is on a different workspace, as focus wouldn't be obscured by
 * the fullscreen container that is constrained to a different workspace.
 *
 * Note that this same logic can be applied to moving containers. If a
 * container can be focused under the fullscreen focus constraints, it can also
 * become a parent or sibling to the currently focused container.
 *
 */
bool con_fullscreen_permits_focusing(Con *con) {
    /* No focus, no problem. */
    if (!focused)
        return true;

    /* Find the first fullscreen ascendent. */
    Con *fs = focused;
    while (fs && fs->fullscreen_mode == CF_NONE)
        fs = fs->parent;

    /* fs must be non-NULL since the workspace con doesn’t have CF_NONE and
     * there always has to be a workspace con in the hierarchy. */
    assert(fs != NULL);
    /* The most common case is we hit the workspace level. In this
     * situation, changing focus is also harmless. */
    assert(fs->fullscreen_mode != CF_NONE);
    if (fs->type == CT_WORKSPACE)
        return true;

    /* Allow it if the container itself is the fullscreen container. */
    if (con == fs)
        return true;

    /* If fullscreen is per-output, the focus being in a different workspace is
     * sufficient to guarantee that change won't leave fullscreen in bad shape. */
    if (fs->fullscreen_mode == CF_OUTPUT &&
        con_get_workspace(con) != con_get_workspace(fs)) {
        return true;
    }

    /* Allow it only if the container to be focused is contained within the
     * current fullscreen container. */
    return con_has_parent(con, fs);
}

/*
 *
 * Checks if the given container has an urgent child.
 *
 */
bool con_has_urgent_child(Con *con) {
    Con *child;

    if (con_is_leaf(con))
        return con->urgent;

    /* We are not interested in floating windows since they can only be
     * attached to a workspace → nodes_head instead of focus_head */
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        if (con_has_urgent_child(child))
            return true;
    }

    return false;
}

/*
 * Make all parent containers urgent if con is urgent or clear the urgent flag
 * of all parent containers if there are no more urgent children left.
 *
 */
void con_update_parents_urgency(Con *con) {
    Con *parent = con->parent;

    /* Urgency hints should not be set on any container higher up in the
     * hierarchy than the workspace level. Unfortunately, since the content
     * container has type == CT_CON, that’s not easy to verify in the loop
     * below, so we need another condition to catch that case: */
    if (con->type == CT_WORKSPACE)
        return;

    bool new_urgency_value = con->urgent;
    while (parent && parent->type != CT_WORKSPACE && parent->type != CT_DOCKAREA) {
        if (new_urgency_value) {
            parent->urgent = true;
        } else {
            /* We can only reset the urgency when the parent
             * has no other urgent children */
            if (!con_has_urgent_child(parent))
                parent->urgent = false;
        }
        parent = parent->parent;
    }
}

/*
 * Set urgency flag to the container, all the parent containers and the workspace.
 *
 */
void con_set_urgency(Con *con, bool urgent) {
    if (urgent && focused == con) {
        DLOG("Ignoring urgency flag for current client\n");
        return;
    }

    const bool old_urgent = con->urgent;

    if (con->urgency_timer == NULL) {
        con->urgent = urgent;
    } else
        DLOG("Discarding urgency WM_HINT because timer is running\n");

    //CLIENT_LOG(con);
    if (con->window) {
        if (con->urgent) {
            gettimeofday(&con->window->urgent, NULL);
        } else {
            con->window->urgent.tv_sec = 0;
            con->window->urgent.tv_usec = 0;
        }
    }

    con_update_parents_urgency(con);

    Con *ws;
    /* Set the urgency flag on the workspace, if a workspace could be found
     * (for dock clients, that is not the case). */
    if ((ws = con_get_workspace(con)) != NULL)
        workspace_update_urgent_flag(ws);

    if (con->urgent != old_urgent) {
        LOG("Urgency flag changed to %d\n", con->urgent);
        ipc_send_window_event("urgent", con);
    }
}

/*
 * Create a string representing the subtree under con.
 *
 */
char *con_get_tree_representation(Con *con) {
    /* this code works as follows:
     *  1) create a string with the layout type (D/V/H/T/S) and an opening bracket
     *  2) append the tree representation of the children to the string
     *  3) add closing bracket
     *
     * The recursion ends when we hit a leaf, in which case we return the
     * class_instance of the contained window.
     */

    /* end of recursion */
    if (con_is_leaf(con)) {
        if (!con->window)
            return sstrdup("nowin");

        if (!con->window->class_instance)
            return sstrdup("noinstance");

        return sstrdup(con->window->class_instance);
    }

    char *buf;
    /* 1) add the Layout type to buf */
    if (con->layout == L_DEFAULT)
        buf = sstrdup("D[");
    else if (con->layout == L_SPLITV)
        buf = sstrdup("V[");
    else if (con->layout == L_SPLITH)
        buf = sstrdup("H[");
    else if (con->layout == L_TABBED)
        buf = sstrdup("T[");
    else if (con->layout == L_STACKED)
        buf = sstrdup("S[");
    else {
        ELOG("BUG: Code not updated to account for new layout type\n");
        assert(false);
    }

    /* 2) append representation of children */
    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        char *child_txt = con_get_tree_representation(child);

        char *tmp_buf;
        sasprintf(&tmp_buf, "%s%s%s", buf,
                  (TAILQ_FIRST(&(con->nodes_head)) == child ? "" : " "), child_txt);
        free(buf);
        buf = tmp_buf;
        free(child_txt);
    }

    /* 3) close the brackets */
    char *complete_buf;
    sasprintf(&complete_buf, "%s]", buf);
    free(buf);

    return complete_buf;
}

/*
 * Returns the container's title considering the current title format.
 *
 */
i3String *con_parse_title_format(Con *con) {
    assert(con->title_format != NULL);

    i3Window *win = con->window;

    /* We need to ensure that we only escape the window title if pango
     * is used by the current font. */
    const bool pango_markup = font_is_pango();

    char *title;
    char *class;
    char *instance;
    char *machine;
    if (win == NULL) {
        title = pango_escape_markup(con_get_tree_representation(con));
        class = sstrdup("i3-frame");
        instance = sstrdup("i3-frame");
        machine = sstrdup("");
    } else {
        title = pango_escape_markup(sstrdup((win->name == NULL) ? "" : i3string_as_utf8(win->name)));
        class = pango_escape_markup(sstrdup((win->class_class == NULL) ? "" : win->class_class));
        instance = pango_escape_markup(sstrdup((win->class_instance == NULL) ? "" : win->class_instance));
        machine = pango_escape_markup(sstrdup((win->machine == NULL) ? "" : win->machine));
    }

    placeholder_t placeholders[] = {
        {.name = "%title", .value = title},
        {.name = "%class", .value = class},
        {.name = "%instance", .value = instance},
        {.name = "%machine", .value = machine},
    };
    const size_t num = sizeof(placeholders) / sizeof(placeholder_t);

    char *formatted_str = format_placeholders(con->title_format, &placeholders[0], num);
    i3String *formatted = i3string_from_utf8(formatted_str);
    i3string_set_markup(formatted, pango_markup);

    free(formatted_str);
    free(title);
    free(class);
    free(instance);

    return formatted;
}

/*
 * Swaps the two containers.
 *
 */
bool con_swap(Con *first, Con *second) {
    assert(first != NULL);
    assert(second != NULL);
    DLOG("Swapping containers %p / %p\n", first, second);

    if (first->type != CT_CON) {
        ELOG("Only regular containers can be swapped, but found con = %p with type = %d.\n", first, first->type);
        return false;
    }

    if (second->type != CT_CON) {
        ELOG("Only regular containers can be swapped, but found con = %p with type = %d.\n", second, second->type);
        return false;
    }

    if (first == second) {
        DLOG("Swapping container %p with itself, nothing to do.\n", first);
        return false;
    }

    if (con_has_parent(first, second) || con_has_parent(second, first)) {
        ELOG("Cannot swap containers %p and %p because they are in a parent-child relationship.\n", first, second);
        return false;
    }

    Con *ws1 = con_get_workspace(first);
    Con *ws2 = con_get_workspace(second);
    Con *restore_focus = NULL;
    if (ws1 == ws2 && ws1 == con_get_workspace(focused)) {
        /* Preserve focus in the current workspace. */
        restore_focus = focused;
    } else if (first == focused || con_has_parent(focused, first)) {
        restore_focus = second;
    } else if (second == focused || con_has_parent(focused, second)) {
        restore_focus = first;
    }

#define SWAP_CONS_IN_TREE(headname, field)                            \
    do {                                                              \
        struct headname *head1 = &(first->parent->headname);          \
        struct headname *head2 = &(second->parent->headname);         \
        Con *first_prev = TAILQ_PREV(first, headname, field);         \
        Con *second_prev = TAILQ_PREV(second, headname, field);       \
        if (second_prev == first) {                                   \
            TAILQ_SWAP(first, second, head1, field);                  \
        } else if (first_prev == second) {                            \
            TAILQ_SWAP(second, first, head1, field);                  \
        } else {                                                      \
            TAILQ_REMOVE(head1, first, field);                        \
            TAILQ_REMOVE(head2, second, field);                       \
            if (second_prev == NULL) {                                \
                TAILQ_INSERT_HEAD(head2, first, field);               \
            } else {                                                  \
                TAILQ_INSERT_AFTER(head2, second_prev, first, field); \
            }                                                         \
            if (first_prev == NULL) {                                 \
                TAILQ_INSERT_HEAD(head1, second, field);              \
            } else {                                                  \
                TAILQ_INSERT_AFTER(head1, first_prev, second, field); \
            }                                                         \
        }                                                             \
    } while (0)

    SWAP_CONS_IN_TREE(nodes_head, nodes);
    SWAP_CONS_IN_TREE(focus_head, focused);
    SWAP(first->parent, second->parent, Con *);

    /* Floating nodes are children of CT_FLOATING_CONs, they are listed in
     * nodes_head and focus_head like all other containers. Thus, we don't need
     * to do anything special other than swapping the floating status and the
     * relevant rects. */
    SWAP(first->floating, second->floating, int);
    SWAP(first->rect, second->rect, Rect);
    SWAP(first->window_rect, second->window_rect, Rect);

    /* We need to copy each other's percentages to ensure that the geometry
     * doesn't change during the swap. */
    SWAP(first->percent, second->percent, double);

    if (restore_focus) {
        con_focus(restore_focus);
    }

    /* Update new parents' & workspaces' urgency. */
    con_set_urgency(first, first->urgent);
    con_set_urgency(second, second->urgent);

    /* Exchange fullscreen modes, can't use SWAP because we need to call the
     * correct functions. */
    fullscreen_mode_t second_fullscreen_mode = second->fullscreen_mode;
    if (first->fullscreen_mode == CF_NONE) {
        con_disable_fullscreen(second);
    } else {
        con_enable_fullscreen(second, first->fullscreen_mode);
    }
    if (second_fullscreen_mode == CF_NONE) {
        con_disable_fullscreen(first);
    } else {
        con_enable_fullscreen(first, second_fullscreen_mode);
    }

    /* We don't actually need this since percentages-wise we haven't changed
     * anything, but we'll better be safe than sorry and just make sure as we'd
     * otherwise crash i3. */
    con_fix_percent(first->parent);
    con_fix_percent(second->parent);

    FREE(first->deco_render_params);
    FREE(second->deco_render_params);
    con_force_split_parents_redraw(first);
    con_force_split_parents_redraw(second);

    return true;
}

/*
 * Returns container's rect size depending on its orientation.
 * i.e. its width when horizontal, its height when vertical.
 *
 */
uint32_t con_rect_size_in_orientation(Con *con) {
    return (con_orientation(con) == HORIZ ? con->rect.width : con->rect.height);
}

/*
 * Merges container specific data that should move with the window (e.g. marks,
 * title format, and the window itself) into another container, and closes the
 * old container.
 *
 */
void con_merge_into(Con *old, Con *new) {
    new->window = old->window;
    old->window = NULL;

    if (old->title_format) {
        FREE(new->title_format);
        new->title_format = old->title_format;
        old->title_format = NULL;
    }

    if (old->sticky_group) {
        FREE(new->sticky_group);
        new->sticky_group = old->sticky_group;
        old->sticky_group = NULL;
    }

    new->sticky = old->sticky;

    con_set_urgency(new, old->urgent);

    mark_t *mark;
    TAILQ_FOREACH (mark, &(old->marks_head), marks) {
        TAILQ_INSERT_TAIL(&(new->marks_head), mark, marks);
        ipc_send_window_event("mark", new);
    }
    new->mark_changed = (TAILQ_FIRST(&(old->marks_head)) != NULL);
    TAILQ_INIT(&(old->marks_head));

    tree_close_internal(old, DONT_KILL_WINDOW, false);
}
