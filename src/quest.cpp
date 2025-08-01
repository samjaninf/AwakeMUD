/* *************************************************************************
*    file: quest.cc                                                        *
*    author: Andrew Hynek                                                  *
*    purpose: contains all autoquest functions and the online quest editor *
*    Copyright (c) 1997, 1998 by Andrew Hynek                              *
*    Copyright (c) 2001 The AwakeMUD Consortium                            *
************************************************************************* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <vector>
#include <algorithm>

#include "structs.hpp"
#include "awake.hpp"
#include "utils.hpp"
#include "comm.hpp"
#include "interpreter.hpp"
#include "handler.hpp"
#include "db.hpp"
#include "quest.hpp"
#include "dblist.hpp"
#include "olc.hpp"
#include "screen.hpp"
#include "boards.hpp"
#include "constants.hpp"
#include "newmatrix.hpp"
#include "config.hpp"
#include "bullet_pants.hpp"

extern bool memory(struct char_data *ch, struct char_data *vict);
extern class objList ObjList;
extern struct time_info_data time_info;
extern int olc_state;
extern bool perform_give(struct char_data *ch, struct char_data *vict,
                           struct obj_data *obj);
extern void add_follower(struct char_data *ch, struct char_data *leader);
extern void free_quest(struct quest_data *quest);
extern bool resize_qst_array(void);
extern char *prep_string_for_writing_to_savefile(char *, const char *);
extern int perform_drop(struct char_data * ch, struct obj_data * obj, byte mode,
                        const char *sname, struct room_data *random_donation_room);

unsigned int get_johnson_overall_max_rep(struct char_data *johnson);
unsigned int get_johnson_overall_min_rep(struct char_data *johnson);
void display_single_emote_for_quest(struct char_data *johnson, emote_t emote_to_display, struct char_data *target);

rnum_t translate_quest_mob_identifier_to_rnum(vnum_t identifier, struct quest_data *quest);
vnum_t translate_quest_mob_identifier_to_vnum(vnum_t identifier, struct quest_data *quest);
struct char_data * fetch_quest_mob_target_mob_proto(struct quest_data *qst, int mob_idx);
struct char_data * fetch_quest_mob_actual_mob_proto(struct quest_data *qst, int mob_idx);

ACMD_CONST(do_say);
ACMD_DECLARE(do_action);
SPECIAL(johnson);
ACMD_DECLARE(do_new_echo);

#define QUEST          d->edit_quest

#define DELETE_ENTRY_FROM_VECTOR_PTR(iterator, vector_ptr) {delete [] *(iterator); *(iterator) = NULL; (vector_ptr)->erase((iterator));}

#define LEVEL_REQUIRED_TO_ADD_ITEM_REWARDS  LVL_BUILDER

const char *obj_loads[] =
  {
    "Do not load",
    "Give item to Johnson (who gives it to quester at start)",
    "Give item to a target",
    "Equip item on a target",
    "Install item in a target",
    "Load at a location",
    "Load at host",
    "\n"
  };

const char *mob_loads[] =
  {
    "Do not load",
    "Load at a location",
    "Load at/follow quester",
    "\n"
  };

const char *obj_objectives[] =
  {
    "No objective",
    "Return item to Johnson",
    "Deliver item to a target",
    "Deliver item to location",
    "Destroy an item",
    "Destroy as many items as possible",
    "Return Paydata to Johnson",
    "Upload to a host",
    "\n"
  };

const char *mob_objectives[] =
  {
    "No objective",
    "Escort target to a location",
    "Kill target",
    "Kill as many targets as possible",
    "Target hunts a different quest target",
    "Do not kill",
    "\n"
  };


const char *sol[] =
  {
    "do not load",
    "give to PC",
    "give to target",
    "equip on target",
    "install in target",
    "load at location",
    "load in host",
    "\n"
  };

const char *sml[] =
  {
    "DNL",
    "Location",
    "Follow quester",
    "\n"
  };

const char *soo[] =
  {
    "none",
    "return to Johnson",
    "deliver to target",
    "deliver to location",
    "destroy one",
    "destroy many",
    "return paydata",
    "upload to host",
    "\n"
  };

const char *smo[] =
  {
    "none",
    "escort",
    "kill one",
    "kill many",
    "hunt escortee",
    "\n"
  };

void end_quest(struct char_data *ch, bool succeeded);

void initialize_quest_for_ch(struct char_data *ch, int quest_rnum, struct char_data *johnson) {
  // Assign them the quest.
  GET_QUEST(ch) = quest_rnum;
  GET_QUEST_STARTED(ch) = time(0);

  // Create their memory structures.
  ch->player_specials->obj_complete = new sh_int[quest_table[GET_QUEST(ch)].num_objs];
  ch->player_specials->mob_complete = new sh_int[quest_table[GET_QUEST(ch)].num_mobs];
  for (int num = 0; num < quest_table[GET_QUEST(ch)].num_objs; num++)
    ch->player_specials->obj_complete[num] = 0;
  for (int num = 0; num < quest_table[GET_QUEST(ch)].num_mobs; num++)
    ch->player_specials->mob_complete[num] = 0;

  // Load up the quest's targets.
  load_quest_targets(johnson, ch);

  // Clear the Johnson's SPARE1 so they're willing to talk.
  GET_SPARE1(johnson) = 0;

  // Start the Johnson's spiel.
  act("^n", FALSE, johnson, 0, 0, TO_ROOM);
  handle_info(johnson, quest_rnum, ch);
}

bool attempt_quit_job(struct char_data *ch, struct char_data *johnson) {
  // Precondition: I cannot be talking right now.
  if (GET_SPARE1(johnson) == 0) {
    if (!memory(johnson, ch)) {
      do_say(johnson, "Hold on, I'm talking to someone else right now.", 0, 0);
      return TRUE;
    } else {
      do_say(johnson, "I'm lookin' for a yes-or-no answer, chummer.", 0, 0);
      return TRUE;
    }
  }

  // Precondition: You must be on a quest.
  if (!GET_QUEST(ch)) {
    do_say(johnson, "You're not even on a run right now.", 0, 0);
    return TRUE;
  }

  // Precondition: You must have gotten the quest from me.
  if (!memory(johnson, ch)) {
    do_say(johnson, "Whoever you got your job from, it wasn't me. What, do we all look alike to you?", 0 , 0);
    send_to_char("^L(OOC note: You can hit RECAP to see who gave you your current job.)^n\r\n", ch);
    return TRUE;
  }

  // Drop the quest.
  if (quest_table[GET_QUEST(ch)].quit_emote && *quest_table[GET_QUEST(ch)].quit_emote) {
    // Don't @ me about this, it's the only way to reliably display a newline in this context.
    act("^n", FALSE, johnson, 0, 0, TO_ROOM);
    char emote_with_carriage_return[MAX_STRING_LENGTH];
    snprintf(emote_with_carriage_return, sizeof(emote_with_carriage_return), "%s\r\n", quest_table[GET_QUEST(ch)].quit_emote);
    display_single_emote_for_quest(johnson, emote_with_carriage_return, ch);
  }
  else if (quest_table[GET_QUEST(ch)].quit)
    do_say(johnson, quest_table[GET_QUEST(ch)].quit, 0, 0);
  else {
    snprintf(buf, sizeof(buf), "WARNING: Null string in quest %ld!", quest_table[GET_QUEST(ch)].vnum);
    mudlog(buf, ch, LOG_SYSLOG, TRUE);
    do_say(johnson, "Fine.", 0, 0);
  }

  end_quest(ch, FALSE);
  forget(johnson, ch);
  return TRUE;
}

struct obj_data *instantiate_quest_object(rnum_t rnum, int load_reason, struct char_data *questor) {
  struct obj_data *obj = read_object(rnum, REAL, load_reason);
  GET_OBJ_QUEST_CHAR_ID(obj) = GET_IDNUM_EVEN_IF_PROJECTING(questor);
  obj->obj_flags.extra_flags.SetBits(ITEM_EXTRA_NODONATE, ITEM_EXTRA_NORENT, ITEM_EXTRA_NOSELL, ENDBIT);
  return obj;
}

void load_quest_targets(struct char_data *johnson, struct char_data *ch)
{
  struct obj_data *obj;
  struct char_data *mob;
  int i, j, room = -1, pos, num = GET_QUEST(ch), rnum = -1;

  for (i = 0; i < quest_table[num].num_mobs; i++)
  {
    if (quest_table[num].mob[i].load == QML_LOCATION &&
        (rnum = real_mobile(quest_table[num].mob[i].vnum)) > -1 &&
        (room = real_room(quest_table[num].mob[i].l_data)) > -1)
    {
      mob = read_mobile(rnum, REAL);
      mob->mob_specials.quest_id = GET_IDNUM(ch);
      mob->mob_loaded_in_room = GET_ROOM_VNUM(&world[room]);
      char_to_room(mob, &world[room]);
      act("$n has arrived.", TRUE, mob, 0, 0, TO_ROOM);
      if(quest_table[num].mob[i].objective == QMO_LOCATION)
        add_follower(mob, ch);
      for (j = 0; j < quest_table[num].num_objs; j++)
        if (quest_table[num].obj[j].l_data == i &&
            (rnum = real_object(quest_table[num].obj[j].vnum)) > -1) {
          switch (quest_table[num].obj[j].load) {
          case QOL_TARMOB_I:
            obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_I, ch);
            obj_to_char(obj, mob);
            break;
          case QOL_TARMOB_E:
            pos = quest_table[num].obj[j].l_data2;
            if (pos >= 0 && pos < NUM_WEARS && (!GET_EQ(mob, pos) ||
                                                (pos == WEAR_WIELD && !GET_EQ(mob, WEAR_HOLD)))) {
              obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_E, ch);
              equip_char(mob, obj, pos);

              // Could be a weapon-- make sure it's loaded if it is.
              if (GET_OBJ_TYPE(obj) == ITEM_WEAPON && WEAPON_IS_GUN(obj)) {
                // If it's carried by an NPC, make sure it's loaded.
                if (GET_WEAPON_MAX_AMMO(obj) > 0) {
                  // Reload from their ammo.
                  for (int index = 0; index < NUM_AMMOTYPES; index++) {
                    if (GET_BULLETPANTS_AMMO_AMOUNT(mob, GET_WEAPON_ATTACK_TYPE(obj), npc_ammo_usage_preferences[index]) > 0) {
                      reload_weapon_from_bulletpants(mob, obj, npc_ammo_usage_preferences[index]);
                      break;
                    }
                  }

                  // If they failed to reload, they have no ammo. Give them some normal and reload with it.
                  if (!obj->contains || GET_MAGAZINE_AMMO_COUNT(obj->contains) == 0) {
                    GET_BULLETPANTS_AMMO_AMOUNT(mob, GET_WEAPON_ATTACK_TYPE(obj), AMMO_NORMAL) = GET_WEAPON_MAX_AMMO(obj) * NUMBER_OF_MAGAZINES_TO_GIVE_TO_UNEQUIPPED_MOBS;
                    reload_weapon_from_bulletpants(mob, obj, AMMO_NORMAL);

                    // Decrement their debris-- we want this reload to not create clutter.
                    get_ch_in_room(mob)->debris--;
                  }
                }

                // Set the firemode.
                if (IS_SET(GET_WEAPON_POSSIBLE_FIREMODES(obj), 1 << MODE_BF)) {
                  GET_WEAPON_FIREMODE(obj) = MODE_BF;
                } else if (IS_SET(GET_WEAPON_POSSIBLE_FIREMODES(obj), 1 << MODE_FA)) {
                  GET_WEAPON_FIREMODE(obj) = MODE_FA;
                  GET_OBJ_TIMER(obj) = 10;
                } else if (IS_SET(GET_WEAPON_POSSIBLE_FIREMODES(obj), 1 << MODE_SA)) {
                  GET_WEAPON_FIREMODE(obj) = MODE_SA;
                } else if (IS_SET(GET_WEAPON_POSSIBLE_FIREMODES(obj), 1 << MODE_SS)) {
                  GET_WEAPON_FIREMODE(obj) = MODE_SS;
                }
              }
              // Weapon is loaded, and the mob has its ammo.
            }
            break;
          case QOL_TARMOB_C:
            obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_C, ch);
            if (GET_OBJ_TYPE(obj) == ITEM_CYBERWARE &&
                GET_ESS(mob) > GET_OBJ_VAL(obj, 1)) {
              obj_to_cyberware(obj, mob);
            } else if (GET_OBJ_TYPE(obj) == ITEM_BIOWARE &&
                       GET_INDEX(mob) > GET_OBJ_VAL(obj, 1)) {
              obj_to_bioware(obj, mob);
            } else
              extract_obj(obj);
            break;
          }
          obj = NULL;
        }
      mob = NULL;
    }
    else if (quest_table[num].mob[i].load == QML_FOLQUESTER &&
               (rnum = real_mobile(quest_table[num].mob[i].vnum)) > -1)
    {
      mob = read_mobile(rnum, REAL);
      mob->mob_specials.quest_id = GET_IDNUM(ch);
      mob->mob_loaded_in_room = GET_ROOM_VNUM(ch->in_room);
      char_to_room(mob, ch->in_room);
      act("$n has arrived.", TRUE, mob, 0, 0, TO_ROOM);
      for (j = 0; j < quest_table[num].num_objs; j++)
        if (quest_table[num].obj[j].l_data == i &&
            (rnum = real_object(quest_table[num].obj[j].vnum)) > -1) {
          switch (quest_table[num].obj[j].load) {
          case QOL_TARMOB_I:
            obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_I, ch);
            obj_to_char(obj, mob);
            break;
          case QOL_TARMOB_E:
            pos = quest_table[num].obj[j].l_data2;
            if (pos >= 0 && pos < NUM_WEARS && (!GET_EQ(mob, pos) ||
                                                (pos == WEAR_WIELD && !GET_EQ(mob, WEAR_HOLD)))) {
              obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_E, ch);
              equip_char(mob, obj, pos);
            }
            break;
          case QOL_TARMOB_C:
            obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_TARMOB_C, ch);
            if (GET_OBJ_TYPE(obj) == ITEM_CYBERWARE &&
                GET_ESS(mob) > GET_OBJ_VAL(obj, 1)) {
              obj_to_cyberware(obj, mob);
            } else if (GET_OBJ_TYPE(obj) == ITEM_BIOWARE &&
                       GET_INDEX(mob) > GET_OBJ_VAL(obj, 1)) {
              obj_to_bioware(obj, mob);
            } else
              extract_obj(obj);
            break;
          }
          obj = NULL;
        }
      add_follower(mob, ch);
      mob = NULL;
    }
  }

  for (i = 0; i < quest_table[num].num_objs; i++)
    if ((rnum = real_object(quest_table[num].obj[i].vnum)) > -1)
      switch (quest_table[num].obj[i].load)
      {
      case QOL_LOCATION:
        if ((room = real_room(quest_table[num].obj[i].l_data)) > -1) {
          obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_LOCATION, ch);
          obj_to_room(obj, &world[room]);
        }
        obj = NULL;
        break;
      case QOL_HOST:
        if ((room = real_host(quest_table[num].obj[i].l_data)) > -1) {
          obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_HOST, ch);
          GET_DECK_ACCESSORY_FILE_FOUND_BY(obj) = GET_IDNUM(ch);
          GET_DECK_ACCESSORY_FILE_REMAINING(obj) = 1;
          obj_to_host(obj, &matrix[room]);
        }
        obj = NULL;
        break;
      case QOL_JOHNSON:
        obj = instantiate_quest_object(rnum, OBJ_LOAD_REASON_QUEST_JOHNSON, ch);
        obj_to_char(obj, johnson);
        if (!perform_give(johnson, ch, obj)) {
          char buf[512];
          snprintf(buf, sizeof(buf), "Looks like your hands are full. You'll need %s for the run.", decapitalize_a_an(obj->text.name));
          do_say(johnson, buf, 0, 0);
          perform_drop(johnson, obj, SCMD_DROP, "drop", NULL);
        }
        obj = NULL;
        break;
      }
}

void extract_quest_targets(idnum_t questor_idnum)
{
  struct obj_data *obj, *next_obj;
  int i;

  {
    bool should_loop = TRUE;
    int loop_counter = 0;
    int loop_rand = rand();

    while (should_loop) {
      should_loop = FALSE;
      loop_counter++;

      for (struct char_data *mob = character_list; mob; mob = mob->next_in_character_list) {
        if (mob->last_loop_rand == loop_rand) {
          continue;
        } else {
          mob->last_loop_rand = loop_rand;
        }

        if (IS_NPC(mob) && mob->mob_specials.quest_id == questor_idnum) {
          for (obj = mob->carrying; obj; obj = next_obj) {
            next_obj = obj->next_content;
            extract_obj(obj);
          }
          for (i = 0; i < NUM_WEARS; i++)
            if (GET_EQ(mob, i))
              extract_obj(GET_EQ(mob, i));

          // We extracted a character, so start over.
          act("$n slips away quietly.", FALSE, mob, 0, 0, TO_ROOM);
          extract_char(mob);
          should_loop = TRUE;
          break;
        }
      }

      if (loop_counter > 1) {
        // mudlog_vfprintf(NULL, LOG_SYSLOG, "Looped %d times over extract_quest_targets().", loop_counter);
      }
    }
  }

  ObjList.RemoveQuestObjs(questor_idnum);
}

bool is_escortee(struct char_data *mob)
{
  int i;

  if (!IS_NPC(mob) || !mob->master || IS_NPC(mob->master) || !GET_QUEST(mob->master))
    return FALSE;

  for (i = 0; i < quest_table[GET_QUEST(mob->master)].num_mobs; i++)
    if (quest_table[GET_QUEST(mob->master)].mob[i].vnum == GET_MOB_VNUM(mob))
      if (quest_table[GET_QUEST(mob->master)].mob[i].objective == QMO_LOCATION)
        return TRUE;

  return FALSE;
}

bool hunting_escortee(struct char_data *ch, struct char_data *vict)
{
  int i, num;

  if (!IS_NPC(ch) || !is_escortee(vict))
    return FALSE;

  // Only attack the escortees of the person with your specific quest.
  if (GET_MOB_QUEST_CHAR_ID(ch) != GET_MOB_QUEST_CHAR_ID(vict))
    return FALSE;

  num = GET_QUEST(vict->master);

  for (i = 0; i < quest_table[num].num_mobs; i++) {
    if (quest_table[num].mob[i].vnum == GET_MOB_VNUM(ch) &&
        quest_table[num].mob[i].objective == QMO_KILL_ESCORTEE &&
        quest_table[num].mob[quest_table[num].mob[i].o_data].vnum == GET_MOB_VNUM(vict))
      return TRUE;
  }

  return FALSE;
}

bool _raw_check_quest_delivery(struct char_data *ch, struct char_data *mob, struct obj_data *obj, bool commit_changes=TRUE) {
  if (!GET_QUEST(ch))
    return FALSE;

  // Scan through all defined objects with objectives in this quest.
  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++) {
    // Check to see if this specific object objective matches the item's vnum.
    if (quest_table[GET_QUEST(ch)].obj[i].vnum == GET_OBJ_VNUM(obj)) {
      switch (quest_table[GET_QUEST(ch)].obj[i].objective) {
        case QOO_JOHNSON:
          if (GET_MOB_SPEC(mob) && (GET_MOB_SPEC(mob) == johnson || GET_MOB_SPEC2(mob) == johnson) && memory(mob, ch)) {
            if (commit_changes)
              ch->player_specials->obj_complete[i] = 1;
            return TRUE;
          }
          break;
        case QOO_TAR_MOB:
          {
            vnum_t mob_vnum = translate_quest_mob_identifier_to_vnum(quest_table[GET_QUEST(ch)].obj[i].o_data, &quest_table[GET_QUEST(ch)]);
            if (mob_vnum == GET_MOB_VNUM(mob)) {
              if (commit_changes)
                ch->player_specials->obj_complete[i] = 1;
              return TRUE;
            }
          }
          break;
        case QOO_RETURN_PAY:
          if (GET_MOB_SPEC(mob) && (GET_MOB_SPEC(mob) == johnson || GET_MOB_SPEC2(mob) == johnson) && memory(mob, ch)) {
            if (GET_DECK_ACCESSORY_FILE_HOST_VNUM(obj) == quest_table[GET_QUEST(ch)].obj[i].o_data) {
              if (commit_changes)
                ch->player_specials->obj_complete[i] = 1;
              return TRUE;
            }
          }
          break;
      }
    }
  }

  return FALSE;
}

bool _could_quest_deliver(struct char_data *ch, struct char_data *mob, struct obj_data *obj) {
  return _raw_check_quest_delivery(ch, mob, obj, FALSE);
}

bool check_quest_delivery(struct char_data *ch, struct char_data *mob, struct obj_data *obj)
{
  if (!ch || !mob || !obj) {
    mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Received (%s, %s, %s) to non-nullable check_quest_delivery()!",
                    ch ? GET_CHAR_NAME(ch) : "NULL",
                    mob ? GET_CHAR_NAME(mob) : "NULL",
                    obj ? GET_OBJ_NAME(obj) : "NULL");
    return FALSE;
  }

  if (IS_NPC(ch) || !IS_NPC(mob))
    return FALSE;

  if (_raw_check_quest_delivery(ch, mob, obj))
    return TRUE;

  if (AFF_FLAGGED(ch, AFF_GROUP)) {
    // Followers
    for (struct follow_type *f = ch->followers; f; f = f->next) {
      if (IS_NPC(f->follower) || !AFF_FLAGGED(f->follower, AFF_GROUP))
        continue;

      // If they're not going to benefit from a quest delivery, skip.
      if (!_could_quest_deliver(f->follower, mob, obj))
        continue;

      // If they would have benefited from this delivery, but aren't in room, then we bail completely.
      if (f->follower->in_room != ch->in_room) {
        send_to_char(ch, "%s must be present for this delivery.\r\n", GET_CHAR_NAME(f->follower));
        return FALSE;
      }

      // This is technically a sanity check, in that we *expect* this to work.
      if (_raw_check_quest_delivery(f->follower, mob, obj)) {
        return TRUE;
      } else {
        mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Sanity check failed in check_quest_delivery(%s, %s, %s)!", GET_CHAR_NAME(ch), GET_CHAR_NAME(mob), GET_OBJ_NAME(obj));
      }
    }

    // Master
    if (ch->master && !IS_NPC(ch->master) && _raw_check_quest_delivery(ch->master, mob, obj)) {
      return TRUE;
    }
  }

  return FALSE;
}

// Checks if this successfully completed a quest step. Note the lack of false returns in the loop, this is on purpose to allow for multiple quest objectives to have the same object vnum!
bool _raw_check_quest_delivery(struct char_data *ch, struct obj_data *obj, struct room_data *in_room, bool commit_changes=TRUE) {
  if (!GET_QUEST(ch))
    return FALSE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++) {
    if (GET_OBJ_VNUM(obj) != quest_table[GET_QUEST(ch)].obj[i].vnum)
      continue;

    // QOO_LOCATION, in right location? True.
    if (quest_table[GET_QUEST(ch)].obj[i].objective == QOO_LOCATION) {
      if (in_room->number == quest_table[GET_QUEST(ch)].obj[i].o_data) {
        if (commit_changes)
          ch->player_specials->obj_complete[i] = 1;
        return TRUE;
      }
    }
    // QOO_UPLOAD, in right host? True.
    else if (quest_table[GET_QUEST(ch)].obj[i].objective == QOO_UPLOAD) {
      if (ch->persona && ch->persona->in_host && matrix[ch->persona->in_host].vnum == quest_table[GET_QUEST(ch)].obj[i].o_data) {
        if (commit_changes)
          ch->player_specials->obj_complete[i] = 1;
        return TRUE;
      }
    }
  }

  return FALSE;
}

bool _could_quest_deliver(struct char_data *ch, struct obj_data *obj, struct room_data *in_room) {
  return _raw_check_quest_delivery(ch, obj, in_room, FALSE);
}

bool check_quest_delivery(struct char_data *ch, struct obj_data *obj)
{
  struct veh_data *veh;
  struct room_data *in_room;

  if (!ch || !obj) {
    mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Received (%s, %s) to non-nullable check_quest_delivery()!",
                    ch ? GET_CHAR_NAME(ch) : "NULL",
                    obj ? GET_OBJ_NAME(obj) : "NULL");
    return FALSE;
  }

  if (IS_NPC(ch))
    return FALSE;

  RIG_VEH(ch, veh);

  if (veh) {
    in_room = veh->in_room;
  } else {
    in_room = ch->in_room;
  }

  if (!in_room) {
    // You can't complete a quest objective from a vehicle.
    return FALSE;
  }

  if (_raw_check_quest_delivery(ch, obj, in_room))
    return TRUE;

  if (AFF_FLAGGED(ch, AFF_GROUP)) {
    // Followers
    for (struct follow_type *f = ch->followers; f; f = f->next) {
      if (IS_NPC(f->follower) || !AFF_FLAGGED(f->follower, AFF_GROUP))
        continue;

      // If they're not going to benefit from a quest delivery, skip.
      if (!_could_quest_deliver(f->follower, obj, in_room))
        continue;

      if (f->follower->in_room != in_room) {
        send_to_char(ch, "%s must be present for this delivery.\r\n", GET_CHAR_NAME(f->follower));
        return FALSE;
      }

      if (_raw_check_quest_delivery(f->follower, obj, in_room)) {
        return TRUE;
      } else {
        mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Sanity check failed in check_quest_delivery(%s, %s)!", GET_CHAR_NAME(ch), GET_OBJ_NAME(obj));
      }
    }

    // Master
    if (ch->master && !IS_NPC(ch->master) && _raw_check_quest_delivery(ch->master, obj, in_room)) {
      return TRUE;
    }
  }

  return FALSE;
}

bool _raw_check_quest_destination(struct char_data *ch, struct char_data *mob) {
  if (!GET_QUEST(ch))
    return FALSE;

  if (mob->mob_specials.quest_id != GET_IDNUM(ch))
    return FALSE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++) {
    if (quest_table[GET_QUEST(ch)].mob[i].objective == QMO_LOCATION
        && mob->in_room->number == quest_table[GET_QUEST(ch)].mob[i].o_data
        && GET_MOB_VNUM(mob) == quest_table[GET_QUEST(ch)].mob[i].vnum)
    {
      ch->player_specials->mob_complete[i] = 1;
      stop_follower(mob);
      do_say(mob, "Thanks for the escort.", 0, 0);
      return TRUE;
    }
  }

  return FALSE;
}

bool check_quest_destination(struct char_data *ch, struct char_data *mob) {
  if (!ch || !mob) {
    mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Received (%s, %s) to non-nullable check_quest_destination()!",
                    ch ? GET_CHAR_NAME(ch) : "NULL",
                    mob ? GET_CHAR_NAME(mob) : "NULL");
    return FALSE;
  }

  if (IS_NPC(ch) || !IS_NPC(mob))
    return FALSE;

  if (_raw_check_quest_destination(ch, mob))
    return TRUE;

  // Technically, none of the code below will ever return TRUE-- this function only triggers when a mob follows you into a room,
  // and the quest target would only be following the original questor. Included it anyways for completeness's sake.
  if (AFF_FLAGGED(ch, AFF_GROUP)) {
    // Followers
    for (struct follow_type *f = ch->followers; f; f = f->next) {
      if (IS_NPC(f->follower) || !AFF_FLAGGED(f->follower, AFF_GROUP))
        continue;

      if (_raw_check_quest_destination(f->follower, mob)) {
        return TRUE;
      }
    }

    // Master
    if (ch->master && !IS_NPC(ch->master) && _raw_check_quest_destination(ch->master, mob)) {
      return TRUE;
    }
  }

  return FALSE;
}

bool _raw_check_quest_destroy(struct char_data *ch, struct obj_data *obj) {
  if (!GET_QUEST(ch))
    return FALSE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++) {
    if (GET_OBJ_VNUM(obj) == quest_table[GET_QUEST(ch)].obj[i].vnum) {
      switch (quest_table[GET_QUEST(ch)].obj[i].objective) {
        case QOO_DSTRY_ONE:
        case QOO_DSTRY_MANY:
          ch->player_specials->obj_complete[i]++;
          if (access_level(ch, LVL_BUILDER)) {
            send_to_char(ch, "[check_quest_destroy for %s: +1 completion for a total of %d.]\r\n", GET_OBJ_NAME(obj), ch->player_specials->obj_complete[i]);
          }
          return TRUE;
      }
    }
  }

  if (access_level(ch, LVL_BUILDER)) {
    send_to_char(ch, "[check_quest_destroy for %s: did not count]\r\n", GET_OBJ_NAME(obj));
  }

  return FALSE;
}

bool check_quest_destroy(struct char_data *ch, struct obj_data *obj) {
  if (!ch || !obj) {
    mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Received (%s, %s) to non-nullable check_quest_destroy()!",
                    ch ? GET_CHAR_NAME(ch) : "NULL",
                    obj ? GET_OBJ_NAME(obj) : "NULL");
    return FALSE;
  }

  if (IS_NPC(ch))
    return FALSE;

  if (_raw_check_quest_destroy(ch, obj))
    return TRUE;

  if (AFF_FLAGGED(ch, AFF_GROUP)) {
    // Followers
    for (struct follow_type *f = ch->followers; f; f = f->next) {
      if (IS_NPC(f->follower) || !AFF_FLAGGED(f->follower, AFF_GROUP))
        continue;

      if (_raw_check_quest_destroy(f->follower, obj)) {
        return TRUE;
      }
    }

    // Master
    if (ch->master && !IS_NPC(ch->master) && _raw_check_quest_destroy(ch->master, obj)) {
      return TRUE;
    }
  }

  return FALSE;
}

#ifdef IS_BUILDPORT
#define QUEST_DEBUG(msg) act(msg, FALSE, ch, 0, victim, TO_ROLLS);
#else
#define QUEST_DEBUG(msg)
#endif

bool _raw_check_quest_kill(struct char_data *ch, struct char_data *victim) {
  if (!GET_QUEST(ch) || !ch->player_specials)
    return FALSE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++) {
    if (ch->player_specials->mob_complete[i] == -1) {
      // They've failed their quest by killing a QMO_DONT_KILL: Bail out, we don't want to process further.
      return FALSE;
    }

    if (GET_MOB_VNUM(victim) == GET_MOB_VNUM(fetch_quest_mob_actual_mob_proto(&quest_table[GET_QUEST(ch)], i))) {
      switch (quest_table[GET_QUEST(ch)].mob[i].objective)
      {
      case QMO_KILL_ONE:
        QUEST_DEBUG("^L_raw_check_quest_kill($n, $N): +1 (only one)^n");
        // If we've already completed this objective, continue.
        if (ch->player_specials->mob_complete[i] >= 1)
          continue;
        // Otherwise, mark it as done.
        ch->player_specials->mob_complete[i]++;
        return TRUE;
      case QMO_KILL_MANY:
        QUEST_DEBUG("^L_raw_check_quest_kill($n, $N): +1 (many)^n");
        ch->player_specials->mob_complete[i]++;
        return TRUE;
      case QMO_DONT_KILL:
        QUEST_DEBUG("^L_raw_check_quest_kill($n, $N): qmo_dont_kill, failed quest^n");
        ch->player_specials->mob_complete[i] = -1;
        send_to_char(ch, "^rJust a moment too late, you remember that you weren't supposed to kill %s^r...^n\r\n", GET_CHAR_NAME(victim));
        return FALSE;
      }
    }
  }
  QUEST_DEBUG("^L_raw_check_quest_kill($n, $N): didn't count for quest^n");
  return FALSE;
}

bool _subsection_check_quest_kill(struct char_data *ch, struct char_data *victim) {
  // You can only share quest ticks if you're grouped. Summoned NPCs are immune to this requirement.
  if (!AFF_FLAGGED(ch, AFF_GROUP) && !IS_NPNPC(ch)) {
    return FALSE;
  }

  // Followers (both projected and not)
  for (struct follow_type *f = ch->followers; f; f = f->next) {
    if (!AFF_FLAGGED(f->follower, AFF_GROUP) && !IS_NPNPC(ch)) {
      continue;
    }

    // Check the follower itself.
    if (_raw_check_quest_kill(f->follower, victim)) {
      return TRUE;
    }

    // Check the follower's original body, if one exists.
    if (f->follower->desc && f->follower->desc->original && _raw_check_quest_kill(f->follower->desc->original, victim)) {
      return TRUE;
    }
  }

  // Master (both projected and not)
  if (ch->master) {
    if (!AFF_FLAGGED(ch->master, AFF_GROUP)) {  // No NPNPC check here since the master should never be an elemental etc.
      return FALSE;
    }

    // Check the master itself.
    if (_raw_check_quest_kill(ch->master, victim)) {
      return TRUE;
    }

    // Check the master's original body, if one exists.
    if (ch->master->desc && ch->master->desc->original && _raw_check_quest_kill(ch->master->desc->original, victim)) {
      return TRUE;
    }
  }

  // Nobody benefited.
  return FALSE;
}

bool check_quest_kill(struct char_data *ch, struct char_data *victim)
{
  if (!ch || !victim) {
    mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Received (%s, %s) to non-nullable check_quest_kill()!",
                    ch ? GET_CHAR_NAME(ch) : "NULL",
                    victim ? GET_CHAR_NAME(victim) : "NULL");
    return FALSE;
  }

  struct char_data *original_ch = ch->desc && ch->desc->original ? ch->desc->original : ch;

  // We can't get quest points for killing players.
  if (!IS_NPC(victim))
    return FALSE;

  // Did this meet our own quest objective?
  if (_raw_check_quest_kill(original_ch, victim))
    return TRUE;

  // Did this meet the quest objective of a grouped follower or leader?
  if (_subsection_check_quest_kill(ch, victim))
    return TRUE;

  // If we're projecting, did this meet the quest objective of a meatform grouped follower or leader?
  if (original_ch != ch && _subsection_check_quest_kill(original_ch, victim))
    return TRUE;

  return FALSE;
}

void end_quest(struct char_data *ch, bool succeeded)
{
  if (IS_NPC(ch) || !GET_QUEST(ch))
    return;

  extract_quest_targets(GET_IDNUM_EVEN_IF_PROJECTING(ch));
  // We mark the quest as completed here because if you fail...
  //well you failed. Better luck next time chummer.
  for (int i = QUEST_TIMER - 1; i > 0; i--) {
    GET_LQUEST(ch, i) = GET_LQUEST(ch, i - 1);

    if (succeeded)
      GET_CQUEST(ch, i) = GET_CQUEST(ch, i - 1);
  }

  GET_LQUEST(ch, 0) = quest_table[GET_QUEST(ch)].vnum;
  if (succeeded)
    GET_CQUEST(ch, 0) = quest_table[GET_QUEST(ch)].vnum;

  GET_QUEST(ch) = 0;
  GET_QUEST_STARTED(ch) = 0;

  delete [] ch->player_specials->mob_complete;
  delete [] ch->player_specials->obj_complete;
  ch->player_specials->mob_complete = NULL;
  ch->player_specials->obj_complete = NULL;

  GET_QUEST_DIRTY_BIT(ch) = TRUE;
}

bool rep_too_high(struct char_data *ch, int num)
{
  if (num < 0 || num > top_of_questt)
    return TRUE;

  if (quest_table[num].max_rep < 10000 && GET_REP(ch) > quest_table[num].max_rep)
    return TRUE;

  return FALSE;
}

bool rep_too_low(struct char_data *ch, int num)
{
  if (num < 0 || num > top_of_questt)
    return TRUE;

  if (GET_REP(ch) < quest_table[num].min_rep)
    return TRUE;

  return FALSE;
}

bool would_be_rewarded_for_turnin(struct char_data *ch) {
  int nuyen = 0, karma = 0;

  bool all = TRUE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++)
    if (ch->player_specials->obj_complete[i]) {
      if (quest_table[GET_QUEST(ch)].obj[i].objective == QOO_DSTRY_MANY) {
        nuyen += quest_table[GET_QUEST(ch)].obj[i].nuyen * ch->player_specials->obj_complete[i];
        karma += quest_table[GET_QUEST(ch)].obj[i].karma * ch->player_specials->obj_complete[i];
      } else {
        nuyen += quest_table[GET_QUEST(ch)].obj[i].nuyen;
        karma += quest_table[GET_QUEST(ch)].obj[i].karma;
      }
    } else
      all = FALSE;

  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++)
    if (ch->player_specials->mob_complete[i]) {
      if (quest_table[GET_QUEST(ch)].mob[i].objective == QMO_KILL_MANY) {
        nuyen += quest_table[GET_QUEST(ch)].mob[i].nuyen * ch->player_specials->mob_complete[i];
        karma += quest_table[GET_QUEST(ch)].mob[i].karma * ch->player_specials->mob_complete[i];
      } else {
        nuyen += quest_table[GET_QUEST(ch)].mob[i].nuyen;
        karma += quest_table[GET_QUEST(ch)].mob[i].karma;
      }
    } else
      all = FALSE;

  if (all)
    return TRUE;

  return nuyen > 0 || karma > 0;
}

bool follower_can_receive_reward(struct char_data *follower, struct char_data *leader, bool send_message) {
  // NPCs of various flavors.
  if (!follower || IS_NPC(follower) || IS_PC_CONJURED_ELEMENTAL(follower) || IS_SPIRIT(follower))
    return FALSE;

  // Non-grouped PCs.
  if (!AFF_FLAGGED(follower, AFF_GROUP)) {
    if (send_message) {
      send_to_char(leader, "^y(OOC note: %s wasn't grouped, so didn't get a cut of the pay.)^n\r\n", GET_CHAR_NAME(follower), HSSH(follower));
      send_to_char(follower, "^y(OOC note: You aren't part of %s's group, so you didn't get a cut of the pay.)^n\r\n", GET_CHAR_NAME(leader));
    }
    return FALSE;
  }

  /*  Removed -- this doesn't feel great for the players and isn't well telegraphed beforehand. -LS
  if (rep_too_low(f->follower, GET_QUEST(ch)) || rep_too_high(f->follower, GET_QUEST(ch))) {
    send_to_char(ch, "^y(OOC note: %s didn't meet the qualifications for this run, so %s didn't get a cut of the pay.)^n\r\n", GET_CHAR_NAME(f->follower), HSSH(f->follower));
    send_to_char("^y(OOC note: You didn't meet the qualifications for this run, so you didn't get a cut of the pay.)^n\r\n", f->follower);
    continue;
  }
  */

  if (follower->in_room != leader->in_room) {
    if (send_message) {
      send_to_char(leader, "^y(OOC note: %s wasn't in the room, so didn't get a cut of the pay.)^n\r\n", GET_CHAR_NAME(follower), HSSH(follower));
      send_to_char(follower, "^y(OOC note: You aren't in the Johnson's room, so you didn't get a cut of %s's pay.)^n\r\n", GET_CHAR_NAME(leader));
    }
    return FALSE;
  }

  if (follower->char_specials.timer >= IDLE_TIMER_PAYOUT_THRESHOLD || PRF_FLAGGED(follower, PRF_AFK)) {
    if (send_message) {
      send_to_char(leader, "^y(OOC note: %s is idle/AFK, so didn't get a cut of the pay.)^n\r\n", GET_CHAR_NAME(follower), HSSH(follower));
      send_to_char(follower, "^y(OOC note: You are idle/AFK, so you didn't get a cut of %s's pay.)^n\r\n", GET_CHAR_NAME(leader));
    }
    return FALSE;
  }

  return TRUE;
}

