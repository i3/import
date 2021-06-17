/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * main.c: Initialization, main loop
 *
 */
#include "all.h"
#include "shmlog.h"

#include <ev.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <locale.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <xcb/xcb_atom.h>
#include <xcb/xinerama.h>
#include <xcb/bigreq.h>

#ifdef I3_ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

#include "sd-daemon.h"

#include "i3-atoms_NET_SUPPORTED.xmacro.h"
#include "i3-atoms_rest.xmacro.h"

/* The original value of RLIMIT_CORE when i3 was started. We need to restore
 * this before starting any other process, since we set RLIMIT_CORE to
 * RLIM_INFINITY for i3 debugging versions. */
struct rlimit original_rlimit_core;

/* The number of file descriptors passed via socket activation. */
int listen_fds;

/* We keep the xcb_prepare watcher around to be able to enable and disable it
 * temporarily for drag_pointer(). */
static struct ev_prepare *xcb_prepare;

char **start_argv;

xcb_connection_t *conn;
/* The screen (0 when you are using DISPLAY=:0) of the connection 'conn' */
int conn_screen;

/* Display handle for libstartup-notification */
SnDisplay *sndisplay;

/* The last timestamp we got from X11 (timestamps are included in some events
 * and are used for some things, like determining a unique ID in startup
 * notification). */
xcb_timestamp_t last_timestamp = XCB_CURRENT_TIME;

xcb_screen_t *root_screen;
xcb_window_t root;

xcb_window_t wm_sn_selection_owner;
xcb_atom_t wm_sn;

/* Color depth, visual id and colormap to use when creating windows and
 * pixmaps. Will use 32 bit depth and an appropriate visual, if available,
 * otherwise the root window’s default (usually 24 bit TrueColor). */
uint8_t root_depth;
xcb_visualtype_t *visual_type;
xcb_colormap_t colormap;

struct ev_loop *main_loop;

xcb_key_symbols_t *keysyms;

/* Default shmlog size if not set by user. */
const int default_shmlog_size = 25 * 1024 * 1024;

/* The list of key bindings */
struct bindings_head *bindings;
const char *current_binding_mode = NULL;

/* The list of exec-lines */
struct autostarts_head autostarts = TAILQ_HEAD_INITIALIZER(autostarts);

/* The list of exec_always lines */
struct autostarts_always_head autostarts_always = TAILQ_HEAD_INITIALIZER(autostarts_always);

/* The list of assignments */
struct assignments_head assignments = TAILQ_HEAD_INITIALIZER(assignments);

/* The list of workspace assignments (which workspace should end up on which
 * output) */
struct ws_assignments_head ws_assignments = TAILQ_HEAD_INITIALIZER(ws_assignments);

/* We hope that those are supported and set them to true */
bool xkb_supported = true;
bool shape_supported = true;

bool force_xinerama = false;

/* Define all atoms as global variables */
#define xmacro(atom) xcb_atom_t A_##atom;
I3_NET_SUPPORTED_ATOMS_XMACRO
I3_REST_ATOMS_XMACRO
#undef xmacro

/*
 * This callback is only a dummy, see xcb_prepare_cb.
 * See also man libev(3): "ev_prepare" and "ev_check" - customise your event loop
 *
 */
static void xcb_got_event(EV_P_ struct ev_io *w, int revents) {
    /* empty, because xcb_prepare_cb are used */
}

/*
 * Called just before the event loop sleeps. Ensures xcb’s incoming and outgoing
 * queues are empty so that any activity will trigger another event loop
 * iteration, and hence another xcb_prepare_cb invocation.
 *
 */
static void xcb_prepare_cb(EV_P_ ev_prepare *w, int revents) {
    /* Process all queued (and possibly new) events before the event loop
       sleeps. */
    xcb_generic_event_t *event;

    while ((event = xcb_poll_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            if (event_is_ignored(event->sequence, 0))
                DLOG("Expected X11 Error received for sequence %x\n", event->sequence);
            else {
                xcb_generic_error_t *error = (xcb_generic_error_t *)event;
                DLOG("X11 Error received (probably harmless)! sequence 0x%x, error_code = %d\n",
                     error->sequence, error->error_code);
            }
            free(event);
            continue;
        }

        /* Strip off the highest bit (set if the event is generated) */
        int type = (event->response_type & 0x7F);

        handle_event(type, event);

        free(event);
    }

    /* Flush all queued events to X11. */
    xcb_flush(conn);
}

/*
 * Enable or disable the main X11 event handling function.
 * This is used by drag_pointer() which has its own, modal event handler, which
 * takes precedence over the normal event handler.
 *
 */
void main_set_x11_cb(bool enable) {
    DLOG("Setting main X11 callback to enabled=%d\n", enable);
    if (enable) {
        ev_prepare_start(main_loop, xcb_prepare);
        /* Trigger the watcher explicitly to handle all remaining X11 events.
         * drag_pointer()’s event handler exits in the middle of the loop. */
        ev_feed_event(main_loop, xcb_prepare, 0);
    } else {
        ev_prepare_stop(main_loop, xcb_prepare);
    }
}

/*
 * Exit handler which destroys the main_loop. Will trigger cleanup handlers.
 *
 */
