/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * include/data.h: This file defines all data structures used by i3
 *
 */
#pragma once

#define SN_API_NOT_YET_FROZEN 1
#include <libsn/sn-launcher.h>

#include <xcb/randr.h>
#include <pcre.h>
#include <sys/time.h>
#include <cairo/cairo.h>

#include "queue.h"

/*
 * To get the big concept: There are helper structures like struct
 * Workspace_Assignment. Every struct which is also defined as type (see
 * forward definitions) is considered to be a major structure, thus important.
 *
 * The following things are all stored in a 'Con', from very high level (the
 * biggest Cons) to very small (a single window):
 *
 * 1) X11 root window (as big as all your outputs combined)
 * 2) output (like LVDS1)
 * 3) content container, dockarea containers
 * 4) workspaces
 * 5) split containers
 * ... (you can arbitrarily nest split containers)
 * 6) X11 window containers
 *
 */

/* Forward definitions */
typedef struct Binding Binding;
typedef struct Rect Rect;
typedef struct xoutput Output;
typedef struct Con Con;
typedef struct Match Match;
typedef struct Assignment Assignment;
typedef struct Window i3Window;
typedef struct mark_t mark_t;

/******************************************************************************
 * Helper types
 *****************************************************************************/
typedef enum { D_LEFT,
               D_RIGHT,
               D_UP,
               D_DOWN } direction_t;
typedef enum { NO_ORIENTATION = 0,
               HORIZ,
               VERT } orientation_t;
typedef enum { BEFORE,
               AFTER } position_t;
typedef enum { BS_NORMAL = 0,
               BS_NONE = 1,
               BS_PIXEL = 2 } border_style_t;

/** parameter to specify whether tree_close_internal() and x_window_kill() should kill
 * only this specific window or the whole X11 client */
typedef enum { DONT_KILL_WINDOW = 0,
               KILL_WINDOW = 1,
               KILL_CLIENT = 2 } kill_window_t;

/** describes if the window is adjacent to the output (physical screen) edges. */
typedef enum { ADJ_NONE = 0,
               ADJ_LEFT_SCREEN_EDGE = (1 << 0),
               ADJ_RIGHT_SCREEN_EDGE = (1 << 1),
               ADJ_UPPER_SCREEN_EDGE = (1 << 2),
               ADJ_LOWER_SCREEN_EDGE = (1 << 4) } adjacent_t;

typedef enum { HEBM_NONE = ADJ_NONE,
               HEBM_VERTICAL = ADJ_LEFT_SCREEN_EDGE | ADJ_RIGHT_SCREEN_EDGE,
               HEBM_HORIZONTAL = ADJ_UPPER_SCREEN_EDGE | ADJ_LOWER_SCREEN_EDGE,
               HEBM_BOTH = HEBM_VERTICAL | HEBM_HORIZONTAL,
               HEBM_SMART = (1 << 5) } hide_edge_borders_mode_t;

typedef enum { MM_REPLACE,
               MM_ADD } mark_mode_t;

/**
 * Container layouts. See Con::layout.
 */
typedef enum {
    L_DEFAULT = 0,
    L_STACKED = 1,
    L_TABBED = 2,
    L_DOCKAREA = 3,
    L_OUTPUT = 4,
    L_SPLITV = 5,
    L_SPLITH = 6
} layout_t;

/**
 * Binding input types. See Binding::input_type.
 */
typedef enum {
    B_KEYBOARD = 0,
    B_MOUSE = 1
} input_type_t;

/**
 * Bitmask for matching XCB_XKB_GROUP_1 to XCB_XKB_GROUP_4.
 */
typedef enum {
    I3_XKB_GROUP_MASK_ANY = 0,
    I3_XKB_GROUP_MASK_1 = (1 << 0),
    I3_XKB_GROUP_MASK_2 = (1 << 1),
    I3_XKB_GROUP_MASK_3 = (1 << 2),
    I3_XKB_GROUP_MASK_4 = (1 << 3)
} i3_xkb_group_mask_t;

