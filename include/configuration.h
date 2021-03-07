/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * include/configuration.h: Contains all structs/variables for the configurable
 * part of i3 as well as functions handling the configuration file (calling
 * the parser (src/config_parse.c) with the correct path, switching key
 * bindings mode).
 *
 */
#pragma once

#include "queue.h"
#include "i3.h"

typedef struct Config Config;
typedef struct Barconfig Barconfig;
extern char *current_configpath;
extern char *current_config;
extern Config config;
extern SLIST_HEAD(modes_head, Mode) modes;
extern TAILQ_HEAD(barconfig_head, Barconfig) barconfigs;

/**
 * Used during the config file lexing/parsing to keep the state of the lexer
 * in order to provide useful error messages in yyerror().
 *
 */
struct context {
    bool has_errors;
    bool has_warnings;

    int line_number;
    char *line_copy;
    const char *filename;

    char *compact_error;

    /* These are the same as in YYLTYPE */
    int first_column;
    int last_column;
};

/**
 * Part of the struct Config. It makes sense to group colors for background,
 * border and text as every element in i3 has them (window decorations, bar).
 *
 */
struct Colortriple {
    color_t border;
    color_t background;
    color_t text;
    color_t indicator;
    color_t child_border;
};

/**
 * Holds a user-assigned variable for parsing the configuration file. The key
 * is replaced by value in every following line of the file.
 *
 */
struct Variable {
    char *key;
    char *value;
    char *next_match;

    SLIST_ENTRY(Variable) variables;
};

/**
 * The configuration file can contain multiple sets of bindings. Apart from the
 * default set (name == "default"), you can specify other sets and change the
 * currently active set of bindings by using the "mode <name>" command.
 *
 */
struct Mode {
    char *name;
    bool pango_markup;
    struct bindings_head *bindings;

    SLIST_ENTRY(Mode) modes;
};

/**
 * Holds part of the configuration (the part which is not already in dedicated
 * structures in include/data.h).
 *
 */
struct Config {
    const char *terminal;
    i3Font font;

    char *ipc_socket_path;
    char *restart_state_path;

    layout_t default_layout;
    int container_stack_limit;
    int container_stack_limit_value;
    int default_border_width;
    int default_floating_border_width;

    /** Default orientation for new containers */
    int default_orientation;

    /** By default, focus follows mouse. If the user explicitly wants to
     * turn this off (and instead rely only on the keyboard for changing
     * focus), we allow them to do this with this relatively special option.
     * It is not planned to add any different focus models. */
    bool disable_focus_follows_mouse;

    /** By default, when switching focus to a window on a different output
     * (e.g. focusing a window on workspace 3 on output VGA-1, coming from
     * workspace 2 on LVDS-1), the mouse cursor is warped to the center of
     * that window.
     *
     * With the mouse_warping option, you can control when the mouse cursor
     * should be warped. "none" disables warping entirely, whereas "output"
     * is the default behavior described above. */
    warping_t mouse_warping;

    /** Remove borders if they are adjacent to the screen edge.
     * This is useful if you are reaching scrollbar on the edge of the
     * screen or do not want to waste a single pixel of displayspace.
     * By default, this is disabled. */
    hide_edge_borders_mode_t hide_edge_borders;

    /** By default, a workspace bar is drawn at the bottom of the screen.
     * If you want to have a more fancy bar, it is recommended to replace
     * the whole bar by dzen2, for example using the i3-wsbar script which
     * comes with i3. Thus, you can turn it off entirely. */
    bool disable_workspace_bar;

    /** When focus wrapping is enabled (the default), attempting to
     * move focus past the edge of the screen (in other words, in a
     * direction in which there are no more containers to focus) will
     * cause the focus to wrap to the opposite edge of the current
     * container. When it is disabled, nothing happens; the current
     * focus is preserved.
     *
     * Additionally, focus wrapping may be forced. Think of the
     * following layout: Horizontal workspace with a tabbed con on the
     * left of the screen and a terminal on the right of the
     * screen. You are in the second container in the tabbed container
     * and focus to the right. By default, i3 will set focus to the
     * terminal on the right. If you are in the first container in the
     * tabbed container however, focusing to the left will
     * wrap. Setting focus_wrapping to FOCUS_WRAPPING_FORCE forces i3
     * to always wrap, which will result in you having to use "focus
     * parent" more often. */
    focus_wrapping_t focus_wrapping;

