/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 */
#include "libi3.h"

#include <assert.h>
#include <cairo/cairo-xcb.h>
#include <err.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const i3Font *savedFont = NULL;

static xcb_visualtype_t *root_visual_type;
static double pango_font_red;
static double pango_font_green;
static double pango_font_blue;

static PangoLayout *create_layout_with_dpi(cairo_t *cr) {
    PangoLayout *layout;
    PangoContext *context;

    context = pango_cairo_create_context(cr);
    pango_cairo_context_set_resolution(context, get_dpi_value());
    layout = pango_layout_new(context);
    g_object_unref(context);

    return layout;
}

/*
 * Loads a Pango font description into an i3Font structure. Returns true
 * on success, false otherwise.
 *
 */
static bool load_pango_font(i3Font *font, const char *desc) {
    /* Load the font description */
    font->specific.pango_desc = pango_font_description_from_string(desc);
    if (!font->specific.pango_desc) {
        ELOG("Could not open font %s with Pango, fallback to X font.\n", desc);
        return false;
    }

    int font_size = pango_font_description_get_size(font->specific.pango_desc) / PANGO_SCALE;

    LOG("Using Pango font %s, size %d\n",
        pango_font_description_get_family(font->specific.pango_desc),
        font_size);

    /* We cache root_visual_type here, since you must call
     * load_pango_font before any other pango function
     * that would need root_visual_type */
    root_visual_type = get_visualtype(root_screen);

    /* Create a dummy Pango layout to compute the font height */
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, root_screen->root, root_visual_type, 1, 1);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = create_layout_with_dpi(cr);
    pango_layout_set_font_description(layout, font->specific.pango_desc);

    /* Get the font height */
    gint height;
    pango_layout_get_pixel_size(layout, NULL, &height);
    font->height = height;

    /* Free resources */
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    /* Set the font type and return successfully */
    font->type = FONT_TYPE_PANGO;
    font->size = font_size;
    return true;
}

/*
 * Draws text using Pango rendering.
 *
 */
static void draw_text_pango(const char *text, size_t text_len,
                            xcb_drawable_t drawable, xcb_visualtype_t *visual, int x, int y,
                            int max_width, bool pango_markup) {
    /* Create the Pango layout */
    /* root_visual_type is cached in load_pango_font */
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, drawable,
                                                        visual, x + max_width, y + savedFont->height);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = create_layout_with_dpi(cr);
    gint height;

    pango_layout_set_font_description(layout, savedFont->specific.pango_desc);
    pango_layout_set_width(layout, max_width * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    if (pango_markup)
        pango_layout_set_markup(layout, text, text_len);
    else
        pango_layout_set_text(layout, text, text_len);

    /* Do the drawing */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, pango_font_red, pango_font_green, pango_font_blue);
    pango_cairo_update_layout(cr, layout);
    pango_layout_get_pixel_size(layout, NULL, &height);
    /* Center the piece of text vertically. */
    int yoffset = (height - savedFont->height) / 2;
    cairo_move_to(cr, x, y - yoffset);
    pango_cairo_show_layout(cr, layout);

    /* Free resources */
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

/*
 * Calculate the text width using Pango rendering.
 *
 */
static int predict_text_width_pango(const char *text, size_t text_len, bool pango_markup) {
    /* Create a dummy Pango layout */
    /* root_visual_type is cached in load_pango_font */
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, root_screen->root, root_visual_type, 1, 1);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = create_layout_with_dpi(cr);

    /* Get the font width */
    gint width;
    pango_layout_set_font_description(layout, savedFont->specific.pango_desc);

    if (pango_markup)
        pango_layout_set_markup(layout, text, text_len);
    else
        pango_layout_set_text(layout, text, text_len);

    pango_cairo_update_layout(cr, layout);
    pango_layout_get_pixel_size(layout, &width, NULL);

    /* Free resources */
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return width;
}

/*
 * Loads a font for usage, also getting its metrics. If fallback is true,
 * the fonts 'fixed' or '-misc-*' will be loaded instead of exiting. If any
 * font was previously loaded, it will be freed.
 *
 */
