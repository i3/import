/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * workspace.c: Modifying workspaces, accessing them, moving containers to
 *              workspaces.
 *
 */
#include "all.h"
#include "yajl_utils.h"

/*
 * Stores a copy of the name of the last used workspace for the workspace
 * back-and-forth switching.
 *
 */
char *previous_workspace_name = NULL;

/* NULL-terminated list of workspace names (in order) extracted from
 * keybindings. */
static char **binding_workspace_names = NULL;

/*
 * Returns the workspace with the given name or NULL if such a workspace does
 * not exist.
 *
 */
Con *get_existing_workspace_by_name(const char *name) {
    Con *output, *workspace = NULL;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        GREP_FIRST(workspace, output_get_content(output), !strcasecmp(child->name, name));
    }

    return workspace;
}

/*
 * Returns the workspace with the given number or NULL if such a workspace does
 * not exist.
 *
 */
Con *get_existing_workspace_by_num(int num) {
    Con *output, *workspace = NULL;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        GREP_FIRST(workspace, output_get_content(output), child->num == num);
    }

    return workspace;
}

/*
 * Sets ws->layout to splith/splitv if default_orientation was specified in the
 * configfile. Otherwise, it uses splith/splitv depending on whether the output
 * is higher than wide.
 *
 */
static void _workspace_apply_default_orientation(Con *ws) {
    /* If default_orientation is set to NO_ORIENTATION we determine
     * orientation depending on output resolution. */
    if (config.default_orientation == NO_ORIENTATION) {
        Con *output = con_get_output(ws);
        ws->layout = (output->rect.height > output->rect.width) ? L_SPLITV : L_SPLITH;
        ws->rect = output->rect;
        DLOG("Auto orientation. Workspace size set to (%d,%d), setting layout to %d.\n",
             output->rect.width, output->rect.height, ws->layout);
    } else {
        ws->layout = (config.default_orientation == HORIZ) ? L_SPLITH : L_SPLITV;
    }
}

/*
 * Returns the first output that is assigned to a workspace specified by the
 * given name or number. Returns NULL if no such output exists.
 *
 * If an assignment matches by number but there is an assignment later that
 * matches by name, the second one is preferred.
 * The order of the 'ws_assignments' queue is respected: if multiple
 * assignments match the criteria, the first one is returned.
 * 'name' is ignored when NULL, 'parsed_num' is ignored when it is -1.
 *
 */
Con *get_assigned_output(const char *name, long parsed_num) {
    Con *output = NULL;
    struct Workspace_Assignment *assignment;
    TAILQ_FOREACH (assignment, &ws_assignments, ws_assignments) {
        if (name && strcmp(assignment->name, name) == 0) {
            DLOG("Found workspace name=\"%s\" assignment to output \"%s\"\n",
                 name, assignment->output);
            Output *assigned_by_name = get_output_by_name(assignment->output, true);
            if (assigned_by_name) {
                /* When the name matches exactly, skip numbered assignments. */
                return assigned_by_name->con;
            }
        } else if (!output && /* Only keep the first numbered assignment. */
                   parsed_num != -1 &&
                   name_is_digits(assignment->name) &&
                   ws_name_to_number(assignment->name) == parsed_num) {
            DLOG("Found workspace number=%ld assignment to output \"%s\"\n",
                 parsed_num, assignment->output);
            Output *assigned_by_num = get_output_by_name(assignment->output, true);
            if (assigned_by_num) {
                output = assigned_by_num->con;
            }
        }
    }

    return output;
}

/*
 * Returns true if the first output assigned to a workspace with the given
 * workspace assignment is the same as the given output.
 */
bool output_triggers_assignment(Output *output, struct Workspace_Assignment *assignment) {
    Con *assigned = get_assigned_output(assignment->name, -1);
    return assigned && assigned == output->con;
}

/*
 * Returns a pointer to the workspace with the given number (starting at 0),
 * creating the workspace if necessary (by allocating the necessary amount of
 * memory and initializing the data structures correctly).
 *
 */
