/* Air walls: port of airwall.py (FINDINGS §5.11), drawn as dotted outlines.
 *
 * Walls come from data.c (airwalls.json, built from the map layouts by tools/airwalls.py). A wall's
 * world point for a local shape point p is t + R (s * p), with R = Rz Ry Rx from its rotation, as
 * sead's makeR. Each frame the walls within AW_RANGE of the camera are projected with the game's
 * camera (+0x38 position, +0x44 look-at, +0x130 vertical fov, +0x140 aspect) and their edges turned
 * into rectangles, nearest wall first, which nvn.c draws by clearing them (no shaders). Edges within
 * about 19 degrees of level or upright are solid lines of a few long rectangles (a staircase); steeper
 * ones are dots, which takes fewer clears than a staircase there. (FINDINGS §5.11 had see-through
 * faces; here only the outlines, and like the Python view they show through scenery.) */
#include "airwall.h"

#include "data.h"
#include "gamedata.h"
#include "link.h"
#include "lang.h"
#include "sys.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float tanf(float x);
float sqrtf(float x);

bool airwalls_shown, airwalls_all;

#define AW_RANGE 60.0f
#define MAX_WALLS 256
#define MAX_POINTS 4096
#define MAX_EDGES 8192
#define MAX_DOTS 3000
#define MAX_DOTS_PER_EDGE 2000
#define REF_WIDTH 1920    /* the picture is worked out in pixels of a 1920x1080 one */
#define REF_HEIGHT 1080
#define DOT_SPACING 6.0f  /* reference pixels between dots */
#define LINE_WIDTH 2.0f   /* reference pixels: a line's thickness, one staircase step at most */
#define STRAIGHT 0.344f   /* tan 19 degrees: up to this slope a staircase takes fewer clears */
#define NEAR_PLANE 0.05f
#define PHYS_Y 0.585f     /* Link's physics position is this far above his drawn one */
#define FACE_UPRIGHT 0.7f /* a face normal with more height than this points up (or down) */

#define WALL_KIND_BLOCKS 0 /* the kind of wall that stops Link */
#define SHAPE_BOX 0        /* any other shape type is a cylinder */
#define CYLINDER_SEGMENTS 16
#define CYLINDER_SIDE_EVERY 4 /* a side line at every 4th segment: four in all */
#define TWO_PI 6.2831853f

/* the game's camera */
#define CAM_POSITION 0x38
#define CAM_LOOK_AT 0x44
#define CAM_FOV_Y 0x130
#define CAM_ASPECT 0x140

/* --- geometry, worked out once per map ------------------------------------------------------------ */

static const airmap_t *current_map;
static char current_map_name[64];
static struct {
  float R[9]; /* rotation, row-major */
  float centre[3], radius;
  uint16_t first_edge, n_edges;
} wall_geo[MAX_WALLS];
static int n_walls;
static float points[MAX_POINTS][3];
static uint16_t edges[MAX_EDGES][2];
static int n_points, n_edges;

static const airwall_t *map_wall(int index)
{
  return &data_airwalls[current_map->first + index];
}

static bool wall_shown(const airwall_t *wall)
{
  return wall->kind == WALL_KIND_BLOCKS || airwalls_all;
}

/* R = Rz Ry Rx for rotation angles (x, y, z) */
static void rotation_matrix(const float angles[3], float R[9])
{
  float sx = sinf(angles[0]), cx = cosf(angles[0]);
  float sy = sinf(angles[1]), cy = cosf(angles[1]);
  float sz = sinf(angles[2]), cz = cosf(angles[2]);
  float m[9] = { cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
                 cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
                 -sy,     sx * cy,                cx * cy };
  memcpy(R, m, sizeof(m));
}

/* t + R (s * local) */
static void to_world(const airwall_t *wall, const float R[9], const float local[3], float out[3])
{
  float scaled[3] = { local[0] * wall->s[0], local[1] * wall->s[1], local[2] * wall->s[2] };
  for (int i = 0; i < 3; i++)
    out[i] = R[i * 3] * scaled[0] + R[i * 3 + 1] * scaled[1] + R[i * 3 + 2] * scaled[2] + wall->t[i];
}

static int add_point(const float p[3])
{
  if (n_points >= MAX_POINTS)
    return -1;
  memcpy(points[n_points], p, sizeof(points[0]));
  return n_points++;
}

static void add_edge(int a, int b)
{
  if (a >= 0 && b >= 0 && n_edges < MAX_EDGES) {
    edges[n_edges][0] = (uint16_t)a;
    edges[n_edges][1] = (uint16_t)b;
    n_edges++;
  }
}