i3Font load_font(const char *pattern, const bool fallback) {
    /* if any font was previously loaded, free it now */
    free_font();

    i3Font font;
    font.type = FONT_TYPE_NONE;
    font.pattern = NULL;

    /* No XCB connction, return early because we're just validating the
     * configuration file. */
    if (conn == NULL) {
        return font;
    }

    /* Try to load a pango font if specified */
    if (strlen(pattern) > strlen("pango:") && !strncmp(pattern, "pango:", strlen("pango:"))) {
        const char *font_pattern = pattern + strlen("pango:");
        if (load_pango_font(&font, font_pattern)) {
            font.pattern = sstrdup(pattern);
            return font;
        }
    } else if (strlen(pattern) > strlen("xft:") && !strncmp(pattern, "xft:", strlen("xft:"))) {
        const char *font_pattern = pattern + strlen("xft:");
        if (load_pango_font(&font, font_pattern)) {
            font.pattern = sstrdup(pattern);
            return font;
        }
    }

    /* Send all our requests first */
    font.specific.xcb.id = xcb_generate_id(conn);
    xcb_void_cookie_t font_cookie = xcb_open_font_checked(conn, font.specific.xcb.id,
                                                          strlen(pattern), pattern);
    xcb_query_font_cookie_t info_cookie = xcb_query_font(conn, font.specific.xcb.id);

    /* Check for errors. If errors, fall back to default font. */
    xcb_generic_error_t *error;
    error = xcb_request_check(conn, font_cookie);

    /* If we fail to open font, fall back to 'fixed' */
    if (fallback && error != NULL) {
        ELOG("Could not open font %s (X error %d). Trying fallback to 'fixed'.\n",
             pattern, error->error_code);
        pattern = "fixed";
        font_cookie = xcb_open_font_checked(conn, font.specific.xcb.id,
                                            strlen(pattern), pattern);
        info_cookie = xcb_query_font(conn, font.specific.xcb.id);

        /* Check if we managed to open 'fixed' */
        free(error);
        error = xcb_request_check(conn, font_cookie);

        /* Fall back to '-misc-*' if opening 'fixed' fails. */
        if (error != NULL) {
            ELOG("Could not open fallback font 'fixed', trying with '-misc-*'.\n");
            pattern = "-misc-*";
            font_cookie = xcb_open_font_checked(conn, font.specific.xcb.id,
                                                strlen(pattern), pattern);
            info_cookie = xcb_query_font(conn, font.specific.xcb.id);

            free(error);
            if ((error = xcb_request_check(conn, font_cookie)) != NULL)
                errx(EXIT_FAILURE, "Could open neither requested font nor fallbacks "
                                   "(fixed or -misc-*): X11 error %d",
                     error->error_code);
        }
    }
    free(error);

    font.pattern = sstrdup(pattern);
    LOG("Using X font %s\n", pattern);

    /* Get information (height/name) for this font */
    if (!(font.specific.xcb.info = xcb_query_font_reply(conn, info_cookie, NULL)))
        errx(EXIT_FAILURE, "Could not load font \"%s\"", pattern);

    /* Get the font table, if possible */
    if (xcb_query_font_char_infos_length(font.specific.xcb.info) == 0)
        font.specific.xcb.table = NULL;
    else
        font.specific.xcb.table = xcb_query_font_char_infos(font.specific.xcb.info);

    /* Calculate the font height */
    font.height = font.specific.xcb.info->font_ascent + font.specific.xcb.info->font_descent;

    /* Set the font type and return successfully */
    font.type = FONT_TYPE_XCB;
    return font;
}

/*
 * Defines the font to be used for the forthcoming calls.
 *
 */
void set_font(i3Font *font) {
    savedFont = font;
}

/*
 * Frees the resources taken by the current font. If no font was previously
 * loaded, it simply returns.
 *
 */
