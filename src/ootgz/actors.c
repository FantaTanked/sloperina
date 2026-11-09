/* The scene's actors: the enemies and their life, for enemyhp.c. Read from the game's disassembly
 * (FINDINGS §9.5), v131072 only so far.
 *
 * The actor manager *(main+actor_mgr_ptr) keeps the live actors in an intrusive red-black tree at
 * [mgr+0x38]: node N has left at +0x00, right at +0x08; each node sits at +8 in an entry E, which holds
 * the actor at +0x28 and the next entry with the same key at +0x38 (no -8). The game walks it in order
 * under the manager's lock; here it's read without the lock, so every pointer is checked and the walk
 * is capped (a tree changing under us costs at most a frame of wrong bars).
 *
 * An actor: its name (char *) at +0x20 ("Enemy_Wolfos"), its state at +0x2C (4 and 5: running; 6: an
 * enemy that hasn't appeared yet), its world position (3 floats) at +0x31C. Its Life component is
 * [[actor+0x270]+0x2B8] (vtable vt_life), with parameter objects (vtable vt_param, s32 value at +8)
 * for the current life at +0x1C10 and the max at +0x1C18 (QueryActorLife reads exactly these). */
#include "enemyhp.h"

#include "link.h"
#include "sys.h"
#include "z64hd.h"

#define MGR_TREE_ROOT 0x38
#define NODE_LEFT 0x00
#define NODE_RIGHT 0x08
#define ENTRY_NODE 0x08   /* the node's place in its entry */
#define ENTRY_ACTOR 0x28
#define ENTRY_NEXT 0x38
#define ENTRY_SIZE 0x40

#define ACTOR_NAME 0x20
#define ACTOR_STATE 0x2C
#define ACTOR_COMPONENTS 0x270
#define ACTOR_POS 0x31C
#define COMPONENTS_LIFE 0x2B8
#define LIFE_CURRENT 0x1C10
#define LIFE_MAX 0x1C18
#define PARAM_VALUE 0x8

/* states: below 2 not started (the game's walks skip them), 3 being set up, 4 and 5 running, 6 not yet
 * appeared (a Wolfos still in the ground, Stalfos before they drop, Poes before they show), 7..11 dying
 * or gone (FINDINGS §9.5) */
#define STATE_RUNNING 4
#define STATE_RUNNING_LAST 5

#define MAX_DEPTH 64
#define MAX_VISITS 4096
#define ENEMY_PREFIX "Enemy_"
#define BAR_HEIGHT 1.8f   /* metres above the actor's position; one height for all for now */
#define NEAR_LINK 30.0f   /* metres: enemies further from Link are left out (a dungeon keeps all its rooms') */
#define REPORT_FRAMES 300 /* a summary in the log this often while enemy health is on */

static bool readable(uintptr_t addr, size_t size)
{
  return addr && !(addr & 7) && sys_mapped(addr, size, SYS_PERM_R);
}

static bool starts_with(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return false;
  return true;
}

#define MAIN_SIZE_MAX 0x5000000 /* main's image is smaller than this */

static bool in_main(uintptr_t addr)
{
  return addr >= z64hd_main && addr < z64hd_main + MAIN_SIZE_MAX;
}

/* why actor_life gave up (for the log) */
enum { LIFE_OK, NO_COMPONENTS, NO_LIFE, LIFE_UNREADABLE, NO_PARAMS, N_LIFE_RESULTS };

/* An actor's life, if it has a Life component. Its vtable isn't required to be vt_life (Link's): an
 * enemy's may be a kind of it. */
static int actor_life(uintptr_t actor, int32_t *life, int32_t *max)
{
  if (!readable(actor + ACTOR_COMPONENTS, 8))
    return NO_COMPONENTS;
  uintptr_t components = *(uintptr_t *)(actor + ACTOR_COMPONENTS);
  if (!readable(components + COMPONENTS_LIFE, 8))
    return NO_COMPONENTS;
  uintptr_t component = *(uintptr_t *)(components + COMPONENTS_LIFE);
  if (!component)
    return NO_LIFE;
  if (!readable(component, LIFE_MAX + 8))
    return LIFE_UNREADABLE;
  uintptr_t cur = *(uintptr_t *)(component + LIFE_CURRENT);
  uintptr_t top = *(uintptr_t *)(component + LIFE_MAX);
  /* QueryActorLife reads +8 of both without checking their kind; here they must at least be objects of
   * the game's (a vtable in main) */
  if (!readable(cur, PARAM_VALUE + 4) || !readable(top, PARAM_VALUE + 4) || !in_main(*(uintptr_t *)cur) ||
      !in_main(*(uintptr_t *)top))
    return NO_PARAMS;
  *life = *(int32_t *)(cur + PARAM_VALUE);
  *max = *(int32_t *)(top + PARAM_VALUE);
  return LIFE_OK;
}

/* --- the walk ------------------------------------------------------------------------------------ */

