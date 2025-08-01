#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#include "structs.hpp"
#include "awake.hpp"
#include "comm.hpp"
#include "handler.hpp"
#include "db.hpp"
#include "interpreter.hpp"
#include "utils.hpp"
#include "newshop.hpp"
#include "screen.hpp"
#include "olc.hpp"
#include "constants.hpp"
#include "config.hpp"
#include "newmail.hpp"
#include "lifestyles.hpp"
#include "chipjacks.hpp"
#include "pocketsec.hpp"
#include "factions.hpp"
#include "metrics.hpp"

extern struct time_info_data time_info;
extern const char *pc_race_types[];

extern struct obj_data *get_first_credstick(struct char_data *ch, const char *arg);
extern void reduce_abilities(struct char_data *vict);
extern void do_probe_object(struct char_data * ch, struct obj_data * j, bool is_in_shop);
extern void weight_change_object(struct obj_data * obj, float weight);
extern char *short_object(int virt, int where);
ACMD_DECLARE(do_say);
ACMD_DECLARE(do_new_echo);

bool shop_can_sell_object(struct obj_data *obj, struct char_data *keeper, int shop_nr);
bool shop_will_buy_item_from_ch(rnum_t shop_nr, struct obj_data *obj, struct char_data *ch);
void shop_install(char *argument, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr);
void shop_uninstall(char *argument, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr);
struct obj_data *shop_package_up_ware(struct obj_data *obj);
int get_cyberware_install_cost(struct obj_data *ware);
void sell_all_stowed_items(struct char_data *ch, rnum_t shop_nr, struct char_data *keeper);

int cmd_say;
int cmd_echo;

const char *shop_flags[] =
  {
    "Nothing",
    "Doctor",
    "!NEGOTIATE",
    "!RESELL",
    "CHARGEN",
    "YES_GHOUL",
    MAX_FLAG_MARKER
  };

const char *shop_type[3] =
  {
    "Grey",
    "Legal",
    "Black"
  };

const char *selling_type[] =
  {
    "Always",
    "Avail",
    "Stock",
    "Bought"
  };

bool is_open(struct char_data *keeper, int shop_nr)
{
#ifdef USE_SHOP_OPEN_CLOSE_TIMES
  char buf[MAX_STRING_LENGTH];
  buf[0] = '\0';
  if (shop_table[shop_nr].open > shop_table[shop_nr].close) {
    if (time_info.hours < shop_table[shop_nr].open && time_info.hours > shop_table[shop_nr].close)
      snprintf(buf, sizeof(buf), "We're not open yet.");
  } else {
    if (time_info.hours < shop_table[shop_nr].open)
      snprintf(buf, sizeof(buf), "We're not open yet.");
    else if (time_info.hours > shop_table[shop_nr].close)
      snprintf(buf, sizeof(buf), "We've closed for the day.");
  }
  if (!*buf)
    return TRUE;
  else
  {
    do_say(keeper, buf, cmd_say, 0);
    return FALSE;
  }
#else
  return TRUE;
#endif
}

bool is_ok_char(struct char_data * keeper, struct char_data * ch, vnum_t shop_nr)
{
  char buf[400];

  if (!access_level(ch, LVL_ADMIN) && !(CAN_SEE(keeper, ch))) {
    strlcpy(buf, "I don't trade with someone I can't see.", sizeof(buf));
    do_say(keeper, buf, cmd_say, 0);
    return FALSE;
  }
  if (IS_PROJECT(ch)) {
    send_to_char("You're having a hard time getting the shopkeeper's attention.\r\n", ch);
    return FALSE;
  }

  if (IS_NPC(ch) || access_level(ch, LVL_BUILDER)) {
    return TRUE;
  }

  if ((shop_table[shop_nr].races.IsSet(RACE_HUMAN) && (GET_RACE(ch) == RACE_HUMAN || GET_RACE(ch) == RACE_GHOUL_HUMAN || GET_RACE(ch) == RACE_DRAKE_HUMAN)) ||
      (shop_table[shop_nr].races.IsSet(RACE_ELF) && (GET_RACE(ch) == RACE_ELF ||
          GET_RACE(ch) == RACE_WAKYAMBI || GET_RACE(ch) == RACE_NIGHTONE ||
          GET_RACE(ch) == RACE_DRYAD || GET_RACE(ch) == RACE_GHOUL_ELF || GET_RACE(ch) == RACE_DRAKE_ELF)) ||
      (shop_table[shop_nr].races.IsSet(RACE_DWARF) && (GET_RACE(ch) == RACE_DWARF ||
          GET_RACE(ch) == RACE_KOBOROKURU || GET_RACE(ch) == RACE_MENEHUNE ||
          GET_RACE(ch) == RACE_GNOME || GET_RACE(ch) == RACE_GHOUL_DWARF || GET_RACE(ch) == RACE_DRAKE_DWARF)) ||
      (shop_table[shop_nr].races.IsSet(RACE_ORK) && (GET_RACE(ch) == RACE_ORK ||
          GET_RACE(ch) == RACE_ONI || GET_RACE(ch) == RACE_SATYR ||
          GET_RACE(ch) == RACE_HOBGOBLIN || GET_RACE(ch) == RACE_OGRE || GET_RACE(ch) == RACE_GHOUL_ORK || GET_RACE(ch) == RACE_DRAKE_ORK)) ||
      (shop_table[shop_nr].races.IsSet(RACE_TROLL) && (GET_RACE(ch) == RACE_TROLL ||
          GET_RACE(ch) == RACE_CYCLOPS || GET_RACE(ch) == RACE_GIANT || GET_RACE(ch) == RACE_MINOTAUR ||
          GET_RACE(ch) == RACE_FOMORI || GET_RACE(ch) == RACE_GHOUL_TROLL || GET_RACE(ch) == RACE_DRAKE_TROLL)))
  {
    snprintf(buf, sizeof(buf), "%s We don't sell to your type here.", GET_CHAR_NAME(ch));
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return FALSE;
  }
  return TRUE;
}

// Player buying from shop.
int buy_price(struct obj_data *obj, vnum_t shop_nr, idnum_t faction_idnum, struct char_data *ch)
{
  // Base cost.
  int cost = GET_OBJ_COST(obj);

  // Multiply base cost by the shop's profit. Under no circumstances will we sell to them for less than 1x cost.
  cost = (int) round(cost * MAX(1, shop_table[shop_nr].profit_buy));

  // If the shop is black or grey market, multiply base cost by the item's street index.
  if (shop_table[shop_nr].type != SHOP_LEGAL && GET_OBJ_STREET_INDEX(obj) > 0)
    cost = (int) round(cost * GET_OBJ_STREET_INDEX(obj));

  // If it's a faction shop, multiply by the faction rep multiplier.
  if (faction_idnum)
    cost *= get_shop_faction_sell_to_player_multiplier(faction_idnum, ch);

  // Add the random multiplier to the cost.
  cost += (int) round((cost * shop_table[shop_nr].random_current) / 100);

  // Enforce the final 1x cost requirement. This is an anti-exploit measure to prevent buying a cheap thing at shop A and reselling at shop B for profit.
  // Note that negotiation happens AFTER this, so people can still negotiate down.
  cost = MAX(cost, GET_OBJ_COST(obj));

  // Return the final value.
  return cost;
}

// Player selling to shop.
int sell_price(struct obj_data *obj, vnum_t shop_nr, idnum_t faction_idnum, struct char_data *ch)
{
  // Base cost.
  int cost = (int) round(GET_OBJ_COST(obj) * shop_table[shop_nr].profit_sell);

  // If the street index is set but is less than 1, multiply by this index regardless of shop legality.
  // This fixes an exploit where someone could buy a discounted thing at a black/grey shop and sell to a legal one for a profit.
  if (GET_OBJ_STREET_INDEX(obj) > 0 && GET_OBJ_STREET_INDEX(obj) < 1)
    cost = (int) round(cost * GET_OBJ_STREET_INDEX(obj));

  // If it's a faction shop, multiply by the faction rep multiplier.
  if (faction_idnum)
    cost *= get_shop_faction_buy_from_player_multiplier(faction_idnum, ch);

  // Add the random multiplier to the cost.
  cost += (int) round((cost * shop_table[shop_nr].random_current) / 100);

  return cost;
}

int transaction_amt(char *arg, size_t arg_len)
{
  int num;
  one_argument(arg, buf);
  if (*buf)
    if ((is_number(buf))) {
      num = atoi(buf);
      char temp_buf[strlen(arg)];
      strlcpy(temp_buf, arg + strlen(buf) + 1, sizeof(temp_buf));
      strlcpy(arg, temp_buf, arg_len);
      return (num);
    }
  return (1);
}

struct shop_sell_data *find_obj_shop(char *arg, vnum_t shop_nr, struct obj_data **obj, struct char_data *ch)
{
  *obj = NULL;
  struct shop_sell_data *sell = shop_table[shop_nr].selling;
  if (*arg == '#' && atoi(arg+1) > 0)
  {
    int num = atoi(arg+1);
    for (;sell; sell = sell->next) {
      num--;

      int real_obj = real_object(sell->vnum);

      if (real_obj >= 0) {
        // Can't sell it? Don't have it show up here.
        struct obj_data *temp_obj = read_object(real_obj, REAL, OBJ_LOAD_REASON_EDITING_EPHEMERAL_LOOKUP);
        if (!shop_can_sell_object(temp_obj, NULL, shop_nr)) {
          num++;
          continue;
        }
        extract_obj(temp_obj);
        temp_obj = NULL;
        if (num <= 0)
          break;
      } else {
        num++;
        continue;
      }
    }
    if (sell)
      *obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_FIND_OBJ_SHOP);
  } else
  {
    // Don't allow purchasing numbers.
    if (atoi(arg) > 0) {
      send_to_char(ch, "You can't buy just a number. Either 'buy #%s', or 'buy %s <name of something>.\r\n", arg, arg);
      return NULL;
    }

    for (; sell; sell = sell->next) {
      int real_obj = real_object(sell->vnum);
      if (real_obj >= 0) {
        if (obj_proto[real_obj].obj_flags.cost &&
            (isname(arg, obj_proto[real_obj].text.name) ||
             isname(arg, obj_proto[real_obj].text.keywords))) {
          *obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_FIND_OBJ_SHOP);
          break;
        }
      }
    }
  }
  return sell;
}

bool uninstall_ware_from_target_character(struct obj_data *obj, struct char_data *remover, struct char_data *victim, bool damage_on_operation) {
  char buf[MAX_STRING_LENGTH], buf3[MAX_STRING_LENGTH];

  if (remover == victim) {
    if (!access_level(remover, LVL_ADMIN)) {
      send_to_char(remover, "You can't operate on yourself!\r\n");
      mudlog("SYSERR: remover = victim in uninstall_ware_from_target_character(). That's not supposed to happen!", remover, LOG_SYSLOG, TRUE);
      return FALSE;
    } else {
      act("Allowing self-operation for $n: Staff", FALSE, remover, 0, 0, TO_ROLLS);
    }
  }

  if (GET_OBJ_TYPE(obj) != ITEM_BIOWARE && GET_OBJ_TYPE(obj) != ITEM_CYBERWARE) {
    snprintf(buf3, sizeof(buf3), "SYSERR: Non-ware object '%s' (%ld) passed to uninstall_ware_from_target_character()!", GET_OBJ_NAME(obj), GET_OBJ_VNUM(obj));
    mudlog(buf3, remover, LOG_SYSLOG, TRUE);
    if (!access_level(remover, LVL_ADMIN)) {
      send_to_char(remover, "An unexpected error occurred when trying to uninstall %s.\r\n", GET_OBJ_NAME(obj));
      return FALSE;
    } else {
      act("Allowing uninstallation of non-ware for $n from $N: Staff", FALSE, remover, 0, victim, TO_ROLLS);
    }
  }

  if (GET_OBJ_COST(obj) == 0 && !IS_NPC(remover)) {
    if (!access_level(remover, LVL_ADMIN)) {
      send_to_char(remover, "%s is Chargen 'ware, so it can't be removed by player cyberdocs. Have your patient sell it to an NPC doc.\r\n", capitalize(GET_OBJ_NAME(obj)));
      return FALSE;
    } else {
      act("Allowing uninstallation of chargen 'ware for $n from $N: Staff", FALSE, remover, 0, victim, TO_ROLLS);
    }
  }

  if (GET_CYBERWARE_TYPE(obj) == CYB_CHIPJACK && obj->contains) {
    send_to_char("You can't uninstall a chipjack with chips in it.\r\n", remover);
    return FALSE;
  }

  if (GET_CYBERWARE_TYPE(obj) == CYB_MEMORY && obj->contains) {
    send_to_char("You can't uninstall headware memory with data in it.\r\n", remover);
    return FALSE;
  }

  if (GET_OBJ_TYPE(obj) == ITEM_BIOWARE) {
    obj_from_bioware(obj);
    GET_INDEX(victim) -= calculate_ware_essence_or_index_cost(victim, obj);
    GET_INDEX(victim) = MAX(0, GET_INDEX(victim));
  } else {
    obj_from_cyberware(obj);
    GET_ESSHOLE(victim) += calculate_ware_essence_or_index_cost(victim, obj);
  }

  if (!IS_NPC(remover)) {
    const char *representation = generate_new_loggable_representation(obj);
    mudlog_vfprintf(remover, LOG_GRIDLOG, "Player Cyberdoc: %s uninstalled %s from %s.", GET_CHAR_NAME(remover), representation, GET_CHAR_NAME(victim));

    if (is_same_host(remover, victim)) {
      // Log anyone doing this from a multibox host.
      mudlog_vfprintf(remover, LOG_CHEATLOG, "Player Cyberdoc: %s uninstalled %s from same-host character %s. (%s)", 
                      GET_CHAR_NAME(remover),
                      representation,
                      GET_CHAR_NAME(victim),
                      GET_LEVEL(remover) < LVL_PRESIDENT ? remover->desc->host : "<obscured>");
    }

    delete [] representation;
  }

  act("$n takes out a sharpened scalpel and lies $N down on the operating table.",
      FALSE, remover, 0, victim, TO_NOTVICT);
  if (IS_NPC(remover)) {
    snprintf(buf, sizeof(buf), "%s Relax...this won't hurt a bit.", GET_CHAR_NAME(victim));
    do_say(remover, buf, cmd_say, SCMD_SAYTO);
  }
  act("You delicately remove $p from $N's body.",
      FALSE, remover, obj, victim, TO_CHAR);
  act("$n performs a delicate procedure on $N.",
      FALSE, remover, 0, victim, TO_NOTVICT);
  act("$n delicately removes $p from your body.",
      FALSE, remover, obj, victim, TO_VICT);

  // If this isn't a newbie shop, damage them like they're just coming out of surgery.
  if (damage_on_operation) {
    GET_PHYSICAL(victim) = 100;
    GET_MENTAL(victim) = 100;
  }

  affect_total(victim);

  // Strip any jacked skills.
  if (GET_OBJ_TYPE(obj) == ITEM_CYBERWARE && GET_CYBERWARE_TYPE(obj) == CYB_MEMORY) {
    deactivate_skillsofts_in_headware_memory(obj, victim, TRUE);
  }    

  return TRUE;
}

