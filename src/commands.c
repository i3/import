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
#include "shmlog.h"

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

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
    TAILQ_ENTRY(owindow) owindows;
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
    TAILQ_FOREACH (con, &all_cons, all_cons) {
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
            TAILQ_FOREACH (mark, &(current->con->marks_head), marks) {
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

    TAILQ_FOREACH (current, &owindows, owindows) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        con_move_to_workspace(current->con, ws, true, false, false);
    }
}

#define CHECK_MOVE_CON_TO_WORKSPACE                                                          \
    do {                                                                                     \
        HANDLE_EMPTY_MATCH;                                                                  \
        if (TAILQ_EMPTY(&owindows)) {                                                        \
            yerror("Nothing to move: specified criteria don't match any window");            \
            return;                                                                          \
        } else {                                                                             \
            bool found = false;                                                              \
            owindow *current = TAILQ_FIRST(&owindows);                                       \
            while (current) {                                                                \
                owindow *next = TAILQ_NEXT(current, owindows);                               \
                                                                                             \
                if (current->con->type == CT_WORKSPACE && !con_has_children(current->con)) { \
                    TAILQ_REMOVE(&owindows, current, owindows);                              \
                } else {                                                                     \
                    found = true;                                                            \
                }                                                                            \
                                                                                             \
                current = next;                                                              \
            }                                                                                \
            if (!found) {                                                                    \
                yerror("Nothing to move: workspace empty");                                  \
                return;                                                                      \
            }                                                                                \
        }                                                                                    \
    } while (0)

/*
 * Implementation of 'move [window|container] [to] workspace
 * next|prev|next_on_output|prev_on_output|current'.
 *
 */