/**
 * The lower 16 bits contain a xcb_key_but_mask_t, the higher 16 bits contain
 * an i3_xkb_group_mask_t. This type is necessary for the fallback logic to
 * work when handling XKB groups (see ticket #1775) and makes the code which
 * locates keybindings upon KeyPress/KeyRelease events simpler.
 */
typedef uint32_t i3_event_state_mask_t;

/**
 * Mouse pointer warping modes.
 */
typedef enum {
    POINTER_WARPING_OUTPUT = 0,
    POINTER_WARPING_NONE = 1
} warping_t;

/**
 * Focus wrapping modes.
 */
typedef enum {
    FOCUS_WRAPPING_OFF = 0,
    FOCUS_WRAPPING_ON = 1,
    FOCUS_WRAPPING_FORCE = 2,
    FOCUS_WRAPPING_WORKSPACE = 3
} focus_wrapping_t;

/**
 * Stores a rectangle, for example the size of a window, the child window etc.
 *
 * Note that x and y can contain signed values in some cases (for example when
 * used for the coordinates of a window, which can be set outside of the
 * visible area, but not when specifying the position of a workspace for the
 * _NET_WM_WORKAREA hint). Not declaring x/y as int32_t saves us a lot of
 * typecasts.
 *
 */
struct Rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/**
 * Stores the reserved pixels on each screen edge read from a
 * _NET_WM_STRUT_PARTIAL.
 *
 */
struct reservedpx {
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;
};

/**
 * Stores a width/height pair, used as part of deco_render_params to check
 * whether the rects width/height have changed.
 *
 */
struct width_height {
    uint32_t w;
    uint32_t h;
};

/**
 * Stores the parameters for rendering a window decoration. This structure is
 * cached in every Con and no re-rendering will be done if the parameters have
 * not changed (only the pixmaps will be copied).
 *
 */
struct deco_render_params {
    struct Colortriple *color;
    int border_style;
    struct width_height con_rect;
    struct width_height con_window_rect;
    Rect con_deco_rect;
    color_t background;
    layout_t parent_layout;
    bool con_is_leaf;
};

/**
 * Stores which workspace (by name or number) goes to which output.
 *
 */
struct Workspace_Assignment {
    char *name;
    char *output;

    TAILQ_ENTRY(Workspace_Assignment) ws_assignments;
};

struct Ignore_Event {
    int sequence;
    int response_type;
    time_t added;

    SLIST_ENTRY(Ignore_Event) ignore_events;
};

/**
 * Stores internal information about a startup sequence, like the workspace it
 * was initiated on.
 *
 */
struct Startup_Sequence {
    /** startup ID for this sequence, generated by libstartup-notification */
    char *id;
    /** workspace on which this startup was initiated */
    char *workspace;
    /** libstartup-notification context for this launch */
    SnLauncherContext *context;
    /** time at which this sequence should be deleted (after it was marked as
     * completed) */
    time_t delete_at;

    TAILQ_ENTRY(Startup_Sequence) sequences;
};

/**
 * Regular expression wrapper. It contains the pattern itself as a string (like
 * ^foo[0-9]$) as well as a pointer to the compiled PCRE expression and the
 * pcre_extra data returned by pcre_study().
 *
 * This makes it easier to have a useful logfile, including the matching or
 * non-matching pattern.
 *
 */
struct regex {
    char *pattern;
    pcre *regex;
    pcre_extra *extra;
};

/**
 * Stores a resolved keycode (from a keysym), including the modifier mask. Will
 * be passed to xcb_grab_key().
 *
 */
struct Binding_Keycode {
    xcb_keycode_t keycode;
    i3_event_state_mask_t modifiers;
    TAILQ_ENTRY(Binding_Keycode) keycodes;
};

/******************************************************************************
 * Major types
 *****************************************************************************/

/**
 * Holds a keybinding, consisting of a keycode combined with modifiers and the
 * command which is executed as soon as the key is pressed (see
 * src/config_parser.c)
 *
 */
