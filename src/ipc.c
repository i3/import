/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * ipc.c: UNIX domain socket IPC (initialization, client handling, protocol).
 *
 */

#include "all.h"
#include "yajl_utils.h"

#include <ev.h>
#include <fcntl.h>
#include <libgen.h>
#include <locale.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <yajl/yajl_gen.h>
#include <yajl/yajl_parse.h>

char *current_socketpath = NULL;

TAILQ_HEAD(ipc_client_head, ipc_client) all_clients = TAILQ_HEAD_INITIALIZER(all_clients);

static void ipc_client_timeout(EV_P_ ev_timer *w, int revents);
static void ipc_socket_writeable_cb(EV_P_ struct ev_io *w, int revents);

static ev_tstamp kill_timeout = 10.0;

void ipc_set_kill_timeout(ev_tstamp new) {
    kill_timeout = new;
}

/*
 * Try to write the contents of the pending buffer to the client's subscription
 * socket. Will set, reset or clear the timeout and io write callbacks depending
 * on the result of the write operation.
 *
 */
static void ipc_push_pending(ipc_client *client) {
    const ssize_t result = writeall_nonblock(client->fd, client->buffer, client->buffer_size);
    if (result < 0) {
        return;
    }

    if ((size_t)result == client->buffer_size) {
        /* Everything was written successfully: clear the timer and stop the io
         * callback. */
        FREE(client->buffer);
        client->buffer_size = 0;
        if (client->timeout) {
            ev_timer_stop(main_loop, client->timeout);
            FREE(client->timeout);
        }
        ev_io_stop(main_loop, client->write_callback);
        return;
    }

    /* Otherwise, make sure that the io callback is enabled and create a new
     * timer if needed. */
    ev_io_start(main_loop, client->write_callback);

    if (!client->timeout) {
        struct ev_timer *timeout = scalloc(1, sizeof(struct ev_timer));
        ev_timer_init(timeout, ipc_client_timeout, kill_timeout, 0.);
        timeout->data = client;
        client->timeout = timeout;
        ev_set_priority(timeout, EV_MINPRI);
        ev_timer_start(main_loop, client->timeout);
    } else if (result > 0) {
        /* Keep the old timeout when nothing is written. Otherwise, we would
         * keep a dead connection by continuously renewing its timeouts. */
        ev_timer_stop(main_loop, client->timeout);
        ev_timer_set(client->timeout, kill_timeout, 0.0);
        ev_timer_start(main_loop, client->timeout);
    }
    if (result == 0) {
        return;
    }

    /* Shift the buffer to the left and reduce the allocated space. */
    client->buffer_size -= (size_t)result;
    memmove(client->buffer, client->buffer + result, client->buffer_size);
    client->buffer = srealloc(client->buffer, client->buffer_size);
}

/*
 * Given a message and a message type, create the corresponding header, merge it
 * with the message and append it to the given client's output buffer. Also,
 * send the message if the client's buffer was empty.
 *
 */
static void ipc_send_client_message(ipc_client *client, size_t size, const uint32_t message_type, const uint8_t *payload) {
    const i3_ipc_header_t header = {
        .magic = {'i', '3', '-', 'i', 'p', 'c'},
        .size = size,
        .type = message_type};
    const size_t header_size = sizeof(i3_ipc_header_t);
    const size_t message_size = header_size + size;

    const bool push_now = (client->buffer_size == 0);
    client->buffer = srealloc(client->buffer, client->buffer_size + message_size);
    memcpy(client->buffer + client->buffer_size, ((void *)&header), header_size);
    memcpy(client->buffer + client->buffer_size + header_size, payload, size);
    client->buffer_size += message_size;

    if (push_now) {
        ipc_push_pending(client);
    }
}

static void free_ipc_client(ipc_client *client, int exempt_fd) {
    if (client->fd != exempt_fd) {
        DLOG("Disconnecting client on fd %d\n", client->fd);
        close(client->fd);
    }

    ev_io_stop(main_loop, client->read_callback);
    FREE(client->read_callback);
    ev_io_stop(main_loop, client->write_callback);
    FREE(client->write_callback);
    if (client->timeout) {
        ev_timer_stop(main_loop, client->timeout);
        FREE(client->timeout);
    }

    free(client->buffer);

    for (int i = 0; i < client->num_events; i++) {
        free(client->events[i]);
    }
    free(client->events);
    TAILQ_REMOVE(&all_clients, client, clients);
    free(client);
}

/*
 * Sends the specified event to all IPC clients which are currently connected
 * and subscribed to this kind of event.
 *
 */
void ipc_send_event(const char *event, uint32_t message_type, const char *payload) {
    ipc_client *current;
    TAILQ_FOREACH (current, &all_clients, clients) {
        for (int i = 0; i < current->num_events; i++) {
            if (strcasecmp(current->events[i], event) == 0) {
                ipc_send_client_message(current, strlen(payload), message_type, (uint8_t *)payload);
                break;
            }
        }
    }
}

/*
 * For shutdown events, we send the reason for the shutdown.
 */
static void ipc_send_shutdown_event(shutdown_reason_t reason) {
    yajl_gen gen = ygenalloc();
    y(map_open);

    ystr("change");

    if (reason == SHUTDOWN_REASON_RESTART) {
        ystr("restart");
    } else if (reason == SHUTDOWN_REASON_EXIT) {
        ystr("exit");
    }

    y(map_close);

    const unsigned char *payload;
    ylength length;

    y(get_buf, &payload, &length);
    ipc_send_event("shutdown", I3_IPC_EVENT_SHUTDOWN, (const char *)payload);

    y(free);
}

