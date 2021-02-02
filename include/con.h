/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * con.c: Functions which deal with containers directly (creating containers,
 *        searching containers, getting specific properties from containers,
 *        …).
 *
 */
#pragma once

#include <config.h>

/**
 * Create a new container (and attach it to the given parent, if not NULL).
 * This function only initializes the data structures.
 *
 */
Con *con_new_skeleton(Con *parent, i3Window *window);

/**
 * A wrapper for con_new_skeleton, to retain the old con_new behaviour
 *
 */
Con *con_new(Con *parent, i3Window *window);

/**
 * Frees the specified container.
 *
 */
void con_free(Con *con);

/**
 * Sets input focus to the given container. Will be updated in X11 in the next
 * run of x_push_changes().
 *
 */
void con_focus(Con *con);

/**
 * Sets input focus to the given container and raises it to the top.
 *
 */
void con_activate(Con *con);

/**
 * Activates the container like in con_activate but removes fullscreen
 * restrictions and properly warps the pointer if needed.
 *
 */
void con_activate_unblock(Con *con);

/**
 * Closes the given container.
 *
 */
void con_close(Con *con, kill_window_t kill_window);

/**
 * Returns true when this node is a leaf node (has no children)
 *
 */
bool con_is_leaf(Con *con);

/**
 * Returns true when this con is a leaf node with a managed X11 window (e.g.,
 * excluding dock containers)
 */
bool con_has_managed_window(Con *con);

/**
 * Returns true if a container should be considered split.
 *
 */
bool con_is_split(Con *con);

/**
 * This will only return true for containers which have some parent with
 * a tabbed / stacked parent of which they are not the currently focused child.
 *
 */
bool con_is_hidden(Con *con);

/**
 * Returns whether the container or any of its children is sticky.
 *
 */
bool con_is_sticky(Con *con);

/**
 * Returns true if this node has regular or floating children.
 *
 */
bool con_has_children(Con *con);

/**
 * Returns true if this node accepts a window (if the node swallows windows,
 * it might already have swallowed enough and cannot hold any more).
 *
 */
bool con_accepts_window(Con *con);

/**
 * Gets the output container (first container with CT_OUTPUT in hierarchy) this
 * node is on.
 *
 */
Con *con_get_output(Con *con);

/**
 * Gets the workspace container this node is on.
 *
 */
Con *con_get_workspace(Con *con);

/**
 * Searches parents of the given 'con' until it reaches one with the specified
 * 'orientation'. Aborts when it comes across a floating_con.
 *
 */
Con *con_parent_with_orientation(Con *con, orientation_t orientation);

/**
 * Returns the first fullscreen node below this node.
 *
 */
Con *con_get_fullscreen_con(Con *con, fullscreen_mode_t fullscreen_mode);

/**
 * Returns the fullscreen node that covers the given workspace if it exists.
 * This is either a CF_GLOBAL fullscreen container anywhere or a CF_OUTPUT
 * fullscreen container in the workspace.
 *
 */
Con *con_get_fullscreen_covering_ws(Con *ws);

/**
 * Returns true if the container is internal, such as __i3_scratch
 *
 */
bool con_is_internal(Con *con);

/**
 * Returns true if the node is floating.
 *
 */
bool con_is_floating(Con *con);

/**
 * Returns true if the container is a docked container.
 *
 */
bool con_is_docked(Con *con);

/**
 * Checks if the given container is either floating or inside some floating
 * container. It returns the FLOATING_CON container.
 *
 */
Con *con_inside_floating(Con *con);

/**
 * Checks if the given container is inside a focused container.
 *
 */
bool con_inside_focused(Con *con);

/**
 * Checks if the container has the given parent as an actual parent.
 *
 */
bool con_has_parent(Con *con, Con *parent);

/**
 * Returns the container with the given client window ID or NULL if no such
 * container exists.
 *
 */
Con *con_by_window_id(xcb_window_t window);

/**
 * Returns the container with the given container ID or NULL if no such
 * container exists.
 *
 */
Con *con_by_con_id(long target);

/**
 * Returns true if the given container (still) exists.
 * This can be used, e.g., to make sure a container hasn't been closed in the meantime.
 *
 */
bool con_exists(Con *con);

/**
 * Returns the container with the given frame ID or NULL if no such container
 * exists.
 *
 */
Con *con_by_frame_id(xcb_window_t frame);

/**
 * Returns the container with the given mark or NULL if no such container
 * exists.
 *
 */
Con *con_by_mark(const char *mark);

/**
 * Returns true if and only if the given containers holds the mark.
 *
 */
bool con_has_mark(Con *con, const char *mark);

/**
 * Toggles the mark on a container.
 * If the container already has this mark, the mark is removed.
 * Otherwise, the mark is assigned to the container.
 *
 */
