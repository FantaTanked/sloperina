/* The game's camera, for drawing things in the world over the picture (air walls, enemy health). */
#include "view.h"

#include "link.h"
#include "sys.h"

float tanf(float x); /* from sdk */
float sqrtf(float x);

/* the game's camera (link.h camera()) */
#define CAM_POSITION 0x38
#define CAM_LOOK_AT 0x44
#define CAM_FOV_Y 0x130
#define CAM_ASPECT 0x140

bool view_camera(view_cam_t *cam)
{
  uintptr_t game_cam = camera();
  if (!game_cam || !sys_mapped(game_cam + CAM_POSITION, CAM_ASPECT - CAM_POSITION + 4, SYS_PERM_R))
    return false;
  const float *pos = (const float *)(game_cam + CAM_POSITION);
  const float *look = (const float *)(game_cam + CAM_LOOK_AT);
  float fov = *(const float *)(game_cam + CAM_FOV_Y);
  float aspect = *(const float *)(game_cam + CAM_ASPECT);
  if (!(fov > 0.1f && fov < 3.0f) || !(aspect > 0.5f && aspect < 4.0f))
    return false;
  float f[3] = { look[0] - pos[0], look[1] - pos[1], look[2] - pos[2] };
  float f_len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (f_len < 1e-4f)
    return false;
  for (int i = 0; i < 3; i++) {
    cam->pos[i] = pos[i];
    cam->forward[i] = f[i] / f_len;
  }
  float r[3] = { -cam->forward[2], 0, cam->forward[0] }; /* forward x world up */
  float r_len = sqrtf(r[0] * r[0] + r[2] * r[2]);
  if (r_len < 1e-4f)
    return false;
  cam->right[0] = r[0] / r_len;
  cam->right[1] = 0;
  cam->right[2] = r[2] / r_len;
  /* up = right x forward */
  cam->up[0] = cam->right[1] * cam->forward[2] - cam->right[2] * cam->forward[1];
  cam->up[1] = cam->right[2] * cam->forward[0] - cam->right[0] * cam->forward[2];
  cam->up[2] = cam->right[0] * cam->forward[1] - cam->right[1] * cam->forward[0];
  cam->tan_y = tanf(fov / 2);
  cam->tan_x = cam->tan_y * aspect;
  return true;
}

/* world point x in camera space: (right, up, forward) */
void view_point(const view_cam_t *cam, const float x[3], float v[3])
{
  float d[3] = { x[0] - cam->pos[0], x[1] - cam->pos[1], x[2] - cam->pos[2] };
  v[0] = d[0] * cam->right[0] + d[1] * cam->right[1] + d[2] * cam->right[2];
  v[1] = d[0] * cam->up[0] + d[1] * cam->up[1] + d[2] * cam->up[2];
  v[2] = d[0] * cam->forward[0] + d[1] * cam->forward[1] + d[2] * cam->forward[2];
}

/* 0..1 across and down the picture */
void view_screen(const view_cam_t *cam, const float v[3], float *sx, float *sy)
{
  *sx = (v[0] / (v[2] * cam->tan_x) + 1) / 2;
  *sy = (1 - v[1] / (v[2] * cam->tan_y)) / 2;
}