void award_follower_payout(struct char_data *follower, int karma, int nuyen, struct char_data *questor) {
  gain_nuyen(follower, nuyen, NUYEN_INCOME_AUTORUNS);
  int gained = gain_karma(follower, karma, TRUE, FALSE, TRUE);
  send_to_char(follower, "You gain %0.2f karma and %d nuyen for being in %s's group.\r\n", (float) gained * 0.01, nuyen, GET_CHAR_NAME(questor));
  mudlog_vfprintf(follower, LOG_GRIDLOG, "%s gains %0.2fk and %dn from job %ld (grouped with %s (%ld))",
                  GET_CHAR_NAME(follower),
                  (float) gained * 0.01,
                  nuyen,
                  GET_QUEST(questor),
                  GET_CHAR_NAME(questor),
                  GET_IDNUM(questor));
}

void reward(struct char_data *ch, struct char_data *johnson)
{
  if (vnum_from_non_approved_zone(quest_table[GET_QUEST(ch)].vnum)) {
#ifdef IS_BUILDPORT
    send_to_char(ch, "This quest's zone is not approved, so no rewards will be assigned for it if it's deployed to main.\r\n");
#else
    send_to_char(ch, "Quest reward suppressed due to this zone not being marked as approved for use in the game world.\r\n");
    end_quest(ch, TRUE);
    return;
#endif
  }

  struct obj_data *obj;
  bool completed_all_objectives = TRUE;
  int nuyen = 0, karma = 0, multiplier;

  // Check object objectives.
  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++) {
    if (quest_table[GET_QUEST(ch)].obj[i].objective == QOO_NO_OBJECTIVE)
      continue;

    if (ch->player_specials->obj_complete[i]) {
      if (quest_table[GET_QUEST(ch)].obj[i].objective == QOO_DSTRY_MANY) {
        multiplier = ch->player_specials->obj_complete[i];
      } else {
        multiplier = 1;
      }
      nuyen += quest_table[GET_QUEST(ch)].obj[i].nuyen * multiplier;
      karma += quest_table[GET_QUEST(ch)].obj[i].karma * multiplier;
    } else {
#ifdef IS_BUILDPORT
      if (1 == 1)
#else
      if (IS_SENATOR(ch))
#endif
      {
        send_to_char(ch, "-- Quest turnin: Did not complete obj objective %d.\r\n", i);
      }
      completed_all_objectives = FALSE;
    }
  }

  // Check mob objectives.
  for (int i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++) {
    if (quest_table[GET_QUEST(ch)].mob[i].objective == QMO_NO_OBJECTIVE || quest_table[GET_QUEST(ch)].mob[i].objective == QMO_KILL_ESCORTEE)
      continue;

    if (ch->player_specials->mob_complete[i]) {
      if (quest_table[GET_QUEST(ch)].mob[i].objective == QMO_KILL_MANY) {
        multiplier = ch->player_specials->mob_complete[i];
      } else {
        multiplier = 1;
      }
      nuyen += quest_table[GET_QUEST(ch)].mob[i].nuyen * multiplier;
      karma += quest_table[GET_QUEST(ch)].mob[i].karma * multiplier;
    } else {
#ifdef IS_BUILDPORT
      if (1 == 1)
#else
      if (IS_SENATOR(ch))
#endif
      {
        send_to_char(ch, "-- Quest turnin: Did not complete mob objective %d.\r\n", i);
      }
      completed_all_objectives = FALSE;
    }
  }

  if (completed_all_objectives) {
    // You get the bonus karma/nuyen for all-objective completion.
    nuyen += quest_table[GET_QUEST(ch)].nuyen;
    karma += quest_table[GET_QUEST(ch)].karma;

    // You also only get the object reward for completing all objectives.
    rnum_t rnum = real_object(quest_table[GET_QUEST(ch)].reward);
    if (rnum > 0) {
      obj = read_object(rnum, REAL, OBJ_LOAD_REASON_QUEST_REWARD);

      // Check to see if they have it on them already.
      if (get_carried_vnum(ch, GET_OBJ_VNUM(obj), FALSE)) {
        act("You already have a copy of $p, so $n doesn't give you another.", FALSE, johnson, obj, ch, TO_VICT);
        extract_obj(obj);
        obj = NULL;
      }
      // You don't have one, so you get one.
      else {
        obj_to_char(obj, ch);
        soulbind_obj_to_char(obj, ch, FALSE);
        if (MOB_FLAGGED(johnson, MOB_INANIMATE)) {
          act("You dispense $p for $N.", FALSE, johnson, obj, ch, TO_CHAR);
          act("$n dispenses $p, and you pick it up.", FALSE, johnson, obj, ch, TO_VICT);
          act("$n dispenses $p for $N.", TRUE, johnson, obj, ch, TO_NOTVICT);
        } else {
          act("You give $p to $N.", FALSE, johnson, obj, ch, TO_CHAR);
          act("$n gives you $p.", FALSE, johnson, obj, ch, TO_VICT);
          act("$n gives $p to $N.", TRUE, johnson, obj, ch, TO_NOTVICT);
        }
      }
    }
  } else {
#ifdef IS_BUILDPORT
    if (1 == 1) {
#else
    if (IS_SENATOR(ch)) {
#endif
      char saybuf[1000];
      snprintf(saybuf, sizeof(saybuf), "%s Partially done's better than nothing.", GET_CHAR_NAME(ch));
      do_say(johnson, saybuf, 0, SCMD_SAYTO);
    }
  }

  nuyen = negotiate(ch, johnson, 0, nuyen, 0, FALSE, FALSE) * NUYEN_GAIN_MULTIPLIER * ((float) GET_CHAR_MULTIPLIER(ch) / 100);

  // If you're grouped, distribute the karma and nuyen equally.
  if (AFF_FLAGGED(ch, AFF_GROUP)) {
    int num_chars_to_give_award_to = 1;
    for (struct follow_type *f = ch->followers; f; f = f->next) {
      // Add the follower to the total OR message them about why they can't be split with.
      if (follower_can_receive_reward(f->follower, ch, TRUE)) {
        num_chars_to_give_award_to++;
      }
    }

    // Add their leader to the total OR message them about why they can't be split with.
    if (ch->master && follower_can_receive_reward(ch->master, ch, TRUE))
      num_chars_to_give_award_to++;

    if (num_chars_to_give_award_to > 1) {
      nuyen = (int)(nuyen / num_chars_to_give_award_to) * GROUP_QUEST_REWARD_MULTIPLIER;
      karma = (int)(karma / num_chars_to_give_award_to) * GROUP_QUEST_REWARD_MULTIPLIER;

      send_to_char("You divide the payout amongst your group.\r\n", ch);

      for (struct follow_type *f = ch->followers; f; f = f->next) {
        // Skip invalid folks WITHOUT sending a message.
        if (!follower_can_receive_reward(f->follower, ch, FALSE))
          continue;

        award_follower_payout(f->follower, karma, nuyen, ch);
      }

      // Skip invalid leaders WITHOUT sending a message.
      if (ch->master && follower_can_receive_reward(ch->master, ch, FALSE)) {
        award_follower_payout(ch->master, karma, nuyen, ch);
      }
    }
  }

  gain_nuyen(ch, nuyen, NUYEN_INCOME_AUTORUNS);
  int gained = gain_karma(ch, karma, TRUE, FALSE, TRUE);
  act("$n gives some nuyen to $N.", TRUE, johnson, 0, ch, TO_NOTVICT);
  act("You give some nuyen to $N.", TRUE, johnson, 0, ch, TO_CHAR);
  snprintf(buf, sizeof(buf), "$n gives you %d nuyen.", nuyen);
  act(buf, FALSE, johnson, 0, ch, TO_VICT);
  send_to_char(ch, "You gain %.2f karma.\r\n", ((float) gained / 100));

  mudlog_vfprintf(ch, LOG_GRIDLOG, "%s gains %0.2fk and %dn from job %ld. Elapsed time v2 %0.2f seconds.",
                  GET_CHAR_NAME(ch),
                  (float) gained * 0.01,
                  nuyen,
                  GET_QUEST(ch),
                  difftime(time(0), GET_QUEST_STARTED(ch)));
  end_quest(ch, TRUE);
}