void con_mark_toggle(Con *con, const char *mark, mark_mode_t mode);

/**
 * Assigns a mark to the container.
 *
 */
void con_mark(Con *con, const char *mark, mark_mode_t mode);

/**
 * Removes marks from containers.
 * If con is NULL, all containers are considered.
 * If name is NULL, this removes all existing marks.
 * Otherwise, it will only remove the given mark (if it is present).
 *
 */
void con_unmark(Con *con, const char *name);

/**
 * Returns the first container below 'con' which wants to swallow this window
 * TODO: priority
 *
 */
Con *con_for_window(Con *con, i3Window *window, Match **store_match);

/**
 * Iterate over the container's focus stack and return an array with the
 * containers inside it, ordered from higher focus order to lowest.
 *
 */
Con **get_focus_order(Con *con);

/**
 * Clear the container's focus stack and re-add it using the provided container
 * array. The function doesn't check if the provided array contains the same
 * containers with the previous focus stack but will not add floating containers
 * in the new focus stack if container is not a workspace.
 *
 */
void set_focus_order(Con *con, Con **focus_order);

/**
 * Returns the number of children of this container.
 *
 */
int con_num_children(Con *con);

/**
 * Returns the number of visible non-floating children of this container.
 * For example, if the container contains a hsplit which has two children,
 * this will return 2 instead of 1.
 */
int con_num_visible_children(Con *con);

/**
 * Count the number of windows (i.e., leaf containers).
 *
 */
int con_num_windows(Con *con);

/**
 * Attaches the given container to the given parent. This happens when moving
 * a container or when inserting a new container at a specific place in the
 * tree.
 *
 * ignore_focus is to just insert the Con at the end (useful when creating a
 * new split container *around* some containers, that is, detaching and
 * attaching them in order without wanting to mess with the focus in between).
 *
 */
void con_attach(Con *con, Con *parent, bool ignore_focus);

/**
 * Detaches the given container from its current parent
 *
 */
void con_detach(Con *con);

/**
 * Updates the percent attribute of the children of the given container. This
 * function needs to be called when a window is added or removed from a
 * container.
 *
 */
void con_fix_percent(Con *con);

/**
 * Toggles fullscreen mode for the given container. Fullscreen mode will not be
 * entered when there already is a fullscreen container on this workspace.
 *
 */
void con_toggle_fullscreen(Con *con, int fullscreen_mode);

/**
 * Enables fullscreen mode for the given container, if necessary.
 *
 */
void con_enable_fullscreen(Con *con, fullscreen_mode_t fullscreen_mode);

/**
 * Disables fullscreen mode for the given container, if necessary.
 *
 */
void con_disable_fullscreen(Con *con);

/**
 * Moves the given container to the currently focused container on the given
 * workspace.
 *
 * The fix_coordinates flag will translate the current coordinates (offset from
 * the monitor position basically) to appropriate coordinates on the
 * destination workspace.
 * Not enabling this behaviour comes in handy when this function gets called by
 * floating_maybe_reassign_ws, which will only "move" a floating window when it
 * *already* changed its coordinates to a different output.
 *
 * The dont_warp flag disables pointer warping and will be set when this
 * function is called while dragging a floating window.
 *
 * If ignore_focus is set, the container will be moved without modifying focus
 * at all.
 *
 * TODO: is there a better place for this function?
 *
 */
void con_move_to_workspace(Con *con, Con *workspace, bool fix_coordinates,
                           bool dont_warp, bool ignore_focus);

/**
 * Moves the given container to the currently focused container on the
 * visible workspace on the given output.
 *
 */
void con_move_to_output(Con *con, Output *output, bool fix_coordinates);

/**
 * Moves the given container to the currently focused container on the
 * visible workspace on the output specified by the given name.
 * The current output for the container is used to resolve relative names
 * such as left, right, up, down.
 *
 */
bool con_move_to_output_name(Con *con, const char *name, bool fix_coordinates);

/**
 * Moves the given container to the given mark.
 *
 */
bool con_move_to_mark(Con *con, const char *mark);

/**
 * Returns the orientation of the given container (for stacked containers,
 * vertical orientation is used regardless of the actual orientation of the
 * container).
 *
 */
orientation_t con_orientation(Con *con);

/**
 * Returns the container which will be focused next when the given container
 * is not available anymore. Called in tree_close_internal and con_move_to_workspace
 * to properly restore focus.
 *
 */
Con *con_next_focused(Con *con);

/**
 * Returns the focused con inside this client, descending the tree as far as
 * possible. This comes in handy when attaching a con to a workspace at the
 * currently focused position, for example.
 *
 */
Con *con_descend_focused(Con *con);

/**
 * Returns the focused con inside this client, descending the tree as far as
 * possible. This comes in handy when attaching a con to a workspace at the
 * currently focused position, for example.
 *
 * Works like con_descend_focused but considers only tiling cons.
 *
 */
