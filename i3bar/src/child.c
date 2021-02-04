/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * child.c: Getting input for the statusline
 *
 */
#include "common.h"
#include "yajl_utils.h"

#include <ctype.h> /* isspace */
#include <err.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <yajl/yajl_gen.h>
#include <yajl/yajl_parse.h>

/* Global variables for child_*() */
i3bar_child child = {0};
i3bar_child ws_child = {0};

#define DLOG_CHILD(c)                                                                                                              \
    do {                                                                                                                           \
        if ((c).pid == 0) {                                                                                                        \
            DLOG("%s: child pid = 0\n", __func__);                                                                                 \
        } else if ((c).pid == child.pid) {                                                                                         \
            DLOG("%s: status_command: pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n",    \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        } else if ((c).pid == ws_child.pid) {                                                                                      \
            DLOG("%s: workspace_command: pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n", \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        } else {                                                                                                                   \
            ELOG("%s: unknown child?! pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n",    \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        }                                                                                                                          \
    } while (0)
#define DLOG_CHILDREN         \
    do {                      \
        DLOG_CHILD(child);    \
        DLOG_CHILD(ws_child); \
    } while (0)

/* JSON parser for stdin */
yajl_handle parser;

/* JSON generator for stdout */
yajl_gen gen;

typedef struct parser_ctx {
    /* True if one of the parsed blocks was urgent */
    bool has_urgent;

    /* A copy of the last JSON map key. */
    char *last_map_key;

    /* The current block. Will be filled, then copied and put into the list of
     * blocks. */
    struct status_block block;
} parser_ctx;

parser_ctx parser_context;

struct statusline_head statusline_head = TAILQ_HEAD_INITIALIZER(statusline_head);
/* Used temporarily while reading a statusline */
struct statusline_head statusline_buffer = TAILQ_HEAD_INITIALIZER(statusline_buffer);

int child_stdin;

/*
 * Remove all blocks from the given statusline.
 * If free_resources is set, the fields of each status block will be free'd.
 */
void clear_statusline(struct statusline_head *head, bool free_resources) {
    struct status_block *first;
    while (!TAILQ_EMPTY(head)) {
        first = TAILQ_FIRST(head);
        if (free_resources) {
            I3STRING_FREE(first->full_text);
            I3STRING_FREE(first->short_text);
            FREE(first->color);
            FREE(first->name);
            FREE(first->instance);
            FREE(first->min_width_str);
            FREE(first->background);
            FREE(first->border);
        }

        TAILQ_REMOVE(head, first, blocks);
        free(first);
    }
}

static void copy_statusline(struct statusline_head *from, struct statusline_head *to) {
    struct status_block *current;
    TAILQ_FOREACH (current, from, blocks) {
        struct status_block *new_block = smalloc(sizeof(struct status_block));
        memcpy(new_block, current, sizeof(struct status_block));
        TAILQ_INSERT_TAIL(to, new_block, blocks);
    }
}

/*
 * Replaces the statusline in memory with an error message. Pass a format
 * string and format parameters as you would in `printf'. The next time
 * `draw_bars' is called, the error message text will be drawn on the bar in
 * the space allocated for the statusline.
 */
__attribute__((format(printf, 1, 2))) static void set_statusline_error(const char *format, ...) {
    clear_statusline(&statusline_head, true);

    char *message;
    va_list args;
    va_start(args, format);
    if (vasprintf(&message, format, args) == -1) {
        goto finish;
    }

    struct status_block *err_block = scalloc(1, sizeof(struct status_block));
    err_block->full_text = i3string_from_utf8("Error: ");
    err_block->name = sstrdup("error");
    err_block->color = sstrdup("#ff0000");
    err_block->no_separator = true;

    struct status_block *message_block = scalloc(1, sizeof(struct status_block));
    message_block->full_text = i3string_from_utf8(message);
    message_block->name = sstrdup("error_message");
    message_block->color = sstrdup("#ff0000");
    message_block->no_separator = true;

    TAILQ_INSERT_HEAD(&statusline_head, err_block, blocks);
    TAILQ_INSERT_TAIL(&statusline_head, message_block, blocks);

finish:
    free(message);
    va_end(args);
}

/*
 * Stop and free() the stdin- and SIGCHLD-watchers
 *
 */
static void cleanup(i3bar_child *c) {
    DLOG_CHILD(*c);

    if (c->stdin_io != NULL) {
        ev_io_stop(main_loop, c->stdin_io);
        FREE(c->stdin_io);

        if (c->pid == child.pid) {
            close(child_stdin);
            child_stdin = 0;
        }
        close(c->stdin_fd);
    }

    if (c->child_sig != NULL) {
        ev_child_stop(main_loop, c->child_sig);
        FREE(c->child_sig);
    }

    memset(c, 0, sizeof(i3bar_child));
}

/*
 * The start of a new array is the start of a new status line, so we clear all
 * previous entries from the buffer.
 */
static int stdin_start_array(void *context) {
    // the blocks are still used by statusline_head, so we won't free the
    // resources here.
    clear_statusline(&statusline_buffer, false);
    return 1;
}

/*
 * The start of a map is the start of a single block of the status line.
 *
 */
static int stdin_start_map(void *context) {
    parser_ctx *ctx = context;
    memset(&(ctx->block), '\0', sizeof(struct status_block));

    /* Default width of the separator block. */
    if (config.separator_symbol == NULL)
        ctx->block.sep_block_width = logical_px(9);
    else
        ctx->block.sep_block_width = logical_px(8) + separator_symbol_width;

    /* By default we draw all four borders if a border is set. */
    ctx->block.border_top = 1;
    ctx->block.border_right = 1;
    ctx->block.border_bottom = 1;
    ctx->block.border_left = 1;

    return 1;
}

static int stdin_map_key(void *context, const unsigned char *key, size_t len) {
    parser_ctx *ctx = context;
    FREE(ctx->last_map_key);
    sasprintf(&(ctx->last_map_key), "%.*s", len, key);
    return 1;
}

static int stdin_boolean(void *context, int val) {
    parser_ctx *ctx = context;

    if (!ctx->last_map_key) {
        return 0;
    }

    if (strcasecmp(ctx->last_map_key, "urgent") == 0) {
        ctx->block.urgent = val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "separator") == 0) {
        ctx->block.no_separator = !val;
        return 1;
    }

    return 1;
}

static int stdin_string(void *context, const unsigned char *val, size_t len) {
    parser_ctx *ctx = context;

    if (!ctx->last_map_key) {
        return 0;
    }

    if (strcasecmp(ctx->last_map_key, "full_text") == 0) {
        ctx->block.full_text = i3string_from_markup_with_length((const char *)val, len);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "short_text") == 0) {
        ctx->block.short_text = i3string_from_markup_with_length((const char *)val, len);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "color") == 0) {
        sasprintf(&(ctx->block.color), "%.*s", len, val);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "background") == 0) {
        sasprintf(&(ctx->block.background), "%.*s", len, val);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "border") == 0) {
        sasprintf(&(ctx->block.border), "%.*s", len, val);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "markup") == 0) {
        ctx->block.pango_markup = (len == strlen("pango") && !strncasecmp((const char *)val, "pango", strlen("pango")));
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "align") == 0) {
        if (len == strlen("center") && !strncmp((const char *)val, "center", strlen("center"))) {
            ctx->block.align = ALIGN_CENTER;
        } else if (len == strlen("right") && !strncmp((const char *)val, "right", strlen("right"))) {
            ctx->block.align = ALIGN_RIGHT;
        } else {
            ctx->block.align = ALIGN_LEFT;
        }
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "min_width") == 0) {
        sasprintf(&(ctx->block.min_width_str), "%.*s", len, val);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "name") == 0) {
        sasprintf(&(ctx->block.name), "%.*s", len, val);
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "instance") == 0) {
        sasprintf(&(ctx->block.instance), "%.*s", len, val);
        return 1;
    }

    return 1;
}