static struct {
  enemy_t *out;
  float distance_sq[64]; /* each out[i]'s from Link, to keep the nearest when out is full */
  int max, n;
  int visits, actors, with_life, enemies;
  uintptr_t link;
  vec3f_t link_pos;
} walk;

static void use_actor(uintptr_t actor)
{
  walk.actors++;
  if (!readable(actor, ACTOR_POS + 12) || actor == walk.link)
    return;
  int32_t life = 0, max = 0;
  int result = actor_life(actor, &life, &max);
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  bool enemy = name && sys_mapped(name, 8, SYS_PERM_R) && starts_with((const char *)name, ENEMY_PREFIX);
  if (result != LIFE_OK)
    return;
  walk.with_life++;
  uint32_t state = *(uint32_t *)(actor + ACTOR_STATE);
  if (!enemy || state < STATE_RUNNING || state > STATE_RUNNING_LAST || max <= 0)
    return;
  const float *pos = (const float *)(actor + ACTOR_POS);
  float dx = pos[0] - walk.link_pos.x, dy = pos[1] - walk.link_pos.y, dz = pos[2] - walk.link_pos.z;
  float distance_sq = dx * dx + dy * dy + dz * dz;
  if (distance_sq > NEAR_LINK * NEAR_LINK)
    return;
  walk.enemies++;
  int slot = walk.n;
  if (walk.n < walk.max) {
    walk.n++;
  } else { /* full: in place of the furthest, if this one is nearer */
    slot = 0;
    for (int i = 1; i < walk.n; i++)
      if (walk.distance_sq[i] > walk.distance_sq[slot])
        slot = i;
    if (walk.distance_sq[slot] <= distance_sq)
      return;
  }
  walk.distance_sq[slot] = distance_sq;
  enemy_t *e = &walk.out[slot];
  e->id = actor;
  e->pos[0] = pos[0];
  e->pos[1] = pos[1];
  e->pos[2] = pos[2];
  e->height = BAR_HEIGHT;
  e->life = life;
  e->max_life = max;
}

/* in order, as the game does; iterative, with a stack of right children still to visit */
static void walk_tree(uintptr_t root)
{
  uintptr_t stack[MAX_DEPTH];
  int depth = 0;
  uintptr_t node = root;
  while ((node || depth) && walk.visits < MAX_VISITS) {
    while (node && depth < MAX_DEPTH) {
      if (!readable(node, 16))
        return;
      stack[depth++] = node;
      node = *(uintptr_t *)(node + NODE_LEFT);
    }
    if (node) /* deeper than a balanced tree of this size can be: not a tree any more */
      return;
    node = stack[--depth];
    for (uintptr_t entry = node - ENTRY_NODE; entry && walk.visits < MAX_VISITS;
         entry = *(uintptr_t *)(entry + ENTRY_NEXT)) {
      walk.visits++;
      if (!readable(entry, ENTRY_SIZE))
        break;
      uintptr_t actor = *(uintptr_t *)(entry + ENTRY_ACTOR);
      if (actor)
        use_actor(actor);
    }
    node = *(uintptr_t *)(node + NODE_RIGHT);
  }
}

/* --- reporting ----------------------------------------------------------------------------------- */

static int report_wait;

static void report(const enemy_t *enemies, int n)
{
  if (report_wait-- > 0)
    return;
  report_wait = REPORT_FRAMES;
  log_printf("actors: %d entries, %d actors, %d with life, %d enemies within %.0f m (%d kept)", walk.visits, walk.actors,
             walk.with_life, walk.enemies, (double)NEAR_LINK, n);
  for (int i = 0; i < n && i < 8; i++) {
    const enemy_t *e = &enemies[i];
    log_printf("actors: %s life %d/%d at %.2f, %.2f, %.2f (state %u)", *(const char **)(e->id + ACTOR_NAME),
               (int)e->life, (int)e->max_life, (double)e->pos[0], (double)e->pos[1], (double)e->pos[2],
               *(uint32_t *)(e->id + ACTOR_STATE));
  }
  int32_t life, max, *cur_ptr, *max_ptr;
  if (walk.link && actor_life(walk.link, &life, &max) == LIFE_OK && link_health(&cur_ptr, &max_ptr))
    log_printf("actors: Link's life %d/%d (link.c: %d/%d)", (int)life, (int)max, (int)*cur_ptr, (int)*max_ptr);
}

/* --- the enemies --------------------------------------------------------------------------------- */

int enemies_read(enemy_t *out, int max)
{
  if (!z64hd_game || !z64hd_game->actor_mgr_ptr)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return 0;
  walk.out = out;
  walk.max = max < (int)(sizeof(walk.distance_sq) / sizeof(walk.distance_sq[0])) ? max :
             (int)(sizeof(walk.distance_sq) / sizeof(walk.distance_sq[0]));
  walk.n = walk.visits = walk.actors = walk.with_life = walk.enemies = 0;
  walk.link = link_actor();
  if (!link_drawn_pos(&walk.link_pos))
    return 0;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  report(out, walk.n);
  return walk.n;
}