/*
 * Calls shutdown() on each socket and closes it. This function is to be called
 * when exiting or restarting only!
 *
 * exempt_fd is never closed. Set to -1 to close all fds.
 *
 */
void ipc_shutdown(shutdown_reason_t reason, int exempt_fd) {
    ipc_send_shutdown_event(reason);

    ipc_client *current;
    while (!TAILQ_EMPTY(&all_clients)) {
        current = TAILQ_FIRST(&all_clients);
        if (current->fd != exempt_fd) {
            shutdown(current->fd, SHUT_RDWR);
        }
        free_ipc_client(current, exempt_fd);
    }
}

/*
 * Executes the given command.
 *
 */
IPC_HANDLER(run_command) {
    /* To get a properly terminated buffer, we copy
     * message_size bytes out of the buffer */
    char *command = sstrndup((const char *)message, message_size);
    LOG("IPC: received: *%.4000s*\n", command);
    yajl_gen gen = yajl_gen_alloc(NULL);

    CommandResult *result = parse_command(command, gen, client);
    free(command);

    if (result->needs_tree_render)
        tree_render();

    command_result_free(result);

    const unsigned char *reply;
    ylength length;
    yajl_gen_get_buf(gen, &reply, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_COMMAND,
                            (const uint8_t *)reply);

    yajl_gen_free(gen);
}

static void dump_rect(yajl_gen gen, const char *name, Rect r) {
    ystr(name);
    y(map_open);
    ystr("x");
    y(integer, (int32_t)r.x);
    ystr("y");
    y(integer, (int32_t)r.y);
    ystr("width");
    y(integer, r.width);
    ystr("height");
    y(integer, r.height);
    y(map_close);
}

static void dump_event_state_mask(yajl_gen gen, Binding *bind) {
    y(array_open);
    for (int i = 0; i < 20; i++) {
        if (bind->event_state_mask & (1 << i)) {
            switch (1 << i) {
                case XCB_KEY_BUT_MASK_SHIFT:
                    ystr("shift");
                    break;
                case XCB_KEY_BUT_MASK_LOCK:
                    ystr("lock");
                    break;
                case XCB_KEY_BUT_MASK_CONTROL:
                    ystr("ctrl");
                    break;
                case XCB_KEY_BUT_MASK_MOD_1:
                    ystr("Mod1");
                    break;
                case XCB_KEY_BUT_MASK_MOD_2:
                    ystr("Mod2");
                    break;
                case XCB_KEY_BUT_MASK_MOD_3:
                    ystr("Mod3");
                    break;
                case XCB_KEY_BUT_MASK_MOD_4:
                    ystr("Mod4");
                    break;
                case XCB_KEY_BUT_MASK_MOD_5:
                    ystr("Mod5");
                    break;
                case XCB_KEY_BUT_MASK_BUTTON_1:
                    ystr("Button1");
                    break;
                case XCB_KEY_BUT_MASK_BUTTON_2:
                    ystr("Button2");
                    break;
                case XCB_KEY_BUT_MASK_BUTTON_3:
                    ystr("Button3");
                    break;
                case XCB_KEY_BUT_MASK_BUTTON_4:
                    ystr("Button4");
                    break;
                case XCB_KEY_BUT_MASK_BUTTON_5:
                    ystr("Button5");
                    break;
                case (I3_XKB_GROUP_MASK_1 << 16):
                    ystr("Group1");
                    break;
                case (I3_XKB_GROUP_MASK_2 << 16):
                    ystr("Group2");
                    break;
                case (I3_XKB_GROUP_MASK_3 << 16):
                    ystr("Group3");
                    break;
                case (I3_XKB_GROUP_MASK_4 << 16):
                    ystr("Group4");
                    break;
            }
        }
    }
    y(array_close);
}

static void dump_binding(yajl_gen gen, Binding *bind) {
    y(map_open);
    ystr("input_code");
    y(integer, bind->keycode);

    ystr("input_type");
    ystr((const char *)(bind->input_type == B_KEYBOARD ? "keyboard" : "mouse"));

    ystr("symbol");
    if (bind->symbol == NULL)
        y(null);
    else
        ystr(bind->symbol);

    ystr("command");
    ystr(bind->command);

    // This key is only provided for compatibility, new programs should use
    // event_state_mask instead.
    ystr("mods");
    dump_event_state_mask(gen, bind);

    ystr("event_state_mask");
    dump_event_state_mask(gen, bind);

    y(map_close);
}