static int stdin_integer(void *context, long long val) {
    parser_ctx *ctx = context;

    if (!ctx->last_map_key) {
        return 0;
    }

    if (strcasecmp(ctx->last_map_key, "min_width") == 0) {
        ctx->block.min_width = (uint32_t)val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "separator_block_width") == 0) {
        ctx->block.sep_block_width = (uint32_t)val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "border_top") == 0) {
        ctx->block.border_top = (uint32_t)val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "border_right") == 0) {
        ctx->block.border_right = (uint32_t)val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "border_bottom") == 0) {
        ctx->block.border_bottom = (uint32_t)val;
        return 1;
    }
    if (strcasecmp(ctx->last_map_key, "border_left") == 0) {
        ctx->block.border_left = (uint32_t)val;
        return 1;
    }

    return 1;
}

/*
 * When a map is finished, we have an entire status block.
 * Move it from the parser's context to the statusline buffer.
 */
static int stdin_end_map(void *context) {
    parser_ctx *ctx = context;
    struct status_block *new_block = smalloc(sizeof(struct status_block));
    memcpy(new_block, &(ctx->block), sizeof(struct status_block));
    /* Ensure we have a full_text set, so that when it is missing (or null),
     * i3bar doesn’t crash and the user gets an annoying message. */
    if (!new_block->full_text)
        new_block->full_text = i3string_from_utf8("SPEC VIOLATION: full_text is NULL!");
    if (new_block->urgent)
        ctx->has_urgent = true;

    if (new_block->min_width_str) {
        i3String *text = i3string_from_utf8(new_block->min_width_str);
        i3string_set_markup(text, new_block->pango_markup);
        new_block->min_width = (uint32_t)predict_text_width(text);
        i3string_free(text);
    }

    i3string_set_markup(new_block->full_text, new_block->pango_markup);

    if (new_block->short_text != NULL)
        i3string_set_markup(new_block->short_text, new_block->pango_markup);

    TAILQ_INSERT_TAIL(&statusline_buffer, new_block, blocks);
    return 1;
}