Con *workspace_get(const char *num) {
    Con *workspace = get_existing_workspace_by_name(num);
    if (workspace) {
        return workspace;
    }

    LOG("Creating new workspace \"%s\"\n", num);

    /* We set workspace->num to the number if this workspace’s name begins with
     * a positive number. Otherwise it’s a named ws and num will be 1. */
    const int parsed_num = ws_name_to_number(num);

    Con *output = get_assigned_output(num, parsed_num);
    /* if an assignment is not found, we create this workspace on the current output */
    if (!output) {
        output = con_get_output(focused);
    }

    /* No parent because we need to attach this container after setting its
     * type. con_attach will handle CT_WORKSPACEs differently. */
    workspace = con_new(NULL, NULL);

    char *name;
    sasprintf(&name, "[i3 con] workspace %s", num);
    x_set_name(workspace, name);
    free(name);

    FREE(workspace->name);
    workspace->name = sstrdup(num);
    workspace->workspace_layout = config.default_layout;
    workspace->num = parsed_num;
    workspace->type = CT_WORKSPACE;

    con_attach(workspace, output_get_content(output), false);
    _workspace_apply_default_orientation(workspace);

    ipc_send_workspace_event("init", workspace, NULL);
    ewmh_update_desktop_properties();

    return workspace;
}

/*
 * Extracts workspace names from keybindings (e.g. “web” from “bindsym $mod+1
 * workspace web”), so that when an output needs a workspace, i3 can start with
 * the first configured one. Needs to be called before reorder_bindings() so
 * that the config-file order is used, not the i3-internal order.
 *
 */
void extract_workspace_names_from_bindings(void) {
    Binding *bind;
    int n = 0;
    if (binding_workspace_names != NULL) {
        for (int i = 0; binding_workspace_names[i] != NULL; i++) {
            free(binding_workspace_names[i]);
        }
        FREE(binding_workspace_names);
    }
    TAILQ_FOREACH (bind, bindings, bindings) {
        DLOG("binding with command %s\n", bind->command);
        if (strlen(bind->command) < strlen("workspace ") ||
            strncasecmp(bind->command, "workspace", strlen("workspace")) != 0)
            continue;
        DLOG("relevant command = %s\n", bind->command);
        const char *target = bind->command + strlen("workspace ");
        while (*target == ' ' || *target == '\t')
            target++;
        /* We check if this is the workspace
         * next/prev/next_on_output/prev_on_output/back_and_forth command.
         * Beware: The workspace names "next", "prev", "next_on_output",
         * "prev_on_output", "back_and_forth" and "current" are OK,
         * so we check before stripping the double quotes */
        if (strncasecmp(target, "next", strlen("next")) == 0 ||
            strncasecmp(target, "prev", strlen("prev")) == 0 ||
            strncasecmp(target, "next_on_output", strlen("next_on_output")) == 0 ||
            strncasecmp(target, "prev_on_output", strlen("prev_on_output")) == 0 ||
            strncasecmp(target, "back_and_forth", strlen("back_and_forth")) == 0 ||
            strncasecmp(target, "current", strlen("current")) == 0)
            continue;
        if (strncasecmp(target, "--no-auto-back-and-forth", strlen("--no-auto-back-and-forth")) == 0) {
            target += strlen("--no-auto-back-and-forth");
            while (*target == ' ' || *target == '\t')
                target++;
        }
        if (strncasecmp(target, "number", strlen("number")) == 0) {
            target += strlen("number");
            while (*target == ' ' || *target == '\t')
                target++;
        }
        char *target_name = parse_string(&target, false);
        if (target_name == NULL)
            continue;
        if (strncasecmp(target_name, "__", strlen("__")) == 0) {
            LOG("Cannot create workspace \"%s\". Names starting with __ are i3-internal.\n", target);
            free(target_name);
            continue;
        }
        DLOG("Saving workspace name \"%s\"\n", target_name);

        binding_workspace_names = srealloc(binding_workspace_names, ++n * sizeof(char *));
        binding_workspace_names[n - 1] = target_name;
    }
    binding_workspace_names = srealloc(binding_workspace_names, ++n * sizeof(char *));
    binding_workspace_names[n - 1] = NULL;
}

/*
 * Returns a pointer to a new workspace in the given output. The workspace
 * is created attached to the tree hierarchy through the given content
 * container.
 *
 */
