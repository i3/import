/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3-input/main.c: Utility which lets the user input commands and sends them
 *                  to i3.
 *
 */
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_keysyms.h>

xcb_visualtype_t *visual_type = NULL;
#include "i3-input.h"
#include "keysym2ucs.h"
#include "libi3.h"

#include <X11/keysym.h>

#define MAX_WIDTH logical_px(500)

/* Exit codes for i3-input:
 * 0 if i3-input exited successfully and the command was run
 * 1 if the user canceled input
 * 2 if i3-input fails for any other reason */
const int EXIT_OK = 0;
const int EXIT_CANCEL = 1;
const int EXIT_ERROR = 2;

/* IPC format string. %s will be replaced with what the user entered, then
 * the command will be sent to i3 */
static char *format;

static int sockfd;
static xcb_key_symbols_t *symbols;
static bool modeswitch_active = false;
static xcb_window_t win;
static surface_t surface;
static xcb_char2b_t glyphs_ucs[512];
static char *glyphs_utf8[512];
static int input_position;
static i3Font font;
static i3String *prompt;
static int prompt_offset = 0;
static int limit;
xcb_window_t root;
xcb_connection_t *conn;
xcb_screen_t *root_screen;
struct frame_options {
    int border;
    int padding;
    color_t border_color;
    color_t fg_color;
    color_t bg_color;
};

/*
 * Having verboselog(), errorlog() and debuglog() is necessary when using libi3.
 *
 */
void verboselog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

void errorlog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void debuglog(char *fmt, ...) {
}

/*
 * Concats the glyphs (either UCS-2 or UTF-8) to a single string, suitable for
 * rendering it (UCS-2) or sending it to i3 (UTF-8).
 *
 */
static uint8_t *concat_strings(char **glyphs, int max) {
    uint8_t *output = scalloc(max + 1, 4);
    uint8_t *walk = output;
    for (int c = 0; c < max; c++) {
        printf("at %c\n", glyphs[c][0]);
        /* if the first byte is 0, this has to be UCS2 */
        if (glyphs[c][0] == '\0') {
            memcpy(walk, glyphs[c], 2);
            walk += 2;
        } else {
            strcpy((char *)walk, glyphs[c]);
            walk += strlen(glyphs[c]);
        }
    }
    printf("output = %s\n", output);
    return output;
}

/*
 * Handles expose events (redraws of the window) and rendering in general. Will
 * be called from the code with event == NULL or from X with event != NULL.
 *
 */
static int handle_expose(void *data, xcb_connection_t *conn, xcb_expose_event_t *event, struct frame_options *frameopts) {
    printf("expose!\n");

    int text_offset = frameopts->border + frameopts->padding;

    /* draw border */
    draw_util_rectangle(&surface, frameopts->border_color, 0, 0, surface.width, surface.height);

    /* draw background */
    draw_util_rectangle(&surface, frameopts->bg_color, frameopts->border, frameopts->border, surface.width - 2 * frameopts->border, surface.height - 2 * frameopts->border);

    /* draw the prompt … */
    if (prompt != NULL) {
        draw_util_text(prompt, &surface, frameopts->fg_color, frameopts->bg_color, text_offset, text_offset, MAX_WIDTH - text_offset);
    }

    /* … and the text */
    if (input_position > 0) {
        i3String *input = i3string_from_ucs2(glyphs_ucs, input_position);
        draw_util_text(input, &surface, frameopts->fg_color, frameopts->bg_color, text_offset + prompt_offset, text_offset, MAX_WIDTH - text_offset);
        i3string_free(input);
    }

    xcb_flush(conn);

    return 1;
}

/*
 * Deactivates the Mode_switch bit upon release of the Mode_switch key.
 *
 */
static int handle_key_release(void *ignored, xcb_connection_t *conn, xcb_key_release_event_t *event) {
    printf("releasing %d, state raw = %d\n", event->detail, event->state);

    xcb_keysym_t sym = xcb_key_press_lookup_keysym(symbols, event, event->state);
    if (sym == XK_Mode_switch) {
        printf("Mode switch disabled\n");
        modeswitch_active = false;
    }

    return 1;
}

static void finish_input(void) {
    char *command = (char *)concat_strings(glyphs_utf8, input_position);

    /* count the occurrences of %s in the string */
    int c;
    int len = strlen(format);
    int cnt = 0;
    for (c = 0; c < (len - 1); c++)
        if (format[c] == '%' && format[c + 1] == 's')
            cnt++;
    printf("occurrences = %d\n", cnt);

    /* allocate space for the output */
    int inputlen = strlen(command);
    char *full = scalloc(strlen(format) - (2 * cnt) /* format without all %s */
                             + (inputlen * cnt)     /* replaced %s */
                             + 1,                   /* trailing NUL */
                         1);
    char *dest = full;
    for (c = 0; c < len; c++) {
        /* if this is not % or it is % but without a following 's',
         * just copy the character */
        if (format[c] != '%' || (c == (len - 1)) || format[c + 1] != 's')
            *(dest++) = format[c];
        else {
            strncat(dest, command, inputlen);
            dest += inputlen;
            /* skip the following 's' of '%s' */
            c++;
        }
    }

    /* prefix the command if a prefix was specified on commandline */
    printf("command = %s\n", full);

    int ret = ipc_send_message(sockfd, strlen(full), 0, (uint8_t *)full);

    free(full);

    exit(ret == 0 ? EXIT_OK : EXIT_ERROR);
}