struct Binding {
    /* The type of input this binding is for. (Mouse bindings are not yet
     * implemented. All bindings are currently assumed to be keyboard bindings.) */
    input_type_t input_type;

    /** If true, the binding should be executed upon a KeyRelease event, not a
     * KeyPress (the default). */
    enum {
        /* This binding will only be executed upon KeyPress events */
        B_UPON_KEYPRESS = 0,
        /* This binding will be executed either upon a KeyRelease event, or… */
        B_UPON_KEYRELEASE = 1,
        /* …upon a KeyRelease event, even if the modifiers don’t match. This
         * state is triggered from get_binding() when the corresponding
         * KeyPress (!) happens, so that users can release the modifier keys
         * before releasing the actual key. */
        B_UPON_KEYRELEASE_IGNORE_MODS = 2,
    } release;

    /** If this is true for a mouse binding, the binding should be executed
     * when the button is pressed over the window border. */
    bool border;

    /** If this is true for a mouse binding, the binding should be executed
     * when the button is pressed over any part of the window, not just the
     * title bar (default). */
    bool whole_window;

    /** If this is true for a mouse binding, the binding should only be
     * executed if the button press was not on the titlebar. */
    bool exclude_titlebar;

    /** Keycode to bind */
    uint32_t keycode;

    /** Bitmask which is applied against event->state for KeyPress and
     * KeyRelease events to determine whether this binding applies to the
     * current state. */
    i3_event_state_mask_t event_state_mask;

    /** Symbol the user specified in configfile, if any. This needs to be
     * stored with the binding to be able to re-convert it into a keycode
     * if the keyboard mapping changes (using Xmodmap for example) */
    char *symbol;

    /** Only in use if symbol != NULL. Contains keycodes which generate the
     * specified symbol. Useful for unbinding and checking which binding was
     * used when a key press event comes in. */
    TAILQ_HEAD(keycodes_head, Binding_Keycode) keycodes_head;

    /** Command, like in command mode */
    char *command;

    TAILQ_ENTRY(Binding) bindings;
};

/**
 * Holds a command specified by either an:
 * - exec-line
 * - exec_always-line
 * in the config (see src/config.c)
 *
 */
struct Autostart {
    /** Command, like in command mode */
    char *command;
    /** no_startup_id flag for start_application(). Determines whether a
     * startup notification context/ID should be created. */
    bool no_startup_id;
    TAILQ_ENTRY(Autostart) autostarts;
    TAILQ_ENTRY(Autostart) autostarts_always;
};

struct output_name {
    char *name;
    SLIST_ENTRY(output_name) names;
};

/**
 * An Output is a physical output on your graphics driver. Outputs which
 * are currently in use have (output->active == true). Each output has a
 * position and a mode. An output usually corresponds to one connected
 * screen (except if you are running multiple screens in clone mode).
 *
 */
struct xoutput {
    /** Output id, so that we can requery the output directly later */
    xcb_randr_output_t id;

    /** Whether the output is currently active (has a CRTC attached with a
     * valid mode) */
    bool active;

    /** Internal flags, necessary for querying RandR screens (happens in
     * two stages) */
    bool changed;
    bool to_be_disabled;
    bool primary;

    /** List of names for the output.
     * An output always has at least one name; the first name is
     * considered the primary one. */
    SLIST_HEAD(names_head, output_name) names_head;

    /** Pointer to the Con which represents this output */
    Con *con;

    /** x, y, width, height */
    Rect rect;

    TAILQ_ENTRY(xoutput) outputs;
};

/**
 * A 'Window' is a type which contains an xcb_window_t and all the related
 * information (hints like _NET_WM_NAME for that window).
 *
 */
struct Window {
    xcb_window_t id;

    /** Holds the xcb_window_t (just an ID) for the leader window (logical
     * parent for toolwindows and similar floating windows) */
    xcb_window_t leader;
    xcb_window_t transient_for;