/*
 * When an array is finished, we have an entire statusline.
 * Copy it from the buffer to the actual statusline.
 */
static int stdin_end_array(void *context) {
    DLOG("copying statusline_buffer to statusline_head\n");
    clear_statusline(&statusline_head, true);
    copy_statusline(&statusline_buffer, &statusline_head);

    DLOG("dumping statusline:\n");
    struct status_block *current;
    TAILQ_FOREACH (current, &statusline_head, blocks) {
        DLOG("full_text = %s\n", i3string_as_utf8(current->full_text));
        DLOG("short_text = %s\n", (current->short_text == NULL ? NULL : i3string_as_utf8(current->short_text)));
        DLOG("color = %s\n", current->color);
    }
    DLOG("end of dump\n");
    return 1;
}

/*
 * Helper function to read stdin
 *
 * Returns NULL on EOF.
 *
 */
static unsigned char *get_buffer(ev_io *watcher, int *ret_buffer_len) {
    int fd = watcher->fd;
    int n = 0;
    int rec = 0;
    int buffer_len = STDIN_CHUNK_SIZE;
    unsigned char *buffer = smalloc(buffer_len + 1);
    buffer[0] = '\0';
    while (1) {
        n = read(fd, buffer + rec, buffer_len - rec);
        if (n == -1) {
            if (errno == EAGAIN) {
                /* finish up */
                break;
            }
            ELOG("read() failed!: %s\n", strerror(errno));
            FREE(buffer);
            exit(EXIT_FAILURE);
        }
        if (n == 0) {
            ELOG("stdin: received EOF\n");
            FREE(buffer);
            *ret_buffer_len = -1;
            return NULL;
        }
        rec += n;

        if (rec == buffer_len) {
            buffer_len += STDIN_CHUNK_SIZE;
            buffer = srealloc(buffer, buffer_len);
        }
    }
    if (*buffer == '\0') {
        FREE(buffer);
        rec = -1;
    }
    *ret_buffer_len = rec;
    return buffer;
}

static void read_flat_input(char *buffer, int length) {
    struct status_block *first = TAILQ_FIRST(&statusline_head);
    /* Clear the old buffer if any. */
    I3STRING_FREE(first->full_text);
    /* Remove the trailing newline and terminate the string at the same
     * time. */
    if (buffer[length - 1] == '\n' || buffer[length - 1] == '\r') {
        buffer[length - 1] = '\0';
    } else {
        buffer[length] = '\0';
    }

    first->full_text = i3string_from_utf8(buffer);
}

