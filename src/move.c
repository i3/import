/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * move.c: Moving containers into some direction.
 *
 */
#include "all.h"

/*
 * Returns the lowest container in the tree that has both a and b as descendants.
 *
 */
static Con *lowest_common_ancestor(Con *a, Con *b) {
    Con *parent_a = a;
    while (parent_a) {
        Con *parent_b = b;
        while (parent_b) {
            if (parent_a == parent_b) {
                return parent_a;
            }
            parent_b = parent_b->parent;
        }
        parent_a = parent_a->parent;
    }
    assert(false);
}

/*
 * Returns the direct child of ancestor that contains con.
 *
 */
static Con *child_containing_con_recursively(Con *ancestor, Con *con) {
    Con *child = con;
    while (child && child->parent != ancestor) {
        child = child->parent;
        assert(child->parent);
    }
    return child;
}

/*
 * Returns true if the given container is the focused descendant of ancestor, recursively.
 *
 */
static bool is_focused_descendant(Con *con, Con *ancestor) {
    Con *current = con;
    while (current != ancestor) {
        if (TAILQ_FIRST(&(current->parent->focus_head)) != current) {
            return false;
        }
        current = current->parent;
        assert(current->parent);
    }
    return true;
}

/*
 * This function detaches 'con' from its parent and inserts it either before or
 * after 'target'.
 *
 */
void insert_con_into(Con *con, Con *target, position_t position) {
    Con *parent = target->parent;
    /* We need to preserve the old con->parent. While it might still be used to
     * insert the entry before/after it, we call the on_remove_child callback
     * afterwards which might then close the con if it is empty. */
    Con *old_parent = con->parent;

    /* We compare the focus order of the children of the lowest common ancestor. If con or
     * its ancestor is before target's ancestor then con should be placed before the target
     * in the focus stack. */
    Con *lca = lowest_common_ancestor(con, parent);
    if (lca == con) {
        ELOG("Container is being inserted into one of its descendants.\n");
        return;
    }

    Con *con_ancestor = child_containing_con_recursively(lca, con);
    Con *target_ancestor = child_containing_con_recursively(lca, target);
    bool moves_focus_from_ancestor = is_focused_descendant(con, con_ancestor);
    bool focus_before;

    /* Determine if con is going to be placed before or after target in the parent's focus stack. */
    if (con_ancestor == target_ancestor) {
        /* Happens when the target is con's old parent. Eg with layout V [ A H [ B C ] ],
         * if we move C up. Target will be H. */
        focus_before = moves_focus_from_ancestor;
    } else {
        /* Look at the focus stack order of the children of the lowest common ancestor. */
        Con *current;
        TAILQ_FOREACH (current, &(lca->focus_head), focused) {
            if (current == con_ancestor || current == target_ancestor) {
                break;
            }
        }
        focus_before = (current == con_ancestor);
    }

    /* If con is the focused container in our old ancestor we place the new ancestor
     * before the old ancestor in the focus stack. Example:
     * Consider the layout [ H [ V1 [ A* B ] V2 [ C ] ] ] where A is focused. We move to
     * a second workspace and from there we move A to the right and switch back to the
     * original workspace. Without the change focus would move to B instead of staying
     * with A. */
    if (moves_focus_from_ancestor && focus_before) {
        Con *place = TAILQ_PREV(con_ancestor, focus_head, focused);
        TAILQ_REMOVE(&(lca->focus_head), target_ancestor, focused);
        if (place) {
            TAILQ_INSERT_AFTER(&(lca->focus_head), place, target_ancestor, focused);
        } else {
            TAILQ_INSERT_HEAD(&(lca->focus_head), target_ancestor, focused);
        }
    }

    con_detach(con);
    con_fix_percent(con->parent);

    /* When moving to a workspace, we respect the user’s configured
     * workspace_layout */
    if (parent->type == CT_WORKSPACE) {
        Con *split = workspace_attach_to(parent);
        if (split != parent) {
            DLOG("Got a new split con, using that one instead\n");
            con->parent = split;
            con_attach(con, split, false);
            DLOG("attached\n");
            con->percent = 0.0;
            con_fix_percent(split);
            con = split;
            DLOG("ok, continuing with con %p instead\n", con);
            con_detach(con);
        }
    }

    con->parent = parent;

    if (parent == lca) {
        if (focus_before) {
            /* Example layout: H [ A B* ], we move A up/down. 'target' will be H. */
            TAILQ_INSERT_BEFORE(target, con, focused);
        } else {
            /* Example layout: H [ A B* ], we move A up/down. 'target' will be H. */
            TAILQ_INSERT_AFTER(&(parent->focus_head), target, con, focused);
        }
    } else {
        if (focus_before) {
            /* Example layout: V [ H [ A B ] C* ], we move C up. 'target' will be A. */
            TAILQ_INSERT_HEAD(&(parent->focus_head), con, focused);
        } else {
            /* Example layout: V [ H [ A* B ] C ], we move C up. 'target' will be A. */
            TAILQ_INSERT_TAIL(&(parent->focus_head), con, focused);
        }
    }

    if (position == BEFORE) {
        TAILQ_INSERT_BEFORE(target, con, nodes);
    } else if (position == AFTER) {
        TAILQ_INSERT_AFTER(&(parent->nodes_head), target, con, nodes);
    }

    /* Pretend the con was just opened with regards to size percent values.
     * Since the con is moved to a completely different con, the old value
     * does not make sense anyways. */
    con->percent = 0.0;
    con_fix_percent(parent);

    CALL(old_parent, on_remove_child);
}

