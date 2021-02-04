/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * workspaces.c: Maintaining the workspace lists
 *
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>

#include <yajl/yajl_parse.h>

/* A datatype to pass through the callbacks to save the state */
struct workspaces_json_params {
    struct ws_head *workspaces;
    i3_ws *workspaces_walk;
    char *cur_key;
    bool need_output;
    bool parsing_rect;
};

/*
 * Parse a boolean value (visible, focused, urgent)
 *
 */
static int workspaces_boolean_cb(void *params_, int val) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;

    if (!strcmp(params->cur_key, "visible")) {
        params->workspaces_walk->visible = val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "focused")) {
        params->workspaces_walk->focused = val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "urgent")) {
        params->workspaces_walk->urgent = val;
        FREE(params->cur_key);
        return 1;
    }

    FREE(params->cur_key);

    return 0;
}

/*
 * Parse an integer (num or the rect)
 *
 */
static int workspaces_integer_cb(void *params_, long long val) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;

    if (!strcmp(params->cur_key, "id")) {
        params->workspaces_walk->id = val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "num")) {
        params->workspaces_walk->num = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "x")) {
        params->workspaces_walk->rect.x = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "y")) {
        params->workspaces_walk->rect.y = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "width")) {
        params->workspaces_walk->rect.w = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "height")) {
        params->workspaces_walk->rect.h = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    FREE(params->cur_key);
    return 0;
}

/*
 * Parse a string (name, output)
 *
 */
static int workspaces_string_cb(void *params_, const unsigned char *val, size_t len) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;

    if (!strcmp(params->cur_key, "name")) {
        const char *ws_name = (const char *)val;
        params->workspaces_walk->canonical_name = sstrndup(ws_name, len);

        if ((config.strip_ws_numbers || config.strip_ws_name) && params->workspaces_walk->num >= 0) {
            /* Special case: strip off the workspace number/name */
            static char ws_num[32];

            snprintf(ws_num, sizeof(ws_num), "%d", params->workspaces_walk->num);

            /* Calculate the length of the number str in the name */
            size_t offset = strspn(ws_name, ws_num);

            /* Also strip off the conventional ws name delimiter */
            if (offset && ws_name[offset] == ':')
                offset += 1;

            if (config.strip_ws_numbers) {
                /* Offset may be equal to length, in which case display the number */
                params->workspaces_walk->name = (offset < len
                                                     ? i3string_from_markup_with_length(ws_name + offset, len - offset)
                                                     : i3string_from_markup(ws_num));
            } else {
                params->workspaces_walk->name = i3string_from_markup(ws_num);
            }
        } else {
            /* Default case: just save the name */
            params->workspaces_walk->name = i3string_from_markup_with_length(ws_name, len);
        }

        /* Save its rendered width */
        params->workspaces_walk->name_width =
            predict_text_width(params->workspaces_walk->name);

        DLOG("Got workspace canonical: %s, name: '%s', name_width: %d, glyphs: %zu\n",
             params->workspaces_walk->canonical_name,
             i3string_as_utf8(params->workspaces_walk->name),
             params->workspaces_walk->name_width,
             i3string_get_num_glyphs(params->workspaces_walk->name));
        FREE(params->cur_key);

        return 1;
    }

    if (!strcmp(params->cur_key, "output")) {
        /* We add the ws to the TAILQ of the output, it belongs to */
        char *output_name = NULL;
        sasprintf(&output_name, "%.*s", len, val);

        i3_output *target = get_output_by_name(output_name);
        i3_ws *ws = params->workspaces_walk;
        if (target != NULL) {
            ws->output = target;
            TAILQ_INSERT_TAIL(ws->output->workspaces, ws, tailq);
        }

        params->need_output = false;
        FREE(output_name);
        FREE(params->cur_key);

        return 1;
    }

    return 0;
}

/*
 * We hit the start of a JSON map (rect or a new workspace)
 *
 */
static int workspaces_start_map_cb(void *params_) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;

    if (params->cur_key == NULL) {
        i3_ws *new_workspace = scalloc(1, sizeof(i3_ws));
        new_workspace->num = -1;

        params->workspaces_walk = new_workspace;
        params->need_output = true;
        params->parsing_rect = false;
    } else {
        params->parsing_rect = true;
    }

    return 1;
}

static int workspaces_end_map_cb(void *params_) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;
    i3_ws *ws = params->workspaces_walk;
    const bool parsing_rect = params->parsing_rect;
    params->parsing_rect = false;

    if (parsing_rect || !ws || !params->need_output) {
        return 1;
    }

    ws->output = get_output_by_name("primary");
    if (ws->output == NULL) {
        ws->output = SLIST_FIRST(outputs);
    }
    TAILQ_INSERT_TAIL(ws->output->workspaces, ws, tailq);

    return 1;
}

/*
 * Parse a key.
 *
 * Essentially we just save it in the parsing state
 *
 */
static int workspaces_map_key_cb(void *params_, const unsigned char *keyVal, size_t keyLen) {
    struct workspaces_json_params *params = (struct workspaces_json_params *)params_;
    FREE(params->cur_key);
    sasprintf(&(params->cur_key), "%.*s", keyLen, keyVal);
    return 1;
}

/* A datastructure to pass all these callbacks to yajl */
static yajl_callbacks workspaces_callbacks = {
    .yajl_boolean = workspaces_boolean_cb,
    .yajl_integer = workspaces_integer_cb,
    .yajl_string = workspaces_string_cb,
    .yajl_start_map = workspaces_start_map_cb,
    .yajl_end_map = workspaces_end_map_cb,
    .yajl_map_key = workspaces_map_key_cb,
};

/*
 * Parse the received JSON string
 *
 */
void parse_workspaces_json(const unsigned char *json, size_t size) {
    DLOG("Got ws json: %.*s\n", (int)size, (const char *)json);  // TODO: delete

    free_workspaces();

    struct workspaces_json_params params = {0};
    yajl_handle handle = yajl_alloc(&workspaces_callbacks, NULL, (void *)&params);
    yajl_status state = yajl_parse(handle, json, size);

    /* FIXME: Proper error handling for JSON parsing */
    switch (state) {
        case yajl_status_ok:
            break;
        case yajl_status_client_canceled:
        case yajl_status_error:
            ELOG("Could not parse workspaces reply!\n");
            if (config.workspace_command) {
                kill_ws_child();
                set_workspace_button_error("Could not parse workspace_command's JSON");
            } else {
                exit(EXIT_FAILURE);
            }
            break;
    }

    yajl_free(handle);
    FREE(params.cur_key);
}

/*
 * free() all workspace data structures. Does not free() the heads of the tailqueues.
 *
 */
void free_workspaces(void) {
    if (outputs == NULL) {
        return;
    }

    i3_output *outputs_walk;
    SLIST_FOREACH (outputs_walk, outputs, slist) {
        if (outputs_walk->workspaces != NULL && !TAILQ_EMPTY(outputs_walk->workspaces)) {
            i3_ws *ws_walk;
            TAILQ_FOREACH (ws_walk, outputs_walk->workspaces, tailq) {
                I3STRING_FREE(ws_walk->name);
                FREE(ws_walk->canonical_name);
            }
            FREE_TAILQ(outputs_walk->workspaces, i3_ws);
        }
    }
}