static bool read_json_input(unsigned char *input, int length) {
    yajl_status status = yajl_parse(parser, input, length);
    bool has_urgent = false;
    if (status != yajl_status_ok) {
        char *message = (char *)yajl_get_error(parser, 0, input, length);

        /* strip the newline yajl adds to the error message */
        if (message[strlen(message) - 1] == '\n')
            message[strlen(message) - 1] = '\0';

        fprintf(stderr, "[i3bar] Could not parse JSON input (code = %d, message = %s): %.*s\n",
                status, message, length, input);

        set_statusline_error("Could not parse JSON (%s)", message);
        yajl_free_error(parser, (unsigned char *)message);
        draw_bars(false);
    } else if (parser_context.has_urgent) {
        has_urgent = true;
    }
    return has_urgent;
}

/*
 * Callbalk for stdin. We read a line from stdin and store the result
 * in statusline
 *
 */
static void stdin_io_cb(struct ev_loop *loop, ev_io *watcher, int revents) {
    int rec;
    unsigned char *buffer = get_buffer(watcher, &rec);
    if (buffer == NULL) {
        return;
    }
    bool has_urgent = false;
    if (child.version > 0) {
        has_urgent = read_json_input(buffer, rec);
    } else {
        read_flat_input((char *)buffer, rec);
    }
    free(buffer);
    draw_bars(has_urgent);
}

/*
 * Callbalk for stdin first line. We read the first line to detect
 * whether this is JSON or plain text
 *
 */
static void stdin_io_first_line_cb(struct ev_loop *loop, ev_io *watcher, int revents) {
    int rec;
    unsigned char *buffer = get_buffer(watcher, &rec);
    if (buffer == NULL) {
        return;
    }
    DLOG("Detecting input type based on buffer *%.*s*\n", rec, buffer);
    /* Detect whether this is JSON or plain text. */
    unsigned int consumed = 0;
    /* At the moment, we don’t care for the version. This might change
     * in the future, but for now, we just discard it. */
    parse_json_header(&child, buffer, rec, &consumed);
    if (child.version > 0) {
        /* If hide-on-modifier is set, we start of by sending the
         * child a SIGSTOP, because the bars aren't mapped at start */
        if (config.hide_on_modifier) {
            stop_child();
        }
        draw_bars(read_json_input(buffer + consumed, rec - consumed));
    } else {
        /* In case of plaintext, we just add a single block and change its
         * full_text pointer later. */
        struct status_block *new_block = scalloc(1, sizeof(struct status_block));
        TAILQ_INSERT_TAIL(&statusline_head, new_block, blocks);
        read_flat_input((char *)buffer, rec);
    }
    free(buffer);
    ev_io_stop(main_loop, child.stdin_io);
    ev_io_init(child.stdin_io, &stdin_io_cb, child.stdin_fd, EV_READ);
    ev_io_start(main_loop, child.stdin_io);
}

static bool isempty(unsigned char *s, size_t len) {
    if (s == NULL || len == 0) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        if (!isspace(s[i])) {
            return false;
        }
    }
    return true;
}

static unsigned char *ws_last_json;
static size_t ws_last_json_len;

static void ws_stdin_io_cb(struct ev_loop *loop, ev_io *watcher, int revents) {
    FILE *fp = fdopen(watcher->fd, "r");
    while (true) {
        unsigned char *buffer = NULL;
        size_t len = 0;
        ssize_t n = getline((char **)&buffer, &len, fp);
        if (n < 0) {
            /* Ignore errors, if the child dies we are going to handle it
             * elsewhere. */
            break;
        }
        if (isempty(buffer, n)) {
            free(buffer);
            continue;
        }
        parse_workspaces_json(buffer, n);

        free(ws_last_json);
        ws_last_json = buffer;
        ws_last_json_len = n;
    }

    draw_bars(false);
}

/*
 * When workspace_command is enabled this function is used to re-parse the
 * latest received JSON from the client.
 */