/*
 * This function detaches 'con' from its parent and puts it in the given
 * workspace. Position is determined by the direction of movement into the
 * workspace container.
 *
 */
static void attach_to_workspace(Con *con, Con *ws, direction_t direction) {
    con_detach(con);
    Con *old_parent = con->parent;
    con->parent = ws;

    if (direction == D_RIGHT || direction == D_DOWN) {
        TAILQ_INSERT_HEAD(&(ws->nodes_head), con, nodes);
    } else {
        TAILQ_INSERT_TAIL(&(ws->nodes_head), con, nodes);
    }
    TAILQ_INSERT_TAIL(&(ws->focus_head), con, focused);

    /* Pretend the con was just opened with regards to size percent values.
     * Since the con is moved to a completely different con, the old value
     * does not make sense anyways. */
    con->percent = 0.0;
    con_fix_percent(ws);

    con_fix_percent(old_parent);
    CALL(old_parent, on_remove_child);
}

/*
 * Moves the given container to the closest output in the given direction if
 * such an output exists.
 *
 */
static void move_to_output_directed(Con *con, direction_t direction) {
    Output *current_output = get_output_for_con(con);
    Output *output = get_output_next(direction, current_output, CLOSEST_OUTPUT);

    if (!output) {
        DLOG("No output in this direction found. Not moving.\n");
        return;
    }

    Con *ws = NULL;
    GREP_FIRST(ws, output_get_content(output->con), workspace_is_visible(child));

    if (!ws) {
        DLOG("No workspace on output in this direction found. Not moving.\n");
        return;
    }

    Con *old_ws = con_get_workspace(con);
    const bool moves_focus = (focused == con);
    attach_to_workspace(con, ws, direction);
    if (moves_focus) {
        /* workspace_show will not correctly update the active workspace because
         * the focused container, con, is now a child of ws. To work around this
         * and still produce the correct workspace focus events (see
         * 517-regress-move-direction-ipc.t) we need to temporarily set focused
         * to the old workspace. */
        focused = old_ws;
        workspace_show(ws);
        con_focus(con);
    }

    /* force re-painting the indicators */
    FREE(con->deco_render_params);

    ipc_send_window_event("move", con);
    tree_flatten(croot);
    ewmh_update_wm_desktop();
}

/*
 * Figures out how to insert a con into a another con in relation to the
 * targetted child node based on the target layout fill order and direction from
 * which it will be inserted.
 *
 */
static position_t tree_insert_position_from_direction(Con *target, direction_t direction) {
    orientation_t o = orientation_from_direction(direction);
    orientation_t target_orientation = con_orientation(target->parent);

    if ((target_orientation == o && (direction == D_LEFT || direction == D_UP)) ||
        (target_orientation != o && target->parent->layout_fill_order == LF_DEFAULT)) {
        return AFTER;
    } else {
        return BEFORE;
    }
}

/*
 * Moves the given container in the given direction
 *
 */