    /** Pointers to the Assignments which were already ran for this Window
     * (assignments run only once) */
    uint32_t nr_assignments;
    Assignment **ran_assignments;

    char *class_class;
    char *class_instance;

    /** The name of the window. */
    i3String *name;

    /** The WM_WINDOW_ROLE of this window (for example, the pidgin buddy window
     * sets "buddy list"). Useful to match specific windows in assignments or
     * for_window. */
    char *role;

    /** WM_CLIENT_MACHINE of the window */
    char *machine;

    /** Flag to force re-rendering the decoration upon changes */
    bool name_x_changed;

    /** Whether the application used _NET_WM_NAME */
    bool uses_net_wm_name;

    /** Whether the application needs to receive WM_TAKE_FOCUS */
    bool needs_take_focus;

    /** Whether this window accepts focus. We store this inverted so that the
     * default will be 'accepts focus'. */
    bool doesnt_accept_focus;

    /** The _NET_WM_WINDOW_TYPE for this window. */
    xcb_atom_t window_type;

    /** The _NET_WM_DESKTOP for this window. */
    uint32_t wm_desktop;

    /** Whether the window says it is a dock window */
    enum { W_NODOCK = 0,
           W_DOCK_TOP = 1,
           W_DOCK_BOTTOM = 2 } dock;

    /** When this window was marked urgent. 0 means not urgent */
    struct timeval urgent;

    /** Pixels the window reserves. left/right/top/bottom */
    struct reservedpx reserved;

    /** Depth of the window */
    uint16_t depth;

    /* the wanted size of the window, used in combination with size
     * increments (see below). */
    int base_width;
    int base_height;

    /* minimum increment size specified for the window (in pixels) */
    int width_increment;
    int height_increment;

    /* Minimum size specified for the window. */
    int min_width;
    int min_height;

    /* Maximum size specified for the window. */
    int max_width;
    int max_height;

    /* aspect ratio from WM_NORMAL_HINTS (MPlayer uses this for example) */
    double min_aspect_ratio;
    double max_aspect_ratio;

    /** Window icon, as Cairo surface */
    cairo_surface_t *icon;

    /** The window has a nonrectangular shape. */
    bool shaped;
    /** The window has a nonrectangular input shape. */
    bool input_shaped;

    /* Time when the window became managed. Used to determine whether a window
     * should be swallowed after initial management. */
    time_t managed_since;

    /* The window has been swallowed. */
    bool swallowed;
};

/**
 * A "match" is a data structure which acts like a mask or expression to match
 * certain windows or not. For example, when using commands, you can specify a
 * command like this: [title="*Firefox*"] kill. The title member of the match
 * data structure will then be filled and i3 will check each window using
 * match_matches_window() to find the windows affected by this command.
 *
 */
struct Match {
    /* Set if a criterion was specified incorrectly. */
    char *error;

    struct regex *title;
    struct regex *application;
    struct regex *class;
    struct regex *instance;
    struct regex *mark;
    struct regex *window_role;
    struct regex *workspace;
    struct regex *machine;
    xcb_atom_t window_type;
    enum {
        U_DONTCHECK = -1,
        U_LATEST = 0,
        U_OLDEST = 1
    } urgent;
    enum {
        M_DONTCHECK = -1,
        M_NODOCK = 0,
        M_DOCK_ANY = 1,
        M_DOCK_TOP = 2,
        M_DOCK_BOTTOM = 3
    } dock;
    xcb_window_t id;
    enum { WM_ANY = 0,
           WM_TILING_AUTO,
           WM_TILING_USER,
           WM_TILING,
           WM_FLOATING_AUTO,
           WM_FLOATING_USER,
           WM_FLOATING } window_mode;
    Con *con_id;

    /* Where the window looking for a match should be inserted:
     *
     * M_HERE   = the matched container will be replaced by the window
     *            (layout saving)
     * M_ASSIGN_WS = the matched container will be inserted in the target_ws.
     * M_BELOW  = the window will be inserted as a child of the matched container
     *            (dockareas)
     *
     */
    enum { M_HERE = 0,
           M_ASSIGN_WS,
           M_BELOW } insert_where;