void repeat_last_ws_json(void) {
    if (ws_last_json) {
        DLOG("Repeating last workspace JSON\n");
        parse_workspaces_json(ws_last_json, ws_last_json_len);
    } else {
        DLOG("Nothing to repeat\n");
    }
}

/*
 * Wrapper around set_workspace_button_error to mimic the call of
 * set_statusline_error.
 */
__attribute__((format(printf, 1, 2))) static void set_workspace_button_error_f(const char *format, ...) {
    char *message;
    va_list args;
    va_start(args, format);
    if (vasprintf(&message, format, args) == -1) {
        goto finish;
    }

    set_workspace_button_error(message);

finish:
    free(message);
    va_end(args);
}

/*
 * Replaces the workspace buttons with an error message.
 */
void set_workspace_button_error(const char *message) {
    free_workspaces();

    char *name = NULL;
    sasprintf(&name, "Error: %s", message);

    i3_output *output;
    SLIST_FOREACH (output, outputs, slist) {
        i3_ws *fake_ws = scalloc(1, sizeof(i3_ws));
        /* Don't set the canonical_name field to make this workspace unfocusable. */
        fake_ws->name = i3string_from_utf8(name);
        fake_ws->name_width = predict_text_width(fake_ws->name);
        fake_ws->num = -1;
        fake_ws->urgent = fake_ws->visible = true;
        fake_ws->output = output;

        TAILQ_INSERT_TAIL(output->workspaces, fake_ws, tailq);
    }

    free(name);
}

/*
 * We received a SIGCHLD, meaning, that the child process terminated.
 * We simply free the respective data structures and don't care for input
 * anymore
 *
 */
static void child_sig_cb(struct ev_loop *loop, ev_child *watcher, int revents) {
    const int exit_status = WEXITSTATUS(watcher->rstatus);

    ELOG("Child (pid: %d) unexpectedly exited with status %d\n",
         watcher->pid,
         exit_status);

    void (*error_function_pointer)(const char *, ...) = NULL;
    const char *command_type = "";
    i3bar_child *c = NULL;
    if (watcher->pid == child.pid) {
        command_type = "status_command";
        error_function_pointer = set_statusline_error;
        c = &child;
    } else if (watcher->pid == ws_child.pid) {
        command_type = "workspace_command";
        error_function_pointer = set_workspace_button_error_f;
        c = &ws_child;
    } else {
        ELOG("Unknown pid?\n");
        return;
    }
    DLOG_CHILD(*c);

    /* this error is most likely caused by a user giving a nonexecutable or
     * nonexistent file, so we will handle those cases separately. */
    if (exit_status == 126) {
        error_function_pointer("%s is not executable (exit %d)", command_type, exit_status);
    } else if (exit_status == 127) {
        error_function_pointer("%s not found or is missing a library dependency (exit %d)", command_type, exit_status);
    } else {
        error_function_pointer("%s process exited unexpectedly (exit %d)", command_type, exit_status);
    }

    cleanup(c);
    draw_bars(false);
}

static void child_write_output(void) {
    if (child.click_events) {
        const unsigned char *output;
        size_t size;
        ssize_t n;

        yajl_gen_get_buf(gen, &output, &size);

        n = writeall(child_stdin, output, size);
        if (n != -1)
            n = writeall(child_stdin, "\n", 1);

        yajl_gen_clear(gen);

        if (n == -1) {
            child.click_events = false;
            kill_child();
            set_statusline_error("child_write_output failed");
            draw_bars(false);
        }
    }
}

/*
 * Start a child process with the specified command and reroute stdin.
 * We actually start a shell to execute the command so we don't have to care
 * about arguments and such.
 *
 * If `command' is NULL, such as in the case when no `status_command' is given
 * in the bar config, no child will be started.
 *
 */