Con *create_workspace_on_output(Output *output, Con *content) {
    /* add a workspace to this output */
    bool exists = true;
    Con *ws = con_new(NULL, NULL);
    ws->type = CT_WORKSPACE;

    /* try the configured workspace bindings first to find a free name */
    for (int n = 0; binding_workspace_names[n] != NULL; n++) {
        char *target_name = binding_workspace_names[n];
        /* Ensure that this workspace is not assigned to a different output —
         * otherwise we would create it, then move it over to its output, then
         * find a new workspace, etc… */
        Con *assigned = get_assigned_output(target_name, -1);
        if (assigned && assigned != output->con) {
            continue;
        }

        const int num = ws_name_to_number(target_name);
        exists = (num == -1)
                     ? get_existing_workspace_by_name(target_name)
                     : get_existing_workspace_by_num(num);
        if (!exists) {
            ws->name = sstrdup(target_name);
            /* Set ->num to the number of the workspace, if the name actually
             * is a number or starts with a number */
            ws->num = num;
            DLOG("Used number %d for workspace with name %s\n", ws->num, ws->name);

            break;
        }
    }

    if (exists) {
        /* get the next unused workspace number */
        DLOG("Getting next unused workspace by number\n");
        int c = 0;
        while (exists) {
            c++;
            Con *assigned = get_assigned_output(NULL, c);
            exists = (get_existing_workspace_by_num(c) || (assigned && assigned != output->con));
            DLOG("result for ws %d: exists = %d\n", c, exists);
        }
        ws->num = c;
        sasprintf(&(ws->name), "%d", c);
    }
    con_attach(ws, content, false);

    char *name;
    sasprintf(&name, "[i3 con] workspace %s", ws->name);
    x_set_name(ws, name);
    free(name);

    ws->fullscreen_mode = CF_OUTPUT;

    ws->workspace_layout = config.default_layout;
    _workspace_apply_default_orientation(ws);

    ipc_send_workspace_event("init", ws, NULL);
    return ws;
}

/*
 * Returns true if the workspace is currently visible. Especially important for
 * multi-monitor environments, as they can have multiple currenlty active
 * workspaces.
 *
 */
bool workspace_is_visible(Con *ws) {
    Con *output = con_get_output(ws);
    if (output == NULL)
        return false;
    Con *fs = con_get_fullscreen_con(output, CF_OUTPUT);
    LOG("workspace visible? fs = %p, ws = %p\n", fs, ws);
    return (fs == ws);
}

/*
 * XXX: we need to clean up all this recursive walking code.
 *
 */
static Con *_get_sticky(Con *con, const char *sticky_group, Con *exclude) {
    Con *current;

    TAILQ_FOREACH (current, &(con->nodes_head), nodes) {
        if (current != exclude &&
            current->sticky_group != NULL &&
            current->window != NULL &&
            strcmp(current->sticky_group, sticky_group) == 0)
            return current;

        Con *recurse = _get_sticky(current, sticky_group, exclude);
        if (recurse != NULL)
            return recurse;
    }

    TAILQ_FOREACH (current, &(con->floating_head), floating_windows) {
        if (current != exclude &&
            current->sticky_group != NULL &&
            current->window != NULL &&
            strcmp(current->sticky_group, sticky_group) == 0)
            return current;

        Con *recurse = _get_sticky(current, sticky_group, exclude);
        if (recurse != NULL)
            return recurse;
    }

    return NULL;
}

/*
 * Reassigns all child windows in sticky containers. Called when the user
 * changes workspaces.
 *
 * XXX: what about sticky containers which contain containers?
 *
 */
static void workspace_reassign_sticky(Con *con) {
    Con *current;
    /* 1: go through all containers */

    /* handle all children and floating windows of this node */
    TAILQ_FOREACH (current, &(con->nodes_head), nodes) {
        if (current->sticky_group == NULL) {
            workspace_reassign_sticky(current);
            continue;
        }

        LOG("Ah, this one is sticky: %s / %p\n", current->name, current);
        /* 2: find a window which we can re-assign */
        Con *output = con_get_output(current);
        Con *src = _get_sticky(output, current->sticky_group, current);

        if (src == NULL) {
            LOG("No window found for this sticky group\n");
            workspace_reassign_sticky(current);
            continue;
        }

        x_move_win(src, current);
        current->window = src->window;
        current->mapped = true;
        src->window = NULL;
        src->mapped = false;

        x_reparent_child(current, src);

        LOG("re-assigned window from src %p to dest %p\n", src, current);
    }

    TAILQ_FOREACH (current, &(con->floating_head), floating_windows) {
        workspace_reassign_sticky(current);
    }
}

/*
 * Callback to reset the urgent flag of the given con to false. May be started by
 * workspace_show to avoid urgency hints being lost by switching to a workspace
 * focusing the con.
 *
 */
