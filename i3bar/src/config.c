/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * config.c: Parses the configuration (received from i3).
 *
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <yajl/yajl_parse.h>

config_t config;
static char *cur_key;
static bool parsing_bindings;
static bool parsing_tray_outputs;

/*
 * Parse a key.
 *
 * Essentially we just save it in cur_key.
 *
 */
static int config_map_key_cb(void *params_, const unsigned char *keyVal, size_t keyLen) {
    FREE(cur_key);
    sasprintf(&(cur_key), "%.*s", keyLen, keyVal);

    if (strcmp(cur_key, "bindings") == 0) {
        parsing_bindings = true;
    }

    if (strcmp(cur_key, "tray_outputs") == 0) {
        parsing_tray_outputs = true;
    }

    return 1;
}

static int config_end_array_cb(void *params_) {
    parsing_bindings = false;
    parsing_tray_outputs = false;
    return 1;
}

/*
 * Parse a null value (current_workspace)
 *
 */
static int config_null_cb(void *params_) {
    if (!strcmp(cur_key, "id")) {
        /* If 'id' is NULL, the bar config was not found. Error out. */
        ELOG("No such bar config. Use 'i3-msg -t get_bar_config' to get the available configs.\n");
        ELOG("Are you starting i3bar by hand? You should not:\n");
        ELOG("Configure a 'bar' block in your i3 config and i3 will launch i3bar automatically.\n");
        exit(EXIT_FAILURE);
    }

    return 1;
}

/*
 * Parse a string
 *
 */