void dump_node(yajl_gen gen, struct Con *con, bool inplace_restart) {
    y(map_open);
    ystr("id");
    y(integer, (uintptr_t)con);

    ystr("type");
    switch (con->type) {
        case CT_ROOT:
            ystr("root");
            break;
        case CT_OUTPUT:
            ystr("output");
            break;
        case CT_CON:
            ystr("con");
            break;
        case CT_FLOATING_CON:
            ystr("floating_con");
            break;
        case CT_WORKSPACE:
            ystr("workspace");
            break;
        case CT_DOCKAREA:
            ystr("dockarea");
            break;
    }

    /* provided for backwards compatibility only. */
    ystr("orientation");
    if (!con_is_split(con))
        ystr("none");
    else {
        if (con_orientation(con) == HORIZ)
            ystr("horizontal");
        else
            ystr("vertical");
    }

    ystr("scratchpad_state");
    switch (con->scratchpad_state) {
        case SCRATCHPAD_NONE:
            ystr("none");
            break;
        case SCRATCHPAD_FRESH:
            ystr("fresh");
            break;
        case SCRATCHPAD_CHANGED:
            ystr("changed");
            break;
    }

    ystr("percent");
    if (con->percent == 0.0)
        y(null);
    else
        y(double, con->percent);

    ystr("urgent");
    y(bool, con->urgent);

    ystr("marks");
    y(array_open);
    mark_t *mark;
    TAILQ_FOREACH (mark, &(con->marks_head), marks) {
        ystr(mark->name);
    }
    y(array_close);

    ystr("focused");
    y(bool, (con == focused));

    if (con->type != CT_ROOT && con->type != CT_OUTPUT) {
        ystr("output");
        ystr(con_get_output(con)->name);
    }

    ystr("layout");
    switch (con->layout) {
        case L_DEFAULT:
            DLOG("About to dump layout=default, this is a bug in the code.\n");
            assert(false);
            break;
        case L_SPLITV:
            ystr("splitv");
            break;
        case L_SPLITH:
            ystr("splith");
            break;
        case L_STACKED:
            ystr("stacked");
            break;
        case L_TABBED:
            ystr("tabbed");
            break;
        case L_DOCKAREA:
            ystr("dockarea");
            break;
        case L_OUTPUT:
            ystr("output");
            break;
    }

    ystr("workspace_layout");
    switch (con->workspace_layout) {
        case L_DEFAULT:
            ystr("default");
            break;
        case L_STACKED:
            ystr("stacked");
            break;
        case L_TABBED:
            ystr("tabbed");
            break;
        default:
            DLOG("About to dump workspace_layout=%d (none of default/stacked/tabbed), this is a bug.\n", con->workspace_layout);
            assert(false);
            break;
    }

    ystr("last_split_layout");
    switch (con->layout) {
        case L_SPLITV:
            ystr("splitv");
            break;
        default:
            ystr("splith");
            break;
    }

    ystr("border");
    switch (con->border_style) {
        case BS_NORMAL:
            ystr("normal");
            break;
        case BS_NONE:
            ystr("none");
            break;
        case BS_PIXEL:
            ystr("pixel");
            break;
    }

    ystr("current_border_width");
    y(integer, con->current_border_width);

    dump_rect(gen, "rect", con->rect);
    dump_rect(gen, "deco_rect", con->deco_rect);
    dump_rect(gen, "window_rect", con->window_rect);
    dump_rect(gen, "geometry", con->geometry);

    ystr("name");
    if (con->window && con->window->name)
        ystr(i3string_as_utf8(con->window->name));
    else if (con->name != NULL)
        ystr(con->name);
    else
        y(null);

    if (con->title_format != NULL) {
        ystr("title_format");
        ystr(con->title_format);
    }

    ystr("window_icon_padding");
    y(integer, con->window_icon_padding);

    ystr("window_icon_position");
    switch (con->window_icon_position) {
        case ICON_POSITION_LEFT:
            ystr("left");
            break;
        case ICON_POSITION_RIGHT:
            ystr("right");
            break;
        case ICON_POSITION_TITLE:
        default:
            ystr("title");
            break;
    }

    if (con->type == CT_WORKSPACE) {
        ystr("num");
        y(integer, con->num);
    }

    ystr("window");
    if (con->window)
        y(integer, con->window->id);
    else
        y(null);

    ystr("window_type");
    if (con->window) {
        if (con->window->window_type == A__NET_WM_WINDOW_TYPE_NORMAL) {
            ystr("normal");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_DOCK) {
            ystr("dock");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_DIALOG) {
            ystr("dialog");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_UTILITY) {
            ystr("utility");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_TOOLBAR) {
            ystr("toolbar");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_SPLASH) {
            ystr("splash");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_MENU) {
            ystr("menu");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_DROPDOWN_MENU) {
            ystr("dropdown_menu");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_POPUP_MENU) {
            ystr("popup_menu");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_TOOLTIP) {
            ystr("tooltip");
        } else if (con->window->window_type == A__NET_WM_WINDOW_TYPE_NOTIFICATION) {
            ystr("notification");
        } else {
            ystr("unknown");
        }
    } else
        y(null);

    if (con->window && !inplace_restart) {
        /* Window properties are useless to preserve when restarting because
         * they will be queried again anyway. However, for i3-save-tree(1),
         * they are very useful and save i3-save-tree dealing with X11. */
        ystr("window_properties");
        y(map_open);

#define DUMP_PROPERTY(key, prop_name)         \
    do {                                      \
        if (con->window->prop_name != NULL) { \
            ystr(key);                        \
            ystr(con->window->prop_name);     \
        }                                     \
    } while (0)

        DUMP_PROPERTY("class", class_class);
        DUMP_PROPERTY("instance", class_instance);
        DUMP_PROPERTY("window_role", role);
        DUMP_PROPERTY("machine", machine);

        if (con->window->name != NULL) {
            ystr("title");
            ystr(i3string_as_utf8(con->window->name));
        }

        ystr("transient_for");
        if (con->window->transient_for == XCB_NONE)
            y(null);
        else
            y(integer, con->window->transient_for);

        y(map_close);
    }

    ystr("nodes");
    y(array_open);
    Con *node;
    if (con->type != CT_DOCKAREA || !inplace_restart) {
        TAILQ_FOREACH (node, &(con->nodes_head), nodes) {
            dump_node(gen, node, inplace_restart);
        }
    }
    y(array_close);

    ystr("floating_nodes");
    y(array_open);
    TAILQ_FOREACH (node, &(con->floating_head), floating_windows) {
        dump_node(gen, node, inplace_restart);
    }
    y(array_close);

    ystr("focus");
    y(array_open);
    TAILQ_FOREACH (node, &(con->focus_head), focused) {
        y(integer, (uintptr_t)node);
    }
    y(array_close);

    ystr("fullscreen_mode");
    y(integer, con->fullscreen_mode);

    ystr("sticky");
    y(bool, con->sticky);

    ystr("floating");
    switch (con->floating) {
        case FLOATING_AUTO_OFF:
            ystr("auto_off");
            break;
        case FLOATING_AUTO_ON:
            ystr("auto_on");
            break;
        case FLOATING_USER_OFF:
            ystr("user_off");
            break;
        case FLOATING_USER_ON:
            ystr("user_on");
            break;
    }

    ystr("swallows");
    y(array_open);
    Match *match;
    TAILQ_FOREACH (match, &(con->swallow_head), matches) {
        /* We will generate a new restart_mode match specification after this
         * loop, so skip this one. */
        if (match->restart_mode)
            continue;
        y(map_open);
        if (match->dock != M_DONTCHECK) {
            ystr("dock");
            y(integer, match->dock);
            ystr("insert_where");
            y(integer, match->insert_where);
        }

#define DUMP_REGEX(re_name)                \
    do {                                   \
        if (match->re_name != NULL) {      \
            ystr(#re_name);                \
            ystr(match->re_name->pattern); \
        }                                  \
    } while (0)

        DUMP_REGEX(class);
        DUMP_REGEX(instance);
        DUMP_REGEX(window_role);
        DUMP_REGEX(title);
        DUMP_REGEX(machine);

#undef DUMP_REGEX
        y(map_close);
    }

    if (inplace_restart) {
        if (con->window != NULL) {
            y(map_open);
            ystr("id");
            y(integer, con->window->id);
            ystr("restart_mode");
            y(bool, true);
            y(map_close);
        }
    }
    y(array_close);

    if (inplace_restart && con->window != NULL) {
        ystr("depth");
        y(integer, con->depth);
    }

    if (inplace_restart && con->type == CT_ROOT && previous_workspace_name) {
        ystr("previous_workspace_name");
        ystr(previous_workspace_name);
    }

    y(map_close);
}

static void dump_bar_bindings(yajl_gen gen, Barconfig *config) {
    if (TAILQ_EMPTY(&(config->bar_bindings)))
        return;

    ystr("bindings");
    y(array_open);

    struct Barbinding *current;
    TAILQ_FOREACH (current, &(config->bar_bindings), bindings) {
        y(map_open);

        ystr("input_code");
        y(integer, current->input_code);
        ystr("command");
        ystr(current->command);
        ystr("release");
        y(bool, current->release == B_UPON_KEYRELEASE);

        y(map_close);
    }

    y(array_close);
}

static char *canonicalize_output_name(char *name) {
    /* Do not canonicalize special output names. */
    if (strcasecmp(name, "primary") == 0) {
        return name;
    }
    Output *output = get_output_by_name(name, false);
    return output ? output_primary_name(output) : name;
}

static void dump_bar_config(yajl_gen gen, Barconfig *config) {
    y(map_open);

    ystr("id");
    ystr(config->id);

    if (config->num_outputs > 0) {
        ystr("outputs");
        y(array_open);
        for (int c = 0; c < config->num_outputs; c++) {
            /* Convert monitor names (RandR ≥ 1.5) or output names
             * (RandR < 1.5) into monitor names. This way, existing
             * configs which use output names transparently keep
             * working. */
            ystr(canonicalize_output_name(config->outputs[c]));
        }
        y(array_close);
    }

    if (!TAILQ_EMPTY(&(config->tray_outputs))) {
        ystr("tray_outputs");
        y(array_open);

        struct tray_output_t *tray_output;
        TAILQ_FOREACH (tray_output, &(config->tray_outputs), tray_outputs) {
            ystr(canonicalize_output_name(tray_output->output));
        }

        y(array_close);
    }

#define YSTR_IF_SET(name)       \
    do {                        \
        if (config->name) {     \
            ystr(#name);        \
            ystr(config->name); \
        }                       \
    } while (0)

    ystr("tray_padding");
    y(integer, config->tray_padding);

    YSTR_IF_SET(socket_path);

    ystr("mode");
    switch (config->mode) {
        case M_HIDE:
            ystr("hide");
            break;
        case M_INVISIBLE:
            ystr("invisible");
            break;
        case M_DOCK:
        default:
            ystr("dock");
            break;
    }

    ystr("hidden_state");
    switch (config->hidden_state) {
        case S_SHOW:
            ystr("show");
            break;
        case S_HIDE:
        default:
            ystr("hide");
            break;
    }

    ystr("modifier");
    y(integer, config->modifier);

    dump_bar_bindings(gen, config);

    ystr("position");
    if (config->position == P_BOTTOM)
        ystr("bottom");
    else
        ystr("top");

    YSTR_IF_SET(status_command);
    YSTR_IF_SET(font);

    if (config->separator_symbol) {
        ystr("separator_symbol");
        ystr(config->separator_symbol);
    }

    ystr("workspace_buttons");
    y(bool, !config->hide_workspace_buttons);

    ystr("workspace_min_width");
    y(integer, config->workspace_min_width);

    ystr("strip_workspace_numbers");
    y(bool, config->strip_workspace_numbers);

    ystr("strip_workspace_name");
    y(bool, config->strip_workspace_name);

    ystr("binding_mode_indicator");
    y(bool, !config->hide_binding_mode_indicator);

    ystr("verbose");
    y(bool, config->verbose);

#undef YSTR_IF_SET
#define YSTR_IF_SET(name)              \
    do {                               \
        if (config->colors.name) {     \
            ystr(#name);               \
            ystr(config->colors.name); \
        }                              \
    } while (0)

    ystr("colors");
    y(map_open);
    YSTR_IF_SET(background);
    YSTR_IF_SET(statusline);
    YSTR_IF_SET(separator);
    YSTR_IF_SET(focused_background);
    YSTR_IF_SET(focused_statusline);
    YSTR_IF_SET(focused_separator);
    YSTR_IF_SET(focused_workspace_border);
    YSTR_IF_SET(focused_workspace_bg);
    YSTR_IF_SET(focused_workspace_text);
    YSTR_IF_SET(active_workspace_border);
    YSTR_IF_SET(active_workspace_bg);
    YSTR_IF_SET(active_workspace_text);
    YSTR_IF_SET(inactive_workspace_border);
    YSTR_IF_SET(inactive_workspace_bg);
    YSTR_IF_SET(inactive_workspace_text);
    YSTR_IF_SET(urgent_workspace_border);
    YSTR_IF_SET(urgent_workspace_bg);
    YSTR_IF_SET(urgent_workspace_text);
    YSTR_IF_SET(binding_mode_border);
    YSTR_IF_SET(binding_mode_bg);
    YSTR_IF_SET(binding_mode_text);
    y(map_close);

    y(map_close);
#undef YSTR_IF_SET
}

IPC_HANDLER(tree) {
    setlocale(LC_NUMERIC, "C");
    yajl_gen gen = ygenalloc();
    dump_node(gen, croot, false);
    setlocale(LC_NUMERIC, "");

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_TREE, payload);
    y(free);
}

/*
 * Formats the reply message for a GET_WORKSPACES request and sends it to the
 * client
 *
 */
IPC_HANDLER(get_workspaces) {
    yajl_gen gen = ygenalloc();
    y(array_open);

    Con *focused_ws = con_get_workspace(focused);

    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        if (con_is_internal(output))
            continue;
        Con *ws;
        TAILQ_FOREACH (ws, &(output_get_content(output)->nodes_head), nodes) {
            assert(ws->type == CT_WORKSPACE);
            y(map_open);

            ystr("id");
            y(integer, (uintptr_t)ws);

            ystr("num");
            y(integer, ws->num);

            ystr("name");
            ystr(ws->name);

            ystr("visible");
            y(bool, workspace_is_visible(ws));

            ystr("focused");
            y(bool, ws == focused_ws);

            ystr("rect");
            y(map_open);
            ystr("x");
            y(integer, ws->rect.x);
            ystr("y");
            y(integer, ws->rect.y);
            ystr("width");
            y(integer, ws->rect.width);
            ystr("height");
            y(integer, ws->rect.height);
            y(map_close);

            ystr("output");
            ystr(output->name);

            ystr("urgent");
            y(bool, ws->urgent);

            y(map_close);
        }
    }

    y(array_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_WORKSPACES, payload);
    y(free);
}

/*
 * Formats the reply message for a GET_OUTPUTS request and sends it to the
 * client
 *
 */
IPC_HANDLER(get_outputs) {
    yajl_gen gen = ygenalloc();
    y(array_open);

    Output *output;
    TAILQ_FOREACH (output, &outputs, outputs) {
        y(map_open);

        ystr("name");
        ystr(output_primary_name(output));

        ystr("active");
        y(bool, output->active);

        ystr("primary");
        y(bool, output->primary);

        ystr("rect");
        y(map_open);
        ystr("x");
        y(integer, output->rect.x);
        ystr("y");
        y(integer, output->rect.y);
        ystr("width");
        y(integer, output->rect.width);
        ystr("height");
        y(integer, output->rect.height);
        y(map_close);

        ystr("current_workspace");
        Con *ws = NULL;
        if (output->con && (ws = con_get_fullscreen_con(output->con, CF_OUTPUT)))
            ystr(ws->name);
        else
            y(null);

        y(map_close);
    }

    y(array_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_OUTPUTS, payload);
    y(free);
}

/*
 * Formats the reply message for a GET_MARKS request and sends it to the
 * client
 *
 */
IPC_HANDLER(get_marks) {
    yajl_gen gen = ygenalloc();
    y(array_open);

    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        mark_t *mark;
        TAILQ_FOREACH (mark, &(con->marks_head), marks) {
            ystr(mark->name);
        }
    }

    y(array_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_MARKS, payload);
    y(free);
}

