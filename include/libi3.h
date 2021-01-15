/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * libi3: contains functions which are used by i3 *and* accompanying tools such
 * as i3-msg, i3-config-wizard, …
 *
 */
#pragma once

#include <config.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_keysyms.h>

#include <pango/pango.h>
#include <cairo/cairo-xcb.h>

#define DEFAULT_DIR_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

/** Mouse buttons */
#define XCB_BUTTON_CLICK_LEFT XCB_BUTTON_INDEX_1
#define XCB_BUTTON_CLICK_MIDDLE XCB_BUTTON_INDEX_2
#define XCB_BUTTON_CLICK_RIGHT XCB_BUTTON_INDEX_3
#define XCB_BUTTON_SCROLL_UP XCB_BUTTON_INDEX_4
#define XCB_BUTTON_SCROLL_DOWN XCB_BUTTON_INDEX_5
/* xcb doesn't define constants for these. */
#define XCB_BUTTON_SCROLL_LEFT 6
#define XCB_BUTTON_SCROLL_RIGHT 7

/**
 * XCB connection and root screen
 *
 */
extern xcb_connection_t *conn;
extern xcb_screen_t *root_screen;

/**
 * Opaque data structure for storing strings.
 *
 */
typedef struct _i3String i3String;

typedef struct Font i3Font;

/**
 * Data structure for cached font information:
 * - font id in X11 (load it once)
 * - font height (multiple calls needed to get it)
 *
 */
struct Font {
    /** The type of font */
    enum {
        FONT_TYPE_NONE = 0,
        FONT_TYPE_XCB,
        FONT_TYPE_PANGO
    } type;

    /** The height of the font, built from font_ascent + font_descent */
    int height;

    /** Font size as described in config */
    int size;

    /** The pattern/name used to load the font. */
    char *pattern;

    union {
        struct {
            /** The xcb-id for the font */
            xcb_font_t id;

            /** Font information gathered from the server */
            xcb_query_font_reply_t *info;

            /** Font table for this font (may be NULL) */
            xcb_charinfo_t *table;
        } xcb;

        /** The pango font description */
        PangoFontDescription *pango_desc;
    } specific;
};

/* Since this file also gets included by utilities which don’t use the i3 log
 * infrastructure, we define a fallback. */
