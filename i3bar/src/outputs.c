/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * outputs.c: Maintaining the outputs list
 *
 */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yajl/yajl_parse.h>

/* A datatype to pass through the callbacks to save the state */
struct outputs_json_params {
    struct outputs_head *outputs;
    i3_output *outputs_walk;
    char *cur_key;
    bool in_rect;
};

/*
 * Parse a null value (current_workspace)
 *
 */
static int outputs_null_cb(void *params_) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;

    FREE(params->cur_key);

    return 1;
}

/*
 * Parse a boolean value (active)
 *
 */
static int outputs_boolean_cb(void *params_, int val) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;

    if (!strcmp(params->cur_key, "active")) {
        params->outputs_walk->active = val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "primary")) {
        params->outputs_walk->primary = val;
        FREE(params->cur_key);
        return 1;
    }

    return 0;
}

/*
 * Parse an integer (current_workspace or the rect)
 *
 */
static int outputs_integer_cb(void *params_, long long val) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;

    if (!strcmp(params->cur_key, "current_workspace")) {
        params->outputs_walk->ws = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "x")) {
        params->outputs_walk->rect.x = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "y")) {
        params->outputs_walk->rect.y = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "width")) {
        params->outputs_walk->rect.w = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    if (!strcmp(params->cur_key, "height")) {
        params->outputs_walk->rect.h = (int)val;
        FREE(params->cur_key);
        return 1;
    }

    return 0;
}

/*
 * Parse a string (name)
 *
 */
static int outputs_string_cb(void *params_, const unsigned char *val, size_t len) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;

    if (!strcmp(params->cur_key, "current_workspace")) {
        char *copy = NULL;
        sasprintf(&copy, "%.*s", len, val);

        char *end;
        errno = 0;
        long parsed_num = strtol(copy, &end, 10);
        if (errno == 0 &&
            (end && *end == '\0'))
            params->outputs_walk->ws = parsed_num;

        FREE(copy);
        FREE(params->cur_key);
        return 1;
    }

    if (strcmp(params->cur_key, "name")) {
        return 0;
    }

    sasprintf(&(params->outputs_walk->name), "%.*s", len, val);

    FREE(params->cur_key);
    return 1;
}

/*
 * We hit the start of a JSON map (rect or a new output)
 *
 */
static int outputs_start_map_cb(void *params_) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;
    i3_output *new_output = NULL;

    if (params->cur_key == NULL) {
        new_output = smalloc(sizeof(i3_output));
        new_output->name = NULL;
        new_output->active = false;
        new_output->primary = false;
        new_output->visible = false;
        new_output->ws = 0,
        new_output->statusline_width = 0;
        new_output->statusline_short_text = false;
        memset(&new_output->rect, 0, sizeof(rect));
        memset(&new_output->bar, 0, sizeof(surface_t));
        memset(&new_output->buffer, 0, sizeof(surface_t));
        memset(&new_output->statusline_buffer, 0, sizeof(surface_t));

        new_output->workspaces = smalloc(sizeof(struct ws_head));
        TAILQ_INIT(new_output->workspaces);

        new_output->trayclients = smalloc(sizeof(struct tc_head));
        TAILQ_INIT(new_output->trayclients);

        params->outputs_walk = new_output;

        return 1;
    }

    if (!strcmp(params->cur_key, "rect")) {
        params->in_rect = true;
    }

    return 1;
}

static void clear_output(i3_output *output) {
    FREE(output->name);
    FREE(output->workspaces);
    FREE(output->trayclients);
}

/*
 * We hit the end of a map (rect or a new output)
 *
 */