/*
 * Returns the version of i3
 *
 */
IPC_HANDLER(get_version) {
    yajl_gen gen = ygenalloc();
    y(map_open);

    ystr("major");
    y(integer, MAJOR_VERSION);

    ystr("minor");
    y(integer, MINOR_VERSION);

    ystr("patch");
    y(integer, PATCH_VERSION);

    ystr("human_readable");
    ystr(i3_version);

    ystr("loaded_config_file_name");
    ystr(current_configpath);

    ystr("included_config_file_names");
    y(array_open);
    IncludedFile *file;
    TAILQ_FOREACH (file, &included_files, files) {
        if (file == TAILQ_FIRST(&included_files)) {
            /* Skip the first file, which is current_configpath. */
            continue;
        }
        ystr(file->path);
    }
    y(array_close);
    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_VERSION, payload);
    y(free);
}

/*
 * Formats the reply message for a GET_BAR_CONFIG request and sends it to the
 * client.
 *
 */
IPC_HANDLER(get_bar_config) {
    yajl_gen gen = ygenalloc();

    /* If no ID was passed, we return a JSON array with all IDs */
    if (message_size == 0) {
        y(array_open);
        Barconfig *current;
        TAILQ_FOREACH (current, &barconfigs, configs) {
            ystr(current->id);
        }
        y(array_close);

        const unsigned char *payload;
        ylength length;
        y(get_buf, &payload, &length);

        ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_BAR_CONFIG, payload);
        y(free);
        return;
    }

    /* To get a properly terminated buffer, we copy
     * message_size bytes out of the buffer */
    char *bar_id = NULL;
    sasprintf(&bar_id, "%.*s", message_size, message);
    LOG("IPC: looking for config for bar ID \"%s\"\n", bar_id);
    Barconfig *current, *config = NULL;
    TAILQ_FOREACH (current, &barconfigs, configs) {
        if (strcmp(current->id, bar_id) != 0)
            continue;

        config = current;
        break;
    }
    free(bar_id);

    if (!config) {
        /* If we did not find a config for the given ID, the reply will contain
         * a null 'id' field. */
        y(map_open);

        ystr("id");
        y(null);

        y(map_close);
    } else {
        dump_bar_config(gen, config);
    }

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_BAR_CONFIG, payload);
    y(free);
}

