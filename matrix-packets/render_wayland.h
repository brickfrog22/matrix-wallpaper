#ifndef RENDER_WAYLAND_H
#define RENDER_WAYLAND_H

#include "streams.h"

/* Initialize Wayland connection, layer-shell surface, and Cairo context.
 * Blocks until the first configure event provides pixel dimensions.
 * Sets cell_width/cell_height and computes screen_width_cells/screen_height_cells.
 * Returns 0 on success, -1 on failure. */
int wayland_init(void);

/* Render one frame: clear buffer, draw streams, draw stats bar, commit.
 * Returns 0 on success, -1 if the display connection is lost. */
int render_frame_wayland(unsigned long frame_count);

/* Dispatch pending Wayland events (non-blocking).
 * Returns the Wayland display fd for use with poll(). */
int wayland_dispatch(void);

/* Get the display fd for poll() */
int wayland_get_fd(void);

/* Get current cell grid dimensions */
int wayland_get_width_cells(void);
int wayland_get_height_cells(void);

/* Check if a reconfigure happened (and clear the flag) */
int wayland_check_reconfigure(void);

/* Clean up Wayland resources */
void wayland_cleanup(void);

#endif /* RENDER_WAYLAND_H */