static void workspace_defer_update_urgent_hint_cb(EV_P_ ev_timer *w, int revents) {
    Con *con = w->data;

    ev_timer_stop(main_loop, con->urgency_timer);
    FREE(con->urgency_timer);

    if (con->urgent) {
        DLOG("Resetting urgency flag of con %p by timer\n", con);
        con_set_urgency(con, false);
        con_update_parents_urgency(con);
        workspace_update_urgent_flag(con_get_workspace(con));
        ipc_send_window_event("urgent", con);
        tree_render();
    }
}

/*
 * Switches to the given workspace
 *
 */
void workspace_show(Con *workspace) {
    Con *current, *old = NULL;

    /* safe-guard against showing i3-internal workspaces like __i3_scratch */
    if (con_is_internal(workspace))
        return;

    /* disable fullscreen for the other workspaces and get the workspace we are
     * currently on. */
    TAILQ_FOREACH (current, &(workspace->parent->nodes_head), nodes) {
        if (current->fullscreen_mode == CF_OUTPUT)
            old = current;
        current->fullscreen_mode = CF_NONE;
    }

    /* enable fullscreen for the target workspace. If it happens to be the
     * same one we are currently on anyways, we can stop here. */
    workspace->fullscreen_mode = CF_OUTPUT;
    current = con_get_workspace(focused);
    if (workspace == current) {
        DLOG("Not switching, already there.\n");
        return;
    }

    /* Used to correctly update focus when pushing sticky windows. Holds the
     * previously focused container in the same output as workspace. For
     * example, if a sticky window is focused and then we switch focus to a
     * workspace in another output and then switch to a third workspace in the
     * first output, the sticky window needs to be refocused. */
    Con *old_focus = old ? con_descend_focused(old) : NULL;

    /* Remember currently focused workspace for switching back to it later with
     * the 'workspace back_and_forth' command.
     * NOTE: We have to duplicate the name as the original will be freed when
     * the corresponding workspace is cleaned up.
     * NOTE: Internal cons such as __i3_scratch (when a scratchpad window is
     * focused) are skipped, see bug #868. */
    if (current && !con_is_internal(current)) {
        FREE(previous_workspace_name);
        previous_workspace_name = sstrdup(current->name);
        DLOG("Setting previous_workspace_name = %s\n", previous_workspace_name);
    }

    workspace_reassign_sticky(workspace);

    DLOG("switching to %p / %s\n", workspace, workspace->name);
    Con *next = con_descend_focused(workspace);

    /* Memorize current output */
    Con *old_output = con_get_output(focused);

    /* Display urgency hint for a while if the newly visible workspace would
     * focus and thereby immediately destroy it */
    if (next->urgent && (int)(config.workspace_urgency_timer * 1000) > 0) {
        /* focus for now… */
        next->urgent = false;
        con_focus(next);

        /* … but immediately reset urgency flags; they will be set to false by
         * the timer callback in case the container is focused at the time of
         * its expiration */
        focused->urgent = true;
        workspace->urgent = true;

        if (focused->urgency_timer == NULL) {
            DLOG("Deferring reset of urgency flag of con %p on newly shown workspace %p\n",
                 focused, workspace);
            focused->urgency_timer = scalloc(1, sizeof(struct ev_timer));
            /* use a repeating timer to allow for easy resets */
            ev_timer_init(focused->urgency_timer, workspace_defer_update_urgent_hint_cb,
                          config.workspace_urgency_timer, config.workspace_urgency_timer);
            focused->urgency_timer->data = focused;
            ev_timer_start(main_loop, focused->urgency_timer);
        } else {
            DLOG("Resetting urgency timer of con %p on workspace %p\n",
                 focused, workspace);
            ev_timer_again(main_loop, focused->urgency_timer);
        }
    } else
        con_focus(next);

    ipc_send_workspace_event("focus", workspace, current);

    DLOG("old = %p / %s\n", old, (old ? old->name : "(null)"));
    /* Close old workspace if necessary. This must be done *after* doing
     * urgency handling, because tree_close_internal() will do a con_focus() on the next
     * client, which will clear the urgency flag too early. Also, there is no
     * way for con_focus() to know about when to clear urgency immediately and
     * when to defer it. */
    if (old && TAILQ_EMPTY(&(old->nodes_head)) && TAILQ_EMPTY(&(old->floating_head))) {
        /* check if this workspace is currently visible */
        if (!workspace_is_visible(old)) {
            LOG("Closing old workspace (%p / %s), it is empty\n", old, old->name);
            yajl_gen gen = ipc_marshal_workspace_event("empty", old, NULL);
            tree_close_internal(old, DONT_KILL_WINDOW, false);

            const unsigned char *payload;
            ylength length;
            y(get_buf, &payload, &length);
            ipc_send_event("workspace", I3_IPC_EVENT_WORKSPACE, (const char *)payload);

            y(free);

            /* Avoid calling output_push_sticky_windows later with a freed container. */
            if (old == old_focus) {
                old_focus = NULL;
            }

            ewmh_update_desktop_properties();
        }
    }

    workspace->fullscreen_mode = CF_OUTPUT;
    LOG("focused now = %p / %s\n", focused, focused->name);

    /* Set mouse pointer */
    Con *new_output = con_get_output(focused);
    if (old_output != new_output) {
        x_set_warp_to(&next->rect);
    }

    /* Update the EWMH hints */
    ewmh_update_current_desktop();

    /* Push any sticky windows to the now visible workspace. */
    output_push_sticky_windows(old_focus);
}