void tree_move(Con *con, direction_t direction) {
    position_t position;
    Con *target;

    DLOG("Moving in direction %d\n", direction);

    /* 1: get the first parent with the same orientation */

    if (con->type == CT_WORKSPACE) {
        DLOG("Not moving workspace\n");
        return;
    }

    if (con->fullscreen_mode == CF_GLOBAL) {
        DLOG("Not moving fullscreen global container\n");
        return;
    }

    if ((con->fullscreen_mode == CF_OUTPUT) ||
        (con->parent->type == CT_WORKSPACE && con_num_children(con->parent) == 1)) {
        /* This is the only con on this workspace */
        move_to_output_directed(con, direction);
        return;
    }

    orientation_t o = orientation_from_direction(direction);

    Con *same_orientation = con_parent_with_orientation(con, o);
    /* The do {} while is used to 'restart' at this point with a different
     * same_orientation, see the very last lines before the end of this block
     * */
    do {
        /* There is no parent container with the same orientation */
        if (!same_orientation) {
            if (con_is_floating(con)) {
                /* this is a floating con, we just disable floating */
                floating_disable(con);
                return;
            }
            if (con_inside_floating(con)) {
                /* 'con' should be moved out of a floating container */
                DLOG("Inside floating, moving to workspace\n");
                attach_to_workspace(con, con_get_workspace(con), direction);
                goto end;
            }
            DLOG("Force-changing orientation\n");
            ws_force_orientation(con_get_workspace(con), o);
            same_orientation = con_parent_with_orientation(con, o);
        }

        /* easy case: the move is within this container */
        if (same_orientation == con->parent) {
            Con *swap = (direction == D_LEFT || direction == D_UP)
                            ? TAILQ_PREV(con, nodes_head, nodes)
                            : TAILQ_NEXT(con, nodes);
            if (swap) {
                if (!con_is_leaf(swap)) {
                    DLOG("Moving into our bordering branch\n");
                    target = con_descend_direction(swap, direction);
                    position = tree_insert_position_from_direction(target, direction);
                    insert_con_into(con, target, position);
                    goto end;
                }

                DLOG("Swapping with sibling.\n");
                if (direction == D_LEFT || direction == D_UP) {
                    TAILQ_SWAP(swap, con, &(swap->parent->nodes_head), nodes);
                } else {
                    TAILQ_SWAP(con, swap, &(swap->parent->nodes_head), nodes);
                }

                ipc_send_window_event("move", con);
                return;
            }

            if (con->parent == con_get_workspace(con)) {
                /* If we couldn't find a place to move it on this workspace, try
                 * to move it to a workspace on a different output */
                move_to_output_directed(con, direction);
                return;
            }

            /* If there was no con with which we could swap the current one,
             * search again, but starting one level higher. */
            same_orientation = con_parent_with_orientation(con->parent, o);
        }
    } while (same_orientation == NULL);

    /* this time, we have to move to another container */
    /* This is the container *above* 'con' (an ancestor of con) which is inside
     * 'same_orientation' */
    Con *above = con;
    while (above->parent != same_orientation)
        above = above->parent;

    /* Enforce the fullscreen focus restrictions. */
    if (!con_fullscreen_permits_focusing(above->parent)) {
        LOG("Cannot move out of fullscreen container\n");
        return;
    }

    DLOG("above = %p\n", above);

    Con *next = (direction == D_UP || direction == D_LEFT ? TAILQ_PREV(above, nodes_head, nodes) : TAILQ_NEXT(above, nodes));

    if (next && !con_is_leaf(next)) {
        DLOG("Moving into the bordering branch of our adjacent container\n");
        target = con_descend_direction(next, direction);
        position = tree_insert_position_from_direction(target, direction);
        insert_con_into(con, target, position);
    } else if (!next &&
               con->parent->parent->type == CT_WORKSPACE &&
               con->parent->layout != L_DEFAULT &&
               con_num_children(con->parent) == 1) {
        /* Con is the lone child of a non-default layout container at the edge
         * of the workspace. Treat it as though the workspace is its parent
         * and move it to the next output. */
        DLOG("Grandparent is workspace\n");
        move_to_output_directed(con, direction);
        return;
    } else {
        DLOG("Moving into container above\n");
        position = (direction == D_UP || direction == D_LEFT ? BEFORE : AFTER);
        insert_con_into(con, above, position);
    }

end:
    /* force re-painting the indicators */
    FREE(con->deco_render_params);

    ipc_send_window_event("move", con);
    tree_flatten(croot);
    ewmh_update_wm_desktop();
}