    TAILQ_ENTRY(Match) matches;

    /* Whether this match was generated when restarting i3 inplace.
     * Leads to not setting focus when managing a new window, because the old
     * focus stack should be restored. */
    bool restart_mode;
};

/**
 * An Assignment makes specific windows go to a specific workspace/output or
 * run a command for that window. With this mechanism, the user can -- for
 * example -- assign their browser to workspace "www". Checking if a window is
 * assigned works by comparing the Match data structure with the window (see
 * match_matches_window()).
 *
 */
struct Assignment {
    /** type of this assignment:
     *
     * A_COMMAND = run the specified command for the matching window
     * A_TO_WORKSPACE = assign the matching window to the specified workspace
     * A_NO_FOCUS = don't focus matched window when it is managed
     *
     * While the type is a bitmask, only one value can be set at a time. It is
     * a bitmask to allow filtering for multiple types, for example in the
     * assignment_for() function.
     *
     */
    enum {
        A_ANY = 0,
        A_COMMAND = (1 << 0),
        A_TO_WORKSPACE = (1 << 1),
        A_NO_FOCUS = (1 << 2),
        A_TO_WORKSPACE_NUMBER = (1 << 3),
        A_TO_OUTPUT = (1 << 4)
    } type;

    /** the criteria to check if a window matches */
    Match match;

    /** destination workspace/command/output, depending on the type */
    union {
        char *command;
        char *workspace;
        char *output;
    } dest;

    TAILQ_ENTRY(Assignment) assignments;
};

/** Fullscreen modes. Used by Con.fullscreen_mode. */
typedef enum { CF_NONE = 0,
               CF_OUTPUT = 1,
               CF_GLOBAL = 2 } fullscreen_mode_t;

struct mark_t {
    char *name;

    TAILQ_ENTRY(mark_t) marks;
};

/**
 * Window icon position.
 */
typedef enum {
    ICON_POSITION_LEFT,
    ICON_POSITION_RIGHT,
    ICON_POSITION_TITLE
} icon_position;

/**
 * A 'Con' represents everything from the X11 root window down to a single X11 window.
 *
 */
struct Con {
    bool mapped;

    /* Should this container be marked urgent? This gets set when the window
     * inside this container (if any) sets the urgency hint, for example. */
    bool urgent;

    /** This counter contains the number of UnmapNotify events for this
     * container (or, more precisely, for its ->frame) which should be ignored.
     * UnmapNotify events need to be ignored when they are caused by i3 itself,
     * for example when reparenting or when unmapping the window on a workspace
     * change. */
    uint8_t ignore_unmap;

    /* The surface used for the frame window. */
    surface_t frame;
    surface_t frame_buffer;
    bool pixmap_recreated;

    enum {
        CT_ROOT = 0,
        CT_OUTPUT = 1,
        CT_CON = 2,
        CT_FLOATING_CON = 3,
        CT_WORKSPACE = 4,
        CT_DOCKAREA = 5
    } type;

    /** the workspace number, if this Con is of type CT_WORKSPACE and the
     * workspace is not a named workspace (for named workspaces, num == -1) */
    int num;

    struct Con *parent;

    /* The position and size for this con. These coordinates are absolute. Note
     * that the rect of a container does not include the decoration. */
    struct Rect rect;
    /* The position and size of the actual client window. These coordinates are
     * relative to the container's rect. */
    struct Rect window_rect;
    /* The position and size of the container's decoration. These coordinates
     * are relative to the container's parent's rect. */
    struct Rect deco_rect;
    /** the geometry this window requested when getting mapped */
    struct Rect geometry;

    char *name;

    /** The format with which the window's name should be displayed. */
    char *title_format;