/*
 * Looks up the workspace by name and switches to it.
 *
 */
void workspace_show_by_name(const char *num) {
    workspace_show(workspace_get(num));
}

/*
 * Focuses the next workspace.
 *
 */
Con *workspace_next(void) {
    Con *current = con_get_workspace(focused);
    Con *next = NULL, *first = NULL, *first_opposite = NULL;
    Con *output;
    bool found_current = false;

    if (current->num == -1) {
        /* If currently a named workspace, find next named workspace. */
        if ((next = TAILQ_NEXT(current, nodes)) != NULL)
            return next;
        TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
            /* Skip outputs starting with __, they are internal. */
            if (con_is_internal(output))
                continue;
            NODES_FOREACH (output_get_content(output)) {
                if (child->type != CT_WORKSPACE)
                    continue;
                if (!first)
                    first = child;
                if (!first_opposite || (child->num != -1 && child->num < first_opposite->num))
                    first_opposite = child;
                if (child == current) {
                    found_current = true;
                } else if (child->num == -1 && found_current) {
                    next = child;
                    return next;
                }
            }
        }
    } else {
        /* If currently a numbered workspace, find next numbered workspace. */
        TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
            /* Skip outputs starting with __, they are internal. */
            if (con_is_internal(output))
                continue;
            NODES_FOREACH (output_get_content(output)) {
                if (child->type != CT_WORKSPACE)
                    continue;
                if (!first || (child->num != -1 && child->num < first->num))
                    first = child;
                if (!first_opposite && child->num == -1)
                    first_opposite = child;
                if (child->num == -1)
                    break;
                if (child == current) {
                    found_current = true;
                } else if (child->num == current->num && found_current) {
                    next = child;
                    return next;
                }
                /* Need to check child against current and next because we are
                 * traversing multiple lists and thus are not guaranteed the
                 * relative order between the list of workspaces. */
                if (current->num < child->num && (!next || child->num < next->num))
                    next = child;
            }
        }
    }

    if (!next)
        next = first_opposite ? first_opposite : first;

    return next;
}

/*
 * Focuses the previous workspace.
 *
 */