/*
 * Handles keypresses by converting the keycodes to keysymbols, then the
 * keysymbols to UCS-2. If the conversion succeeded, the glyph is saved in the
 * internal buffers and displayed in the input window.
 *
 * Also handles backspace (deleting one character) and return (sending the
 * command to i3).
 *
 */
static int handle_key_press(void *ignored, xcb_connection_t *conn, xcb_key_press_event_t *event, struct frame_options *frameopts) {
    printf("Keypress %d, state raw = %d\n", event->detail, event->state);

    // TODO: port the input handling code from i3lock once libxkbcommon ≥ 0.5.0
    // is available in distros.

    /* See the documentation of xcb_key_symbols_get_keysym for this one.
     * Basically: We get either col 0 or col 1, depending on whether shift is
     * pressed. */
    int col = (event->state & XCB_MOD_MASK_SHIFT);

    /* If modeswitch is currently active, we need to look in group 2 or 3,
     * respectively. */
    if (modeswitch_active)
        col += 2;

    xcb_keysym_t sym = xcb_key_press_lookup_keysym(symbols, event, col);
    if (sym == XK_Mode_switch) {
        printf("Mode switch enabled\n");
        modeswitch_active = true;
        return 1;
    }

    if (sym == XK_Return)
        finish_input();

    if (sym == XK_BackSpace) {
        if (input_position == 0)
            return 1;

        input_position--;
        free(glyphs_utf8[input_position]);

        handle_expose(NULL, conn, NULL, frameopts);
        return 1;
    }
    if (sym == XK_Escape) {
        exit(EXIT_CANCEL);
    }

    /* TODO: handle all of these? */
    printf("is_keypad_key = %d\n", xcb_is_keypad_key(sym));
    printf("is_private_keypad_key = %d\n", xcb_is_private_keypad_key(sym));
    printf("xcb_is_cursor_key = %d\n", xcb_is_cursor_key(sym));
    printf("xcb_is_pf_key = %d\n", xcb_is_pf_key(sym));
    printf("xcb_is_function_key = %d\n", xcb_is_function_key(sym));
    printf("xcb_is_misc_function_key = %d\n", xcb_is_misc_function_key(sym));
    printf("xcb_is_modifier_key = %d\n", xcb_is_modifier_key(sym));

    if (xcb_is_modifier_key(sym) || xcb_is_cursor_key(sym))
        return 1;

    printf("sym = %c (%d)\n", sym, sym);

    /* convert the keysym to UCS */
    uint16_t ucs = keysym2ucs(sym);
    if ((int16_t)ucs == -1) {
        fprintf(stderr, "Keysym could not be converted to UCS, skipping\n");
        return 1;
    }

    xcb_char2b_t inp;
    inp.byte1 = (ucs & 0xff00) >> 2;
    inp.byte2 = (ucs & 0x00ff) >> 0;

    printf("inp.byte1 = %02x, inp.byte2 = %02x\n", inp.byte1, inp.byte2);
    /* convert it to UTF-8 */
    char *out = convert_ucs2_to_utf8(&inp, 1);
    printf("converted to %s\n", out);

    glyphs_ucs[input_position] = inp;
    glyphs_utf8[input_position] = out;
    input_position++;

    if (input_position == limit)
        finish_input();

    handle_expose(NULL, conn, NULL, frameopts);
    return 1;
}