//Comparator function for sorting quest
bool compareRep(const quest_entry &a, const quest_entry &b)
{
  return (a.rep < b.rep);
}

// New quest function builds a list of quests that exclude already
//done, and outgrown, sorts it by reputation and returns the lowest
//rep one first. It returns 0 if no more quests are available or -1 if
//the johnson is broken.
int new_quest(struct char_data *mob, struct char_data *ch)
{
  int num = 0;
  bool allow_disconnected = vnum_from_non_approved_zone(GET_MOB_VNUM(mob));

  quest_entry temp_entry;
  std::vector<quest_entry> qlist;

  for (int quest_idx = 0; quest_idx <= top_of_questt; quest_idx++)
    if (quest_table[quest_idx].johnson == GET_MOB_VNUM(mob))
      num++;

  if (num < 1) {
    if (mob_index[mob->nr].func == johnson) {
      mob_index[mob->nr].func = mob_index[mob->nr].sfunc;
      mob_index[mob->nr].sfunc = NULL;
    } else if (mob_index[mob->nr].sfunc == johnson) {
      mob_index[mob->nr].sfunc = NULL;
    }
    snprintf(buf, sizeof(buf), "Stripping Johnson status from %s (%ld) due to mob not having any quests to assign.",
             GET_NAME(mob), GET_MOB_VNUM(mob));
    mudlog(buf, NULL, LOG_SYSLOG, true);
    return -1;
  }

  // Build array of quests for this johnson, excluding quests that are already
  // done or max_rep is below character rep. We include those with min_rep
  // higher than character rep because we want johnsons to hint to available
  // runs at higher character rep.
  bool skipped_from_dq = FALSE;
  bool skipped_from_missing_prereq = FALSE;
  for (int quest_idx = 0; quest_idx <= top_of_questt; quest_idx++) {
    if (quest_table[quest_idx].johnson == GET_MOB_VNUM(mob)) {
      if (!allow_disconnected && vnum_from_non_approved_zone(quest_table[quest_idx].vnum)) {
#ifdef IS_BUILDPORT
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "[Quest %ld would be skipped due to non-connected status, but allowing since this is buildport.]\r\n", quest_table[quest_idx].vnum);
        }
#else
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "[Skipping quest %ld: vnum from non-connected zone.]\r\n", quest_table[quest_idx].vnum);
        }
        continue;
#endif
      }

      if (rep_too_high(ch, quest_idx)) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "[Skipping quest %ld: You exceed rep cap of %d.]\r\n", quest_table[quest_idx].vnum, quest_table[quest_idx].max_rep);
        }
        continue;
      }

      if (ch->master && GET_QUEST(ch->master) == quest_idx) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "[Skipping quest %ld: Your group leader %s already has it.]\r\n", quest_table[quest_idx].vnum, GET_CHAR_NAME(ch->master));
        }
        continue;
      }

      bool follower_has_quest = FALSE;
      for (struct follow_type *fd = ch->followers; fd; fd = fd->next) {
        if (fd->follower && GET_QUEST(fd->follower) == quest_idx) {
          if (access_level(ch, LVL_BUILDER)) {
            send_to_char(ch, "[Skipping quest %ld: Your follower %s already has it.]\r\n", quest_table[quest_idx].vnum, GET_CHAR_NAME(fd->follower));
          }
          follower_has_quest = TRUE;
          break;
        }
      }
      if (follower_has_quest)
        continue;

      bool found = FALSE;
      for (int q = QUEST_TIMER - 1; q >= 0; q--) {
        if (GET_LQUEST(ch, q) == quest_table[quest_idx].vnum) {
          found = TRUE;
          break;
        }
      }
      if (found) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "[Skipping quest %ld: It exists in your LQUEST list. Use a diagnostic scanner and ^WCLEANSE^n yourself.]\r\n", quest_table[quest_idx].vnum);
        }
        continue;
      }

      if (quest_table[quest_idx].prerequisite_quest || quest_table[quest_idx].disqualifying_quest) {
        bool prereq_found = FALSE, dq_found = FALSE;
        for (int q = QUEST_TIMER - 1; q >= 0; q--) {
          if (quest_table[quest_idx].prerequisite_quest && GET_CQUEST(ch, q) == quest_table[quest_idx].prerequisite_quest) {
            prereq_found = TRUE;
          }
          if (quest_table[quest_idx].disqualifying_quest && GET_CQUEST(ch, q) == quest_table[quest_idx].disqualifying_quest) {
            dq_found = TRUE;
          }
        }

        if (!prereq_found && rep_too_high(ch, quest_table[quest_idx].prerequisite_quest)) {
          if (access_level(ch, LVL_BUILDER)) {
            send_to_char(ch, "[Skipping quest %ld: You need to have done prerequisite quest %lu first.]\r\n", quest_table[quest_idx].vnum, quest_table[quest_idx].prerequisite_quest);
          }
          skipped_from_missing_prereq = TRUE;
          continue;
        }

        if (dq_found) {
          if (access_level(ch, LVL_BUILDER)) {
            send_to_char(ch, "[Skipping quest %ld: You completed disqualifying quest %lu.]\r\n", quest_table[quest_idx].vnum, quest_table[quest_idx].disqualifying_quest);
          }
          skipped_from_dq = TRUE;
          continue;
        }
      }

      temp_entry.index = quest_idx;
      temp_entry.rep = quest_table[quest_idx].min_rep;
      qlist.push_back(temp_entry);
    }
  }
  // Sort vector by reputation and return a quest if vector is not empty.
  if (!qlist.empty()) {
    sort(qlist.begin(), qlist.end(), compareRep);
    return qlist[0].index;
  } else {
    if (skipped_from_dq || skipped_from_missing_prereq)
      send_to_char("^L(OOC: You've either not completed the prerequisite jobs or have done a disqualifying job recently. Do some more jobs in the area, then come back and try again!)^n\r\n", ch);
  }
  return 0;
}

void display_single_emote_for_quest(struct char_data *johnson, emote_t emote_to_display, struct char_data *target) {
  char output[MAX_STRING_LENGTH];

  // Convert $N to an @target.
  char target_at_string[100];
  snprintf(target_at_string, sizeof(target_at_string), "@%s", GET_CHAR_NAME(target));
  replace_word(emote_to_display, output, sizeof(output), "$N", target_at_string);

  // Send our emote.
  do_new_echo(johnson, output, 0, 0);
}

void display_emotes_for_quest(struct char_data *johnson, emote_vector_t *vec, struct char_data *target) {
  // Get our current position in the emote chain.
  int pos = GET_SPARE1(johnson);

  if (pos >= (int) vec->size()) {
    mudlog_vfprintf(target, LOG_SYSLOG, "SYSERR: Received invalid pos %d which is greater than johnson emote vec size %lu!", pos, vec->size());
    send_to_char("(Whoops, something went wrong! Please see ^WRECAP^n for your quest details.)\r\n", target);
    return;
  }

  // Increment the current-emote pointer, and wrap back to -1 if we're done.
  if ((unsigned long) ++GET_SPARE1(johnson) >= vec->size())
    GET_SPARE1(johnson) = -1;

  // Convert and display the emote.
  display_single_emote_for_quest(johnson, vec->at(pos), target);

  // Let them know there's more to come.
  if (GET_SPARE1(johnson) >= 0) {
    send_to_char(target, "^L(%s is still acting...)^n\r\n", GET_CHAR_NAME(johnson));
  }
}

void handle_info(struct char_data *johnson, int num, struct char_data *target)
{
  // If there's an emote set available, print that.
  if (quest_table[num].info_emotes && !quest_table[num].info_emotes->empty()) {
    display_emotes_for_quest(johnson, quest_table[num].info_emotes, target);
    return;
  }

  int i, speech_index = 0;

  // Pre-calculate some text string lengths.
  size_t sayto_invocation_len = strlen(GET_CHAR_NAME(target)) + strlen(" ..."); // "<name> ..."
  size_t terminal_data_len = strlen("...") + 1; // "...\0"

  // Want to control how much the Johnson says per tick? Change this magic number.
  char speech[sayto_invocation_len + 200 + terminal_data_len];
  speech[0] = '\0';

  // Spare1 is the position in the info string we last left off at.
  int pos = GET_SPARE1(johnson);
  int starting_pos = pos;

  // Calculate how much text we can put into the speech buffer, leaving room for ellipses and \0.
  int allowed = sizeof(speech) - strlen(speech) - terminal_data_len;

  // i is the total length of the info string. We skip any newlines at the end.
  i = strlen(quest_table[num].info);
  while (quest_table[num].info[i] == '\r' || quest_table[num].info[i] == '\n' || quest_table[num].info[i] == '\0')
    i--;

  // We assume that all info strings will have ellipses (aka '...') at the end.
  bool will_add_ellipses = TRUE;

  // If the entire string won't fit, find the space or newline closest to the end. We'll write up to that.
  if ((pos + allowed) < i) {
    for (i = pos + allowed; i > pos; i--)
      if ((isspace(*(quest_table[num].info + i)) || *(quest_table[num].info + i) == '\r') &&
          isprint(*(quest_table[num].info + i - 1)) &&
          !isspace(*(quest_table[num].info + i - 1)))
        break;
    GET_SPARE1(johnson) = i + 1;
  }

  // Otherwise, we'll be done talking after this call-- wipe out their spare1 data
  //  (position in string) .
  else {
    GET_SPARE1(johnson) = -1;
    will_add_ellipses = FALSE;
  }

  // Print the string into the speech buff, replacing newlines with spaces.
  for (; pos < i && speech_index < allowed; pos++)
  {
    if (*(quest_table[num].info + pos) == '\n')
      continue;
    if (*(quest_table[num].info + pos) == '\r')
      speech[speech_index++] = ' ';
    else
      speech[speech_index++] = *(quest_table[num].info + pos);
  }

  speech[speech_index] = '\0';

  char say_buf[strlen(speech) + 100];
  snprintf(say_buf, sizeof(say_buf), "%s %s%s%s", GET_CHAR_NAME(target), starting_pos > 0 ? "..." : "", speech, will_add_ellipses ? "..." : "");

  // Say it.
  do_say(johnson, say_buf, 0, SCMD_SAYTO);
}