    /** By default, use the RandR API for multi-monitor setups.
     * Unfortunately, the nVidia binary graphics driver doesn't support
     * this API. Instead, it only support the less powerful Xinerama API,
     * which can be enabled by this option.
     *
     * Note: this option takes only effect on the initial startup (eg.
     * reconfiguration is not possible). On startup, the list of screens
     * is fetched once and never updated. */
    bool force_xinerama;

    /** Don’t use RandR 1.5 for querying outputs. */
    bool disable_randr15;

    /** Overwrites output detection (for testing), see src/fake_outputs.c */
    char *fake_outputs;

    /** Automatic workspace back and forth switching. If this is set, a
     * switch to the currently active workspace will switch to the
     * previously focused one instead, making it possible to fast toggle
     * between two workspaces. */
    bool workspace_auto_back_and_forth;

    /** By default, urgency is cleared immediately when switching to another
     * workspace leads to focusing the con with the urgency hint. When having
     * multiple windows on that workspace, the user needs to guess which
     * application raised the event. To prevent this, the reset of the urgency
     * flag can be delayed using an urgency timer. */
    float workspace_urgency_timer;

    /** Behavior when a window sends a NET_ACTIVE_WINDOW message. */
    enum {
        /* Focus if the target workspace is visible, set urgency hint otherwise. */
        FOWA_SMART,
        /* Always set the urgency hint. */
        FOWA_URGENT,
        /* Always focus the window. */
        FOWA_FOCUS,
        /* Ignore the request (no focus, no urgency hint). */
        FOWA_NONE
    } focus_on_window_activation;

    /** Specifies whether or not marks should be displayed in the window
     * decoration. Marks starting with a "_" will be ignored either way. */
    bool show_marks;

    /** Title alignment options. */
    enum {
        ALIGN_LEFT,
        ALIGN_CENTER,
        ALIGN_RIGHT
    } title_align;

    /** The default border style for new windows. */
    border_style_t default_border;

    /** The default border style for new floating windows. */
    border_style_t default_floating_border;

    /** The modifier which needs to be pressed in combination with your mouse
     * buttons to do things with floating windows (move, resize) */
    uint32_t floating_modifier;

    /** Maximum and minimum dimensions of a floating window */
    int32_t floating_maximum_width;
    int32_t floating_maximum_height;
    int32_t floating_minimum_width;
    int32_t floating_minimum_height;

    /* Color codes are stored here */
    struct config_client {
        color_t background;
        struct Colortriple focused;
        struct Colortriple focused_inactive;
        struct Colortriple unfocused;
        struct Colortriple urgent;
        struct Colortriple placeholder;
    } client;
    struct config_bar {
        struct Colortriple focused;
        struct Colortriple unfocused;
        struct Colortriple urgent;
    } bar;

    /** What should happen when a new popup is opened during fullscreen mode */
    enum {
        /* display (and focus) the popup when it belongs to the fullscreen
         * window only. */
        PDF_SMART = 0,

        /* leave fullscreen mode unconditionally */
        PDF_LEAVE_FULLSCREEN = 1,

        /* just ignore the popup, that is, don’t map it */
        PDF_IGNORE = 2,
    } popup_during_fullscreen;

    /* The number of currently parsed barconfigs */
    int number_barconfigs;
};

/**
 * Holds the status bar configuration (i3bar). One of these structures is
 * created for each 'bar' block in the config.
 *
 */
struct Barconfig {
    /** Automatically generated ID for this bar config. Used by the bar process
     * to request a specific configuration. */
    char *id;

    /** Number of outputs in the outputs array */
    int num_outputs;
    /** Outputs on which this bar should show up on. We use an array for
     * simplicity (since we store just strings). */
    char **outputs;

    /* List of outputs on which the tray is allowed to be shown, in order.
     * The special value "none" disables it (per default, it will be shown) and
     * the special value "primary" enabled it on the primary output. */
    TAILQ_HEAD(tray_outputs_head, tray_output_t) tray_outputs;

    /* Padding around the tray icons. */
    int tray_padding;