bool install_ware_in_target_character(struct obj_data *ware, struct char_data *installer, struct char_data *recipient, bool damage_on_operation) {
  struct obj_data *check;
  char buf[MAX_STRING_LENGTH], buf3[MAX_STRING_LENGTH];

  if (installer == recipient) {
    if (!access_level(installer, LVL_ADMIN)) {
      send_to_char(installer, "You can't operate on yourself!\r\n");
      mudlog("SYSERR: installer = recipient in install_ware_in_target_character(). That's not supposed to happen!", installer, LOG_SYSLOG, TRUE);
      return FALSE;
    } else {
      act("Allowing self-operation for $n: Staff", FALSE, installer, 0, 0, TO_ROLLS);
    }
  }

  strlcpy(buf, GET_CHAR_NAME(recipient), sizeof(buf));

  // Go home dragon, you're drunk! Disables installing of cyber/bio in both shops and playerdocs - Vile
  if (IS_DRAGON(recipient)) {
    if (IS_NPC(installer)) {
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " Your magical nature makes this operation impossible!");
      do_say(installer, buf, cmd_say, SCMD_SAYTO);
    } else {
      send_to_char(installer, "Their magical nature rejects the installation of %s!\r\n", GET_OBJ_NAME(ware));
    }
    return FALSE;
  }

  // Item must be compatible with your current gear.
  switch (GET_OBJ_TYPE(ware)) {
    case ITEM_CYBERWARE:
    case ITEM_BIOWARE:
      for (struct obj_data *bio = recipient->bioware; bio; bio = bio->next_content)
        if (!biocyber_compatibility(ware, bio, recipient)) {
          send_to_char(installer, "%s isn't compatible with what's already installed.\r\n", CAP(GET_OBJ_NAME(ware)));
          return FALSE;
        }
      for (struct obj_data *cyber = recipient->cyberware; cyber; cyber = cyber->next_content)
        if (!biocyber_compatibility(ware, cyber, recipient)) {
          send_to_char(installer, "%s isn't compatible with what's already installed.\r\n", CAP(GET_OBJ_NAME(ware)));
          return FALSE;
        }
      break;
    default:
      snprintf(buf3, sizeof(buf3), "SYSERR: Non-ware object '%s' (%ld) passed to install_ware_in_target_character()!", decapitalize_a_an(ware), GET_OBJ_VNUM(ware));
      mudlog(buf3, installer, LOG_SYSLOG, TRUE);
      send_to_char(installer, "An unexpected error occurred when trying to install %s (code 1).\r\n", decapitalize_a_an(ware));
      send_to_char(recipient, "An unexpected error occurred when trying to install %s (code 1).\r\n", decapitalize_a_an(ware));
      return FALSE;
  }

  if (blocked_by_soulbinding(recipient, ware, FALSE)) {
    send_to_char(installer, "You can't install %s in %s: it has been customized to fit someone else's biology.\r\n", decapitalize_a_an(ware), GET_CHAR_NAME(recipient));
    send_to_char(installer, "You can't have %s installed: it has been customized to fit someone else's biology.\r\n", decapitalize_a_an(ware));
    return FALSE;
  }

  // Edge case: We remove the object from its container further down, and we want to make sure this doesn't break anything.
  if (ware->in_obj && GET_OBJ_TYPE(ware->in_obj) != ITEM_SHOPCONTAINER) {
    snprintf(buf3, sizeof(buf3), "SYSERR: '%s' (%ld) contained in something that's not a shopcontainer!", decapitalize_a_an(ware), GET_OBJ_VNUM(ware));
    mudlog(buf3, installer, LOG_SYSLOG, TRUE);
    send_to_char(installer, "An unexpected error occurred when trying to install %s (code 2).\r\n", decapitalize_a_an(ware));
    send_to_char(recipient, "An unexpected error occurred when trying to install %s (code 2).\r\n", decapitalize_a_an(ware));
    return FALSE;
  }

  // Don't shrek the mages.
  if (IS_OBJ_STAT(ware, ITEM_EXTRA_MAGIC_INCOMPATIBLE) && (GET_MAG(recipient) > 0 || GET_TRADITION(recipient) != TRAD_MUNDANE)) {
    if (IS_NPC(installer)) {
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That operation would eradicate your magic!");
      do_say(installer, buf, cmd_say, SCMD_SAYTO);
    } else {
      send_to_char(installer, "You can't install %s-- it's not compatible with magic.\r\n", decapitalize_a_an(ware));
    }
    return FALSE;
  }

  // Reject installing magic-incompat 'ware into magic-using characters.
  if (GET_OBJ_TYPE(ware) == ITEM_CYBERWARE) {
    int esscost = calculate_ware_essence_or_index_cost(recipient, ware);

    // Check to see if the operation is even possible with their current essence / hole.
    if (GET_REAL_ESS(recipient) + GET_ESSHOLE(recipient) <= esscost) {
      if (IS_NPC(installer)) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That operation would kill you!");
        do_say(installer, buf, cmd_say, SCMD_SAYTO);
      } else {
        send_to_char(installer, "There's not enough meat left to install %s into!\r\n", GET_OBJ_NAME(ware));
      }
      return FALSE;
    }

    // Check for matching cyberware and related limits.
    int num_reaction_enhancers = 0;
    int num_handspurs = 0;
    int num_handrazors = 0;
    int num_improved_handrazors = 0;
    for (check = recipient->cyberware; check != NULL; check = check->next_content) {
      if (GET_CYBERWARE_TYPE(ware) == GET_CYBERWARE_TYPE(check)) {
        if (GET_CYBERWARE_TYPE(check) == CYB_REACTIONENHANCE) {
          if (++num_reaction_enhancers == 6) {
            if (IS_NPC(installer)) {
              snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You already have the maximum number of reaction enhancers installed.");
              do_say(installer, buf, cmd_say, SCMD_SAYTO);
            } else {
              send_to_char(installer, "You can't install %s-- more reaction enhancers wouldn't have any effect.\r\n", GET_OBJ_NAME(ware));
            }
            return FALSE;
          }
        } else if (GET_CYBERWARE_TYPE(check) == CYB_HANDRAZOR) {
          if (IS_SET(GET_CYBERWARE_FLAGS(check), 1 << CYBERWEAPON_IMPROVED)) {
            if (++num_improved_handrazors >= 2) {
              if (IS_NPC(installer)) {
                strlcat(buf, " You already have the maximum of two installed.", sizeof(buf));
                do_say(installer, buf, cmd_say, SCMD_SAYTO);
              } else {
                send_to_char(installer, "You can't install %s-- the maximum of two is already installed.\r\n", GET_OBJ_NAME(ware));
              }
              return FALSE;
            }
          } else {
            if (++num_handrazors >= 2) {
              if (IS_NPC(installer)) {
                strlcat(buf, " You already have the maximum of two installed.", sizeof(buf));
                do_say(installer, buf, cmd_say, SCMD_SAYTO);
              } else {
                send_to_char(installer, "You can't install %s-- the maximum of two is already installed.\r\n", GET_OBJ_NAME(ware));
              }
              return FALSE;
            }
          }
        } else if (GET_CYBERWARE_TYPE(check) == CYB_HANDSPUR) {
          if (++num_handspurs >= 2) {
            if (IS_NPC(installer)) {
              strlcat(buf, " You already have the maximum of two installed.", sizeof(buf));
              do_say(installer, buf, cmd_say, SCMD_SAYTO);
            } else {
              send_to_char(installer, "You can't install %s-- the maximum of two is already installed.\r\n", GET_OBJ_NAME(ware));
            }
            return FALSE;
          }
        } else {
          if (GET_OBJ_VNUM(check) == GET_OBJ_VNUM(ware) && !is_custom_ware(check)) {
            if (IS_NPC(installer)) {
              snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You already have %s installed.", GET_OBJ_NAME(ware));
              do_say(installer, buf, cmd_say, SCMD_SAYTO);
            } else {
              send_to_char(installer, "You can't install %s-- another one is already installed.\r\n", GET_OBJ_NAME(ware));
            }
            return FALSE;
          }

          if (GET_CYBERWARE_TYPE(ware) != CYB_EYES && GET_CYBERWARE_TYPE(ware) != CYB_FILTRATION && !is_custom_ware(check)) {
            if (IS_NPC(installer)) {
              snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You already have %s, and it's too similar to %s for them to work together.", GET_OBJ_NAME(check), GET_OBJ_NAME(ware));
              do_say(installer, buf, cmd_say, SCMD_SAYTO);
            } else {
              send_to_char(installer, "You can't install %s-- it's too similar to the already-installed %s.\r\n", GET_OBJ_NAME(ware), GET_OBJ_NAME(check));
            }
            return FALSE;
          }
        }
      }
    }

    // Adapt for essence hole.
    if (GET_ESSHOLE(recipient) < esscost) {
      esscost = esscost - GET_ESSHOLE(recipient);

      // Deduct magic, if any.
      if (GET_TRADITION(recipient) != TRAD_MUNDANE) {
        if (GET_REAL_MAG(recipient) - esscost < 100) {
          if (IS_NPC(installer)) {
            snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That would take away the last of your magic!");
            do_say(installer, buf, cmd_say, SCMD_SAYTO);
          } else {
            send_to_char(installer, "%s would take away the last of their magic!\r\n", capitalize(GET_OBJ_NAME(ware)));
          }
          return FALSE;
        }
        magic_loss(recipient, esscost, TRUE);
      }
      GET_ESSHOLE(recipient) = 0;
      GET_REAL_ESS(recipient) -= esscost;
    } else {
      GET_ESSHOLE(recipient) -= esscost;
    }

    // Unpackage it if needed, and extract the container.
    if (ware->in_obj && GET_OBJ_TYPE(ware->in_obj) == ITEM_SHOPCONTAINER) {
      struct obj_data *container = ware->in_obj;
      obj_from_obj(ware);
      GET_OBJ_EXTRA(container).RemoveBit(ITEM_EXTRA_KEPT);
      extract_obj(container);
      container = NULL;
    }

    // Install it.
    obj_to_cyberware(ware, recipient);
  }

  // You must have the index to support it.
  else if (GET_OBJ_TYPE(ware) == ITEM_BIOWARE) {
    int esscost = calculate_ware_essence_or_index_cost(recipient, ware);
    if (GET_INDEX(recipient) + esscost > 900) {
      if (IS_NPC(installer)) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That operation would kill you!");
        do_say(installer, buf, cmd_say, SCMD_SAYTO);
      } else {
        send_to_char(installer, "There's not enough meat left to install %s into!\r\n", GET_OBJ_NAME(ware));
      }
      return FALSE;
    }

    if ((GET_BIOWARE_TYPE(ware) == BIO_PATHOGENICDEFENSE || GET_BIOWARE_TYPE(ware) == BIO_TOXINEXTRACTOR) &&
        GET_BIOWARE_RATING(ware) > GET_REAL_BOD(recipient) / 2)
    {
      if (IS_NPC(installer)) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " Your body can't support pathogenic defenses that are that strong.");
        do_say(installer, buf, cmd_say, SCMD_SAYTO);
      } else {
        send_to_char(installer, "The defenses from %s are too powerful for their body.\r\n", GET_OBJ_NAME(ware));
      }
      return FALSE;
    }

    for (check = recipient->bioware; check; check = check->next_content) {
      if ((GET_OBJ_VNUM(check) == GET_OBJ_VNUM(ware)) && !is_custom_ware(ware)) {
        if (IS_NPC(installer)) {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You already have that installed.");
          do_say(installer, buf, cmd_say, SCMD_SAYTO);
        } else {
          send_to_char(installer, "Another %s is already installed.\r\n", GET_OBJ_NAME(ware));
        }
        return FALSE;
      }
      if (GET_BIOWARE_TYPE(check) == GET_BIOWARE_TYPE(ware) && !is_custom_ware(ware)) {
        if (IS_NPC(installer)) {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You already have %s installed, and it's too similar to %s for them to work together.", GET_OBJ_NAME(check), GET_OBJ_NAME(ware));
          do_say(installer, buf, cmd_say, SCMD_SAYTO);
        } else {
          send_to_char(installer, "You can't install %s-- the already-installed %s would conflict with it.\r\n", GET_OBJ_NAME(ware), GET_OBJ_NAME(check));
        }
        return FALSE;
      }
    }

    GET_INDEX(recipient) += esscost;
    if (GET_INDEX(recipient) > recipient->real_abils.highestindex) {
      if (GET_TRADITION(recipient) != TRAD_MUNDANE) {
        int change = GET_INDEX(recipient) - recipient->real_abils.highestindex;
        change /= 2;
        if (GET_REAL_MAG(recipient) - change < 100) {
          if (IS_NPC(installer)) {
            snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That would take away the last of your magic!");
            do_say(installer, buf, cmd_say, SCMD_SAYTO);
          } else {
            send_to_char(installer, "You can't install %s-- it would take away the last of their magic.\r\n", GET_OBJ_NAME(ware), GET_OBJ_NAME(check));
          }
          GET_INDEX(recipient) -= esscost;
          return FALSE;
        }
        magic_loss(recipient, change, TRUE);
      }
      recipient->real_abils.highestindex = GET_INDEX(recipient);
    }

    // Unpackage it if needed, and extract the container.
    if (ware->in_obj && GET_OBJ_TYPE(ware->in_obj) == ITEM_SHOPCONTAINER) {
      struct obj_data *container = ware->in_obj;
      obj_from_obj(ware);
      GET_OBJ_EXTRA(container).RemoveBit(ITEM_EXTRA_KEPT);
      extract_obj(container);
      container = NULL;
    }

    // Install it.
    obj_to_bioware(ware, recipient);
  }

  // Sanity check.
  else {
    mudlog_vfprintf(installer, LOG_SYSLOG, "CRITICAL SYSERR: Not only is %s (%ld) not cyberware or bioware, our prior check to ensure safety failed!!", GET_OBJ_NAME(ware), GET_OBJ_VNUM(ware));
    send_to_char(installer, "An error has occurred and the operation has been aborted. (1)\r\n");
    send_to_char(recipient, "An error has occurred and the operation has been aborted. (1)\r\n");
    return FALSE;
  }

  // Soulbind it. In case there's a need to make this chargen-only later on: "PLR_FLAGGED(recipient, PLR_NOT_YET_AUTHED)"
  soulbind_obj_to_char(ware, recipient, TRUE);

  if (!IS_NPC(installer)) {
    const char *representation = generate_new_loggable_representation(ware);
    mudlog_vfprintf(installer, LOG_GRIDLOG, "Player Cyberdoc: %s (%ld) installed %s in %s (%ld).", 
                    GET_CHAR_NAME(installer), GET_IDNUM(installer),
                    representation,
                    GET_CHAR_NAME(recipient), GET_IDNUM(recipient));

    if (is_same_host(installer, recipient)) {
      // Log anyone doing this from a multibox host.
      mudlog_vfprintf(installer, LOG_CHEATLOG, "Player Cyberdoc: %s installed %s in same-host character %s. (%s)", 
                      GET_CHAR_NAME(installer),
                      representation,
                      GET_CHAR_NAME(recipient),
                      GET_LEVEL(installer) < LVL_PRESIDENT ? installer->desc->host : "<obscured>");
    }

    delete [] representation;
  }

  // Send installation messages.
  act("$n takes out a sharpened scalpel and lies $N down on the operating table.",
      FALSE, installer, 0, recipient, TO_NOTVICT);
  if (IS_NPC(installer)) {
    snprintf(buf, sizeof(buf), "%s Relax...this won't hurt a bit.", GET_CHAR_NAME(recipient));
    do_say(installer, buf, cmd_say, SCMD_SAYTO);
  }
  act("You delicately install $p into $N's body.",
      FALSE, installer, ware, recipient, TO_CHAR);
  act("$n performs a delicate procedure on $N.",
      FALSE, installer, 0, recipient, TO_NOTVICT);
  act("$n delicately installs $p into your body.",
      FALSE, installer, ware, recipient, TO_VICT);

  // If this isn't a newbie shop, damage them like they're just coming out of surgery.
  if (damage_on_operation) { // aka !shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN)
    GET_PHYSICAL(recipient) = 100;
    GET_MENTAL(recipient) = 100;
  }

  if (GET_BIOOVER(recipient) > 0)
    send_to_char("You don't feel too well.\r\n", recipient);
  return TRUE;
}

// Yes, it's a monstrosity. No, I don't want to hear about it. YOU refactor it.
bool shop_receive(struct char_data *ch, struct char_data *keeper, char *arg, int buynum, bool cash,
                  struct shop_sell_data *sell, struct obj_data *obj, struct obj_data *cred, long price,
                  vnum_t shop_nr, struct shop_order_data *order)
{
  char buf[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];
  strlcpy(buf, GET_CHAR_NAME(ch), sizeof(buf));
  int bought = 0;
  bool print_multiples_at_end = TRUE;

  // Item must be available in the store.
  if (sell && sell->type == SELL_STOCK && sell->stock < 1)
  {
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " That item isn't currently available.");
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return FALSE;
  }

  if (cred) {
    send_to_char(ch, "You pull out %s to pay.\r\n", decapitalize_a_an(GET_OBJ_NAME(cred)));
  }

  // Character must have enough nuyen for it.
  if ((cred && GET_BANK(ch) < price) || (!cred && GET_NUYEN(ch) < price))
  {
    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "displays, \"%s\"", shop_table[shop_nr].not_enough_nuyen);
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %s", shop_table[shop_nr].not_enough_nuyen);
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
    return FALSE;
  }

  // Pre-compose our string.
  snprintf(buf2, sizeof(buf2), "You now have %s.", GET_OBJ_NAME(obj));

  // Cyberware / bioware doctor.
  if (shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
    if (!shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN)) {
      // We used to have a ton of compatibility checks here, but now we just give them the thing in a box!
      struct obj_data *shop_container = shop_package_up_ware(obj);
      act("$n packages up $N's purchase and hands it to $M.", TRUE, keeper, NULL, ch, TO_NOTVICT);
      act("$n packages up your purchase and hands it to you.", TRUE, keeper, NULL, ch, TO_VICT);
      obj_to_char(shop_container, ch);

      snprintf(buf2, sizeof(buf2), "You now have %s.", GET_OBJ_NAME(shop_container));

      // I'm tempted to reduce the price by the installation cost, but that brings up all sorts of exploits and edge cases.
      // For example, if install cost is 10k and the thing cost 9k, it's free to purchase, and you can sell it back for a ~3k profit!
    } else {
      if (!install_ware_in_target_character(obj, keeper, ch, FALSE))
        return FALSE;
    }

    bought = 1;

    if (cred)
      lose_bank(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);
    else
      lose_nuyen(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);

    // Log it.
    snprintf(buf, sizeof(buf), "Purchased cyber/bio '%s' (%ld) for %ld nuyen.", GET_OBJ_NAME(obj), GET_OBJ_VNUM(obj), price + (order ? order->paid : 0));
    mudlog(buf, ch, LOG_GRIDLOG, TRUE);

    if (sell) {
      if (sell->type == SELL_BOUGHT && !--sell->stock) {
        struct shop_sell_data *temp;
        REMOVE_FROM_LIST(sell, shop_table[shop_nr].selling, next);
        delete sell;
        sell = NULL;
      } else if (sell->type == SELL_STOCK)
        sell->stock--;
    }
  }

  // Neither cyber nor bioware. Handle as normal object.
  else {
    if (IS_CARRYING_N(ch) + 1 > CAN_CARRY_N(ch)) {
      send_to_char("You can't carry any more items.\r\n", ch);
      return FALSE;
    }
    if (GET_OBJ_WEIGHT(obj) > CAN_CARRY_W(ch)) {
      send_to_char("It weighs too much!\r\n", ch);
      return FALSE;
    }

    // Special handling for stackable things. TODO: Review this to make sure sell struct etc is updated appropriately.
    if ((GET_OBJ_TYPE(obj) == ITEM_DECK_ACCESSORY && GET_DECK_ACCESSORY_TYPE(obj) == TYPE_PARTS)
        || (GET_OBJ_TYPE(obj) == ITEM_MAGIC_TOOL && GET_MAGIC_TOOL_TYPE(obj) == TYPE_SUMMONING)
        || GET_OBJ_TYPE(obj) == ITEM_GUN_AMMO
        || GET_OBJ_TYPE(obj) == ITEM_DRUG
        || GET_OBJ_VNUM(obj) == OBJ_ANTI_DRUG_CHEMS)
    {
      bought = 0;
      float current_obj_weight = 0;

      // Deduct money up to the amount they can afford. Update the object's cost to match.
      while (bought < buynum && (cred ? GET_BANK(ch) : GET_NUYEN(ch)) >= price) {
        bought++;

        // Prevent taking more than you can carry.
        current_obj_weight += GET_OBJ_WEIGHT(obj);
        if (current_obj_weight > CAN_CARRY_W(ch)) {
          if (--bought <= 0) {
            send_to_char("It weighs too much.\r\n", ch);
            return FALSE;
          } else {
            // send_to_char(ch, "You can only carry %d of that.\r\n", bought);
            // ^-- the shopkeeper will tell them this.
            break;
          }
        }

        if (cred)
          lose_bank(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);
        else
          lose_nuyen(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);
      }

      if (bought == 0) {
        send_to_char("You can't afford that.\r\n", ch);
        return FALSE;
      }

      GET_OBJ_COST(obj) = GET_OBJ_COST(obj) * bought;

      // Give them the item (it's gun ammo)
      if (GET_OBJ_TYPE(obj) == ITEM_GUN_AMMO) {
        print_multiples_at_end = FALSE;

        // Update its quantity and weight to match the increased ammo load. Cost already done above.
        GET_AMMOBOX_QUANTITY(obj) *= bought;
        GET_OBJ_WEIGHT(obj) *= bought;

        // In theory this is dead code now after the 'you can only carry x' code change above. Will see.
        if (GET_OBJ_WEIGHT(obj) > CAN_CARRY_W(ch)) {
          send_to_char("You start gathering up the ammo you paid for, but realize you can't carry it all! The shopkeeper gives you a /look/, then refunds you in cash.\r\n", ch);
          // In this specific instance, we not only assign raw nuyen, we also decrement the purchase nuyen counter. It's a refund, after all.
          long refund_amount = price * bought;
          GET_NUYEN_RAW(ch) += refund_amount;
          GET_NUYEN_INCOME_THIS_PLAY_SESSION(ch, NUYEN_OUTFLOW_SHOP_PURCHASES) -= refund_amount;
          extract_obj(obj);
          return FALSE;
        }

        struct obj_data *orig = ch->carrying;
        for (; orig; orig = orig->next_content) {
          if (GET_OBJ_TYPE(orig) == ITEM_GUN_AMMO
              && GET_AMMOBOX_INTENDED_QUANTITY(orig) <= 0
              && GET_AMMOBOX_WEAPON(obj) == GET_AMMOBOX_WEAPON(orig)
              && GET_AMMOBOX_TYPE(obj) == GET_AMMOBOX_TYPE(orig))
            break;
        }
        if (orig) {
          // They were carrying one already. Combine them.
          snprintf(buf2, sizeof(buf2), "You add the purchased %d rounds", GET_AMMOBOX_QUANTITY(obj));
          combine_ammo_boxes(ch, obj, orig, FALSE);
          snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), " into %s.", GET_OBJ_NAME(orig));
        } else {
          // Just give the purchased thing to them directly. Handle restring if needed.
          if (bought > 1) {
            char new_name_buf[500];

            // Compose the new name.
            snprintf(new_name_buf, sizeof(new_name_buf), "a box of %s %s ammunition",
              ammo_type[GET_AMMOBOX_TYPE(obj)].name,
              weapon_types[GET_AMMOBOX_WEAPON(obj)]
            );

            // Commit the change.
            obj->restring = str_dup(new_name_buf);

            snprintf(buf2, sizeof(buf2), "You now have %s (contains %d rounds).", GET_OBJ_NAME(obj), GET_AMMOBOX_QUANTITY(obj));
          } else {
            snprintf(buf2, sizeof(buf2), "You now have %s.", GET_OBJ_NAME(obj));
          }
          // buf2 is sent to the character with a newline appended at the end of the function.

          obj_to_char(obj, ch);
        }
      }

      // Give them the item (it's drugs)
      else if (GET_OBJ_TYPE(obj) == ITEM_DRUG) {
        print_multiples_at_end = FALSE;

        // Update its quantity and weight to match the increased dose count. Cost already done above.
        GET_OBJ_DRUG_DOSES(obj) *= bought;
        GET_OBJ_WEIGHT(obj) *= bought;

        // In theory this is dead code now after the 'you can only carry x' code change above. Will see.
        if (GET_OBJ_WEIGHT(obj) > CAN_CARRY_W(ch)) {
          send_to_char("You start gathering up the doses you paid for, but realize you can't carry it all! The shopkeeper scowls at you, then refunds you in cash.\r\n", ch);
          // In this specific instance, we not only assign raw nuyen, we also decrement the purchase nuyen counter. It's a refund, after all.
          long refund_amount = price * bought;
          GET_NUYEN_RAW(ch) += refund_amount;
          GET_NUYEN_INCOME_THIS_PLAY_SESSION(ch, NUYEN_OUTFLOW_SHOP_PURCHASES) -= refund_amount;
          extract_obj(obj);
          return FALSE;
        }

        struct obj_data *orig = ch->carrying;
        for (; orig; orig = orig->next_content) {
          if (GET_OBJ_TYPE(orig) == ITEM_DRUG && GET_OBJ_DRUG_TYPE(orig) == GET_OBJ_DRUG_TYPE(obj))
            break;
        }
        if (orig) {
          // They were carrying one already. Combine them.
          snprintf(buf2, sizeof(buf2), "You add the purchased %d doses", GET_OBJ_DRUG_DOSES(obj));
          combine_drugs(ch, obj, orig, FALSE);
          snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), " into %s.", GET_OBJ_NAME(orig));
        } else {
          // Just give the purchased thing to them directly. Handle restring if needed.
          if (bought > 1) {
            char new_name_buf[500];

            // Compose the new name.
            snprintf(new_name_buf, sizeof(new_name_buf), "a box of %s %ss",
              drug_types[GET_OBJ_DRUG_TYPE(obj)].name,
              drug_types[GET_OBJ_DRUG_TYPE(obj)].delivery_method
            );

            // Commit the change.
            obj->restring = str_dup(new_name_buf);

            snprintf(buf2, sizeof(buf2), "You now have %s (contains %d doses).", GET_OBJ_NAME(obj), GET_OBJ_DRUG_DOSES(obj));
          } else {
            snprintf(buf2, sizeof(buf2), "You now have %s.", GET_OBJ_NAME(obj));
          }
          // buf2 is sent to the character with a newline appended at the end of the function.

          obj_to_char(obj, ch);
        }
      }

      // Give them the item (it's chems)
      else if (GET_OBJ_VNUM(obj) == OBJ_ANTI_DRUG_CHEMS) {
        print_multiples_at_end = FALSE;

        // Update its quantity and weight to match the increased dose count. Cost already done above.
        GET_CHEMS_QTY(obj) *= bought;
        GET_OBJ_WEIGHT(obj) *= bought;

        // In theory this is dead code now after the 'you can only carry x' code change above. Will see.
        if (GET_OBJ_WEIGHT(obj) > CAN_CARRY_W(ch)) {
          send_to_char("You start gathering up the doses you paid for, but realize you can't carry it all! The shopkeeper scowls at you, then refunds you in cash.\r\n", ch);
          // In this specific instance, we not only assign raw nuyen, we also decrement the purchase nuyen counter. It's a refund, after all.
          long refund_amount = price * bought;
          GET_NUYEN_RAW(ch) += refund_amount;
          GET_NUYEN_INCOME_THIS_PLAY_SESSION(ch, NUYEN_OUTFLOW_SHOP_PURCHASES) -= refund_amount;
          extract_obj(obj);
          return FALSE;
        }

        struct obj_data *orig = ch->carrying;
        for (; orig; orig = orig->next_content) {
          if (GET_OBJ_VNUM(orig) == OBJ_ANTI_DRUG_CHEMS)
            break;
        }
        if (orig) {
          // They were carrying one already. Combine them.
          snprintf(buf2, sizeof(buf2), "You add the purchased %d doses", GET_CHEMS_QTY(obj));
          GET_CHEMS_QTY(orig) += GET_CHEMS_QTY(obj);
          weight_change_object(orig, GET_OBJ_WEIGHT(obj));
          snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), " into %s.", GET_OBJ_NAME(orig));
        } else {
          // Just give the purchased thing to them directly. Handle restring if needed.
          snprintf(buf2, sizeof(buf2), "You now have %s (contains %d doses).", GET_OBJ_NAME(obj), GET_CHEMS_QTY(obj));
          // buf2 is sent to the character with a newline appended at the end of the function.

          obj_to_char(obj, ch);
        }
      }

      // Give them the item (it's parts or conjuring materials)
      else {
        struct obj_data *orig = ch->carrying;
        for (; orig; orig = orig->next_content)
          if (GET_OBJ_TYPE(obj) == GET_OBJ_TYPE(orig)
              && GET_OBJ_VAL(obj, 0) == GET_OBJ_VAL(orig, 0)
              && GET_OBJ_VAL(obj, 1) == GET_OBJ_VAL(orig, 1))
            break;
        if (orig) {
          GET_OBJ_COST(orig) += GET_OBJ_COST(obj);
          extract_obj(obj);
          obj = NULL;
        } else {
          obj_to_char(obj, ch);
        }
        send_to_char("[OOC: Your purchase has been bundled up into one unit with the appropriate value.]\r\n", ch);
      }

      if (sell) {
        if (sell->type == SELL_BOUGHT && (sell->stock -= bought) <= 0) {
          struct shop_sell_data *temp;
          REMOVE_FROM_LIST(sell, shop_table[shop_nr].selling, next);
          delete sell;
          sell = NULL;
        } else if (sell->type == SELL_STOCK)
          sell->stock = MAX(0, sell->stock - bought);
      }
    }

    // Non-stackable things.
    else {
      while (obj && (bought < buynum
                     && IS_CARRYING_N(ch) < CAN_CARRY_N(ch)
                     && GET_OBJ_WEIGHT(obj) <= CAN_CARRY_W(ch)
                     && (cred ? GET_BANK(ch) : GET_NUYEN(ch)) >= price)) {
        // ID-lock anything that needs locking.
        soulbind_obj_to_char(obj, ch, shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN) || PLR_FLAGGED(ch, PLR_NOT_YET_AUTHED));

        /* Visa defense: Unlock visas so they can be fixed for folks.
        if (GET_OBJ_VNUM(obj) == OBJ_MULTNOMAH_VISA || GET_OBJ_VNUM(obj) == OBJ_CARIBBEAN_VISA)
          GET_VISA_OWNER(obj) = 0
        */

        // Hardened armor is not ID-locked on purchase, but IS on first wear. This unlocks it after the above statement.
        if (IS_OBJ_STAT(obj, ITEM_EXTRA_HARDENED_ARMOR))
          GET_WORN_HARDENED_ARMOR_CUSTOMIZED_FOR(obj) = -1;

        obj_to_char(obj, ch);
        bought++;

        if (sell) {
          obj = NULL;
          switch (sell->type) {
            case SELL_BOUGHT:
              if (--(sell->stock) == 0) {
                struct shop_sell_data *temp = NULL;
                REMOVE_FROM_LIST(sell, shop_table[shop_nr].selling, next);
                DELETE_AND_NULL(sell);
              } else
                obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_SHOP_RECEIVE);
              break;
            case SELL_STOCK:
              sell->stock--;
              if (sell->stock > 0)
                obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_SHOP_RECEIVE);
              break;
            default:
              obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_SHOP_RECEIVE);
              break;
          }
        } else {
          obj = read_object(obj->item_number, REAL, OBJ_LOAD_REASON_SHOP_RECEIVE);
        }

        // Deduct the cost.
        if (cred)
          lose_bank(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);
        else
          lose_nuyen(ch, price, NUYEN_OUTFLOW_SHOP_PURCHASES);
      }
      if (obj) {
        // Obj was loaded but not given to the character.
        extract_obj(obj);
        obj = NULL;
      }
    }

    if (bought < buynum) {
      strlcpy(buf, GET_CHAR_NAME(ch), sizeof(buf));
      if (IS_CARRYING_N(ch) >= CAN_CARRY_N(ch))
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You can only carry %d.", bought);
      else if (GET_OBJ_WEIGHT(ch->carrying) > CAN_CARRY_W(ch))
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You can only carry %d.", bought);
      else if ((cash ? GET_NUYEN(ch) : GET_BANK(ch)) < price)
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You can only afford %d.", bought);
      else
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " I'm only willing to give you %d.", bought); // Error case.
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
  }
  // Write the nuyen cost to buf3 and the current buy-string to arg.
  char price_buf[100], tmp[MAX_INPUT_LENGTH * 2];
  snprintf(price_buf, sizeof(price_buf), "%ld", price * bought);
  strlcpy(tmp, shop_table[shop_nr].buy, sizeof(tmp));

  // Use our new replace_substring() function to swap out all %d's in arg with the nuyen string.
  replace_substring(tmp, buf3, "%d", price_buf);

  // Compose the sayto string for the keeper.
  if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
    snprintf(buf, sizeof(buf), "displays, \"%s\"", buf3);
    do_new_echo(keeper, buf, cmd_echo, 0);
  } else {
    snprintf(buf, sizeof(buf), "%s %s", GET_CHAR_NAME(ch), buf3);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
  }

  if (bought > 1 && print_multiples_at_end)
    snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), " (x%d)", bought);
  send_to_char(buf2, ch);
  send_to_char("\r\n", ch);

  // Log it. Right now, this prints a null object most of the time.
  /*
  if (bought >= 1 && obj) {
    snprintf(buf, sizeof(buf), "Purchased %d of '%s' (%ld) for %d nuyen.", bought, GET_OBJ_NAME(obj), GET_OBJ_VNUM(obj), price);
    mudlog(buf, ch, LOG_GRIDLOG, TRUE);
  }
  */

  if (order) {
    order->number -= bought;
    if (order->number == 0) {
      struct shop_order_data *temp;
      REMOVE_FROM_LIST(order, shop_table[shop_nr].order, next);
      delete order;
    } else if (order->number < 0) {
      mudlog("SYSERR: Purchased quantity greater than ordered quantity in shop_receive()!", ch, LOG_SYSLOG, TRUE);
    }
  }

  return TRUE;
}

