/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * commands.c: all command functions (see commands_parser.c)
 *
 */
#include "all.h"

#include <stdint.h>
#include <float.h>
#include <stdarg.h>

#ifdef I3_ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

#include "shmlog.h"

// Macros to make the YAJL API a bit easier to use.
#define y(x, ...) (cmd_output->json_gen != NULL ? yajl_gen_##x(cmd_output->json_gen, ##__VA_ARGS__) : 0)
#define ystr(str) (cmd_output->json_gen != NULL ? yajl_gen_string(cmd_output->json_gen, (unsigned char *)str, strlen(str)) : 0)
#define ysuccess(success)                   \
    do {                                    \
        if (cmd_output->json_gen != NULL) { \
            y(map_open);                    \
            ystr("success");                \
            y(bool, success);               \
            y(map_close);                   \
        }                                   \
    } while (0)
#define yerror(format, ...)                             \
    do {                                                \
        if (cmd_output->json_gen != NULL) {             \
            char *message;                              \
            sasprintf(&message, format, ##__VA_ARGS__); \
            y(map_open);                                \
            ystr("success");                            \
            y(bool, false);                             \
            ystr("error");                              \
            ystr(message);                              \
            y(map_close);                               \
            free(message);                              \
        }                                               \
    } while (0)

/** If an error occurred during parsing of the criteria, we want to exit instead
 * of relying on fallback behavior. See #2091. */
#define HANDLE_INVALID_MATCH                                   \
    do {                                                       \
        if (current_match->error != NULL) {                    \
            yerror("Invalid match: %s", current_match->error); \
            return;                                            \
        }                                                      \
    } while (0)

/** When the command did not include match criteria (!), we use the currently
 * focused container. Do not confuse this case with a command which included
 * criteria but which did not match any windows. This macro has to be called in
 * every command.
 */
#define HANDLE_EMPTY_MATCH                              \
    do {                                                \
        HANDLE_INVALID_MATCH;                           \
                                                        \
        if (match_is_empty(current_match)) {            \
            while (!TAILQ_EMPTY(&owindows)) {           \
                owindow *ow = TAILQ_FIRST(&owindows);   \
                TAILQ_REMOVE(&owindows, ow, owindows);  \
                free(ow);                               \
            }                                           \
            owindow *ow = smalloc(sizeof(owindow));     \
            ow->con = focused;                          \
            TAILQ_INIT(&owindows);                      \
            TAILQ_INSERT_TAIL(&owindows, ow, owindows); \
        }                                               \
    } while (0)

/*
 * Checks whether we switched to a new workspace and returns false in that case,
 * signaling that further workspace switching should be done by the calling function
 * If not, calls workspace_back_and_forth() if workspace_auto_back_and_forth is set
 * and return true, signaling that no further workspace switching should occur in the calling function.
 *
 */
static bool maybe_back_and_forth(struct CommandResultIR *cmd_output, const char *name) {
    Con *ws = con_get_workspace(focused);

    /* If we switched to a different workspace, do nothing */
    if (strcmp(ws->name, name) != 0)
        return false;

    DLOG("This workspace is already focused.\n");
    if (config.workspace_auto_back_and_forth) {
        workspace_back_and_forth();
        cmd_output->needs_tree_render = true;
    }
    return true;
}

/*
 * Return the passed workspace unless it is the current one and auto back and
 * forth is enabled, in which case the back_and_forth workspace is returned.
 */
static Con *maybe_auto_back_and_forth_workspace(Con *workspace) {
    Con *current, *baf;

    if (!config.workspace_auto_back_and_forth)
        return workspace;

    current = con_get_workspace(focused);

    if (current == workspace) {
        baf = workspace_back_and_forth_get();
        if (baf != NULL) {
            DLOG("Substituting workspace with back_and_forth, as it is focused.\n");
            return baf;
        }
    }

    return workspace;
}

/*******************************************************************************
 * Criteria functions.
 ******************************************************************************/

/*
 * Helper data structure for an operation window (window on which the operation
 * will be performed). Used to build the TAILQ owindows.
 *
 */
typedef struct owindow {
    Con *con;

    TAILQ_ENTRY(owindow)
    owindows;
} owindow;

typedef TAILQ_HEAD(owindows_head, owindow) owindows_head;

static owindows_head owindows;

/*
 * Initializes the specified 'Match' data structure and the initial state of
 * commands.c for matching target windows of a command.
 *
 */
void cmd_criteria_init(I3_CMD) {
    Con *con;
    owindow *ow;

    DLOG("Initializing criteria, current_match = %p\n", current_match);
    match_free(current_match);
    match_init(current_match);
    while (!TAILQ_EMPTY(&owindows)) {
        ow = TAILQ_FIRST(&owindows);
        TAILQ_REMOVE(&owindows, ow, owindows);
        free(ow);
    }
    TAILQ_INIT(&owindows);
    /* copy all_cons */
    TAILQ_FOREACH(con, &all_cons, all_cons) {
        ow = smalloc(sizeof(owindow));
        ow->con = con;
        TAILQ_INSERT_TAIL(&owindows, ow, owindows);
    }
}

/*
 * A match specification just finished (the closing square bracket was found),
 * so we filter the list of owindows.
 *
 */
void cmd_criteria_match_windows(I3_CMD) {
    owindow *next, *current;

    DLOG("match specification finished, matching...\n");
    /* copy the old list head to iterate through it and start with a fresh
     * list which will contain only matching windows */
    struct owindows_head old = owindows;
    TAILQ_INIT(&owindows);
    for (next = TAILQ_FIRST(&old); next != TAILQ_END(&old);) {
        /* make a copy of the next pointer and advance the pointer to the
         * next element as we are going to invalidate the element’s
         * next/prev pointers by calling TAILQ_INSERT_TAIL later */
        current = next;
        next = TAILQ_NEXT(next, owindows);

        DLOG("checking if con %p / %s matches\n", current->con, current->con->name);

        /* We use this flag to prevent matching on window-less containers if
         * only window-specific criteria were specified. */
        bool accept_match = false;

        if (current_match->con_id != NULL) {
            accept_match = true;

            if (current_match->con_id == current->con) {
                DLOG("con_id matched.\n");
            } else {
                DLOG("con_id does not match.\n");
                FREE(current);
                continue;
            }
        }

        if (current_match->mark != NULL && !TAILQ_EMPTY(&(current->con->marks_head))) {
            accept_match = true;
            bool matched_by_mark = false;

            mark_t *mark;
            TAILQ_FOREACH(mark, &(current->con->marks_head), marks) {
                if (!regex_matches(current_match->mark, mark->name))
                    continue;

                DLOG("match by mark\n");
                matched_by_mark = true;
                break;
            }

            if (!matched_by_mark) {
                DLOG("mark does not match.\n");
                FREE(current);
                continue;
            }
        }

        if (current->con->window != NULL) {
            if (match_matches_window(current_match, current->con->window)) {
                DLOG("matches window!\n");
                accept_match = true;
            } else {
                DLOG("doesn't match\n");
                FREE(current);
                continue;
            }
        }

        if (accept_match) {
            TAILQ_INSERT_TAIL(&owindows, current, owindows);
        } else {
            FREE(current);
            continue;
        }
    }

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
    }
}

/*
 * Interprets a ctype=cvalue pair and adds it to the current match
 * specification.
 *
 */
void cmd_criteria_add(I3_CMD, const char *ctype, const char *cvalue) {
    match_parse_property(current_match, ctype, cvalue);
}

static void move_matches_to_workspace(Con *ws) {
    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        con_move_to_workspace(current->con, ws, true, false, false);
    }
}

/*
 * Implementation of 'move [window|container] [to] workspace
 * next|prev|next_on_output|prev_on_output|current'.
 *
 */
