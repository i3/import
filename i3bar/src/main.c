/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 */
#include "common.h"

#include <ev.h>
#include <getopt.h>
#include <glob.h>
#include <i3/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ev_loop *main_loop;

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
 * Glob path, i.e. expand ~
 *
 */
static char *expand_path(char *path) {
    static glob_t globbuf;
    if (glob(path, GLOB_NOCHECK | GLOB_TILDE, NULL, &globbuf) < 0) {
        ELOG("glob() failed\n");
        exit(EXIT_FAILURE);
    }
    char *result = sstrdup(globbuf.gl_pathc > 0 ? globbuf.gl_pathv[0] : path);
    globfree(&globbuf);
    return result;
}

static void print_usage(char *elf_name) {
    printf("Usage: %s [-b bar_id] [-s sock_path] [-t] [-h] [-v] [-V]\n", elf_name);
    printf("\n");
    printf("-b, --bar_id       <bar_id>\tBar ID for which to get the configuration, defaults to the first bar from the i3 config\n");
    printf("-s, --socket       <sock_path>\tConnect to i3 via <sock_path>\n");
    printf("-t, --transparency Enable transparency (RGBA colors)\n");
    printf("-h, --help         Display this help message and exit\n");
    printf("-v, --version      Display version number and exit\n");
    printf("-V, --verbose      Enable verbose mode\n");
    printf("\n");
    printf(" PLEASE NOTE that i3bar will be automatically started by i3\n"
           " as soon as there is a 'bar' configuration block in your\n"
           " config file. You should never need to start it manually.\n");
    printf("\n");
}

/*
 * We watch various signals, that are there to make our application stop.
 * If we get one of those, we ev_unloop() and invoke the cleanup routines
 * in main() with that
 *
 */
static void sig_cb(struct ev_loop *loop, ev_signal *watcher, int revents) {
    switch (watcher->signum) {
        case SIGTERM:
            DLOG("Got a SIGTERM, stopping\n");
            break;
        case SIGINT:
            DLOG("Got a SIGINT, stopping\n");
            break;
        case SIGHUP:
            DLOG("Got a SIGHUP, stopping\n");
    }
    ev_unloop(main_loop, EVUNLOOP_ALL);
}

int main(int argc, char **argv) {
    char *socket_path = NULL;

    /* Initialize the standard config to use 0 as default */
    memset(&config, '\0', sizeof(config_t));

    static struct option long_opt[] = {
        {"socket", required_argument, 0, 's'},
        {"bar_id", required_argument, 0, 'b'},
        {"transparency", no_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"verbose", no_argument, 0, 'V'},
        {NULL, 0, 0, 0}};

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "b:s:thvV", long_opt, &option_index)) != -1) {
        switch (opt) {
            case 's':
                socket_path = expand_path(optarg);
                break;
            case 'v':
                printf("i3bar version " I3_VERSION " © 2010 Axel Wagner and contributors\n");
                exit(EXIT_SUCCESS);
                break;
            case 'b':
                config.bar_id = sstrdup(optarg);
                break;
            case 't':
                config.transparency = true;
                break;
            case 'V':
                config.verbose = true;
                break;
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
                break;
        }
    }

    LOG("i3bar version " I3_VERSION "\n");

    main_loop = ev_default_loop(0); /* needed in init_xcb_early */
    char *atom_sock_path = init_xcb_early();

    /* Select a socket_path if the user hasn't specified one */
    if (socket_path == NULL) {
        socket_path = getenv("I3SOCK");
        if (socket_path != NULL) {
            socket_path = sstrdup(socket_path);
        }
    }

    if (socket_path == NULL) {
        socket_path = atom_sock_path;
    } else {
        free(atom_sock_path);
    }

    if (socket_path == NULL) {
        char *i3_default_sock_path = "/tmp/i3-ipc.sock";
        ELOG("No socket path specified, default to %s\n", i3_default_sock_path);
        socket_path = sstrdup(i3_default_sock_path);
    }

    init_dpi();

    init_outputs();

    init_connection(socket_path);
    /* Request the bar configuration. When it arrives, we fill the config
     * array. In case that config.bar_id is empty, we will receive a list of
     * available configs and then request the configuration for the first bar.
     * See got_bar_config for more. */
    i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_BAR_CONFIG, config.bar_id);
    free(socket_path);

    /* We listen to SIGTERM/QUIT/INT and try to exit cleanly, by stopping the main loop.
     * We only need those watchers on the stack, so putting them on the stack saves us
     * some calls to free() */
    ev_signal *sig_term = smalloc(sizeof(ev_signal));
    ev_signal *sig_int = smalloc(sizeof(ev_signal));
    ev_signal *sig_hup = smalloc(sizeof(ev_signal));

    ev_signal_init(sig_term, &sig_cb, SIGTERM);
    ev_signal_init(sig_int, &sig_cb, SIGINT);
    ev_signal_init(sig_hup, &sig_cb, SIGHUP);

    ev_signal_start(main_loop, sig_term);
    ev_signal_start(main_loop, sig_int);
    ev_signal_start(main_loop, sig_hup);

    atexit(kill_child_at_exit);

    /* From here on everything should run smooth for itself, just start listening for
     * events. We stop simply stop the event loop, when we are finished */
    ev_loop(main_loop, 0);

    clean_xcb();
    ev_default_destroy();

    return 0;
}