// block negotiation for things like chips, parts, conjuring materials, etc-- these should have a flat cost
bool can_negotiate_for_item(struct obj_data *obj) {
  if (GET_OBJ_TYPE(obj) == ITEM_DECK_ACCESSORY && GET_DECK_ACCESSORY_TYPE(obj) == TYPE_PARTS)
    return FALSE;

  if (GET_OBJ_TYPE(obj) == ITEM_MAGIC_TOOL && GET_MAGIC_TOOL_TYPE(obj) == TYPE_SUMMONING)
    return FALSE;

  return TRUE;
}

void shop_buy(char *arg, size_t arg_len, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  struct obj_data *obj = NULL, *cred = get_first_credstick(ch, "credstick");
  struct shop_sell_data *sell;
  int price, buynum;
  bool cash = FALSE;

  // Prevent ghouls from being loved by anyone except their own mother.
  if (IS_GHOUL(ch) && !shop_table[shop_nr].flags.AreAnySet(SHOP_YES_GHOUL, SHOP_CHARGEN, ENDBIT) && !MOB_FLAGGED(keeper, MOB_INANIMATE)) {
    snprintf(buf, sizeof(buf), "%s GET THE FRAG OUTTA HERE GHOUL!", GET_CHAR_NAME(ch));
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return;
  }

  // Prevent negative transactions.
  if ((buynum = transaction_amt(arg, arg_len)) < 0)
  {
    send_to_char("You can't specify a negative amount. Use the SELL command instead for that.\r\n", ch);
    return;
  }

  // Find the item in their list.
  if (!(sell = find_obj_shop(arg, shop_nr, &obj, ch)))
  {
    if (atoi(arg) > 0) {
      // Adapt for the player probably meaning an item number instead of an item with a numeric keyword.
      char oopsbuf[strlen(arg) + 2];
      snprintf(oopsbuf, sizeof(oopsbuf), "#%s", arg);
      sell = find_obj_shop(oopsbuf, shop_nr, &obj, ch);
    }
    if (!sell) {
      if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
        snprintf(buf, sizeof(buf), "displays, \"%s\"", shop_table[shop_nr].no_such_itemk);
        do_new_echo(keeper, buf, cmd_echo, 0);
      } else {
        snprintf(buf, sizeof(buf), "%s %s", GET_CHAR_NAME(ch), shop_table[shop_nr].no_such_itemk);
        do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      }
      return;
    }
  }

  one_argument(arg, buf);

  // Allow specification of cash purchases in grey shops.
  // Fallback: You didn't specify cash, and you have no credstick on hand.
  if (!str_cmp(buf, "cash") || !cred)
  {
    if (shop_table[shop_nr].type == SHOP_LEGAL) {
      if (access_level(ch, LVL_ADMIN)) {
        send_to_char(ch, "You stare unblinkingly at %s until %s make%s an exception to the no-credstick, no-sale policy.\r\n",
                     GET_NAME(keeper),
                     HSSH(keeper),
                     HSSH_SHOULD_PLURAL(keeper) ? "s" : ""
                    );
      } else {
        snprintf(buf, sizeof(buf), "%s No Credstick, No Sale.", GET_CHAR_NAME(ch));
        do_say(keeper, buf, cmd_say, SCMD_SAYTO);
        send_to_char("You need to have an activated credstick in your inventory to purchase that.\r\n", ch);
        if (obj)
          extract_obj(obj);
        return;
      }
    }

    // Strip out the CASH argument.
    if (!str_cmp(buf, "cash")) {
      arg = any_one_arg(arg, buf);
      skip_spaces(&arg);
    } else {
      send_to_char("Lacking an activated credstick, you choose to deal in cash.\r\n", ch );
    }

    cash = TRUE;
  }

  // You have a credstick, but the shopkeeper doesn't want it.
  if (!cash && cred && shop_table[shop_nr].type == SHOP_BLACK) {
    send_to_char("The shopkeeper refuses to deal with credsticks.\r\n", ch);
    cash = TRUE;
  }

  // You must clarify what you want to buy.
  if (!*arg || !buynum) {
    snprintf(buf, sizeof(buf), "%s What do you want to buy?", GET_CHAR_NAME(ch));
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    if (obj)
      extract_obj(obj);
    return;
  }

  // Calculate the price.
  price = buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch);
  int bprice = price / 10;
  if (!shop_table[shop_nr].flags.IsSet(SHOP_WONT_NEGO) && !MOB_FLAGGED(keeper, MOB_INANIMATE) && can_negotiate_for_item(obj))
    price = negotiate(ch, keeper, 0, price, 0, TRUE, TRUE);

  // Attempt to order the item.
  if (sell->type == SELL_AVAIL && GET_OBJ_AVAILTN(obj) > 0)
  {
    if (GET_AVAIL_OFFSET(ch) > 0) {
      price += bprice * GET_AVAIL_OFFSET(ch);
    }

    // Don't let people re-try repeatedly.
    for (int q = 0; q < SHOP_LAST_IDNUM_LIST_SIZE; q++) {
      if (sell->lastidnum[q] == GET_IDNUM(ch)) {
        snprintf(buf, sizeof(buf), "%s Sorry, I couldn't get that in for you. Try again tomorrow.", GET_CHAR_NAME(ch));
        do_say(keeper, buf, cmd_say, SCMD_SAYTO);
        extract_obj(obj);
        return;
      }
    }

    // Stop people from buying enormous quantities.
    extern int max_things_you_can_purchase_at_once;
    if (buynum > max_things_you_can_purchase_at_once) {
      snprintf(buf, sizeof(buf), "%s I can't get that many in at once. Limit is %d.", GET_CHAR_NAME(ch), max_things_you_can_purchase_at_once);
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      extract_obj(obj);
      return;
    }

    // Prevent trying to pre-order something if you don't have the scratch. Calculated using the flat price, not the negotiated one.
    long preorder_cost_for_one_object = GET_OBJ_COST(obj) / PREORDER_COST_DIVISOR;

    if (!cred || shop_table[shop_nr].type == SHOP_BLACK) {
      cash = TRUE;
      cred = NULL;
    }

    if (!cash && !cred) {
      mudlog("SYSERR: Ended up with !cash and !cred in shop purchasing!", ch, LOG_SYSLOG, TRUE);
      send_to_char(ch, "Sorry, something went wrong.\r\n");
      extract_obj(obj);
      return;
    }

    long calculated_cost = preorder_cost_for_one_object * buynum;
    if ((cash && GET_NUYEN(ch) < calculated_cost)
        || (cred && GET_BANK(ch) < calculated_cost))
    {
      snprintf(buf, sizeof(buf), "%s It'll cost you %ld nuyen to place that order. Come back when you've got the funds.", GET_CHAR_NAME(ch), calculated_cost);
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      extract_obj(obj);
      return;
    }

    struct obj_data *phero = find_bioware(ch, BIO_TAILOREDPHEROMONES);

    int success = get_eti_test_results(ch,
                                       shop_table[shop_nr].etiquette,
                                       GET_OBJ_AVAILTN(obj),
                                       GET_AVAIL_OFFSET(ch),
                                       GET_POWER(ch, ADEPT_KINESICS),
                                       get_metavariant_penalty(ch, keeper),
                                       abs(GET_BEST_LIFESTYLE(ch)),
                                       phero ? GET_BIOWARE_RATING(phero) * (GET_BIOWARE_IS_CULTURED(phero) ? 2 : 1) : 0,
                                       0);

    // Failure case.
    if (success < 1) {
      if (GET_SKILL(ch, shop_table[shop_nr].etiquette) == 0) {
        if (phero)
          snprintf(buf, sizeof(buf), "Not even your tailored pheromones can soothe $N's annoyance at your lack of %s.\r\n",
                   skills[shop_table[shop_nr].etiquette].name);
        else
          snprintf(buf, sizeof(buf), "$N seems annoyed that you don't even know the basics of %s.\r\n",
                   skills[shop_table[shop_nr].etiquette].name);
      } else {
        snprintf(buf, sizeof(buf), "You exert every bit of %s you can muster, %sbut $N shakes $S head after calling a few contacts.\r\n",
                 skills[shop_table[shop_nr].etiquette].name,
                 phero ? "aided by your tailored pheromones, " : "");
      }
      act(buf, FALSE, ch, 0, keeper, TO_CHAR);

      snprintf(buf, sizeof(buf), "%s I can't get ahold of that one for a while.", GET_CHAR_NAME(ch));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);

      // Add them to the forbidden list.
      for (int q = SHOP_LAST_IDNUM_LIST_SIZE - 1; q >= 1; q--)
        sell->lastidnum[q] = sell->lastidnum[q-1];
      sell->lastidnum[0] = GET_IDNUM(ch);

      extract_obj(obj);
      return;
    }

    if (GET_SKILL(ch, shop_table[shop_nr].etiquette) == 0) {
      snprintf(buf, sizeof(buf), "$N seems annoyed that you don't even know the basics of %s, but %syou convince $M to call a few contacts anyways.\r\n",
              skills[shop_table[shop_nr].etiquette].name,
              phero ? "aided by your tailored pheromones, " : "");
    } else {
      snprintf(buf, sizeof(buf), "You exert every bit of %s you can muster, %sand $N nods to you after calling a few contacts.\r\n",
               skills[shop_table[shop_nr].etiquette].name,
               phero ? "aided by your tailored pheromones, " : "");
    }
    act(buf, FALSE, ch, 0, keeper, TO_CHAR);

    // Placed order successfully. Order time is multiplied by 10% per availoffset tick, then multiplied again by quantity.
    float totaltime = (GET_OBJ_AVAILDAY(obj) * (GET_AVAIL_OFFSET(ch) ? 0.1 * GET_AVAIL_OFFSET(ch) : 1) * buynum) / success;

    if (access_level(ch, LVL_VICEPRES)) {
      send_to_char(ch, "You use your staff powers to greatly accelerate the ordering process (was %.2f days).\r\n", totaltime);
      totaltime = 0.0;
    }

    // Pay the preorder cost.
    if (cash) {
      lose_nuyen(ch, calculated_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);
    } else {
      lose_bank(ch, calculated_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);
    }
    send_to_char(ch, "You put down a %ld nuyen deposit on your order.\r\n", calculated_cost);

    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      if (totaltime < 1) {
        int hours = MAX(1, (int)(24 * totaltime));
        snprintf(buf, sizeof(buf), "displays, \"ETA for %s: %d hour%s.\"",
                 get_string_after_color_code_removal(GET_OBJ_NAME(obj), NULL),
                 hours,
                 hours == 1 ? "" : "s");
      } else {
        snprintf(buf, sizeof(buf), "displays, \"ETA for %s: %d day%s.\"",
                 get_string_after_color_code_removal(GET_OBJ_NAME(obj), NULL),
                 (int) totaltime,
                 totaltime == 1 ? "" : "s");
      }
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      if (totaltime < 1) {
        int hours = MAX(1, (int)(24 * totaltime));
        snprintf(buf, sizeof(buf), "%s %s will take about %d hour%s to come in.",
                 GET_CHAR_NAME(ch),
                 get_string_after_color_code_removal(GET_OBJ_NAME(obj), NULL),
                 hours,
                 hours == 1 ? "" : "s");
      } else {
        snprintf(buf, sizeof(buf), "%s %s will take about %d day%s to come in.",
                 GET_CHAR_NAME(ch),
                 get_string_after_color_code_removal(GET_OBJ_NAME(obj), NULL),
                 (int) totaltime,
                 totaltime == 1 ? "" : "s");
      }
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }

    // If they have a pre-existing order, just bump up the quantity and update the order time.
    struct shop_order_data *order = shop_table[shop_nr].order;
    for (; order; order = order->next)
      if (order->player == GET_IDNUM(ch) && order->item == sell->vnum)
        break;

    if (order) {
      if (order->timeavail < time(0))
        order->timeavail = time(0);
      order->number += buynum;
      order->timeavail = order->timeavail + (int)(SECS_PER_MUD_DAY * totaltime);
    } else {
      // Create a new order.
      struct shop_order_data *order = new shop_order_data;
      order->item = sell->vnum;
      order->player = GET_IDNUM(ch);
      order->timeavail = time(0) + (int)(SECS_PER_MUD_DAY * totaltime);
      order->number = buynum;
      order->price = price;
      order->next = shop_table[shop_nr].order;
      order->sent = FALSE;
      shop_table[shop_nr].order = order;
      order->paid = preorder_cost_for_one_object;
      order->expiration = order->timeavail + (60 * 60 * 24 * PREORDERS_ARE_GOOD_FOR_X_DAYS);
    }

    // Clean up.
    extract_obj(obj);
    obj = NULL;

    // New characters get reminded how to obtain their order.
    if (SHOULD_SEE_TIPS(ch))
      send_to_char("\r\nYou can ^WCHECK^n the status of your order and ^WRECEIVE^n it when it's here.\r\n", ch);
  } else
  {
    // Give them the thing without fanfare.
    shop_receive(ch, keeper, arg, buynum, cash, sell, obj, shop_table[shop_nr].type == SHOP_BLACK ? NULL : cred, price, shop_nr, NULL);
  }
}

int negotiate_and_payout_sellprice(struct char_data *ch, struct char_data *keeper, vnum_t shop_nr, int sellprice) {
#ifdef USE_HAMMERSPACE
  // Since we added hammerspace/stowage, more loot is being collected faster, and the friction point of having to do loot
  // runs to drones/cars/etc has gone away. Since the speed at which farming can be done has increased, we must decrease
  // the nuyen gained from farming. In lieu of builders going through and lowering the value of all items in the game,
  // we apply a modifier here to reduce the sell price of an item accordingly. This doesn't apply in chargen, of course.
  if (!shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN))
    sellprice *= 0.70;