Con *workspace_prev(void) {
    Con *current = con_get_workspace(focused);
    Con *prev = NULL, *first_opposite = NULL, *last = NULL;
    Con *output;
    bool found_current = false;

    if (current->num == -1) {
        /* If named workspace, find previous named workspace. */
        prev = TAILQ_PREV(current, nodes_head, nodes);
        if (prev && prev->num != -1)
            prev = NULL;
        if (!prev) {
            TAILQ_FOREACH_REVERSE (output, &(croot->nodes_head), nodes_head, nodes) {
                /* Skip outputs starting with __, they are internal. */
                if (con_is_internal(output))
                    continue;
                NODES_FOREACH_REVERSE (output_get_content(output)) {
                    if (child->type != CT_WORKSPACE)
                        continue;
                    if (!last)
                        last = child;
                    if (!first_opposite || (child->num != -1 && child->num > first_opposite->num))
                        first_opposite = child;
                    if (child == current) {
                        found_current = true;
                    } else if (child->num == -1 && found_current) {
                        prev = child;
                        return prev;
                    }
                }
            }
        }
    } else {
        /* If numbered workspace, find previous numbered workspace. */
        TAILQ_FOREACH_REVERSE (output, &(croot->nodes_head), nodes_head, nodes) {
            /* Skip outputs starting with __, they are internal. */
            if (con_is_internal(output))
                continue;
            NODES_FOREACH_REVERSE (output_get_content(output)) {
                if (child->type != CT_WORKSPACE)
                    continue;
                if (!last || (child->num != -1 && last->num < child->num))
                    last = child;
                if (!first_opposite && child->num == -1)
                    first_opposite = child;
                if (child->num == -1)
                    continue;
                if (child == current) {
                    found_current = true;
                } else if (child->num == current->num && found_current) {
                    prev = child;
                    return prev;
                }
                /* Need to check child against current and previous because we
                 * are traversing multiple lists and thus are not guaranteed
                 * the relative order between the list of workspaces. */
                if (current->num > child->num && (!prev || child->num > prev->num))
                    prev = child;
            }
        }
    }

    if (!prev)
        prev = first_opposite ? first_opposite : last;

    return prev;
}

/*
 * Focuses the next workspace on the same output.
 *
 */
Con *workspace_next_on_output(void) {
    Con *current = con_get_workspace(focused);
    Con *next = NULL;
    Con *output = con_get_output(focused);
    bool found_current = false;

    if (current->num == -1) {
        /* If currently a named workspace, find next named workspace. */
        next = TAILQ_NEXT(current, nodes);
    } else {
        /* If currently a numbered workspace, find next numbered workspace. */
        NODES_FOREACH (output_get_content(output)) {
            if (child->type != CT_WORKSPACE)
                continue;
            if (child->num == -1)
                break;
            if (child == current) {
                found_current = true;
            } else if (child->num == current->num && found_current) {
                next = child;
                goto workspace_next_on_output_end;
            }
            /* Need to check child against current and next because we are
             * traversing multiple lists and thus are not guaranteed the
             * relative order between the list of workspaces. */
            if (current->num < child->num && (!next || child->num < next->num))
                next = child;
        }
    }

    /* Find next named workspace. */
    if (!next) {
        NODES_FOREACH (output_get_content(output)) {
            if (child->type != CT_WORKSPACE)
                continue;
            if (child == current) {
                found_current = true;
            } else if (child->num == -1 && (current->num != -1 || found_current)) {
                next = child;
                goto workspace_next_on_output_end;
            }
        }
    }

    /* Find first workspace. */
    if (!next) {
        NODES_FOREACH (output_get_content(output)) {
            if (child->type != CT_WORKSPACE)
                continue;
            if (!next || (child->num != -1 && child->num < next->num))
                next = child;
        }
    }
workspace_next_on_output_end:
    return next;
}

/*
 * Focuses the previous workspace on same output.
 *
 */
Con *workspace_prev_on_output(void) {
    Con *current = con_get_workspace(focused);
    Con *prev = NULL;
    Con *output = con_get_output(focused);
    bool found_current = false;
    DLOG("output = %s\n", output->name);

    if (current->num == -1) {
        /* If named workspace, find previous named workspace. */
        prev = TAILQ_PREV(current, nodes_head, nodes);
        if (prev && prev->num != -1)
            prev = NULL;
    } else {
        /* If numbered workspace, find previous numbered workspace. */
        NODES_FOREACH_REVERSE (output_get_content(output)) {
            if (child->type != CT_WORKSPACE || child->num == -1)
                continue;
            if (child == current) {
                found_current = true;
            } else if (child->num == current->num && found_current) {
                prev = child;
                goto workspace_prev_on_output_end;
            }
            /* Need to check child against current and previous because we
             * are traversing multiple lists and thus are not guaranteed
             * the relative order between the list of workspaces. */
            if (current->num > child->num && (!prev || child->num > prev->num))
                prev = child;
        }
    }

    /* Find previous named workspace. */
    if (!prev) {
        NODES_FOREACH_REVERSE (output_get_content(output)) {
            if (child->type != CT_WORKSPACE)
                continue;
            if (child == current) {
                found_current = true;
            } else if (child->num == -1 && (current->num != -1 || found_current)) {
                prev = child;
                goto workspace_prev_on_output_end;
            }
        }
    }

    /* Find last workspace. */
    if (!prev) {
        NODES_FOREACH_REVERSE (output_get_content(output)) {
            if (child->type != CT_WORKSPACE)
                continue;
            if (!prev || child->num > prev->num)
                prev = child;
        }
    }

workspace_prev_on_output_end:
    return prev;
}