static int outputs_end_map_cb(void *params_) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;
    if (params->in_rect) {
        params->in_rect = false;
        /* Ignore the end of a rect */
        return 1;
    }

    /* See if we actually handle that output */
    if (config.num_outputs > 0) {
        const bool is_primary = params->outputs_walk->primary;
        bool handle_output = false;
        for (int c = 0; c < config.num_outputs; c++) {
            if ((strcasecmp(params->outputs_walk->name, config.outputs[c]) == 0) ||
                (strcasecmp(config.outputs[c], "primary") == 0 && is_primary) ||
                (strcasecmp(config.outputs[c], "nonprimary") == 0 && !is_primary)) {
                handle_output = true;
                break;
            }
        }
        if (!handle_output) {
            DLOG("Ignoring output \"%s\", not configured to handle it.\n",
                 params->outputs_walk->name);
            clear_output(params->outputs_walk);
            FREE(params->outputs_walk);
            FREE(params->cur_key);
            return 1;
        }
    }

    i3_output *target = get_output_by_name(params->outputs_walk->name);

    if (target == NULL) {
        SLIST_INSERT_HEAD(outputs, params->outputs_walk, slist);
    } else {
        target->active = params->outputs_walk->active;
        target->primary = params->outputs_walk->primary;
        target->ws = params->outputs_walk->ws;
        target->rect = params->outputs_walk->rect;

        clear_output(params->outputs_walk);
        FREE(params->outputs_walk);
    }
    return 1;
}

/*
 * Parse a key.
 *
 * Essentially we just save it in the parsing state
 *
 */
static int outputs_map_key_cb(void *params_, const unsigned char *keyVal, size_t keyLen) {
    struct outputs_json_params *params = (struct outputs_json_params *)params_;
    FREE(params->cur_key);
    sasprintf(&(params->cur_key), "%.*s", keyLen, keyVal);
    return 1;
}

/* A datastructure to pass all these callbacks to yajl */
static yajl_callbacks outputs_callbacks = {
    .yajl_null = outputs_null_cb,
    .yajl_boolean = outputs_boolean_cb,
    .yajl_integer = outputs_integer_cb,
    .yajl_string = outputs_string_cb,
    .yajl_start_map = outputs_start_map_cb,
    .yajl_map_key = outputs_map_key_cb,
    .yajl_end_map = outputs_end_map_cb,
};

struct outputs_head *outputs;
/*
 * Initiate the outputs list
 *
 */
void init_outputs(void) {
    outputs = smalloc(sizeof(struct outputs_head));
    SLIST_INIT(outputs);
}

/*
 * Parse the received JSON string
 *
 */
void parse_outputs_json(const unsigned char *json, size_t size) {
    struct outputs_json_params params;
    params.outputs_walk = NULL;
    params.cur_key = NULL;
    params.in_rect = false;

    yajl_handle handle = yajl_alloc(&outputs_callbacks, NULL, (void *)&params);
    yajl_status state = yajl_parse(handle, json, size);

    /* FIXME: Proper errorhandling for JSON-parsing */
    switch (state) {
        case yajl_status_ok:
            break;
        case yajl_status_client_canceled:
        case yajl_status_error:
            ELOG("Could not parse outputs reply!\n");
            exit(EXIT_FAILURE);
            break;
    }

    yajl_free(handle);
}

/*
 * free() all outputs data structures.
 *
 */
void free_outputs(void) {
    free_workspaces();

    i3_output *outputs_walk;
    if (outputs == NULL) {
        return;
    }
    SLIST_FOREACH (outputs_walk, outputs, slist) {
        destroy_window(outputs_walk);
        if (outputs_walk->trayclients != NULL && !TAILQ_EMPTY(outputs_walk->trayclients)) {
            FREE_TAILQ(outputs_walk->trayclients, trayclient);
        }
        clear_output(outputs_walk);
    }
    FREE_SLIST(outputs, i3_output);
}

/*
 * Returns the output with the given name
 *
 */
i3_output *get_output_by_name(char *name) {
    if (name == NULL) {
        return NULL;
    }
    const bool is_primary = !strcasecmp(name, "primary");

    i3_output *walk;
    SLIST_FOREACH (walk, outputs, slist) {
        if ((is_primary && walk->primary) || !strcmp(walk->name, name)) {
            break;
        }
    }

    return walk;
}

/*
 * Returns true if the output has the currently focused workspace
 *
 */
bool output_has_focus(i3_output *output) {
    i3_ws *ws_walk;
    TAILQ_FOREACH (ws_walk, output->workspaces, tailq) {
        if (ws_walk->focused) {
            return true;
        }
    }
    return false;
}