void cmd_move_con_to_workspace(I3_CMD, const char *which) {
    DLOG("which=%s\n", which);

    /* We have nothing to move:
     *  when criteria was specified but didn't match any window or
     *  when criteria wasn't specified and we don't have any window focused. */
    if ((!match_is_empty(current_match) && TAILQ_EMPTY(&owindows)) ||
        (match_is_empty(current_match) && focused->type == CT_WORKSPACE &&
         !con_has_children(focused))) {
        ysuccess(false);
        return;
    }

    HANDLE_EMPTY_MATCH;

    /* get the workspace */
    Con *ws;
    if (strcmp(which, "next") == 0)
        ws = workspace_next();
    else if (strcmp(which, "prev") == 0)
        ws = workspace_prev();
    else if (strcmp(which, "next_on_output") == 0)
        ws = workspace_next_on_output();
    else if (strcmp(which, "prev_on_output") == 0)
        ws = workspace_prev_on_output();
    else if (strcmp(which, "current") == 0)
        ws = con_get_workspace(focused);
    else {
        ELOG("BUG: called with which=%s\n", which);
        ysuccess(false);
        return;
    }

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/**
 * Implementation of 'move [window|container] [to] workspace back_and_forth'.
 *
 */
void cmd_move_con_to_workspace_back_and_forth(I3_CMD) {
    Con *ws = workspace_back_and_forth_get();
    if (ws == NULL) {
        yerror("No workspace was previously active.");
        return;
    }

    HANDLE_EMPTY_MATCH;

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move [--no-auto-back-and-forth] [window|container] [to] workspace <name>'.
 *
 */
void cmd_move_con_to_workspace_name(I3_CMD, const char *name, const char *_no_auto_back_and_forth) {
    if (strncasecmp(name, "__", strlen("__")) == 0) {
        LOG("You cannot move containers to i3-internal workspaces (\"%s\").\n", name);
        ysuccess(false);
        return;
    }

    const bool no_auto_back_and_forth = (_no_auto_back_and_forth != NULL);

    /* We have nothing to move:
     *  when criteria was specified but didn't match any window or
     *  when criteria wasn't specified and we don't have any window focused. */
    if (!match_is_empty(current_match) && TAILQ_EMPTY(&owindows)) {
        ELOG("No windows match your criteria, cannot move.\n");
        ysuccess(false);
        return;
    } else if (match_is_empty(current_match) && focused->type == CT_WORKSPACE &&
               !con_has_children(focused)) {
        ysuccess(false);
        return;
    }

    LOG("should move window to workspace %s\n", name);
    /* get the workspace */
    Con *ws = workspace_get(name, NULL);

    if (!no_auto_back_and_forth)
        ws = maybe_auto_back_and_forth_workspace(ws);

    HANDLE_EMPTY_MATCH;

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move [--no-auto-back-and-forth] [window|container] [to] workspace number <name>'.
 *
 */
void cmd_move_con_to_workspace_number(I3_CMD, const char *which, const char *_no_auto_back_and_forth) {
    const bool no_auto_back_and_forth = (_no_auto_back_and_forth != NULL);

    /* We have nothing to move:
     *  when criteria was specified but didn't match any window or
     *  when criteria wasn't specified and we don't have any window focused. */
    if ((!match_is_empty(current_match) && TAILQ_EMPTY(&owindows)) ||
        (match_is_empty(current_match) && focused->type == CT_WORKSPACE &&
         !con_has_children(focused))) {
        ysuccess(false);
        return;
    }

    LOG("should move window to workspace %s\n", which);

    long parsed_num = ws_name_to_number(which);
    if (parsed_num == -1) {
        LOG("Could not parse initial part of \"%s\" as a number.\n", which);
        yerror("Could not parse number \"%s\"", which);
        return;
    }

    Con *ws = get_existing_workspace_by_num(parsed_num);
    if (!ws) {
        ws = workspace_get(which, NULL);
    }

    if (!no_auto_back_and_forth)
        ws = maybe_auto_back_and_forth_workspace(ws);

    HANDLE_EMPTY_MATCH;

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

static void cmd_resize_floating(I3_CMD, const char *way, const char *direction, Con *floating_con, int px) {
    LOG("floating resize\n");
    Rect old_rect = floating_con->rect;
    Con *focused_con = con_descend_focused(floating_con);

    /* ensure that resize will take place even if pixel increment is smaller than
     * height increment or width increment.
     * fixes #1011 */
    const i3Window *window = focused_con->window;
    if (window != NULL) {
        if (strcmp(direction, "up") == 0 || strcmp(direction, "down") == 0 ||
            strcmp(direction, "height") == 0) {
            if (px < 0)
                px = (-px < window->height_increment) ? -window->height_increment : px;
            else
                px = (px < window->height_increment) ? window->height_increment : px;
        } else if (strcmp(direction, "left") == 0 || strcmp(direction, "right") == 0) {
            if (px < 0)
                px = (-px < window->width_increment) ? -window->width_increment : px;
            else
                px = (px < window->width_increment) ? window->width_increment : px;
        }
    }

    if (strcmp(direction, "up") == 0) {
        floating_con->rect.height += px;
    } else if (strcmp(direction, "down") == 0 || strcmp(direction, "height") == 0) {
        floating_con->rect.height += px;
    } else if (strcmp(direction, "left") == 0) {
        floating_con->rect.width += px;
    } else {
        floating_con->rect.width += px;
    }

    floating_check_size(floating_con);

    /* Did we actually resize anything or did the size constraints prevent us?
     * If we could not resize, exit now to not move the window. */
    if (memcmp(&old_rect, &(floating_con->rect), sizeof(Rect)) == 0)
        return;

    if (strcmp(direction, "up") == 0) {
        floating_con->rect.y -= (floating_con->rect.height - old_rect.height);
    } else if (strcmp(direction, "left") == 0) {
        floating_con->rect.x -= (floating_con->rect.width - old_rect.width);
    }

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (floating_con->scratchpad_state == SCRATCHPAD_FRESH)
        floating_con->scratchpad_state = SCRATCHPAD_CHANGED;
}

static bool cmd_resize_tiling_direction(I3_CMD, Con *current, const char *way, const char *direction, int ppt) {
    LOG("tiling resize\n");
    Con *second = NULL;
    Con *first = current;
    direction_t search_direction;
    if (!strcmp(direction, "left"))
        search_direction = D_LEFT;
    else if (!strcmp(direction, "right"))
        search_direction = D_RIGHT;
    else if (!strcmp(direction, "up"))
        search_direction = D_UP;
    else
        search_direction = D_DOWN;

    bool res = resize_find_tiling_participants(&first, &second, search_direction, false);
    if (!res) {
        LOG("No second container in this direction found.\n");
        ysuccess(false);
        return false;
    }

    /* get the default percentage */
    int children = con_num_children(first->parent);
    LOG("ins. %d children\n", children);
    double percentage = 1.0 / children;
    LOG("default percentage = %f\n", percentage);

    /* resize */
    LOG("first->percent before = %f\n", first->percent);
    LOG("second->percent before = %f\n", second->percent);
    if (first->percent == 0.0)
        first->percent = percentage;
    if (second->percent == 0.0)
        second->percent = percentage;
    double new_first_percent = first->percent + ((double)ppt / 100.0);
    double new_second_percent = second->percent - ((double)ppt / 100.0);
    LOG("new_first_percent = %f\n", new_first_percent);
    LOG("new_second_percent = %f\n", new_second_percent);
    /* Ensure that the new percentages are positive. */
    if (new_first_percent > 0.0 && new_second_percent > 0.0) {
        first->percent = new_first_percent;
        second->percent = new_second_percent;
        LOG("first->percent after = %f\n", first->percent);
        LOG("second->percent after = %f\n", second->percent);
    } else {
        LOG("Not resizing, already at minimum size\n");
    }

    return true;
}

static int compare_cons_by_percent(const void *a, const void *b) {
    Con *first = *((Con **)a);
    Con *second = *((Con **)b);
    return second->percent - first->percent;
}
static bool cmd_resize_tiling_width_height(I3_CMD, Con *current, const char *way, const char *direction, int ppt) {
    LOG("width/height resize\n");

    /* get the appropriate current container (skip stacked/tabbed cons) */
    Con *dummy = NULL;
    direction_t search_direction = (strcmp(direction, "width") == 0 ? D_LEFT : D_DOWN);
    bool search_result = resize_find_tiling_participants(&current, &dummy, search_direction, true);
    if (search_result == false) {
        ysuccess(false);
        return false;
    }

    /* get the default percentage */
    int children = con_num_children(current->parent);
    LOG("ins. %d children\n", children);
    double percentage = 1.0 / children;
    LOG("default percentage = %f\n", percentage);

    /* Ensure all the other children have a percentage set. */
    Con *child;
    TAILQ_FOREACH(child, &(current->parent->nodes_head), nodes) {
        LOG("child->percent = %f (child %p)\n", child->percent, child);
        if (child->percent == 0.0)
            child->percent = percentage;
    }

    LOG("current->percent before = %f\n", current->percent);

    double min_pct = fabs(ppt / 100.0);

    /* Grow */
    if (ppt > 0) {
        /* Sort cons by ascending percent */
        Con **tmp = scalloc(children, sizeof(Con *));
        int loop = 0;
        TAILQ_FOREACH(child, &(current->parent->nodes_head), nodes) {
            tmp[loop++] = child;
        }
        qsort(tmp, children, sizeof(Con *), compare_cons_by_percent);

        /* Walk children and shrink as appropriate. */
        /* Less-than-avg children will give what they can, and the remaining avg adjusted accordingly. */
        double requested_grow = ((double)ppt / 100.0);
        double total_remaining_shrinkage = requested_grow;
        int children_remaining = children - 1;
        for (loop = 0; loop < children; ++loop) {
            Con *child = tmp[loop]; /* readability */
            if (child == current)
                continue;
            double subtract_percent = total_remaining_shrinkage / children_remaining;
            --children_remaining;
            if (child->percent <= min_pct)
                continue;
            LOG("child->percent before (%p) = %f\n", child, child->percent);
            if (child->percent <= subtract_percent + min_pct) /* partial shrink */
                subtract_percent = child->percent - min_pct;
            total_remaining_shrinkage -= subtract_percent;
            child->percent -= subtract_percent;
            LOG("child->percent after (%p) = %f\n", child, child->percent);
        }
        free(tmp);

        if (requested_grow == total_remaining_shrinkage) {
            LOG("Not resizing, already at maximum size\n");
            ysuccess(false);
            return false;
        }
        current->percent += (requested_grow - total_remaining_shrinkage);
        LOG("current->percent after = %f\n", current->percent);

        return true;
    }

    /* Shrink */
    if (current->percent <= min_pct) {
        LOG("Not resizing, already at minimum size\n");
        ysuccess(false);
        return false;
    }

    double requested_shrink = ((double)ppt / 100.0);

    /* Check for partial shrink */
    if (current->percent + requested_shrink <= min_pct)
        requested_shrink = min_pct - current->percent;

    current->percent += requested_shrink;
    LOG("current->percent after = %f\n", current->percent);
    double subtract_percent = requested_shrink / (children - 1);
    LOG("subtract_percent = %f\n", subtract_percent);

    TAILQ_FOREACH(child, &(current->parent->nodes_head), nodes) {
        if (child == current)
            continue;
        child->percent -= subtract_percent;
        LOG("child->percent after (%p) = %f\n", child, child->percent);
    }

    return true;
}

/*
 * Implementation of 'resize grow|shrink <direction> [<px> px] [or <ppt> ppt]'.
 *
 */
void cmd_resize(I3_CMD, const char *way, const char *direction, long resize_px, long resize_ppt) {
    DLOG("resizing in way %s, direction %s, px %ld or ppt %ld\n", way, direction, resize_px, resize_ppt);
    if (strcmp(way, "shrink") == 0) {
        resize_px *= -1;
        resize_ppt *= -1;
    }

    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        /* Don't handle dock windows (issue #1201) */
        if (current->con->window && current->con->window->dock) {
            DLOG("This is a dock window. Not resizing (con = %p)\n)", current->con);
            continue;
        }

        Con *floating_con;
        if ((floating_con = con_inside_floating(current->con))) {
            cmd_resize_floating(current_match, cmd_output, way, direction, floating_con, resize_px);
        } else {
            if (strcmp(direction, "width") == 0 ||
                strcmp(direction, "height") == 0) {
                if (!cmd_resize_tiling_width_height(current_match, cmd_output,
                                                    current->con, way, direction, resize_ppt))
                    return;
            } else {
                if (!cmd_resize_tiling_direction(current_match, cmd_output,
                                                 current->con, way, direction, resize_ppt))
                    return;
            }
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'resize set <width> [px | ppt] <height> [px | ppt]'.
 *
 */
void cmd_resize_set(I3_CMD, long cwidth, const char *mode_width, long cheight, const char *mode_height) {
    DLOG("resizing to %ld %s x %ld %s\n", cwidth, mode_width, cheight, mode_height);
    if (cwidth < 0 || cheight < 0) {
        ELOG("Resize failed: dimensions cannot be negative (was %ld %s x %ld %s)\n", cwidth, mode_width, cheight, mode_height);
        return;
    }

    HANDLE_EMPTY_MATCH;

    owindow *current;
    bool success = true;
    TAILQ_FOREACH(current, &owindows, owindows) {
        Con *floating_con;
        if ((floating_con = con_inside_floating(current->con))) {
            Con *output = con_get_output(floating_con);
            if (cwidth == 0) {
                cwidth = floating_con->rect.width;
            } else if (mode_width && strcmp(mode_width, "ppt") == 0) {
                cwidth = output->rect.width * ((double)cwidth / 100.0);
            }
            if (cheight == 0) {
                cheight = floating_con->rect.height;
            } else if (mode_height && strcmp(mode_height, "ppt") == 0) {
                cheight = output->rect.height * ((double)cheight / 100.0);
            }
            floating_resize(floating_con, cwidth, cheight);
        } else {
            if (current->con->window && current->con->window->dock) {
                DLOG("This is a dock window. Not resizing (con = %p)\n)", current->con);
                continue;
            }

            if (cwidth > 0 && mode_width && strcmp(mode_width, "ppt") == 0) {
                /* get the appropriate current container (skip stacked/tabbed cons) */
                Con *target = current->con;
                Con *dummy;
                resize_find_tiling_participants(&target, &dummy, D_LEFT, true);

                /* Calculate new size for the target container */
                double current_percent = target->percent;
                char *action_string;
                long adjustment;

                if (current_percent > cwidth) {
                    action_string = "shrink";
                    adjustment = (int)(current_percent * 100) - cwidth;
                } else {
                    action_string = "grow";
                    adjustment = cwidth - (int)(current_percent * 100);
                }

                /* perform resizing and report failure if not possible */
                if (!cmd_resize_tiling_width_height(current_match, cmd_output,
                                                    target, action_string, "width", adjustment)) {
                    success = false;
                }
            }

            if (cheight > 0 && mode_height && strcmp(mode_height, "ppt") == 0) {
                /* get the appropriate current container (skip stacked/tabbed cons) */
                Con *target = current->con;
                Con *dummy;
                resize_find_tiling_participants(&target, &dummy, D_DOWN, true);

                /* Calculate new size for the target container */
                double current_percent = target->percent;
                char *action_string;
                long adjustment;

                if (current_percent > cheight) {
                    action_string = "shrink";
                    adjustment = (int)(current_percent * 100) - cheight;
                } else {
                    action_string = "grow";
                    adjustment = cheight - (int)(current_percent * 100);
                }

                /* perform resizing and report failure if not possible */
                if (!cmd_resize_tiling_width_height(current_match, cmd_output,
                                                    target, action_string, "height", adjustment)) {
                    success = false;
                }
            }
        }
    }

    cmd_output->needs_tree_render = true;
    ysuccess(success);
}

static int border_width_from_style(border_style_t border_style, long border_width, Con *con) {
    if (border_style == BS_NONE) {
        return 0;
    }
    if (border_width >= 0) {
        return logical_px(border_width);
    }

    const bool is_floating = con_inside_floating(con) != NULL;
    /* Load the configured defaults. */
    if (is_floating && border_style == config.default_floating_border) {
        return config.default_floating_border_width;
    } else if (!is_floating && border_style == config.default_border) {
        return config.default_border_width;
    } else {
        /* Use some hardcoded values. */
        return logical_px(border_style == BS_NORMAL ? 2 : 1);
    }
}

/*
 * Implementation of 'border normal|pixel [<n>]', 'border none|1pixel|toggle'.
 *
 */
void cmd_border(I3_CMD, const char *border_style_str, long border_width) {
    DLOG("border style should be changed to %s with border width %ld\n", border_style_str, border_width);
    owindow *current;

    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);

        border_style_t border_style;
        if (strcmp(border_style_str, "toggle") == 0) {
            border_style = (current->con->border_style + 1) % 3;
        } else if (strcmp(border_style_str, "normal") == 0) {
            border_style = BS_NORMAL;
        } else if (strcmp(border_style_str, "pixel") == 0) {
            border_style = BS_PIXEL;
        } else if (strcmp(border_style_str, "none") == 0) {
            border_style = BS_NONE;
        } else {
            ELOG("BUG: called with border_style=%s\n", border_style_str);
            ysuccess(false);
            return;
        }

        const int con_border_width = border_width_from_style(border_style, border_width, current->con);
        con_set_border_style(current->con, border_style, con_border_width);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'nop <comment>'.
 *
 */
void cmd_nop(I3_CMD, const char *comment) {
    LOG("-------------------------------------------------\n");
    LOG("  NOP: %s\n", comment);
    LOG("-------------------------------------------------\n");
    ysuccess(true);
}

/*
 * Implementation of 'append_layout <path>'.
 *
 */
void cmd_append_layout(I3_CMD, const char *cpath) {
    LOG("Appending layout \"%s\"\n", cpath);

    /* Make sure we allow paths like '~/.i3/layout.json' */
    char *path = resolve_tilde(cpath);

    char *buf = NULL;
    ssize_t len;
    if ((len = slurp(path, &buf)) < 0) {
        /* slurp already logged an error. */
        goto out;
    }

    if (!json_validate(buf, len)) {
        ELOG("Could not parse \"%s\" as JSON, not loading.\n", path);
        yerror("Could not parse \"%s\" as JSON.", path);
        goto out;
    }

    json_content_t content = json_determine_content(buf, len);
    LOG("JSON content = %d\n", content);
    if (content == JSON_CONTENT_UNKNOWN) {
        ELOG("Could not determine the contents of \"%s\", not loading.\n", path);
        yerror("Could not determine the contents of \"%s\".", path);
        goto out;
    }

    Con *parent = focused;
    if (content == JSON_CONTENT_WORKSPACE) {
        parent = output_get_content(con_get_output(parent));
    } else {
        /* We need to append the layout to a split container, since a leaf
         * container must not have any children (by definition).
         * Note that we explicitly check for workspaces, since they are okay for
         * this purpose, but con_accepts_window() returns false for workspaces. */
        while (parent->type != CT_WORKSPACE && !con_accepts_window(parent))
            parent = parent->parent;
    }
    DLOG("Appending to parent=%p instead of focused=%p\n", parent, focused);
    char *errormsg = NULL;
    tree_append_json(parent, buf, len, &errormsg);
    if (errormsg != NULL) {
        yerror(errormsg);
        free(errormsg);
        /* Note that we continue executing since tree_append_json() has
         * side-effects — user-provided layouts can be partly valid, partly
         * invalid, leading to half of the placeholder containers being
         * created. */
    } else {
        ysuccess(true);
    }

    // XXX: This is a bit of a kludge. Theoretically, render_con(parent,
    // false); should be enough, but when sending 'workspace 4; append_layout
    // /tmp/foo.json', the needs_tree_render == true of the workspace command
    // is not executed yet and will be batched with append_layout’s
    // needs_tree_render after the parser finished. We should check if that is
    // necessary at all.
    render_con(croot, false);

    restore_open_placeholder_windows(parent);

    if (content == JSON_CONTENT_WORKSPACE)
        ipc_send_workspace_event("restored", parent, NULL);

    cmd_output->needs_tree_render = true;
out:
    free(path);
    free(buf);
}

/*
 * Implementation of 'workspace next|prev|next_on_output|prev_on_output'.
 *
 */
void cmd_workspace(I3_CMD, const char *which) {
    Con *ws;

    DLOG("which=%s\n", which);

    if (con_get_fullscreen_con(croot, CF_GLOBAL)) {
        LOG("Cannot switch workspace while in global fullscreen\n");
        ysuccess(false);
        return;
    }

    if (strcmp(which, "next") == 0)
        ws = workspace_next();
    else if (strcmp(which, "prev") == 0)
        ws = workspace_prev();
    else if (strcmp(which, "next_on_output") == 0)
        ws = workspace_next_on_output();
    else if (strcmp(which, "prev_on_output") == 0)
        ws = workspace_prev_on_output();
    else {
        ELOG("BUG: called with which=%s\n", which);
        ysuccess(false);
        return;
    }

    workspace_show(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'workspace [--no-auto-back-and-forth] number <name>'
 *
 */
void cmd_workspace_number(I3_CMD, const char *which, const char *_no_auto_back_and_forth) {
    const bool no_auto_back_and_forth = (_no_auto_back_and_forth != NULL);

    if (con_get_fullscreen_con(croot, CF_GLOBAL)) {
        LOG("Cannot switch workspace while in global fullscreen\n");
        ysuccess(false);
        return;
    }

    long parsed_num = ws_name_to_number(which);
    if (parsed_num == -1) {
        LOG("Could not parse initial part of \"%s\" as a number.\n", which);
        yerror("Could not parse number \"%s\"", which);
        return;
    }

    Con *workspace = get_existing_workspace_by_num(parsed_num);
    if (!workspace) {
        LOG("There is no workspace with number %ld, creating a new one.\n", parsed_num);
        ysuccess(true);
        workspace_show_by_name(which);
        cmd_output->needs_tree_render = true;
        return;
    }
    if (!no_auto_back_and_forth && maybe_back_and_forth(cmd_output, workspace->name)) {
        ysuccess(true);
        return;
    }
    workspace_show(workspace);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'workspace back_and_forth'.
 *
 */
void cmd_workspace_back_and_forth(I3_CMD) {
    if (con_get_fullscreen_con(croot, CF_GLOBAL)) {
        LOG("Cannot switch workspace while in global fullscreen\n");
        ysuccess(false);
        return;
    }

    workspace_back_and_forth();

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'workspace [--no-auto-back-and-forth] <name>'
 *
 */
void cmd_workspace_name(I3_CMD, const char *name, const char *_no_auto_back_and_forth) {
    const bool no_auto_back_and_forth = (_no_auto_back_and_forth != NULL);

    if (strncasecmp(name, "__", strlen("__")) == 0) {
        LOG("You cannot switch to the i3-internal workspaces (\"%s\").\n", name);
        ysuccess(false);
        return;
    }

    if (con_get_fullscreen_con(croot, CF_GLOBAL)) {
        LOG("Cannot switch workspace while in global fullscreen\n");
        ysuccess(false);
        return;
    }

    DLOG("should switch to workspace %s\n", name);
    if (!no_auto_back_and_forth && maybe_back_and_forth(cmd_output, name)) {
        ysuccess(true);
        return;
    }
    workspace_show_by_name(name);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'mark [--add|--replace] [--toggle] <mark>'
 *
 */
void cmd_mark(I3_CMD, const char *mark, const char *mode, const char *toggle) {
    HANDLE_EMPTY_MATCH;

    owindow *current = TAILQ_FIRST(&owindows);
    if (current == NULL) {
        ysuccess(false);
        return;
    }

    /* Marks must be unique, i.e., no two windows must have the same mark. */
    if (current != TAILQ_LAST(&owindows, owindows_head)) {
        yerror("A mark must not be put onto more than one window");
        return;
    }

    DLOG("matching: %p / %s\n", current->con, current->con->name);

    mark_mode_t mark_mode = (mode == NULL || strcmp(mode, "--replace") == 0) ? MM_REPLACE : MM_ADD;
    if (toggle != NULL) {
        con_mark_toggle(current->con, mark, mark_mode);
    } else {
        con_mark(current->con, mark, mark_mode);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'unmark [mark]'
 *
 */
void cmd_unmark(I3_CMD, const char *mark) {
    if (match_is_empty(current_match)) {
        con_unmark(NULL, mark);
    } else {
        owindow *current;
        TAILQ_FOREACH(current, &owindows, owindows) {
            con_unmark(current->con, mark);
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'mode <string>'.
 *
 */
void cmd_mode(I3_CMD, const char *mode) {
    DLOG("mode=%s\n", mode);
    switch_mode(mode);

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move [window|container] [to] output <str>'.
 *
 */
void cmd_move_con_to_output(I3_CMD, const char *name) {
    DLOG("Should move window to output \"%s\".\n", name);
    HANDLE_EMPTY_MATCH;

    owindow *current;
    bool had_error = false;
    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);

        had_error |= !con_move_to_output_name(current->con, name, true);
    }

    cmd_output->needs_tree_render = true;
    ysuccess(!had_error);
}

/*
 * Implementation of 'move [container|window] [to] mark <str>'.
 *
 */
void cmd_move_con_to_mark(I3_CMD, const char *mark) {
    DLOG("moving window to mark \"%s\"\n", mark);

    HANDLE_EMPTY_MATCH;

    bool result = true;
    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("moving matched window %p / %s to mark \"%s\"\n", current->con, current->con->name, mark);
        result &= con_move_to_mark(current->con, mark);
    }

    cmd_output->needs_tree_render = true;
    ysuccess(result);
}

/*
 * Implementation of 'floating enable|disable|toggle'
 *
 */
void cmd_floating(I3_CMD, const char *floating_mode) {
    owindow *current;

    DLOG("floating_mode=%s\n", floating_mode);

    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        if (strcmp(floating_mode, "toggle") == 0) {
            DLOG("should toggle mode\n");
            toggle_floating_mode(current->con, false);
        } else {
            DLOG("should switch mode to %s\n", floating_mode);
            if (strcmp(floating_mode, "enable") == 0) {
                floating_enable(current->con, false);
            } else {
                floating_disable(current->con, false);
            }
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move workspace to [output] <str>'.
 *
 */
void cmd_move_workspace_to_output(I3_CMD, const char *name) {
    DLOG("should move workspace to output %s\n", name);

    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        if (con_is_internal(ws)) {
            continue;
        }

        Output *current_output = get_output_for_con(ws);
        if (current_output == NULL) {
            ELOG("Cannot get current output. This is a bug in i3.\n");
            ysuccess(false);
            return;
        }

        Output *target_output = get_output_from_string(current_output, name);
        if (!target_output) {
            ELOG("Could not get output from string \"%s\"\n", name);
            ysuccess(false);
            return;
        }

        bool success = workspace_move_to_output(ws, target_output);
        if (!success) {
            ELOG("Failed to move workspace to output.\n");
            ysuccess(false);
            return;
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'split v|h|t|vertical|horizontal|toggle'.
 *
 */
void cmd_split(I3_CMD, const char *direction) {
    HANDLE_EMPTY_MATCH;

    owindow *current;
    LOG("splitting in direction %c\n", direction[0]);
    TAILQ_FOREACH(current, &owindows, owindows) {
        if (con_is_docked(current->con)) {
            ELOG("Cannot split a docked container, skipping.\n");
            continue;
        }

        DLOG("matching: %p / %s\n", current->con, current->con->name);
        if (direction[0] == 't') {
            layout_t current_layout;
            if (current->con->type == CT_WORKSPACE) {
                current_layout = current->con->layout;
            } else {
                current_layout = current->con->parent->layout;
            }
            /* toggling split orientation */
            if (current_layout == L_SPLITH) {
                tree_split(current->con, VERT);
            } else {
                tree_split(current->con, HORIZ);
            }
        } else {
            tree_split(current->con, (direction[0] == 'v' ? VERT : HORIZ));
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'kill [window|client]'.
 *
 */
void cmd_kill(I3_CMD, const char *kill_mode_str) {
    if (kill_mode_str == NULL)
        kill_mode_str = "window";

    DLOG("kill_mode=%s\n", kill_mode_str);

    int kill_mode;
    if (strcmp(kill_mode_str, "window") == 0)
        kill_mode = KILL_WINDOW;
    else if (strcmp(kill_mode_str, "client") == 0)
        kill_mode = KILL_CLIENT;
    else {
        ELOG("BUG: called with kill_mode=%s\n", kill_mode_str);
        ysuccess(false);
        return;
    }

    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        con_close(current->con, kill_mode);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'exec [--no-startup-id] <command>'.
 *
 */
void cmd_exec(I3_CMD, const char *nosn, const char *command) {
    bool no_startup_id = (nosn != NULL);

    DLOG("should execute %s, no_startup_id = %d\n", command, no_startup_id);
    start_application(command, no_startup_id);

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'focus left|right|up|down'.
 *
 */
void cmd_focus_direction(I3_CMD, const char *direction) {
    DLOG("direction = *%s*\n", direction);

    if (strcmp(direction, "left") == 0)
        tree_next('p', HORIZ);
    else if (strcmp(direction, "right") == 0)
        tree_next('n', HORIZ);
    else if (strcmp(direction, "up") == 0)
        tree_next('p', VERT);
    else if (strcmp(direction, "down") == 0)
        tree_next('n', VERT);
    else {
        ELOG("Invalid focus direction (%s)\n", direction);
        ysuccess(false);
        return;
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Focus a container and disable any other fullscreen container not permitting the focus.
 *
 */
static void cmd_focus_force_focus(Con *con) {
    /* Disable fullscreen container in workspace with container to be focused. */
    Con *ws = con_get_workspace(con);
    Con *fullscreen_on_ws = (focused && focused->fullscreen_mode == CF_GLOBAL) ? focused : con_get_fullscreen_con(ws, CF_OUTPUT);
    if (fullscreen_on_ws && fullscreen_on_ws != con && !con_has_parent(con, fullscreen_on_ws)) {
        con_disable_fullscreen(fullscreen_on_ws);
    }
    con_activate(con);
}

/*
 * Implementation of 'focus tiling|floating|mode_toggle'.
 *
 */
void cmd_focus_window_mode(I3_CMD, const char *window_mode) {
    DLOG("window_mode = %s\n", window_mode);

    bool to_floating = false;
    if (strcmp(window_mode, "mode_toggle") == 0) {
        to_floating = !con_inside_floating(focused);
    } else if (strcmp(window_mode, "floating") == 0) {
        to_floating = true;
    } else if (strcmp(window_mode, "tiling") == 0) {
        to_floating = false;
    }

    Con *ws = con_get_workspace(focused);
    Con *current;
    bool success = false;
    TAILQ_FOREACH(current, &(ws->focus_head), focused) {
        if ((to_floating && current->type != CT_FLOATING_CON) ||
            (!to_floating && current->type == CT_FLOATING_CON))
            continue;

        cmd_focus_force_focus(con_descend_focused(current));
        success = true;
        break;
    }

    if (success) {
        cmd_output->needs_tree_render = true;
        ysuccess(true);
    } else {
        yerror("Failed to find a %s container in workspace.", to_floating ? "floating" : "tiling");
    }
}

/*
 * Implementation of 'focus parent|child'.
 *
 */
void cmd_focus_level(I3_CMD, const char *level) {
    DLOG("level = %s\n", level);
    bool success = false;

    /* Focusing the parent can only be allowed if the newly
     * focused container won't escape the fullscreen container. */
    if (strcmp(level, "parent") == 0) {
        if (focused && focused->parent) {
            if (con_fullscreen_permits_focusing(focused->parent))
                success = level_up();
            else
                ELOG("'focus parent': Currently in fullscreen, not going up\n");
        }
    }

    /* Focusing a child should always be allowed. */
    else
        success = level_down();

    cmd_output->needs_tree_render = success;
    // XXX: default reply for now, make this a better reply
    ysuccess(success);
}

/*
 * Implementation of 'focus'.
 *
 */
void cmd_focus(I3_CMD) {
    DLOG("current_match = %p\n", current_match);

    if (match_is_empty(current_match)) {
        ELOG("You have to specify which window/container should be focused.\n");
        ELOG("Example: [class=\"urxvt\" title=\"irssi\"] focus\n");

        yerror("You have to specify which window/container should be focused");

        return;
    }

    Con *__i3_scratch = workspace_get("__i3_scratch", NULL);
    int count = 0;
    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        /* If no workspace could be found, this was a dock window.
         * Just skip it, you cannot focus dock windows. */
        if (!ws)
            continue;

        /* In case this is a scratchpad window, call scratchpad_show(). */
        if (ws == __i3_scratch) {
            scratchpad_show(current->con);
            count++;
            /* While for the normal focus case we can change focus multiple
             * times and only a single window ends up focused, we could show
             * multiple scratchpad windows. So, rather break here. */
            break;
        }

        /* If the container is not on the current workspace,
         * workspace_show() will switch to a different workspace and (if
         * enabled) trigger a mouse pointer warp to the currently focused
         * container (!) on the target workspace.
         *
         * Therefore, before calling workspace_show(), we make sure that
         * 'current' will be focused on the workspace. However, we cannot
         * just con_focus(current) because then the pointer will not be
         * warped at all (the code thinks we are already there).
         *
         * So we focus 'current' to make it the currently focused window of
         * the target workspace, then revert focus. */
        Con *currently_focused = focused;
        cmd_focus_force_focus(current->con);
        con_activate(currently_focused);

        /* Now switch to the workspace, then focus */
        workspace_show(ws);
        LOG("focusing %p / %s\n", current->con, current->con->name);
        con_activate(current->con);
        count++;
    }

    if (count > 1)
        LOG("WARNING: Your criteria for the focus command matches %d containers, "
            "while only exactly one container can be focused at a time.\n",
            count);

    cmd_output->needs_tree_render = true;
    ysuccess(count > 0);
}

/*
 * Implementation of 'fullscreen enable|toggle [global]' and
 *                   'fullscreen disable'
 *
 */
void cmd_fullscreen(I3_CMD, const char *action, const char *fullscreen_mode) {
    fullscreen_mode_t mode = strcmp(fullscreen_mode, "global") == 0 ? CF_GLOBAL : CF_OUTPUT;
    DLOG("%s fullscreen, mode = %s\n", action, fullscreen_mode);
    owindow *current;

    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        if (strcmp(action, "toggle") == 0) {
            con_toggle_fullscreen(current->con, mode);
        } else if (strcmp(action, "enable") == 0) {
            con_enable_fullscreen(current->con, mode);
        } else if (strcmp(action, "disable") == 0) {
            con_disable_fullscreen(current->con);
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'sticky enable|disable|toggle'.
 *
 */
void cmd_sticky(I3_CMD, const char *action) {
    DLOG("%s sticky on window\n", action);
    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        if (current->con->window == NULL) {
            ELOG("only containers holding a window can be made sticky, skipping con = %p\n", current->con);
            continue;
        }
        DLOG("setting sticky for container = %p / %s\n", current->con, current->con->name);

        bool sticky = false;
        if (strcmp(action, "enable") == 0)
            sticky = true;
        else if (strcmp(action, "disable") == 0)
            sticky = false;
        else if (strcmp(action, "toggle") == 0)
            sticky = !current->con->sticky;

        current->con->sticky = sticky;
        ewmh_update_sticky(current->con->window->id, sticky);
    }

    /* A window we made sticky might not be on a visible workspace right now, so we need to make
     * sure it gets pushed to the front now. */
    output_push_sticky_windows(focused);

    ewmh_update_wm_desktop();

    cmd_output->needs_tree_render = true;
    ysuccess(true);
}

/*
 * Implementation of 'move <direction> [<pixels> [px]]'.
 *
 */
void cmd_move_direction(I3_CMD, const char *direction, long move_px) {
    owindow *current;
    HANDLE_EMPTY_MATCH;

    Con *initially_focused = focused;

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("moving in direction %s, px %ld\n", direction, move_px);
        if (con_is_floating(current->con)) {
            DLOG("floating move with %ld pixels\n", move_px);
            Rect newrect = current->con->parent->rect;
            if (strcmp(direction, "left") == 0) {
                newrect.x -= move_px;
            } else if (strcmp(direction, "right") == 0) {
                newrect.x += move_px;
            } else if (strcmp(direction, "up") == 0) {
                newrect.y -= move_px;
            } else if (strcmp(direction, "down") == 0) {
                newrect.y += move_px;
            }
            floating_reposition(current->con->parent, newrect);
        } else {
            tree_move(current->con, (strcmp(direction, "right") == 0 ? D_RIGHT : (strcmp(direction, "left") == 0 ? D_LEFT : (strcmp(direction, "up") == 0 ? D_UP : D_DOWN))));
            cmd_output->needs_tree_render = true;
        }
    }

    /* the move command should not disturb focus */
    if (focused != initially_focused)
        con_activate(initially_focused);

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'layout default|stacked|stacking|tabbed|splitv|splith'.
 *
 */
void cmd_layout(I3_CMD, const char *layout_str) {
    HANDLE_EMPTY_MATCH;

    layout_t layout;
    if (!layout_from_name(layout_str, &layout)) {
        ELOG("Unknown layout \"%s\", this is a mismatch between code and parser spec.\n", layout_str);
        return;
    }

    DLOG("changing layout to %s (%d)\n", layout_str, layout);

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        if (con_is_docked(current->con)) {
            ELOG("cannot change layout of a docked container, skipping it.\n");
            continue;
        }

        DLOG("matching: %p / %s\n", current->con, current->con->name);
        con_set_layout(current->con, layout);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'layout toggle [all|split]'.
 *
 */
void cmd_layout_toggle(I3_CMD, const char *toggle_mode) {
    owindow *current;

    if (toggle_mode == NULL)
        toggle_mode = "default";

    DLOG("toggling layout (mode = %s)\n", toggle_mode);

    /* check if the match is empty, not if the result is empty */
    if (match_is_empty(current_match))
        con_toggle_layout(focused, toggle_mode);
    else {
        TAILQ_FOREACH(current, &owindows, owindows) {
            DLOG("matching: %p / %s\n", current->con, current->con->name);
            con_toggle_layout(current->con, toggle_mode);
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'exit'.
 *
 */
void cmd_exit(I3_CMD) {
    LOG("Exiting due to user command.\n");
#ifdef I3_ASAN_ENABLED
    __lsan_do_leak_check();
#endif
    ipc_shutdown(SHUTDOWN_REASON_EXIT);
    unlink(config.ipc_socket_path);
    xcb_disconnect(conn);
    exit(0);

    /* unreached */
}

/*
 * Implementation of 'reload'.
 *
 */
void cmd_reload(I3_CMD) {
    LOG("reloading\n");
    kill_nagbar(&config_error_nagbar_pid, false);
    kill_nagbar(&command_error_nagbar_pid, false);
    load_configuration(conn, NULL, true);
    x_set_i3_atoms();
    /* Send an IPC event just in case the ws names have changed */
    ipc_send_workspace_event("reload", NULL, NULL);
    /* Send an update event for the barconfig just in case it has changed */
    update_barconfig();

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'restart'.
 *
 */
void cmd_restart(I3_CMD) {
    LOG("restarting i3\n");
    ipc_shutdown(SHUTDOWN_REASON_RESTART);
    unlink(config.ipc_socket_path);
    /* We need to call this manually since atexit handlers don’t get called
     * when exec()ing */
    purge_zerobyte_logfile();
    i3_restart(false);

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'open'.
 *
 */
void cmd_open(I3_CMD) {
    LOG("opening new container\n");
    Con *con = tree_open_con(NULL, NULL);
    con->layout = L_SPLITH;
    con_activate(con);

    y(map_open);
    ystr("success");
    y(bool, true);
    ystr("id");
    y(integer, (uintptr_t)con);
    y(map_close);

    cmd_output->needs_tree_render = true;
}

/*
 * Implementation of 'focus output <output>'.
 *
 */
void cmd_focus_output(I3_CMD, const char *name) {
    owindow *current;

    DLOG("name = %s\n", name);

    HANDLE_EMPTY_MATCH;

    /* get the output */
    Output *current_output = NULL;
    Output *output;

    TAILQ_FOREACH(current, &owindows, owindows)
    current_output = get_output_for_con(current->con);
    assert(current_output != NULL);

    output = get_output_from_string(current_output, name);

    if (!output) {
        LOG("No such output found.\n");
        ysuccess(false);
        return;
    }

    /* get visible workspace on output */
    Con *ws = NULL;
    GREP_FIRST(ws, output_get_content(output->con), workspace_is_visible(child));
    if (!ws) {
        ysuccess(false);
        return;
    }

    workspace_show(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move [window|container] [to] [absolute] position <px> [px] <px> [px]
 *
 */
void cmd_move_window_to_position(I3_CMD, long x, long y) {
    bool has_error = false;

    owindow *current;
    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH(current, &owindows, owindows) {
        if (!con_is_floating(current->con)) {
            ELOG("Cannot change position. The window/container is not floating\n");

            if (!has_error) {
                yerror("Cannot change position of a window/container because it is not floating.");
                has_error = true;
            }

            continue;
        }

        Rect newrect = current->con->parent->rect;

        DLOG("moving to position %ld %ld\n", x, y);
        newrect.x = x;
        newrect.y = y;

        if (!floating_reposition(current->con->parent, newrect)) {
            yerror("Cannot move window/container out of bounds.");
            has_error = true;
        }
    }

    if (!has_error)
        ysuccess(true);
}

/*
 * Implementation of 'move [window|container] [to] [absolute] position center
 *
 */
void cmd_move_window_to_center(I3_CMD, const char *method) {
    bool has_error = false;
    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        Con *floating_con = con_inside_floating(current->con);
        if (floating_con == NULL) {
            ELOG("con %p / %s is not floating, cannot move it to the center.\n",
                 current->con, current->con->name);

            if (!has_error) {
                yerror("Cannot change position of a window/container because it is not floating.");
                has_error = true;
            }

            continue;
        }

        if (strcmp(method, "absolute") == 0) {
            DLOG("moving to absolute center\n");
            floating_center(floating_con, croot->rect);

            floating_maybe_reassign_ws(floating_con);
            cmd_output->needs_tree_render = true;
        }

        if (strcmp(method, "position") == 0) {
            DLOG("moving to center\n");
            floating_center(floating_con, con_get_workspace(floating_con)->rect);

            cmd_output->needs_tree_render = true;
        }
    }

    // XXX: default reply for now, make this a better reply
    if (!has_error)
        ysuccess(true);
}

/*
 * Implementation of 'move [window|container] [to] position mouse'
 *
 */
void cmd_move_window_to_mouse(I3_CMD) {
    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        Con *floating_con = con_inside_floating(current->con);
        if (floating_con == NULL) {
            DLOG("con %p / %s is not floating, cannot move it to the mouse position.\n",
                 current->con, current->con->name);
            continue;
        }

        DLOG("moving floating container %p / %s to cursor position\n", floating_con, floating_con->name);
        floating_move_to_pointer(floating_con);
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);
}

/*
 * Implementation of 'move scratchpad'.
 *
 */
void cmd_move_scratchpad(I3_CMD) {
    DLOG("should move window to scratchpad\n");
    owindow *current;

    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        scratchpad_move(current->con);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'scratchpad show'.
 *
 */
void cmd_scratchpad_show(I3_CMD) {
    DLOG("should show scratchpad window\n");
    owindow *current;
    bool result = false;

    if (match_is_empty(current_match)) {
        result = scratchpad_show(NULL);
    } else {
        TAILQ_FOREACH(current, &owindows, owindows) {
            DLOG("matching: %p / %s\n", current->con, current->con->name);
            result |= scratchpad_show(current->con);
        }
    }

    cmd_output->needs_tree_render = true;

    ysuccess(result);
}

/*
 * Implementation of 'swap [container] [with] id|con_id|mark <arg>'.
 *
 */
void cmd_swap(I3_CMD, const char *mode, const char *arg) {
    HANDLE_EMPTY_MATCH;

    owindow *match = TAILQ_FIRST(&owindows);
    if (match == NULL) {
        DLOG("No match found for swapping.\n");
        return;
    }

    Con *con;
    if (strcmp(mode, "id") == 0) {
        long target;
        if (!parse_long(arg, &target, 0)) {
            yerror("Failed to parse %s into a window id.\n", arg);
            return;
        }

        con = con_by_window_id(target);
    } else if (strcmp(mode, "con_id") == 0) {
        long target;
        if (!parse_long(arg, &target, 0)) {
            yerror("Failed to parse %s into a container id.\n", arg);
            return;
        }

        con = con_by_con_id(target);
    } else if (strcmp(mode, "mark") == 0) {
        con = con_by_mark(arg);
    } else {
        yerror("Unhandled swap mode \"%s\". This is a bug.\n", mode);
        return;
    }

    if (con == NULL) {
        yerror("Could not find container for %s = %s\n", mode, arg);
        return;
    }

    if (match != TAILQ_LAST(&owindows, owindows_head)) {
        DLOG("More than one container matched the swap command, only using the first one.");
    }

    if (match->con == NULL) {
        DLOG("Match %p has no container.\n", match);
        ysuccess(false);
        return;
    }

    DLOG("Swapping %p with %p.\n", match->con, con);
    bool result = con_swap(match->con, con);

    cmd_output->needs_tree_render = true;
    ysuccess(result);
}

/*
 * Implementation of 'title_format <format>'
 *
 */
void cmd_title_format(I3_CMD, const char *format) {
    DLOG("setting title_format to \"%s\"\n", format);
    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH(current, &owindows, owindows) {
        DLOG("setting title_format for %p / %s\n", current->con, current->con->name);
        FREE(current->con->title_format);

        /* If we only display the title without anything else, we can skip the parsing step,
         * so we remove the title format altogether. */
        if (strcasecmp(format, "%title") != 0) {
            current->con->title_format = sstrdup(format);

            if (current->con->window != NULL) {
                i3String *formatted_title = con_parse_title_format(current->con);
                ewmh_update_visible_name(current->con->window->id, i3string_as_utf8(formatted_title));
                I3STRING_FREE(formatted_title);
            }
        } else {
            if (current->con->window != NULL) {
                /* We can remove _NET_WM_VISIBLE_NAME since we don't display a custom title. */
                ewmh_update_visible_name(current->con->window->id, NULL);
            }
        }

        if (current->con->window != NULL) {
            /* Make sure the window title is redrawn immediately. */
            current->con->window->name_x_changed = true;
        } else {
            /* For windowless containers we also need to force the redrawing. */
            FREE(current->con->deco_render_params);
        }
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);
}

/*
 * Implementation of 'rename workspace [<name>] to <name>'
 *
 */
void cmd_rename_workspace(I3_CMD, const char *old_name, const char *new_name) {
    if (strncasecmp(new_name, "__", strlen("__")) == 0) {
        LOG("Cannot rename workspace to \"%s\": names starting with __ are i3-internal.\n", new_name);
        ysuccess(false);
        return;
    }
    if (old_name) {
        LOG("Renaming workspace \"%s\" to \"%s\"\n", old_name, new_name);
    } else {
        LOG("Renaming current workspace to \"%s\"\n", new_name);
    }

    Con *workspace;
    if (old_name) {
        workspace = get_existing_workspace_by_name(old_name);
    } else {
        workspace = con_get_workspace(focused);
        old_name = workspace->name;
    }

    if (!workspace) {
        yerror("Old workspace \"%s\" not found", old_name);
        return;
    }

    Con *check_dest = get_existing_workspace_by_name(new_name);

    /* If check_dest == workspace, the user might be changing the case of the
     * workspace, or it might just be a no-op. */
    if (check_dest != NULL && check_dest != workspace) {
        yerror("New workspace \"%s\" already exists", new_name);
        return;
    }

    /* Change the name and try to parse it as a number. */
    /* old_name might refer to workspace->name, so copy it before free()ing */
    char *old_name_copy = sstrdup(old_name);
    FREE(workspace->name);
    workspace->name = sstrdup(new_name);

    workspace->num = ws_name_to_number(new_name);
    LOG("num = %d\n", workspace->num);

    /* By re-attaching, the sort order will be correct afterwards. */
    Con *previously_focused = focused;
    Con *previously_focused_content = focused->type == CT_WORKSPACE ? focused->parent : NULL;
    Con *parent = workspace->parent;
    con_detach(workspace);
    con_attach(workspace, parent, false);

    /* Move the workspace to the correct output if it has an assignment */
    struct Workspace_Assignment *assignment = NULL;
    TAILQ_FOREACH(assignment, &ws_assignments, ws_assignments) {
        if (assignment->output == NULL)
            continue;
        if (strcmp(assignment->name, workspace->name) != 0 && (!name_is_digits(assignment->name) || ws_name_to_number(assignment->name) != workspace->num)) {
            continue;
        }

        Output *target_output = get_output_by_name(assignment->output, true);
        if (!target_output) {
            LOG("Could not get output named \"%s\"\n", assignment->output);
            continue;
        }
        workspace_move_to_output(workspace, target_output);

        bool can_restore_focus = previously_focused != NULL;
        /* NB: If previously_focused is a workspace we can't
         * work directly with it since it might have been cleaned up by
         * workspace_show() already, depending on the
         * focus order/number of other workspaces on the output.
         * Instead, we loop through the available workspaces and only focus
         * previously_focused if we still find it. */
        if (previously_focused_content) {
            Con *workspace = NULL;
            GREP_FIRST(workspace, previously_focused_content, child == previously_focused);
            can_restore_focus &= (workspace != NULL);
        }

        if (can_restore_focus) {
            /* Restore the previous focus since con_attach messes with the focus. */
            workspace_show(con_get_workspace(previously_focused));
            con_activate(previously_focused);
        }

        break;
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);

    ipc_send_workspace_event("rename", workspace, NULL);
    ewmh_update_desktop_names();
    ewmh_update_desktop_viewport();
    ewmh_update_current_desktop();

    startup_sequence_rename_workspace(old_name_copy, new_name);
    free(old_name_copy);
}

/*
 * Implementation of 'bar mode dock|hide|invisible|toggle [<bar_id>]'
 *
 */
bool cmd_bar_mode(const char *bar_mode, const char *bar_id) {
    int mode = M_DOCK;
    bool toggle = false;
    if (strcmp(bar_mode, "dock") == 0)
        mode = M_DOCK;
    else if (strcmp(bar_mode, "hide") == 0)
        mode = M_HIDE;
    else if (strcmp(bar_mode, "invisible") == 0)
        mode = M_INVISIBLE;
    else if (strcmp(bar_mode, "toggle") == 0)
        toggle = true;
    else {
        ELOG("Unknown bar mode \"%s\", this is a mismatch between code and parser spec.\n", bar_mode);
        return false;
    }

    bool changed_sth = false;
    Barconfig *current = NULL;
    TAILQ_FOREACH(current, &barconfigs, configs) {
        if (bar_id && strcmp(current->id, bar_id) != 0)
            continue;

        if (toggle)
            mode = (current->mode + 1) % 2;

        DLOG("Changing bar mode of bar_id '%s' to '%s (%d)'\n", current->id, bar_mode, mode);
        current->mode = mode;
        changed_sth = true;

        if (bar_id)
            break;
    }

    if (bar_id && !changed_sth) {
        DLOG("Changing bar mode of bar_id %s failed, bar_id not found.\n", bar_id);
        return false;
    }

    return true;
}

/*
 * Implementation of 'bar hidden_state hide|show|toggle [<bar_id>]'
 *
 */
bool cmd_bar_hidden_state(const char *bar_hidden_state, const char *bar_id) {
    int hidden_state = S_SHOW;
    bool toggle = false;
    if (strcmp(bar_hidden_state, "hide") == 0)
        hidden_state = S_HIDE;
    else if (strcmp(bar_hidden_state, "show") == 0)
        hidden_state = S_SHOW;
    else if (strcmp(bar_hidden_state, "toggle") == 0)
        toggle = true;
    else {
        ELOG("Unknown bar state \"%s\", this is a mismatch between code and parser spec.\n", bar_hidden_state);
        return false;
    }

    bool changed_sth = false;
    Barconfig *current = NULL;
    TAILQ_FOREACH(current, &barconfigs, configs) {
        if (bar_id && strcmp(current->id, bar_id) != 0)
            continue;

        if (toggle)
            hidden_state = (current->hidden_state + 1) % 2;

        DLOG("Changing bar hidden_state of bar_id '%s' to '%s (%d)'\n", current->id, bar_hidden_state, hidden_state);
        current->hidden_state = hidden_state;
        changed_sth = true;

        if (bar_id)
            break;
    }

    if (bar_id && !changed_sth) {
        DLOG("Changing bar hidden_state of bar_id %s failed, bar_id not found.\n", bar_id);
        return false;
    }

    return true;
}

/*
 * Implementation of 'bar (hidden_state hide|show|toggle)|(mode dock|hide|invisible|toggle) [<bar_id>]'
 *
 */
void cmd_bar(I3_CMD, const char *bar_type, const char *bar_value, const char *bar_id) {
    bool ret;
    if (strcmp(bar_type, "mode") == 0)
        ret = cmd_bar_mode(bar_value, bar_id);
    else if (strcmp(bar_type, "hidden_state") == 0)
        ret = cmd_bar_hidden_state(bar_value, bar_id);
    else {
        ELOG("Unknown bar option type \"%s\", this is a mismatch between code and parser spec.\n", bar_type);
        ret = false;
    }

    ysuccess(ret);
    if (!ret)
        return;

    update_barconfig();
}

/*
 * Implementation of 'shmlog <size>|toggle|on|off'
 *
 */
void cmd_shmlog(I3_CMD, const char *argument) {
    if (!strcmp(argument, "toggle"))
        /* Toggle shm log, if size is not 0. If it is 0, set it to default. */
        shmlog_size = shmlog_size ? -shmlog_size : default_shmlog_size;
    else if (!strcmp(argument, "on"))
        shmlog_size = default_shmlog_size;
    else if (!strcmp(argument, "off"))
        shmlog_size = 0;
    else {
        long new_size = 0;
        if (!parse_long(argument, &new_size, 0)) {
            yerror("Failed to parse %s into a shmlog size.\n", argument);
            return;
        }
        /* If shm logging now, restart logging with the new size. */
        if (shmlog_size > 0) {
            shmlog_size = 0;
            LOG("Restarting shm logging...\n");
            init_logging();
        }
        shmlog_size = (int)new_size;
    }
    LOG("%s shm logging\n", shmlog_size > 0 ? "Enabling" : "Disabling");
    init_logging();
    update_shmlog_atom();
    ysuccess(true);
}

/*
 * Implementation of 'debuglog toggle|on|off'
 *
 */
void cmd_debuglog(I3_CMD, const char *argument) {
    bool logging = get_debug_logging();
    if (!strcmp(argument, "toggle")) {
        LOG("%s debug logging\n", logging ? "Disabling" : "Enabling");
        set_debug_logging(!logging);
    } else if (!strcmp(argument, "on") && !logging) {
        LOG("Enabling debug logging\n");
        set_debug_logging(true);
    } else if (!strcmp(argument, "off") && logging) {
        LOG("Disabling debug logging\n");
        set_debug_logging(false);
    }
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}