    /* Bar height. */
    int height;

    /** Path to the i3 IPC socket. This option is discouraged since programs
     * can find out the path by looking for the I3_SOCKET_PATH property on the
     * root window! */
    char *socket_path;

    /** Bar display mode (hide unless modifier is pressed or show in dock mode or always hide in invisible mode) */
    enum { M_DOCK = 0,
           M_HIDE = 1,
           M_INVISIBLE = 2 } mode;

    /* The current hidden_state of the bar, which indicates whether it is hidden or shown */
    enum { S_HIDE = 0,
           S_SHOW = 1 } hidden_state;

    /** Bar modifier (to show bar when in hide mode). */
    uint32_t modifier;

    TAILQ_HEAD(bar_bindings_head, Barbinding) bar_bindings;

    /** Bar position (bottom by default). */
    enum { P_BOTTOM = 0,
           P_TOP = 1 } position;

    /** Command that should be run to execute i3bar, give a full path if i3bar is not
     * in your $PATH.
     * By default just 'i3bar' is executed. */
    char *i3bar_command;

    /** Command that should be run to get a statusline, for example 'i3status'.
     * Will be passed to the shell. */
    char *status_command;

    /** Font specification for all text rendered on the bar. */
    char *font;

    /** A custom separator to use instead of a vertical line. */
    char *separator_symbol;

    /** Hide workspace buttons? Configuration option is 'workspace_buttons no'
     * but we invert the bool to get the correct default when initializing with
     * zero. */
    bool hide_workspace_buttons;

    /** The minimal width for workspace buttons. */
    int workspace_min_width;

    /** Strip workspace numbers? Configuration option is
     * 'strip_workspace_numbers yes'. */
    bool strip_workspace_numbers;

    /** Strip workspace name? Configuration option is
     * 'strip_workspace_name yes'. */
    bool strip_workspace_name;

    /** Hide mode button? Configuration option is 'binding_mode_indicator no'
     * but we invert the bool for the same reason as hide_workspace_buttons.*/
    bool hide_binding_mode_indicator;

    /** Enable verbose mode? Useful for debugging purposes. */
    bool verbose;

    struct bar_colors {
        char *background;
        char *statusline;
        char *separator;

        char *focused_background;
        char *focused_statusline;
        char *focused_separator;

        char *focused_workspace_border;
        char *focused_workspace_bg;
        char *focused_workspace_text;

        char *active_workspace_border;
        char *active_workspace_bg;
        char *active_workspace_text;

        char *inactive_workspace_border;
        char *inactive_workspace_bg;
        char *inactive_workspace_text;

        char *urgent_workspace_border;
        char *urgent_workspace_bg;
        char *urgent_workspace_text;

        char *binding_mode_border;
        char *binding_mode_bg;
        char *binding_mode_text;
    } colors;

    TAILQ_ENTRY(Barconfig) configs;
};

/**
 * Defines a mouse command to be executed instead of the default behavior when
 * clicking on the non-statusline part of i3bar.
 *
 */
struct Barbinding {
    /** The button to be used (e.g., 1 for "button1"). */
    int input_code;

    /** The command which is to be executed for this button. */
    char *command;

    /** If true, the command will be executed after the button is released. */
    bool release;

    TAILQ_ENTRY(Barbinding) bindings;
};

struct tray_output_t {
    char *output;

    TAILQ_ENTRY(tray_output_t) tray_outputs;
};

typedef enum {
    C_VALIDATE,
    C_LOAD,
    C_RELOAD,
} config_load_t;

/**
 * (Re-)loads the configuration file (sets useful defaults before).
 *
 * If you specify override_configpath, only this path is used to look for a
 * configuration file.
 *
 * load_type specifies the type of loading: C_VALIDATE is used to only verify
 * the correctness of the config file (used with the flag -C). C_LOAD will load
 * the config for normal use and display errors in the nagbar. C_RELOAD will
 * also clear the previous config.
 */
bool load_configuration(const char *override_configfile, config_load_t load_type);

/**
 * Ungrabs all keys, to be called before re-grabbing the keys because of a
 * mapping_notify event or a configuration file reload
 *
 */
void ungrab_all_keys(xcb_connection_t *conn);
