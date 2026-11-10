/* The game's camera, for drawing things in the world over the picture (air walls, enemy health). */
#ifndef VIEW_H
#define VIEW_H

#include <stdbool.h>

typedef struct {
  float pos[3], forward[3], right[3], up[3];
  float tan_x, tan_y; /* tangents of half the horizontal and vertical fov */
  float near, far;    /* its projection's planes (metres ahead) */
} view_cam_t;

/* The camera this frame (position, look-at, vertical fov, aspect); false if it isn't readable. */
bool view_camera(view_cam_t *cam);
/* world point x in camera space: (right, up, forward) */
void view_point(const view_cam_t *cam, const float x[3], float v[3]);
/* camera-space point to 0..1 across and down the picture (v[2] must be in front of the camera) */
void view_screen(const view_cam_t *cam, const float v[3], float *sx, float *sy);
/* the game's depth buffer's value (0 at the near plane .. 1 at the far one) for a point `ahead` metres in
 * front of the camera (v[2]): 0 for one at the near plane or nearer */
float view_depth(const view_cam_t *cam, float ahead);

#endif