/* A box: 8 corners (bit k of the corner number picks the side on axis k) and 12 edges. */
static void add_box(const airwall_t *wall, const float R[9], const airshape_t *box)
{
  int base = n_points;
  for (int corner = 0; corner < 8; corner++) {
    float local[3], world[3];
    for (int axis = 0; axis < 3; axis++)
      local[axis] = box->a[axis] + (corner >> axis & 1 ? box->b[axis] : -box->b[axis]);
    to_world(wall, R, local, world);
    add_point(world);
  }
  for (int corner = 0; corner < 8; corner++)
    for (int axis = 0; axis < 3; axis++)
      if (!(corner >> axis & 1))
        add_edge(base + corner, base + (corner | 1 << axis));
}

/* A cylinder: rings at both ends, joined by four side lines. */
static void add_cylinder(const airwall_t *wall, const float R[9], const airshape_t *cylinder)
{
  int base = n_points;
  for (int ring = 0; ring < 2; ring++) {
    const float *centre = ring ? cylinder->b : cylinder->a;
    for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
      float angle = TWO_PI * j / CYLINDER_SEGMENTS;
      float local[3] = { centre[0] + cylinder->radius * cosf(angle), centre[1],
                         centre[2] + cylinder->radius * sinf(angle) };
      float world[3];
      to_world(wall, R, local, world);
      add_point(world);
    }
  }
  for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
    add_edge(base + j, base + (j + 1) % CYLINDER_SEGMENTS);
    add_edge(base + CYLINDER_SEGMENTS + j, base + CYLINDER_SEGMENTS + (j + 1) % CYLINDER_SEGMENTS);
    if (j % CYLINDER_SIDE_EVERY == 0)
      add_edge(base + j, base + CYLINDER_SEGMENTS + j);
  }
}

/* The sphere around points[first_point..n_points), for skipping walls quickly. */
static void bounding_sphere(const airwall_t *wall, int first_point, float centre[3], float *radius)
{
  float sum[3] = { 0 };
  int count = n_points - first_point;
  for (int j = first_point; j < n_points; j++)
    for (int axis = 0; axis < 3; axis++)
      sum[axis] += points[j][axis];
  for (int axis = 0; axis < 3; axis++)
    centre[axis] = count ? sum[axis] / count : wall->t[axis];
  *radius = 0;
  for (int j = first_point; j < n_points; j++) {
    float dx = points[j][0] - centre[0];
    float dy = points[j][1] - centre[1];
    float dz = points[j][2] - centre[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance > *radius)
      *radius = distance;
  }
}

static void build_map_geometry(const airmap_t *map)
{
  n_walls = 0;
  n_points = 0;
  n_edges = 0;
  for (int i = 0; i < map->n && n_walls < MAX_WALLS; i++) {
    const airwall_t *wall = &data_airwalls[map->first + i];
    __typeof__(wall_geo[0]) *geo = &wall_geo[n_walls++];
    rotation_matrix(wall->r, geo->R);
    geo->first_edge = (uint16_t)n_edges;
    int first_point = n_points;
    for (int k = 0; k < wall->n_shapes; k++) {
      const airshape_t *shape = &data_airshapes[wall->first_shape + k];
      if (shape->type == SHAPE_BOX)
        add_box(wall, geo->R, shape);
      else
        add_cylinder(wall, geo->R, shape);
    }
    geo->n_edges = (uint16_t)(n_edges - geo->first_edge);
    bounding_sphere(wall, first_point, geo->centre, &geo->radius);
  }
  log_printf("air walls: %s: %d walls, %d edges", map->map, n_walls, n_edges);
}

static bool str_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* Rebuild the geometry when the map changes. */
static void update_map(void)
{
  const char *name = gd_get_str(GD_STR_MAP);
  if (!name || !name[0]) {
    current_map = 0;
    return;
  }
  if (current_map && str_equal(current_map_name, name))
    return;
  snprintf(current_map_name, sizeof(current_map_name), "%s", name);
  current_map = 0;
  for (int i = 0; i < data_n_airmaps; i++)
    if (str_equal(data_airmaps[i].map, name)) {
      current_map = &data_airmaps[i];
      build_map_geometry(current_map);
      break;
    }
  if (!current_map)
    n_walls = 0;
}

/* --- Link inside a wall (airwall.py inside()) ------------------------------------------------------- */