#if !defined(LOG)
void verboselog(char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
#define LOG(fmt, ...) verboselog("[libi3] " __FILE__ " " fmt, ##__VA_ARGS__)
#endif
#if !defined(ELOG)
void errorlog(char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
#define ELOG(fmt, ...) errorlog("[libi3] ERROR: " fmt, ##__VA_ARGS__)
#endif
#if !defined(DLOG)
void debuglog(char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
#define DLOG(fmt, ...) debuglog("%s:%s:%d - " fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif

/**
 * Try to get the contents of the given atom (for example I3_SOCKET_PATH) from
 * the X11 root window and return NULL if it doesn’t work.
 *
 * If the provided XCB connection is NULL, a new connection will be
 * established.
 *
 * The memory for the contents is dynamically allocated and has to be
 * free()d by the caller.
 *
 */
char *root_atom_contents(const char *atomname, xcb_connection_t *provided_conn, int screen);

/**
 * Safe-wrapper around malloc which exits if malloc returns NULL (meaning that
 * there is no more memory available)
 *
 */
void *smalloc(size_t size);

/**
 * Safe-wrapper around calloc which exits if malloc returns NULL (meaning that
 * there is no more memory available)
 *
 */
void *scalloc(size_t num, size_t size);

/**
 * Safe-wrapper around realloc which exits if realloc returns NULL (meaning
 * that there is no more memory available).
 *
 */
void *srealloc(void *ptr, size_t size);

/**
 * Safe-wrapper around strdup which exits if malloc returns NULL (meaning that
 * there is no more memory available)
 *
 */
char *sstrdup(const char *str);

/**
 * Safe-wrapper around strndup which exits if strndup returns NULL (meaning that
 * there is no more memory available)
 *
 */
char *sstrndup(const char *str, size_t size);

/**
 * Safe-wrapper around asprintf which exits if it returns -1 (meaning that
 * there is no more memory available)
 *
 */
int sasprintf(char **strp, const char *fmt, ...);

/**
 * Wrapper around correct write which returns -1 (meaning that
 * write failed) or count (meaning that all bytes were written)
 *
 */
ssize_t writeall(int fd, const void *buf, size_t count);

/**
 * Like writeall, but instead of retrying upon EAGAIN (returned when a write
 * would block), the function stops and returns the total number of bytes
 * written so far.
 *
 */
ssize_t writeall_nonblock(int fd, const void *buf, size_t count);

/**
 * Safe-wrapper around writeall which exits if it returns -1 (meaning that
 * write failed)
 *
 */
ssize_t swrite(int fd, const void *buf, size_t count);

/**
 * Like strcasecmp but considers the case where either string is NULL.
 *
 */
int strcasecmp_nullable(const char *a, const char *b);

/**
 * Build an i3String from an UTF-8 encoded string.
 * Returns the newly-allocated i3String.
 *
 */
i3String *i3string_from_utf8(const char *from_utf8);

/**
 * Build an i3String from an UTF-8 encoded string in Pango markup.
 *
 */
i3String *i3string_from_markup(const char *from_markup);

/**
 * Build an i3String from an UTF-8 encoded string with fixed length.
 * To be used when no proper NULL-termination is available.
 * Returns the newly-allocated i3String.
 *
 */
i3String *i3string_from_utf8_with_length(const char *from_utf8, ssize_t num_bytes);

/**
 * Build an i3String from an UTF-8 encoded string in Pango markup with fixed
 * length.
 *
 */
i3String *i3string_from_markup_with_length(const char *from_markup, size_t num_bytes);

/**
 * Build an i3String from an UCS-2 encoded string.
 * Returns the newly-allocated i3String.
 *
 */
i3String *i3string_from_ucs2(const xcb_char2b_t *from_ucs2, size_t num_glyphs);

/**
 * Copies the given i3string.
 * Note that this will not free the source string.
 */
i3String *i3string_copy(i3String *str);

/**
 * Free an i3String.
 *
 */
void i3string_free(i3String *str);

/**
 * Securely i3string_free by setting the pointer to NULL
 * to prevent accidentally using freed memory.
 *
 */
#define I3STRING_FREE(str)      \
    do {                        \
        if (str != NULL) {      \
            i3string_free(str); \
            str = NULL;         \
        }                       \
    } while (0)

/**
 * Returns the UTF-8 encoded version of the i3String.
 *
 */
const char *i3string_as_utf8(i3String *str);

/**
 * Returns the UCS-2 encoded version of the i3String.
 *
 */
const xcb_char2b_t *i3string_as_ucs2(i3String *str);

/**
 * Returns the number of bytes (UTF-8 encoded) in an i3String.
 *
 */
size_t i3string_get_num_bytes(i3String *str);

/**
 * Whether the given i3String is in Pango markup.
 */
bool i3string_is_markup(i3String *str);

/**
 * Set whether the i3String should use Pango markup.
 */
void i3string_set_markup(i3String *str, bool pango_markup);

/**
 * Escape pango markup characters in the given string.
 */
i3String *i3string_escape_markup(i3String *str);

/**
 * Returns the number of glyphs in an i3String.
 *
 */
size_t i3string_get_num_glyphs(i3String *str);

/**
 * Connects to the i3 IPC socket and returns the file descriptor for the
 * socket. die()s if anything goes wrong.
 *
 */
int ipc_connect(const char *socket_path);

/**
 * Formats a message (payload) of the given size and type and sends it to i3 via
 * the given socket file descriptor.
 *
 * Returns -1 when write() fails, errno will remain.
 * Returns 0 on success.
 *
 */
int ipc_send_message(int sockfd, const uint32_t message_size,
                     const uint32_t message_type, const uint8_t *payload);

/**
 * Reads a message from the given socket file descriptor and stores its length
 * (reply_length) as well as a pointer to its contents (reply).
 *
 * Returns -1 when read() fails, errno will remain.
 * Returns -2 when the IPC protocol is violated (invalid magic, unexpected
 * message type, EOF instead of a message). Additionally, the error will be
 * printed to stderr.
 * Returns 0 on success.
 *
 */
int ipc_recv_message(int sockfd, uint32_t *message_type,
                     uint32_t *reply_length, uint8_t **reply);

/**
 * Generates a configure_notify event and sends it to the given window
 * Applications need this to think they’ve configured themselves correctly.
 * The truth is, however, that we will manage them.
 *
 */
void fake_configure_notify(xcb_connection_t *conn, xcb_rectangle_t r, xcb_window_t window, int border_width);

#define HAS_G_UTF8_MAKE_VALID GLIB_CHECK_VERSION(2, 52, 0)
#if !HAS_G_UTF8_MAKE_VALID
gchar *g_utf8_make_valid(const gchar *str, gssize len);
#endif

/**
 * Returns the colorpixel to use for the given hex color (think of HTML). Only
 * works for true-color (vast majority of cases) at the moment, avoiding a
 * roundtrip to X11.
 *
 * The hex_color has to start with #, for example #FF00FF.
 *
 * NOTE that get_colorpixel() does _NOT_ check the given color code for validity.
 * This has to be done by the caller.
 *
 * NOTE that this function may in the future rely on a global xcb_connection_t
 * variable called 'conn' to be present.
 *
 */
uint32_t get_colorpixel(const char *hex) __attribute__((const));

#ifndef HAVE_STRNDUP
/**
 * Taken from FreeBSD
 * Returns a pointer to a new string which is a duplicate of the
 * string, but only copies at most n characters.
 *
 */
char *strndup(const char *str, size_t n);
#endif

/**
 * All-in-one function which returns the modifier mask (XCB_MOD_MASK_*) for the
 * given keysymbol, for example for XCB_NUM_LOCK (usually configured to mod2).
 *
 * This function initiates one round-trip. Use get_mod_mask_for() directly if
 * you already have the modifier mapping and key symbols.
 *
 */
uint32_t aio_get_mod_mask_for(uint32_t keysym, xcb_key_symbols_t *symbols);

/**
 * Returns the modifier mask (XCB_MOD_MASK_*) for the given keysymbol, for
 * example for XCB_NUM_LOCK (usually configured to mod2).
 *
 * This function does not initiate any round-trips.
 *
 */
uint32_t get_mod_mask_for(uint32_t keysym,
                          xcb_key_symbols_t *symbols,
                          xcb_get_modifier_mapping_reply_t *modmap_reply);

/**
 * Loads a font for usage, also getting its height. If fallback is true,
 * the fonts 'fixed' or '-misc-*' will be loaded instead of exiting. If any
 * font was previously loaded, it will be freed.
 *
 */
i3Font load_font(const char *pattern, const bool fallback);

/**
 * Defines the font to be used for the forthcoming calls.
 *
 */
void set_font(i3Font *font);

/**
 * Frees the resources taken by the current font. If no font was previously
 * loaded, it simply returns.
 *
 */
void free_font(void);

/**
 * Converts the given string to UTF-8 from UCS-2 big endian. The return value
 * must be freed after use.
 *
 */
char *convert_ucs2_to_utf8(xcb_char2b_t *text, size_t num_glyphs);

/**
 * Converts the given string to UCS-2 big endian for use with
 * xcb_image_text_16(). The amount of real glyphs is stored in real_strlen,
 * a buffer containing the UCS-2 encoded string (16 bit per glyph) is
 * returned. It has to be freed when done.
 *
 */
xcb_char2b_t *convert_utf8_to_ucs2(char *input, size_t *real_strlen);

/* Represents a color split by color channel. */
typedef struct color_t {
    double red;
    double green;
    double blue;
    double alpha;

    /* The colorpixel we use for direct XCB calls. */
    uint32_t colorpixel;
} color_t;

#define COLOR_TRANSPARENT ((color_t){.red = 0.0, .green = 0.0, .blue = 0.0, .colorpixel = 0})

/**
 * Defines the colors to be used for the forthcoming draw_text calls.
 *
 */
void set_font_colors(xcb_gcontext_t gc, color_t foreground, color_t background);

/**
 * Returns true if and only if the current font is a pango font.
 *
 */
bool font_is_pango(void);

/**
 * Draws text onto the specified X drawable (normally a pixmap) at the
 * specified coordinates (from the top left corner of the leftmost, uppermost
 * glyph) and using the provided gc.
 *
 * Text must be specified as an i3String.
 *
 */
void draw_text(i3String *text, xcb_drawable_t drawable, xcb_gcontext_t gc,
               xcb_visualtype_t *visual, int x, int y, int max_width);

/**
 * ASCII version of draw_text to print static strings.
 *
 */
void draw_text_ascii(const char *text, xcb_drawable_t drawable,
                     xcb_gcontext_t gc, int x, int y, int max_width);

/**
 * Predict the text width in pixels for the given text. Text must be
 * specified as an i3String.
 *
 */
int predict_text_width(i3String *text);

/**
 * Returns the visual type associated with the given screen.
 *
 */
xcb_visualtype_t *get_visualtype(xcb_screen_t *screen);

/**
 * Returns true if this version of i3 is a debug build (anything which is not a
 * release version), based on the git version number.
 *
 */
bool is_debug_build(void) __attribute__((const));

/**
 * Returns the name of a temporary file with the specified prefix.
 *
 */
char *get_process_filename(const char *prefix);

/**
 * This function returns the absolute path to the executable it is running in.
 *
 * The implementation follows https://stackoverflow.com/a/933996/712014
 *
 * Returned value must be freed by the caller.
 */
char *get_exe_path(const char *argv0);

/**
 * Initialize the DPI setting.
 * This will use the 'Xft.dpi' X resource if available and fall back to
 * guessing the correct value otherwise.
 */
void init_dpi(void);

/**
 * This function returns the value of the DPI setting.
 *
 */
long get_dpi_value(void);

/**
 * Convert a logical amount of pixels (e.g. 2 pixels on a “standard” 96 DPI
 * screen) to a corresponding amount of physical pixels on a standard or retina
 * screen, e.g. 5 pixels on a 227 DPI MacBook Pro 13" Retina screen.
 *
 */
int logical_px(const int logical);

/**
 * This function resolves ~ in pathnames.
 * It may resolve wildcards in the first part of the path, but if no match
 * or multiple matches are found, it just returns a copy of path as given.
 *
 */
char *resolve_tilde(const char *path);

/**
 * Get the path of the first configuration file found. If override_configpath is
 * specified, that path is returned and saved for further calls. Otherwise,
 * checks the home directory first, then the system directory, always taking
 * into account the XDG Base Directory Specification ($XDG_CONFIG_HOME,
 * $XDG_CONFIG_DIRS).
 *
 */
char *get_config_path(const char *override_configpath, bool use_system_paths);

#ifndef HAVE_MKDIRP
/**
 * Emulates mkdir -p (creates any missing folders)
 *
 */
int mkdirp(const char *path, mode_t mode);
#endif

/** Helper structure for usage in format_placeholders(). */
typedef struct placeholder_t {
    /* The placeholder to be replaced, e.g., "%title". */
    const char *name;
    /* The value this placeholder should be replaced with. */
    const char *value;
} placeholder_t;

/**
 * Replaces occurrences of the defined placeholders in the format string.
 *
 */
char *format_placeholders(char *format, placeholder_t *placeholders, int num);

/* We need to flush cairo surfaces twice to avoid an assertion bug. See #1989
 * and https://bugs.freedesktop.org/show_bug.cgi?id=92455. */
#define CAIRO_SURFACE_FLUSH(surface)  \
    do {                              \
        cairo_surface_flush(surface); \
        cairo_surface_flush(surface); \
    } while (0)

/* A wrapper grouping an XCB drawable and both a graphics context
 * and the corresponding cairo objects representing it. */
typedef struct surface_t {
    /* The drawable which is being represented. */
    xcb_drawable_t id;

    /* A classic XCB graphics context. */
    xcb_gcontext_t gc;

    xcb_visualtype_t *visual_type;

    int width;
    int height;

    /* A cairo surface representing the drawable. */
    cairo_surface_t *surface;

    /* The cairo object representing the drawable. In general,
     * this is what one should use for any drawing operation. */
    cairo_t *cr;
} surface_t;

/**
 * Initialize the surface to represent the given drawable.
 *
 */
void draw_util_surface_init(xcb_connection_t *conn, surface_t *surface, xcb_drawable_t drawable,
                            xcb_visualtype_t *visual, int width, int height);

/**
 * Resize the surface to the given size.
 *
 */
void draw_util_surface_set_size(surface_t *surface, int width, int height);

/**
 * Destroys the surface.
 *
 */
void draw_util_surface_free(xcb_connection_t *conn, surface_t *surface);

/**
 * Parses the given color in hex format to an internal color representation.
 * Note that the input must begin with a hash sign, e.g., "#3fbc59".
 *
 */
color_t draw_util_hex_to_color(const char *color);

/**
 * Draw the given text using libi3.
 * This function also marks the surface dirty which is needed if other means of
 * drawing are used. This will be the case when using XCB to draw text.
 *
 */
void draw_util_text(i3String *text, surface_t *surface, color_t fg_color, color_t bg_color, int x, int y, int max_width);

/**
 * Draws a filled rectangle.
 * This function is a convenience wrapper and takes care of flushing the
 * surface as well as restoring the cairo state.
 *
 */
void draw_util_rectangle(surface_t *surface, color_t color, double x, double y, double w, double h);

/**
 * Clears a surface with the given color.
 *
 */
void draw_util_clear_surface(surface_t *surface, color_t color);

/**
 * Copies a surface onto another surface.
 *
 */
void draw_util_copy_surface(surface_t *src, surface_t *dest, double src_x, double src_y,
                            double dest_x, double dest_y, double width, double height);