static int config_string_cb(void *params_, const unsigned char *val, size_t _len) {
    int len = (int)_len;
    /* The id and socket_path are ignored, we already know them. */
    if (!strcmp(cur_key, "id") || !strcmp(cur_key, "socket_path"))
        return 1;

    if (parsing_bindings) {
        if (strcmp(cur_key, "command") == 0) {
            binding_t *binding = TAILQ_LAST(&(config.bindings), bindings_head);
            if (binding == NULL) {
                ELOG("There is no binding to put the current command onto. This is a bug in i3.\n");
                return 0;
            }

            if (binding->command != NULL) {
                ELOG("The binding for input_code = %d already has a command. This is a bug in i3.\n", binding->input_code);
                return 0;
            }

            sasprintf(&(binding->command), "%.*s", len, val);
            return 1;
        }

        ELOG("Unknown key \"%s\" while parsing bar bindings.\n", cur_key);
        return 0;
    }

    if (parsing_tray_outputs) {
        DLOG("Adding tray_output = %.*s to the list.\n", len, val);
        tray_output_t *tray_output = scalloc(1, sizeof(tray_output_t));
        sasprintf(&(tray_output->output), "%.*s", len, val);
        TAILQ_INSERT_TAIL(&(config.tray_outputs), tray_output, tray_outputs);
        return 1;
    }

    if (!strcmp(cur_key, "mode")) {
        DLOG("mode = %.*s, len = %d\n", len, val, len);
        config.hide_on_modifier = (len == strlen("dock") && !strncmp((const char *)val, "dock", strlen("dock")) ? M_DOCK
                                                                                                                : (len == strlen("hide") && !strncmp((const char *)val, "hide", strlen("hide")) ? M_HIDE
                                                                                                                                                                                                : M_INVISIBLE));
        return 1;
    }

    if (!strcmp(cur_key, "hidden_state")) {
        DLOG("hidden_state = %.*s, len = %d\n", len, val, len);
        config.hidden_state = (len == strlen("hide") && !strncmp((const char *)val, "hide", strlen("hide")) ? S_HIDE : S_SHOW);
        return 1;
    }

    /* Kept for backwards compatibility. */
    if (!strcmp(cur_key, "modifier")) {
        DLOG("modifier = %.*s\n", len, val);
        if (len == strlen("none") && !strncmp((const char *)val, "none", strlen("none"))) {
            config.modifier = XCB_NONE;
            return 1;
        }

        if (len == strlen("shift") && !strncmp((const char *)val, "shift", strlen("shift"))) {
            config.modifier = ShiftMask;
            return 1;
        }
        if (len == strlen("ctrl") && !strncmp((const char *)val, "ctrl", strlen("ctrl"))) {
            config.modifier = ControlMask;
            return 1;
        }
        if (len == strlen("Mod") + 1 && !strncmp((const char *)val, "Mod", strlen("Mod"))) {
            switch (val[3]) {
                case '1':
                    config.modifier = Mod1Mask;
                    return 1;
                case '2':
                    config.modifier = Mod2Mask;
                    return 1;
                case '3':
                    config.modifier = Mod3Mask;
                    return 1;
                case '5':
                    config.modifier = Mod5Mask;
                    return 1;
            }
        }

        config.modifier = Mod4Mask;
        return 1;
    }

    /* This key was sent in <= 4.10.2. We keep it around to avoid breakage for
     * users updating from that version and restarting i3bar before i3. */
    if (!strcmp(cur_key, "wheel_up_cmd")) {
        DLOG("wheel_up_cmd = %.*s\n", len, val);
        binding_t *binding = scalloc(1, sizeof(binding_t));
        binding->input_code = 4;
        sasprintf(&(binding->command), "%.*s", len, val);
        TAILQ_INSERT_TAIL(&(config.bindings), binding, bindings);
        return 1;
    }

    /* This key was sent in <= 4.10.2. We keep it around to avoid breakage for
     * users updating from that version and restarting i3bar before i3. */
    if (!strcmp(cur_key, "wheel_down_cmd")) {
        DLOG("wheel_down_cmd = %.*s\n", len, val);
        binding_t *binding = scalloc(1, sizeof(binding_t));
        binding->input_code = 5;
        sasprintf(&(binding->command), "%.*s", len, val);
        TAILQ_INSERT_TAIL(&(config.bindings), binding, bindings);
        return 1;
    }

    if (!strcmp(cur_key, "position")) {
        DLOG("position = %.*s\n", len, val);
        config.position = (len == strlen("top") && !strncmp((const char *)val, "top", strlen("top")) ? POS_TOP : POS_BOT);
        return 1;
    }

    if (!strcmp(cur_key, "status_command")) {
        DLOG("status_command = %.*s\n", len, val);
        sasprintf(&config.command, "%.*s", len, val);
        return 1;
    }

    if (!strcmp(cur_key, "workspace_command")) {
        DLOG("workspace_command = %.*s\n", len, val);
        sasprintf(&config.workspace_command, "%.*s", len, val);
        return 1;
    }

    if (!strcmp(cur_key, "font")) {
        DLOG("font = %.*s\n", len, val);
        FREE(config.fontname);
        sasprintf(&config.fontname, "%.*s", len, val);
        return 1;
    }

    if (!strcmp(cur_key, "separator_symbol")) {
        DLOG("separator = %.*s\n", len, val);
        I3STRING_FREE(config.separator_symbol);
        config.separator_symbol = i3string_from_utf8_with_length((const char *)val, len);
        return 1;
    }

    if (!strcmp(cur_key, "outputs")) {
        DLOG("+output %.*s\n", len, val);
        int new_num_outputs = config.num_outputs + 1;
        config.outputs = srealloc(config.outputs, sizeof(char *) * new_num_outputs);
        sasprintf(&config.outputs[config.num_outputs], "%.*s", len, val);
        config.num_outputs = new_num_outputs;
        return 1;
    }

    /* We keep the old single tray_output working for users who only restart i3bar
     * after updating. */
    if (!strcmp(cur_key, "tray_output")) {
        DLOG("Found deprecated key tray_output %.*s.\n", len, val);
        tray_output_t *tray_output = scalloc(1, sizeof(tray_output_t));
        sasprintf(&(tray_output->output), "%.*s", len, val);
        TAILQ_INSERT_TAIL(&(config.tray_outputs), tray_output, tray_outputs);
        return 1;
    }

#define COLOR(json_name, struct_name)                                  \
    do {                                                               \
        if (!strcmp(cur_key, #json_name)) {                            \
            DLOG(#json_name " = " #struct_name " = %.*s\n", len, val); \
            sasprintf(&(config.colors.struct_name), "%.*s", len, val); \
            return 1;                                                  \
        }                                                              \
    } while (0)

    COLOR(statusline, bar_fg);
    COLOR(background, bar_bg);
    COLOR(separator, sep_fg);
    COLOR(focused_statusline, focus_bar_fg);
    COLOR(focused_background, focus_bar_bg);
    COLOR(focused_separator, focus_sep_fg);
    COLOR(focused_workspace_border, focus_ws_border);
    COLOR(focused_workspace_bg, focus_ws_bg);
    COLOR(focused_workspace_text, focus_ws_fg);
    COLOR(active_workspace_border, active_ws_border);
    COLOR(active_workspace_bg, active_ws_bg);
    COLOR(active_workspace_text, active_ws_fg);
    COLOR(inactive_workspace_border, inactive_ws_border);
    COLOR(inactive_workspace_bg, inactive_ws_bg);
    COLOR(inactive_workspace_text, inactive_ws_fg);
    COLOR(urgent_workspace_border, urgent_ws_border);
    COLOR(urgent_workspace_bg, urgent_ws_bg);
    COLOR(urgent_workspace_text, urgent_ws_fg);
    COLOR(binding_mode_border, binding_mode_border);
    COLOR(binding_mode_bg, binding_mode_bg);
    COLOR(binding_mode_text, binding_mode_fg);

    printf("got unexpected string %.*s for cur_key = %s\n", len, val, cur_key);

    return 0;
}

/*
 * Parse a boolean value
 *
 */
static int config_boolean_cb(void *params_, int val) {
    if (parsing_bindings) {
        if (strcmp(cur_key, "release") == 0) {
            binding_t *binding = TAILQ_LAST(&(config.bindings), bindings_head);
            if (binding == NULL) {
                ELOG("There is no binding to put the current command onto. This is a bug in i3.\n");
                return 0;
            }

            binding->release = val;
            return 1;
        }

        ELOG("Unknown key \"%s\" while parsing bar bindings.\n", cur_key);
    }

    if (!strcmp(cur_key, "binding_mode_indicator")) {
        DLOG("binding_mode_indicator = %d\n", val);
        config.disable_binding_mode_indicator = !val;
        return 1;
    }

    if (!strcmp(cur_key, "workspace_buttons")) {
        DLOG("workspace_buttons = %d\n", val);
        config.disable_ws = !val;
        return 1;
    }

    if (!strcmp(cur_key, "strip_workspace_numbers")) {
        DLOG("strip_workspace_numbers = %d\n", val);
        config.strip_ws_numbers = val;
        return 1;
    }

    if (!strcmp(cur_key, "strip_workspace_name")) {
        DLOG("strip_workspace_name = %d\n", val);
        config.strip_ws_name = val;
        return 1;
    }

    if (!strcmp(cur_key, "verbose")) {
        if (!config.verbose) {
            DLOG("verbose = %d\n", val);
            config.verbose = val;
        }
        return 1;
    }

    return 0;
}

/*
 * Parse an integer value
 *
 */
static int config_integer_cb(void *params_, long long val) {
    if (parsing_bindings) {
        if (strcmp(cur_key, "input_code") == 0) {
            binding_t *binding = scalloc(1, sizeof(binding_t));
            binding->input_code = val;
            TAILQ_INSERT_TAIL(&(config.bindings), binding, bindings);

            return 1;
        }

        ELOG("Unknown key \"%s\" while parsing bar bindings.\n", cur_key);
        return 0;
    }

    if (!strcmp(cur_key, "tray_padding")) {
        DLOG("tray_padding = %lld\n", val);
        config.tray_padding = val;
        return 1;
    }

    if (!strcmp(cur_key, "modifier")) {
        DLOG("modifier = %lld\n", val);
        config.modifier = (uint32_t)val;
        return 1;
    }

    if (!strcmp(cur_key, "workspace_min_width")) {
        DLOG("workspace_min_width = %lld\n", val);
        config.ws_min_width = val;
        return 1;
    }

    return 0;
}

/* A datastructure to pass all these callbacks to yajl */
static yajl_callbacks outputs_callbacks = {
    .yajl_null = config_null_cb,
    .yajl_boolean = config_boolean_cb,
    .yajl_integer = config_integer_cb,
    .yajl_string = config_string_cb,
    .yajl_end_array = config_end_array_cb,
    .yajl_map_key = config_map_key_cb,
};

/*
 * Parse the received bar configuration JSON string
 *
 */
void parse_config_json(const unsigned char *json, size_t size) {
    TAILQ_INIT(&(config.bindings));
    TAILQ_INIT(&(config.tray_outputs));

    yajl_handle handle = yajl_alloc(&outputs_callbacks, NULL, NULL);
    yajl_status state = yajl_parse(handle, json, size);

    /* FIXME: Proper error handling for JSON parsing */
    switch (state) {
        case yajl_status_ok:
            break;
        case yajl_status_client_canceled:
        case yajl_status_error:
            ELOG("Could not parse config reply!\n");
            exit(EXIT_FAILURE);
            break;
    }

    if (config.disable_ws && config.workspace_command) {
        ELOG("You have specified 'workspace_buttons no'. Your 'workspace_command %s' will be ignored.\n", config.workspace_command);
        FREE(config.workspace_command);
    }

    yajl_free(handle);
}

static int i3bar_config_string_cb(void *params_, const unsigned char *val, size_t _len) {
    sasprintf(&config.bar_id, "%.*s", (int)_len, val);
    return 0; /* Stop parsing */
}

/*
 * Parse the received bar configuration list. The only usecase right now is to
 * automatically get the first bar id.
 *
 */
void parse_get_first_i3bar_config(const unsigned char *json, size_t size) {
    yajl_callbacks configs_callbacks = {
        .yajl_string = i3bar_config_string_cb,
    };
    yajl_handle handle = yajl_alloc(&configs_callbacks, NULL, NULL);
    yajl_parse(handle, json, size);
    yajl_free(handle);
}

/*
 * free()s the color strings as soon as they are not needed anymore.
 *
 */
void free_colors(struct xcb_color_strings_t *colors) {
#define FREE_COLOR(x)    \
    do {                 \
        FREE(colors->x); \
    } while (0)
    FREE_COLOR(bar_fg);
    FREE_COLOR(bar_bg);
    FREE_COLOR(sep_fg);
    FREE_COLOR(focus_bar_fg);
    FREE_COLOR(focus_bar_bg);
    FREE_COLOR(focus_sep_fg);
    FREE_COLOR(active_ws_fg);
    FREE_COLOR(active_ws_bg);
    FREE_COLOR(active_ws_border);
    FREE_COLOR(inactive_ws_fg);
    FREE_COLOR(inactive_ws_bg);
    FREE_COLOR(inactive_ws_border);
    FREE_COLOR(urgent_ws_fg);
    FREE_COLOR(urgent_ws_bg);
    FREE_COLOR(urgent_ws_border);
    FREE_COLOR(focus_ws_fg);
    FREE_COLOR(focus_ws_bg);
    FREE_COLOR(focus_ws_border);
    FREE_COLOR(binding_mode_fg);
    FREE_COLOR(binding_mode_bg);
    FREE_COLOR(binding_mode_border);
#undef FREE_COLOR
}