#endif

  // Negotiate the total cost.
  if (!shop_table[shop_nr].flags.IsSet(SHOP_WONT_NEGO) && !MOB_FLAGGED(keeper, MOB_INANIMATE))
    sellprice = negotiate(ch, keeper, 0, sellprice, 0, FALSE, TRUE);

  // Pay it out as nuyen.
  if (shop_table[shop_nr].type == SHOP_BLACK)
    gain_nuyen(ch, sellprice, NUYEN_INCOME_SHOP_SALES);
  else
    gain_bank(ch, sellprice, NUYEN_INCOME_SHOP_SALES);

  // Just in case the caller cares about the negotiated price.
  return sellprice;
}

void shop_sell(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH], buf3[MAX_STRING_LENGTH];

  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;

  struct obj_data *obj = NULL, *cred = get_first_credstick(ch, "credstick");
  struct shop_sell_data *sell = shop_table[shop_nr].selling;

  if (!*arg) {
    send_to_char("Syntax: SELL <item>, or SELL STOWED to sell your stowed loot.\r\n", ch);
    return;
  }

  // Prevent ghouls from being loved by anyone except their own mother.
  if (IS_GHOUL(ch) && !shop_table[shop_nr].flags.AreAnySet(SHOP_YES_GHOUL, SHOP_CHARGEN, ENDBIT) && !MOB_FLAGGED(keeper, MOB_INANIMATE)) {
    snprintf(buf, sizeof(buf), "%s GET THE FRAG OUTTA HERE GHOUL!", GET_CHAR_NAME(ch));
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return;
  }

  if (!str_cmp(arg, "stowed")) {
    sell_all_stowed_items(ch, shop_nr, keeper);
    return;
  }

  // Find the object.
  obj = get_obj_in_list_vis(ch, arg, ch->carrying);
  if (!obj && shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
    if (!(obj = get_obj_in_list_vis(ch, arg, ch->cyberware))) {
      obj = get_obj_in_list_vis(ch, arg, ch->bioware);
    }
    if (obj && !shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN)) {
      send_to_char(ch, "You'll have to uninstall %s^n before you can sell it.\r\n", decapitalize_a_an(GET_OBJ_NAME(obj)));
      return;
    }
  }

  if (!obj) {
    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      snprintf(buf, sizeof(buf), "@self displays, \"%s\"", shop_table[shop_nr].no_such_itemp);
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      snprintf(buf, sizeof(buf) - strlen(buf), "%s %s", GET_CHAR_NAME(ch), shop_table[shop_nr].no_such_itemp);
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
    return;
  }

  if (IS_OBJ_STAT(obj, ITEM_EXTRA_KEPT)) {
    send_to_char(ch, "You'll have to use the KEEP command on %s before you can sell it.", decapitalize_a_an(GET_OBJ_NAME(obj)));
    return;
  }

  if (GET_OBJ_TYPE(obj) == ITEM_SHOPCONTAINER) {
    if (!shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
      if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
        strlcpy(buf, "@self displays, \"Error: No surgical suite available.\"", sizeof(buf));
        do_new_echo(keeper, buf, cmd_echo, 0);
      } else {
        snprintf(buf, sizeof(buf), "%s I won't buy %s off of you. Take it to a cyberdoc.", GET_CHAR_NAME(ch), GET_OBJ_NAME(obj));
        do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      }
      return;
    }

    if (!obj->contains) {
      send_to_char(ch, "%s is empty!\r\n", capitalize(GET_OBJ_NAME(obj)));
      snprintf(buf, sizeof(buf), "SYSERR: Shop container '%s' is empty!", GET_OBJ_NAME(obj));
      mudlog(buf, ch, LOG_SYSLOG, TRUE);
      return;
    }

    obj = obj->contains;
  }

  if (!shop_will_buy_item_from_ch(shop_nr, obj, ch))
  {
    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      snprintf(buf, sizeof(buf), "@self displays, \"%s\"", shop_table[shop_nr].doesnt_buy);
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      snprintf(buf, sizeof(buf), "%s %s", GET_CHAR_NAME(ch), shop_table[shop_nr].doesnt_buy);
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
    return;
  }

  if (shop_table[shop_nr].type == SHOP_LEGAL && !cred)
  {
    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      snprintf(buf, sizeof(buf), "@self displays, \"Error: Credstick required for transactions.\"");
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      snprintf(buf, sizeof(buf), "%s No cred, no business.", GET_CHAR_NAME(ch));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
    return;
  }

  int sellprice = sell_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch);

  if (shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR) && !obj->in_obj) {
    for (struct obj_data *ware_verifier = ch->carrying; ware_verifier; ware_verifier = ware_verifier->next_content) {
      if (ware_verifier == obj) {
        mudlog("SYSERR: carrying an uninstalled piece of 'ware!", ch, LOG_SYSLOG, TRUE);
        return;
      }
    }

    if (GET_CYBERWARE_TYPE(obj) == CYB_CHIPJACK && obj->contains) {
      send_to_char("You can't uninstall a chipjack with chips in it.\r\n", ch);
      return;
    }

    if (GET_CYBERWARE_TYPE(obj) == CYB_MEMORY && obj->contains) {
      send_to_char("You can't uninstall headware memory with data in it.\r\n", ch);
      return;
    }

    if (!uninstall_ware_from_target_character(obj, keeper, ch, !shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN))) {
      mudlog_vfprintf(ch, LOG_SYSLOG, "SYSERR: Failed to shop-uninstall %s from %s: Bailing out without paying them.", GET_OBJ_NAME(obj), GET_CHAR_NAME(ch));
      send_to_char(ch, "You're not able to sell %s right now.\r\n", GET_OBJ_NAME(obj));
      return;
    }
  }
  else {
    if (obj->in_obj) {
      struct obj_data *container = obj->in_obj;

      // Pull the 'ware out.
      obj_from_obj(obj);

      // Remove the container and junk it.
      obj_from_char(container);
      extract_obj(container);
      container = NULL;
    } else {
      obj_from_char(obj);
    }
  }

  negotiate_and_payout_sellprice(ch, keeper, shop_nr, sellprice);

  const char *representation = generate_new_loggable_representation(obj);
  snprintf(buf3, sizeof(buf3), "%s sold %s^g at %s^g (%ld) for %d.", GET_CHAR_NAME(ch), representation, GET_CHAR_NAME(keeper), shop_table[shop_nr].vnum, sellprice);
  delete [] representation;
  mudlog(buf3, ch, LOG_GRIDLOG, TRUE);

  // Write the nuyen cost to buf3 and the current buy-string to arg.
  char price_buf[100], tmp[MAX_INPUT_LENGTH * 2];
  snprintf(price_buf, sizeof(price_buf), "%d", sellprice);
  strlcpy(tmp, shop_table[shop_nr].sell, sizeof(tmp));

  // Use our new replace_substring() function to swap out all %d's in arg with the nuyen string.
  replace_substring(tmp, buf3, "%d", price_buf);

  if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
    snprintf(buf, sizeof(buf), "@self displays, \"%s\"", buf3);
    do_new_echo(keeper, buf, cmd_echo, 0);
  } else {
    snprintf(buf, sizeof(buf), "%s %s", GET_CHAR_NAME(ch), buf3);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
  }

  for (;sell; sell = sell->next)
    if (sell->vnum == GET_OBJ_VNUM(obj))
      break;
  if (!sell)
  {
    if (!shop_table[shop_nr].flags.IsSet(SHOP_NORESELL) && GET_OBJ_TYPE(obj) != ITEM_GUN_MAGAZINE && GET_OBJ_TYPE(obj) !=
        ITEM_CYBERWARE && GET_OBJ_TYPE(obj) != ITEM_BIOWARE) {
      sell = new shop_sell_data;
      sell->type = SELL_BOUGHT;
      sell->stock = 1;
      sell->vnum = GET_OBJ_VNUM(obj);
      sell->next = shop_table[shop_nr].selling;
      shop_table[shop_nr].selling = sell;
    }
    int x = 0;
    for (sell = shop_table[shop_nr].selling; sell; sell = sell->next)
      x++;
    if (x > MAX_ITEMS_IN_SHOP_INVENTORY) {
      struct shop_sell_data *temp;
      x = number(1, x-1);
      for (sell = shop_table[shop_nr].selling; sell && x > 0; sell = sell->next)
        x--;
      REMOVE_FROM_LIST(sell, shop_table[shop_nr].selling, next);
    }
  } 
  else if ((sell->type == SELL_STOCK || sell->type == SELL_BOUGHT) && sell->stock <= 10) {
    sell->stock++;
  }

  // Track ammo sold.
  if (GET_OBJ_TYPE(obj) == ITEM_WEAPON && WEAPON_IS_GUN(obj) && obj->contains && GET_OBJ_TYPE(obj->contains) == ITEM_GUN_MAGAZINE)
    AMMOTRACK_OK(GET_MAGAZINE_BONDED_ATTACKTYPE(obj->contains), GET_MAGAZINE_AMMO_TYPE(obj->contains), AMMOTRACK_SOLD, -GET_MAGAZINE_AMMO_COUNT(obj->contains));

  extract_obj(obj);
  obj = NULL;
}

void shop_list(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  char formatstr[MAX_STRING_LENGTH];
  char paddingnumberstr[12];
  if (!is_open(keeper, shop_nr))
    return;

  /*   We allow anyone to view the list, on the presumption that you're just browsing the shelves or whatnot.
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  */

  // No projecting through doors to get price lists etc though.
  if (IS_PROJECT(ch)) {
    send_to_char("You're not able to read the prices from the astral plane.\r\n", ch);
    return;
  }

  struct obj_data *obj;
  int i = 1;
  bool has_availtns = FALSE;
  bool has_negotiatable = FALSE;

  if (PRF_FLAGGED(ch, PRF_SCREENREADER)) {
    snprintf(buf, sizeof(buf), "%s has the following items available for sale:\r\n", GET_NAME(keeper));

    for (struct shop_sell_data *sell = shop_table[shop_nr].selling; sell; sell = sell->next, i++) {
      // Read the object; however, if it's an invalid vnum or has no sale cost, skip it.
      obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_EDITING_EPHEMERAL_LOOKUP);
      if (!shop_can_sell_object(obj, keeper, shop_nr)) {
        i--;
        continue;
      }

      has_negotiatable |= can_negotiate_for_item(obj);

      // List the item to the player.
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Item #%d: %s for %d nuyen", i, GET_OBJ_NAME(obj), buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));

      // Doctorshop? Tack on bioware / cyberware info.
      if (shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "; it's %s that costs %.2f %s",
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? "cyberware" : "bioware",
                ((float)calculate_ware_essence_or_index_cost(ch, obj) / 100),
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? "essence" : "bio index");

      }

      // Finish up with availability info.
      if (!(sell->type == SELL_ALWAYS) && !(sell->type == SELL_AVAIL && GET_OBJ_AVAILDAY(obj) == 0)) {
        if (sell->type == SELL_AVAIL) {
          has_availtns = TRUE;
          int arbitrary_difficulty = GET_OBJ_AVAILTN(obj);
          if (arbitrary_difficulty <= 2) {
            strlcat(buf, ". It's a trivial special order", sizeof(buf));
          } else if (arbitrary_difficulty <= 4) {
            strlcat(buf, ". It's an easy special order", sizeof(buf));
          } else if (arbitrary_difficulty <= 7) {
            strlcat(buf, ". It's a moderately-difficult special order", sizeof(buf));
          } else if (arbitrary_difficulty <= 10) {
            strlcat(buf, ". It's a special order that will take some serious convincing", sizeof(buf));
          } else {
            strlcat(buf, ". It's a special order that probably needs a fixer", sizeof(buf));
          }
        } else if (sell->stock <= 0) {
          strlcat(buf, ". It is currently out of stock", sizeof(buf));
        } else {
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), ". Only %d %s in stock", sell->stock, sell->stock > 1 ? "are" : "is");
        }
      }

      if (IS_OBJ_STAT(obj, ITEM_EXTRA_NERPS))
        strlcat(buf, ". OOC note: It has no coded effect", sizeof(buf));

      strlcat(buf, ".\r\n", sizeof(buf));

      // Clean up so we don't leak the object.
      extract_obj(obj);
      obj = NULL;
    }
    strlcat(buf, "\r\nYou can use PROBE #1 or INFO #1 for more details.\r\n", sizeof(buf));

    if (has_availtns)
      snprintf(ENDOF(buf), sizeof(buf), "This shop uses %s for difficult purchases.\r\n", skills[shop_table[shop_nr].etiquette].name);

    if (shop_table[shop_nr].flags.IsSet(SHOP_WONT_NEGO) || MOB_FLAGGED(keeper, MOB_INANIMATE))
      has_negotiatable = FALSE;

    // Add info about metavariant penalties.
    if (SHOULD_SEE_TIPS(ch) && get_metavariant_penalty(ch, keeper) && (has_availtns || has_negotiatable)) {
      snprintf(ENDOF(buf), sizeof(buf), "As %s %s, you have a penalty on%s%s%s tests here.\r\n",
               AN(pc_race_types_decap[(int)GET_RACE(ch)]),
               pc_race_types_decap[(int)GET_RACE(ch)],
               has_availtns ? " etiquette" : "",
               has_availtns && has_negotiatable ? " and" : "",
               has_negotiatable ? " negotiation" : "");
    }

    page_string(ch->desc, buf, 1);
    return;
  }


  if (shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
    strlcpy(buf, " **   Avail    Item                                                      Rating  Ess/Index     Price\r\n"
                 "----------------------------------------------------------------------------------------------------\r\n", sizeof(buf));

    for (struct shop_sell_data *sell = shop_table[shop_nr].selling; sell; sell = sell->next, i++) {
      obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_EDITING_EPHEMERAL_LOOKUP);
      if (!shop_can_sell_object(obj, keeper, shop_nr)) {
        i--;
        continue;
      }
      has_negotiatable |= can_negotiate_for_item(obj);
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %3d)  ", i);
      if (sell->type == SELL_ALWAYS || (sell->type == SELL_AVAIL && GET_OBJ_AVAILTN(obj) == 0))
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Yes      ");
      else if (sell->type == SELL_AVAIL) {
        has_availtns = TRUE;
        int arbitrary_difficulty = GET_OBJ_AVAILTN(obj);
        if (arbitrary_difficulty <= 2) {
          strlcat(buf, "Trivial  ", sizeof(buf));
        } else if (arbitrary_difficulty <= 4) {
          strlcat(buf, "Easy     ", sizeof(buf));
        } else if (arbitrary_difficulty <= 7) {
          strlcat(buf, "Medium   ", sizeof(buf));
        } else if (arbitrary_difficulty <= 10) {
          strlcat(buf, "Hard     ", sizeof(buf));
        } else if (arbitrary_difficulty <= 14) {
          strlcat(buf, "Harder   ", sizeof(buf));
        } else {
          strlcat(buf, "Fixer    ", sizeof(buf));
        }
        /*
        if (GET_OBJ_AVAILDAY(obj) < 1)
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "~%-2d Hours      ", (int)(24 * GET_OBJ_AVAILDAY(obj)));
        else
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "~%-2d Day%c       ", (int)GET_OBJ_AVAILDAY(obj), GET_OBJ_AVAILDAY(obj) > 1 ? 's' : ' ');
        */
      } else {
        if (sell->stock <= 0)
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "SoldOut  ");
        else
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%-3d      ", sell->stock);
      }
      if (GET_OBJ_VAL(obj, 1) > 0)
        snprintf(buf2, sizeof(buf2), "%d", GET_OBJ_VAL(obj, 1));
      else strlcpy(buf2, "-", sizeof(buf2));

      if (IS_OBJ_STAT(obj, ITEM_EXTRA_NERPS)) {
        //Format string: "^Y(N)^n %-58s^n %-6s%2s   %0.2f%c  %9d\r\n"
        //We apply padding for color codes here.
        snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 53 + count_color_codes_in_string(GET_OBJ_NAME(obj)));
        snprintf(formatstr, sizeof(formatstr), "%s%s%s", "^Y(N)^n %-", paddingnumberstr, "s^n %-6s%2s   %0.2f%c  %9d\r\n");
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, GET_OBJ_NAME(obj),
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? "Cyber" : "Bio", buf2, ((float)GET_OBJ_VAL(obj, 4) / 100),
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? 'E' : 'I', buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));
      } else {
        //Format string: "%-62s^n %-6s%2s   %0.2f%c  %9d\r\n"
        //We apply padding for color codes here.
        snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 57 + count_color_codes_in_string(GET_OBJ_NAME(obj)));
        snprintf(formatstr, sizeof(formatstr), "%s%s%s", "%-", paddingnumberstr, "s^n %-6s%2s   %0.2f%c  %9d\r\n");
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, GET_OBJ_NAME(obj),
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? "Cyber" : "Bio", buf2, ((float)GET_OBJ_VAL(obj, 4) / 100),
                GET_OBJ_TYPE(obj) == ITEM_CYBERWARE ? 'E' : 'I', buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));
      }
      extract_obj(obj);
      obj = NULL;
    }
    send_to_char(buf, ch);
    // Tips and TN skill are shown at the end of the function.
  } else {
    send_to_char(ch, " **   Avail    Item                                                                          Price\r\n"
                     "----------------------------------------------------------------------------------------------------\r\n");
    for (struct shop_sell_data *sell = shop_table[shop_nr].selling; sell; sell = sell->next, i++) {
      obj = read_object(sell->vnum, VIRTUAL, OBJ_LOAD_REASON_EDITING_EPHEMERAL_LOOKUP);
      if (!shop_can_sell_object(obj, keeper, shop_nr)) {
        i--;
        continue;
      }
      has_negotiatable |= can_negotiate_for_item(obj);
      snprintf(buf, sizeof(buf), " %2d)  ", i);
      if (sell->type == SELL_ALWAYS || (sell->type == SELL_AVAIL && GET_OBJ_AVAILTN(obj) == 0))
        strlcat(buf, "Yes      ", sizeof(buf));
      else if (sell->type == SELL_AVAIL) {
        has_availtns = TRUE;
        int arbitrary_difficulty = GET_OBJ_AVAILTN(obj);
        if (arbitrary_difficulty <= 2) {
          strlcat(buf, "Trivial  ", sizeof(buf));
        } else if (arbitrary_difficulty <= 4) {
          strlcat(buf, "Easy     ", sizeof(buf));
        } else if (arbitrary_difficulty <= 7) {
          strlcat(buf, "Medium   ", sizeof(buf));
        } else if (arbitrary_difficulty <= 10) {
          strlcat(buf, "Hard     ", sizeof(buf));
        } else if (arbitrary_difficulty <= 14) {
          strlcat(buf, "Harder   ", sizeof(buf));
        } else {
          strlcat(buf, "Fixer    ", sizeof(buf));
        }
      } else {
        if (sell->stock <= 0)
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "SoldOut  ");
        else
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%-3d     ", sell->stock);
      }

      if (IS_OBJ_STAT(obj, ITEM_EXTRA_NERPS)) {
        //Format string for reference: "^Y(N)^n %-44s^n %6d\r\n"
        //We apply padding for color codes here.
        snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 71 + count_color_codes_in_string(GET_OBJ_NAME(obj)));
        snprintf(formatstr, sizeof(formatstr), "%s%s%s", "^Y(N)^n %-", paddingnumberstr, "s^n %7d\r\n");
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, GET_OBJ_NAME(obj), buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));
      } else {
        //Format string for reference: "%-48s^n %6d\r\n"
        //We apply padding for color codes here.
        snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 75 + count_color_codes_in_string(GET_OBJ_NAME(obj)));
        snprintf(formatstr, sizeof(formatstr), "%s%s%s", "%-", paddingnumberstr, "s^n %7d\r\n");
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, GET_OBJ_NAME(obj),
                  buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));
      }
      send_to_char(buf, ch);
      extract_obj(obj);
      obj = NULL;
    }
  }

  // New characters get reminded about the probe and info commands.
  if (SHOULD_SEE_TIPS(ch))
    send_to_char("\r\nUse ^WPROBE^n for more details.\r\n", ch);

  // Inform about the etti type used here.
  if (has_availtns) {
    send_to_char(ch, "This shop uses %s for difficult purchases.\r\n", skills[shop_table[shop_nr].etiquette].name);
  }

  if (shop_table[shop_nr].flags.IsSet(SHOP_WONT_NEGO) || MOB_FLAGGED(keeper, MOB_INANIMATE))
    has_negotiatable = FALSE;

  // Add info about metavariant penalties.
  if (SHOULD_SEE_TIPS(ch) && get_metavariant_penalty(ch, keeper) && (has_availtns || has_negotiatable)) {
    send_to_char(ch, "As %s %s, you have a penalty on%s%s%s tests here.\r\n",
                 AN(pc_race_types_decap[(int)GET_RACE(ch)]),
                 pc_race_types_decap[(int)GET_RACE(ch)],
                 has_availtns ? " etiquette" : "",
                 has_availtns && has_negotiatable ? " and" : "",
                 has_negotiatable ? " negotiation" : "");
  }
}