static xcb_rectangle_t get_window_position(struct frame_options *frameopts) {
    xcb_rectangle_t result = (xcb_rectangle_t){logical_px(50), logical_px(50), MAX_WIDTH, font.height + 2 * frameopts->border + 2 * frameopts->padding};

    xcb_get_property_reply_t *supporting_wm_reply = NULL;
    xcb_get_input_focus_reply_t *input_focus = NULL;
    xcb_get_geometry_reply_t *geometry = NULL;
    xcb_get_property_reply_t *wm_class = NULL;
    xcb_translate_coordinates_reply_t *coordinates = NULL;

    xcb_atom_t A__NET_SUPPORTING_WM_CHECK;
    xcb_intern_atom_cookie_t nswc_cookie = xcb_intern_atom(conn, 0, strlen("_NET_SUPPORTING_WM_CHECK"), "_NET_SUPPORTING_WM_CHECK");
    xcb_intern_atom_reply_t *nswc_reply = xcb_intern_atom_reply(conn, nswc_cookie, NULL);
    if (nswc_reply == NULL) {
        ELOG("Could not intern atom _NET_SUPPORTING_WM_CHECK\n");
        exit(EXIT_ERROR);
    }
    A__NET_SUPPORTING_WM_CHECK = nswc_reply->atom;
    free(nswc_reply);

    supporting_wm_reply = xcb_get_property_reply(
        conn, xcb_get_property(conn, false, root, A__NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 0, 32), NULL);
    xcb_window_t *supporting_wm_win = NULL;
    if (supporting_wm_reply == NULL || xcb_get_property_value_length(supporting_wm_reply) == 0) {
        DLOG("Could not determine EWMH support window.\n");
    } else {
        supporting_wm_win = xcb_get_property_value(supporting_wm_reply);
    }

    /* In rare cases, the window holding the input focus might disappear while we are figuring out its
     * position. To avoid this, we grab the server in the meantime. */
    xcb_grab_server(conn);

    input_focus = xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL);
    if (input_focus == NULL || input_focus->focus == XCB_NONE) {
        DLOG("Failed to receive the current input focus or no window has the input focus right now.\n");
        goto free_resources;
    }

    /* We need to ignore the EWMH support window to which the focus can be set if there's no suitable window to focus. */
    if (supporting_wm_win != NULL && input_focus->focus == *supporting_wm_win) {
        DLOG("Input focus is on the EWMH support window, ignoring.\n");
        goto free_resources;
    }

    geometry = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, input_focus->focus), NULL);
    if (geometry == NULL) {
        DLOG("Failed to received window geometry.\n");
        goto free_resources;
    }

    wm_class = xcb_get_property_reply(
        conn, xcb_get_property(conn, false, input_focus->focus, XCB_ATOM_WM_CLASS, XCB_GET_PROPERTY_TYPE_ANY, 0, 32), NULL);

    /* We need to find out whether the input focus is on an i3 frame window. If it is, we must not translate the coordinates. */
    if (wm_class == NULL || xcb_get_property_value_length(wm_class) == 0 || strcmp(xcb_get_property_value(wm_class), "i3-frame") != 0) {
        coordinates = xcb_translate_coordinates_reply(
            conn, xcb_translate_coordinates(conn, input_focus->focus, root, geometry->x, geometry->y), NULL);
        if (coordinates == NULL) {
            DLOG("Failed to translate coordinates.\n");
            goto free_resources;
        }

        DLOG("Determined coordinates of window with input focus at x = %i / y = %i.\n", coordinates->dst_x, coordinates->dst_y);
        result.x += coordinates->dst_x;
        result.y += coordinates->dst_y;
    } else {
        DLOG("Determined coordinates of window with input focus at x = %i / y = %i.\n", geometry->x, geometry->y);
        result.x += geometry->x;
        result.y += geometry->y;
    }

free_resources:
    xcb_ungrab_server(conn);
    xcb_flush(conn);

    FREE(supporting_wm_reply);
    FREE(input_focus);
    FREE(geometry);
    FREE(wm_class);
    FREE(coordinates);
    return result;
}