static void i3_exit(void) {
    if (*shmlogname != '\0') {
        fprintf(stderr, "Closing SHM log \"%s\"\n", shmlogname);
        fflush(stderr);
        shm_unlink(shmlogname);
    }
    ipc_shutdown(SHUTDOWN_REASON_EXIT, -1);
    unlink(config.ipc_socket_path);
    if (current_log_stream_socket_path != NULL) {
        unlink(current_log_stream_socket_path);
    }
    xcb_disconnect(conn);

    /* If a nagbar is active, kill it */
    kill_nagbar(config_error_nagbar_pid, false);
    kill_nagbar(command_error_nagbar_pid, false);

/* We need ev >= 4 for the following code. Since it is not *that* important (it
 * only makes sure that there are no i3-nagbar instances left behind) we still
 * support old systems with libev 3. */
#if EV_VERSION_MAJOR >= 4
    ev_loop_destroy(main_loop);
#endif

#ifdef I3_ASAN_ENABLED
    __lsan_do_leak_check();
#endif
}

/*
 * (One-shot) Handler for all signals with default action "Core", see signal(7)
 *
 * Unlinks the SHM log and re-raises the signal.
 *
 */
static void handle_core_signal(int sig, siginfo_t *info, void *data) {
    if (*shmlogname != '\0') {
        shm_unlink(shmlogname);
    }
    raise(sig);
}

/*
 * (One-shot) Handler for all signals with default action "Term", see signal(7)
 *
 * Exits the program gracefully.
 *
 */
static void handle_term_signal(struct ev_loop *loop, ev_signal *signal, int revents) {
    /* We exit gracefully here in the sense that cleanup handlers
     * installed via atexit are invoked. */
    exit(128 + signal->signum);
}

/*
 * Set up handlers for all signals with default action "Term", see signal(7)
 *
 */
static void setup_term_handlers(void) {
    static struct ev_signal signal_watchers[6];
    size_t num_watchers = sizeof(signal_watchers) / sizeof(signal_watchers[0]);

    /* We have to rely on libev functionality here and should not use
     * sigaction handlers because we need to invoke the exit handlers
     * and cannot do so from an asynchronous signal handling context as
     * not all code triggered during exit is signal safe (and exiting
     * the main loop from said handler is not easily possible). libev's
     * signal handlers does not impose such a constraint on us. */
    ev_signal_init(&signal_watchers[0], handle_term_signal, SIGHUP);
    ev_signal_init(&signal_watchers[1], handle_term_signal, SIGINT);
    ev_signal_init(&signal_watchers[2], handle_term_signal, SIGALRM);
    ev_signal_init(&signal_watchers[3], handle_term_signal, SIGTERM);
    ev_signal_init(&signal_watchers[4], handle_term_signal, SIGUSR1);
    ev_signal_init(&signal_watchers[5], handle_term_signal, SIGUSR1);
    for (size_t i = 0; i < num_watchers; i++) {
        ev_signal_start(main_loop, &signal_watchers[i]);
        /* The signal handlers should not block ev_run from returning
         * and so none of the signal handlers should hold a reference to
         * the main loop. */
        ev_unref(main_loop);
    }
}

