/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * mode.c: Handle mode event and show current binding mode in the bar
 *
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>

#include <yajl/yajl_parse.h>

/* A datatype to pass through the callbacks to save the state */
struct mode_json_params {
    char *cur_key;
    char *name;
    bool pango_markup;
    mode *mode;
};

/*
 * Parse a string (change)
 *
 */
static int mode_string_cb(void *params_, const unsigned char *val, size_t len) {
    struct mode_json_params *params = (struct mode_json_params *)params_;

    if (!strcmp(params->cur_key, "change")) {
        sasprintf(&(params->name), "%.*s", len, val);
        FREE(params->cur_key);
        return 1;
    }

    FREE(params->cur_key);
    return 0;
}

/*
 * Parse a boolean.
 *
 */
static int mode_boolean_cb(void *params_, int val) {
    struct mode_json_params *params = (struct mode_json_params *)params_;

    if (strcmp(params->cur_key, "pango_markup") == 0) {
        DLOG("Setting pango_markup to %d.\n", val);
        params->pango_markup = val;

        FREE(params->cur_key);
        return 1;
    }

    FREE(params->cur_key);
    return 0;
}

/*
 * Parse a key.
 *
 * Essentially we just save it in the parsing state
 *
 */
static int mode_map_key_cb(void *params_, const unsigned char *keyVal, size_t keyLen) {
    struct mode_json_params *params = (struct mode_json_params *)params_;
    FREE(params->cur_key);
    sasprintf(&(params->cur_key), "%.*s", keyLen, keyVal);
    return 1;
}

static int mode_end_map_cb(void *params_) {
    struct mode_json_params *params = (struct mode_json_params *)params_;

    /* Save the name */
    params->mode->name = i3string_from_utf8(params->name);
    i3string_set_markup(params->mode->name, params->pango_markup);
    /* Save its rendered width */
    params->mode->name_width = predict_text_width(params->mode->name);

    DLOG("Got mode change: %s\n", i3string_as_utf8(params->mode->name));
    FREE(params->cur_key);

    return 1;
}

/* A datastructure to pass all these callbacks to yajl */
static yajl_callbacks mode_callbacks = {
    .yajl_string = mode_string_cb,
    .yajl_boolean = mode_boolean_cb,
    .yajl_map_key = mode_map_key_cb,
    .yajl_end_map = mode_end_map_cb,
};

/*
 * Parse the received JSON string
 *
 */
void parse_mode_json(const unsigned char *json, size_t size) {
    struct mode_json_params params;
    mode binding;
    params.cur_key = NULL;
    params.mode = &binding;

    yajl_handle handle = yajl_alloc(&mode_callbacks, NULL, (void *)&params);
    yajl_status state = yajl_parse(handle, json, size);

    /* FIXME: Proper error handling for JSON parsing */
    switch (state) {
        case yajl_status_ok:
            break;
        case yajl_status_client_canceled:
        case yajl_status_error:
            ELOG("Could not parse mode event!\n");
            exit(EXIT_FAILURE);
            break;
    }

    /* We don't want to indicate default binding mode */
    if (strcmp("default", i3string_as_utf8(params.mode->name)) == 0)
        I3STRING_FREE(params.mode->name);

    /* Set the new binding mode */
    set_current_mode(&binding);

    yajl_free(handle);

    FREE(params.cur_key);
}