SPECIAL(johnson)
{
  struct char_data *johnson = (struct char_data *) me, *temp = NULL;
  int i, obj_complete = 0, mob_complete = 0, new_q, cached_new_q = -2, comm = CMD_JOB_NONE;

  if (!IS_NPC(johnson))
    return FALSE;

  if (!johnson->in_room) {
    mudlog("SYSERR: Johnson not in a room!", johnson, LOG_SYSLOG, TRUE);
    return FALSE;
  }

  if (!cmd) {
    if (GET_SPARE1(johnson) >= 0) {
      for (temp = johnson->in_room->people; temp; temp = temp->next_in_room)
        if (memory(johnson, temp))
          break;
      if (!temp) {
        GET_SPARE1(johnson) = -1;
      } else if (GET_QUEST(temp)) {
        handle_info(johnson, GET_QUEST(temp), temp);
      } else {
        // We're in the gap between someone asking for a job and accepting it. Do nothing.
      }
    }
    return FALSE;
  }

  skip_spaces(&argument);

  bool need_to_speak = FALSE;
  bool need_to_act = FALSE;
  bool is_sayto = CMD_IS("sayto") || CMD_IS("\"") || CMD_IS("ask") || CMD_IS("whisper");

  // If there's an astral state mismatch, bail out.
  if ((IS_ASTRAL(johnson) && !SEES_ASTRAL(ch)) || (IS_ASTRAL(ch) && !SEES_ASTRAL(johnson))) {
    return FALSE;
  }

  if (is_sayto) {
    // Do some really janky stuff: copy-paste target finding logic from sayto and see if they're talking to me.
    char mangled_argument[MAX_INPUT_LENGTH + 1];
    struct char_data *to = NULL;

    strlcpy(mangled_argument, argument, sizeof(mangled_argument));
    half_chop(argument, buf, buf2, sizeof(buf2));
    if (ch->in_veh)
      to = get_char_veh(ch, buf, ch->in_veh);
    else
      to = get_char_room_vis(ch, buf);

    if (to != johnson)
      return FALSE;

    // They really are talking to us.
  }

  if (CMD_IS("say") || CMD_IS("'") || is_sayto || CMD_IS("exclaim")) {
    if (str_str(argument, "quit"))
      comm = CMD_JOB_QUIT;
    else if (str_str(argument, "collect") || str_str(argument, "complete") || str_str(argument, "done") || str_str(argument, "finish") || str_str(argument, "pay"))
      comm = CMD_JOB_DONE;
    else if (str_str(argument, "work") || str_str(argument, "business") ||
             str_str(argument, "run") || str_str(argument, "shadowrun") ||
             str_str(argument, "job") || str_str(argument, "help"))
      comm = CMD_JOB_START;
    else if (str_str(argument, "yes") || str_str(argument, "accept") || str_str(argument, "yeah")
            || str_str(argument, "sure") || str_str(argument, "okay"))
      comm = CMD_JOB_YES;
    else if (str_str(argument, "no"))
      comm = CMD_JOB_NO;
    else if (access_level(ch, LVL_BUILDER) && !str_cmp(argument, "clear")) {
      for (int i = QUEST_TIMER - 1; i >= 0; i--) {
        GET_LQUEST(ch, i) = 0;
        GET_CQUEST(ch, i) = 0;
      }
      send_to_char("OK, your quest history has been cleared.\r\n", ch);
      GET_QUEST_DIRTY_BIT(ch) = TRUE;
      return FALSE;
    } else {
      //snprintf(buf, sizeof(buf), "INFO: No Johnson keywords found in %s's speech: '%s'.", GET_CHAR_NAME(ch), argument);
      //mudlog(buf, ch, LOG_SYSLOG, TRUE);
      return FALSE;
    }
    need_to_speak = TRUE;
  } else if (CMD_IS("nod") || CMD_IS("agree")) {
    comm = CMD_JOB_YES;
    need_to_act = TRUE;
  } else if (CMD_IS("shake") || CMD_IS("disagree")) {
    comm = CMD_JOB_NO;
    need_to_act = TRUE;
  } else if (CMD_IS("quests") || CMD_IS("jobs")) {
    // Intercept the 'quests' and 'jobs' ease-of-use commands to give them a new one if they don't have one yet.
    if (!GET_QUEST(ch)) {
      do_say(ch, "I'm looking for a job.", 0, 0);
      comm = CMD_JOB_START;
    } else {
      return FALSE;
    }
  } else if (CMD_IS("complete")) {
    if (GET_QUEST(ch)) {
      do_say(ch, "I've finished the job.", 0, 0);
      comm = CMD_JOB_DONE;
    } else {
      return FALSE;
    }
  } else
    return FALSE;

  if (IS_ASTRAL(ch)) {
    send_to_char("Astral projections aren't really employable.\r\n", ch);
    return FALSE;
  }

  if (IS_NPC(ch)) {
    send_to_char("NPCs don't get autoruns, go away.\r\n", ch);
    return FALSE;
  }

  if (PRF_FLAGGED(ch, PRF_NOHASSLE)) {
    send_to_char("You can't take runs with NOHASSLE turned on-- TOGGLE NOHASSLE to turn it off.\r\n", ch);
    return FALSE;
  }

  if (PRF_FLAGGED(ch, PRF_HIRED)) {
    send_to_char("You can't take autoruns while hired for a player run-- TOGGLE HIRED to disable your hired flag, then try again.\r\n", ch);
    return FALSE;
  }

  if (AFF_FLAGGED(johnson, AFF_GROUP) && ch->master) {
    send_to_char("I don't know how you ended up leading this Johnson around, but you can't take quests from your charmies.\r\n", ch);
    snprintf(buf, sizeof(buf), "WARNING: %s somehow managed to start leading Johnson %s.", GET_CHAR_NAME(ch), GET_NAME(johnson));
    mudlog(buf, ch, LOG_SYSLOG, TRUE);
    return FALSE;
  }

  // Hack to get around the fact that moving the failure check after the interact check would make you double-speak and double-act.
  if (need_to_speak)
    do_say(ch, argument, 0, is_sayto ? SCMD_SAYTO : 0);
  if (need_to_act)
    do_action(ch, argument, cmd, 0);

  switch (comm) {
    case CMD_JOB_QUIT:
      return attempt_quit_job(ch, johnson);
    case CMD_JOB_DONE:
      // Precondition: I cannot be talking right now.
      if (GET_SPARE1(johnson) == 0) {
        if (!memory(johnson, ch)) {
          do_say(johnson, "Hold on, I'm talking to someone else right now.", 0, 0);
          return TRUE;
        } else {
          do_say(johnson, "I'm lookin' for a yes-or-no answer, chummer.", 0, 0);
          return TRUE;
        }
      }

      // Precondition: You must be on a quest.
      if (!GET_QUEST(ch)) {
        do_say(johnson, "You're not even on a run right now.", 0, 0);
        return TRUE;
      }

      // Precondition: You must have gotten the quest from me.
      if (!memory(johnson, ch)) {
        do_say(johnson, "Whoever you got your job from, it wasn't me. What, do we all look alike to you?", 0 , 0);
        send_to_char("^L(OOC note: You can hit RECAP to see who gave you your current job.)^n\r\n", ch);
        return TRUE;
      }

      // Check for failure.
      for (i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++) {
        if (ch->player_specials->mob_complete[i] == -1) {
          do_say(johnson, "You fragged it up, and you still want to get paid?", 0, 0);
          end_quest(ch, FALSE);
          forget(johnson, ch);
          return TRUE;
        }
      }

      // Check for some form of completion-- even if one thing is done, we'll allow them to turn in the quest.
      for (i = 0; i < quest_table[GET_QUEST(ch)].num_objs; i++)
        if (ch->player_specials->obj_complete[i]) {
          obj_complete = 1;
          break;
        }
      for (i = 0; i < quest_table[GET_QUEST(ch)].num_mobs; i++)
        if (ch->player_specials->mob_complete[i]) {
          mob_complete = 1;
          break;
        }

      // Process turnin of the quest. The reward() function handles the work here.
      if (obj_complete || mob_complete) {
        if (!would_be_rewarded_for_turnin(ch)) {
          do_say(johnson, "You're not done yet!", 0, 0);
          return TRUE;
        }

        if (quest_table[GET_QUEST(ch)].finish_emote && *quest_table[GET_QUEST(ch)].finish_emote) {
          // Don't @ me about this, it's the only way to reliably display a newline in this context.
          act("^n", FALSE, johnson, 0, 0, TO_ROOM);
          char emote_with_carriage_return[MAX_STRING_LENGTH];
          snprintf(emote_with_carriage_return, sizeof(emote_with_carriage_return), "%s\r\n", quest_table[GET_QUEST(ch)].finish_emote);
          display_single_emote_for_quest(johnson, emote_with_carriage_return, ch);
        }
        else if (quest_table[GET_QUEST(ch)].finish)
          do_say(johnson, quest_table[GET_QUEST(ch)].finish, 0, 0);
        else {
          snprintf(buf, sizeof(buf), "WARNING: Null string in quest %ld!", quest_table[GET_QUEST(ch)].vnum);
          mudlog(buf, ch, LOG_SYSLOG, TRUE);
          do_say(johnson, "Well done.", 0, 0);
        }
        reward(ch, johnson);
        forget(johnson, ch);

        if (GET_QUEST(ch) == QST_MAGE_INTRO && GET_TRADITION(ch) != TRAD_MUNDANE)
          send_to_char(ch, "^M(OOC):^n You've discovered a follow-on quest! Follow the hint %s gave you to continue.\r\n", GET_CHAR_NAME(johnson));
      } else
        do_say(johnson, "You haven't completed any of your objectives yet.", 0, 0);

      return TRUE;
    case CMD_JOB_START: {

      // Reject high-rep characters.
      unsigned int johnson_max_rep = get_johnson_overall_max_rep(johnson);
      if (johnson_max_rep < 10000 && johnson_max_rep < GET_REP(ch)) {
        do_say(johnson, "My jobs aren't high-profile enough for someone with your rep!", 0, 0);
        send_to_char(ch, "[OOC: This Johnson caps out at %d reputation, so you won't get any further work from them.]\r\n", johnson_max_rep);

        GET_SPARE1(johnson) = -1;
        if (memory(johnson, ch))
            forget(johnson, ch);
        return TRUE;
      }

      new_q = new_quest(johnson, ch);
      //Clever hack to safely save us a call to new_quest() that compiler will be ok with.
      //If we have a cached quest use that and reset the cache integer back to -2 when
      //it is consumed.
      cached_new_q = new_q;

      //Handle out of quests and broken johnsons.
      //Calls to new_quest() return 0 when there's no quest left available and
      //-1 if the johnson is broken and has no quests.
      if (new_q == 0) {
          do_say(johnson, "I got nothing for you now. Come back later.", 0, 0);
          return TRUE;
      }
      if (new_q == -1) {
          return TRUE;
      }

      // Precondition: I cannot be talking right now.
      if (GET_SPARE1(johnson) == 0) {
        if (!memory(johnson, ch)) {
          do_say(johnson, "Hold on, I'm talking to someone else right now.", 0, 0);
          return TRUE;
        } else {
          do_say(johnson, "I'm lookin' for a yes-or-no answer, chummer.", 0, 0);
          return TRUE;
        }
      }

      // Precondition: You may not have an active quest.
      if (GET_QUEST(ch)) {
        do_say(johnson, "Maybe when you've finished what you're doing.", 0, 0);
        send_to_char("^L(OOC note: You're currently on another run. You can hit RECAP to see the details for it.)^n\r\n", ch);
        return TRUE;
      }

      // Precondition: You cannot be a flagged killer or a blacklisted character.
      if (PLR_FLAGGED(ch, PLR_KILLER) || PLR_FLAGGED(ch, PLR_BLACKLIST)) {
        do_say(johnson, "Word on the street is you can't be trusted.", 0, 0);
        GET_SPARE1(johnson) = -1;
        if (memory(johnson, ch))
          forget(johnson, ch);
      }

      // Reject low-rep characters.
      if (rep_too_low(ch, new_q)) {
        unsigned int johnson_min_rep = get_johnson_overall_min_rep(johnson);
        if (johnson_min_rep > GET_REP(ch)) {
          do_say(johnson, "You're not even worth my time right now.", 0, 0);
          send_to_char(ch, "[OOC: This Johnson has a minimum reputation requirement of %d. Come back when you have at least that much rep.]\r\n", johnson_min_rep);
        } else {
            int rep_delta = quest_table[new_q].min_rep - GET_REP(ch);
            if (rep_delta >= 1000) {
                do_say(johnson, "Who are you?", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn a whole lot more rep before you can take on this job.]\r\n", johnson_min_rep);
                }
            }
            else if  (rep_delta >= 500) {
                do_say(johnson, "Don't talk to me.", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn significantly more rep before you can take on this job.]\r\n", johnson_min_rep);
                }
            }
            else if  (rep_delta >= 200) {
                do_say(johnson, "Go pick up a few new tricks.", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn %d more points of rep before you can take on this job.]\r\n", rep_delta);
                }
            }
            else if  (rep_delta >= 100) {
                do_say(johnson, "You're still a little wet behind the ears for this one.", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn %d more points of rep before you can take on this job.]\r\n", rep_delta);
                }
            }
            else if  (rep_delta >= 20) {
                do_say(johnson, "Come back later, omae.", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn %d more points of rep before you can take on this job.]\r\n", rep_delta);
                }
            }
            else {
                do_say(johnson, "I might have something for you soon.", 0, 0);
                if (PRF_FLAGGED(ch, PRF_SEE_TIPS)) {
                  send_to_char(ch, "[OOC: You need to earn %d more points of rep before you can take on this job.]\r\n", rep_delta);
                }
            }
        }

        GET_SPARE1(johnson) = -1;
        if (memory(johnson, ch))
          forget(johnson, ch);
        return TRUE;
      }

      // Assign the quest.
      GET_SPARE1(johnson) = 0;
      if (quest_table[new_q].intro_emote && *quest_table[new_q].intro_emote) {
        // Don't @ me about this, it's the only way to reliably display a newline in this context.
        act("^n", FALSE, johnson, 0, 0, TO_ROOM);
        char intro_emote_with_carriage_return[MAX_STRING_LENGTH];
        snprintf(intro_emote_with_carriage_return, sizeof(intro_emote_with_carriage_return), "%s\r\n", quest_table[new_q].intro_emote);
        display_single_emote_for_quest(johnson, intro_emote_with_carriage_return, ch);
      }
      else if (quest_table[new_q].intro) {
        do_say(johnson, quest_table[new_q].intro, 0, 0);
      }
      else {
        snprintf(buf, sizeof(buf), "WARNING: Null intro string in quest %ld!", quest_table[new_q].vnum);
        mudlog(buf, ch, LOG_SYSLOG, TRUE);
        do_say(johnson, "I've got a job for you.", 0, 0);
      }
      do_say(johnson, "Are you interested?", 0, 0);
      if (!memory(johnson, ch))
        remember(johnson, ch);

      return TRUE;
    }
    case CMD_JOB_YES:
      // Precondition: If I'm not talking right now, don't react.
      if (GET_SPARE1(johnson) == -1) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "Johnson won't react-- GET_SPARE1 is -1.\r\n");
        }
        return TRUE;
      }

      // Precondition: If I have no memory of you, dismiss you.
      if (!memory(johnson, ch)) {
        do_say(johnson, "Hold on, I'm talking to someone else right now.", 0, 0);
        return TRUE;
      }
      //Clever hack to safely save us a call to new_quest() that compiler will be ok with.
      //If we have a cached quest use that and reset the cache integer back to -2 when
      //it is consumed.
      if (cached_new_q != -2) {
        new_q = cached_new_q;
        cached_new_q = -2;
      } else {
        new_q = new_quest(johnson, ch);
      }

      //Handle out of quests and broken johnsons.
      //Calls to new_quest() return 0 when there's no quest left available and
      //-1 if the johnson is broken and has no quests.
      if (new_q == 0) {
        //This is should never really happen but if it does let's log it.
        snprintf(buf, sizeof(buf), "WARNING: Quest vanished between offered and accepted from %s (%ld) due.", GET_NAME(johnson), GET_MOB_VNUM(johnson));
        mudlog(buf, NULL, LOG_SYSLOG, true);
        do_say(johnson, "What are you talking about?", 0, 0);
        return TRUE;
      }
      if (new_q == -1) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "Johnson won't react-- they're broken!\r\n");
        }
        return TRUE;
      }

      // Precondition: You may not have an active quest.
      if (GET_QUEST(ch)) {
        // If it's the same quest, just bail out without a message.
        if (GET_QUEST(ch) == new_q) {
          if (access_level(ch, LVL_BUILDER)) {
            send_to_char(ch, "Johnson won't react-- you're already on this quest.\r\n");
          }
          return TRUE;
        }

        do_say(johnson, "Maybe when you've finished what you're doing.", 0, 0);
        send_to_char("^L(OOC note: You're currently on another run. You can hit RECAP to see the details for it.)^n\r\n", ch);
        return TRUE;
      }

      // Start the quest.
      initialize_quest_for_ch(ch, new_q, johnson);

      return TRUE;

    case CMD_JOB_NO:
      // Precondition: If I'm not talking right now, don't react.
      if (GET_SPARE1(johnson) == -1) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "Johnson won't react-- GET_SPARE1 is -1.\r\n");
        }
        return TRUE;
      }

      // Precondition: If I have no memory of you, dismiss you.
      if (!memory(johnson, ch)) {
        do_say(johnson, "Hold on, I'm talking to someone else right now.", 0, 0);
        return TRUE;
      }

      // Precondition: You can't be on a quest. Fixes edge case where you could say 'no' during spiel to cancel run.
      if (GET_QUEST(ch)) {
        if (access_level(ch, LVL_BUILDER)) {
          send_to_char(ch, "Johnson won't react-- you're already on a quest and can't decline one.\r\n");
        }
        return TRUE;
      }

      //Clever hack to safely save us a call to new_quest() that compiler will be ok with.
      //If we have a cached quest use that and reset the cache integer back to -2 when
      //it is consumed.
      if (cached_new_q != -2) {
        new_q = cached_new_q;
        cached_new_q = -2;
      } else {
        new_q = new_quest(johnson, ch);
      }

      //Handle out of quests and broken johnsons.
      //Calls to new_quest() return 0 when there's no quest left available and
      //-1 if the johnson is broken and has no quests.
      if (new_q == 0) {
        //This is should never really happen but if it does let's log it.
        snprintf(buf, sizeof(buf), "WARNING: Quest vanished between offering and declining from %s (%ld).", GET_NAME(johnson), GET_MOB_VNUM(johnson));
        mudlog(buf, NULL, LOG_SYSLOG, true);
        do_say(johnson, "What are you talking about?", 0, 0);
        return TRUE;
      }
      if (new_q == -1) {
        return TRUE;
      }

      // Decline the quest.
      GET_SPARE1(johnson) = -1;
      GET_QUEST(ch) = 0;
      GET_QUEST_STARTED(ch) = 0;
      forget(johnson, ch);
      if (quest_table[new_q].decline_emote && *quest_table[new_q].decline_emote) {
        // Don't @ me about this, it's the only way to reliably display a newline in this context.
        act("^n", FALSE, johnson, 0, 0, TO_ROOM);
        char emote_with_carriage_return[MAX_STRING_LENGTH];
        snprintf(emote_with_carriage_return, sizeof(emote_with_carriage_return), "%s\r\n", quest_table[new_q].decline_emote);
        display_single_emote_for_quest(johnson, emote_with_carriage_return, ch);
      }
      else if (quest_table[new_q].decline)
        do_say(johnson, quest_table[new_q].decline, 0, 0);
      else {
        snprintf(buf, sizeof(buf), "WARNING: Null string in quest %ld!", quest_table[new_q].vnum);
        mudlog(buf, ch, LOG_SYSLOG, TRUE);
        do_say(johnson, "Fine.", 0, 0);
      }
      return TRUE;
    default:
      new_q = new_quest(johnson, ch);
      do_say(johnson, "Ugh, drank too much last night. Talk to me later when I've sobered up.", 0, 0);
      snprintf(buf, sizeof(buf), "WARNING: Failed to evaluate Johnson tree and return successful message for Johnson '%s' (%ld). Values: comm = %d, spare1 = %ld, quest = %d (maps to %ld)",
              GET_NAME(johnson), GET_MOB_VNUM(johnson), comm, GET_SPARE1(johnson), new_q, quest_table[new_q].vnum);
      mudlog(buf, ch, LOG_SYSLOG, TRUE);
      break;
  }

  return TRUE;
}


/*
 * Almost a copy of the 'is_open()' for shops but modified
 * a good deal to work for Johnsons
 */
void johnson_update(void)
{
  struct char_data *johnson = NULL, *tmp = NULL;
  char buf[200];
  int i, rstart = 0, rend = 0;

  *buf = 0;

  for ( i = 0; i <= top_of_questt;  i++ ) {
    /* Random times */
    if ( quest_table[i].s_time == -1 ) {
      if ( dice(1, 6) )
        rstart++;

      if ( dice(1, 6) ) {
        rstart--;
        rend++;
      }
    }

    /* Needs to come to 'work' */
    if ( rstart || quest_table[i].s_time > time_info.hours ) {
      if ( quest_table[i].s_string != NULL )
        strcpy( buf, quest_table[i].s_string );
      johnson = read_mobile( quest_table[i].johnson, REAL );
      MOB_FLAGS(johnson).SetBit(MOB_ISNPC);
      johnson->mob_loaded_in_room = GET_ROOM_VNUM(&world[quest_table[i].s_room]);
      char_to_room( johnson, &world[quest_table[i].s_room] );
    }
    /* Needs to head off */
    else if ( rend || quest_table[i].e_time < time_info.hours ) {
      if ( quest_table[i].e_string != NULL )
        strcpy( buf, quest_table[i].e_string );
      for ( johnson = character_list; johnson != NULL; johnson = johnson->next_in_character_list ) {
        if ( johnson->nr == (tmp = read_mobile( quest_table[i].johnson, REAL))->nr )
          break;
      }
      if ( johnson != NULL && johnson->in_room) {
        MOB_FLAGS(johnson).SetBit(MOB_ISNPC);
        char_from_room( johnson );
        char_to_room( johnson, &world[0] );
        extract_char(johnson);
      }
      if ( tmp != NULL && tmp->in_room) {
        MOB_FLAGS(tmp).SetBit(MOB_ISNPC);
        extract_char( tmp );
      }
    }
  }

  if ( *buf != '\0' ) {
    act( buf, TRUE, johnson, 0, 0, TO_ROOM );
  }

  return;
}


void assign_johnsons(void)
{
  int i, rnum;

  for (i = 0; i <= top_of_questt; i++) {
    if ((rnum = real_mobile(quest_table[i].johnson)) < 0)
      log_vfprintf("Johnson #%ld does not exist (quest #%ld)",
          quest_table[i].johnson, quest_table[i].vnum);
    else if (mob_index[rnum].func != johnson && mob_index[rnum].sfunc != johnson) {
      mob_index[rnum].sfunc = mob_index[rnum].func;
      mob_index[rnum].func = johnson;
    }
  }
}

void list_detailed_quest(struct char_data *ch, long rnum)
{
  int i;

  {
    rnum_t johnson = real_mobile(quest_table[rnum].johnson);

    snprintf(buf, sizeof(buf), "Vnum: [%5ld], Rnum: [%ld], Johnson: [%s (%ld)]\r\n",
            quest_table[rnum].vnum, rnum,
            johnson < 0 ? "^ynone^n" : GET_NAME(mob_proto+johnson),
            quest_table[rnum].johnson);
  }

  if (!access_level(ch, LVL_ADMIN))
  {
    send_to_char(buf, ch);
    return;
  }

  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Time allowed: [^c%d^n], Minimum reputation: [^c%d^n], "
          "Maximum reputation: [^c%d^n]\r\n", quest_table[rnum].time,
          quest_table[rnum].min_rep, quest_table[rnum].max_rep);


  rnum_t quest_reward_rnum = real_object(quest_table[rnum].reward);
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Bonus nuyen: [^c%d^n (%d)], Bonus Karma: [^c%0.2f^n (%0.2f)], "
           "Reward: [^c%ld (%s)^n]\r\n",
           (int) (quest_table[rnum].nuyen * NUYEN_GAIN_MULTIPLIER),
           quest_table[rnum].nuyen,
           ((float)quest_table[rnum].karma / 100) * KARMA_GAIN_MULTIPLIER,
           ((float)quest_table[rnum].karma / 100),
           quest_table[rnum].reward,
           quest_reward_rnum >= 0 ? GET_OBJ_NAME(&obj_proto[quest_reward_rnum]) : "<invalid>");

  for (i = 0; i < quest_table[rnum].num_mobs; i++) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "^mM^n%2d) ^c%d^n (%d) nuyen/^c%0.2f^n (%0.2f) karma: vnum %ld; %s (%ld); %s (%ld)\r\n",
            i,
            (int) (quest_table[rnum].mob[i].nuyen * NUYEN_GAIN_MULTIPLIER),
            quest_table[rnum].mob[i].nuyen,
            ((float)quest_table[rnum].mob[i].karma / 100) * KARMA_GAIN_MULTIPLIER,
            ((float)quest_table[rnum].mob[i].karma / 100),
            quest_table[rnum].mob[i].vnum, sml[(int)quest_table[rnum].mob[i].load],
            quest_table[rnum].mob[i].l_data,
            smo[(int)quest_table[rnum].mob[i].objective],
            quest_table[rnum].mob[i].o_data);
  }


  for (i = 0; i < quest_table[rnum].num_objs; i++) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "^oO^n%2d) ^c%d^n (%d) nuyen/^c%0.2f^n (%0.2f) karma: vnum %ld; %s (%ld/%ld); %s (%ld)\r\n",
            i,
            (int) (quest_table[rnum].obj[i].nuyen * NUYEN_GAIN_MULTIPLIER),
            quest_table[rnum].obj[i].nuyen,
            ((float)quest_table[rnum].obj[i].karma / 100) * KARMA_GAIN_MULTIPLIER,
            ((float)quest_table[rnum].obj[i].karma / 100),
            quest_table[rnum].obj[i].vnum, sol[(int)quest_table[rnum].obj[i].load],
            quest_table[rnum].obj[i].l_data, quest_table[rnum].obj[i].l_data2,
            soo[(int)quest_table[rnum].obj[i].objective],
            quest_table[rnum].obj[i].o_data);
  }

  page_string(ch->desc, buf, 1);
}