static int parse_restart_fd(void) {
    const char *restart_fd = getenv("_I3_RESTART_FD");
    if (restart_fd == NULL) {
        return -1;
    }

    long int fd = -1;
    if (!parse_long(restart_fd, &fd, 10)) {
        ELOG("Malformed _I3_RESTART_FD \"%s\"\n", restart_fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    /* Keep a symbol pointing to the I3_VERSION string constant so that we have
     * it in gdb backtraces. */
    static const char *_i3_version __attribute__((used)) = I3_VERSION;
    char *override_configpath = NULL;
    bool autostart = true;
    char *layout_path = NULL;
    bool delete_layout_path = false;
    bool disable_randr15 = false;
    char *fake_outputs = NULL;
    bool disable_signalhandler = false;
    bool only_check_config = false;
    bool replace_wm = false;
    static struct option long_options[] = {
        {"no-autostart", no_argument, 0, 'a'},
        {"config", required_argument, 0, 'c'},
        {"version", no_argument, 0, 'v'},
        {"moreversion", no_argument, 0, 'm'},
        {"more-version", no_argument, 0, 'm'},
        {"more_version", no_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {"layout", required_argument, 0, 'L'},
        {"restart", required_argument, 0, 0},
        {"force-xinerama", no_argument, 0, 0},
        {"force_xinerama", no_argument, 0, 0},
        {"disable-randr15", no_argument, 0, 0},
        {"disable_randr15", no_argument, 0, 0},
        {"disable-signalhandler", no_argument, 0, 0},
        {"shmlog-size", required_argument, 0, 0},
        {"shmlog_size", required_argument, 0, 0},
        {"get-socketpath", no_argument, 0, 0},
        {"get_socketpath", no_argument, 0, 0},
        {"fake_outputs", required_argument, 0, 0},
        {"fake-outputs", required_argument, 0, 0},
        {"force-old-config-parser-v4.4-only", no_argument, 0, 0},
        {"replace", no_argument, 0, 'r'},
        {0, 0, 0, 0}};
    int option_index = 0, opt;

    setlocale(LC_ALL, "");

    /* Get the RLIMIT_CORE limit at startup time to restore this before
     * starting processes. */
    getrlimit(RLIMIT_CORE, &original_rlimit_core);

    /* Disable output buffering to make redirects in .xsession actually useful for debugging */
    if (!isatty(fileno(stdout)))
        setbuf(stdout, NULL);

    srand(time(NULL));

    /* Init logging *before* initializing debug_build to guarantee early
     * (file) logging. */
    init_logging();

    /* On release builds, disable SHM logging by default. */
    shmlog_size = (is_debug_build() || strstr(argv[0], "i3-with-shmlog") != NULL ? default_shmlog_size : 0);

    start_argv = argv;

    while ((opt = getopt_long(argc, argv, "c:CvmaL:hld:Vr", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'a':
                LOG("Autostart disabled using -a\n");
                autostart = false;
                break;
            case 'L':
                FREE(layout_path);
                layout_path = sstrdup(optarg);
                delete_layout_path = false;
                break;
            case 'c':
                FREE(override_configpath);
                override_configpath = sstrdup(optarg);
                break;
            case 'C':
                LOG("Checking configuration file only (-C)\n");
                only_check_config = true;
                break;
            case 'v':
                printf("i3 version %s © 2009 Michael Stapelberg and contributors\n", i3_version);
                exit(EXIT_SUCCESS);
                break;
            case 'm':
                printf("Binary i3 version:  %s © 2009 Michael Stapelberg and contributors\n", i3_version);
                display_running_version();
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                set_verbosity(true);
                break;
            case 'd':
                LOG("Enabling debug logging\n");
                set_debug_logging(true);
                break;
            case 'l':
                /* DEPRECATED, ignored for the next 3 versions (3.e, 3.f, 3.g) */
                break;
            case 'r':
                replace_wm = true;
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "force-xinerama") == 0 ||
                    strcmp(long_options[option_index].name, "force_xinerama") == 0) {
                    force_xinerama = true;
                    ELOG("Using Xinerama instead of RandR. This option should be "
                         "avoided at all cost because it does not refresh the list "
                         "of screens, so you cannot configure displays at runtime. "
                         "Please check if your driver really does not support RandR "
                         "and disable this option as soon as you can.\n");
                    break;
                } else if (strcmp(long_options[option_index].name, "disable-randr15") == 0 ||
                           strcmp(long_options[option_index].name, "disable_randr15") == 0) {
                    disable_randr15 = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "disable-signalhandler") == 0) {
                    disable_signalhandler = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "get-socketpath") == 0 ||
                           strcmp(long_options[option_index].name, "get_socketpath") == 0) {
                    char *socket_path = root_atom_contents("I3_SOCKET_PATH", NULL, 0);
                    if (socket_path) {
                        printf("%s\n", socket_path);
                        /* With -O2 (i.e. the buildtype=debugoptimized meson
                         * option, which we set by default), gcc 9.2.1 optimizes
                         * away socket_path at this point, resulting in a Leak
                         * Sanitizer report. An explicit free helps: */
                        free(socket_path);
                        exit(EXIT_SUCCESS);
                    }

                    exit(EXIT_FAILURE);
                } else if (strcmp(long_options[option_index].name, "shmlog-size") == 0 ||
                           strcmp(long_options[option_index].name, "shmlog_size") == 0) {
                    shmlog_size = atoi(optarg);
                    /* Re-initialize logging immediately to get as many
                     * logmessages as possible into the SHM log. */
                    init_logging();
                    LOG("Limiting SHM log size to %d bytes\n", shmlog_size);
                    break;
                } else if (strcmp(long_options[option_index].name, "restart") == 0) {
                    FREE(layout_path);
                    layout_path = sstrdup(optarg);
                    delete_layout_path = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "fake-outputs") == 0 ||
                           strcmp(long_options[option_index].name, "fake_outputs") == 0) {
                    LOG("Initializing fake outputs: %s\n", optarg);
                    fake_outputs = sstrdup(optarg);
                    break;
                } else if (strcmp(long_options[option_index].name, "force-old-config-parser-v4.4-only") == 0) {
                    ELOG("You are passing --force-old-config-parser-v4.4-only, but that flag was removed by now.\n");
                    break;
                }
            /* fall-through */
            default:
                fprintf(stderr, "Usage: %s [-c configfile] [-d all] [-a] [-v] [-V] [-C]\n", argv[0]);
                fprintf(stderr, "\n");
                fprintf(stderr, "\t-a          disable autostart ('exec' lines in config)\n");
                fprintf(stderr, "\t-c <file>   use the provided configfile instead\n");
                fprintf(stderr, "\t-C          validate configuration file and exit\n");
                fprintf(stderr, "\t-d all      enable debug output\n");
                fprintf(stderr, "\t-L <file>   path to the serialized layout during restarts\n");
                fprintf(stderr, "\t-v          display version and exit\n");
                fprintf(stderr, "\t-V          enable verbose mode\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--force-xinerama\n"
                                "\tUse Xinerama instead of RandR.\n"
                                "\tThis option should only be used if you are stuck with the\n"
                                "\told nVidia closed source driver (older than 302.17), which does\n"
                                "\tnot support RandR.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--get-socketpath\n"
                                "\tRetrieve the i3 IPC socket path from X11, print it, then exit.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--shmlog-size <limit>\n"
                                "\tLimits the size of the i3 SHM log to <limit> bytes. Setting this\n"
                                "\tto 0 disables SHM logging entirely.\n"
                                "\tThe default is %d bytes.\n",
                        shmlog_size);
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--replace\n"
                                "\tReplace an existing window manager.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "If you pass plain text arguments, i3 will interpret them as a command\n"
                                "to send to a currently running i3 (like i3-msg). This allows you to\n"
                                "use nice and logical commands, such as:\n"
                                "\n"
                                "\ti3 border none\n"
                                "\ti3 floating toggle\n"
                                "\ti3 kill window\n"
                                "\n");
                exit(opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    if (only_check_config) {
        exit(load_configuration(override_configpath, C_VALIDATE) ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    /* If the user passes more arguments, we act like i3-msg would: Just send
     * the arguments as an IPC message to i3. This allows for nice semantic
     * commands such as 'i3 border none'. */
    if (optind < argc) {
        /* We enable verbose mode so that the user knows what’s going on.
         * This should make it easier to find mistakes when the user passes
         * arguments by mistake. */
        set_verbosity(true);

        LOG("Additional arguments passed. Sending them as a command to i3.\n");
        char *payload = NULL;
        while (optind < argc) {
            if (!payload) {
                payload = sstrdup(argv[optind]);
            } else {
                char *both;
                sasprintf(&both, "%s %s", payload, argv[optind]);
                free(payload);
                payload = both;
            }
            optind++;
        }
        DLOG("Command is: %s (%zd bytes)\n", payload, strlen(payload));
        char *socket_path = root_atom_contents("I3_SOCKET_PATH", NULL, 0);
        if (!socket_path) {
            ELOG("Could not get i3 IPC socket path\n");
            return 1;
        }

        int sockfd = socket(AF_LOCAL, SOCK_STREAM, 0);
        if (sockfd == -1)
            err(EXIT_FAILURE, "Could not create socket");

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_LOCAL;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
        FREE(socket_path);
        if (connect(sockfd, (const struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0)
            err(EXIT_FAILURE, "Could not connect to i3");

        if (ipc_send_message(sockfd, strlen(payload), I3_IPC_MESSAGE_TYPE_RUN_COMMAND,
                             (uint8_t *)payload) == -1)
            err(EXIT_FAILURE, "IPC: write()");
        FREE(payload);

        uint32_t reply_length;
        uint32_t reply_type;
        uint8_t *reply;
        int ret;
        if ((ret = ipc_recv_message(sockfd, &reply_type, &reply_length, &reply)) != 0) {
            if (ret == -1)
                err(EXIT_FAILURE, "IPC: read()");
            return 1;
        }
        if (reply_type != I3_IPC_REPLY_TYPE_COMMAND)
            errx(EXIT_FAILURE, "IPC: received reply of type %d but expected %d (COMMAND)", reply_type, I3_IPC_REPLY_TYPE_COMMAND);
        printf("%.*s\n", reply_length, reply);
        FREE(reply);
        return 0;
    }

    /* Enable logging to handle the case when the user did not specify --shmlog-size */
    init_logging();

    /* Try to enable core dumps by default when running a debug build */
    if (is_debug_build()) {
        struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
        setrlimit(RLIMIT_CORE, &limit);

        /* The following code is helpful, but not required. We thus don’t pay
         * much attention to error handling, non-linux or other edge cases. */
        LOG("CORE DUMPS: You are running a development version of i3, so coredumps were automatically enabled (ulimit -c unlimited).\n");
        size_t cwd_size = 1024;
        char *cwd = smalloc(cwd_size);
        char *cwd_ret;
        while ((cwd_ret = getcwd(cwd, cwd_size)) == NULL && errno == ERANGE) {
            cwd_size = cwd_size * 2;
            cwd = srealloc(cwd, cwd_size);
        }
        if (cwd_ret != NULL)
            LOG("CORE DUMPS: Your current working directory is \"%s\".\n", cwd);
        int patternfd;
        if ((patternfd = open("/proc/sys/kernel/core_pattern", O_RDONLY)) >= 0) {
            memset(cwd, '\0', cwd_size);
            if (read(patternfd, cwd, cwd_size) > 0)
                /* a trailing newline is included in cwd */
                LOG("CORE DUMPS: Your core_pattern is: %s", cwd);
            close(patternfd);
        }
        free(cwd);
    }

    LOG("i3 %s starting\n", i3_version);

    conn = xcb_connect(NULL, &conn_screen);
    if (xcb_connection_has_error(conn))
        errx(EXIT_FAILURE, "Cannot open display");

    sndisplay = sn_xcb_display_new(conn, NULL, NULL);

    /* Initialize the libev event loop. This needs to be done before loading
     * the config file because the parser will install an ev_child watcher
     * for the nagbar when config errors are found. */
    main_loop = EV_DEFAULT;
    if (main_loop == NULL)
        die("Could not initialize libev. Bad LIBEV_FLAGS?\n");

    root_screen = xcb_aux_get_screen(conn, conn_screen);
    root = root_screen->root;

    /* Prefetch X11 extensions that we are interested in. */
    xcb_prefetch_extension_data(conn, &xcb_xkb_id);
    xcb_prefetch_extension_data(conn, &xcb_shape_id);
    /* BIG-REQUESTS is used by libxcb internally. */
    xcb_prefetch_extension_data(conn, &xcb_big_requests_id);
    if (force_xinerama) {
        xcb_prefetch_extension_data(conn, &xcb_xinerama_id);
    } else {
        xcb_prefetch_extension_data(conn, &xcb_randr_id);
    }

    /* Prepare for us to get a current timestamp as recommended by ICCCM */
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){XCB_EVENT_MASK_PROPERTY_CHANGE});
    xcb_change_property(conn, XCB_PROP_MODE_APPEND, root, XCB_ATOM_SUPERSCRIPT_X, XCB_ATOM_CARDINAL, 32, 0, "");

    /* Place requests for the atoms we need as soon as possible */
#define xmacro(atom) \
    xcb_intern_atom_cookie_t atom##_cookie = xcb_intern_atom(conn, 0, strlen(#atom), #atom);
    I3_NET_SUPPORTED_ATOMS_XMACRO
    I3_REST_ATOMS_XMACRO
#undef xmacro

    root_depth = root_screen->root_depth;
    colormap = root_screen->default_colormap;
    visual_type = xcb_aux_find_visual_by_attrs(root_screen, -1, 32);
    if (visual_type != NULL) {
        root_depth = xcb_aux_get_depth_of_visual(root_screen, visual_type->visual_id);
        colormap = xcb_generate_id(conn);

        xcb_void_cookie_t cm_cookie = xcb_create_colormap_checked(conn,
                                                                  XCB_COLORMAP_ALLOC_NONE,
                                                                  colormap,
                                                                  root,
                                                                  visual_type->visual_id);

        xcb_generic_error_t *error = xcb_request_check(conn, cm_cookie);
        if (error != NULL) {
            ELOG("Could not create colormap. Error code: %d\n", error->error_code);
            exit(EXIT_FAILURE);
        }
    } else {
        visual_type = get_visualtype(root_screen);
    }

    xcb_prefetch_maximum_request_length(conn);

    init_dpi();

    DLOG("root_depth = %d, visual_id = 0x%08x.\n", root_depth, visual_type->visual_id);
    DLOG("root_screen->height_in_pixels = %d, root_screen->height_in_millimeters = %d\n",
         root_screen->height_in_pixels, root_screen->height_in_millimeters);
    DLOG("One logical pixel corresponds to %d physical pixels on this display.\n", logical_px(1));

    xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(conn, root);
    xcb_query_pointer_cookie_t pointercookie = xcb_query_pointer(conn, root);

    /* Get the PropertyNotify event we caused above */
    xcb_flush(conn);
    {
        xcb_generic_event_t *event;
        DLOG("waiting for PropertyNotify event\n");
        while ((event = xcb_wait_for_event(conn)) != NULL) {
            if (event->response_type == XCB_PROPERTY_NOTIFY) {
                last_timestamp = ((xcb_property_notify_event_t *)event)->time;
                free(event);
                break;
            }
            free(event);
        }
        DLOG("got timestamp %d\n", last_timestamp);
    }

    /* Setup NetWM atoms */
#define xmacro(name)                                                                       \
    do {                                                                                   \
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, name##_cookie, NULL); \
        if (!reply) {                                                                      \
            ELOG("Could not get atom " #name "\n");                                        \
            exit(-1);                                                                      \
        }                                                                                  \
        A_##name = reply->atom;                                                            \
        free(reply);                                                                       \
    } while (0);
    I3_NET_SUPPORTED_ATOMS_XMACRO
    I3_REST_ATOMS_XMACRO
#undef xmacro

    load_configuration(override_configpath, C_LOAD);

    if (config.ipc_socket_path == NULL) {
        /* Fall back to a file name in /tmp/ based on the PID */
        if ((config.ipc_socket_path = getenv("I3SOCK")) == NULL)
            config.ipc_socket_path = get_process_filename("ipc-socket");
        else
            config.ipc_socket_path = sstrdup(config.ipc_socket_path);
    }

    if (config.force_xinerama) {
        force_xinerama = true;
    }

    /* Acquire the WM_Sn selection. */
    {
        /* Get the WM_Sn atom */
        char *atom_name = xcb_atom_name_by_screen("WM_S", conn_screen);
        wm_sn_selection_owner = xcb_generate_id(conn);

        if (atom_name == NULL) {
            ELOG("xcb_atom_name_by_screen(\"WM_S\", %d) failed, exiting\n", conn_screen);
            return 1;
        }

        xcb_intern_atom_reply_t *atom_reply;
        atom_reply = xcb_intern_atom_reply(conn,
                                           xcb_intern_atom_unchecked(conn,
                                                                     0,
                                                                     strlen(atom_name),
                                                                     atom_name),
                                           NULL);
        free(atom_name);
        if (atom_reply == NULL) {
            ELOG("Failed to intern the WM_Sn atom, exiting\n");
            return 1;
        }
        wm_sn = atom_reply->atom;
        free(atom_reply);

        /* Check if the selection is already owned */
        xcb_get_selection_owner_reply_t *selection_reply =
            xcb_get_selection_owner_reply(conn,
                                          xcb_get_selection_owner(conn, wm_sn),
                                          NULL);
        if (selection_reply && selection_reply->owner != XCB_NONE && !replace_wm) {
            ELOG("Another window manager is already running (WM_Sn is owned)");
            return 1;
        }

        /* Become the selection owner */
        xcb_create_window(conn,
                          root_screen->root_depth,
                          wm_sn_selection_owner, /* window id */
                          root_screen->root,     /* parent */
                          -1, -1, 1, 1,          /* geometry */
                          0,                     /* border width */
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          root_screen->root_visual,
                          0, NULL);
        xcb_change_property(conn,
                            XCB_PROP_MODE_REPLACE,
                            wm_sn_selection_owner,
                            XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING,
                            8,
                            (strlen("i3-WM_Sn") + 1) * 2,
                            "i3-WM_Sn\0i3-WM_Sn\0");

        xcb_set_selection_owner(conn, wm_sn_selection_owner, wm_sn, last_timestamp);

        if (selection_reply && selection_reply->owner != XCB_NONE) {
            unsigned int usleep_time = 100000; /* 0.1 seconds */
            int check_rounds = 150;            /* Wait for a maximum of 15 seconds */
            xcb_get_geometry_reply_t *geom_reply = NULL;

            DLOG("waiting for old WM_Sn selection owner to exit");
            do {
                free(geom_reply);
                usleep(usleep_time);
                if (check_rounds-- == 0) {
                    ELOG("The old window manager is not exiting");
                    return 1;
                }
                geom_reply = xcb_get_geometry_reply(conn,
                                                    xcb_get_geometry(conn, selection_reply->owner),
                                                    NULL);
            } while (geom_reply != NULL);
        }
        free(selection_reply);

        /* Announce that we are the new owner */
        /* Every X11 event is 32 bytes long. Therefore, XCB will copy 32 bytes.
         * In order to properly initialize these bytes, we allocate 32 bytes even
         * though we only need less for an xcb_client_message_event_t */
        union {
            xcb_client_message_event_t message;
            char storage[32];
        } event;
        memset(&event, 0, sizeof(event));
        event.message.response_type = XCB_CLIENT_MESSAGE;
        event.message.window = root_screen->root;
        event.message.format = 32;
        event.message.type = A_MANAGER;
        event.message.data.data32[0] = last_timestamp;
        event.message.data.data32[1] = wm_sn;
        event.message.data.data32[2] = wm_sn_selection_owner;

        xcb_send_event(conn, 0, root_screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, event.storage);
    }

    xcb_void_cookie_t cookie;
    cookie = xcb_change_window_attributes_checked(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){ROOT_EVENT_MASK});
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error != NULL) {
        ELOG("Another window manager seems to be running (X error %d)\n", error->error_code);
#ifdef I3_ASAN_ENABLED
        __lsan_do_leak_check();
#endif
        return 1;
    }

    xcb_get_geometry_reply_t *greply = xcb_get_geometry_reply(conn, gcookie, NULL);
    if (greply == NULL) {
        ELOG("Could not get geometry of the root window, exiting\n");
        return 1;
    }
    DLOG("root geometry reply: (%d, %d) %d x %d\n", greply->x, greply->y, greply->width, greply->height);

    xcursor_load_cursors();

    /* Set a cursor for the root window (otherwise the root window will show no
       cursor until the first client is launched). */
    xcursor_set_root_cursor(XCURSOR_CURSOR_POINTER);

    const xcb_query_extension_reply_t *extreply;
    extreply = xcb_get_extension_data(conn, &xcb_xkb_id);
    xkb_supported = extreply->present;
    if (!extreply->present) {
        DLOG("xkb is not present on this server\n");
    } else {
        DLOG("initializing xcb-xkb\n");
        xcb_xkb_use_extension(conn, XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);
        xcb_xkb_select_events(conn,
                              XCB_XKB_ID_USE_CORE_KBD,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY,
                              0,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY,
                              0xff,
                              0xff,
                              NULL);

        /* Setting both, XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE and
         * XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED, will lead to the
         * X server sending us the full XKB state in KeyPress and KeyRelease:
         * https://cgit.freedesktop.org/xorg/xserver/tree/xkb/xkbEvents.c?h=xorg-server-1.20.0#n927
         *
         * XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT enable detectable autorepeat:
         * https://www.x.org/releases/current/doc/kbproto/xkbproto.html#Detectable_Autorepeat
         * This affects bindings using the --release flag: instead of getting multiple KeyRelease
         * events we get only one event when the key is physically released by the user.
         */
        const uint32_t mask = XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE |
                              XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED |
                              XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT;
        xcb_xkb_per_client_flags_reply_t *pcf_reply;
        /* The last three parameters are unset because they are only relevant
         * when using a feature called “automatic reset of boolean controls”:
         * https://www.x.org/releases/X11R7.7/doc/kbproto/xkbproto.html#Automatic_Reset_of_Boolean_Controls
         * */
        pcf_reply = xcb_xkb_per_client_flags_reply(
            conn,
            xcb_xkb_per_client_flags(
                conn,
                XCB_XKB_ID_USE_CORE_KBD,
                mask,
                mask,
                0 /* uint32_t ctrlsToChange */,
                0 /* uint32_t autoCtrls */,
                0 /* uint32_t autoCtrlsValues */),
            NULL);

#define PCF_REPLY_ERROR(_value)                                    \
    do {                                                           \
        if (pcf_reply == NULL || !(pcf_reply->value & (_value))) { \
            ELOG("Could not set " #_value "\n");                   \
        }                                                          \
    } while (0)

        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE);
        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED);
        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT);

        free(pcf_reply);
        xkb_base = extreply->first_event;
    }

    /* Check for Shape extension. We want to handle input shapes which is
     * introduced in 1.1. */
    extreply = xcb_get_extension_data(conn, &xcb_shape_id);
    if (extreply->present) {
        shape_base = extreply->first_event;
        xcb_shape_query_version_cookie_t cookie = xcb_shape_query_version(conn);
        xcb_shape_query_version_reply_t *version =
            xcb_shape_query_version_reply(conn, cookie, NULL);
        shape_supported = version && version->minor_version >= 1;
        free(version);
    } else {
        shape_supported = false;
    }
    if (!shape_supported) {
        DLOG("shape 1.1 is not present on this server\n");
    }

    restore_connect();

    property_handlers_init();

    ewmh_setup_hints();

    keysyms = xcb_key_symbols_alloc(conn);

    xcb_numlock_mask = aio_get_mod_mask_for(XCB_NUM_LOCK, keysyms);

    if (!load_keymap())
        die("Could not load keymap\n");

    translate_keysyms();
    grab_all_keys(conn);

    bool needs_tree_init = true;
    if (layout_path != NULL) {
        LOG("Trying to restore the layout from \"%s\".\n", layout_path);
        needs_tree_init = !tree_restore(layout_path, greply);
        if (delete_layout_path) {
            unlink(layout_path);
            const char *dir = dirname(layout_path);
            /* possibly fails with ENOTEMPTY if there are files (or
             * sockets) left. */
            rmdir(dir);
        }
    }
    if (needs_tree_init)
        tree_init(greply);

    free(greply);

    /* Setup fake outputs for testing */
    if (fake_outputs == NULL && config.fake_outputs != NULL)
        fake_outputs = config.fake_outputs;

    if (fake_outputs != NULL) {
        fake_outputs_init(fake_outputs);
        FREE(fake_outputs);
        config.fake_outputs = NULL;
    } else if (force_xinerama) {
        /* Force Xinerama (for drivers which don't support RandR yet, esp. the
         * nVidia binary graphics driver), when specified either in the config
         * file or on command-line */
        xinerama_init();
    } else {
        DLOG("Checking for XRandR...\n");
        randr_init(&randr_base, disable_randr15 || config.disable_randr15);
    }

    /* We need to force disabling outputs which have been loaded from the
     * layout file but are no longer active. This can happen if the output has
     * been disabled in the short time between writing the restart layout file
     * and restarting i3. See #2326. */
    if (layout_path != NULL && randr_base > -1) {
        Con *con;
        TAILQ_FOREACH (con, &(croot->nodes_head), nodes) {
            Output *output;
            TAILQ_FOREACH (output, &outputs, outputs) {
                if (output->active || strcmp(con->name, output_primary_name(output)) != 0)
                    continue;

                /* This will correctly correlate the output with its content
                 * container. We need to make the connection to properly
                 * disable the output. */
                if (output->con == NULL) {
                    output_init_con(output);
                    output->changed = false;
                }

                output->to_be_disabled = true;
                randr_disable_output(output);
            }
        }
    }
    FREE(layout_path);

    scratchpad_fix_resolution();

    xcb_query_pointer_reply_t *pointerreply;
    Output *output = NULL;
    if (!(pointerreply = xcb_query_pointer_reply(conn, pointercookie, NULL))) {
        ELOG("Could not query pointer position, using first screen\n");
    } else {
        DLOG("Pointer at %d, %d\n", pointerreply->root_x, pointerreply->root_y);
        output = get_output_containing(pointerreply->root_x, pointerreply->root_y);
        if (!output) {
            ELOG("ERROR: No screen at (%d, %d), starting on the first screen\n",
                 pointerreply->root_x, pointerreply->root_y);
        }
    }
    if (!output) {
        output = get_first_output();
    }
    con_activate(con_descend_focused(output_get_content(output->con)));
    free(pointerreply);

    tree_render();

    /* Create the UNIX domain socket for IPC */
    int ipc_socket = create_socket(config.ipc_socket_path, &current_socketpath);
    if (ipc_socket == -1) {
        ELOG("Could not create the IPC socket, IPC disabled\n");
    } else {
        struct ev_io *ipc_io = scalloc(1, sizeof(struct ev_io));
        ev_io_init(ipc_io, ipc_new_client, ipc_socket, EV_READ);
        ev_io_start(main_loop, ipc_io);
    }

    /* Chose a file name in /tmp/ based on the PID */
    char *log_stream_socket_path = get_process_filename("log-stream-socket");
    int log_socket = create_socket(log_stream_socket_path, &current_log_stream_socket_path);
    free(log_stream_socket_path);
    if (log_socket == -1) {
        ELOG("Could not create the log socket, i3-dump-log -f will not work\n");
    } else {
        struct ev_io *log_io = scalloc(1, sizeof(struct ev_io));
        ev_io_init(log_io, log_new_client, log_socket, EV_READ);
        ev_io_start(main_loop, log_io);
    }

    /* Also handle the UNIX domain sockets passed via socket activation. The
     * parameter 1 means "remove the environment variables", we don’t want to
     * pass these to child processes. */
    listen_fds = sd_listen_fds(0);
    if (listen_fds < 0)
        ELOG("socket activation: Error in sd_listen_fds\n");
    else if (listen_fds == 0)
        DLOG("socket activation: no sockets passed\n");
    else {
        int flags;
        for (int fd = SD_LISTEN_FDS_START;
             fd < (SD_LISTEN_FDS_START + listen_fds);
             fd++) {
            DLOG("socket activation: also listening on fd %d\n", fd);

            /* sd_listen_fds() enables FD_CLOEXEC by default.
             * However, we need to keep the file descriptors open for in-place
             * restarting, therefore we explicitly disable FD_CLOEXEC. */
            if ((flags = fcntl(fd, F_GETFD)) < 0 ||
                fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
                ELOG("Could not disable FD_CLOEXEC on fd %d\n", fd);
            }

            struct ev_io *ipc_io = scalloc(1, sizeof(struct ev_io));
            ev_io_init(ipc_io, ipc_new_client, fd, EV_READ);
            ev_io_start(main_loop, ipc_io);
        }
    }

    {
        const int restart_fd = parse_restart_fd();
        if (restart_fd != -1) {
            DLOG("serving restart fd %d", restart_fd);
            ipc_client *client = ipc_new_client_on_fd(main_loop, restart_fd);
            ipc_confirm_restart(client);
            unsetenv("_I3_RESTART_FD");
        }
    }

    /* Set up i3 specific atoms like I3_SOCKET_PATH and I3_CONFIG_PATH */
    x_set_i3_atoms();
    ewmh_update_workarea();

    /* Set the ewmh desktop properties. */
    ewmh_update_desktop_properties();

    struct ev_io *xcb_watcher = scalloc(1, sizeof(struct ev_io));
    xcb_prepare = scalloc(1, sizeof(struct ev_prepare));

    ev_io_init(xcb_watcher, xcb_got_event, xcb_get_file_descriptor(conn), EV_READ);
    ev_io_start(main_loop, xcb_watcher);

    ev_prepare_init(xcb_prepare, xcb_prepare_cb);
    ev_prepare_start(main_loop, xcb_prepare);

    xcb_flush(conn);

    /* What follows is a fugly consequence of X11 protocol race conditions like
     * the following: In an i3 in-place restart, i3 will reparent all windows
     * to the root window, then exec() itself. In the new process, it calls
     * manage_existing_windows. However, in case any application sent a
     * generated UnmapNotify message to the WM (as GIMP does), this message
     * will be handled by i3 *after* managing the window, thus i3 thinks the
     * window just closed itself. In reality, the message was sent in the time
     * period where i3 wasn’t running yet.
     *
     * To prevent this, we grab the server (disables processing of any other
     * connections), then discard all pending events (since we didn’t do
     * anything, there cannot be any meaningful responses), then ungrab the
     * server. */
    xcb_grab_server(conn);
    {
        xcb_aux_sync(conn);
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn)) != NULL) {
            if (event->response_type == 0) {
                free(event);
                continue;
            }

            /* Strip off the highest bit (set if the event is generated) */
            int type = (event->response_type & 0x7F);

            /* We still need to handle MapRequests which are sent in the
             * timespan starting from when we register as a window manager and
             * this piece of code which drops events. */
            if (type == XCB_MAP_REQUEST)
                handle_event(type, event);

            free(event);
        }
        manage_existing_windows(root);
    }
    xcb_ungrab_server(conn);

    if (autostart) {
        /* When the root's window background is set to NONE, that might mean
         * that old content stays visible when a window is closed. That has
         * unpleasant effect of "my terminal (does not seem to) close!".
         *
         * There does not seem to be an easy way to query for this problem, so
         * we test for it: Open & close a window and check if the background is
         * redrawn or the window contents stay visible.
         */
        LOG("This is not an in-place restart, checking if a wallpaper is set.\n");

        xcb_screen_t *root = xcb_aux_get_screen(conn, conn_screen);
        if (is_background_set(conn, root)) {
            LOG("A wallpaper is set, so no screenshot is necessary.\n");
        } else {
            LOG("No wallpaper set, copying root window contents to a pixmap\n");
            set_screenshot_as_wallpaper(conn, root);
        }
    }