void free_font(void) {
    /* if there is no saved font, simply return */
    if (savedFont == NULL)
        return;

    free(savedFont->pattern);
    switch (savedFont->type) {
        case FONT_TYPE_NONE:
            /* Nothing to do */
            break;
        case FONT_TYPE_XCB: {
            /* Close the font and free the info */
            xcb_close_font(conn, savedFont->specific.xcb.id);
            free(savedFont->specific.xcb.info);
            break;
        }
        case FONT_TYPE_PANGO:
            /* Free the font description */
            pango_font_description_free(savedFont->specific.pango_desc);
            break;
    }

    savedFont = NULL;
}

/*
 * Defines the colors to be used for the forthcoming draw_text calls.
 *
 */
void set_font_colors(xcb_gcontext_t gc, color_t foreground, color_t background) {
    assert(savedFont != NULL);

    switch (savedFont->type) {
        case FONT_TYPE_NONE:
            /* Nothing to do */
            break;
        case FONT_TYPE_XCB: {
            /* Change the font and colors in the GC */
            uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
            uint32_t values[] = {foreground.colorpixel, background.colorpixel, savedFont->specific.xcb.id};
            xcb_change_gc(conn, gc, mask, values);
            break;
        }
        case FONT_TYPE_PANGO:
            /* Save the foreground font */
            pango_font_red = foreground.red;
            pango_font_green = foreground.green;
            pango_font_blue = foreground.blue;
            break;
    }
}

/*
 * Returns true if and only if the current font is a pango font.
 *
 */
bool font_is_pango(void) {
    return savedFont->type == FONT_TYPE_PANGO;
}

static int predict_text_width_xcb(const xcb_char2b_t *text, size_t text_len);

static void draw_text_xcb(const xcb_char2b_t *text, size_t text_len, xcb_drawable_t drawable,
                          xcb_gcontext_t gc, int x, int y) {
    /* X11 coordinates for fonts start at the baseline */
    int pos_y = y + savedFont->specific.xcb.info->font_ascent;

    /* The X11 protocol limits text drawing to 255 chars, so we may need
     * multiple calls */
    int offset = 0;
    for (;;) {
        /* Calculate the size of this chunk */
        int chunk_size = (text_len > 255 ? 255 : text_len);
        const xcb_char2b_t *chunk = text + offset;

        /* Draw it */
        xcb_image_text_16(conn, chunk_size, drawable, gc, x, pos_y, chunk);

        /* Advance the offset and length of the text to draw */
        offset += chunk_size;
        text_len -= chunk_size;

        /* Check if we're done */
        if (text_len == 0)
            break;

        /* Advance pos_x based on the predicted text width */
        x += predict_text_width_xcb(chunk, chunk_size);
    }
}

/*
 * Draws text onto the specified X drawable (normally a pixmap) at the
 * specified coordinates (from the top left corner of the leftmost, uppermost
 * glyph) and using the provided gc.
 *
 * Text must be specified as an i3String.
 *
 */
void draw_text(i3String *text, xcb_drawable_t drawable, xcb_gcontext_t gc,
               xcb_visualtype_t *visual, int x, int y, int max_width) {
    assert(savedFont != NULL);
    if (visual == NULL) {
        visual = root_visual_type;
    }

    switch (savedFont->type) {
        case FONT_TYPE_NONE:
            /* Nothing to do */
            return;
        case FONT_TYPE_XCB:
            draw_text_xcb(i3string_as_ucs2(text), i3string_get_num_glyphs(text),
                          drawable, gc, x, y);
            break;
        case FONT_TYPE_PANGO:
            /* Render the text using Pango */
            draw_text_pango(i3string_as_utf8(text), i3string_get_num_bytes(text),
                            drawable, visual, x, y, max_width, i3string_is_markup(text));
            return;
    }
}

/*
 * ASCII version of draw_text to print static strings.
 *
 */