    /** Whether the window icon should be displayed, and with what padding. -1
      * means display no window icon (default behavior), 0 means display without
      * any padding, 1 means display with 1 pixel of padding and so on. */
    int window_icon_padding;

    /** The position of the window icon, if enabled.
      * ICON_POSITION_LEFT means position the icon at the left of the title bar.
      * ICON_POSITION_RIGHT means position the icon at the right of the title bar.
      * ICON_POSITION_TITLE means position the icon to the left of the title (exact
      * position depends on title alignment). */
    icon_position window_icon_position;

    /* a sticky-group is an identifier which bundles several containers to a
     * group. The contents are shared between all of them, that is they are
     * displayed on whichever of the containers is currently visible */
    char *sticky_group;

    /* user-definable marks to jump to this container later */
    TAILQ_HEAD(marks_head, mark_t) marks_head;
    /* cached to decide whether a redraw is needed */
    bool mark_changed;

    double percent;

    /* the x11 border pixel attribute */
    int border_width;
    int current_border_width;

    struct Window *window;

    /* timer used for disabling urgency */
    struct ev_timer *urgency_timer;

    /** Cache for the decoration rendering */
    struct deco_render_params *deco_render_params;

    /* Only workspace-containers can have floating clients */
    TAILQ_HEAD(floating_head, Con) floating_head;

    TAILQ_HEAD(nodes_head, Con) nodes_head;
    TAILQ_HEAD(focus_head, Con) focus_head;

    TAILQ_HEAD(swallow_head, Match) swallow_head;

    fullscreen_mode_t fullscreen_mode;

    /* Whether this window should stick to the glass. This corresponds to
     * the _NET_WM_STATE_STICKY atom and will only be respected if the
     * window is floating. */
    bool sticky;

    /* layout is the layout of this container: one of split[v|h], stacked or
     * tabbed. Special containers in the tree (above workspaces) have special
     * layouts like dockarea or output.
     *
     * last_split_layout is one of splitv or splith to support the old "layout
     * default" command which by now should be "layout splitv" or "layout
     * splith" explicitly.
     *
     * workspace_layout is only for type == CT_WORKSPACE cons. When you change
     * the layout of a workspace without any children, i3 cannot just set the
     * layout (because workspaces need to be splitv/splith to allow focus
     * parent and opening new containers). Instead, it stores the requested
     * layout in workspace_layout and creates a new split container with that
     * layout whenever a new container is attached to the workspace. */
    layout_t layout, last_split_layout, workspace_layout;
    border_style_t border_style;
    /** floating? (= not in tiling layout) This cannot be simply a bool
     * because we want to keep track of whether the status was set by the
     * application (by setting _NET_WM_WINDOW_TYPE appropriately) or by the
     * user. The user’s choice overwrites automatic mode, of course. The
     * order of the values is important because we check with >=
     * FLOATING_AUTO_ON if a client is floating. */
    enum {
        FLOATING_AUTO_OFF = 0,
        FLOATING_USER_OFF = 1,
        FLOATING_AUTO_ON = 2,
        FLOATING_USER_ON = 3
    } floating;

    TAILQ_ENTRY(Con) nodes;
    TAILQ_ENTRY(Con) focused;
    TAILQ_ENTRY(Con) all_cons;
    TAILQ_ENTRY(Con) floating_windows;

    /** callbacks */
    void (*on_remove_child)(Con *);

    enum {
        /* Not a scratchpad window. */
        SCRATCHPAD_NONE = 0,

        /* Just moved to scratchpad, not resized by the user yet.
         * Window will be auto-centered and sized appropriately. */
        SCRATCHPAD_FRESH = 1,

        /* The user changed position/size of the scratchpad window. */
        SCRATCHPAD_CHANGED = 2
    } scratchpad_state;

    /* The ID of this container before restarting. Necessary to correctly
     * interpret back-references in the JSON (such as the focus stack). */
    int old_id;

    /* Depth of the container window */
    uint16_t depth;

    /* The colormap for this con if a custom one is used. */
    xcb_colormap_t colormap;
};