void boot_one_quest(struct quest_data *quest)
{
  int count, quest_nr = -1, i;

  if ((top_of_questt + 2) >= top_of_quest_array)
    // if it cannot resize, return...the edit_quest is freed later
    if (!resize_qst_array())
    {
      olc_state = 0;
      return;
    }

  for (count = 0; count <= top_of_questt; count++)
    if (quest_table[count].vnum > quest->vnum)
    {
      quest_nr = count;
      break;
    }

  if (quest_nr == -1)
    quest_nr = top_of_questt + 1;
  else
    for (count = top_of_questt + 1; count > quest_nr; count--)
    {
      // copy quest_table[count-1] to quest_table[count]
      quest_table[count] = quest_table[count-1];
    }

  top_of_questt++;

  // and now we copy quest to quest_table[quest_nr]
  quest_table[quest_nr].vnum = quest->vnum;
  quest_table[quest_nr].johnson = quest->johnson;
  quest_table[quest_nr].time = quest->time;
  quest_table[quest_nr].min_rep = quest->min_rep;
  quest_table[quest_nr].max_rep = quest->max_rep;
  quest_table[quest_nr].nuyen = quest->nuyen;
  quest_table[quest_nr].karma = quest->karma;
  quest_table[quest_nr].reward = quest->reward;
  quest_table[quest_nr].prerequisite_quest = quest->prerequisite_quest;
  quest_table[quest_nr].disqualifying_quest = quest->disqualifying_quest;

  quest_table[quest_nr].num_objs = quest->num_objs;
  if (quest_table[quest_nr].num_objs > 0)
  {
    quest_table[quest_nr].obj = new struct quest_om_data[quest_table[quest_nr].num_objs];
    for (i = 0; i < quest_table[quest_nr].num_objs; i++)
      quest_table[quest_nr].obj[i] = quest->obj[i];
  } else
    quest_table[quest_nr].obj = NULL;

  quest_table[quest_nr].num_mobs = quest->num_mobs;
  if (quest_table[quest_nr].num_mobs > 0)
  {
    quest_table[quest_nr].mob = new struct quest_om_data[quest_table[quest_nr].num_mobs];
    for (i = 0; i < quest_table[quest_nr].num_mobs; i++)
      quest_table[quest_nr].mob[i] = quest->mob[i];
  } else
    quest_table[quest_nr].mob = NULL;

  quest_table[quest_nr].intro = str_dup(quest->intro);
  quest_table[quest_nr].decline = str_dup(quest->decline);
  quest_table[quest_nr].quit = str_dup(quest->quit);
  quest_table[quest_nr].finish = str_dup(quest->finish);
  quest_table[quest_nr].info = str_dup(quest->info);
  quest_table[quest_nr].intro_emote = str_dup(quest->intro_emote);
  quest_table[quest_nr].decline_emote = str_dup(quest->decline_emote);
  quest_table[quest_nr].quit_emote = str_dup(quest->quit_emote);
  quest_table[quest_nr].finish_emote = str_dup(quest->finish_emote);
  CLONE_EMOTE_VECTOR(quest->info_emotes, quest_table[quest_nr].info_emotes);

  quest_table[quest_nr].done = str_dup(quest->done);
  quest_table[quest_nr].s_string = str_dup(quest->s_string);
  quest_table[quest_nr].e_string = str_dup(quest->e_string);
#ifdef USE_QUEST_LOCATION_CODE
  quest_table[quest_nr].location = str_dup(quest->location);
#endif

  if ((i = real_mobile(quest_table[quest_nr].johnson)) > 0 &&
      mob_index[i].func != johnson)
  {
    mob_index[i].sfunc = mob_index[i].func;
    mob_index[i].func = johnson;
    mob_proto[i].real_abils.attributes[QUI] = MAX(1, mob_proto[i].real_abils.attributes[QUI]);

    // Ensure that all instances of this johnson don't have a spare1 value set
    for (struct char_data *tmp = character_list; tmp; tmp = tmp->next_in_character_list) {
      if (IS_NPC(tmp) && GET_MOB_VNUM(tmp) == quest_table[quest_nr].johnson) {
        GET_SPARE1(tmp) = 0;
      }
    }
  }
}

void reboot_quest(int rnum, struct quest_data *quest)
{
  int i, ojn, njn;

  if (quest_table[rnum].johnson != quest->johnson)
  {
    ojn = real_mobile(quest_table[rnum].johnson);
    njn = real_mobile(quest->johnson);
    if (njn < 0) {
      char oopsbuf[5000];
      snprintf(oopsbuf, sizeof(oopsbuf), "BUILD ERROR: Quest %ld has non-existent new Johnson %ld.", quest_table[rnum].vnum, quest->johnson);
      mudlog(oopsbuf, NULL, LOG_SYSLOG, TRUE);
      return;
    }

    // It's possible for ojn to be -1 in the case of the quest first being built.
    if (ojn >= 0) {
      if (mob_index[ojn].func == johnson) {
        mob_index[ojn].func = mob_index[ojn].sfunc;
        mob_index[ojn].sfunc = NULL;
      } else if (mob_index[ojn].sfunc == johnson)
        mob_index[ojn].sfunc = NULL;
    }

    mob_index[njn].sfunc = mob_index[njn].func;
    mob_index[njn].func = johnson;
    quest_table[rnum].johnson = quest->johnson;
  }

  quest_table[rnum].time = quest->time;
  quest_table[rnum].min_rep = quest->min_rep;
  quest_table[rnum].max_rep = quest->max_rep;
  quest_table[rnum].num_objs = quest->num_objs;
  quest_table[rnum].num_mobs = quest->num_mobs;
  quest_table[rnum].nuyen = quest->nuyen;
  quest_table[rnum].karma = quest->karma;
  quest_table[rnum].reward = quest->reward;
  quest_table[rnum].prerequisite_quest = quest->prerequisite_quest;
  quest_table[rnum].disqualifying_quest = quest->disqualifying_quest;

  if (quest_table[rnum].obj)
    delete [] quest_table[rnum].obj;

  if (quest_table[rnum].num_objs > 0)
  {
    quest_table[rnum].obj = new quest_om_data[quest_table[rnum].num_objs];

    for (i = 0; i < quest_table[rnum].num_objs; i++)
      quest_table[rnum].obj[i] = quest->obj[i];
  } else
    quest_table[rnum].obj = NULL;

  if (quest_table[rnum].mob)
    delete [] quest_table[rnum].mob;

  if (quest_table[rnum].num_mobs > 0)
  {
    quest_table[rnum].mob = new quest_om_data[quest_table[rnum].num_mobs];

    for (i = 0; i < quest_table[rnum].num_mobs; i++)
      quest_table[rnum].mob[i] = quest->mob[i];
  } else
    quest_table[rnum].mob = NULL;

  if (quest_table[rnum].intro)
    delete [] quest_table[rnum].intro;
  quest_table[rnum].intro = str_dup(quest->intro);

  if (quest_table[rnum].decline)
    delete [] quest_table[rnum].decline;
  quest_table[rnum].decline = str_dup(quest->decline);

  if (quest_table[rnum].quit)
    delete [] quest_table[rnum].quit;
  quest_table[rnum].quit = str_dup(quest->quit);

  if (quest_table[rnum].finish)
    delete [] quest_table[rnum].finish;
  quest_table[rnum].finish = str_dup(quest->finish);

  if (quest_table[rnum].info)
    delete [] quest_table[rnum].info;
  quest_table[rnum].info = str_dup(quest->info);

#define DELETE_AND_NULL_EMOTE_VECTOR(vect) {                              \
  if ((vect)) {                                                           \
    /* Delete the existing strings, which were allocated with str_dup. */ \
    for (auto it = (vect)->begin(); it != (vect)->end(); it++) {          \
      delete [] *it;                                                      \
      *it = NULL;                                                         \
    }                                                                     \
    /* Clear the vector entries containing the deleted strings. */        \
    (vect)->clear();                                                      \
    /* Delete the vector itself, since it was made with new. */           \
    delete (vect);                                                        \
  }                                                                       \
  (vect) = NULL;                                                          \
}

  delete [] quest_table[rnum].intro_emote;
  quest_table[rnum].intro_emote = quest->intro_emote;
  quest->intro_emote = NULL;

  delete [] quest_table[rnum].decline_emote;
  quest_table[rnum].decline_emote = quest->decline_emote;
  quest->decline_emote = NULL;

  delete [] quest_table[rnum].quit_emote;
  quest_table[rnum].quit_emote = quest->quit_emote;
  quest->quit_emote = NULL;

  delete [] quest_table[rnum].finish_emote;
  quest_table[rnum].finish_emote = quest->finish_emote;
  quest->finish_emote = NULL;

  DELETE_AND_NULL_EMOTE_VECTOR(quest_table[rnum].info_emotes);
  quest_table[rnum].info_emotes = quest->info_emotes;
  quest->info_emotes = NULL;

  if (quest_table[rnum].done)
    delete [] quest_table[rnum].done;
  quest_table[rnum].done = str_dup(quest->done);

  if (quest_table[rnum].s_string)
    delete [] quest_table[rnum].s_string;
  quest_table[rnum].s_string = str_dup(quest->s_string);

  if (quest_table[rnum].e_string)
    delete [] quest_table[rnum].e_string;
  quest_table[rnum].e_string = str_dup(quest->e_string);

#ifdef USE_QUEST_LOCATION_CODE
  if (quest_table[rnum].location)
    delete [] quest_table[rnum].location;
  quest_table[rnum].location = str_dup(quest->location);
#endif
}

int write_quests_to_disk(int zone) {
  long i, j, found = 0, counter;
  FILE *fp;
  zone = real_zone(zone);
  bool wrote_something = FALSE;

  char final_file_name[1000];
  snprintf(final_file_name, sizeof(final_file_name), "world/qst/%d.qst", zone_table[zone].number);

  char tmp_file_name[1000];
  snprintf(tmp_file_name, sizeof(tmp_file_name), "%s.tmp", final_file_name);

  if (!(fp = fopen(tmp_file_name, "w+"))) {
    log_vfprintf("SYSERR: could not open file %s", tmp_file_name);

    fclose(fp);
    return 0;
  }

  for (counter = zone_table[zone].number * 100;
       counter <= zone_table[zone].top; counter++) {
    if ((i = real_quest(counter)) > -1) {
      wrote_something = TRUE;
      fprintf(fp, "#%ld\n", quest_table[i].vnum);
      fprintf(fp, "%ld %d %d %d %d %d %ld %d %d %d %d %d %d %d %d %d %ld %ld %ld\n", quest_table[i].johnson,
              quest_table[i].time, quest_table[i].min_rep,
              quest_table[i].max_rep, quest_table[i].nuyen,
              quest_table[i].karma, quest_table[i].reward,
              quest_table[i].num_objs, quest_table[i].num_mobs,
              quest_table[i].s_time, quest_table[i].e_time,
              quest_table[i].s_room,
              quest_table[i].intro_emote ? 1 : 0,
              quest_table[i].decline_emote ? 1 : 0,
              quest_table[i].quit_emote ? 1 : 0,
              quest_table[i].finish_emote ? 1 : 0,
              quest_table[i].info_emotes ? quest_table[i].info_emotes->size() : 0,
              quest_table[i].prerequisite_quest,
              quest_table[i].disqualifying_quest
            );

      for (j = 0; j < quest_table[i].num_objs; j++)
        fprintf(fp, "%ld %d %d %d %d %ld %ld %ld\n", 
                quest_table[i].obj[j].vnum,
                quest_table[i].obj[j].nuyen, 
                quest_table[i].obj[j].karma,
                quest_table[i].obj[j].load, 
                quest_table[i].obj[j].objective,
                quest_table[i].obj[j].l_data, 
                quest_table[i].obj[j].l_data2,
                quest_table[i].obj[j].o_data);

      for (j = 0; j < quest_table[i].num_mobs; j++)
        fprintf(fp, "%ld %d %d %d %d %ld %ld %ld\n", 
                quest_table[i].mob[j].vnum,
                quest_table[i].mob[j].nuyen, 
                quest_table[i].mob[j].karma,
                quest_table[i].mob[j].load, 
                quest_table[i].mob[j].objective,
                quest_table[i].mob[j].l_data, 
                quest_table[i].mob[j].l_data2,
                quest_table[i].mob[j].o_data);

#define WRITE_EMOTES_TO_DISK(type) if (quest_table[i].type##_emotes) {for (auto a: *(quest_table[i].type##_emotes)) { fprintf(fp, "%s~\r\n", prep_string_for_writing_to_savefile(buf2, a)); }}

      if (quest_table[i].intro_emote)
        fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].intro_emote));
      if (quest_table[i].decline_emote)
        fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].decline_emote));
      if (quest_table[i].quit_emote)
        fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].quit_emote));
      if (quest_table[i].finish_emote)
        fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].finish_emote));

      WRITE_EMOTES_TO_DISK(info);

#undef WRITE_EMOTES_TO_DISK

      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].intro));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].decline));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].quit));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].finish));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].info));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].s_string));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].e_string));
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].done));
#ifdef USE_QUEST_LOCATION_CODE
      fprintf(fp, "%s~\n", prep_string_for_writing_to_savefile(buf2, quest_table[i].location));
#endif
    }
  }
  fprintf(fp, "$~\n");
  fclose(fp);

  // If we wrote anything for this zone, update the index file.
  if (wrote_something) {
    fp = fopen("world/qst/index", "w+");

    for (i = 0; i <= top_of_zone_table; ++i) {
      found = 0;
      for (j = 0; !found && j <= top_of_questt; j++)
        if (quest_table[j].vnum >= (zone_table[i].number * 100) &&
            quest_table[j].vnum <= zone_table[i].top) {
          found = 1;
          fprintf(fp, "%d.qst\n", zone_table[i].number);
        }
    }

    fprintf(fp, "$~\n");
    fclose(fp);

    // Then remove the old file and rename the temp file.
    remove(final_file_name);
    rename(tmp_file_name, final_file_name);
  }
  // Otherwise, delete the empty junk file.
  else
    remove(tmp_file_name);

  return 1;
}

void qedit_list_obj_objectives(struct descriptor_data *d)
{
  CLS(CH);

  *buf = '\0';

  for (int i = 0; i < QUEST->num_objs; i++)
  {
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%2d) ", i);

    rnum_t obj_rnum = real_object(QUEST->obj[i].vnum);
    struct obj_data *obj = (obj_rnum >= 0 ? &obj_proto[obj_rnum] : NULL);

    {
      // These are derived from l_data, which is not used in the second stanza.
      rnum_t mob_rnum;
      bool target_is_listed_mob;
      if (QUEST->obj[i].l_data < 0 || QUEST->obj[i].l_data >= QUEST->num_mobs || (mob_rnum = real_mobile(QUEST->mob[QUEST->obj[i].l_data].vnum)) < 0) {
        mob_rnum = real_mobile(QUEST->obj[i].l_data);
        target_is_listed_mob = FALSE;
      } else {
        target_is_listed_mob = TRUE;
      }
      struct char_data *mob = (mob_rnum >= 0 ? &mob_proto[mob_rnum] : NULL);

      rnum_t room_rnum = real_room(QUEST->obj[i].l_data);
      struct room_data *room = (room_rnum >= 0 ? &world[room_rnum] : NULL);

      rnum_t host_rnum = real_host(QUEST->obj[i].l_data);
      struct host_data *host = (host_rnum >= 0 ? &matrix[host_rnum] : NULL);
      
      switch (QUEST->obj[i].load) {
        case QUEST_NONE:
          strlcat(buf, "Load nothing", sizeof(buf));
          break;
        case QOL_JOHNSON:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Give %ld (%s) to Johnson", 
                   QUEST->obj[i].vnum,
                   obj ? GET_OBJ_NAME(obj) : "N/A");
          break;
        case QOL_TARMOB_I:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Add %ld (%s) to inventory of %s%ld (%s) (NOT A DELIVERY OBJECTIVE)", 
                   QUEST->obj[i].vnum,
                   obj ? GET_OBJ_NAME(obj) : "N/A",
                   target_is_listed_mob ? "M" : "vnum ",
                   QUEST->obj[i].l_data,
                   mob ? GET_NAME(mob) : "NULL");
          break;
        case QOL_TARMOB_E:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Equip %s%ld (%s) with %ld (%s) at %s", 
                   target_is_listed_mob ? "M" : "vnum ",
                   QUEST->obj[i].l_data,
                   mob ? GET_NAME(mob) : "NULL",
                   QUEST->obj[i].vnum,
                   obj ? GET_OBJ_NAME(obj) : "N/A",
                   wear_bits[QUEST->obj[i].l_data2]);
          break;
        case QOL_TARMOB_C:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Install %ld (%s) in %s%ld (%s)", 
                   QUEST->obj[i].vnum,
                   obj ? GET_OBJ_NAME(obj) : "N/A",
                   target_is_listed_mob ? "M" : "vnum ",
                   QUEST->obj[i].l_data,
                   mob ? GET_NAME(mob) : "NULL");
          break;
        case QOL_HOST:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Load %ld (%s) in host %ld (%s)", 
                   QUEST->obj[i].vnum, 
                   obj ? GET_OBJ_NAME(obj) : "N/A",
                   QUEST->obj[i].l_data,
                   host ? host->name : NULL);
          break;
        case QOL_LOCATION:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Load %ld (%s) in room %ld (%s)", 
                   QUEST->obj[i].vnum,
                   obj ? GET_OBJ_NAME(obj) : "N/A",
                   QUEST->obj[i].l_data,
                   room ? GET_ROOM_NAME(room) : "NULL");
          break;
      }
    }

    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "\r\n    Award %d (%d) nuyen & %0.2f (%0.2f) karma for ",
        (int) (QUEST->obj[i].nuyen * NUYEN_GAIN_MULTIPLIER),
        QUEST->obj[i].nuyen,
        ((float)QUEST->obj[i].karma / 100) * KARMA_GAIN_MULTIPLIER, ((float)QUEST->obj[i].karma / 100));

    {
      // These are derived from o_data, which is not used in the first stanza.
      rnum_t mob_rnum;
      bool target_is_listed_mob;
      if (QUEST->obj[i].o_data < 0 || QUEST->obj[i].o_data >= QUEST->num_mobs || (mob_rnum = real_mobile(QUEST->mob[QUEST->obj[i].o_data].vnum)) < 0) {
        mob_rnum = real_mobile(QUEST->obj[i].o_data);
        target_is_listed_mob = FALSE;
      } else {
        target_is_listed_mob = TRUE;
      }
      struct char_data *mob = (mob_rnum >= 0 ? &mob_proto[mob_rnum] : NULL);

      rnum_t room_rnum = real_room(QUEST->obj[i].o_data);
      struct room_data *room = (room_rnum >= 0 ? &world[room_rnum] : NULL);

      rnum_t host_rnum = real_host(QUEST->obj[i].o_data);
      struct host_data *host = (host_rnum >= 0 ? &matrix[host_rnum] : NULL);
      
      switch (QUEST->obj[i].objective) {
        case QUEST_NONE:
          strlcat(buf, "nothing (no objective)\r\n", sizeof(buf));
          break;
        case QOO_JOHNSON:
          strlcat(buf, "returning item to Johnson\r\n", sizeof(buf));
          break;
        case QOO_TAR_MOB:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "delivering item %ld (%s) to %s%ld (%s)\r\n",
                  QUEST->obj[i].vnum,
                  obj ? GET_OBJ_NAME(obj) : "N/A",
                  target_is_listed_mob ? "M" : "vnum ",
                  QUEST->obj[i].o_data,
                  mob ? GET_NAME(mob) : "NULL");
          break;
        case QOO_LOCATION:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "delivering item to room %ld (%s)\r\n",
                   QUEST->obj[i].o_data,
                   room ? GET_ROOM_NAME(room) : "NULL");
          break;
        case QOO_DSTRY_ONE:
          strlcat(buf, "destroying item\r\n", sizeof(buf));
          break;
        case QOO_DSTRY_MANY:
          strlcat(buf, "each item destroyed\r\n", sizeof(buf));
          break;
        case QOO_UPLOAD:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "uploading to host %ld (%s)\n\n", 
                   QUEST->obj[i].o_data,
                   host ? host->name : "NULL");
          break;
        case QOO_RETURN_PAY:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "returning paydata from host %ld (%s)\r\n",
                   QUEST->obj[i].o_data,
                   host ? host->name : "NULL");
      }
    }
  }
  send_to_char(buf, CH);
}