/*
 * Returns a list of configured binding modes
 *
 */
IPC_HANDLER(get_binding_modes) {
    yajl_gen gen = ygenalloc();

    y(array_open);
    struct Mode *mode;
    SLIST_FOREACH (mode, &modes, modes) {
        ystr(mode->name);
    }
    y(array_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_BINDING_MODES, payload);
    y(free);
}

/*
 * Callback for the YAJL parser (will be called when a string is parsed).
 *
 */
static int add_subscription(void *extra, const unsigned char *s,
                            ylength len) {
    ipc_client *client = extra;

    DLOG("should add subscription to extra %p, sub %.*s\n", client, (int)len, s);
    int event = client->num_events;

    client->num_events++;
    client->events = srealloc(client->events, client->num_events * sizeof(char *));
    /* We copy the string because it is not null-terminated and strndup()
     * is missing on some BSD systems */
    client->events[event] = scalloc(len + 1, 1);
    memcpy(client->events[event], s, len);

    DLOG("client is now subscribed to:\n");
    for (int i = 0; i < client->num_events; i++) {
        DLOG("event %s\n", client->events[i]);
    }
    DLOG("(done)\n");

    return 1;
}

/*
 * Subscribes this connection to the event types which were given as a JSON
 * serialized array in the payload field of the message.
 *
 */
