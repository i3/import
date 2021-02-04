/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * child.c: Getting input for the statusline
 *
 */
#pragma once

#include <config.h>

#include <ev.h>

#define STDIN_CHUNK_SIZE 1024

typedef struct {
    pid_t pid;

    /**
     * The version number is an uint32_t to avoid machines with different sizes of
     * 'int' to allow different values here. It’s highly unlikely we ever exceed
     * even an int8_t, but still…
     */
    uint32_t version;

    bool stopped;
    /**
     * The signal requested by the client to inform it of the hidden state of i3bar
     */
    int stop_signal;
    /**
     * The signal requested by the client to inform it of the unhidden state of i3bar
     */
    int cont_signal;

    /**
     * Enable click events
     */
    bool click_events;
    bool click_events_init;

    /**
     * stdin- and SIGCHLD-watchers
     */
    ev_io *stdin_io;
    ev_child *child_sig;
    int stdin_fd;
} i3bar_child;

/*
 * Remove all blocks from the given statusline.
 * If free_resources is set, the fields of each status block will be free'd.
 */
void clear_statusline(struct statusline_head *head, bool free_resources);

/*
 * Start a child process with the specified command and reroute stdin.
 * We actually start a shell to execute the command so we don't have to care
 * about arguments and such.
 *
 * If `command' is NULL, such as in the case when no `status_command' is given
 * in the bar config, no child will be started.
 *
 */
void start_child(char *command);

/*
 * Same as start_child but starts the configured client that manages workspace
 * buttons.
 *
 */
void start_ws_child(char *command);

/*
 * kill()s the child process (if any). Called when exit()ing.
 *
 */
void kill_child_at_exit(void);

/*
 * kill()s the child process (if any) and closes and free()s the stdin- and
 * SIGCHLD-watchers
 *
 */
void kill_child(void);

/*
 * kill()s the workspace child process (if any) and closes and free()s the
 * stdin- and SIGCHLD-watchers.
 * Similar to kill_child.
 *
 */
void kill_ws_child(void);

/*
 * Sends a SIGSTOP to the child process (if existent)
 *
 */
void stop_child(void);

/*
 * Sends a SIGCONT to the child process (if existent)
 *
 */
void cont_child(void);

/*
 * Whether or not the child want click events
 *
 */
bool child_want_click_events(void);

/*
 * Generates a click event, if enabled.
 *
 */
void send_block_clicked(int button, const char *name, const char *instance, int x, int y, int x_rel, int y_rel, int out_x, int out_y, int width, int height, int mods);

/*
 * When workspace_command is enabled this function is used to re-parse the
 * latest received JSON from the client.
 */
void repeat_last_ws_json(void);

/*
 * Replaces the workspace buttons with an error message.
 */
void set_workspace_button_error(const char *message);