void shop_value(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  struct obj_data *obj;
  strlcpy(buf, GET_CHAR_NAME(ch), sizeof(buf));
  if (!*arg)
  {
    send_to_char("What item do you want valued?\r\n", ch);
    return;
  }
  if (shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR))
  {
    if (!(obj = get_obj_in_list_vis(ch, arg, ch->cyberware))
        && !(obj = get_obj_in_list_vis(ch, arg, ch->bioware))
        && !(obj = get_obj_in_list_vis(ch, arg, ch->carrying))) {
      send_to_char(ch, "You don't seem to have a '%s'.\r\n", arg);
      return;
    }
  } else
  {
    if (!(obj = get_obj_in_list_vis(ch, arg, ch->carrying))) {
      send_to_char(ch, "You don't seem to have a '%s'.\r\n", arg);
      return;
    }
  }

  if (GET_OBJ_TYPE(obj) == ITEM_SHOPCONTAINER) {
    if (!shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
      snprintf(buf, sizeof(buf), "%s I wouldn't buy %s off of you. Take it to a cyberdoc.", GET_CHAR_NAME(ch), GET_OBJ_NAME(obj));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      return;
    }

    if (!obj->contains) {
      send_to_char(ch, "%s is empty!\r\n", capitalize(GET_OBJ_NAME(obj)));
      snprintf(buf, sizeof(buf), "SYSERR: Shop container '%s' is empty!", GET_OBJ_NAME(obj));
      mudlog(buf, ch, LOG_SYSLOG, TRUE);
      return;
    }

    obj = obj->contains;

    int install_cost = get_cyberware_install_cost(obj);

    snprintf(buf, sizeof(buf), "%s I'd charge %d nuyen to install it, and", GET_CHAR_NAME(ch), install_cost);
  } else {
    // Since we're not pre-filling buf with something else to say, just stick the name in for the sayto target.
    strlcpy(buf, GET_CHAR_NAME(ch), sizeof(buf));
  }

  if (!shop_will_buy_item_from_ch(shop_nr, obj, ch))
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " I wouldn't buy %s off of you.", GET_OBJ_NAME(obj));
  else
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " I would be able to give you around %d nuyen for %s.", sell_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch), GET_OBJ_NAME(obj));

  do_say(keeper, buf, cmd_say, SCMD_SAYTO);
}

bool shop_probe(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr) {
  if (!is_open(keeper, shop_nr))
    return FALSE;

  /*   We allow anyone to view the list, on the presumption that you're just browsing the shelves or whatnot.
  // if (!is_ok_char(keeper, ch, shop_nr))
  //  return FALSE;
  */

  struct obj_data *obj = NULL;
  skip_spaces(&arg);

  if (!*arg) {
    // No error message, let do_probe() handle it.
    return FALSE;
  }

  // By popular request, all shop item scans must start with # now.
  if (*arg != '#')
    return FALSE;

  struct shop_sell_data *sell = find_obj_shop(arg, shop_nr, &obj, ch);
  if (!sell && atoi(arg) > 0) {
    // Adapt for the player probably meaning an item number instead of an item with a numeric keyword.
    char oopsbuf[strlen(arg) + 2];
    snprintf(oopsbuf, sizeof(oopsbuf), "#%s", arg);
    sell = find_obj_shop(oopsbuf, shop_nr, &obj, ch);
  }

  if (!sell || !obj) {
    return FALSE;
  }

  send_to_char(ch, "^yProbing ^Yshopkeeper's^y ^n%s^y...^n\r\n", GET_OBJ_NAME(obj));
  do_probe_object(ch, obj, TRUE);
  return TRUE;
}

void shop_info(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  int num = 0, num2 = 0;
  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  struct obj_data *obj;
  skip_spaces(&arg);

  if (!*arg) {
    send_to_char("Syntax: INFO <item>\r\n", ch);
    return;
  }

  if (!find_obj_shop(arg, shop_nr, &obj, ch))
  {
    bool successful = FALSE;
    if (atoi(arg) > 0) {
      // Adapt for the player probably meaning an item number instead of an item with a numeric keyword.
      char oopsbuf[strlen(arg) + 2];
      snprintf(oopsbuf, sizeof(oopsbuf), "#%s", arg);
      successful = (find_obj_shop(oopsbuf, shop_nr, &obj, ch) != NULL);
    }
    if (!successful) {
      snprintf(buf, sizeof(buf), "%s I don't have that item.", GET_CHAR_NAME(ch));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      return;
    }
  }
  snprintf(buf, sizeof(buf), "%s %s is", GET_CHAR_NAME(ch), CAP(obj->text.name));
  switch (GET_OBJ_TYPE(obj))
  {
  case ITEM_WEAPON:
    if (WEAPON_IS_GUN(obj)) {
      if (GET_OBJ_VAL(obj, 0) < 3)
        strlcat(buf, " a weak", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 0) < 6)
        strlcat(buf, " a low powered", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 0) < 10)
        strlcat(buf, " a moderately powered", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 0) < 12)
        strlcat(buf, " a strong", sizeof(buf));
      else
        strlcat(buf, " a high powered", sizeof(buf));

      if (IS_SET(GET_OBJ_VAL(obj, 10), 1 << MODE_SS))
        strlcat(buf, " single shot", sizeof(buf));
      else if (IS_SET(GET_OBJ_VAL(obj, 10), 1 << MODE_FA))
        strlcat(buf, " fully automatic", sizeof(buf));
      else if (IS_SET(GET_OBJ_VAL(obj, 10), 1 << MODE_BF))
        strlcat(buf, " burst-fire", sizeof(buf));
      else if (IS_SET(GET_OBJ_VAL(obj, 10), 1 << MODE_SA))
        strlcat(buf, " semi-automatic", sizeof(buf));
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %s", weapon_types[GET_OBJ_VAL(obj, 3)]);
      if (IS_OBJ_STAT(obj, ITEM_EXTRA_TWOHANDS))
        strlcat(buf, " and requires two hands to wield correctly", sizeof(buf));
      if (GET_WEAPON_INTEGRAL_RECOIL_COMP(obj))
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), ". It has %d round%s of built-in recoil compensation",
                GET_WEAPON_INTEGRAL_RECOIL_COMP(obj),
                GET_WEAPON_INTEGRAL_RECOIL_COMP(obj) > 1 ? "s" : "");
      if (GET_OBJ_VAL(obj, 7) > 0 || GET_OBJ_VAL(obj, 8) > 0 || GET_OBJ_VAL(obj, 9) > 0)
        strlcat(buf, ". It comes standard with ", sizeof(buf));

      int real_obj;
      if (GET_OBJ_VAL(obj, 7) > 0 && (real_obj = real_object(GET_OBJ_VAL(obj, 7))) > 0) {
        strlcat(buf, obj_proto[real_obj].text.name, sizeof(buf));
        if (GET_OBJ_VAL(obj, 8) > 0 && GET_OBJ_VAL(obj, 9) > 0)
          strlcat(buf, ", ", sizeof(buf));
        else if ((GET_OBJ_VAL(obj, 8) > 0 || GET_OBJ_VAL(obj, 9) > 0))
          strlcat(buf, " and ", sizeof(buf));

      }
      if (GET_OBJ_VAL(obj, 8) > 0 && (real_obj = real_object(GET_OBJ_VAL(obj, 8))) > 0) {
        strlcat(buf, obj_proto[real_obj].text.name, sizeof(buf));
        if (GET_OBJ_VAL(obj, 9) > 0)
          strlcat(buf, " and ", sizeof(buf));
      }
      if (GET_OBJ_VAL(obj, 9) > 0 && (real_obj = real_object(GET_OBJ_VAL(obj, 9))) > 9) {
        strlcat(buf, obj_proto[real_obj].text.name, sizeof(buf));
      }
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), ". It can hold a maximum of %d rounds.", GET_OBJ_VAL(obj, 5));
    } else {
      // Map damage value to phrase.
      if (GET_WEAPON_DAMAGE_CODE(obj) == LIGHT) {
        strlcat(buf, " a lightly-damaging", sizeof(buf));
      } else if (GET_WEAPON_DAMAGE_CODE(obj) == MODERATE) {
        strlcat(buf, " a moderately-damaging", sizeof(buf));
      } else if (GET_WEAPON_DAMAGE_CODE(obj) == SERIOUS) {
        strlcat(buf, " a strong", sizeof(buf));
      } else if (GET_WEAPON_DAMAGE_CODE(obj) == DEADLY) {
        strlcat(buf, " a deadly", sizeof(buf));
      } else {
        strlcat(buf, " an indeterminate-strength", sizeof(buf));
        snprintf(buf1, sizeof(buf1), "SYSERR: Unable to map damage value %d for weapon '%s' (%ld) to a damage phrase.",
                GET_WEAPON_DAMAGE_CODE(obj), GET_OBJ_NAME(obj), GET_OBJ_VNUM(obj));
        mudlog(buf1, NULL, LOG_SYSLOG, TRUE);
      }
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %s", weapon_types[GET_OBJ_VAL(obj, 3)]);

      // Two-handed weapon?
      if (IS_OBJ_STAT(obj, ITEM_EXTRA_TWOHANDS))
        strlcat(buf, " that requires two hands to wield correctly.", sizeof(buf));
      else
        strlcat(buf, ".", sizeof(buf));

      // Reach?
      if (GET_WEAPON_REACH(obj)) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " As a long weapon, it gives you %d meter%s of extended reach.",
                GET_WEAPON_REACH(obj), GET_WEAPON_REACH(obj) > 1 ? "s" : "");
      }

      if (GET_WEAPON_FOCUS_RATING(obj) > 0) {
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " It is a weapon focus of force %d.", GET_WEAPON_FOCUS_RATING(obj));
      }

      // Map strength bonus to phrase.
      if (GET_WEAPON_STR_BONUS(obj) != 0) {
        if (GET_WEAPON_STR_BONUS(obj) == 1) {
          strlcat(buf, " It is somewhat well-constructed and will let you hit a little harder in combat.", sizeof(buf));
        } else if (GET_WEAPON_STR_BONUS(obj) == 2) {
          strlcat(buf, " It is well-constructed, letting you land strong hits.", sizeof(buf));
        } else if (GET_WEAPON_STR_BONUS(obj) == 3) {
          strlcat(buf, " It is extremely well-constructed, letting you hit with great strength.", sizeof(buf));
        } else if (GET_WEAPON_STR_BONUS(obj) == 4) {
          strlcat(buf, " It is masterfully constructed, letting you hit as hard as possible.", sizeof(buf));
        } else {
          strlcat(buf, " It has an indeterminate strength modifier.", sizeof(buf));
          snprintf(buf1, sizeof(buf1), "SYSERR: Unable to map strength modifier %d for weapon '%s' (%ld) to a feature phrase.",
                  GET_WEAPON_STR_BONUS(obj), GET_OBJ_NAME(obj), GET_OBJ_VNUM(obj));
          mudlog(buf1, NULL, LOG_SYSLOG, TRUE);
        }
      }
    }
    break;
  case ITEM_WORN:
    num = (GET_OBJ_VAL(obj, 5) > 20 ? (int)(GET_OBJ_VAL(obj, 5) / 100) : GET_OBJ_VAL(obj, 5)) +
          (GET_OBJ_VAL(obj, 6) > 20 ? (int)(GET_OBJ_VAL(obj, 6) / 100) : GET_OBJ_VAL(obj, 6));
    if (num < 1)
      strlcat(buf, " a piece of clothing", sizeof(buf));
    else if (num < 4)
      strlcat(buf, " a lightly armored piece of clothing", sizeof(buf));
    else if (num < 7)
      strlcat(buf, " a piece of light armor", sizeof(buf));
    else if (num < 10)
      strlcat(buf, " a moderately rated piece of armor", sizeof(buf));
    else
      strlcat(buf, " a piece of heavy armor", sizeof(buf));
    if (GET_OBJ_VAL(obj, 1) > 5)
      strlcat(buf, " designed for carrying ammunition", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 4) > 3 && GET_OBJ_VAL(obj, 4) < 6)
      strlcat(buf, " that can carry a bit of gear", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 4) >= 6)
      strlcat(buf, " that can carry a lot of gear", sizeof(buf));
    if (GET_OBJ_VAL(obj, 7) < -2)
      strlcat(buf, ". It is also very bulky.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 7) < 1)
      strlcat(buf, ". It is easier to see under clothing.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 7) < 4)
      strlcat(buf, ". It is quite concealable.", sizeof(buf));
    else
      strlcat(buf, ". It's almost invisible under clothing.", sizeof(buf));
    break;
  case ITEM_PROGRAM:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " a rating %d %s program, that is %dMp in size.", GET_OBJ_VAL(obj, 1),
            programs[GET_OBJ_VAL(obj, 0)].name, GET_OBJ_VAL(obj, 2));
    break;
  case ITEM_DRUG:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %d dose%s of the drug %s.",
             GET_OBJ_DRUG_DOSES(obj),
             GET_OBJ_DRUG_DOSES(obj) != 1 ? "s" : "",
             drug_types[GET_OBJ_DRUG_TYPE(obj)].name);
    break;
  case ITEM_CYBERDECK:
    if (GET_OBJ_VAL(obj, 0) < 4)
      strlcat(buf, " a beginners cyberdeck", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 9)
      strlcat(buf, " a cyberdeck of moderate ability", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 12)
      strlcat(buf, " a top of the range cyberdeck", sizeof(buf));
    else
      strlcat(buf, " one of the best cyberdecks you'll ever see", sizeof(buf));
    if (GET_OBJ_VAL(obj, 2) + GET_OBJ_VAL(obj, 3) < 600)
      strlcat(buf, " with a modest amount of memory.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 2) + GET_OBJ_VAL(obj, 3) < 1400)
      strlcat(buf, " containing a satisfactory amount of memory.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 2) + GET_OBJ_VAL(obj, 3) < 3000)
      strlcat(buf, " featuring a fair amount of memory.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 2) + GET_OBJ_VAL(obj, 3) < 5000)
      strlcat(buf, " with oodles of memory.", sizeof(buf));
    else
      strlcat(buf, " with more memory than you could shake a datajack at.", sizeof(buf));
    if (GET_OBJ_VAL(obj, 1) < 2)
      strlcat(buf, " You better hope you don't run into anything nasty while using it", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 1) < 5)
      strlcat(buf, " It offers adequate protection from feedback", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 1) < 9)
      strlcat(buf, " Nothing will phase you", sizeof(buf));
    else
      strlcat(buf, " It could protect you from anything", sizeof(buf));
    if (GET_OBJ_VAL(obj, 4) < 100)
      strlcat(buf, " but you're out of luck if you want to transfer anything.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 4) < 200)
      strlcat(buf, " and it transfers slowly, but will get the job done.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 4) < 300)
      strlcat(buf, " and on the plus side it's IO is excellent.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 4) < 500)
      strlcat(buf, " also the IO is second to none.", sizeof(buf));
    else
      strlcat(buf, " and it can upload faster than light.", sizeof(buf));
    break;
  case ITEM_FOOD:
    if (GET_OBJ_VAL(obj, 0) < 2)
      strlcat(buf, " a small", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 5)
      strlcat(buf, " a average", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 10)
      strlcat(buf, " a large", sizeof(buf));
    else
      strlcat(buf, " a huge", sizeof(buf));
    strlcat(buf, " portion of food.", sizeof(buf));
    break;
  case ITEM_DOCWAGON:
    strlcat(buf, " a DocWagon contract, it will call them out when your vital signs drop.", sizeof(buf));
    break;
  case ITEM_CONTAINER:
    if (GET_OBJ_VAL(obj, 0) < 5)
      strlcat(buf, " tiny", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 15)
      strlcat(buf, " small", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 30)
      strlcat(buf, " large", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) < 60)
      strlcat(buf, " huge", sizeof(buf));
    else
      strlcat(buf, " gigantic", sizeof(buf));

    if (obj->obj_flags.wear_flags.AreAnySet(ITEM_WEAR_BACK, ITEM_WEAR_ABOUT, ENDBIT))
      strlcat(buf, " backpack.", sizeof(buf));
    else
      strlcat(buf, " container.", sizeof(buf));
    break;
  case ITEM_DECK_ACCESSORY:
    if (GET_OBJ_VAL(obj, 0) == TYPE_COOKER) {
      strlcat(buf, " a", sizeof(buf));
      if (GET_DECK_ACCESSORY_COOKER_RATING(obj) <= 2)
        strlcat(buf, " sluggish", sizeof(buf));
      else if (GET_DECK_ACCESSORY_COOKER_RATING(obj) <= 4)
        strlcat(buf, " average", sizeof(buf));
      else if (GET_DECK_ACCESSORY_COOKER_RATING(obj) <= 6)
        strlcat(buf, " quick", sizeof(buf));
      else if (GET_DECK_ACCESSORY_COOKER_RATING(obj) <= 8)
        strlcat(buf, " speedy", sizeof(buf));
      else
        strlcat(buf, " lightning fast", sizeof(buf));
      strlcat(buf, " optical chip encoder.", sizeof(buf));
    } else if (GET_OBJ_VAL(obj, 0) == TYPE_COMPUTER) {
      strlcat(buf, " a personal computer. It has ", sizeof(buf));
      if (GET_OBJ_VAL(obj, 1) < 150)
        strlcat(buf, " a small memory capacity.", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 1) < 500)
        strlcat(buf, " a moderate amount of memory.", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 1) < 1500)
        strlcat(buf, " a large amount of memory.", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 1) < 3000)
        strlcat(buf, " more than enough memory for most people.", sizeof(buf));
      else
        strlcat(buf, " an almost unimaginable amount of memory.", sizeof(buf));
    } else if (GET_OBJ_VAL(obj, 0) == TYPE_PARTS)
      strlcat(buf, " used in the construction of cyberdeck components.", sizeof(buf));
    else if (GET_OBJ_VAL(obj, 0) == TYPE_UPGRADE && GET_OBJ_VAL(obj, 1) == 3)
      strlcat(buf, " used to allow other people to surf along side you.", sizeof(buf));
    break;
  case ITEM_SPELL_FORMULA:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " a rating %d spell formula, describing %s. It is designed for use by a %s mage.", GET_OBJ_VAL(obj, 0),
            spells[GET_OBJ_VAL(obj, 1)].name, GET_OBJ_VAL(obj, 2) == 1 ? "shamanic" : "hermetic");
    break;
  case ITEM_GUN_ACCESSORY:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " a firearm accessory that attaches to the %s of a weapon.", (GET_OBJ_VAL(obj, 0) == 0 ? "top" :
                                                                                     (GET_OBJ_VAL(obj, 0) == 1 ? "barrel" : "bottom")));
    break;
  case ITEM_GUN_AMMO:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " a box of ammunition for reloading %s magazines. It contains %d rounds of %s ammo.",
            weapon_types[GET_AMMOBOX_WEAPON(obj)],
            GET_AMMOBOX_QUANTITY(obj),
            ammo_type[GET_AMMOBOX_TYPE(obj)].name);
    break;
  case ITEM_FOCUS:
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " a rating %d %s focus.", GET_OBJ_VAL(obj, 1), foci_type[GET_OBJ_VAL(obj, 0)]);
    break;
  case ITEM_MAGIC_TOOL:
    if (GET_OBJ_VAL(obj, 0) == TYPE_LIBRARY_CONJURE || GET_OBJ_VAL(obj, 0) == TYPE_LIBRARY_SPELL) {
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "  is a rating %d ", GET_OBJ_VAL(obj, 1));
      if (GET_OBJ_VAL(obj, 0) == TYPE_LIBRARY_CONJURE)
        strlcat(buf, "conjuring", sizeof(buf));
      else if (GET_OBJ_VAL(obj, 0) == TYPE_LIBRARY_SPELL)
        strlcat(buf, "sorcery", sizeof(buf));
      strlcat(buf, " library.", sizeof(buf));
    }
    break;
  case ITEM_MOD:
    strlcat(buf, " a vehicle modification for the ", sizeof(buf));
    if (GET_VEHICLE_MOD_LOCATION(obj) >= MOD_INTAKE_FRONT && GET_VEHICLE_MOD_LOCATION(obj) <= MOD_INTAKE_REAR)
      strlcat(buf, "intake", sizeof(buf));
    else if (GET_VEHICLE_MOD_LOCATION(obj) >= MOD_BODY_FRONT && GET_VEHICLE_MOD_LOCATION(obj) <= MOD_BODY_WINDOWS)
      strlcat(buf, "body", sizeof(buf));
    else if (GET_VEHICLE_MOD_LOCATION(obj) >= MOD_COMPUTER1 && GET_VEHICLE_MOD_LOCATION(obj) <= MOD_COMPUTER3)
      strlcat(buf, "computer", sizeof(buf));
    else strlcat(buf, mod_name[GET_VEHICLE_MOD_LOCATION(obj)], sizeof(buf));
    strlcat(buf, ". It is for ", sizeof(buf));
    for (int q = 1; q < NUM_ENGINE_TYPES; q++)
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << q))
        num++;
    if (num) {
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_ELECTRIC)) {
        strlcat(buf, "electric", sizeof(buf));
        num2++;
        num--;
      }
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_FUELCELL)) {
        if (num2) {
          if (num > 1)
            strlcat(buf, ", ", sizeof(buf));
          else strlcat(buf, " and ", sizeof(buf));
        }
        strlcat(buf, "fuel cell", sizeof(buf));
        num2++;
        num--;
      }
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_GASOLINE)) {
        if (num2) {
          if (num > 1)
            strlcat(buf, ", ", sizeof(buf));
          else strlcat(buf, " and ", sizeof(buf));
        }
        strlcat(buf, "gasoline", sizeof(buf));
        num2++;
        num--;
      }
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_METHANE)) {
        if (num2) {
          if (num > 1)
            strlcat(buf, ", ", sizeof(buf));
          else strlcat(buf, " and ", sizeof(buf));
        }
        strlcat(buf, "methane", sizeof(buf));
        num2++;
        num--;
      }
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_DIESEL)) {
        if (num2) {
          if (num > 1)
            strlcat(buf, ", ", sizeof(buf));
          else strlcat(buf, " and ", sizeof(buf));
        }
        strlcat(buf, "diesel", sizeof(buf));
        num2++;
        num--;
      }
      if (IS_SET(GET_VEHICLE_MOD_ENGINE_BITS(obj), 1 << ENGINE_JET)) {
        if (num2) {
          if (num > 1)
            strlcat(buf, ", ", sizeof(buf));
          else strlcat(buf, " and ", sizeof(buf));
        }
        strlcat(buf, "jet", sizeof(buf));
        num2++;
        num--;
      }
    } else strlcat(buf, "all", sizeof(buf));
    strlcat(buf, " engines.", sizeof(buf));
    break;
  default:
    strlcat(buf, " for sale.", sizeof(buf));
  }
  strlcat(buf, " It weighs about ", sizeof(buf));
  if (GET_OBJ_WEIGHT(obj) < 1) {
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%d grams", (int)(GET_OBJ_WEIGHT(obj) * 1000));
  } else snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%.0f kilogram%s", GET_OBJ_WEIGHT(obj), (GET_OBJ_WEIGHT(obj) >= 2 ? "s" : ""));
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " and I couldn't let it go for less than %d nuyen.", buy_price(obj, shop_nr, GET_MOB_FACTION_IDNUM(keeper), ch));

  if (IS_OBJ_STAT(obj, ITEM_EXTRA_NERPS)) {
    strlcat(buf, " ^Y(OOC: It has no special coded effects.)^n", sizeof(buf));
  }

  do_say(keeper, buf, cmd_say, SCMD_SAYTO);
  send_to_char(ch, "\r\n%s\r\n", obj->text.look_desc);

  if (GET_OBJ_TYPE(obj) == ITEM_WEAPON && WEAPON_IS_GUN(obj)) {
    for (int i = ACCESS_LOCATION_TOP; i <= ACCESS_LOCATION_UNDER; ++i) {
      if (GET_OBJ_VAL(obj, i) >= 0) {
        send_to_char(ch, "There is %s attached to the %s of it.\r\n",
                     GET_OBJ_VAL(obj, i) > 0 ? short_object(GET_OBJ_VAL(obj, i), 2) : "nothing",
                     gun_accessory_locations[i - ACCESS_LOCATION_TOP]);
      }
    }
  }
}