IPC_HANDLER(subscribe) {
    yajl_handle p;
    yajl_status stat;

    /* Setup the JSON parser */
    static yajl_callbacks callbacks = {
        .yajl_string = add_subscription,
    };

    p = yalloc(&callbacks, (void *)client);
    stat = yajl_parse(p, (const unsigned char *)message, message_size);
    if (stat != yajl_status_ok) {
        unsigned char *err;
        err = yajl_get_error(p, true, (const unsigned char *)message,
                             message_size);
        ELOG("YAJL parse error: %s\n", err);
        yajl_free_error(p, err);

        const char *reply = "{\"success\":false}";
        ipc_send_client_message(client, strlen(reply), I3_IPC_REPLY_TYPE_SUBSCRIBE, (const uint8_t *)reply);
        yajl_free(p);
        return;
    }
    yajl_free(p);
    const char *reply = "{\"success\":true}";
    ipc_send_client_message(client, strlen(reply), I3_IPC_REPLY_TYPE_SUBSCRIBE, (const uint8_t *)reply);

    if (client->first_tick_sent) {
        return;
    }

    bool is_tick = false;
    for (int i = 0; i < client->num_events; i++) {
        if (strcmp(client->events[i], "tick") == 0) {
            is_tick = true;
            break;
        }
    }
    if (!is_tick) {
        return;
    }

    client->first_tick_sent = true;
    const char *payload = "{\"first\":true,\"payload\":\"\"}";
    ipc_send_client_message(client, strlen(payload), I3_IPC_EVENT_TICK, (const uint8_t *)payload);
}

/*
 * Returns the raw last loaded i3 configuration file contents.
 */
IPC_HANDLER(get_config) {
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("config");
    ystr(current_config);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_CONFIG, payload);
    y(free);
}

/*
 * Sends the tick event from the message payload to subscribers. Establishes a
 * synchronization point in event-related tests.
 */