/* How deep point P is inside wall `index` (0 if outside). *up: the nearest face points up (+1),
 * down (-1) or sideways (0). Only boxes count. */
static float depth_inside(int index, const float P[3], int *up)
{
  const airwall_t *wall = map_wall(index);
  const float *R = wall_geo[index].R;
  float d[3] = { P[0] - wall->t[0], P[1] - wall->t[1], P[2] - wall->t[2] };
  float rotated[3]; /* R^T d: P in the wall's own axes, still scaled */
  for (int i = 0; i < 3; i++)
    rotated[i] = R[i] * d[0] + R[3 + i] * d[1] + R[6 + i] * d[2];
  for (int k = 0; k < wall->n_shapes; k++) {
    const airshape_t *box = &data_airshapes[wall->first_shape + k];
    if (box->type != SHAPE_BOX)
      continue;
    float best = 1e9f;
    float normal[3] = { 0 };
    bool in = true;
    for (int axis = 0; axis < 3; axis++) {
      float scale = wall->s[axis] < 0 ? -wall->s[axis] : wall->s[axis];
      if (scale < 1e-9f)
        scale = 1e-9f;
      float local = rotated[axis] / scale - box->a[axis];
      if (local <= -box->b[axis] || local >= box->b[axis]) {
        in = false;
        break;
      }
      /* distance to the face on each side of this axis */
      for (int sign = -1; sign <= 1; sign += 2) {
        float depth = (box->b[axis] - sign * local) * scale;
        if (depth < best) {
          best = depth;
          for (int i = 0; i < 3; i++)
            normal[i] = R[i * 3 + axis] * sign;
        }
      }
    }
    if (in) {
      *up = normal[1] > FACE_UPRIGHT ? 1 : normal[1] < -FACE_UPRIGHT ? -1 : 0;
      return best;
    }
  }
  return 0;
}

static float link_depth;
static int link_wall = -1; /* the wall Link is deepest in, or -1 */
static int link_up;

static void check_link(void)
{
  vec3f_t drawn;
  link_wall = -1;
  if (!current_map || !link_drawn_pos(&drawn))
    return;
  float P[3] = { drawn.x, drawn.y + PHYS_Y, drawn.z };
  float best = 0;
  for (int i = 0; i < n_walls; i++) {
    if (!wall_shown(map_wall(i)))
      continue;
    float dx = P[0] - wall_geo[i].centre[0];
    float dy = P[1] - wall_geo[i].centre[1];
    float dz = P[2] - wall_geo[i].centre[2];
    if (dx * dx + dy * dy + dz * dz > wall_geo[i].radius * wall_geo[i].radius)
      continue;
    int up;
    float depth = depth_inside(i, P, &up);
    if (depth > best) {
      best = depth;
      link_wall = i;
      link_up = up;
    }
  }
  link_depth = best;
}

void airwall_link_text(char *buf, int len)
{
  if (!current_map)
    snprintf(buf, len, TF("no walls here"));
  else if (link_wall < 0)
    snprintf(buf, len, TF("outside"));
  else
    snprintf(buf, len, TF("inside %.2f, out %s"), link_depth,
             T(link_up > 0 ? "up" : link_up < 0 ? "down" : "sideways"));
}

/* --- projection to dots ------------------------------------------------------------------------- */

/* Two buffers: the game thread fills one while the render thread draws the other. */
static aw_dot_t dots[2][MAX_DOTS];
static int n_dots[2];
static volatile int published = -1; /* the buffer to draw, or -1 for none */
static int n_walls_near;

typedef struct {
  float pos[3], forward[3], right[3], up[3];
  float tan_x, tan_y; /* tangents of half the horizontal and vertical fov */
} cam_t;

static bool read_camera(cam_t *cam)
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
static void to_view(const cam_t *cam, const float x[3], float v[3])
{
  float d[3] = { x[0] - cam->pos[0], x[1] - cam->pos[1], x[2] - cam->pos[2] };
  v[0] = d[0] * cam->right[0] + d[1] * cam->right[1] + d[2] * cam->right[2];
  v[1] = d[0] * cam->up[0] + d[1] * cam->up[1] + d[2] * cam->up[2];
  v[2] = d[0] * cam->forward[0] + d[1] * cam->forward[1] + d[2] * cam->forward[2];
}

/* 0..1 across and down the picture */
static void to_screen(const cam_t *cam, const float v[3], float *sx, float *sy)
{
  *sx = (v[0] / (v[2] * cam->tan_x) + 1) / 2;
  *sy = (1 - v[1] / (v[2] * cam->tan_y)) / 2;
}