void shop_check(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  char formatstr[MAX_STRING_LENGTH];
  char paddingnumberstr[12];

  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  int i = 0;
  snprintf(buf, sizeof(buf), "You have the following on order: \r\n");
  for (struct shop_order_data *order = shop_table[shop_nr].order; order; order = order->next)
    if (order->player == GET_IDNUM(ch))
    {
      i++;
      float totaltime = order->timeavail - time(0);
      totaltime = totaltime / SECS_PER_MUD_DAY;
      int real_obj = real_object(order->item);
      if (real_obj >= 0) {
        //Format string: " %d) %-30s (%d) - "
        //We apply padding for color codes here.
        snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 30 + count_color_codes_in_string(GET_OBJ_NAME(&obj_proto[real_obj])));
        snprintf(formatstr, sizeof(formatstr), "%s%s%s", "%d) %-", paddingnumberstr, "s (%d) - ");
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, i, GET_OBJ_NAME(&obj_proto[real_obj]), order->number);
      }
      else
        strlcat(buf, " ERROR\r\n", sizeof(buf));
      if (totaltime < 0) {
        time_t time_left = order->expiration - time(0);
        int days = time_left / (60 * 60 * 24);
        time_left -= 60 * 60 * 24 * days;

        int hours = time_left / (60 * 60);
        time_left -= 60 * 60 * hours;

        int minutes = time_left / 60;

        if (days > 0)
          snprintf(buf3, sizeof(buf3), "in %d IRL day%s, %d hour%s", days, days != 1 ? "s" : "", hours, hours != 1 ? "s" : "");
        else if (hours > 0)
          snprintf(buf3, sizeof(buf3), "in %d IRL hour%s, %d minute%s", hours, hours != 1 ? "s" : "", minutes, minutes != 1 ? "s" : "");
        else if (minutes > 0)
          snprintf(buf3, sizeof(buf3), "in %d IRL minute%s", minutes, minutes != 1 ? "s" : "");
        else
          strlcpy(buf3, "at any moment", sizeof(buf3));
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " AVAILABLE (%d nuyen / ea; expires %s)\r\n", order->price - order->paid, buf3);
      }
      else if (totaltime < 1 && (int)(24 * totaltime) == 0)
        strlcat(buf, " less than one hour\r\n", sizeof(buf));
      else {
        int days = totaltime;
        int hours = (int)(24 * totaltime);
        snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " %d %s%s\r\n", 
                 days < 1 ? hours : days,
                 days < 1 ? "hour" : "day",
                 days < 1 ? (hours == 1 ? "s" : "") : (days == 1 ? "s" : ""));
      }
    }
  if (i == 0)
  {
    if (MOB_FLAGGED(keeper, MOB_INANIMATE)) {
      snprintf(buf, sizeof(buf), "displays, \"NO ORDERS FOUND.\"");
      do_new_echo(keeper, buf, cmd_echo, 0);
    } else {
      snprintf(buf, sizeof(buf), "%s You don't have anything on order here.", GET_CHAR_NAME(ch));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    }
  } else
    send_to_char(buf, ch);
}


void shop_rec(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  char buf[MAX_STRING_LENGTH];
  int number = atoi(arg);
	if (number <= 0) {
		send_to_char("Unrecognized selection. Syntax: RECEIVE [number].\r\n", ch);
		return;
	}
  for (struct shop_order_data *order = shop_table[shop_nr].order; order; order = order->next) {
    if (order->player == GET_IDNUM(ch) && order->timeavail < time(0) && !--number)
    {
      struct obj_data *obj = read_object(order->item, VIRTUAL, OBJ_LOAD_REASON_SHOP_RECEIVE), *cred = get_first_credstick(ch, "credstick");
      if (!cred && shop_table[shop_nr].type == SHOP_LEGAL) {
        if (access_level(ch, LVL_ADMIN)) {
          send_to_char(ch, "You stare unblinkingly at %s until %s makes an exception to the no-credstick, no-sale policy.\r\n",
                       GET_NAME(keeper), HSSH(keeper));
        } else {
          snprintf(buf, sizeof(buf), "%s No Credstick, No Sale.", GET_CHAR_NAME(ch));
          do_say(keeper, buf, cmd_say, SCMD_SAYTO);
          send_to_char("You need to have an activated credstick in your inventory to shop here.\r\n", ch);
          extract_obj(obj);
          return;
        }
      }
      shop_receive(ch, keeper, arg, order->number, cred && shop_table[shop_nr].type != SHOP_BLACK? 0 : 1, NULL, obj,
                       shop_table[shop_nr].type == SHOP_BLACK ? NULL : cred, order->price - order->paid, shop_nr, order);
      return;
    }
  }
  snprintf(buf, sizeof(buf), "%s I don't have anything for you.", GET_CHAR_NAME(ch));
  do_say(keeper, buf, cmd_say, SCMD_SAYTO);
}


void shop_cancel(char *arg, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  if (!is_open(keeper, shop_nr))
    return;
  if (!is_ok_char(keeper, ch, shop_nr))
    return;
  int number;
  strlcpy(buf, GET_CHAR_NAME(ch), sizeof(buf));
  if (!(number = atoi(arg)))
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " What order do you want to cancel?");
  else
  {
    for (struct shop_order_data *order = shop_table[shop_nr].order; order; order = order->next)
      if (order->player == GET_IDNUM(ch) && !--number) {
        struct shop_order_data *temp;
        int real_obj = real_object(order->item);
        if (real_obj >= 0)
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " I'll let my contacts know you no longer want %s.", GET_OBJ_NAME(&obj_proto[real_obj]));
        else
          snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " I'll let my contacts know you no longer want that.");

        // Refund the prepayment, minus the usual fee.
        if (order->paid > 0) {
          int total_prepayment = order->paid * order->number;
          int repayment_amount = total_prepayment - (total_prepayment / PREORDER_RESTOCKING_FEE_DIVISOR);
          if (repayment_amount > 0) {
            // In this instance, we do a raw refund, then decrement the shop purchase amount.
            GET_NUYEN_RAW(ch) += repayment_amount;
            GET_NUYEN_INCOME_THIS_PLAY_SESSION(ch, NUYEN_OUTFLOW_SHOP_PURCHASES) -= repayment_amount;
            act("$n hands $N some nuyen.", FALSE, keeper, 0, ch, TO_ROOM);
          }
        }

        REMOVE_FROM_LIST(order, shop_table[shop_nr].order, next);
        delete order;
        do_say(keeper, buf, cmd_say, SCMD_SAYTO);
        return;
      }
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " You don't have that many orders with me.");
  }
  do_say(keeper, buf, cmd_say, SCMD_SAYTO);

}

void shop_hours(struct char_data *ch, vnum_t shop_nr)
{
#ifdef USE_SHOP_OPEN_CLOSE_TIMES
  char buf[MAX_STRING_LENGTH];
  strlcpy(buf, "This shop is ", sizeof(buf));
  if (!shop_table[shop_nr].open && shop_table[shop_nr].close == 24)
    strlcat(buf, "always open", sizeof(buf));
  else {
    strlcat(buf, "open from ", sizeof(buf));
    if (shop_table[shop_nr].open < 12)
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%dam", shop_table[shop_nr].open);
    else if (shop_table[shop_nr].open == 12)
      strlcat(buf, "noon", sizeof(buf));
    else if (shop_table[shop_nr].open == 24)
      strlcat(buf, "midnight", sizeof(buf));
    else
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%dpm", shop_table[shop_nr].open - 12);
    strlcat(buf, " until ", sizeof(buf));
    if (shop_table[shop_nr].close < 12)
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%dam", shop_table[shop_nr].close);
    else if (shop_table[shop_nr].close == 12)
      strlcat(buf, "noon", sizeof(buf));
    else if (shop_table[shop_nr].close == 24)
      strlcat(buf, "midnight", sizeof(buf));
    else
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "%dpm", shop_table[shop_nr].close - 12);
  }
  strlcat(buf, ".\r\n", sizeof(buf));
  send_to_char(buf, ch);
#else
  send_to_char("The shop-hours system is disabled, so shops are always open.\r\n", ch);
#endif
}

SPECIAL(shop_keeper)
{
  struct char_data *keeper = (struct char_data *) me;
  vnum_t shop_nr;

  if (!cmd)
    return FALSE;
  for (shop_nr = 0; shop_nr <= top_of_shopt; shop_nr++)
    if (shop_table[shop_nr].keeper == GET_MOB_VNUM(keeper))
      break;
  if (shop_nr > top_of_shopt)
    return FALSE;

  bool cmd_is_buy = CMD_IS("buy");
  bool cmd_is_sell = CMD_IS("sell");
  bool cmd_is_list = CMD_IS("list");
  bool cmd_is_info = CMD_IS("info");
  bool cmd_is_value = CMD_IS("value");
  bool cmd_is_check = CMD_IS("check");
  bool cmd_is_receive = CMD_IS("receive") || CMD_IS("recieve");
  bool cmd_is_hours = CMD_IS("hours");
  bool cmd_is_cancel = CMD_IS("cancel");
  bool cmd_is_probe = CMD_IS("probe");
  bool cmd_is_install = CMD_IS("install");
  bool cmd_is_uninstall = CMD_IS("uninstall");

  if (!(cmd_is_buy || cmd_is_sell || cmd_is_list || cmd_is_info || cmd_is_value || cmd_is_check || cmd_is_receive || cmd_is_hours || cmd_is_cancel || cmd_is_probe || cmd_is_install || cmd_is_uninstall))
    return FALSE;

  if (GET_MOB_FACTION_IDNUM(keeper) && !faction_shop_will_deal_with_player(GET_MOB_FACTION_IDNUM(keeper), ch)) {
    send_to_char(ch, "%s is loyal and won't deal with you until you improve your reputation with %s.\r\n",
                 CAP(GET_CHAR_NAME(keeper)),
                 decapitalize_a_an(get_faction_name(GET_MOB_FACTION_IDNUM(keeper), ch)));
    return TRUE;
  }

  skip_spaces(&argument);

  if (cmd_is_buy)
    shop_buy(argument, strlen(argument), ch, keeper, shop_nr);
  else if (cmd_is_sell)
    shop_sell(argument, ch, keeper, shop_nr);
  else if (cmd_is_list)
    shop_list(argument, ch, keeper, shop_nr);
  else if (cmd_is_info)
    shop_info(argument, ch, keeper, shop_nr);
  else if (cmd_is_value)
    shop_value(argument, ch, keeper, shop_nr);
  else if (cmd_is_check)
    shop_check(argument, ch, keeper, shop_nr);
  else if (cmd_is_receive)
    shop_rec(argument, ch, keeper, shop_nr);
  else if (cmd_is_hours)
    shop_hours(ch, shop_nr);
  else if (cmd_is_cancel)
    shop_cancel(argument, ch, keeper, shop_nr);
  else if (cmd_is_probe)
    return shop_probe(argument, ch, keeper, shop_nr);
  else if (cmd_is_install && shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR))
    shop_install(argument, ch, keeper, shop_nr);
  else if (cmd_is_uninstall && shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR))
    shop_uninstall(argument, ch, keeper, shop_nr);
  else
    return FALSE;
  return TRUE;
}

void assign_shopkeepers(void)
{
  int index, rnum;
  cmd_say = find_command("say");
  cmd_echo = find_command("echo");
  for (index = 0; index <= top_of_shopt; index++) {
    if (shop_table[index].keeper <= 0)
      continue;
    if ((rnum = real_mobile(shop_table[index].keeper)) < 0)
      log_vfprintf("Shopkeeper #%ld does not exist (shop #%ld)",
          shop_table[index].keeper, shop_table[index].vnum);
    else if (mob_index[rnum].func != shop_keeper && shop_table[index].keeper != 1151) {
      mob_index[rnum].sfunc = mob_index[rnum].func;
      mob_index[rnum].func = shop_keeper;
    }
  }
}

void randomize_shop_prices(void)
{
  PERF_PROF_SCOPE(pr_, __func__);
  for (int i = 0; i <= top_of_shopt; i++) {
    if (shop_table[i].random_amount)
      shop_table[i].random_current = number(-shop_table[i].random_amount, shop_table[i].random_amount);
    else
      shop_table[i].random_current = 0;
    for (struct shop_sell_data *sell = shop_table[i].selling; sell; sell = sell->next)
      for (int q = 0; q < SHOP_LAST_IDNUM_LIST_SIZE; q++)
        sell->lastidnum[q] = 0;
  }
}

void list_detailed_shop(struct char_data *ch, vnum_t shop_nr)
{
  char buf[MAX_STRING_LENGTH];
  char formatstr[MAX_STRING_LENGTH];
  char paddingnumberstr[12];

  snprintf(buf, sizeof(buf), "Vnum:       [%5ld], Rnum: [%5ld]\r\n", shop_table[shop_nr].vnum, shop_nr);

  int real_mob = real_mobile(shop_table[shop_nr].keeper);
  if (real_mob > 0) {
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Name: %30s Shopkeeper: %s [%5ld]\r\n", shop_table[shop_nr].shopname,
             mob_proto[real_mob].player.physical_text.name,
             shop_table[shop_nr].keeper);
  } else {
    snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Name: %30s Shopkeeper: (N/A) [%5ld]\r\n", shop_table[shop_nr].shopname, shop_table[shop_nr].keeper);
  }

  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Buy at:     [%1.2f], Sell at: [%1.2f], +/- %%: [%d], Current %%: [%d]",
          shop_table[shop_nr].profit_buy, shop_table[shop_nr].profit_sell, shop_table[shop_nr].random_amount,
          shop_table[shop_nr].random_current);
#ifdef USE_SHOP_OPEN_CLOSE_TIMES
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), ", Hours [%d-%d]\r\n", shop_table[shop_nr].open, shop_table[shop_nr].close);
#else
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), ", Hours (disabled) [%d-%d]\r\n", shop_table[shop_nr].open, shop_table[shop_nr].close);
#endif
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Type:       %s, Etiquette: %s\r\n", shop_type[shop_table[shop_nr].type], skills[shop_table[shop_nr].etiquette].name);
  shop_table[shop_nr].races.PrintBits(buf2, MAX_STRING_LENGTH, pc_race_types, NUM_RACES);
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "!Serves:     %s\r\n", buf2);
  shop_table[shop_nr].flags.PrintBits(buf2, MAX_STRING_LENGTH, shop_flags, SHOP_FLAGS);
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Flags:      %s\r\n", buf2);
  shop_table[shop_nr].buytypes.PrintBits(buf2, MAX_STRING_LENGTH, item_types, NUM_ITEMS);
  snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), "Buytypes:   %s\r\n", buf2);
  strlcat(buf, "Selling: \r\n", sizeof(buf));
  for (struct shop_sell_data *selling = shop_table[shop_nr].selling; selling; selling = selling->next) {
    int real_obj = real_object(selling->vnum);
    if (real_obj) {
      //Format string: "%-50s (%5ld) Type: %s Amount: %d\r\n"
      //We apply padding for color codes here.
      snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 50 + count_color_codes_in_string(obj_proto[real_obj].text.name));
      snprintf(formatstr, sizeof(formatstr), "%s%s%s", "%-", paddingnumberstr, "s (%5ld) Type: %s Amount: %d\r\n");
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), formatstr, obj_proto[real_obj].text.name,
              selling->vnum, selling_type[selling->type], selling->stock);
    }
  }
  page_string(ch->desc, buf, 0);
}

void write_shops_to_disk(int zone)
{
  long counter, realcounter;
  FILE *fp;
  struct shop_data *shop;
  zone = real_zone(zone);
  snprintf(buf, sizeof(buf), "%s/%d.shp", SHP_PREFIX, zone_table[zone].number);
  fp = fopen(buf, "w+");

  /* start running through all mobiles in this zone */
  for (counter = zone_table[zone].number * 100;
       counter <= zone_table[zone].top;
       counter++) {
    realcounter = real_shop(counter);

    if (realcounter >= 0) {
      shop = shop_table+realcounter;
      fprintf(fp, "#%ld\n", shop->vnum);
      fprintf(fp, "Keeper:\t%ld\n"
              "ProfitBuy:\t%.2f\n"
              "ProfitSell:\t%.2f\n"
              "Random:\t%d\n"
#ifdef USE_SHOP_OPEN_CLOSE_TIMES
              "Open:\t%d\n"
              "Close:\t%d\n"
#else
              "Open (disabled):\t%d\n"
              "Close (disabled):\t%d\n"
#endif
              "Type:\t%s\n",
              shop->keeper, shop->profit_buy, shop->profit_sell, shop->random_amount, shop->open,
              shop->close, shop_type[shop->type]);
      fprintf(fp, "NoSuchItemKeeper:\t%s\n"
              "NoSuchItemPlayer:\t%s\n"
              "NoNuyen:\t%s\n"
              "DoesntBuy:\t%s\n"
              "Buy:\t%s\n"
              "Sell:\t%s\n"
              "Name:\t%s\n",
              shop->no_such_itemk, shop->no_such_itemp, shop->not_enough_nuyen, shop->doesnt_buy, shop->buy, shop->sell, shop->shopname);
      fprintf(fp, "Flags:\t%s\n"
              "Races:\t%s\n"
              "Buytypes:\t%s\n"
              "Etiquette:\t%d\n",
              shop->flags.ToString(), shop->races.ToString(), shop->buytypes.ToString(), shop->etiquette);
      fprintf(fp, "[SELLING]\n");
      int i = 0;
      for (struct shop_sell_data *sell = shop->selling; sell; sell = sell->next, i++) {
        fprintf(fp, "\t[SELL %d]\n"
                "\t\tVnum:\t%ld\n"
                "\t\tType:\t%s\n"
                "\t\tStock:\t%d\n",
                i, sell->vnum, selling_type[sell->type], sell->stock);
      }
      fprintf(fp, "BREAK\n");
    }
  }
  fprintf(fp, "END\n");
  fclose(fp);
  write_index_file("shp");
}