void cmd_move_con_to_workspace(I3_CMD, const char *which) {
    DLOG("which=%s\n", which);

    CHECK_MOVE_CON_TO_WORKSPACE;

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
        yerror("BUG: called with which=%s", which);
        return;
    }

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
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
void cmd_move_con_to_workspace_name(I3_CMD, const char *name, const char *no_auto_back_and_forth) {
    if (strncasecmp(name, "__", strlen("__")) == 0) {
        yerror("You cannot move containers to i3-internal workspaces (\"%s\").", name);
        return;
    }

    CHECK_MOVE_CON_TO_WORKSPACE;

    LOG("should move window to workspace %s\n", name);
    /* get the workspace */
    Con *ws = workspace_get(name);

    if (no_auto_back_and_forth == NULL) {
        ws = maybe_auto_back_and_forth_workspace(ws);
    }

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'move [--no-auto-back-and-forth] [window|container] [to] workspace number <name>'.
 *
 */
void cmd_move_con_to_workspace_number(I3_CMD, const char *which, const char *no_auto_back_and_forth) {
    CHECK_MOVE_CON_TO_WORKSPACE;

    LOG("should move window to workspace %s\n", which);

    long parsed_num = ws_name_to_number(which);
    if (parsed_num == -1) {
        LOG("Could not parse initial part of \"%s\" as a number.\n", which);
        yerror("Could not parse number \"%s\"", which);
        return;
    }

    Con *ws = get_existing_workspace_by_num(parsed_num);
    if (!ws) {
        ws = workspace_get(which);
    }

    if (no_auto_back_and_forth == NULL) {
        ws = maybe_auto_back_and_forth_workspace(ws);
    }

    move_matches_to_workspace(ws);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Convert a string direction ("left", "right", etc.) to a direction_t. Assumes
 * valid direction string.
 */
static direction_t parse_direction(const char *str) {
    if (strcmp(str, "left") == 0) {
        return D_LEFT;
    } else if (strcmp(str, "right") == 0) {
        return D_RIGHT;
    } else if (strcmp(str, "up") == 0) {
        return D_UP;
    } else if (strcmp(str, "down") == 0) {
        return D_DOWN;
    } else {
        ELOG("Invalid direction. This is a parser bug.\n");
        assert(false);
    }
}

static void cmd_resize_floating(I3_CMD, const char *direction_str, Con *floating_con, int px) {
    Rect old_rect = floating_con->rect;
    Con *focused_con = con_descend_focused(floating_con);

    direction_t direction;
    if (strcmp(direction_str, "height") == 0) {
        direction = D_DOWN;
    } else if (strcmp(direction_str, "width") == 0) {
        direction = D_RIGHT;
    } else {
        direction = parse_direction(direction_str);
    }
    orientation_t orientation = orientation_from_direction(direction);

    /* ensure that resize will take place even if pixel increment is smaller than
     * height increment or width increment.
     * fixes #1011 */
    const i3Window *window = focused_con->window;
    if (window != NULL) {
        if (orientation == VERT) {
            if (px < 0) {
                px = (-px < window->height_increment) ? -window->height_increment : px;
            } else {
                px = (px < window->height_increment) ? window->height_increment : px;
            }
        } else {
            if (px < 0) {
                px = (-px < window->width_increment) ? -window->width_increment : px;
            } else {
                px = (px < window->width_increment) ? window->width_increment : px;
            }
        }
    }

    if (orientation == VERT) {
        floating_con->rect.height += px;
    } else {
        floating_con->rect.width += px;
    }
    floating_check_size(floating_con, orientation == VERT);

    /* Did we actually resize anything or did the size constraints prevent us?
     * If we could not resize, exit now to not move the window. */
    if (rect_equals(old_rect, floating_con->rect)) {
        return;
    }

    if (direction == D_UP) {
        floating_con->rect.y -= (floating_con->rect.height - old_rect.height);
    } else if (direction == D_LEFT) {
        floating_con->rect.x -= (floating_con->rect.width - old_rect.width);
    }

    /* If this is a scratchpad window, don't auto center it from now on. */
    if (floating_con->scratchpad_state == SCRATCHPAD_FRESH) {
        floating_con->scratchpad_state = SCRATCHPAD_CHANGED;
    }
}

static bool cmd_resize_tiling_direction(I3_CMD, Con *current, const char *direction, int px, int ppt) {
    Con *second = NULL;
    Con *first = current;
    direction_t search_direction = parse_direction(direction);

    bool res = resize_find_tiling_participants(&first, &second, search_direction, false);
    if (!res) {
        yerror("No second container found in this direction.");
        return false;
    }

    if (ppt) {
        /* For backwards compatibility, 'X px or Y ppt' means that ppt is
         * preferred. */
        px = 0;
    }
    return resize_neighboring_cons(first, second, px, ppt);
}

static bool cmd_resize_tiling_width_height(I3_CMD, Con *current, const char *direction, int px, double ppt) {
    LOG("width/height resize\n");

    /* get the appropriate current container (skip stacked/tabbed cons) */
    Con *dummy = NULL;
    direction_t search_direction = (strcmp(direction, "width") == 0 ? D_LEFT : D_DOWN);
    bool search_result = resize_find_tiling_participants(&current, &dummy, search_direction, true);
    if (search_result == false) {
        yerror("Failed to find appropriate tiling containers for resize operation");
        return false;
    }

    /* get the default percentage */
    int children = con_num_children(current->parent);
    LOG("ins. %d children\n", children);
    double percentage = 1.0 / children;
    LOG("default percentage = %f\n", percentage);

    /* Ensure all the other children have a percentage set. */
    Con *child;
    TAILQ_FOREACH (child, &(current->parent->nodes_head), nodes) {
        LOG("child->percent = %f (child %p)\n", child->percent, child);
        if (child->percent == 0.0)
            child->percent = percentage;
    }

    double new_current_percent;
    double subtract_percent;
    if (ppt != 0.0) {
        new_current_percent = current->percent + ppt;
    } else {
        /* Convert px change to change in percentages */
        ppt = (double)px / (double)con_rect_size_in_orientation(current->parent);
        new_current_percent = current->percent + ppt;
    }
    subtract_percent = ppt / (children - 1);
    if (ppt < 0.0 && new_current_percent < percent_for_1px(current)) {
        yerror("Not resizing, container would end with less than 1px");
        return false;
    }

    LOG("new_current_percent = %f\n", new_current_percent);
    LOG("subtract_percent = %f\n", subtract_percent);
    /* Ensure that the new percentages are positive. */
    if (subtract_percent >= 0.0) {
        TAILQ_FOREACH (child, &(current->parent->nodes_head), nodes) {
            if (child == current) {
                continue;
            }
            if (child->percent - subtract_percent < percent_for_1px(child)) {
                yerror("Not resizing, already at minimum size (child %p would end up with a size of %.f", child, child->percent - subtract_percent);
                return false;
            }
        }
    }

    current->percent = new_current_percent;
    LOG("current->percent after = %f\n", current->percent);

    TAILQ_FOREACH (child, &(current->parent->nodes_head), nodes) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
        /* Don't handle dock windows (issue #1201) */
        if (current->con->window && current->con->window->dock) {
            DLOG("This is a dock window. Not resizing (con = %p)\n)", current->con);
            continue;
        }

        Con *floating_con;
        if ((floating_con = con_inside_floating(current->con))) {
            cmd_resize_floating(current_match, cmd_output, direction, floating_con, resize_px);
        } else {
            if (strcmp(direction, "width") == 0 ||
                strcmp(direction, "height") == 0) {
                const double ppt = (double)resize_ppt / 100.0;
                if (!cmd_resize_tiling_width_height(current_match, cmd_output,
                                                    current->con, direction,
                                                    resize_px, ppt)) {
                    yerror("Cannot resize.");
                    return;
                }
            } else {
                if (!cmd_resize_tiling_direction(current_match, cmd_output,
                                                 current->con, direction,
                                                 resize_px, resize_ppt)) {
                    yerror("Cannot resize.");
                    return;
                }
            }
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

static bool resize_set_tiling(I3_CMD, Con *target, orientation_t resize_orientation, bool is_ppt, long target_size) {
    direction_t search_direction;
    char *mode;
    if (resize_orientation == HORIZ) {
        search_direction = D_LEFT;
        mode = "width";
    } else {
        search_direction = D_DOWN;
        mode = "height";
    }

    /* Get the appropriate current container (skip stacked/tabbed cons) */
    Con *dummy;
    resize_find_tiling_participants(&target, &dummy, search_direction, true);

    /* Calculate new size for the target container */
    double ppt = 0.0;
    int px = 0;
    if (is_ppt) {
        ppt = (double)target_size / 100.0 - target->percent;
    } else {
        px = target_size - (resize_orientation == HORIZ ? target->rect.width : target->rect.height);
    }

    /* Perform resizing and report failure if not possible */
    return cmd_resize_tiling_width_height(current_match, cmd_output,
                                          target, mode, px, ppt);
}

/*
 * Implementation of 'resize set <width> [px | ppt] <height> [px | ppt]'.
 *
 */
void cmd_resize_set(I3_CMD, long cwidth, const char *mode_width, long cheight, const char *mode_height) {
    DLOG("resizing to %ld %s x %ld %s\n", cwidth, mode_width, cheight, mode_height);
    if (cwidth < 0 || cheight < 0) {
        yerror("Dimensions cannot be negative.");
        return;
    }

    HANDLE_EMPTY_MATCH;

    owindow *current;
    bool success = true;
    TAILQ_FOREACH (current, &owindows, owindows) {
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

            if (cwidth > 0) {
                bool is_ppt = mode_width && strcmp(mode_width, "ppt") == 0;
                success &= resize_set_tiling(current_match, cmd_output, current->con,
                                             HORIZ, is_ppt, cwidth);
            }
            if (cheight > 0) {
                bool is_ppt = mode_height && strcmp(mode_height, "ppt") == 0;
                success &= resize_set_tiling(current_match, cmd_output, current->con,
                                             VERT, is_ppt, cheight);
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

    TAILQ_FOREACH (current, &owindows, owindows) {
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
            yerror("BUG: called with border_style=%s", border_style_str);
            return;
        }

        const int con_border_width = border_width_from_style(border_style, border_width, current->con);
        con_set_border_style(current->con, border_style, con_border_width);
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);
}

/*
 * Implementation of 'nop <comment>'.
 *
 */
void cmd_nop(I3_CMD, const char *comment) {
    LOG("-------------------------------------------------\n");
    LOG("  NOP: %.4000s\n", comment);
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
        yerror("Could not slurp \"%s\".", path);
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
    render_con(croot);

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
        yerror("Cannot switch workspace while in global fullscreen");
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
        yerror("BUG: called with which=%s", which);
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
        yerror("Cannot switch workspace while in global fullscreen");
        return;
    }

    long parsed_num = ws_name_to_number(which);
    if (parsed_num == -1) {
        yerror("Could not parse initial part of \"%s\" as a number.", which);
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
        yerror("Cannot switch workspace while in global fullscreen");
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
        yerror("You cannot switch to the i3-internal workspaces (\"%s\").", name);
        return;
    }

    if (con_get_fullscreen_con(croot, CF_GLOBAL)) {
        yerror("Cannot switch workspace while in global fullscreen");
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
        yerror("Given criteria don't match a window");
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
        TAILQ_FOREACH (current, &owindows, owindows) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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

    TAILQ_FOREACH (current, &owindows, owindows) {
        DLOG("matching: %p / %s\n", current->con, current->con->name);
        if (strcmp(floating_mode, "toggle") == 0) {
            DLOG("should toggle mode\n");
            toggle_floating_mode(current->con, false);
        } else {
            DLOG("should switch mode to %s\n", floating_mode);
            if (strcmp(floating_mode, "enable") == 0) {
                floating_enable(current->con, false);
            } else {
                floating_disable(current->con);
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
    TAILQ_FOREACH (current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        if (con_is_internal(ws)) {
            continue;
        }

        Output *current_output = get_output_for_con(ws);
        Output *target_output = get_output_from_string(current_output, name);
        if (!target_output) {
            yerror("Could not get output from string \"%s\"", name);
            return;
        }

        workspace_move_to_output(ws, target_output);
    }

    cmd_output->needs_tree_render = true;
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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
        yerror("BUG: called with kill_mode=%s", kill_mode_str);
        return;
    }

    HANDLE_EMPTY_MATCH;

    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
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

    HANDLE_EMPTY_MATCH;

    int count = 0;
    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
        count++;
    }

    if (count > 1) {
        LOG("WARNING: Your criteria for the exec command match %d containers, "
            "so the command will execute this many times.\n",
            count);
    }

    TAILQ_FOREACH (current, &owindows, owindows) {
        DLOG("should execute %s, no_startup_id = %d\n", command, no_startup_id);
        start_application(command, no_startup_id);
    }

    ysuccess(true);
}

#define CMD_FOCUS_WARN_CHILDREN                                                        \
    do {                                                                               \
        int count = 0;                                                                 \
        owindow *current;                                                              \
        TAILQ_FOREACH (current, &owindows, owindows) {                                 \
            count++;                                                                   \
        }                                                                              \
                                                                                       \
        if (count > 1) {                                                               \
            LOG("WARNING: Your criteria for the focus command matches %d containers, " \
                "while only exactly one container can be focused at a time.\n",        \
                count);                                                                \
        }                                                                              \
    } while (0)

/*
 * Implementation of 'focus left|right|up|down|next|prev'.
 *
 */
void cmd_focus_direction(I3_CMD, const char *direction_str) {
    HANDLE_EMPTY_MATCH;
    CMD_FOCUS_WARN_CHILDREN;

    direction_t direction;
    position_t position;
    bool auto_direction = true;
    if (strcmp(direction_str, "prev") == 0) {
        position = BEFORE;
    } else if (strcmp(direction_str, "next") == 0) {
        position = AFTER;
    } else {
        auto_direction = false;
        direction = parse_direction(direction_str);
    }

    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        if (!ws || con_is_internal(ws)) {
            continue;
        }
        if (auto_direction) {
            orientation_t o = con_orientation(current->con->parent);
            direction = direction_from_orientation_position(o, position);
        }
        tree_next(current->con, direction);
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'focus next|prev sibling'
 *
 */
void cmd_focus_sibling(I3_CMD, const char *direction_str) {
    HANDLE_EMPTY_MATCH;
    CMD_FOCUS_WARN_CHILDREN;

    const position_t direction = (STARTS_WITH(direction_str, "prev")) ? BEFORE : AFTER;
    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        if (!ws || con_is_internal(ws)) {
            continue;
        }
        Con *next = get_tree_next_sibling(current->con, direction);
        if (next) {
            if (next->type == CT_WORKSPACE) {
                /* On the workspace level, we need to make sure that the
                 * workspace change happens properly. However, workspace_show
                 * descends focus so we also have to put focus on the workspace
                 * itself to maintain consistency. See #3997. */
                workspace_show(next);
                con_focus(next);
            } else {
                con_activate(next);
            }
        }
    }

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
    ysuccess(true);
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
    TAILQ_FOREACH (current, &(ws->focus_head), focused) {
        if ((to_floating && current->type != CT_FLOATING_CON) ||
            (!to_floating && current->type == CT_FLOATING_CON))
            continue;

        con_activate_unblock(con_descend_focused(current));
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
void cmd_focus(I3_CMD, bool focus_workspace) {
    DLOG("current_match = %p\n", current_match);

    if (match_is_empty(current_match)) {
        ELOG("You have to specify which window/container should be focused.\n");
        ELOG("Example: [class=\"urxvt\" title=\"irssi\"] focus\n");

        yerror("You have to specify which window/container should be focused");
        return;
    } else if (TAILQ_EMPTY(&owindows)) {
        yerror("No window matches given criteria");
        return;
    }

    CMD_FOCUS_WARN_CHILDREN;

    Con *__i3_scratch = workspace_get("__i3_scratch");
    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
        Con *ws = con_get_workspace(current->con);
        /* If no workspace could be found, this was a dock window.
         * Just skip it, you cannot focus dock windows. */
        if (!ws) {
            continue;
        }

        /* In case this is a scratchpad window, call scratchpad_show(). */
        if (ws == __i3_scratch && !focus_workspace) {
            scratchpad_show(current->con);
            /* While for the normal focus case we can change focus multiple
             * times and only a single window ends up focused, we could show
             * multiple scratchpad windows. So, rather break here. */
            break;
        }

        if (focus_workspace) {
            LOG("focusing workspace %p / %s - %p / %s\n", current->con, current->con->name, ws, ws->name);
            workspace_show(ws);
        } else {
            LOG("focusing %p / %s\n", current->con, current->con->name);
            con_activate_unblock(current->con);
        }
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);
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

    TAILQ_FOREACH (current, &owindows, owindows) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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
 * Implementation of 'move <direction> [<amount> [px|ppt]]'.
 *
 */
void cmd_move_direction(I3_CMD, const char *direction_str, long amount, const char *mode) {
    owindow *current;
    HANDLE_EMPTY_MATCH;

    Con *initially_focused = focused;
    direction_t direction = parse_direction(direction_str);

    const bool is_ppt = mode && strcmp(mode, "ppt") == 0;

    DLOG("moving in direction %s, %ld %s\n", direction_str, amount, mode);
    TAILQ_FOREACH (current, &owindows, owindows) {
        if (con_is_floating(current->con)) {
            DLOG("floating move with %ld %s\n", amount, mode);
            Rect newrect = current->con->parent->rect;
            Con *output = con_get_output(current->con);

            switch (direction) {
                case D_LEFT:
                    newrect.x -= is_ppt ? output->rect.width * ((double)amount / 100.0) : amount;
                    break;
                case D_RIGHT:
                    newrect.x += is_ppt ? output->rect.width * ((double)amount / 100.0) : amount;
                    break;
                case D_UP:
                    newrect.y -= is_ppt ? output->rect.height * ((double)amount / 100.0) : amount;
                    break;
                case D_DOWN:
                    newrect.y += is_ppt ? output->rect.height * ((double)amount / 100.0) : amount;
                    break;
            }

            cmd_output->needs_tree_render = floating_reposition(current->con->parent, newrect);
        } else {
            tree_move(current->con, direction);
            cmd_output->needs_tree_render = true;
        }
    }

    /* The move command should not disturb focus. con_exists is called because
     * tree_move calls tree_flatten. */
    if (focused != initially_focused && con_exists(initially_focused)) {
        con_activate(initially_focused);
    }

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
        yerror("Unknown layout \"%s\", this is a mismatch between code and parser spec.", layout_str);
        return;
    }

    DLOG("changing layout to %s (%d)\n", layout_str, layout);

    owindow *current;
    TAILQ_FOREACH (current, &owindows, owindows) {
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
        TAILQ_FOREACH (current, &owindows, owindows) {
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
    exit(EXIT_SUCCESS);

    /* unreached */
}

/*
 * Implementation of 'reload'.
 *
 */
void cmd_reload(I3_CMD) {
    LOG("reloading\n");

    kill_nagbar(config_error_nagbar_pid, false);
    kill_nagbar(command_error_nagbar_pid, false);
    /* start_nagbar() will refuse to start a new process if the passed pid is
     * set. This will happen when our child watcher is triggered by libev when
     * the loop re-starts. However, config errors might be detected before
     * that since we will read the config right now with load_configuration.
     * See #4104. */
    config_error_nagbar_pid = command_error_nagbar_pid = -1;

    load_configuration(NULL, C_RELOAD);
    x_set_i3_atoms();
    /* Send an IPC event just in case the ws names have changed */
    ipc_send_workspace_event("reload", NULL, NULL);
    /* Send an update event for each barconfig just in case it has changed */
    Barconfig *current;
    TAILQ_FOREACH (current, &barconfigs, configs) {
        ipc_send_barconfig_update_event(current);
    }

    // XXX: default reply for now, make this a better reply
    ysuccess(true);
}

/*
 * Implementation of 'restart'.
 *
 */
void cmd_restart(I3_CMD) {
    LOG("restarting i3\n");
    int exempt_fd = -1;
    if (cmd_output->client != NULL) {
        exempt_fd = cmd_output->client->fd;
        LOG("Carrying file descriptor %d across restart\n", exempt_fd);
        int flags;
        if ((flags = fcntl(exempt_fd, F_GETFD)) < 0 ||
            fcntl(exempt_fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
            ELOG("Could not disable FD_CLOEXEC on fd %d\n", exempt_fd);
        }
        char *fdstr = NULL;
        sasprintf(&fdstr, "%d", exempt_fd);
        setenv("_I3_RESTART_FD", fdstr, 1);
    }
    ipc_shutdown(SHUTDOWN_REASON_RESTART, exempt_fd);
    unlink(config.ipc_socket_path);
    if (current_log_stream_socket_path != NULL) {
        unlink(current_log_stream_socket_path);
    }
    /* We need to call this manually since atexit handlers don’t get called
     * when exec()ing */
    purge_zerobyte_logfile();
    i3_restart(false);
    /* unreached */
    assert(false);
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
    HANDLE_EMPTY_MATCH;

    if (TAILQ_EMPTY(&owindows)) {
        ysuccess(true);
        return;
    }

    Output *current_output = get_output_for_con(TAILQ_FIRST(&owindows)->con);
    Output *output = get_output_from_string(current_output, name);

    if (!output) {
        yerror("Output %s not found.", name);
        return;
    }

    /* get visible workspace on output */
    Con *ws = NULL;
    GREP_FIRST(ws, output_get_content(output->con), workspace_is_visible(child));
    if (!ws) {
        yerror("BUG: No workspace found on output.");
        return;
    }

    workspace_show(ws);

    cmd_output->needs_tree_render = true;
    ysuccess(true);
}

/*
 * Implementation of 'move [window|container] [to] [absolute] position [<pos_x> [px|ppt] <pos_y> [px|ppt]]
 *
 */
void cmd_move_window_to_position(I3_CMD, long x, const char *mode_x, long y, const char *mode_y) {
    bool has_error = false;

    owindow *current;
    HANDLE_EMPTY_MATCH;

    TAILQ_FOREACH (current, &owindows, owindows) {
        if (!con_is_floating(current->con)) {
            ELOG("Cannot change position. The window/container is not floating\n");

            if (!has_error) {
                yerror("Cannot change position of a window/container because it is not floating.");
                has_error = true;
            }

            continue;
        }

        Rect newrect = current->con->parent->rect;
        Con *output = con_get_output(current->con);

        newrect.x = mode_x && strcmp(mode_x, "ppt") == 0 ? output->rect.width * ((double)x / 100.0) : x;
        newrect.y = mode_y && strcmp(mode_y, "ppt") == 0 ? output->rect.height * ((double)y / 100.0) : y;
        DLOG("moving to position %d %s %d %s\n", newrect.x, mode_x, newrect.y, mode_y);

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
    TAILQ_FOREACH (current, &owindows, owindows) {
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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

    TAILQ_FOREACH (current, &owindows, owindows) {
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
        TAILQ_FOREACH (current, &owindows, owindows) {
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
        yerror("No match found for swapping.");
        return;
    }
    if (match->con == NULL) {
        yerror("Match %p has no container.", match);
        return;
    }

    Con *con;
    if (strcmp(mode, "id") == 0) {
        long target;
        if (!parse_long(arg, &target, 0)) {
            yerror("Failed to parse %s into a window id.", arg);
            return;
        }

        con = con_by_window_id(target);
    } else if (strcmp(mode, "con_id") == 0) {
        long target;
        if (!parse_long(arg, &target, 0)) {
            yerror("Failed to parse %s into a container id.", arg);
            return;
        }

        con = con_by_con_id(target);
    } else if (strcmp(mode, "mark") == 0) {
        con = con_by_mark(arg);
    } else {
        yerror("Unhandled swap mode \"%s\". This is a bug.", mode);
        return;
    }

    if (con == NULL) {
        yerror("Could not find container for %s = %s", mode, arg);
        return;
    }

    if (match != TAILQ_LAST(&owindows, owindows_head)) {
        LOG("More than one container matched the swap command, only using the first one.");
    }

    DLOG("Swapping %p with %p.\n", match->con, con);
    bool result = con_swap(match->con, con);

    cmd_output->needs_tree_render = true;
    // XXX: default reply for now, make this a better reply
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
    TAILQ_FOREACH (current, &owindows, owindows) {
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
        yerror("Cannot rename workspace to \"%s\": names starting with __ are i3-internal.", new_name);
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
    ipc_send_workspace_event("rename", workspace, NULL);

    Con *assigned = get_assigned_output(workspace->name, workspace->num);
    if (assigned) {
        workspace_move_to_output(workspace, get_output_for_con(assigned));
    }

    bool can_restore_focus = previously_focused != NULL;
    /* NB: If previously_focused is a workspace we can't work directly with it
     * since it might have been cleaned up by workspace_show() already,
     * depending on the focus order/number of other workspaces on the output.
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
        con_focus(previously_focused);
    }

    /* Let back-and-forth work after renaming the previous workspace.
     * See #3694. */
    if (previous_workspace_name && !strcmp(previous_workspace_name, old_name_copy)) {
        FREE(previous_workspace_name);
        previous_workspace_name = sstrdup(new_name);
    }

    cmd_output->needs_tree_render = true;
    ysuccess(true);

    ewmh_update_desktop_properties();

    startup_sequence_rename_workspace(old_name_copy, new_name);
    free(old_name_copy);
}

/*
 * Implementation of 'bar mode dock|hide|invisible|toggle [<bar_id>]'
 *
 */
void cmd_bar_mode(I3_CMD, const char *bar_mode, const char *bar_id) {
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
        assert(false);
    }

    if (TAILQ_EMPTY(&barconfigs)) {
        yerror("No bars found\n");
        return;
    }

    Barconfig *current = NULL;
    TAILQ_FOREACH (current, &barconfigs, configs) {
        if (bar_id && strcmp(current->id, bar_id) != 0) {
            continue;
        }

        if (toggle) {
            mode = (current->mode + 1) % 2;
        }

        DLOG("Changing bar mode of bar_id '%s' from '%d' to '%s (%d)'\n",
             current->id, current->mode, bar_mode, mode);
        if ((int)current->mode != mode) {
            current->mode = mode;
            ipc_send_barconfig_update_event(current);
        }

        if (bar_id) {
            /* We are looking for a specific bar and we found it */
            ysuccess(true);
            return;
        }
    }

    if (bar_id) {
        /* We are looking for a specific bar and we did not find it */
        yerror("Changing bar mode of bar_id %s failed, bar_id not found.\n", bar_id);
    } else {
        /* We have already handled the case of no bars, so we must have
         * updated all active bars now. */
        ysuccess(true);
    }
}

/*
 * Implementation of 'bar hidden_state hide|show|toggle [<bar_id>]'
 *
 */
void cmd_bar_hidden_state(I3_CMD, const char *bar_hidden_state, const char *bar_id) {
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
        assert(false);
    }

    if (TAILQ_EMPTY(&barconfigs)) {
        yerror("No bars found\n");
        return;
    }

    Barconfig *current = NULL;
    TAILQ_FOREACH (current, &barconfigs, configs) {
        if (bar_id && strcmp(current->id, bar_id) != 0) {
            continue;
        }

        if (toggle) {
            hidden_state = (current->hidden_state + 1) % 2;
        }

        DLOG("Changing bar hidden_state of bar_id '%s' from '%d' to '%s (%d)'\n",
             current->id, current->hidden_state, bar_hidden_state, hidden_state);
        if ((int)current->hidden_state != hidden_state) {
            current->hidden_state = hidden_state;
            ipc_send_barconfig_update_event(current);
        }

        if (bar_id) {
            /* We are looking for a specific bar and we found it */
            ysuccess(true);
            return;
        }
    }

    if (bar_id) {
        /* We are looking for a specific bar and we did not find it */
        yerror("Changing bar hidden_state of bar_id %s failed, bar_id not found.\n", bar_id);
    } else {
        /* We have already handled the case of no bars, so we must have
         * updated all active bars now. */
        ysuccess(true);
    }
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
            yerror("Failed to parse %s into a shmlog size.", argument);
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
