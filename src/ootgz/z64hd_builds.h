/* Main offsets for program 0400486024DEC000, shared by the module and mkips. */
#ifndef Z64HD_BUILDS_H
#define Z64HD_BUILDS_H

#include "z64hd.h"

/* The stores that write Link's position which i found by abusing lichii's cheat engine
 * X and Y share the stp (pair); the variants per lock are the trainer's XY_VARIANTS. Z is one str.
 * The collision resolver's two stores freeze with any lock */
static const struct z64hd_pair pair_1_1_1 = {
  0x180EA24, 0x6D0106E0, /* stp d0, d1, [x23, #0x10] */
  0xFD000EE1,            /* str d1, [x23, #0x18]: X held, Y moves */
  0xFD000AE0,            /* str d0, [x23, #0x10]: Y held, X moves */
};

static const struct z64hd_store stores_1_1_1[] = {
  { 0x18D2C18, 0xA9002688, LOCK_BIT_COLLISION },
  { 0x18D2C20, 0xA9002668, LOCK_BIT_COLLISION },
  { 0x180EA2C, 0xFD0012E0, LOCK_BIT_Z },       /* str d0, [x23, #0x20] */
};

static const struct z64hd_build z64hd_builds[] = {
  {
    .version = "1.0.0",
    .build_id = "33FA6C2C23848282A77E8B88A0A93F4887C55920",
    .revision = 65536,
    .nnmain = 0x11C1FB0,
    .mod0 = 0x3BE705C,
    .cave_start = 0x3BE6A00,
    .cave_end = 0x3BE7000,
    .hook_slot = 0x4E2A070,
    .frame_site = 0x3AF2A6C,
    .gd_unlock = 0x3BE2B20,
    .gd_mgr_ptr = 0x4C184D8,
    .gd_getint = 0x420D50,
    .gd_setint = 0x4211F0,
    .vt_int_field = 0x4BBAB10,
    .vt_bool_type = 0x4BB9538,
    .vt_float_type = 0x4BB97B8,
    .vt_enum_type = 0x4BB98F8,
    .pouch_mgr_ptr = 0x4C18428,
    .pouch_add = 0x11378F0,
    /* The base game's item event returns from AddPouch without the separate notification. */
    .pouch_notify = 0,
    .pouch_notify_ptr = 0,
    .pouch_remove = 0x33456F0,
    .equip_slot_fn = 0x2DA26F0,
    .equip_all_fn = 0xEE0840,
    .vt_equip = 0x4AE4690,
    .vt_equip_owner = 0x4B7AA20,
    .ui_mgr_ptr = 0x4C183F0,
    .vt_dpad = 0x4B7BC20,
    .vt_hud = 0x4B85EB0,
    .vt_map_screen = 0x4B82AA0,
    .event_mgr_ptr = 0x4C183E8,
    .event_req_ctor = 0xD3C9A0,
    .event_req_submit = 0xD3D2B0,
    .event_req_sub_dtor = 0x27C630,
    .event_req_sub_vt = 0x49101E8,
    .vt_linkobj = 0x4AC0E30,
    .vt_param = 0x4AFE430,
    .disp_ptr = 0x4C18A98,
    .camera_ptr = 0x4E27C08,
    .frame_counter = 0x4E14740,
    /* made at 0xCCE6B0 (0x490 bytes; constructor 0xCCE730: a mutex at +0x00, +0x28 / +0x40 zeroed, the
     * actor tree's root at +0x38 null), as v131072's; the actors' life and position fields are the same */
    .actor_mgr_ptr = 0x4E20930,
    .link_paths = {
      { 0x4DC7210, 0x480 }, { 0x4E243F8, 0x2E0 }, { 0x4E23158, 0xEF8 },
      { 0x4E24400, 0x328 },
    },
  },
  {
    .version = "1.1.1",
    .build_id = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348",
    .revision = 131072,
    .nnmain = 0x1202CD0,
    .mod0 = 0x3C4705C,
    .cave_start = 0x3C46400,
    .cave_end = 0x3C47000,
    .hook_slot = 0x4E9A4D0,
    /* the word after the hook slot: unverified (BUILDING.md: watch it before relying on it) */
    .lock_slot = 0x4E9A4D8,
    .pair = &pair_1_1_1,
    .stores = stores_1_1_1,
    .store_count = sizeof(stores_1_1_1) / sizeof(stores_1_1_1[0]),
    .frame_site = 0x3B500DC,
    .gd_unlock = 0x3C42470,
    .gd_mgr_ptr = 0x4C7E180,
    .gd_getint = 0x163D20,
    .gd_setint = 0x163B70,
    .vt_int_field = 0x4C20718,
    .vt_bool_type = 0x4C1F140,
    .vt_float_type = 0x4C1F3C0,
    .vt_enum_type = 0x4C1F500,
    .pouch_mgr_ptr = 0x4C7E0D0,
    .pouch_add = 0x110D570,
    .pouch_notify = 0x110D0B0,
    .pouch_notify_ptr = 0x4E6B718,
    .pouch_remove = 0x33A4FA0,
    .equip_slot_fn = 0x2DF6C80,
    .equip_all_fn = 0xF201B0,
    .vt_equip = 0x4B49698,
    .vt_equip_owner = 0x4BDFE68,
    .ui_mgr_ptr = 0x4C7E098,
    .vt_dpad = 0x4BE1068,
    .vt_hud = 0x4BEB710,
    .vt_map_screen = 0x4BE8300,
    .event_mgr_ptr = 0x4C7E090,
    .event_req_ctor = 0xD79F10,
    .event_req_submit = 0xD7A820,
    .event_req_sub_dtor = 0x20B6F0,
    .event_req_sub_vt = 0x4974B10,
    .vt_linkobj = 0x4B25E18,
    .vt_param = 0x4B63438,
    .disp_ptr = 0x4C7E740,
    .camera_ptr = 0x4E98068,
    .frame_counter = 0x4E84BA0,
    .actor_mgr_ptr = 0x4E90D90,
    .vt_life = 0x4B4C460,
    .vt_cct_property = 0x4C34F10,
    .vt_cct_body = 0x4C34658,
    .cct_force_no_hit = 0x158D40,
    .cct_set_position = 0x6B2530,
    .cct_set_velocity = 0x12DBE0,
    .link_paths = {
      { 0x4E371D0, 0x480 }, { 0x4E94858, 0x2E0 }, { 0x4E935B8, 0xEF8 },
      { 0x4E94860, 0x328 }, { 0x4E94368, 0xD70 },
    },
  },
};

#endif