void start_child(char *command) {
    if (command == NULL) {
        return;
    }

    /* Allocate a yajl parser which will be used to parse stdin. */
    static yajl_callbacks callbacks = {
        .yajl_boolean = stdin_boolean,
        .yajl_integer = stdin_integer,
        .yajl_string = stdin_string,
        .yajl_start_map = stdin_start_map,
        .yajl_map_key = stdin_map_key,
        .yajl_end_map = stdin_end_map,
        .yajl_start_array = stdin_start_array,
        .yajl_end_array = stdin_end_array,
    };
    parser = yajl_alloc(&callbacks, NULL, &parser_context);
    gen = yajl_gen_alloc(NULL);

    int pipe_in[2];  /* pipe we read from */
    int pipe_out[2]; /* pipe we write to */

    if (pipe(pipe_in) == -1) {
        err(EXIT_FAILURE, "pipe(pipe_in)");
    }
    if (pipe(pipe_out) == -1) {
        err(EXIT_FAILURE, "pipe(pipe_out)");
    }

    child.pid = fork();
    switch (child.pid) {
        case -1:
            ELOG("Couldn't fork(): %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        case 0:
            /* Child-process. Reroute streams and start shell */
            close(pipe_in[0]);
            close(pipe_out[1]);

            dup2(pipe_in[1], STDOUT_FILENO);
            dup2(pipe_out[0], STDIN_FILENO);

            setpgid(child.pid, 0);
            execl(_PATH_BSHELL, _PATH_BSHELL, "-c", command, (char *)NULL);
            return;
        default:
            /* Parent-process. Reroute streams */
            close(pipe_in[1]);
            close(pipe_out[0]);

            child.stdin_fd = pipe_in[0];
            child_stdin = pipe_out[1];

            break;
    }

    /* We set O_NONBLOCK because blocking is evil in event-driven software */
    fcntl(child.stdin_fd, F_SETFL, O_NONBLOCK);

    child.stdin_io = smalloc(sizeof(ev_io));
    ev_io_init(child.stdin_io, &stdin_io_first_line_cb, child.stdin_fd, EV_READ);
    ev_io_start(main_loop, child.stdin_io);

    /* We must cleanup, if the child unexpectedly terminates */
    child.child_sig = smalloc(sizeof(ev_child));
    ev_child_init(child.child_sig, &child_sig_cb, child.pid, 0);
    ev_child_start(main_loop, child.child_sig);

    DLOG_CHILD(child);
}

/*
 * Same as start_child but starts the configured client that manages workspace
 * buttons.
 *
 */
void start_ws_child(char *command) {
    if (command == NULL) {
        return;
    }

    ws_child.stop_signal = SIGSTOP;
    ws_child.cont_signal = SIGCONT;

    int pipe_in[2]; /* pipe we read from */
    if (pipe(pipe_in) == -1) {
        err(EXIT_FAILURE, "pipe(pipe_in)");
    }

    ws_child.pid = fork();
    switch (ws_child.pid) {
        case -1:
            ELOG("Couldn't fork(): %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        case 0:
            /* Child-process. Reroute streams and start shell */
            close(pipe_in[0]);
            dup2(pipe_in[1], STDOUT_FILENO);

            setpgid(ws_child.pid, 0);
            execl(_PATH_BSHELL, _PATH_BSHELL, "-c", command, (char *)NULL);
            return;
        default:
            /* Parent-process. Reroute streams */
            close(pipe_in[1]);
            ws_child.stdin_fd = pipe_in[0];
            break;
    }

    /* We set O_NONBLOCK because blocking is evil in event-driven software */
    fcntl(ws_child.stdin_fd, F_SETFL, O_NONBLOCK);

    ws_child.stdin_io = smalloc(sizeof(ev_io));
    ev_io_init(ws_child.stdin_io, &ws_stdin_io_cb, ws_child.stdin_fd, EV_READ);
    ev_io_start(main_loop, ws_child.stdin_io);

    /* We must cleanup, if the child unexpectedly terminates */
    ws_child.child_sig = smalloc(sizeof(ev_child));
    ev_child_init(ws_child.child_sig, &child_sig_cb, ws_child.pid, 0);
    ev_child_start(main_loop, ws_child.child_sig);

    DLOG_CHILD(ws_child);
}

static void child_click_events_initialize(void) {
    DLOG_CHILD(child);

    if (!child.click_events_init) {
        yajl_gen_array_open(gen);
        child_write_output();
        child.click_events_init = true;
    }
}

/*
 * Generates a click event, if enabled.
 *
 */
void send_block_clicked(int button, const char *name, const char *instance, int x, int y, int x_rel, int y_rel, int out_x, int out_y, int width, int height, int mods) {
    if (!child.click_events) {
        return;
    }

    child_click_events_initialize();

    yajl_gen_map_open(gen);

    if (name) {
        ystr("name");
        ystr(name);
    }

    if (instance) {
        ystr("instance");
        ystr(instance);
    }

    ystr("button");
    yajl_gen_integer(gen, button);

    ystr("modifiers");
    yajl_gen_array_open(gen);
    if (mods & XCB_MOD_MASK_SHIFT)
        ystr("Shift");
    if (mods & XCB_MOD_MASK_CONTROL)
        ystr("Control");
    if (mods & XCB_MOD_MASK_1)
        ystr("Mod1");
    if (mods & XCB_MOD_MASK_2)
        ystr("Mod2");
    if (mods & XCB_MOD_MASK_3)
        ystr("Mod3");
    if (mods & XCB_MOD_MASK_4)
        ystr("Mod4");
    if (mods & XCB_MOD_MASK_5)
        ystr("Mod5");
    yajl_gen_array_close(gen);

    ystr("x");
    yajl_gen_integer(gen, x);

    ystr("y");
    yajl_gen_integer(gen, y);

    ystr("relative_x");
    yajl_gen_integer(gen, x_rel);

    ystr("relative_y");
    yajl_gen_integer(gen, y_rel);

    ystr("output_x");
    yajl_gen_integer(gen, out_x);

    ystr("output_y");
    yajl_gen_integer(gen, out_y);

    ystr("width");
    yajl_gen_integer(gen, width);

    ystr("height");
    yajl_gen_integer(gen, height);

    yajl_gen_map_close(gen);
    child_write_output();
}

/*
 * kill()s the child process (if any). Called when exit()ing.
 *
 */
void kill_child_at_exit(void) {
    DLOG_CHILDREN;

    if (child.pid > 0) {
        if (child.cont_signal > 0 && child.stopped)
            killpg(child.pid, child.cont_signal);
        killpg(child.pid, SIGTERM);
    }
    if (ws_child.pid > 0) {
        if (ws_child.cont_signal > 0 && ws_child.stopped)
            killpg(ws_child.pid, ws_child.cont_signal);
        killpg(ws_child.pid, SIGTERM);
    }
}

static void _kill(i3bar_child *c) {
    DLOG_CHILD(*c);

    if (c->pid > 0) {
        if (c->cont_signal > 0 && c->stopped)
            killpg(c->pid, c->cont_signal);
        killpg(c->pid, SIGTERM);
        int status;
        waitpid(c->pid, &status, 0);
        cleanup(c);
    }
}

/*
 * kill()s the child process (if any) and closes and free()s the stdin- and
 * SIGCHLD-watchers
 *
 */
void kill_child(void) {
    _kill(&child);
}

/*
 * kill()s the workspace child process (if any) and closes and free()s the
 * stdin- and SIGCHLD-watchers.
 * Similar to kill_child.
 *
 */
void kill_ws_child(void) {
    _kill(&ws_child);
}

/*
 * Sends a SIGSTOP to the child process (if existent)
 *
 */
void stop_child(void) {
    DLOG_CHILDREN;

    if (child.stop_signal > 0 && !child.stopped) {
        child.stopped = true;
        killpg(child.pid, child.stop_signal);
    }

    if (ws_child.stop_signal > 0 && !ws_child.stopped) {
        ws_child.stopped = true;
        killpg(ws_child.pid, ws_child.stop_signal);
    }
}

/*
 * Sends a SIGCONT to the child process (if existent)
 *
 */
void cont_child(void) {
    DLOG_CHILDREN;

    if (child.cont_signal > 0 && child.stopped) {
        child.stopped = false;
        killpg(child.pid, child.cont_signal);
    }

    if (ws_child.cont_signal > 0 && ws_child.stopped) {
        ws_child.stopped = false;
        killpg(ws_child.pid, ws_child.cont_signal);
    }
}

/*
 * Whether or not the child want click events
 *
 */
bool child_want_click_events(void) {
    return child.click_events;
}