void qedit_list_mob_objectives(struct descriptor_data *d)
{
  CLS(CH);

  *buf = '\0';

  for (int i = 0; i < QUEST->num_mobs; i++) {
    struct char_data *mob_ptr = fetch_quest_mob_actual_mob_proto(QUEST, i);
    
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%2d) ", i);

    switch (QUEST->mob[i].load) {
      case QUEST_NONE:
        strlcat(buf, "Not set", sizeof(buf));
        break;
      case QML_LOCATION:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Load %ld (%s) at room %ld (%s)",
                 QUEST->mob[i].vnum,
                 GET_CHAR_NAME(mob_ptr),
                 QUEST->mob[i].l_data,
                 real_room(QUEST->mob[i].l_data) >= 0 ? GET_ROOM_NAME(&world[real_room(QUEST->mob[i].l_data)]) : "NULL");
        break;
      case QML_FOLQUESTER:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Load %ld (%s) and follow quester",
                 QUEST->mob[i].vnum,
                 GET_CHAR_NAME(mob_ptr));
        break;
      default:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "\r\n - Unknown QML %d\r\n", QUEST->mob[i].load);
        break;
    }

    bool target_is_listed_mob = TRUE;
    if (QUEST->mob[i].o_data >= 0 && (QUEST->mob[i].o_data >= QUEST->num_mobs || real_mobile(QUEST->mob[QUEST->mob[i].o_data].vnum) < 0)) {
      target_is_listed_mob = FALSE;
    }
    struct char_data *target_ptr = fetch_quest_mob_target_mob_proto(QUEST, i);

    switch (QUEST->mob[i].objective) {
      case QUEST_NONE:
      case QMO_LOCATION:
      case QMO_KILL_ONE:
      case QMO_KILL_MANY:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "\r\n    Award %d (%d) nuyen & %0.2f (%0.2f) karma for ",
                 (int) (QUEST->mob[i].nuyen * NUYEN_GAIN_MULTIPLIER),
                 QUEST->mob[i].nuyen,
                 ((float)QUEST->mob[i].karma / 100) * KARMA_GAIN_MULTIPLIER,
                 ((float)QUEST->mob[i].karma / 100));

        if (QUEST->mob[i].objective == QUEST_NONE) {
          strlcat(buf, "nothing\r\n", sizeof(buf));
        } else if (QUEST->mob[i].objective == QMO_LOCATION) {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "escorting target to room %ld\r\n", QUEST->mob[i].o_data);
        } else if (QUEST->mob[i].objective == QMO_KILL_ONE) {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "killing target '%s' (%ld)\r\n",
                   GET_CHAR_NAME(mob_ptr),
                   QUEST->mob[i].vnum);
        } else if (QUEST->mob[i].objective == QMO_KILL_MANY) {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "each target '%s' (%ld) killed\r\n",
                   GET_CHAR_NAME(mob_ptr),
                   QUEST->mob[i].vnum);
        }
        break;
      case QMO_KILL_ESCORTEE:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Target hunts %s%ld (%s)\r\n", 
                 target_is_listed_mob ? "M" : "vnum ",
                 QUEST->mob[i].o_data,
                 GET_CHAR_NAME(target_ptr));
        break;
      case QMO_DONT_KILL:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "\r\n    Fail quest if target '%s' (%ld) is killed.\r\n",
                 GET_CHAR_NAME(mob_ptr),
                 QUEST->mob[i].vnum);
        break;
      default:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "\r\n - Unknown QMO %d\r\n", QUEST->mob[i].objective);
        break;
    }
  }
  send_to_char(buf, CH);
}

void qedit_disp_emote_menu(struct descriptor_data *d, int mode)
{
  emote_vector_t *vect;

  #define EMOTE_MENU_SWITCH_CASE(case_qualifier, emote_vector) \
     case (case_qualifier):                                    \
       if (!(emote_vector)) {                                  \
         (emote_vector) = new emote_vector_t;                  \
       }                                                       \
       vect = (emote_vector);                                  \
       break;                                                  \

  switch (mode) {
    EMOTE_MENU_SWITCH_CASE(QEDIT_EMOTE_MENU__INFO_EMOTES, QUEST->info_emotes)
    default:
      mudlog("SYSERR: Got unknown mode to qedit_disp_emote_menu()!", CH, LOG_SYSLOG, TRUE);
      return;
  }
  #undef EMOTE_MENU_SWITCH_CASE

  if (vect->empty()) {
    send_to_char("No emotes currently defined.\r\n", CH);
  } else {
    int i = 0;

    send_to_char("Emotes list:\r\n", CH);
    for (auto a: *(vect)) {
      send_to_char(CH, "%d)  %s\r\n\r\n", ++i, a);
    }
    send_to_char("\r\n", CH);
  }

  send_to_char(CH, " a) %s\r\n"
               " d) Delete an existing emote\r\n"
               " e) Edit (replace) an existing emote\r\n"
               " i) Insert a new emote before another\r\n"
               " q) Return to main menu\r\n"
               "Enter your choice: ", vect->empty() ? "Add the first emote" : "Append a new emote to the end of the list");

  d->edit_number2 = 0;
  d->edit_number3 = mode;
  d->edit_mode = QEDIT_EMOTE_MENU;
}

void insert_or_append_emote_at_position(struct descriptor_data *d, const char *string) {
  emote_vector_t *emote_vector;

  log_vfprintf("Entered insert_or_append_emote_at_position with '%s', edit_number2 = %ld, edit_number3 = %ld.",
               string,
               d->edit_number2,
               d->edit_number3);

  switch (d->edit_number3) {
    case QEDIT_EMOTE_MENU__INFO_EMOTES:
      emote_vector = QUEST->info_emotes;
      break;
    default:
      mudlog("SYSERR: Unknown emote menu mode in insert_or_append_emote_at_position(), extend switch!", CH, LOG_SYSLOG, TRUE);
      return;
  }

  // Append mode.
  if (d->edit_number2 == -1 || emote_vector->empty()) {
    log("Pushing back.");
    emote_vector->push_back(str_dup(string));
    return;
  }

  // Replace mode.
  for (auto it = emote_vector->begin(); it != emote_vector->end(); it++) {
    if ((d->edit_number2)-- == 0) {
      log("Inserting.");
      emote_vector->insert(it, str_dup(string));
      return;
    }
  }

  // If we got here, we found nothing to replace.
  log_vfprintf("Pushing back (fallthrough case). edit_number2: %ld", d->edit_number2);
  emote_vector->push_back(str_dup(string));
}

void qedit_disp_obj_menu(struct descriptor_data *d)
{
  send_to_char(CH, "Item objective menu:\r\n"
               " 1) List current objectives\r\n"
               " 2) Edit an existing objective\r\n"
               " 3) Add a new objective (%d slots remaining)\r\n"
               " q) Return to main menu\r\n"
               "Enter your choice: ", QMAX_OBJS - QUEST->num_objs);

  d->edit_number2 = 0;
  d->edit_mode = QEDIT_O_MENU;
}

void qedit_disp_mob_menu(struct descriptor_data *d)
{
  send_to_char(CH, "Mobile objective menu:\r\n"
               " 1) List current objectives\r\n"
               " 2) Edit an existing objective\r\n"
               " 3) Add a new objective (%d slots remaining)\r\n"
               " q) Return to main menu\r\n"
               "Enter your choice: ", QMAX_MOBS - QUEST->num_mobs);

  d->edit_number2 = 0;
  d->edit_mode = QEDIT_M_MENU;
}

void qedit_disp_obj_loads(struct descriptor_data *d)
{
  int i;

  CLS(CH);

  for (i = 0; i < NUM_OBJ_LOADS; i++)
    send_to_char(CH, "%2d) %s\r\n", i, obj_loads[i]);

  send_to_char(CH, "Enter item load location: ");

  d->edit_mode = QEDIT_O_LOAD;
}

void qedit_disp_mob_loads(struct descriptor_data *d)
{
  int i;

  CLS(CH);

  for (i = 0; i < NUM_MOB_LOADS; i++)
    send_to_char(CH, "%2d) %s\r\n", i, mob_loads[i]);

  send_to_char(CH, "Enter mobile load location: ");

  d->edit_mode = QEDIT_M_LOAD;
}

void qedit_disp_obj_objectives(struct descriptor_data *d)
{
  int i;

  CLS(CH);

  for (i = 0; i < NUM_OBJ_OBJECTIVES; i++)
    send_to_char(CH, "%2d) %s\r\n", i, obj_objectives[i]);

  send_to_char(CH, "Enter item objective: ");

  d->edit_mode = QEDIT_O_OBJECTIVE;
}

void qedit_disp_mob_objectives(struct descriptor_data *d)
{
  int i;

  CLS(CH);

  for (i = 0; i < NUM_MOB_OBJECTIVES; i++)
    send_to_char(CH, "%2d) %s\r\n", i, mob_objectives[i]);

  send_to_char(CH, "Enter mob objective/task: ");

  d->edit_mode = QEDIT_M_OBJECTIVE;
}

void qedit_disp_locations(struct descriptor_data *d)
{
  int i;

  CLS(CH);

  for (i = 0; i < (NUM_WEARS - 1); i += 2)
    send_to_char(CH, "%2d) %-20s    %2d) %-20s\r\n", i + 1, short_where[i],
                 i + 2, i + 1 < (NUM_WEARS - 1) ? short_where[i + 1] : "");

  send_to_char(CH, "Enter wear location: ");
  d->edit_mode = QEDIT_O_LDATA2;
}