IPC_HANDLER(send_tick) {
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("first");
    y(bool, false);

    ystr("payload");
    yajl_gen_string(gen, (unsigned char *)message, message_size);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("tick", I3_IPC_EVENT_TICK, (const char *)payload);
    y(free);

    const char *reply = "{\"success\":true}";
    ipc_send_client_message(client, strlen(reply), I3_IPC_REPLY_TYPE_TICK, (const uint8_t *)reply);
    DLOG("Sent tick event\n");
}

struct sync_state {
    char *last_key;
    uint32_t rnd;
    xcb_window_t window;
};

static int _sync_json_key(void *extra, const unsigned char *val, size_t len) {
    struct sync_state *state = extra;
    FREE(state->last_key);
    state->last_key = scalloc(len + 1, 1);
    memcpy(state->last_key, val, len);
    return 1;
}

static int _sync_json_int(void *extra, long long val) {
    struct sync_state *state = extra;
    if (strcasecmp(state->last_key, "rnd") == 0) {
        state->rnd = val;
    } else if (strcasecmp(state->last_key, "window") == 0) {
        state->window = (xcb_window_t)val;
    }
    return 1;
}

IPC_HANDLER(sync) {
    yajl_handle p;
    yajl_status stat;

    /* Setup the JSON parser */
    static yajl_callbacks callbacks = {
        .yajl_map_key = _sync_json_key,
        .yajl_integer = _sync_json_int,
    };

    struct sync_state state;
    memset(&state, '\0', sizeof(struct sync_state));
    p = yalloc(&callbacks, (void *)&state);
    stat = yajl_parse(p, (const unsigned char *)message, message_size);
    FREE(state.last_key);
    if (stat != yajl_status_ok) {
        unsigned char *err;
        err = yajl_get_error(p, true, (const unsigned char *)message,
                             message_size);
        ELOG("YAJL parse error: %s\n", err);
        yajl_free_error(p, err);

        const char *reply = "{\"success\":false}";
        ipc_send_client_message(client, strlen(reply), I3_IPC_REPLY_TYPE_SYNC, (const uint8_t *)reply);
        yajl_free(p);
        return;
    }
    yajl_free(p);

    DLOG("received IPC sync request (rnd = %d, window = 0x%08x)\n", state.rnd, state.window);
    sync_respond(state.window, state.rnd);
    const char *reply = "{\"success\":true}";
    ipc_send_client_message(client, strlen(reply), I3_IPC_REPLY_TYPE_SYNC, (const uint8_t *)reply);
}

IPC_HANDLER(get_binding_state) {
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("name");
    ystr(current_binding_mode);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_client_message(client, length, I3_IPC_REPLY_TYPE_GET_BINDING_STATE, payload);
    y(free);
}

/* The index of each callback function corresponds to the numeric
 * value of the message type (see include/i3/ipc.h) */
handler_t handlers[13] = {
    handle_run_command,
    handle_get_workspaces,
    handle_subscribe,
    handle_get_outputs,
    handle_tree,
    handle_get_marks,
    handle_get_bar_config,
    handle_get_version,
    handle_get_binding_modes,
    handle_get_config,
    handle_send_tick,
    handle_sync,
    handle_get_binding_state,
};

/*
 * Handler for activity on a client connection, receives a message from a
 * client.
 *
 * For now, the maximum message size is 2048. I’m not sure for what the
 * IPC interface will be used in the future, thus I’m not implementing a
 * mechanism for arbitrarily long messages, as it seems like overkill
 * at the moment.
 *
 */
static void ipc_receive_message(EV_P_ struct ev_io *w, int revents) {
    uint32_t message_type;
    uint32_t message_length;
    uint8_t *message = NULL;
    ipc_client *client = (ipc_client *)w->data;
    assert(client->fd == w->fd);

    int ret = ipc_recv_message(w->fd, &message_type, &message_length, &message);
    /* EOF or other error */
    if (ret < 0) {
        /* Was this a spurious read? See ev(3) */
        if (ret == -1 && errno == EAGAIN) {
            FREE(message);
            return;
        }

        /* If not, there was some kind of error. We don’t bother and close the
         * connection. Delete the client from the list of clients. */
        free_ipc_client(client, -1);
        FREE(message);
        return;
    }

    if (message_type >= (sizeof(handlers) / sizeof(handler_t)))
        DLOG("Unhandled message type: %d\n", message_type);
    else {
        handler_t h = handlers[message_type];
        h(client, message, 0, message_length, message_type);
    }

    FREE(message);
}

static void ipc_client_timeout(EV_P_ ev_timer *w, int revents) {
    /* No need to be polite and check for writeability, the other callback would
     * have been called by now. */
    ipc_client *client = (ipc_client *)w->data;

    char *cmdline = NULL;
#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred peercred;
    socklen_t so_len = sizeof(peercred);
    if (getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &peercred, &so_len) != 0) {
        goto end;
    }
    char *exepath;
    sasprintf(&exepath, "/proc/%d/cmdline", peercred.pid);

    int fd = open(exepath, O_RDONLY);
    free(exepath);
    if (fd == -1) {
        goto end;
    }
    char buf[512] = {'\0'}; /* cut off cmdline for the error message. */
    const ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) {
        goto end;
    }
    for (char *walk = buf; walk < buf + n - 1; walk++) {
        if (*walk == '\0') {
            *walk = ' ';
        }
    }
    cmdline = buf;

    if (cmdline) {
        ELOG("client %p with pid %d and cmdline '%s' on fd %d timed out, killing\n", client, peercred.pid, cmdline, client->fd);
    }