/*
 * Focuses the previously focused workspace.
 *
 */
void workspace_back_and_forth(void) {
    if (!previous_workspace_name) {
        DLOG("No previous workspace name set. Not switching.\n");
        return;
    }

    workspace_show_by_name(previous_workspace_name);
}

/*
 * Returns the previously focused workspace con, or NULL if unavailable.
 *
 */
Con *workspace_back_and_forth_get(void) {
    if (!previous_workspace_name) {
        DLOG("No previous workspace name set.\n");
        return NULL;
    }

    return workspace_get(previous_workspace_name);
}

static bool get_urgency_flag(Con *con) {
    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        if (child->urgent || get_urgency_flag(child)) {
            return true;
        }
    }

    TAILQ_FOREACH (child, &(con->floating_head), floating_windows) {
        if (child->urgent || get_urgency_flag(child)) {
            return true;
        }
    }

    return false;
}

/*
 * Goes through all clients on the given workspace and updates the workspace’s
 * urgent flag accordingly.
 *
 */
void workspace_update_urgent_flag(Con *ws) {
    bool old_flag = ws->urgent;
    ws->urgent = get_urgency_flag(ws);
    DLOG("Workspace urgency flag changed from %d to %d\n", old_flag, ws->urgent);

    if (old_flag != ws->urgent)
        ipc_send_workspace_event("urgent", ws, NULL);
}

/*
 * 'Forces' workspace orientation by moving all cons into a new split-con with
 * the same layout as the workspace and then changing the workspace layout.
 *
 */
void ws_force_orientation(Con *ws, orientation_t orientation) {
    /* 1: create a new split container */
    Con *split = con_new(NULL, NULL);
    split->parent = ws;

    /* 2: copy layout from workspace */
    split->layout = ws->layout;

    /* 3: move the existing cons of this workspace below the new con */
    Con **focus_order = get_focus_order(ws);

    DLOG("Moving cons\n");
    while (!TAILQ_EMPTY(&(ws->nodes_head))) {
        Con *child = TAILQ_FIRST(&(ws->nodes_head));
        con_detach(child);
        con_attach(child, split, true);
    }

    set_focus_order(split, focus_order);
    free(focus_order);

    /* 4: switch workspace layout */
    ws->layout = (orientation == HORIZ) ? L_SPLITH : L_SPLITV;
    DLOG("split->layout = %d, ws->layout = %d\n", split->layout, ws->layout);

    /* 5: attach the new split container to the workspace */
    DLOG("Attaching new split (%p) to ws (%p)\n", split, ws);
    con_attach(split, ws, false);

    /* 6: fix the percentages */
    con_fix_percent(ws);
}

/*
 * Called when a new con (with a window, not an empty or split con) should be
 * attached to the workspace (for example when managing a new window or when
 * moving an existing window to the workspace level).
 *
 * Depending on the workspace_layout setting, this function either returns the
 * workspace itself (default layout) or creates a new stacked/tabbed con and
 * returns that.
 *
 */
Con *workspace_attach_to(Con *ws) {
    DLOG("Attaching a window to workspace %p / %s\n", ws, ws->name);

    if (ws->workspace_layout == L_DEFAULT) {
        DLOG("Default layout, just attaching it to the workspace itself.\n");
        return ws;
    }

    DLOG("Non-default layout, creating a new split container\n");
    /* 1: create a new split container */
    Con *new = con_new(NULL, NULL);
    new->parent = ws;

    /* 2: set the requested layout on the split con */
    new->layout = ws->workspace_layout;

    /* 4: attach the new split container to the workspace */
    DLOG("Attaching new split %p to workspace %p\n", new, ws);
    con_attach(new, ws, false);

    /* 5: fix the percentages */
    con_fix_percent(ws);

    return new;
}

/*
 * Creates a new container and re-parents all of children from the given
 * workspace into it.
 *
 * The container inherits the layout from the workspace.
 */
