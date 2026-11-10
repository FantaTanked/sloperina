/* Ocarina of Time (Switch 2): relocatable main offsets and shared structure layouts. */
#ifndef Z64HD_H
#define Z64HD_H

#include <stdbool.h>
#include <stdint.h>

struct z64hd_link_path {
  uint32_t global;
  uint32_t member;
};

struct z64hd_store {
  uint32_t off;
  uint32_t original;
  uint32_t bit;
};

/* X and Y share one store so it has its own stub with a bit for each axis lol */
struct z64hd_pair {
  uint32_t off;
  uint32_t original;  /* stp d0, d1, [x23, #0x10] */
  uint32_t x_held;    /* what the store becomes while X is held: str d1, [x23, #0x18] (Y only) */
  uint32_t y_held;    /* while Y is held: str d0, [x23, #0x10] (X only) */
};

#define LOCK_BIT_X         0u /* body X (horizontal) */
#define LOCK_BIT_Y         1u /* body Y (height) */
#define LOCK_BIT_Z         2u /* body Z (horizontal) */
#define LOCK_BIT_COLLISION 3u /* the collision resolver's two stores; set whenever an axis is held */

struct z64hd_build {
  const char *version;
  const char *build_id;
  uint32_t revision;
  uint32_t nnmain;
  uint32_t mod0;
  uint32_t cave_start;
  uint32_t cave_end;
  uint32_t hook_slot;
  uint32_t lock_slot;               /* 8-byte word in main's bss */
  const struct z64hd_pair *pair;    /* NULL when the build has no stores */
  const struct z64hd_store *stores;
  uint32_t store_count;
  uint32_t frame_site;
  uint32_t gd_unlock;
  uint32_t gd_mgr_ptr;
  uint32_t gd_getint;
  uint32_t gd_setint;
  uint32_t vt_int_field;
  uint32_t vt_bool_type;
  uint32_t vt_float_type;
  uint32_t vt_enum_type;
  uint32_t pouch_mgr_ptr;
  uint32_t pouch_add;
  uint32_t pouch_notify;
  uint32_t pouch_notify_ptr;
  uint32_t pouch_remove;
  uint32_t equip_slot_fn;
  uint32_t equip_all_fn;
  uint32_t vt_equip;
  uint32_t vt_equip_owner;
  uint32_t ui_mgr_ptr;
  uint32_t vt_dpad;
  uint32_t vt_hud;
  uint32_t vt_map_screen;
  uint32_t event_mgr_ptr;
  uint32_t event_req_ctor;
  uint32_t event_req_submit;
  uint32_t event_req_sub_dtor;
  uint32_t event_req_sub_vt;
  uint32_t vt_linkobj;
  uint32_t vt_param;
  uint32_t disp_ptr;
  uint32_t camera_ptr;
  uint32_t frame_counter;
  uint32_t actor_mgr_ptr; /* the actor manager (actors.c); 0: not known for this build */
  uint32_t vt_life;       /* the Life component's vtable */
  /* Link's character controller (cct.c; FINDINGS "Native collision bypass"); 0: not known for this build */
  uint32_t vt_cct_property;
  uint32_t vt_cct_body;
  uint32_t cct_force_no_hit;  /* void (body, bool): queued, preserves filter state */
  uint32_t cct_set_position;  /* void (property, const vec3f_t *): updates attached bodies */
  uint32_t cct_set_velocity;  /* void (body, const vec3f_t *): queues linear velocity */
  struct z64hd_link_path link_paths[5];
};

extern const struct z64hd_build *z64hd_game;
extern uintptr_t z64hd_main;

#define MAIN(off)          (z64hd_main + (uintptr_t)(off))

/* the character controller's, for the running build (0 where it isn't known) */
#define Z64HD_VT_CCT_PROPERTY  (z64hd_game->vt_cct_property)
#define Z64HD_VT_CCT_BODY      (z64hd_game->vt_cct_body)
#define Z64HD_CCT_FORCE_NO_HIT (z64hd_game->cct_force_no_hit)
#define Z64HD_CCT_SET_POSITION (z64hd_game->cct_set_position)
#define Z64HD_CCT_SET_VELOCITY (z64hd_game->cct_set_velocity)
#define MAIN_FN(type, off) ((type)MAIN(off))

/* Structure offsets shared by the supported builds. */
#define Z64HD_GD_INT_TYPE       0xC8        /* manager+: the Int type entry (see gamedata.h) */
#define Z64HD_GD_TYPES_SCAN     0x800       /* other type entries are found by vtable within manager+0..this */
#define Z64HD_GD_STRING_TYPE    0x608       /* manager+: the String64 type entry */
#define Z64HD_STR_STRIDE        0x108
#define Z64HD_STR_VALUE         0x68        /* char *value, followed by u32 capacity */
#define Z64HD_UI_REFRESH        (0x310 / 8) /* vtable slot 98: "refresh contents" */
#define Z64HD_EVENT_REQ_SIZE    0x1B20
#define Z64HD_EVENT_REQ_PARAMS  0x08        /* param entry array (32 inline entries at +0x18) */
#define Z64HD_EVENT_REQ_NPARAMS 0x10        /* u32 count; capacity (u32) at +0 */
#define Z64HD_EVENT_REQ_SUB_A   0x1950
#define Z64HD_EVENT_REQ_SUB_B   0x1968
#define Z64HD_EVENT_REQ_NAME    0x1A90      /* char *event name; capacity (u32) at +0x1A98 */
#define Z64HD_EVENT_PARAM_SIZE  0xC8        /* entry: u32 type, char *name (+8, cap u32 +0x10), value +0x98 */
#define Z64HD_EVENT_PARAM_STRING 1          /* value: char * */
#define Z64HD_EVENT_PARAM_BOOL   4          /* value: u8 */

#endif