#define SHOP d->edit_shop
#define CH d->character
extern void free_shop(struct shop_data *shop);

void shedit_disp_race_menu(struct descriptor_data *d)
{
  CLS(CH);
  send_to_char("1) Human\r\n2) Dwarf\r\n3) Elf\r\n4) Ork\r\n5) Troll\r\n", CH);
  SHOP->races.PrintBits(buf, MAX_STRING_LENGTH, pc_race_types, NUM_RACES);
  send_to_char(CH, "Won't sell to: ^c%s^n\r\nEnter Race (0 to quit): ", buf);
  d->edit_mode = SHEDIT_RACE_MENU;
}

void shedit_disp_flag_menu(struct descriptor_data *d)
{
  CLS(CH);
  for (int i = 1; i < SHOP_FLAGS; i += 2)
  {
    send_to_char(CH, "%2d) %-20s %2d) %-20s\r\n",
                 i, shop_flags[i],
                 i + 1, i + 1 <= SHOP_FLAGS ?
                 shop_flags[i + 1] : "");
  }
  SHOP->flags.PrintBits(buf, MAX_STRING_LENGTH, shop_flags, SHOP_FLAGS);
  send_to_char(CH, "Flags: ^c%s^n\r\nEnter Flag (0 to quit): ", buf);
  d->edit_mode = SHEDIT_FLAG_MENU;
}

void shedit_disp_buytypes_menu(struct descriptor_data *d)
{
  CLS(CH);
  for (int counter = 1; counter < NUM_ITEMS; counter++)
  {
    send_to_char(CH, "%s%2d) %-20s%s",
                 counter % 2 == 0 ? " " : "",
                 counter,
                 item_types[counter],
                 (counter % 2 == 0 || counter == NUM_ITEMS - 1) ? "\r\n" : ""
                );
  }
  SHOP->buytypes.PrintBits(buf, MAX_STRING_LENGTH, item_types, NUM_ITEMS);
  send_to_char(CH, "Will Buy: ^c%s^n\r\nSelect Buytypes (0 to quit): ", buf);
  d->edit_mode = SHEDIT_BUYTYPES_MENU;
}

void shedit_disp_text_menu(struct descriptor_data *d)
{
  CLS(CH);
  send_to_char(CH, "1) Shop Name: ^c%s^n\r\n", SHOP->shopname);
  send_to_char(CH, "2) No Such Item (Keeper): ^c%s^n\r\n", SHOP->no_such_itemk);
  send_to_char(CH, "3) No Such Item (Player): ^c%s^n\r\n", SHOP->no_such_itemp);
  send_to_char(CH, "4) Not Enough Nuyen: ^c%s^n\r\n", SHOP->not_enough_nuyen);
  send_to_char(CH, "5) Doesn't Buy: ^c%s^n\r\n", SHOP->doesnt_buy);
  send_to_char(CH, "6) Buy: ^c%s^n\r\n", SHOP->buy);
  send_to_char(CH, "7) Sell: ^c%s^n\r\n", SHOP->sell);
  send_to_char(    "q) Return to Main Menu\r\nEnter Your Choice: ", CH);
  d->edit_mode = SHEDIT_TEXT_MENU;
}

void shedit_disp_selling_menu(struct descriptor_data *d)
{
  CLS(CH);
  int i = 1;
  char formatstr[MAX_STRING_LENGTH];
  char paddingnumberstr[12];
  for (struct shop_sell_data *sell = SHOP->selling; sell; sell = sell->next, i++)
  {
    int real_obj = real_object(sell->vnum);
    if (real_obj < 0)
      snprintf(buf, sizeof(buf), "%d) INVALID OBJECT, DELETE IT  ", i);
    else {
      //Format string: "%d) ^c%-50s^n (^c%5ld^n) Type: ^c%6s^n"
      //We apply padding for color codes here.
      snprintf(paddingnumberstr, sizeof(paddingnumberstr), "%d", 50 + count_color_codes_in_string(GET_OBJ_NAME(&obj_proto[real_obj])));
      snprintf(formatstr, sizeof(formatstr), "%s%s%s", "%d) ^c%-", paddingnumberstr, "s^n (^c%5ld^n) Type: ^c%6s^n");
      snprintf(buf, sizeof(buf), formatstr, i, GET_OBJ_NAME(&obj_proto[real_obj]),
              sell->vnum, selling_type[sell->type]);
    }
    if (sell->type == SELL_STOCK)
      snprintf(ENDOF(buf), sizeof(buf) - strlen(buf), " Stock: ^c%d^n", sell->stock);
    strlcat(buf, "\r\n", sizeof(buf));
    send_to_char(buf, CH);
  }
  send_to_char("a) Add Entry\r\nd) Delete Entry\r\nEnter Choice (0 to quit)", CH);
  d->edit_mode = SHEDIT_SELLING_MENU;
}

void shedit_disp_menu(struct descriptor_data *d)
{
  int real_mob = real_mobile(SHOP->keeper);
  CLS(CH);
  send_to_char(CH, "Shop Number: %ld\r\n", SHOP->vnum);
  send_to_char(CH, "1) Keeper: ^c%ld^n (^c%s^n)\r\n", SHOP->keeper,
               real_mob > 0 ? GET_NAME(&mob_proto[real_mob]) : "NULL");
  send_to_char(CH, "2) Shop Type: ^c%s^n\r\n", shop_type[SHOP->type]);
  send_to_char(CH, "3) Cost Multiplier when Player Buying: ^c%.2f^n\r\n", SHOP->profit_buy);
  send_to_char(CH, "4) Cost Multiplier when Player Selling: ^c%.2f^n\r\n", SHOP->profit_sell);
  send_to_char(CH, "5) %% +/-: ^c%d^n\r\n", SHOP->random_amount);
#ifdef USE_SHOP_OPEN_CLOSE_TIMES
  send_to_char(CH, "6) Opens: ^c%d^n Closes: ^c%d^n\r\n", SHOP->open, SHOP->close);
#else
  send_to_char(CH, "6) Opens: ^c%d^n Closes: ^c%d^n (Note: system is currently disabled)\r\n", SHOP->open, SHOP->close);
#endif
  send_to_char(CH, "7) Etiquette Used for Availability Rolls: ^c%s^n\r\n", skills[SHOP->etiquette].name);
  SHOP->races.PrintBits(buf, MAX_STRING_LENGTH, pc_race_types, NUM_RACES);
  send_to_char(CH, "8) Doesn't Trade With: ^c%s^n\r\n", buf);
  SHOP->flags.PrintBits(buf, MAX_STRING_LENGTH, shop_flags, SHOP_FLAGS);
  send_to_char(CH, "9) Flags: ^c%s^n\r\n", buf);
  send_to_char("a) Buytype Menu\r\n", CH);
  send_to_char("b) Text Menu\r\n", CH);
  send_to_char("c) Selling Menu\r\n", CH);
  send_to_char("q) Quit and save\r\n", CH);
  send_to_char("x) Exit and abort\r\n", CH);
  send_to_char("Enter your choice:\r\n", CH);
  d->edit_mode = SHEDIT_MAIN_MENU;
}

void shedit_parse(struct descriptor_data *d, const char *arg)
{
  int number = atoi(arg);
  float profit;
  rnum_t shop_nr, i;
  switch(d->edit_mode)
  {
  case SHEDIT_CONFIRM_EDIT:
    switch (*arg) {
    case 'y':
    case 'Y':
      shedit_disp_menu(d);
      break;
    case 'n':
    case 'N':
      STATE(d) = CON_PLAYING;
      free_shop(SHOP);
      delete d->edit_shop;
      d->edit_shop = NULL;
      d->edit_number = 0;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      break;
    default:
      send_to_char("That's not a valid choice!\r\n", CH);
      send_to_char("Do you wish to edit it?\r\n", CH);
      break;
    }
    break;
  case SHEDIT_CONFIRM_SAVESTRING:
    switch(*arg) {
    case 'y':
    case 'Y':
#ifdef ONLY_LOG_BUILD_ACTIONS_ON_CONNECTED_ZONES
      if (!vnum_from_non_approved_zone(d->edit_number)) {
#else
      {
#endif
        snprintf(buf, sizeof(buf),"%s wrote new shop #%ld",
                GET_CHAR_NAME(d->character), d->edit_number);
        mudlog(buf, d->character, LOG_WIZLOG, TRUE);
      }
      shop_nr = real_shop(d->edit_number);
      if (shop_nr > 0) {
        rnum_t okn, nkn;
        if (shop_table[shop_nr].keeper != SHOP->keeper && shop_table[shop_nr].keeper != 1151) {
          okn = real_mobile(shop_table[shop_nr].keeper);
          nkn = real_mobile(SHOP->keeper);
          if (okn > 0 && mob_index[okn].func == shop_keeper) {
            mob_index[okn].func = mob_index[okn].sfunc;
            mob_index[okn].sfunc = NULL;
          } else if (okn > 0 && mob_index[okn].sfunc == shop_keeper)
            mob_index[okn].sfunc = NULL;
          if (nkn > 0) {
            mob_index[nkn].sfunc = mob_index[nkn].func;
            mob_index[nkn].func = shop_keeper;
          }
        }
        SHOP->order = shop_table[shop_nr].order;
        SHOP->vnum = d->edit_number;
        free_shop(shop_table + shop_nr);
        shop_table[shop_nr] = *SHOP;
      } else {
        vnum_t counter, counter2;
        bool found = FALSE;
        for (counter = 0; counter <= top_of_shopt; counter++) {
          if (shop_table[counter].vnum > d->edit_number) {
            for (counter2 = top_of_shopt + 1; counter2 > counter; counter2--)
              shop_table[counter2] = shop_table[counter2 - 1];

            SHOP->vnum = d->edit_number;
            shop_table[counter] = *(d->edit_shop);
            shop_nr = counter;
            found = TRUE;
            break;
          }
        }
        if (!found) {
          SHOP->vnum = d->edit_number;
          shop_table[++top_of_shopt] = *SHOP;
          shop_nr = top_of_shopt;
        }
      }
      i = real_mobile(shop_table[shop_nr].keeper);
      if (i > 0 && shop_table[shop_nr].keeper != 1151) {
        mob_index[i].sfunc = mob_index[i].func;
        mob_index[i].func = shop_keeper;
      }
      write_shops_to_disk(CH->player_specials->saved.zonenum);
      delete d->edit_shop;
      d->edit_shop = NULL;
      d->edit_number = 0;
      STATE(d) = CON_PLAYING;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      send_to_char("Done.\r\n", d->character);
      break;
    case 'n':
    case 'N':
      send_to_char("Shop not saved, aborting.\r\n", d->character);
      STATE(d) = CON_PLAYING;
      free_shop(SHOP);
      delete d->edit_shop;
      d->edit_shop = NULL;
      d->edit_number = 0;
      d->edit_number2 = 0;
      PLR_FLAGS(d->character).RemoveBit(PLR_EDITING);
      break;
    default:
      send_to_char("Invalid choice!\r\n", d->character);
      send_to_char("Do you wish to save this shop internally?\r\n", d->character);
      break;
    }
    break;
  case SHEDIT_MAIN_MENU:
    switch (*arg) {
    case '1':
      send_to_char(CH, "Enter Shopkeeper vnum: ");
      d->edit_mode = SHEDIT_KEEPER;
      break;
    case '2':
      CLS(CH);
      send_to_char(CH, "0) Grey (nuyen and credstick; applies street index)\r\n1) Legal (credstick only; no street index)\r\n2) Black (nuyen only; applies street index)\r\nEnter Shop Type: ");
      d->edit_mode = SHEDIT_TYPE;
      break;
    case '3':
      send_to_char(CH, "Enter multiplier for buy command (1.0 or higher): ");
      d->edit_mode = SHEDIT_PROFIT_BUY;
      break;
    case '4':
      send_to_char(CH, "Enter multiplier for sell command (should be 0.1 unless you have special approval): ");
      d->edit_mode = SHEDIT_PROFIT_SELL;
      break;
    case '5':
      send_to_char(CH, "Enter maximum price deviation: ");
      d->edit_mode = SHEDIT_RANDOM;
      break;
    case '6':
      send_to_char(CH, "Enter opening time: ");
      d->edit_mode = SHEDIT_OPEN;
      break;
    case '7':
      CLS(CH);
      send_to_char(CH, "1) Corporate Etiquette\r\n"
                   "2) Media Etiquette\r\n"
                   "3) Street Etiquette\r\n"
                   "4) Tribal Etiquette\r\n"
                   "5) Elf Etiquette\r\n"
                   "Enter Etiquette skill required for availability tests: ");
      d->edit_mode = SHEDIT_ETTI;
      break;
    case '8':
      shedit_disp_race_menu(d);
      break;
    case '9':
      shedit_disp_flag_menu(d);
      break;
    case 'a':
    case 'A':
      shedit_disp_buytypes_menu(d);
      break;
    case 'b':
    case 'B':
      shedit_disp_text_menu(d);
      break;
    case 'c':
    case 'C':
      shedit_disp_selling_menu(d);
      break;
    case 'q':
    case 'Q':
      d->edit_mode = SHEDIT_CONFIRM_SAVESTRING;
      shedit_parse(d, "y");
      break;
    case 'x':
    case 'X':
      d->edit_mode = SHEDIT_CONFIRM_SAVESTRING;
      shedit_parse(d,"n");
      break;
    }
    break;
  case SHEDIT_ETTI:
    if (number < 1 || number > 5) {
      send_to_char("Invalid Choice! Enter Etiquette skill: ", CH);
      return;
    }
    SHOP->etiquette = --number + SKILL_CORPORATE_ETIQUETTE;
    shedit_disp_menu(d);
    break;
  case SHEDIT_KEEPER:
    SHOP->keeper = number;
    shedit_disp_menu(d);
    break;
  case SHEDIT_TYPE:
    if (number < 0 || number > 2) {
      send_to_char("Invalid Choice! Enter Shop Type: ", CH);
      return;
    }
    SHOP->type = number;
    shedit_disp_menu(d);
    break;
  case SHEDIT_PROFIT_BUY:
    profit = atof(arg);
    if (profit < 1) {
      send_to_char("Buy price multiplier must be at least 1! Enter multiplier: ", CH);
      return;
    }
    SHOP->profit_buy = profit;
    shedit_disp_menu(d);
    break;
  case SHEDIT_PROFIT_SELL:
    profit = atof(arg);
    if (profit > 1 || profit <= 0) {
      send_to_char("Sell price multiplier must be greater than 0 and no more than 1! Enter multiplier: ", CH);
      return;
    }
    SHOP->profit_sell = profit;
    shedit_disp_menu(d);
    break;
  case SHEDIT_RANDOM:
    if (number < 0) {
      send_to_char("Invalid Amount! Enter Maximum Price Deviation: ", CH);
      return;
    }
    SHOP->random_amount = number;
    shedit_disp_menu(d);
    break;
  case SHEDIT_OPEN:
    if (number < 0 || number > 24) {
      send_to_char("Invalid Time! Enter Opening Time (Between 0 and 24): ", CH);
      return;
    }
    SHOP->open = number;
    send_to_char("Enter Closing Time: ", CH);
    d->edit_mode = SHEDIT_OPEN2;
    break;
  case SHEDIT_OPEN2:
    if (number < 0 || number > 24) {
      send_to_char("Invalid Time! Enter Closing Time (Between 0 and 24): ", CH);
      return;
    }
    SHOP->close = number;
    shedit_disp_menu(d);
    break;
  case SHEDIT_RACE_MENU:
    switch (*arg) {
    case '0':
      shedit_disp_menu(d);
      break;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
      number++;
      SHOP->races.ToggleBit(number);
      shedit_disp_race_menu(d);
      break;
    default:
      shedit_disp_race_menu(d);
      break;
    }
    break;
  case SHEDIT_FLAG_MENU:
    if (number == 0) {
      shedit_disp_menu(d);
      return;
    } else if (number > 0 && number < SHOP_FLAGS)
      SHOP->flags.ToggleBit(number);
    shedit_disp_flag_menu(d);
    break;
  case SHEDIT_BUYTYPES_MENU:
    if (number == 0) {
      shedit_disp_menu(d);
      return;
    } else if (number > 0 && number < NUM_ITEMS)
      SHOP->buytypes.ToggleBit(number);
    shedit_disp_buytypes_menu(d);
    break;
  case SHEDIT_TEXT_MENU:
    switch (*arg) {
    case 'q':
    case 'Q':
      shedit_disp_menu(d);
      break;
    case '1':
      send_to_char("Enter name of shop: ", CH);
      d->edit_mode = SHEDIT_SHOPNAME;
      break;
    case '2':
      send_to_char("Enter no such item (Keeper) message: ", CH);
      d->edit_mode = SHEDIT_NSIK;
      break;
    case '3':
      send_to_char("Enter no such item (Player) message: ", CH);
      d->edit_mode = SHEDIT_NSIP;
      break;
    case '4':
      send_to_char("Enter not enough nuyen message: ", CH);
      d->edit_mode = SHEDIT_NEN;
      break;
    case '5':
      send_to_char("Enter doesn't buy message: ", CH);
      d->edit_mode = SHEDIT_NOBUY;
      break;
    case '6':
      send_to_char("Enter buying message (%d for nuyen value): ", CH);
      d->edit_mode = SHEDIT_BUYMSG;
      break;
    case '7':
      send_to_char("Enter selling message: (%d for nuyen value): ", CH);
      d->edit_mode = SHEDIT_SELLMSG;
      break;
    }
    break;
  case SHEDIT_NSIK:
    if (SHOP->no_such_itemk)
      delete [] SHOP->no_such_itemk;
    SHOP->no_such_itemk = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_NSIP:
    if (SHOP->no_such_itemp)
      delete [] SHOP->no_such_itemp;
    SHOP->no_such_itemp = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_NEN:
    if (SHOP->not_enough_nuyen)
      delete [] SHOP->not_enough_nuyen;
    SHOP->not_enough_nuyen = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_NOBUY:
    if (SHOP->doesnt_buy)
      delete [] SHOP->doesnt_buy;
    SHOP->doesnt_buy = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_BUYMSG:
    if (SHOP->buy)
      delete [] SHOP->buy;
    SHOP->buy = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_SELLMSG:
    if (SHOP->sell)
      delete [] SHOP->sell;
    SHOP->sell = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_SHOPNAME:
    if (SHOP->shopname)
      delete [] SHOP->shopname;
    SHOP->shopname = str_dup(arg);
    shedit_disp_text_menu(d);
    break;
  case SHEDIT_SELLING_MENU:
    switch (*arg) {
    case '0':
      shedit_disp_menu(d);
      break;
    case 'a':
    case 'A':
      send_to_char("Enter Item VNum to sell: ", CH);
      d->edit_mode = SHEDIT_SELL_ADD;
      break;
    case 'd':
    case 'D':
      send_to_char("Delete which entry: ", CH);
      d->edit_mode = SHEDIT_SELL_DELETE;
      break;
    }
    break;
  case SHEDIT_SELL_ADD:
    if (number > 0) {
      struct shop_sell_data *sell = new shop_sell_data;
      sell->vnum = number;
      sell->next = SHOP->selling;
      sell->stock = 0;
      SHOP->selling = sell;
      CLS(CH);
      send_to_char("0) Always\r\n1) Availability Code\r\n2) Limited Stock\r\nEnter Supply Type: ", CH);
      d->edit_mode = SHEDIT_SELL_ADD1;
    } else
      send_to_char("Invalid VNum! What Item VNum: ", CH);
    break;
  case SHEDIT_SELL_ADD1:
    if (number >= 0 && number <= 2) {
      SHOP->selling->type = number;
      if (number == SELL_STOCK) {
        send_to_char("How many in stock: ", CH);
        d->edit_mode = SHEDIT_SELL_ADD2;
      } else
        shedit_disp_selling_menu(d);
    } else
      send_to_char("Invalid Type! Enter Supply Type: ", CH);
    break;
  case SHEDIT_SELL_ADD2:
    if (number > 0) {
      SHOP->selling->stock = number;
      shedit_disp_selling_menu(d);
    } else
      send_to_char("Must be stocking more than 0! How many in stock: ", CH);
    break;
  case SHEDIT_SELL_DELETE:
    if (number > 0) {
      for (struct shop_sell_data *sell = SHOP->selling; number && sell; sell = sell->next) {
        number--;
        if (!number) {
          struct shop_sell_data *temp;
          REMOVE_FROM_LIST(sell, SHOP->selling, next);
          delete [] sell;
          break;
        }
      }
      shedit_disp_selling_menu(d);
    } else
      shedit_disp_selling_menu(d);
    break;
  }
}

bool shop_can_sell_object(struct obj_data *obj, struct char_data *keeper, int shop_nr) {
  if (!obj) {
    snprintf(buf2, sizeof(buf2), "Shop %ld ('%s'): Hiding nonexistant item from sale.", shop_table[shop_nr].vnum, keeper ? GET_NAME(keeper) : "NO_KEEPER");
    mudlog(buf2, keeper, LOG_SYSLOG, TRUE);
    return FALSE;
  }

  // Pre-compose our rejection string.
  snprintf(buf2, sizeof(buf2), "Shop %ld ('%s'): Hiding %s (%ld) from sale due to ",
           shop_table[shop_nr].vnum,
           keeper ? GET_NAME(keeper) : "masked",
           GET_OBJ_NAME(obj),
           GET_OBJ_VNUM(obj));

  // Don't allow sale of forbidden vnums.
  if (GET_OBJ_VNUM(obj) == OBJ_OLD_BLANK_MAGAZINE_FROM_CLASSIC
      || GET_OBJ_VNUM(obj) == OBJ_BLANK_MAGAZINE) {
    strlcat(buf2, "matching a forbidden vnum.", sizeof(buf2));
    mudlog(buf2, keeper, LOG_SYSLOG, TRUE);
    extract_obj(obj);
    return FALSE;
  }

  // Don't allow sale of zero-cost items.
  if (GET_OBJ_COST(obj) < 1) {
    snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), "cost of %d.", GET_OBJ_COST(obj));
    mudlog(buf2, keeper, LOG_SYSLOG, TRUE);
    extract_obj(obj);
    return FALSE;
  }

  // Checks based on item type.
  switch (GET_OBJ_TYPE(obj)) {
    // Don't allow sale of NERP spell formulae.
    case ITEM_SPELL_FORMULA:
      if (spell_is_nerp(GET_SPELLFORMULA_SPELL(obj))) {
        snprintf(ENDOF(buf2), sizeof(buf2) - strlen(buf2), "having NERP spell %s.", spells[GET_OBJ_VAL(obj, 1)].name);
        mudlog(buf2, keeper, LOG_SYSLOG, TRUE);
        extract_obj(obj);
        return FALSE;
      }
      break;
    /*
    case ITEM_FIREWEAPON:
    case ITEM_MISSILE:
      strlcat(buf, "being a fireweapon or fireweapon ammo.", sizeof(buf));
      mudlog(buf2, keeper, LOG_SYSLOG, TRUE);
      extract_obj(obj);
      return FALSE;
    */
  }

  // Can sell it.
  return TRUE;
}