int main(int argc, char *argv[]) {
    char *socket_path = NULL;
    char *pattern = NULL;
    int o, option_index = 0;

    struct frame_options *frameopts = &(struct frame_options){
        .border_color = draw_util_hex_to_color("#FF0000"),
        .fg_color = draw_util_hex_to_color("#FFFFFF"),
        .bg_color = draw_util_hex_to_color("#000000"),
        .border = 2,
        .padding = 2};

    static struct option long_options[] = {
        {"socket", required_argument, 0, 's'},
        {"version", no_argument, 0, 'v'},
        {"limit", required_argument, 0, 'l'},
        {"prompt", required_argument, 0, 'P'},
        {"prefix", required_argument, 0, 'p'},
        {"format", required_argument, 0, 'F'},
        {"font", required_argument, 0, 'f'},
        {"fg-color", required_argument, 0, 'C'},
        {"bg-color", required_argument, 0, 'B'},
        {"border-color", required_argument, 0, 'W'},
        {"border", required_argument, 0, 'b'},
        {"padding", required_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    char *options_string = "s:p:P:f:l:F:C:B:W:b:S:vh";

    while ((o = getopt_long(argc, argv, options_string, long_options, &option_index)) != -1) {
        switch (o) {
            case 's':
                FREE(socket_path);
                socket_path = sstrdup(optarg);
                break;
            case 'v':
                printf("i3-input " I3_VERSION);
                return EXIT_OK;
            case 'p':
                /* This option is deprecated, but will still work in i3 v4.1, 4.2 and 4.3 */
                fprintf(stderr, "i3-input: WARNING: the -p option is DEPRECATED in favor of the -F (format) option\n");
                FREE(format);
                sasprintf(&format, "%s%%s", optarg);
                break;
            case 'l':
                limit = atoi(optarg);
                break;
            case 'P':
                i3string_free(prompt);
                prompt = i3string_from_utf8(optarg);
                break;
            case 'f':
                FREE(pattern);
                pattern = sstrdup(optarg);
                break;
            case 'C':
                frameopts->fg_color = draw_util_hex_to_color(optarg);
                break;
            case 'B':
                frameopts->bg_color = draw_util_hex_to_color(optarg);
                break;
            case 'W':
                frameopts->border_color = draw_util_hex_to_color(optarg);
                break;
            case 'b':
                frameopts->border = logical_px(atoi(optarg));
                break;
            case 'S':
                frameopts->padding = logical_px(atoi(optarg));
                break;
            case 'F':
                FREE(format);
                format = sstrdup(optarg);
                break;
            case 'h':
                printf("i3-input " I3_VERSION "\n");
                printf("i3-input [-s <socket>] [-F <format>] [-l <limit>] [-P <prompt>] [-f <font>] [-C <fg-color>] [-B <bg-color>] [-W <border-color>] [-S <padding>] [-b <border>] [-v]\n");
                printf("\n");
                printf("Example:\n");
                printf("    i3-input -F 'workspace \"%%s\"' -P 'Switch to workspace: '\n");
                return EXIT_OK;
        }
    }
    if (!format) {
        format = "%s";
    }

    printf("using format \"%s\"\n", format);

    int screen;
    conn = xcb_connect(NULL, &screen);
    if (!conn || xcb_connection_has_error(conn))
        die("Cannot open display");

    sockfd = ipc_connect(socket_path);

    root_screen = xcb_aux_get_screen(conn, screen);
    root = root_screen->root;

    symbols = xcb_key_symbols_alloc(conn);

    init_dpi();
    font = load_font(pattern ? pattern : "pango:monospace 8", true);
    set_font(&font);

    if (prompt != NULL)
        prompt_offset = predict_text_width(prompt);

    const xcb_rectangle_t win_pos = get_window_position(frameopts);

    /* Open an input window */
    win = xcb_generate_id(conn);
    xcb_create_window(
        conn,
        XCB_COPY_FROM_PARENT,
        win,                                                 /* the window id */
        root,                                                /* parent == root */
        win_pos.x, win_pos.y, win_pos.width, win_pos.height, /* dimensions */
        0,                                                   /* X11 border = 0, we draw our own */
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_WINDOW_CLASS_COPY_FROM_PARENT, /* copy visual from parent */
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
        (uint32_t[]){
            0, /* back pixel: black */
            1, /* override redirect: don’t manage this window */
            XCB_EVENT_MASK_EXPOSURE});

    /* Map the window (make it visible) */
    xcb_map_window(conn, win);

    /* Initialize the drawable surface */
    draw_util_surface_init(conn, &surface, win, get_visualtype(root_screen), win_pos.width, win_pos.height);

    /* Grab the keyboard to get all input */
    xcb_flush(conn);

    /* Try (repeatedly, if necessary) to grab the keyboard. We might not
     * get the keyboard at the first attempt because of the keybinding
     * still being active when started via a wm’s keybinding. */
    xcb_grab_keyboard_cookie_t cookie;
    xcb_grab_keyboard_reply_t *reply = NULL;

    int count = 0;
    while ((reply == NULL || reply->status != XCB_GRAB_STATUS_SUCCESS) && (count++ < 500)) {
        cookie = xcb_grab_keyboard(conn, false, win, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        reply = xcb_grab_keyboard_reply(conn, cookie, NULL);
        usleep(1000);
    }

    if (reply->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "Could not grab keyboard, status = %d\n", reply->status);
        exit(EXIT_ERROR);
    }

    xcb_flush(conn);

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            fprintf(stderr, "X11 Error received! sequence %x\n", event->sequence);
            continue;
        }

        /* Strip off the highest bit (set if the event is generated) */
        int type = (event->response_type & 0x7F);

        switch (type) {
            case XCB_KEY_PRESS:
                handle_key_press(NULL, conn, (xcb_key_press_event_t *)event, frameopts);
                break;

            case XCB_KEY_RELEASE:
                handle_key_release(NULL, conn, (xcb_key_release_event_t *)event);
                break;

            case XCB_EXPOSE:
                if (((xcb_expose_event_t *)event)->count == 0) {
                    handle_expose(NULL, conn, (xcb_expose_event_t *)event, frameopts);
                }

                break;
        }

        free(event);
    }

    FREE(frameopts);
    draw_util_surface_free(conn, &surface);
    return EXIT_OK;
}
