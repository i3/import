/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * workspaces.c: Maintaining the workspace lists
 *
 */
#pragma once

#include "common.h"

#include <xcb/xproto.h>

typedef struct i3_ws i3_ws;

TAILQ_HEAD(ws_head, i3_ws);

/*
 * Start parsing the received JSON string
 *
 */
void parse_workspaces_json(char *json);

/*
 * free() all workspace data structures
 *
 */
void free_workspaces(void);

struct i3_ws {
    uintptr_t id;             /* Workspace ID - C pointer to a workspace container */
    int num;                  /* The internal number of the ws */
    char *canonical_name;     /* The true name of the ws according to the ipc */
    i3String *name;           /* The name of the ws that is displayed on the bar */
    int name_width;           /* The rendered width of the name */
    int button_width;         /* The rendered width of the button */
    bool visible;             /* If the ws is currently visible on an output */
    bool focused;             /* If the ws is currently focused */
    bool urgent;              /* If the urgent hint of the ws is set */
    rect rect;                /* The rect of the ws (not used (yet)) */
    struct i3_output *output; /* The current output of the ws */

    TAILQ_ENTRY(i3_ws) tailq; /* Pointer for the TAILQ-Macro */
};