void draw_text_ascii(const char *text, xcb_drawable_t drawable,
                     xcb_gcontext_t gc, int x, int y, int max_width) {
    assert(savedFont != NULL);

    switch (savedFont->type) {
        case FONT_TYPE_NONE:
            /* Nothing to do */
            return;
        case FONT_TYPE_XCB: {
            size_t text_len = strlen(text);
            if (text_len > 255) {
                /* The text is too long to draw it directly to X */
                i3String *str = i3string_from_utf8(text);
                draw_text(str, drawable, gc, NULL, x, y, max_width);
                i3string_free(str);
            } else {
                /* X11 coordinates for fonts start at the baseline */
                int pos_y = y + savedFont->specific.xcb.info->font_ascent;

                xcb_image_text_8(conn, text_len, drawable, gc, x, pos_y, text);
            }
            break;
        }
        case FONT_TYPE_PANGO:
            /* Render the text using Pango */
            draw_text_pango(text, strlen(text),
                            drawable, root_visual_type, x, y, max_width, false);
            return;
    }
}

static int xcb_query_text_width(const xcb_char2b_t *text, size_t text_len) {
    /* Make the user know we’re using the slow path, but only once. */
    static bool first_invocation = true;
    if (first_invocation) {
        fprintf(stderr, "Using slow code path for text extents\n");
        first_invocation = false;
    }

    /* Query the text width */
    xcb_generic_error_t *error;
    xcb_query_text_extents_cookie_t cookie = xcb_query_text_extents(conn,
                                                                    savedFont->specific.xcb.id, text_len, (xcb_char2b_t *)text);
    xcb_query_text_extents_reply_t *reply = xcb_query_text_extents_reply(conn,
                                                                         cookie, &error);
    if (reply == NULL) {
        /* We return a safe estimate because a rendering error is better than
         * a crash. Plus, the user will see the error in their log. */
        fprintf(stderr, "Could not get text extents (X error code %d)\n",
                error->error_code);
        free(error);
        return savedFont->specific.xcb.info->max_bounds.character_width * text_len;
    }

    int width = reply->overall_width;
    free(reply);
    return width;
}

static int predict_text_width_xcb(const xcb_char2b_t *input, size_t text_len) {
    if (text_len == 0)
        return 0;

    int width;
    if (savedFont->specific.xcb.table == NULL) {
        /* If we don't have a font table, fall back to querying the server */
        width = xcb_query_text_width(input, text_len);
    } else {
        /* Save some pointers for convenience */
        xcb_query_font_reply_t *font_info = savedFont->specific.xcb.info;
        xcb_charinfo_t *font_table = savedFont->specific.xcb.table;

        /* Calculate the width using the font table */
        width = 0;
        for (size_t i = 0; i < text_len; i++) {
            xcb_charinfo_t *info;
            int row = input[i].byte1;
            int col = input[i].byte2;

            if (row < font_info->min_byte1 ||
                row > font_info->max_byte1 ||
                col < font_info->min_char_or_byte2 ||
                col > font_info->max_char_or_byte2)
                continue;

            /* Don't you ask me, how this one works… (Merovius) */
            info = &font_table[((row - font_info->min_byte1) *
                                (font_info->max_char_or_byte2 - font_info->min_char_or_byte2 + 1)) +
                               (col - font_info->min_char_or_byte2)];

            if (info->character_width != 0 ||
                (info->right_side_bearing |
                 info->left_side_bearing |
                 info->ascent |
                 info->descent) != 0) {
                width += info->character_width;
            }
        }
    }

    return width;
}

/*
 * Predict the text width in pixels for the given text. Text must be
 * specified as an i3String.
 *
 */
int predict_text_width(i3String *text) {
    assert(savedFont != NULL);

    switch (savedFont->type) {
        case FONT_TYPE_NONE:
            /* Nothing to do */
            return 0;
        case FONT_TYPE_XCB:
            return predict_text_width_xcb(i3string_as_ucs2(text), i3string_get_num_glyphs(text));
        case FONT_TYPE_PANGO:
            /* Calculate extents using Pango */
            return predict_text_width_pango(i3string_as_utf8(text), i3string_get_num_bytes(text),
                                            i3string_is_markup(text));
    }
    assert(false);
}