Con *con_descend_tiling_focused(Con *con);

/**
 * Returns the leftmost, rightmost, etc. container in sub-tree. For example, if
 * direction is D_LEFT, then we return the rightmost container and if direction
 * is D_RIGHT, we return the leftmost container.  This is because if we are
 * moving D_LEFT, and thus want the rightmost container.
 */
Con *con_descend_direction(Con *con, direction_t direction);

/**
 * Returns a "relative" Rect which contains the amount of pixels that need to
 * be added to the original Rect to get the final position (obviously the
 * amount of pixels for normal, 1pixel and borderless are different).
 *
 */
Rect con_border_style_rect(Con *con);

/**
 * Returns adjacent borders of the window. We need this if hide_edge_borders is
 * enabled.
 */
adjacent_t con_adjacent_borders(Con *con);

/**
 * Use this function to get a container’s border style. This is important
 * because when inside a stack, the border style is always BS_NORMAL.
 * For tabbed mode, the same applies, with one exception: when the container is
 * borderless and the only element in the tabbed container, the border is not
 * rendered.
 *
 * For children of a CT_DOCKAREA, the border style is always none.
 *
 */
int con_border_style(Con *con);

/**
 * Sets the given border style on con, correctly keeping the position/size of a
 * floating window.
 *
 */
void con_set_border_style(Con *con, int border_style, int border_width);

/**
 * This function changes the layout of a given container. Use it to handle
 * special cases like changing a whole workspace to stacked/tabbed (creates a
 * new split container before).
 *
 */
void con_set_layout(Con *con, layout_t layout);

/**
 * This function toggles the layout of a given container. toggle_mode can be
 * either 'default' (toggle only between stacked/tabbed/last_split_layout),
 * 'split' (toggle only between splitv/splith) or 'all' (toggle between all
 * layouts).
 *
 */
void con_toggle_layout(Con *con, const char *toggle_mode);

/**
 * This function changes the way new containers get added to layouts. The
 * 'default' means the layout is filled left-to-right or top-to-bottom
 * depending on orientation. 'reverse' changes that to right-to-left or
 * bottom-to-top. 'toggle' inverts the setting depending on its previous value.
 *
 */
void con_set_layout_fill_order(Con *con, const char *fill_order);

/**
 * Determines the minimum size of the given con by looking at its children (for
 * split/stacked/tabbed cons). Will be called when resizing floating cons
 *
 */
Rect con_minimum_size(Con *con);

/**
 * Returns true if changing the focus to con would be allowed considering
 * the fullscreen focus constraints. Specifically, if a fullscreen container or
 * any of its descendants is focused, this function returns true if and only if
 * focusing con would mean that focus would still be visible on screen, i.e.,
 * the newly focused container would not be obscured by a fullscreen container.
 *
 * In the simplest case, if a fullscreen container or any of its descendants is
 * fullscreen, this functions returns true if con is the fullscreen container
 * itself or any of its descendants, as this means focus wouldn't escape the
 * boundaries of the fullscreen container.
 *
 * In case the fullscreen container is of type CF_OUTPUT, this function returns
 * true if con is on a different workspace, as focus wouldn't be obscured by
 * the fullscreen container that is constrained to a different workspace.
 *
 * Note that this same logic can be applied to moving containers. If a
 * container can be focused under the fullscreen focus constraints, it can also
 * become a parent or sibling to the currently focused container.
 *
 */
bool con_fullscreen_permits_focusing(Con *con);

/**
 * Checks if the given container has an urgent child.
 *
 */
bool con_has_urgent_child(Con *con);

/**
 * Make all parent containers urgent if con is urgent or clear the urgent flag
 * of all parent containers if there are no more urgent children left.
 *
 */
void con_update_parents_urgency(Con *con);

/**
 * Set urgency flag to the container, all the parent containers and the workspace.
 *
 */
void con_set_urgency(Con *con, bool urgent);

/**
 * Create a string representing the subtree under con.
 *
 */
char *con_get_tree_representation(Con *con);

/**
 * force parent split containers to be redrawn
 *
 */
void con_force_split_parents_redraw(Con *con);

/**
 * Returns the window title considering the current title format.
 *
 */
i3String *con_parse_title_format(Con *con);

/**
 * Swaps the two containers.
 *
 */
bool con_swap(Con *first, Con *second);

/**
 * Returns given container's rect size depending on its orientation.
 * i.e. its width when horizontal, its height when vertical.
 *
 */
uint32_t con_rect_size_in_orientation(Con *con);

/**
 * Merges container specific data that should move with the window (e.g. marks,
 * title format, and the window itself) into another container, and closes the
 * old container.
 *
 */
void con_merge_into(Con *old, Con *new);