Con *workspace_encapsulate(Con *ws) {
    if (TAILQ_EMPTY(&(ws->nodes_head))) {
        ELOG("Workspace %p / %s has no children to encapsulate\n", ws, ws->name);
        return NULL;
    }

    Con *new = con_new(NULL, NULL);
    new->parent = ws;
    new->layout = ws->layout;

    Con **focus_order = get_focus_order(ws);

    DLOG("Moving children of workspace %p / %s into container %p\n",
         ws, ws->name, new);
    Con *child;
    while (!TAILQ_EMPTY(&(ws->nodes_head))) {
        child = TAILQ_FIRST(&(ws->nodes_head));
        con_detach(child);
        con_attach(child, new, true);
    }

    set_focus_order(new, focus_order);
    free(focus_order);

    con_attach(new, ws, true);

    return new;
}

/*
 * Move the given workspace to the specified output.
 */
void workspace_move_to_output(Con *ws, Output *output) {
    DLOG("Moving workspace %p / %s to output %p / \"%s\".\n", ws, ws->name, output, output_primary_name(output));

    Output *current_output = get_output_for_con(ws);
    Con *content = output_get_content(output->con);
    DLOG("got output %p with content %p\n", output, content);

    if (ws->parent == content) {
        DLOG("Nothing to do, workspace already there\n");
        return;
    }

    Con *previously_visible_ws = TAILQ_FIRST(&(content->focus_head));
    if (previously_visible_ws) {
        DLOG("Previously visible workspace = %p / %s\n", previously_visible_ws, previously_visible_ws->name);
    } else {
        DLOG("No previously visible workspace on output.\n");
    }

    bool workspace_was_visible = workspace_is_visible(ws);
    if (con_num_children(ws->parent) == 1) {
        DLOG("Creating a new workspace to replace \"%s\" (last on its output).\n", ws->name);

        /* check if we can find a workspace assigned to this output */
        bool used_assignment = false;
        struct Workspace_Assignment *assignment;
        TAILQ_FOREACH (assignment, &ws_assignments, ws_assignments) {
            if (!output_triggers_assignment(current_output, assignment)) {
                continue;
            }
            /* check if this workspace's name or num is already attached to the tree */
            const int num = ws_name_to_number(assignment->name);
            const bool attached = (num == -1)
                                      ? get_existing_workspace_by_name(assignment->name)
                                      : get_existing_workspace_by_num(num);
            if (attached) {
                continue;
            }

            /* so create the workspace referenced to by this assignment */
            DLOG("Creating workspace from assignment %s.\n", assignment->name);
            workspace_get(assignment->name);
            used_assignment = true;
            break;
        }

        /* if we couldn't create the workspace using an assignment, create it on
         * the output. Workspace init IPC events are sent either by
         * workspace_get or create_workspace_on_output. */
        if (!used_assignment) {
            create_workspace_on_output(current_output, ws->parent);
        }
    }
    DLOG("Detaching\n");

    /* detach from the old output and attach to the new output */
    Con *old_content = ws->parent;
    con_detach(ws);
    if (workspace_was_visible) {
        /* The workspace which we just detached was visible, so focus the next
         * one in the focus-stack. */
        Con *focus_ws = TAILQ_FIRST(&(old_content->focus_head));
        DLOG("workspace was visible, focusing %p / %s now\n", focus_ws, focus_ws->name);
        workspace_show(focus_ws);
    }
    con_attach(ws, content, false);

    /* fix the coordinates of the floating containers */
    Con *floating_con;
    TAILQ_FOREACH (floating_con, &(ws->floating_head), floating_windows) {
        floating_fix_coordinates(floating_con, &(old_content->rect), &(content->rect));
    }

    ipc_send_workspace_event("move", ws, NULL);
    if (workspace_was_visible) {
        /* Focus the moved workspace on the destination output. */
        workspace_show(ws);
    }

    ewmh_update_desktop_properties();

    if (!previously_visible_ws) {
        return;
    }

    /* NB: We cannot simply work with previously_visible_ws since it might have
     * been cleaned up by workspace_show() already, depending on the focus
     * order/number of other workspaces on the output. Instead, we loop through
     * the available workspaces and only work with previously_visible_ws if we
     * still find it. */
    TAILQ_FOREACH (ws, &(content->nodes_head), nodes) {
        if (ws != previously_visible_ws) {
            continue;
        }

        /* Call the on_remove_child callback of the workspace which previously
         * was visible on the destination output. Since it is no longer visible,
         * it might need to get cleaned up. */
        CALL(previously_visible_ws, on_remove_child);
        break;
    }
}