end:
#endif
    if (!cmdline) {
        ELOG("client %p on fd %d timed out, killing\n", client, client->fd);
    }

    free_ipc_client(client, -1);
}

static void ipc_socket_writeable_cb(EV_P_ ev_io *w, int revents) {
    DLOG("fd %d writeable\n", w->fd);
    ipc_client *client = (ipc_client *)w->data;

    /* If this callback is called then there should be a corresponding active
     * timer. */
    assert(client->timeout != NULL);
    ipc_push_pending(client);
}

/*
 * Handler for activity on the listening socket, meaning that a new client
 * has just connected and we should accept() him. Sets up the event handler
 * for activity on the new connection and inserts the file descriptor into
 * the list of clients.
 *
 */
void ipc_new_client(EV_P_ struct ev_io *w, int revents) {
    struct sockaddr_un peer;
    socklen_t len = sizeof(struct sockaddr_un);
    int fd;
    if ((fd = accept(w->fd, (struct sockaddr *)&peer, &len)) < 0) {
        if (errno != EINTR) {
            perror("accept()");
        }
        return;
    }

    /* Close this file descriptor on exec() */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    ipc_new_client_on_fd(EV_A_ fd);
}

/*
 * ipc_new_client_on_fd() only sets up the event handler
 * for activity on the new connection and inserts the file descriptor into
 * the list of clients.
 *
 * This variant is useful for the inherited IPC connection when restarting.
 *
 */
ipc_client *ipc_new_client_on_fd(EV_P_ int fd) {
    set_nonblock(fd);

    ipc_client *client = scalloc(1, sizeof(ipc_client));
    client->fd = fd;

    client->read_callback = scalloc(1, sizeof(struct ev_io));
    client->read_callback->data = client;
    ev_io_init(client->read_callback, ipc_receive_message, fd, EV_READ);
    ev_io_start(EV_A_ client->read_callback);

    client->write_callback = scalloc(1, sizeof(struct ev_io));
    client->write_callback->data = client;
    ev_io_init(client->write_callback, ipc_socket_writeable_cb, fd, EV_WRITE);

    DLOG("IPC: new client connected on fd %d\n", fd);
    TAILQ_INSERT_TAIL(&all_clients, client, clients);
    return client;
}

/*
 * Generates a json workspace event. Returns a dynamically allocated yajl
 * generator. Free with yajl_gen_free().
 */
yajl_gen ipc_marshal_workspace_event(const char *change, Con *current, Con *old) {
    setlocale(LC_NUMERIC, "C");
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("change");
    ystr(change);

    ystr("current");
    if (current == NULL)
        y(null);
    else
        dump_node(gen, current, false);

    ystr("old");
    if (old == NULL)
        y(null);
    else
        dump_node(gen, old, false);

    y(map_close);

    setlocale(LC_NUMERIC, "");

    return gen;
}

/*
 * For the workspace events we send, along with the usual "change" field, also
 * the workspace container in "current". For focus events, we send the
 * previously focused workspace in "old".
 */
void ipc_send_workspace_event(const char *change, Con *current, Con *old) {
    yajl_gen gen = ipc_marshal_workspace_event(change, current, old);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("workspace", I3_IPC_EVENT_WORKSPACE, (const char *)payload);

    y(free);
}

/*
 * For the window events we send, along the usual "change" field,
 * also the window container, in "container".
 */
void ipc_send_window_event(const char *property, Con *con) {
    DLOG("Issue IPC window %s event (con = %p, window = 0x%08x)\n",
         property, con, (con->window ? con->window->id : XCB_WINDOW_NONE));

    setlocale(LC_NUMERIC, "C");
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("change");
    ystr(property);

    ystr("container");
    dump_node(gen, con, false);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("window", I3_IPC_EVENT_WINDOW, (const char *)payload);
    y(free);
    setlocale(LC_NUMERIC, "");
}

/*
 * For the barconfig update events, we send the serialized barconfig.
 */
void ipc_send_barconfig_update_event(Barconfig *barconfig) {
    DLOG("Issue barconfig_update event for id = %s\n", barconfig->id);
    setlocale(LC_NUMERIC, "C");
    yajl_gen gen = ygenalloc();

    dump_bar_config(gen, barconfig);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("barconfig_update", I3_IPC_EVENT_BARCONFIG_UPDATE, (const char *)payload);
    y(free);
    setlocale(LC_NUMERIC, "");
}

/*
 * For the binding events, we send the serialized binding struct.
 */
void ipc_send_binding_event(const char *event_type, Binding *bind) {
    DLOG("Issue IPC binding %s event (sym = %s, code = %d)\n", event_type, bind->symbol, bind->keycode);

    setlocale(LC_NUMERIC, "C");

    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("change");
    ystr(event_type);

    ystr("binding");
    dump_binding(gen, bind);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("binding", I3_IPC_EVENT_BINDING, (const char *)payload);

    y(free);
    setlocale(LC_NUMERIC, "");
}

/*
 * Sends a restart reply to the IPC client on the specified fd.
 */
void ipc_confirm_restart(ipc_client *client) {
    DLOG("ipc_confirm_restart(fd %d)\n", client->fd);
    static const char *reply = "[{\"success\":true}]";
    ipc_send_client_message(
        client, strlen(reply), I3_IPC_REPLY_TYPE_COMMAND,
        (const uint8_t *)reply);
    ipc_push_pending(client);
}