void qedit_disp_menu(struct descriptor_data *d)
{
  char s_time[10], e_time[10];

  CLS(CH);
  send_to_char(CH, "Quest number: %s%ld%s\r\n", CCCYN(CH, C_CMP),
               d->edit_number, CCNRM(CH, C_CMP));
  send_to_char(CH, "1) Johnson: %s%ld%s (%s%s%s)\r\n", CCCYN(CH, C_CMP),
               QUEST->johnson, CCNRM(CH, C_CMP), CCCYN(CH, C_CMP),
               real_mobile(QUEST->johnson) < 0 ? "null" :
               GET_NAME(mob_proto+real_mobile(QUEST->johnson)),
               CCNRM(CH, C_CMP));
  send_to_char(CH, "2) Time allowed (minutes): %s%d%s\r\n", CCCYN(CH, C_CMP),
               QUEST->time, CCNRM(CH, C_CMP));
  send_to_char(CH, "3) Reputation range: %s%d%s-%s%d%s\r\n", CCCYN(CH, C_CMP),
               QUEST->min_rep, CCNRM(CH, C_CMP), CCCYN(CH, C_CMP),
               QUEST->max_rep, CCNRM(CH, C_CMP));
  send_to_char(CH, "4) Bonus nuyen: %s%d%s\r\n", CCCYN(CH, C_CMP),
               QUEST->nuyen, CCNRM(CH, C_CMP));
  send_to_char(CH, "5) Bonus karma: %s%0.2f%s\r\n", CCCYN(CH, C_CMP),
               ((float)QUEST->karma / 100), CCNRM(CH, C_CMP));
  send_to_char(CH, "6) Item objective menu\r\n");
  send_to_char(CH, "7) Mobile objective menu\r\n");
  send_to_char(CH, "\r\n");
  send_to_char(CH, "8a) Intro speech:      %s%s%s\r\n",
                    CCCYN(CH, C_CMP),
                    QUEST->intro_emote ? "<overridden by emotes>" : QUEST->intro,
                    CCNRM(CH, C_CMP));
  send_to_char(CH, "8b) Intro emote:       %s%s%s\r\n", CCCYN(CH, C_CMP), QUEST->intro_emote ? QUEST->intro_emote : "<not set>", CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
  send_to_char(CH, "9a) Decline speech:    %s%s%s\r\n",
                   CCCYN(CH, C_CMP),
                   QUEST->decline_emote ?  "<overridden by emotes>" : QUEST->decline,
                   CCNRM(CH, C_CMP));
  send_to_char(CH, "9b) Decline emote:     %s%s%s\r\n", CCCYN(CH, C_CMP), QUEST->decline_emote ? QUEST->decline_emote : "<not set>", CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
#ifdef USE_QUEST_LOCATION_CODE
  send_to_char(CH, "0) Location hint:      %s%s%s\r\n", CCCYN(CH, C_CMP),
               QUEST->decline, CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
#endif
  send_to_char(CH, "aa) Quit speech%s%s%s%s\r\n",
                   QUEST->quit_emote ? " (overridden by emotes in person, still seen in ENDRUN):" : ":       ",
                   CCCYN(CH, C_CMP),
                   QUEST->quit,
                   CCNRM(CH, C_CMP));
  send_to_char(CH, "ab) Quit emotes:       %s%s%s\r\n", CCCYN(CH, C_CMP), QUEST->quit_emote ? QUEST->quit_emote : "<not set>", CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
  send_to_char(CH, "ba) Completion speech: %s%s%s\r\n",
                   CCCYN(CH, C_CMP),
                   QUEST->finish_emote ? "<overridden by emotes>" : QUEST->finish,
                   CCNRM(CH, C_CMP));
  send_to_char(CH, "bb) Completion emotes: %s%s%s\r\n", CCCYN(CH, C_CMP), QUEST->finish_emote ? QUEST->finish_emote : "<not set>", CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
  send_to_char(CH, "ca) Accepted text (%s):\r\n%s%s%s\r\n\r\n", QUEST->info_emotes && !QUEST->info_emotes->empty() ? "emotes set: this is recap only" : "speech and recap", CCCYN(CH, C_CMP), QUEST->info, CCNRM(CH, C_CMP));
  send_to_char(CH, "cb) Accepted emotes (overrides speech if set):  %s%s%s\r\n", CCCYN(CH, C_CMP), (!QUEST->info_emotes || QUEST->info_emotes->empty()) ? "<not set>" : "<set>", CCNRM(CH, C_CMP));
  send_to_char(CH, "\r\n");
  /*
   * Determine what to print for the times the Johnson is out
   */
  if ( QUEST->s_time == -1 )
  {
    strcpy(s_time,"Random");
    e_time[0] = '\0';
  } else if ( QUEST->s_time == QUEST->e_time )
  {
    strcpy(s_time,"Always");
    e_time[0] = '\0';
  } else
  {
    snprintf(s_time, sizeof(s_time), "%d - ", QUEST->s_time);
    snprintf(e_time, sizeof(e_time), "%d", QUEST->e_time);
  }
  send_to_char(CH, "d) Johnson hours: %s%s%s%s\r\n", CCCYN(CH, C_CMP),
               s_time, e_time, CCNRM(CH, C_CMP));

  send_to_char(CH, "e) Start work message: %s%s%s\r\n", CCCYN(CH, C_CMP),
               QUEST->s_string, CCNRM(CH, C_CMP));
  send_to_char(CH, "f) End work message: %s%s%s\r\n", CCCYN(CH, C_CMP),
               QUEST->e_string, CCNRM(CH, C_CMP));
  send_to_char(CH, "g) Quest already completed message: %s%s%s\r\n", CCCYN(CH, C_CMP),
               QUEST->done, CCNRM(CH, C_CMP));

  rnum_t real_obj;
  send_to_char(CH, "h) Item Reward: %s%ld%s (%s%s%s)\r\n", CCCYN(CH, C_CMP),
                QUEST->reward, CCNRM(CH, C_CMP), CCCYN(CH, C_CMP),
                (real_obj = real_object(QUEST->reward)) <= 0 ? "no item reward" : obj_proto[real_obj].text.name,
                CCNRM(CH, C_CMP));

  send_to_char(CH, "i) Prerequisite quest: %s%ld%s\r\n", CCCYN(CH, C_CMP), QUEST->prerequisite_quest, CCNRM(CH, C_CMP));
  send_to_char(CH, "j) Disqualifying quest: %s%ld%s\r\n", CCCYN(CH, C_CMP), QUEST->disqualifying_quest, CCNRM(CH, C_CMP));

  send_to_char("q) Quit and save\r\n", CH);
  send_to_char("x) Exit and abort\r\n", CH);
  send_to_char("Enter your choice:\r\n", CH);
  d->edit_mode = QEDIT_MAIN_MENU;
}

#define SET_QUEST_EMOTE_IF_ARG_HAS_CONTENTS(emote_to_set) {  \
  delete [] emote_to_set;                                    \
  if (*arg) {                                                \
    char arg_mutable[MAX_STRING_LENGTH];                     \
    strlcpy(arg_mutable, arg, sizeof(arg_mutable));          \
    delete_doubledollar(arg_mutable);                        \
    emote_to_set = str_dup(arg_mutable);                     \
  } else {                                                   \
    emote_to_set = NULL;                                     \
  }                                                          \
}

void qedit_parse(struct descriptor_data *d, const char *arg)
{
  int number;
  float karma;

  switch (d->edit_mode)
  {
  case QEDIT_CONFIRM_EDIT:
    switch (*arg) {
    case 'y':
    case 'Y':
      qedit_disp_menu(d);
      break;
    case 'n':
    case 'N':
      STATE(d) = CON_PLAYING;
      free_quest(d->edit_quest);
      delete d->edit_quest;
      d->edit_quest = NULL;
      d->edit_number = 0;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      break;
    default:
      send_to_char("That's not a valid choice!\r\n", CH);
      send_to_char("Do you wish to edit it?\r\n", CH);
      break;
    }
    break;
  case QEDIT_CONFIRM_SAVESTRING:
    switch(*arg) {
    case 'y':
    case 'Y':
#ifdef ONLY_LOG_BUILD_ACTIONS_ON_CONNECTED_ZONES
      if (!vnum_from_non_approved_zone(d->edit_number)) {
#else
      {
#endif
        snprintf(buf, sizeof(buf),"%s wrote new quest #%ld",
                GET_CHAR_NAME(d->character), d->edit_number);
        mudlog(buf, d->character, LOG_WIZLOG, TRUE);
      }

      if (real_quest(d->edit_number) == -1)
        boot_one_quest(QUEST);
      else
        reboot_quest(real_quest(d->edit_number), QUEST);
      if (!write_quests_to_disk(d->character->player_specials->saved.zonenum))
        send_to_char("There was an error in writing the zone's quests.\r\n", d->character);
      free_quest(d->edit_quest);
      delete d->edit_quest;
      d->edit_quest = NULL;
      d->edit_number = 0;
      d->edit_number2 = 0;
      STATE(d) = CON_PLAYING;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      send_to_char("Done.\r\n", d->character);
      break;
    case 'n':
    case 'N':
      send_to_char("Quest not saved, aborting.\r\n", d->character);
      STATE(d) = CON_PLAYING;
      free_quest(d->edit_quest);
      delete d->edit_quest;
      d->edit_quest = NULL;
      d->edit_number = 0;
      d->edit_number2 = 0;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      break;
    default:
      send_to_char("Invalid choice!\r\n", d->character);
      send_to_char("Do you wish to save this quest internally?\r\n", d->character);
      break;
    }
    break;
  case QEDIT_MAIN_MENU:
    switch (*arg) {
      case 'q':
      case 'Q':
        d->edit_mode = QEDIT_CONFIRM_SAVESTRING;
        qedit_parse(d, "y");
        break;
      case 'x':
      case 'X':
        d->edit_mode = QEDIT_CONFIRM_SAVESTRING;
        qedit_parse(d, "n");
        break;
      case '1':
        send_to_char("Enter Johnson's vnum: ", CH);
        d->edit_mode = QEDIT_JOHNSON;
        break;
      case '2':
        send_to_char("Enter allowed time (in mud minutes): ", CH);
        d->edit_mode = QEDIT_TIME;
        break;
      case '3':
        send_to_char("Enter minimum reputation: ", CH);
        d->edit_mode = QEDIT_MIN_REP;
        break;
      case '4':
        send_to_char("Enter bonus nuyen: ", CH);
        d->edit_mode = QEDIT_NUYEN;
        break;
      case '5':
        send_to_char("Enter bonus karma: ", CH);
        d->edit_mode = QEDIT_KARMA;
        break;
      case '6':
        CLS(CH);
        qedit_disp_obj_menu(d);
        break;
      case '7':
        CLS(CH);
        qedit_disp_mob_menu(d);
        break;
      case '8':
        switch(*(arg + 1)) {
          case 'a':
            send_to_char("Enter the intro text that will be spoken by the Johnson: ", d->character);
            d->edit_mode = QEDIT_INTRO;
            break;
          case 'b':
            send_to_char("Enter the intro emote that will be displayed by the Johnson (use $N for the player): ", d->character);
            d->edit_mode = QEDIT_INTRO_EMOTE;
            break;
          default:
            qedit_disp_menu(d);
            break;
        }
        break;
      case '9':
        switch(*(arg + 1)) {
          case 'a':
            send_to_char("Enter the decline text that will be spoken by the Johnson: ", d->character);
            d->edit_mode = QEDIT_DECLINE;
            break;
          case 'b':
            send_to_char("Enter the decline emote that will be displayed by the Johnson (use $N for the player): ", d->character);
            d->edit_mode = QEDIT_DECLINE_EMOTE;
            break;
          default:
            qedit_disp_menu(d);
            break;
        }
        break;
  #ifdef USE_QUEST_LOCATION_CODE
      case '0':
        send_to_char("Enter a description of the Johnson's location (ex: 'a booth on the second level of Dante's Inferno'): ", d->character);
        d->edit_mode = QEDIT_LOCATION;
        break;
  #endif
      case 'a':
      case 'A':
        switch(*(arg + 1)) {
          case 'a':
            send_to_char("Enter the quit text that will be spoken by the Johnson: ", d->character);
            d->edit_mode = QEDIT_QUIT;
            break;
          case 'b':
          send_to_char("Enter the quit emote that will be displayed by the Johnson (use $N for the player): ", d->character);
          d->edit_mode = QEDIT_QUIT_EMOTE;
            break;
          default:
            qedit_disp_menu(d);
            break;
        }
        break;
      case 'b':
      case 'B':
        switch(*(arg + 1)) {
          case 'a':
            send_to_char("Enter the completion text that will be spoken by the Johnson: ", d->character);
            d->edit_mode = QEDIT_FINISH;
            break;
          case 'b':
            send_to_char("Enter the completion emote that will be displayed by the Johnson (use $N for the player): ", d->character);
            d->edit_mode = QEDIT_FINISH_EMOTE;
            break;
          default:
            qedit_disp_menu(d);
            break;
        }
        break;
      case 'c':
      case 'C':
        switch(*(arg + 1)) {
          case 'a':
            send_to_char("Enter informational text:\r\n", d->character);
            d->edit_mode = QEDIT_INFO;
            DELETE_D_STR_IF_EXTANT(d);
            INITIALIZE_NEW_D_STR(d);
            d->max_str = MAX_MESSAGE_LENGTH;
            d->mail_to = 0;
            break;
          case 'b':
            qedit_disp_emote_menu(d, QEDIT_EMOTE_MENU__INFO_EMOTES);
            break;
          default:
            qedit_disp_menu(d);
            break;
        }
        break;
      case 'd':
      case 'D':
        send_to_char("Enter hour for Johnson to start giving jobs: ", CH);
        d->edit_mode = QEDIT_SHOUR;
        break;
      case 'e':
      case 'E':
        send_to_char("Enter the string that will be given when the Johnson comes to work:\r\n", CH);
        d->edit_mode = QEDIT_SSTRING;
        break;
      case 'f':
      case 'F':
        send_to_char("Enter the string that will be given when the Johnson leaves work:\r\n", CH);
        d->edit_mode = QEDIT_ESTRING;
        break;
      case 'g':
      case 'G':
        send_to_char("Enter the string that will be given if quest is already complete:\r\n", CH);
        d->edit_mode = QEDIT_DONE;
        break;
      case 'h':
      case 'H':
        if (!access_level(CH, LEVEL_REQUIRED_TO_ADD_ITEM_REWARDS)) {
          send_to_char(CH, "Sorry, you don't have access to set that. Ask someone rank %d or higher.\r\n", LEVEL_REQUIRED_TO_ADD_ITEM_REWARDS);
          qedit_disp_menu(d);
        } else {
          send_to_char("Enter vnum of reward (-1 for nothing): ", CH);
          d->edit_mode = QEDIT_REWARD;
        }
        break;
      case 'i':
      case 'I':
        send_to_char("Enter the vnum of the quest that must be done before this (0 for no prerequisite):\r\n", CH);
        d->edit_mode = QEDIT_PREREQUISITE;
        break;
      case 'j':
      case 'J':
        send_to_char("Enter the vnum of the quest that disqualifies you from this quest if complete (0 for no DQ):\r\n", CH);
        d->edit_mode = QEDIT_DISQUALIFYING;
        break;
      default:
        qedit_disp_menu(d);
        break;
      }
    break;
  case QEDIT_JOHNSON:
    number = atoi(arg);
    if (real_mobile(number) < 0) {
      send_to_char("No such mob!  Enter Johnson's vnum: ", CH);
      return;
    } else {
      QUEST->johnson = number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_PREREQUISITE:
    number = atoi(arg);
    if (number == 0 || real_quest(number) >= 0) {
      QUEST->prerequisite_quest = number;
      qedit_disp_menu(d);
    } else {
      send_to_char("That's not a valid quest vnum. Enter a vnum, or 0 for no quest: ", CH);
    }
    return;
  case QEDIT_DISQUALIFYING:
    number = atoi(arg);
    if (number == 0 || real_quest(number) >= 0) {
      QUEST->disqualifying_quest = number;
      qedit_disp_menu(d);
    } else {
      send_to_char("That's not a valid quest vnum. Enter a vnum, or 0 for no quest: ", CH);
    }
    return;
  case QEDIT_TIME:
    number = atoi(arg);
    if (number < 30 || number > 1440)
      send_to_char("Time must range from 30 to 1440 mud minutes.\r\n"
                   "Enter allowed time: ", CH);
    else {
      QUEST->time = number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_MIN_REP:
    number = atoi(arg);
    if (number < 0 || number > 1500)
      send_to_char("Invalid value.  Enter minimum reputation between 0-1500: ", CH);
    else {
      QUEST->min_rep = number;
      send_to_char("Enter maximum reputation (10000 for no limit): ", CH);
      d->edit_mode = QEDIT_MAX_REP;
    }
    break;
  case QEDIT_MAX_REP:
    number = atoi(arg);
    if ((unsigned)number < QUEST->min_rep)
      send_to_char("Maximum reputation must be greater than the minimum.\r\n"
                   "Enter maximum reputation: ", CH);
    else {
      QUEST->max_rep = number > 10000 ? 10000 : number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_NUYEN:
    number = atoi(arg);
    if (number < 0 || number > 500000)
      send_to_char("Invalid amount.  Enter bonus nuyen: ", CH);
    else {
      QUEST->nuyen = number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_KARMA:
    karma = atof(arg);
    if (karma < 0.0 || karma > 25.0)
      send_to_char("Invalid amount.  Enter bonus karma: ", CH);
    else {
      QUEST->karma = (int)(karma * 100);
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_M_MENU:
    switch (*arg) {
    case '1':
      qedit_list_mob_objectives(d);
      qedit_disp_mob_menu(d);
      break;
    case '2':
      send_to_char("Enter number of mobile objective to edit: ", CH);
      d->edit_mode = QEDIT_M_AWAIT_NUMBER;
      break;
    case '3':
      if (QUEST->num_mobs < QMAX_MOBS) {
        d->edit_number2 = QUEST->num_mobs;
        QUEST->num_mobs++;
        send_to_char("Enter vnum of mob: ", CH);
        d->edit_mode = QEDIT_M_VNUM;
      } else {
        CLS(CH);
        qedit_disp_mob_menu(d);
      }
      break;
    case 'q':
    case 'Q':
      qedit_disp_menu(d);
      break;
    default:
      CLS(CH);
      qedit_disp_mob_menu(d);
      break;
    }
    break;
  case QEDIT_M_AWAIT_NUMBER:
    number = atoi(arg);
    if (number < 0 || number >= QUEST->num_mobs) {
      CLS(CH);
      qedit_disp_mob_menu(d);
    } else {
      d->edit_number2 = number;
      d->edit_mode = QEDIT_M_VNUM;
      send_to_char("Enter vnum of mob: ", CH);
    }
    break;
  case QEDIT_M_VNUM:
    number = atoi(arg);
    if (real_mobile(number) < 0)
      send_to_char("No such mob.  Enter vnum of mob: ", CH);
    else {
      QUEST->mob[d->edit_number2].vnum = number;
      d->edit_mode = QEDIT_M_NUYEN;
      send_to_char("Enter nuyen reward: ", CH);
    }
    break;
  case QEDIT_M_NUYEN:
    number = atoi(arg);
    if (number < 0 || number > 25000)
      send_to_char("Invalid amount.  Enter nuyen reward: ", CH);
    else {
      QUEST->mob[d->edit_number2].nuyen = number;
      d->edit_mode = QEDIT_M_KARMA;
      send_to_char("Enter karma reward: ", CH);
    }
    break;
  case QEDIT_M_KARMA:
    karma = atof(arg);
    if (karma < 0.0 || karma > 5.0)
      send_to_char("Invalid amount.  Enter karma reward: ", CH);
    else {
      QUEST->mob[d->edit_number2].karma = (int)(karma * 100);
      qedit_disp_mob_loads(d);
    }
    break;
  case QEDIT_M_LOAD:
    number = atoi(arg);
    if (number < 0 || number >= NUM_MOB_LOADS)
      qedit_disp_mob_loads(d);
    else {
      QUEST->mob[d->edit_number2].load = (byte)(number);
      switch (QUEST->mob[d->edit_number2].load) {
      case QML_LOCATION:
        d->edit_mode = QEDIT_M_LDATA;
        send_to_char("Enter vnum of room to load mob into: ", CH);
        break;
      default:
        qedit_disp_mob_objectives(d);
        break;
      }
    }
    break;
  case QEDIT_M_OBJECTIVE:
    number = atoi(arg);
    if (number < 0 || number >= NUM_MOB_OBJECTIVES)
      qedit_disp_mob_objectives(d);
    else {
      QUEST->mob[d->edit_number2].objective = (byte)(number);
      switch (QUEST->mob[d->edit_number2].objective) {
      case QMO_LOCATION:
        d->edit_mode = QEDIT_M_ODATA;
        send_to_char(CH, "Enter vnum of room mob must be led to: ");
        break;
      case QMO_KILL_ESCORTEE:
        d->edit_mode = QEDIT_M_ODATA;
        send_to_char(CH, "Enter M# of mob to hunt ('l' to list, 'q' to quit): ");
        break;
      default:
        CLS(CH);
        qedit_disp_mob_menu(d);
        break;
      }
    }
    break;
  case QEDIT_M_LDATA:
    // only if the mob_load type is QML_LOCATION will we ever reach this
    number = atoi(arg);
    if (real_room(number) < 0)
      send_to_char(CH, "Invalid room.  Enter vnum of room to load mob into: ");
    else {
      QUEST->mob[d->edit_number2].l_data = number;
      qedit_disp_mob_objectives(d);
    }
    break;
  case QEDIT_M_LDATA2:
    break;             // unused (for now)
  case QEDIT_M_ODATA:
    number = atoi(arg);
    switch (QUEST->mob[d->edit_number2].objective) {
    case QMO_LOCATION:
      if (real_room(number) < 0)
        send_to_char(CH, "Invalid room.  Enter vnum of room mob "
                     "must be led to: ");
      else {
        QUEST->mob[d->edit_number2].o_data = number;
        qedit_disp_mob_menu(d);
      }
      break;
    case QMO_KILL_ESCORTEE:
      if (*arg == 'q' || *arg == 'Q') {
        QUEST->mob[d->edit_number2].vnum = 0;
        QUEST->mob[d->edit_number2].nuyen = 0;
        QUEST->mob[d->edit_number2].karma = 0;
        QUEST->mob[d->edit_number2].load = 0;
        QUEST->mob[d->edit_number2].objective = 0;
        QUEST->mob[d->edit_number2].l_data = 0;
        QUEST->mob[d->edit_number2].l_data2 = 0;
        QUEST->mob[d->edit_number2].o_data = 0;
        QUEST->num_mobs--;
        CLS(CH);
        qedit_disp_mob_menu(d);
      } else if (*arg == 'l' || *arg == 'L') {
        qedit_list_mob_objectives(d);
        send_to_char(CH, "Enter M# of mob to hunt ('l' to list, 'q' to quit): ");
      } else if (number < 0 || number >= QUEST->num_mobs)
        send_to_char(CH, "Invalid response. Enter M# of mob to hunt ('l' to list, 'q' to quit): ");
      else {
        QUEST->mob[d->edit_number2].o_data = number;
        CLS(CH);
        qedit_disp_mob_menu(d);
      }
      break;
    }
    break;
  case QEDIT_EMOTE_MENU:
    switch (*arg) {
      case 'a':
      case 'A':
        // add new at end
        send_to_char("Write your new emote: ", CH);
        d->edit_mode = QEDIT_EMOTE__INSERT_EMOTE_BEFORE;
        d->edit_number2 = -1;
        break;
      case 'd':
      case 'D':
        // delete existing
        send_to_char("Enter emote number to delete (0 to abort): ", CH);
        d->edit_mode = QEDIT_EMOTE__AWAIT_NUMBER_FOR_DELETION;
        break;
      case 'e':
      case 'E':
      case 'r':
      case 'R':
        // edit existing
        send_to_char("Enter emote number to edit (0 to abort): ", CH);
        d->edit_mode = QEDIT_EMOTE__AWAIT_NUMBER_FOR_EDIT;
        break;
      case 'i':
      case 'I':
        // insert new
        send_to_char("Enter emote number to insert BEFORE (0 to abort): ", CH);
        d->edit_mode = QEDIT_EMOTE__AWAIT_NUMBER_FOR_INSERT_BEFORE;
        break;
      case 'q':
      case 'Q':
        qedit_disp_menu(d);
        break;
      default:
        CLS(CH);
        qedit_disp_emote_menu(d, d->edit_number3);
        break;
    }
    break;
  case QEDIT_O_MENU:
    switch (*arg) {
    case '1':
      qedit_list_obj_objectives(d);
      qedit_disp_obj_menu(d);
      break;
    case '2':
      send_to_char("Enter number of item objective to edit: ", CH);
      d->edit_mode = QEDIT_O_AWAIT_NUMBER;
      break;
    case '3':
      if (QUEST->num_objs < QMAX_OBJS) {
        d->edit_number2 = QUEST->num_objs;
        QUEST->num_objs++;
        send_to_char("Enter vnum of item (0 for nothing): ", CH);
        d->edit_mode = QEDIT_O_VNUM;
      } else {
        CLS(CH);
        qedit_disp_obj_menu(d);
      }
      break;
    case 'q':
    case 'Q':
      qedit_disp_menu(d);
      break;
    default:
      CLS(CH);
      qedit_disp_obj_menu(d);
      break;
    }
    break;
  case QEDIT_EMOTE__INSERT_EMOTE_BEFORE:
    // We only ever get here when writing an intro emote. Just plop it in and go.
    {
      char mutable_arg[MAX_STRING_LENGTH];
      strlcpy(mutable_arg, arg, sizeof(mutable_arg));
      insert_or_append_emote_at_position(d, delete_doubledollar(mutable_arg));
      qedit_disp_emote_menu(d, d->edit_number3);
    }
    break;
  case QEDIT_EMOTE__AWAIT_NUMBER_FOR_EDIT:
  case QEDIT_EMOTE__AWAIT_NUMBER_FOR_DELETION:
  case QEDIT_EMOTE__AWAIT_NUMBER_FOR_INSERT_BEFORE:
    {
      emote_vector_t *emote_vector;
      switch (d->edit_number3) {
        case QEDIT_EMOTE_MENU__INFO_EMOTES:
          emote_vector = QUEST->info_emotes;
          break;
        default:
          mudlog("SYSERR: Unknown emote menu mode in qedit, extend switch!", CH, LOG_SYSLOG, TRUE);
          return;
      }

      number = atoi(arg);
      if (number == 0) {
        CLS(CH);
        qedit_disp_emote_menu(d, d->edit_number3);
      }
      else if (number < 1 || number > (int) emote_vector->size()) {
        send_to_char(CH, "Invalid number. Enter emote between 1-%ld, or 0 to abort: ", emote_vector->size());
      } else {
        if (number == 0) {
          d->edit_number2 = 0;
          qedit_disp_emote_menu(d, d->edit_number3);
          break;
        }

        d->edit_number2 = (number -= 1);
        switch (d->edit_mode) {
          case QEDIT_EMOTE__AWAIT_NUMBER_FOR_EDIT:
            // Essentially, delete the existing and write a new one.
            for (auto it = emote_vector->begin(); it != emote_vector->end(); it++) {
              if ((number)-- == 0) {
                DELETE_ENTRY_FROM_VECTOR_PTR(it, emote_vector);
                break;
              }
            }
            // fall through
          case QEDIT_EMOTE__AWAIT_NUMBER_FOR_INSERT_BEFORE:
            send_to_char(CH, "Write your %s emote: ", d->edit_mode == QEDIT_EMOTE__AWAIT_NUMBER_FOR_EDIT ? "revised" : "new");
            d->edit_mode = QEDIT_EMOTE__INSERT_EMOTE_BEFORE;
            // We know that d->edit_number3 has the emote type, so we're golden here.
            break;
          case QEDIT_EMOTE__AWAIT_NUMBER_FOR_DELETION:
            for (auto it = emote_vector->begin(); it != emote_vector->end(); it++) {
              if ((number)-- == 0) {
                DELETE_ENTRY_FROM_VECTOR_PTR(it, emote_vector);
                break;
              }
            }
            d->edit_number2 = 0;
            qedit_disp_emote_menu(d, d->edit_number3);
            break;
        }
      }
    }
    break;
  case QEDIT_O_AWAIT_NUMBER:
    number = atoi(arg);
    if (number < 0 || number >= QUEST->num_objs) {
      CLS(CH);
      qedit_disp_obj_menu(d);
    } else {
      d->edit_number2 = number;
      d->edit_mode = QEDIT_O_VNUM;
      send_to_char("Enter vnum of item (0 for nothing): ", CH);
    }
    break;
  case QEDIT_O_VNUM:
    number = atoi(arg);
    if (number != 0 && real_object(number) < 0)
      send_to_char("No such item.  Enter vnum of item (0 for nothing): ", CH);
    else {
      QUEST->obj[d->edit_number2].vnum = number;
      d->edit_mode = QEDIT_O_NUYEN;
      send_to_char("Enter nuyen reward: ", CH);
    }
    break;
  case QEDIT_O_NUYEN:
    number = atoi(arg);
    if (number < 0 || number > 25000)
      send_to_char("Invalid amount.  Enter nuyen reward: ", CH);
    else {
      QUEST->obj[d->edit_number2].nuyen = number;
      d->edit_mode = QEDIT_O_KARMA;
      send_to_char("Enter karma reward: ", CH);
    }
    break;
  case QEDIT_O_KARMA:
    karma = atof(arg);
    if (karma < 0.0 || karma > 5.0)
      send_to_char("Invalid amount.  Enter karma reward: ", CH);
    else {
      QUEST->obj[d->edit_number2].karma = (int)(karma * 100);
      qedit_disp_obj_loads(d);
    }
    break;
  case QEDIT_O_LOAD:
    number = atoi(arg);
    if (number < 0 || number >= NUM_OBJ_LOADS)
      qedit_disp_obj_loads(d);
    else {
      QUEST->obj[d->edit_number2].load = (byte)(number);
      switch (QUEST->obj[d->edit_number2].load) {
      case QOL_TARMOB_I:
        d->edit_mode = QEDIT_O_LDATA;
        send_to_char(CH, "Enter M# of mob to give item to: ('l' to list, 'q' to quit): ");
        break;
      case QOL_TARMOB_E:
        d->edit_mode = QEDIT_O_LDATA;
        send_to_char(CH, "Enter M# of mob to equip item on: ('l' to list, 'q' to quit): ");
        break;
      case QOL_TARMOB_C:
        d->edit_mode = QEDIT_O_LDATA;
        send_to_char(CH, "Enter M# of mob to install item in: ('l' to list, 'q' to quit): ");
        break;
      case QOL_HOST:
        d->edit_mode = QEDIT_O_LDATA;
        send_to_char(CH, "Enter vnum of host to load item into: ");
        break;
      case QOL_LOCATION:
        d->edit_mode = QEDIT_O_LDATA;
        send_to_char(CH, "Enter vnum of room to load item into: ");
        break;
      default:
        qedit_disp_obj_objectives(d);
        break;
      }
    }
    break;
  case QEDIT_O_OBJECTIVE:
    number = atoi(arg);
    if (number < 0 || number >= NUM_OBJ_OBJECTIVES)
      qedit_disp_obj_objectives(d);
    else {
      QUEST->obj[d->edit_number2].objective = (byte)(number);
      switch (QUEST->obj[d->edit_number2].objective) {
      case QOO_TAR_MOB:
        d->edit_mode = QEDIT_O_ODATA;
        send_to_char(CH, "Enter M# of mob item must be delivered to ('l' to list, 'q' to quit): ");
        break;
      case QOO_LOCATION:
        d->edit_mode = QEDIT_O_ODATA;
        send_to_char(CH, "Enter vnum of room item must be delivered to: ");
        break;
      case QOO_RETURN_PAY:
        QUEST->obj[d->edit_number2].vnum = OBJ_BLANK_OPTICAL_CHIP;
        // Fallthrough.
      case QOO_UPLOAD:
        d->edit_mode = QEDIT_O_ODATA;
        send_to_char(CH, "Enter vnum of host paydata must be retrieved/uploaded from: ");
        break;
      default:
        CLS(CH);
        qedit_disp_obj_menu(d);
        break;
      }
    }
    break;
  case QEDIT_O_LDATA:
    number = atoi(arg);
    switch (QUEST->obj[d->edit_number2].load) {
    case QOL_TARMOB_I:
      if (*arg == 'l' || *arg == 'L') {
        qedit_list_mob_objectives(d);
        send_to_char(CH, "Enter M# of mob to put item in inventory of: ('l' to list, 'q' to quit): ");
      } else if (number < 0 || number >= QUEST->num_mobs)
        send_to_char(CH, "Invalid response. Enter M# of mob to put item in inventory of: ('l' to list, 'q' to quit): ");
      else {
        QUEST->obj[d->edit_number2].l_data = number;
        qedit_disp_obj_objectives(d);
      }
      break;
    case QOL_TARMOB_E:
      if (*arg == 'l' || *arg == 'L') {
        qedit_list_mob_objectives(d);
        send_to_char(CH, "Enter M# of mob to equip item on: ('l' to list, 'q' to quit): ");
      } else if (number < 0 || number >= QUEST->num_mobs)
        send_to_char(CH, "Invalid response. Enter M# of mob to equip item on: ('l' to list, 'q' to quit): ");
      else {
        QUEST->obj[d->edit_number2].l_data = number;
        qedit_disp_locations(d);
      }
      break;
    case QOL_TARMOB_C:
      if (*arg == 'l' || *arg == 'L') {
        qedit_list_mob_objectives(d);
        send_to_char(CH, "Enter M# of mob to install item in: ('l' to list, 'q' to quit): ");
      } else  if (number < 0 || number >= QUEST->num_mobs)
        send_to_char(CH, "Invalid response. Enter M# of mob to install item in: ('l' to list, 'q' to quit): ");
      else {
        QUEST->obj[d->edit_number2].l_data = number;
        qedit_disp_obj_objectives(d);
      }
      break;
    case QOL_LOCATION:
      if (real_room(number) < 0)
        send_to_char(CH, "Enter vnum of room to load item into: ");
      else {
        QUEST->obj[d->edit_number2].l_data = number;
        qedit_disp_obj_objectives(d);
      }
      break;
    case QOL_HOST:
      if (real_host(number) < 0)
        send_to_char(CH, "Enter vnum of host to load item into: ");
      else {
        QUEST->obj[d->edit_number2].l_data = number;
        qedit_disp_obj_objectives(d);
      }
      break;
    }
    break;
  case QEDIT_O_LDATA2:
    // only QOL_TARMOB_E should ever make it here
    number = atoi(arg);
    if (number < 1 || number > (NUM_WEARS - 1))
      qedit_disp_locations(d);
    else {
      QUEST->obj[d->edit_number2].l_data2 = number - 1;
      qedit_disp_obj_objectives(d);
    }
    break;
  case QEDIT_O_ODATA:
    number = atoi(arg);
    switch (QUEST->obj[d->edit_number2].objective) {
    case QOO_TAR_MOB:
      if (*arg == 'q' || *arg == 'Q') {
        CLS(CH);
        qedit_disp_obj_menu(d);
      } else if (*arg == 'l' || *arg == 'L') {
        qedit_list_mob_objectives(d);
        send_to_char(CH, "Enter M# or vnum of the mob the item must be delivered to: ('l' to list, 'q' to quit): ");
      } else if (number < 0 || translate_quest_mob_identifier_to_rnum(number, QUEST) < 0)
        send_to_char(CH, "Invalid response. Enter M# or vnum of the mob the item must be delivered to: ('l' to list, 'q' to quit): ");
      else {
        QUEST->obj[d->edit_number2].o_data = number;
        CLS(CH);
        qedit_disp_obj_menu(d);
      }
      break;
    case QOO_LOCATION:
      if (real_room(number) < 0)
        send_to_char(CH, "That's not a valid room. Enter vnum of room item must be delivered to: ");
      else {
        QUEST->obj[d->edit_number2].o_data = number;
        CLS(CH);
        qedit_disp_obj_menu(d);
      }
      break;
    case QOO_UPLOAD:
    case QOO_RETURN_PAY:
      if (real_host(number) < 0)
        send_to_char(CH, "That's not a valid host. Enter vnum of host paydata must be retrieved/uploaded from: ");
      else {
        QUEST->obj[d->edit_number2].o_data = number;

        CLS(CH);
        qedit_disp_obj_menu(d);
      }
      break;
    }
    break;
  case QEDIT_INTRO:
    if (QUEST->intro)
      delete [] QUEST->intro;
    QUEST->intro = str_dup(arg);
    qedit_disp_menu(d);
    break;
  case QEDIT_INTRO_EMOTE:
    SET_QUEST_EMOTE_IF_ARG_HAS_CONTENTS(QUEST->intro_emote);
    qedit_disp_menu(d);
    break;
  case QEDIT_DECLINE_EMOTE:
    SET_QUEST_EMOTE_IF_ARG_HAS_CONTENTS(QUEST->decline_emote);
    qedit_disp_menu(d);
    break;
  case QEDIT_DECLINE:
    if (QUEST->decline)
      delete [] QUEST->decline;
    QUEST->decline = str_dup(arg);
    qedit_disp_menu(d);
    break;
#ifdef USE_QUEST_LOCATION_CODE
  case QEDIT_LOCATION:
    if (QUEST->location)
      delete [] QUEST->location;
    QUEST->location = str_dup(arg);
    qedit_disp_menu(d);
    break;
#endif
  case QEDIT_QUIT:
    if (QUEST->quit)
      delete [] QUEST->quit;
    QUEST->quit = str_dup(arg);
    qedit_disp_menu(d);
    break;
  case QEDIT_QUIT_EMOTE:
    SET_QUEST_EMOTE_IF_ARG_HAS_CONTENTS(QUEST->quit_emote);
    qedit_disp_menu(d);
    break;
  case QEDIT_FINISH:
    if (QUEST->finish)
      delete [] QUEST->finish;
    QUEST->finish = str_dup(arg);
    qedit_disp_menu(d);
    break;
  case QEDIT_FINISH_EMOTE:
    SET_QUEST_EMOTE_IF_ARG_HAS_CONTENTS(QUEST->finish_emote);
    qedit_disp_menu(d);
    break;
  case QEDIT_INFO:
    break;               // we should never get here
  case QEDIT_REWARD:
    number = atoi(arg);
    if (real_object(number) < -1)
      send_to_char(CH, "Invalid vnum.  Enter vnum of reward (-1 for nothing): ");
    else {
      QUEST->reward = number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_SHOUR:
    number = atoi(arg);
    if ( number > 23 || number < -1 ) {
      send_to_char("Needs to be between -1 and 23.\r\nWhat time does he start work? ", CH);
    } else {
      QUEST->s_time = number;
      d->edit_mode = QEDIT_EHOUR;
      send_to_char("Enter hour for Johnson to stop giving jobs: ", CH);
    }
    break;
  case QEDIT_EHOUR:
    number = atoi(arg);
    if ( number > 23 || number < 0 ) {
      send_to_char("Needs to be between 0 and 23.\r\nWhat time does he stop work? ", CH);
      return;
    } else {
      QUEST->e_time = number;
      qedit_disp_menu(d);
    }
    break;
  case QEDIT_SSTRING:
    if (QUEST->s_string)
      delete [] QUEST->s_string;
    QUEST->s_string = str_dup(arg);
    qedit_disp_menu(d);
    break;
  case QEDIT_ESTRING:
    if (QUEST->e_string)
      delete [] QUEST->e_string;
    QUEST->e_string = str_dup(arg);
    qedit_disp_menu(d);
    break;
  case QEDIT_DONE:
    if (QUEST->done)
      delete [] QUEST->done;
    QUEST->done = str_dup(arg);
    qedit_disp_menu(d);
    break;
  }
}

// Remotely end a run. Requires a phone.
ACMD(do_endrun) {
  struct obj_data *phone = NULL;

  // Must be on a quest.
  FAILURE_CASE(!GET_QUEST(ch), "But you're not on a run.");

  // Must type the whole command.
  FAILURE_CASE(subcmd == SCMD_QUI, "You must type the whole ^WENDRUN^n command to quit your job.");

  // Must have a phone.
  for (phone = ch->carrying; phone; phone = phone->next_content)
    if (GET_OBJ_TYPE(phone) == ITEM_PHONE)
      break;
  // Worn phones are OK.
  if (!phone)
    for (int x = 0; !phone && x < NUM_WEARS; x++)
      if (GET_EQ(ch, x) && GET_OBJ_TYPE(GET_EQ(ch, x)) == ITEM_PHONE)
        phone = GET_EQ(ch, x);
  // Cyberware phones are fine.
  if (!phone)
    for (phone = ch->cyberware; phone; phone = phone->next_content)
      if (GET_OBJ_VAL(phone, 0) == CYB_PHONE)
        break;

  // Drop the quest.
  for (struct char_data *johnson = character_list; johnson; johnson = johnson->next_in_character_list) {
    if (IS_NPC(johnson) && (GET_MOB_VNUM(johnson) == quest_table[GET_QUEST(ch)].johnson)) {
      if (phone) {
        send_to_char(ch, "You call your Johnson, and after a short wait the phone is picked up.\r\n"
                         "^Y%s on the other end of the line says, \"%s\"^n\r\n"
                         "With your run abandoned, you hang up the phone.\r\n",
                         GET_CHAR_NAME(johnson),
                         quest_table[GET_QUEST(ch)].quit);
        if (ch->in_room)
          act("$n makes a brief phone call to $s Johnson to quit $s current run. Scandalous.", FALSE, ch, 0, 0, TO_ROOM);
        snprintf(buf, sizeof(buf), "$z's phone rings. $e answers, listens for a moment, then says into it, \"%s\"", quest_table[GET_QUEST(ch)].quit);
        act(buf, FALSE, johnson, NULL, NULL, TO_ROOM);

        end_quest(ch, FALSE);
        forget(johnson, ch);
      } else if (ch->in_room && ch->in_room == johnson->in_room) {
        attempt_quit_job(ch, johnson);
      } else {
        send_to_char(ch, "You'll either need to head back and talk to %s^n in person or get a phone you can use to call %s.\r\n", GET_CHAR_NAME(johnson), HMHR(johnson));
      }
      return;
    }
  }

  // Error case.
  mudlog("SYSERR: Attempted remote job termination, but the Johnson could not be found!", ch, LOG_SYSLOG, TRUE);
  send_to_char("You dial your phone, but something's up with the connection, and you can't get through.\r\n", ch);
}

unsigned int get_johnson_overall_max_rep(struct char_data *johnson) {
  unsigned int max_rep = 0;

  bool johnson_is_from_disconnected_zone = vnum_from_non_approved_zone(GET_MOB_VNUM(johnson));
#ifdef IS_BUILDPORT
  johnson_is_from_disconnected_zone = TRUE;
#endif

  for (int i = 0; i <= top_of_questt; i++) {
    if (quest_table[i].johnson == GET_MOB_VNUM(johnson)
        && (johnson_is_from_disconnected_zone
            || !vnum_from_non_approved_zone(quest_table[i].vnum)))
    {
      max_rep = MAX(max_rep, quest_table[i].max_rep);
    }
  }

  return max_rep;
}

unsigned int get_johnson_overall_min_rep(struct char_data *johnson) {
  unsigned int min_rep = UINT_MAX;

  bool johnson_is_from_disconnected_zone = vnum_from_non_approved_zone(GET_MOB_VNUM(johnson));
  #ifdef IS_BUILDPORT
    johnson_is_from_disconnected_zone = TRUE;
  #endif

  for (int i = 0; i <= top_of_questt; i++) {
    if (quest_table[i].johnson == GET_MOB_VNUM(johnson)
        && (johnson_is_from_disconnected_zone
            || !vnum_from_non_approved_zone(quest_table[i].vnum)))
    {
      min_rep = MIN(min_rep, quest_table[i].min_rep);
    }
  }

  return min_rep;
}

// TODO: Have quests able to disable this printout (mystery quests etc)
void display_quest_goals_to_ch(struct char_data *ch) {
  rnum_t johnson_rnum = real_mobile(quest_table[GET_QUEST(ch)].johnson);
  struct char_data *johnson = (johnson_rnum >= 0 ? &mob_proto[johnson_rnum] : NULL);

  send_to_char("\r\nObjectives:\r\n", ch);

  // Check mob objectives.
  for (int objective_idx = 0; objective_idx < quest_table[GET_QUEST(ch)].num_mobs; objective_idx++) {
    if (quest_table[GET_QUEST(ch)].mob[objective_idx].objective == QMO_NO_OBJECTIVE || quest_table[GET_QUEST(ch)].mob[objective_idx].objective == QMO_KILL_ESCORTEE)
      continue;

    struct char_data *mob = fetch_quest_mob_actual_mob_proto(&quest_table[GET_QUEST(ch)], objective_idx);

    rnum_t room_rnum = real_room(quest_table[GET_QUEST(ch)].mob[objective_idx].o_data);
    struct room_data *room = room_rnum >= 0 ? &world[room_rnum] : NULL;

    switch (quest_table[GET_QUEST(ch)].mob[objective_idx].objective) {
      case QUEST_NONE:
      case QMO_KILL_ESCORTEE:
        continue;
      case QMO_LOCATION:
        send_to_char(ch, " - Escort %s to %s (%s)\r\n", GET_CHAR_NAME(mob), GET_ROOM_NAME(room), ch->player_specials->mob_complete[objective_idx] ? "^gdone^n" : "incomplete");
        break;
      case QMO_KILL_ONE:
        send_to_char(ch, " - Kill %s (%s)\r\n", GET_CHAR_NAME(mob), ch->player_specials->mob_complete[objective_idx] ? "^gdone^n" : "incomplete");
        break;
      case QMO_KILL_MANY:
        send_to_char(ch, " - Kill as many %s as you can (^c%d^n killed)\r\n", GET_CHAR_NAME(mob), ch->player_specials->mob_complete[objective_idx]);
        break;
      case QMO_DONT_KILL:
        send_to_char(ch, " - Ensure %s survives (%s)\r\n", GET_CHAR_NAME(mob), ch->player_specials->mob_complete[objective_idx] == -1 ? "^rFAILED^n" : "^gso far, so good^n");
        break;
      default:
        continue;
    }

  }

  // Check object objectives.
  for (int objective_idx = 0; objective_idx < quest_table[GET_QUEST(ch)].num_objs; objective_idx++) {
    if (quest_table[GET_QUEST(ch)].obj[objective_idx].objective == QOO_NO_OBJECTIVE)
      continue;

    // 1. Find and deliver 'an envelope with weird insignias on it' to Ricky Skeezeball (done)
    // 2. Kill as many 'a bodyguard' as you can (14 killed)
    // 3. Deliver 'an electronics kit' to Janine Reyes (incomplete)
    // 4. Destroy 'the occupant's sense of safety' (done)
    // 5. Upload 'a virus' to the matrix host 'In the Dojo' (incomplete)
    // 6. Don't kill 'a postal worker' (failed)

    // ch->player_specials->obj_complete[objective_idx]

    strlcpy(buf, " - ", sizeof(buf));

    rnum_t obj_rnum = real_object(quest_table[GET_QUEST(ch)].obj[objective_idx].vnum);
    struct obj_data *obj = (obj_rnum >= 0 ? &obj_proto[obj_rnum] : NULL);

    switch (quest_table[GET_QUEST(ch)].obj[objective_idx].load) {
      case QUEST_NONE:
      case QOL_TARMOB_C:
        break;
      case QOL_JOHNSON:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Take '%s' ", GET_OBJ_NAME(obj));
        break;
      case QOL_TARMOB_I:
      case QOL_TARMOB_E:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Retrieve '%s' ", GET_OBJ_NAME(obj));
        break;
      case QOL_HOST:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Download '%s' ", GET_OBJ_NAME(obj));
        break;
      case QOL_LOCATION:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Locate '%s' ", GET_OBJ_NAME(obj));
        break;
      default:
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Obtain '%s' ", GET_OBJ_NAME(obj));
        break;
    }

    {
      // These are derived from o_data, which is not used in the first stanza.
      rnum_t mob_rnum = -1;
      if (quest_table[GET_QUEST(ch)].obj[objective_idx].o_data >= 0
          && (quest_table[GET_QUEST(ch)].obj[objective_idx].o_data >= quest_table[GET_QUEST(ch)].num_mobs
              || (mob_rnum = real_mobile(quest_table[GET_QUEST(ch)].mob[quest_table[GET_QUEST(ch)].obj[objective_idx].o_data].vnum)) < 0))
      {
        mob_rnum = real_mobile(quest_table[GET_QUEST(ch)].obj[objective_idx].o_data);
      }
      struct char_data *mob = (mob_rnum >= 0 ? &mob_proto[mob_rnum] : NULL);

      rnum_t room_rnum = real_room(quest_table[GET_QUEST(ch)].obj[objective_idx].o_data);
      struct room_data *room = (room_rnum >= 0 ? &world[room_rnum] : NULL);

      rnum_t host_rnum = real_host(quest_table[GET_QUEST(ch)].obj[objective_idx].o_data);
      struct host_data *host = (host_rnum >= 0 ? &matrix[host_rnum] : NULL);
      
      switch (quest_table[GET_QUEST(ch)].obj[objective_idx].objective) {
        case QUEST_NONE:
          break;
        case QOO_JOHNSON:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and deliver it to %s (%s)", GET_CHAR_NAME(johnson), ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
        case QOO_TAR_MOB:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and deliver it to %s (%s)", GET_CHAR_NAME(mob), ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
        case QOO_LOCATION:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and deliver it to %s (%s)", GET_ROOM_NAME(room), ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
        case QOO_DSTRY_ONE:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and destroy it (%s)", ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
        case QOO_DSTRY_MANY:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and destroy as many as you can (%d destroyed)", ch->player_specials->obj_complete[objective_idx]);
          break;
        case QOO_UPLOAD:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "and upload it to '%s' (%s)", host ? host->name : "NULL", ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
        case QOO_RETURN_PAY:
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Deliver paydata from host %s to %s (%s)",
                   host ? host->name : "NULL",
                   GET_CHAR_NAME(johnson),
                   ch->player_specials->obj_complete[objective_idx] ? "done" : "incomplete");
          break;
      }
    }
  
    send_to_char(ch, "%s\r\n", buf);
  }
}

ACMD(do_recap)
{
  if (!GET_QUEST(ch))
    send_to_char(ch, "You're not currently on a run.\r\n");
  else {
#ifdef USE_QUEST_LOCATION_CODE
    if (quest_table[GET_QUEST(ch)].location)
      snprintf(buf, sizeof(buf), "At %s, %s told you: \r\n%s", quest_table[GET_QUEST(ch)].location, GET_NAME(mob_proto+real_mobile(quest_table[GET_QUEST(ch)].johnson)),
              quest_table[GET_QUEST(ch)].info);
    else
#endif
      snprintf(buf, sizeof(buf), "%s told you: \r\n%s", GET_NAME(mob_proto+real_mobile(quest_table[GET_QUEST(ch)].johnson)),
              quest_table[GET_QUEST(ch)].info);
    send_to_char(buf, ch);

#ifdef IS_BUILDPORT
    display_quest_goals_to_ch(ch);
#endif
  }
}

struct char_data * fetch_quest_mob_target_mob_proto(struct quest_data *qst, int mob_idx) {
  rnum_t derived_rnum = translate_quest_mob_identifier_to_rnum(mob_idx, qst);

  if (derived_rnum < 0)
    return NULL;
  
  return &mob_proto[derived_rnum];
}

struct char_data * fetch_quest_mob_actual_mob_proto(struct quest_data *qst, int mob_idx) {
  rnum_t mob_rnum = real_mobile(qst->mob[mob_idx].vnum);

  if (mob_rnum < 0)
    return NULL;
  
  return &mob_proto[mob_rnum];
}

rnum_t translate_quest_mob_identifier_to_rnum(vnum_t identifier, struct quest_data *quest) {
  // Invalid vnum? Don't look it up.
  if (identifier < 0)
    return -1;
  
  // Not on the quest mob table? Assume it's a direct vnum reference and return the rnum of that.
  if (identifier >= quest->num_mobs || (quest->mob[identifier].vnum <= 0)) {
    return real_mobile(identifier);
  }

  // It's on the mob table. Check to see if the associated vnum actually exists.
  rnum_t rnum = real_mobile(quest->mob[identifier].vnum);
  if (rnum < 0) {
    // Called-out quest mob didn't exist, so treat it as a direct vnum reference.
    return real_mobile(identifier);
  }

  // Existed, return its rnum.
  return rnum;
}

vnum_t translate_quest_mob_identifier_to_vnum(vnum_t identifier, struct quest_data *quest) {
  // This is technically inefficient since this goes vnum -> rnum -> vnum, but we call this very infrequently.
  rnum_t rnum = translate_quest_mob_identifier_to_rnum(identifier, quest);

  // Impossible to parse into a valid mob, return it as written.
  if (rnum < 0) {
    return identifier;
  }

  // Return the vnum of the mob.
  return mob_index[rnum].vnum;
}