void shop_install(char *argument, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr) {
  struct obj_data *obj;
  char buf[MAX_STRING_LENGTH];

  // Non-docs won't install things.
  if (!shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
    strlcpy(buf, "Hold on now, I'm not a doctor! Find someone else to install your 'ware.", sizeof(buf));
    do_say(keeper, buf, cmd_say, 0);
    return;
  }

  if (!access_level(ch, LVL_ADMIN) && !(CAN_SEE(keeper, ch))) {
    strlcpy(buf, "How am I supposed to work on someone I can't see?", sizeof(buf));
    do_say(keeper, buf, cmd_say, 0);
    return;
  }

  if (IS_PROJECT(ch) || IS_NPC(ch)) {
    send_to_char("You're having a hard time getting the shopkeeper's attention.\r\n", ch);
    return;
  }

  argument = one_argument(argument, arg);

  if (!*arg) {
    send_to_char(ch, "What 'ware you want the shopkeeper to install?\r\n");
    return;
  }

  int dotmode = find_all_dots(arg, sizeof(arg));

  /* Can't junk or donate all */
  if ((dotmode == FIND_ALL) || dotmode == FIND_ALLDOT) {
    send_to_char(ch, "You'll have to install one thing at a time.\r\n");
    return;
  }

  if (!(obj = get_obj_in_list_vis(ch, arg, ch->carrying))) {
    send_to_char(ch, "You don't seem to have %s %s in your inventory.\r\n", AN(arg), arg);
    return;
  }

  if (GET_OBJ_TYPE(obj) != ITEM_SHOPCONTAINER) {
    send_to_char(ch, "Shopkeepers can only install 'ware from packages, and %s doesn't qualify.\r\n", GET_OBJ_NAME(obj));
    return;
  }

  if (!obj->contains) {
    send_to_char(ch, "%s is empty!\r\n", capitalize(GET_OBJ_NAME(obj)));
    snprintf(buf, sizeof(buf), "SYSERR: Shop container '%s' is empty!", GET_OBJ_NAME(obj));
    mudlog(buf, ch, LOG_SYSLOG, TRUE);
    return;
  }

  obj = obj->contains;

  // We charge 1/X of the price of the thing to install it, up to the configured maximum value.
  int install_cost = get_cyberware_install_cost(obj);

  // Try to deduct the install cost from their credstick.
  struct obj_data *cred = get_first_credstick(ch, "credstick");
  if (!cred || install_cost > GET_BANK(ch))
    cred = NULL;

  if (!cred && install_cost > GET_NUYEN(ch)) {
    snprintf(buf, sizeof(buf), "%s I'd charge %d nuyen to install that. Come back when you've got the cash.", GET_CHAR_NAME(ch), install_cost);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return;
  }

  // Chargen shops should never see an install command like this, so we automatically assume you're getting injured.
  if (install_ware_in_target_character(obj, keeper, ch, TRUE)) {
    snprintf(buf, sizeof(buf), "%s That'll be %d nuyen.", GET_CHAR_NAME(ch), install_cost);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);

    // Success! Deduct the cost from your payment method.
    if (cred)
      lose_bank(ch, install_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);
    else
      lose_nuyen(ch, install_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);
  }
}

void shop_uninstall(char *argument, struct char_data *ch, struct char_data *keeper, vnum_t shop_nr) {
  struct obj_data *obj;
  char buf[MAX_STRING_LENGTH];

  // Non-docs won't uninstall things.
  if (!shop_table[shop_nr].flags.IsSet(SHOP_DOCTOR)) {
    strlcpy(buf, "Hold on now, I'm not a doctor! Find someone else to uninstall your 'ware.", sizeof(buf));
    do_say(keeper, buf, cmd_say, 0);
    return;
  }

  // Can't uninstall in chargen.
  if (shop_table[shop_nr].flags.IsSet(SHOP_CHARGEN)) {
    send_to_char(ch, "Sorry, you can't do that in character generation.\r\n");
    return;
  }

  if (!access_level(ch, LVL_ADMIN) && !(CAN_SEE(keeper, ch))) {
    strlcpy(buf, "How am I supposed to work on someone I can't see?", sizeof(buf));
    do_say(keeper, buf, cmd_say, 0);
    return;
  }

  if (IS_PROJECT(ch) || IS_NPC(ch)) {
    send_to_char("You're having a hard time getting the shopkeeper's attention.\r\n", ch);
    return;
  }

  argument = one_argument(argument, arg);

  if (!*arg) {
    send_to_char(ch, "What 'ware you want the shopkeeper to uninstall?\r\n");
    return;
  }

  int dotmode = find_all_dots(arg, sizeof(arg));

  /* Can't junk or donate all */
  if ((dotmode == FIND_ALL) || dotmode == FIND_ALLDOT) {
    send_to_char(ch, "You'll have to uninstall one thing at a time.\r\n");
    return;
  }

  if (!(obj = get_obj_in_list_vis(ch, arg, ch->cyberware)) && !(obj = get_obj_in_list_vis(ch, arg, ch->bioware))) {
    send_to_char(ch, "You don't seem to have %s %s installed.\r\n", AN(arg), arg);
    return;
  }

  if (GET_OBJ_TYPE(obj) != ITEM_BIOWARE && GET_OBJ_TYPE(obj) != ITEM_CYBERWARE) {
    send_to_char(ch, "Shopkeepers can only uninstall 'ware.\r\n", GET_OBJ_NAME(obj));
    return;
  }

  // We charge 1/X of the price of the thing to install it, up to the configured maximum value.
  int uninstall_cost = get_cyberware_install_cost(obj);

  // Try to deduct the install cost from their credstick.
  struct obj_data *cred = get_first_credstick(ch, "credstick");
  if (!cred || uninstall_cost > GET_BANK(ch))
    cred = NULL;

  if (!cred && uninstall_cost > GET_NUYEN(ch)) {
    snprintf(buf, sizeof(buf), "%s I'd charge %d nuyen to uninstall that. Come back when you've got the cash.", GET_CHAR_NAME(ch), uninstall_cost);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);
    return;
  }

  if (GET_CYBERWARE_TYPE(obj) == CYB_CHIPJACK && obj->contains) {
    send_to_char("You can't uninstall a chipjack with chips in it.\r\n", ch);
    return;
  }

  if (GET_CYBERWARE_TYPE(obj) == CYB_MEMORY && obj->contains) {
    send_to_char("You can't uninstall headware memory with data in it.\r\n", ch);
    return;
  }

  // Chargen shops should never see an install command like this, so we automatically assume you're getting injured.
  if (uninstall_ware_from_target_character(obj, keeper, ch, TRUE)) {
    snprintf(buf, sizeof(buf), "%s That'll be %d nuyen.", GET_CHAR_NAME(ch), uninstall_cost);
    do_say(keeper, buf, cmd_say, SCMD_SAYTO);

    // Success! Deduct the cost from your payment method.
    if (cred)
      lose_bank(ch, uninstall_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);
    else
      lose_nuyen(ch, uninstall_cost, NUYEN_OUTFLOW_SHOP_PURCHASES);

    // Package it up and hand it over.
    if (GET_OBJ_COST(obj) > 0) {
      struct obj_data *shop_container = shop_package_up_ware(obj);
      act("$n packages up $N's old 'ware and hands it to $M.", TRUE, keeper, NULL, ch, TO_NOTVICT);
      act("$n packages up your old 'ware and hands it to you.", TRUE, keeper, NULL, ch, TO_VICT);
      obj_to_char(shop_container, ch);
    } else {
      snprintf(buf, sizeof(buf), "%s Sorry, %s was too damaged to be worth reusing.", GET_CHAR_NAME(ch), GET_OBJ_NAME(obj));
      do_say(keeper, buf, cmd_say, SCMD_SAYTO);
      extract_obj(obj);
      obj = NULL;
    }
  }
}

struct obj_data *shop_package_up_ware(struct obj_data *obj) {
  struct obj_data *shop_container = read_object(OBJ_SHOPCONTAINER, VIRTUAL, OBJ_LOAD_REASON_SPECPROC);
  GET_OBJ_BARRIER(shop_container) = 32;
  GET_OBJ_MATERIAL(shop_container) = MATERIAL_ADV_PLASTICS;
  GET_OBJ_COST(shop_container) = 0;
  GET_OBJ_EXTRA(shop_container).SetBit(ITEM_EXTRA_KEPT);

  snprintf(buf3, sizeof(buf3), "a packaged-up '%s'%s", GET_OBJ_NAME(obj), obj->restring ? " (restrung)" : "");
  DELETE_ARRAY_IF_EXTANT(shop_container->restring);
  shop_container->restring = str_dup(buf3);

  obj_to_obj(obj, shop_container);
  return shop_container;
}

void save_shop_orders() {
  PERF_PROF_SCOPE(pr_, __func__);
  FILE *fl;
  float totaltime = 0;
  time_t curr_time = time(0);
  char shop_file_name[MAX_STRING_LENGTH];
  char shop_message[MAX_STRING_LENGTH];

  for (int shop_nr = 0; shop_nr <= top_of_shopt; shop_nr++) {
    // Wipe the existing shop order save files-- they're out of date.
    snprintf(shop_file_name, sizeof(shop_file_name), "order/%ld", shop_table[shop_nr].vnum);
    unlink(shop_file_name);

    if (shop_table[shop_nr].order) {
      // Expire out orders that have reached their end of life. Yes, this means a whole separate for-loop just for this.
      struct shop_order_data *next_order, *temp;
      for (struct shop_order_data *order = shop_table[shop_nr].order; order; order = next_order) {
        next_order = order->next;
        totaltime = order->expiration - curr_time;
        if (totaltime <= 0) {
          // Notify them about the expiry, but only for orders with a prepay-- this prevents the 7-day spamstorm when this change is first launched.
          if (order->paid > 0) {
            // Calculate the amount.
            int total_prepayment = order->paid * order->number;
            int repayment_amount = total_prepayment - (total_prepayment / PREORDER_RESTOCKING_FEE_DIVISOR);

            // Look up the item (we need its name for the mail).
            int real_obj = real_object(order->item);
            snprintf(shop_message, sizeof(shop_message), "%s can't be held for you any longer at %s. %d nuyen will be refunded to your account.\r\n",
                     real_obj > 0 ? CAP(obj_proto[real_obj].text.name) : "Something",
                     shop_table[shop_nr].shopname,
                     repayment_amount
                    );

            // Look up the shopkeeper, then send the mail with their name attached.
            int real_mob = real_mobile(shop_table[shop_nr].keeper);
            if (real_mob > 0)
              raw_store_mail(order->player, 0, mob_proto[real_mob].player.physical_text.name, (const char *) shop_message);
            else
              raw_store_mail(order->player, 0, "An anonymous shopkeeper", (const char *) shop_message);

            // Wire the funds. This will not notify them (they're already getting a message through here.)
            // This is a thorny one-- this is technically a sink, since we're losing X% of the refunded value, but the PC may not be online.
            // We'll leave this as an invisible sink for now.
            if (repayment_amount > 0)
              wire_nuyen(NULL, repayment_amount, order->player, "expired shop order refund");
          }

          // Remove the order from the list, then delete it.
          REMOVE_FROM_LIST(order, shop_table[shop_nr].order, next);
          delete order;
        }
      }

      // Since we potentially wiped all the shop orders, we have to check if we have any others-- no sense writing an empty file.
      if (!shop_table[shop_nr].order)
        continue;

      // We have orders, so open the shop file and write the header.
      if (!(fl = fopen(shop_file_name, "w"))) {
        perror("SYSERR: Error saving order file");
        continue;
      }
      int i = 0;
      fprintf(fl, "[ORDERS]\n");

      // Iterate through the orders, writing them each to file. Also, send a mail if the order is ready to be picked up.
      for (struct shop_order_data *order = shop_table[shop_nr].order; order; order = order->next, i++) {
        totaltime = order->timeavail - time(0);
        if (!order->sent && totaltime < 0) {
          int real_obj = real_object(order->item);
          snprintf(shop_message, sizeof(shop_message), "%s has arrived at %s and is ready to be received for a total cost of %d nuyen. It will be held for you for %d days.\r\n",
                   real_obj > 0 ? CAP(obj_proto[real_obj].text.name) : "Something",
                   shop_table[shop_nr].shopname,
                   (order->price - order->paid) * order->number,
                   PREORDERS_ARE_GOOD_FOR_X_DAYS
                  );
          int real_mob = real_mobile(shop_table[shop_nr].keeper);
          if (real_mob > 0)
            raw_store_mail(order->player, 0, mob_proto[real_mob].player.physical_text.name, (const char *) shop_message);
          else
            raw_store_mail(order->player, 0, "An anonymous shopkeeper", (const char *) shop_message);
          order->sent = TRUE;
        }
        fprintf(fl, "\t[ORDER %d]\n", i);
        fprintf(fl, "\t\tItem:\t%ld\n"
                "\t\tPlayer:\t%ld\n"
                "\t\tTime:\t%d\n"
                "\t\tNumber:\t%d\n"
                "\t\tPrice:\t%d\n"
                "\t\tSent:\t%d\n"
                "\t\tPaid:\t%d\n"
                "\t\tExpiration:\t%ld\n", order->item, order->player, order->timeavail, order->number, order->price, order->sent, order->paid, order->expiration);
      }
      fclose(fl);
    }
  }
}

bool shop_will_buy_item_from_ch(rnum_t shop_nr, struct obj_data *obj, struct char_data *ch) {
  // This item cannot be sold.
  if (IS_OBJ_STAT(obj, ITEM_EXTRA_NOSELL) || IS_OBJ_STAT(obj, ITEM_EXTRA_STAFF_ONLY) || IS_OBJ_STAT(obj, ITEM_EXTRA_WIZLOAD)) {
    send_to_char(ch, "%s can't be sold.\r\n", capitalize(GET_OBJ_NAME(obj)));
    return FALSE;
  }

  if (IS_OBJ_STAT(obj, ITEM_EXTRA_HARDENED_ARMOR) && GET_WORN_HARDENED_ARMOR_CUSTOMIZED_FOR(obj) != -1) {
    send_to_char(ch, "%s has been customized already, so it can't be sold.\r\n", capitalize(GET_OBJ_NAME(obj)));
    return FALSE;
  }

  if (!obj->contains && GET_OBJ_TYPE(obj) == ITEM_SHOPCONTAINER) {
    send_to_char(ch, "%s is empty!\r\n", capitalize(GET_OBJ_NAME(obj)));
    snprintf(buf, sizeof(buf), "SYSERR: Shop container '%s' is empty!", GET_OBJ_NAME(obj));
    mudlog(buf, ch, LOG_SYSLOG, TRUE);
    return FALSE;
  }

  // Item has contents.
  if (obj->contains) {
    switch (GET_OBJ_TYPE(obj)) {
      case ITEM_SHOPCONTAINER:
        break;
      case ITEM_WEAPON:
        if (GET_OBJ_TYPE(obj->contains) != ITEM_GUN_MAGAZINE) {
          send_to_char(ch, "You'll have to empty %s out before you can sell it.\r\n", decapitalize_a_an(GET_OBJ_NAME(obj)));
          return FALSE;
        }
        break;
      default:
        send_to_char(ch, "You'll have to empty %s out before you can sell it.\r\n", decapitalize_a_an(GET_OBJ_NAME(obj)));
        return FALSE;
    }
  }

  // This item has no value.
  if (GET_OBJ_COST(obj) < 1) {
    send_to_char(ch, "%s is worthless!\r\n", capitalize(GET_OBJ_NAME(obj)));
    return FALSE;
  }

  // Item is not from a connected zone.
  if (vnum_from_non_approved_zone(GET_OBJ_VNUM(obj))) {
    char oopsbuf[1000];
    snprintf(oopsbuf, sizeof(oopsbuf), "BUILD ERROR: Somehow %s got %s^n (%ld) which is from a non-approved zone.",
             GET_CHAR_NAME(ch),
             GET_OBJ_NAME(obj),
             GET_OBJ_VNUM(obj));
    mudlog(oopsbuf, ch, LOG_SYSLOG, TRUE);
    // Disabling this rejection code for now until we fix all these. -LS
//    send_to_char(ch, "%s is bugged!\r\n", capitalize(GET_OBJ_NAME(obj)));
//    return FALSE;
  }

  // If this shop doesn't buy this item type at all, bail out. We don't send a message for this one-- the shopkeeper has a flavor line to say.
  if (!shop_table[shop_nr].buytypes.IsSet(GET_OBJ_TYPE(obj))) {
    return FALSE;
  }

  return TRUE;
}

int get_eti_test_results(struct char_data *ch, int eti_skill, int availtn, int availoff, int kinesics, int meta_penalty, int lifestyle, int pheromone_dice, int skill_dice) {
  char rollbuf[10000] = {0};
  
  // Calculate eti TNs, factoring in settings, powers, and racism.
  int target = availtn;
  snprintf(rollbuf, sizeof(rollbuf), "Etiquette test. Initial TN %d", target);

  if (availoff) {
    snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ", -%d (availoffset)", availoff);
    target -= availoff;
  }

  if (kinesics) {
    snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ", -%d (kinesics)", kinesics);
    target -= kinesics;
  }

  if (meta_penalty) {
    snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ", +%d (metavariant)", meta_penalty);
    target += meta_penalty;
  }

  // House rule: Give a better TN for high-grade lifestyles.
  switch (lifestyle) {
    case LIFESTYLE_HIGH:
      target -= 1;
      strlcat(rollbuf, ", -1 (high lifestyle)", sizeof(rollbuf));
      break;
    case LIFESTYLE_LUXURY:
      target -= 2;
      strlcat(rollbuf, ", -2 (luxury lifestyle)", sizeof(rollbuf));
      break;
  }

  // Calculate their skill dice, including from bioware.
  int skill = (skill_dice || !ch) ? skill_dice : get_skill(ch, eti_skill, target);
  snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ", final %d after get_skill(). Base skill %d", target, skill);

  if (pheromone_dice) {
    snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ", +%d (pheromones)", pheromone_dice);
    skill += pheromone_dice;
  }

  // Roll up the success test.
  int success = success_test(skill, target);
  snprintf(ENDOF(rollbuf), sizeof(rollbuf) - strlen(rollbuf), ". Rolled %d success%s.", success, success == 1 ? "" : "s");
  
  if (ch)
    act(rollbuf, TRUE, ch, 0, 0, TO_ROLLS);

  return success;
}

int get_cyberware_install_cost(struct obj_data *ware) {
  return MIN(CYBERWARE_INSTALLATION_COST_MAXIMUM, GET_OBJ_COST(ware) / CYBERWARE_INSTALLATION_COST_FACTOR);
}