#if defined(__OpenBSD__)
    if (pledge("stdio rpath wpath cpath proc exec unix", NULL) == -1)
        err(EXIT_FAILURE, "pledge");
#endif

    if (!disable_signalhandler)
        setup_signal_handler();
    else {
        struct sigaction action;

        action.sa_sigaction = handle_core_signal;
        action.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
        sigemptyset(&action.sa_mask);

        /* Catch all signals with default action "Core", see signal(7) */
        if (sigaction(SIGQUIT, &action, NULL) == -1 ||
            sigaction(SIGILL, &action, NULL) == -1 ||
            sigaction(SIGABRT, &action, NULL) == -1 ||
            sigaction(SIGFPE, &action, NULL) == -1 ||
            sigaction(SIGSEGV, &action, NULL) == -1)
            ELOG("Could not setup signal handler.\n");
    }

    setup_term_handlers();
    /* Ignore SIGPIPE to survive errors when an IPC client disconnects
     * while we are sending them a message */
    signal(SIGPIPE, SIG_IGN);

    /* Autostarting exec-lines */
    if (autostart) {
        while (!TAILQ_EMPTY(&autostarts)) {
            struct Autostart *exec = TAILQ_FIRST(&autostarts);

            LOG("auto-starting %s\n", exec->command);
            start_application(exec->command, exec->no_startup_id);

            FREE(exec->command);
            TAILQ_REMOVE(&autostarts, exec, autostarts);
            FREE(exec);
        }
    }

    /* Autostarting exec_always-lines */
    while (!TAILQ_EMPTY(&autostarts_always)) {
        struct Autostart *exec_always = TAILQ_FIRST(&autostarts_always);

        LOG("auto-starting (always!) %s\n", exec_always->command);
        start_application(exec_always->command, exec_always->no_startup_id);

        FREE(exec_always->command);
        TAILQ_REMOVE(&autostarts_always, exec_always, autostarts_always);
        FREE(exec_always);
    }

    /* Start i3bar processes for all configured bars */
    Barconfig *barconfig;
    TAILQ_FOREACH (barconfig, &barconfigs, configs) {
        char *command = NULL;
        sasprintf(&command, "%s %s --bar_id=%s --socket=\"%s\"",
                  barconfig->i3bar_command ? barconfig->i3bar_command : "exec i3bar",
                  barconfig->verbose ? "-V" : "",
                  barconfig->id, current_socketpath);
        LOG("Starting bar process: %s\n", command);
        start_application(command, true);
        free(command);
    }

    /* Make sure to destroy the event loop to invoke the cleanup callbacks
     * when calling exit() */
    atexit(i3_exit);

    ev_loop(main_loop, 0);
}