/* 0..size to 0..65535, clamped */
static uint16_t to_unit16(float v, float size)
{
  v = v / size * 65535;
  return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* a rectangle from two corners in reference pixels */
static void add_rect(aw_dot_t *out, int *n, float ax, float ay, float bx, float by, uint8_t color)
{
  aw_dot_t *rect = &out[(*n)++];
  rect->x0 = to_unit16(ax < bx ? ax : bx, REF_WIDTH);
  rect->x1 = to_unit16(ax < bx ? bx : ax, REF_WIDTH);
  rect->y0 = to_unit16(ay < by ? ay : by, REF_HEIGHT);
  rect->y1 = to_unit16(ay < by ? by : ay, REF_HEIGHT);
  rect->color = color;
}

/* Cut the line va-vb (camera space) at the near plane. False if it's all behind it. */
static bool clip_near(float va[3], float vb[3])
{
  if (va[2] < NEAR_PLANE && vb[2] < NEAR_PLANE)
    return false;
  if (va[2] < NEAR_PLANE || vb[2] < NEAR_PLANE) {
    float t = (NEAR_PLANE - va[2]) / (vb[2] - va[2]);
    float cut[3];
    for (int i = 0; i < 3; i++)
      cut[i] = va[i] + t * (vb[i] - va[i]);
    if (va[2] < NEAR_PLANE)
      memcpy(va, cut, sizeof(cut));
    else
      memcpy(vb, cut, sizeof(cut));
  }
  return true;
}

/* Cut the line (x0, y0)-(x1, y1) to the reference picture (Liang-Barsky). False if it's all outside. */
static bool clip_to_picture(float *x0, float *y0, float *x1, float *y1)
{
  float dx = *x1 - *x0, dy = *y1 - *y0;
  float t0 = 0, t1 = 1;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { *x0, REF_WIDTH - *x0, *y0, REF_HEIGHT - *y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0)
        return false;
      continue;
    }
    float r = q[i] / p[i];
    if (p[i] < 0) {
      if (r > t1)
        return false;
      if (r > t0)
        t0 = r;
    } else {
      if (r < t0)
        return false;
      if (r < t1)
        t1 = r;
    }
  }
  *x1 = *x0 + t1 * dx;
  *y1 = *y0 + t1 * dy;
  *x0 += t0 * dx;
  *y0 += t0 * dy;
  return true;
}

/* A line on the picture: near level or upright, a staircase of long rectangles; otherwise dots. */
static void add_line(aw_dot_t *out, int *n, float x0, float y0, float x1, float y1, uint8_t color)
{
  float dx = x1 - x0, dy = y1 - y0;
  float abs_dx = dx < 0 ? -dx : dx, abs_dy = dy < 0 ? -dy : dy;
  bool level = abs_dx >= abs_dy;
  float major = level ? abs_dx : abs_dy, minor = level ? abs_dy : abs_dx;
  if (minor <= major * STRAIGHT) {
    /* pieces along the main direction, each at most LINE_WIDTH off the next */
    int pieces = (int)(minor / LINE_WIDTH) + 1;
    for (int i = 0; i < pieces && *n < MAX_DOTS; i++) {
      float ta = (float)i / pieces, tb = (float)(i + 1) / pieces, t_mid = (ta + tb) / 2;
      float ax, ay, bx, by;
      if (level) {
        ax = x0 + dx * ta;
        bx = x0 + dx * tb;
        ay = by = y0 + dy * t_mid;
      } else {
        ay = y0 + dy * ta;
        by = y0 + dy * tb;
        ax = bx = x0 + dx * t_mid;
      }
      add_rect(out, n, ax, ay, bx, by, color);
    }
    return;
  }
  int steps = (int)(major / DOT_SPACING);
  if (steps > MAX_DOTS_PER_EDGE)
    steps = MAX_DOTS_PER_EDGE;
  for (int i = 0; i <= steps && *n < MAX_DOTS; i++) {
    float t = steps ? (float)i / steps : 0;
    float x = x0 + dx * t, y = y0 + dy * t;
    add_rect(out, n, x, y, x, y, color);
  }
}

/* The edge a-b (world points), projected, clipped and added to out[*n...]. */
static void add_edge_dots(aw_dot_t *out, int *n, const cam_t *cam, const float a[3], const float b[3], uint8_t color)
{
  float va[3], vb[3];
  to_view(cam, a, va);
  to_view(cam, b, vb);
  if (!clip_near(va, vb))
    return;
  float x0, y0, x1, y1;
  to_screen(cam, va, &x0, &y0);
  to_screen(cam, vb, &x1, &y1);
  x0 *= REF_WIDTH;
  x1 *= REF_WIDTH;
  y0 *= REF_HEIGHT;
  y1 *= REF_HEIGHT;
  if (!clip_to_picture(&x0, &y0, &x1, &y1))
    return;
  add_line(out, n, x0, y0, x1, y1, color);
}

/* --- hidden while the game is paused ------------------------------------------------------------- */

/* The game's own pause state (see debug > pause research). Frame advance holds the game without
 * setting these, so the walls still show while frame advancing. */
#define GD_SEQUENCE_IS_PAUSE 0x604AEBE1u                   /* Sequence_IsPause (bool) */
#define GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT 0x64A61098u   /* Sequence_GamePauseRequestCount (int) */

/* True while the game shows its pause screen or an item menu, where the walls would draw over it. */
static bool game_is_paused(void)
{
  bool is_pause = false;
  int32_t pause_requests = 0;
  gd_get_bool(GD_SEQUENCE_IS_PAUSE, &is_pause);
  gd_get_int(GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT, &pause_requests);
  bool paused = is_pause || pause_requests > 0;

  /* Log each change, with both values, so it can be checked which one the game's menus set. */
  static bool was_paused;
  if (paused != was_paused) {
    if (paused)
      log_printf("air walls: hidden, game paused (IsPause %d, GamePauseRequestCount %d)", (int)is_pause,
                 (int)pause_requests);
    else
      log_printf("air walls: shown again, game unpaused");
    was_paused = paused;
  }
  return paused;
}

/* --- frame ---------------------------------------------------------------------------------------- */

static int16_t near_order[MAX_WALLS];  /* walls within AW_RANGE of the camera, nearest first */
static float near_distance[MAX_WALLS];

/* Fill near_order (an insertion sort) and return how many walls are in it. */
static int sort_near_walls(const cam_t *cam)
{
  int count = 0;
  for (int i = 0; i < n_walls; i++) {
    if (!wall_shown(map_wall(i)))
      continue;
    float dx = wall_geo[i].centre[0] - cam->pos[0];
    float dy = wall_geo[i].centre[1] - cam->pos[1];
    float dz = wall_geo[i].centre[2] - cam->pos[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz) - wall_geo[i].radius;
    if (distance > AW_RANGE)
      continue;
    int j = count++;
    while (j > 0 && near_distance[j - 1] > distance) {
      near_order[j] = near_order[j - 1];
      near_distance[j] = near_distance[j - 1];
      j--;
    }
    near_order[j] = (int16_t)i;
    near_distance[j] = distance;
  }
  return count;
}

void airwall_frame(void)
{
  update_map();
  check_link();
  int next = published == 0 ? 1 : 0;
  int n = 0;
  n_walls_near = 0;
  bool draw = airwalls_shown && current_map && !game_is_paused();
  cam_t cam;
  if (draw && read_camera(&cam)) {
    /* nearest wall first, so the dot budget goes to what's close */
    n_walls_near = sort_near_walls(&cam);
    for (int k = 0; k < n_walls_near && n < MAX_DOTS; k++) {
      int i = near_order[k];
      const airwall_t *wall = map_wall(i);
      uint8_t color = i == link_wall ? AW_INSIDE : wall->kind == WALL_KIND_BLOCKS ? AW_BLOCKS : AW_OTHER;
      for (int e = wall_geo[i].first_edge; e < wall_geo[i].first_edge + wall_geo[i].n_edges; e++)
        add_edge_dots(dots[next], &n, &cam, points[edges[e][0]], points[edges[e][1]], color);
    }
  }
  n_dots[next] = n;
  __atomic_store_n(&published, n ? next : -1, __ATOMIC_RELEASE);
}

const aw_dot_t *airwall_published(int *n)
{
  int buffer = __atomic_load_n(&published, __ATOMIC_ACQUIRE);
  *n = buffer < 0 ? 0 : n_dots[buffer];
  return buffer < 0 ? 0 : dots[buffer];
}

void airwall_summary(char *buf, int len)
{
  if (!current_map)
    snprintf(buf, len, TF("no air walls in this map"));
  else
    snprintf(buf, len, "%d walls here, %d within %.0f", n_walls, n_walls_near, (double)AW_RANGE);
}
