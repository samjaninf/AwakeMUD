#include "awake.hpp"
#include "structs.hpp"
#include "utils.hpp"
#include "comm.hpp"
#include "newecho.hpp"
#include "ignore_system.hpp"
#include "db.hpp"
#include "interpreter.hpp"

extern int global_non_secure_random_number;

extern struct remem *safe_found_mem(struct char_data *rememberer, struct char_data *ch);
extern void display_room_name(struct char_data *ch, struct room_data *in_room, bool in_veh);
extern void display_room_desc(struct char_data *ch);
extern void disp_long_exits(struct char_data *ch, bool autom);
extern int isname(const char *str, const char *namelist);

const char *get_char_representation_for_docwagon(struct char_data *vict, struct char_data *viewer);
void send_docwagon_chat_message(struct char_data *ch, const char *message, bool is_chat);

#define IS_VALID_POTENTIAL_RESCUER(plr) (GET_LEVEL(plr) == LVL_MORTAL && plr->char_specials.timer < 5 && !PRF_FLAGGED(plr, PRF_AFK) && !PRF_FLAGGED(plr, PRF_HIRED))

// Returns a 5-digit faux ID to help tell characters apart in anonymous messages.
int get_docwagon_faux_id(struct char_data *ch) {
  return (((GET_IDNUM_EVEN_IF_PROJECTING(ch) * 217 + global_non_secure_random_number) + 29783) / 3) % 99999;
}

const char *get_location_string_for_room(struct room_data *in_room) {
  static char location_info[1000] = { 0 };
  const char *gridguide_coords = get_gridguide_coordinates_for_room(in_room);

  if (!in_room) {
    mudlog("SYSERR: Received invalid in_room to get_location_string_for_room()!", NULL, LOG_SYSLOG, TRUE);
    return "";
  }

  if (gridguide_coords) {
    snprintf(location_info, sizeof(location_info), "GridGuide coordinates [%s], AKA '%s' (%ld)", gridguide_coords, decapitalize_a_an(GET_ROOM_NAME(in_room)), GET_ROOM_VNUM(in_room));
  } else {
    snprintf(location_info, sizeof(location_info), "'%s' (%ld)", decapitalize_a_an(GET_ROOM_NAME(in_room)), GET_ROOM_VNUM(in_room));
  }

  return location_info;
}

int alert_player_doctors_of_mort(struct char_data *ch, struct obj_data *docwagon) {
  char speech_buf[500];
  int potential_rescuer_count = 0;
  struct room_data *in_room;
  const char *location_info;

  if (!ch || !(in_room = get_ch_in_room(ch))) {
    mudlog("SYSERR: NULL or missing char to alert_player_doctors_of_mort()!", ch, LOG_SYSLOG, TRUE);
    return 0;
  }

  // They don't have a modulator-- not an error, just bail.
  if (!docwagon && !(docwagon = find_best_active_docwagon_modulator(ch)))
    return 0;

  // Skip newbies. They have no death penalty.
  if (PLR_FLAGGED(ch, PLR_NEWBIE))
    return 0;

  // They don't want to participate-- not an error, just bail.
  if (PRF_FLAGGED(ch, PRF_DONT_ALERT_PLAYER_DOCTORS_ON_MORT))
    return 0;

  location_info = get_location_string_for_room(in_room);

  for (struct descriptor_data *desc = descriptor_list; desc; desc = desc->next) {
    struct char_data *plr = desc->original ? desc->original : desc->character;

    if (!plr || IS_NPC(plr) || !plr->desc || plr == ch)
      continue;

    if (!AFF_FLAGGED(plr, AFF_WEARING_ACTIVE_DOCWAGON_RECEIVER) || !AWAKE(plr))
      continue;

    if (IS_IGNORING(plr, is_blocking_ic_interaction_from, ch) || IS_IGNORING(ch, is_blocking_ic_interaction_from, plr))
      continue;

    // Compose the display string.
    const char *display_string = decapitalize_a_an(get_char_representation_for_docwagon(ch, plr));

    // We already sent this person a message, so just prompt them instead of doing the whole thing.
    if (ch->sent_docwagon_messages_to.find(GET_IDNUM_EVEN_IF_PROJECTING(plr)) != ch->sent_docwagon_messages_to.end()) {
      send_to_char(plr, "Your DocWagon receiver vibrates, indicating that %s still needs assistance.\r\n", display_string);

      if (IS_VALID_POTENTIAL_RESCUER(plr)) {
        potential_rescuer_count++;
      }

      continue;
    }

    switch (GET_DOCWAGON_CONTRACT_GRADE(docwagon)) {
      case DOCWAGON_GRADE_PLATINUM:
        snprintf(speech_buf, sizeof(speech_buf),
                 "Any available unit, we have a Platinum-grade contractee downed at %s."
                 " Records show them as %s. This is a highest-priority recovery!",
                 location_info,
                 get_string_after_color_code_removal(display_string, NULL));

        send_to_char(plr,
                     "Your DocWagon receiver emits a shrill alarm, followed by a brusque human voice: \"^Y%s^n\"\r\n",
                     capitalize(replace_too_long_words(plr, NULL, speech_buf, SKILL_ENGLISH, "^Y")));

        if (plr->in_room) {
          act("$n's DocWagon receiver emits a shrill alarm.", TRUE, plr, 0, 0, TO_ROOM);
          for (struct char_data *mob = plr->in_room->people; mob; mob = mob->next_in_room) {
            if (IS_NPC(mob) && !mob->desc) {
              set_mob_alert(mob, 20);
            }
          }
        } else if (plr->in_veh) {
          act("$n's DocWagon receiver emits a shrill alarm.", TRUE, plr, 0, 0, TO_VEH);
        }
        break;
      case DOCWAGON_GRADE_GOLD:
        snprintf(speech_buf, sizeof(speech_buf),
                 "Recovery specialist, a Gold-grade contractee has been downed at %s. Records show them as %s. Render aid if possible.",
                 location_info,
                 get_string_after_color_code_removal(display_string, NULL));

        send_to_char(plr,
                     "Your DocWagon receiver beeps loudly, followed by an automated voice: \"^y%s^n\"\r\n",
                     capitalize(replace_too_long_words(plr, NULL, speech_buf, SKILL_ENGLISH, "^y")));

        if (plr->in_room) {
          act("$n's DocWagon receiver beeps loudly.", TRUE, plr, 0, 0, TO_ROOM);
          for (struct char_data *mob = plr->in_room->people; mob; mob = mob->next_in_room) {
            if (IS_NPC(mob) && !mob->desc) {
              set_mob_alert(mob, 20);
            }
          }
        } else if (plr->in_veh) {
          act("$n's DocWagon receiver beeps loudly.", TRUE, plr, 0, 0, TO_VEH);
        }
        break;
      case DOCWAGON_GRADE_BASIC:
      default:
        if (GET_DOCWAGON_CONTRACT_GRADE(docwagon) != DOCWAGON_GRADE_BASIC) {
          char oopsbuf[500];
          snprintf(oopsbuf, sizeof(oopsbuf), "SYSERR: Unknown DocWagon modulator grade %d for %s (%ld)!", GET_DOCWAGON_CONTRACT_GRADE(docwagon), GET_OBJ_NAME(docwagon), GET_OBJ_VNUM(docwagon));
          mudlog(oopsbuf, ch, LOG_SYSLOG, TRUE);
        }

        snprintf(speech_buf, sizeof(speech_buf),
                 "Notice: Basic-grade contractee downed at %s. Recover if safe to do so.",
                 location_info);

        send_to_char(plr,
                     "Your DocWagon receiver vibrates, and text flashes up on its screen: \"^W%s^n\" An accompanying image of %s^n is displayed.\r\n",
                     capitalize(replace_too_long_words(plr, NULL, speech_buf, SKILL_ENGLISH, "^W")),
                     display_string);
        break;
    }

    if (!IS_SENATOR(plr)) {
      send_to_char(plr, "^c(Please announce on ^WOOC^c if you're on your way! Alternatively, use ^WDOCWAGON ACCEPT %s^c for anonymous response.)^n\r\n", GET_CHAR_NAME(ch));
    }

    // If they're not staff, AFK, idle, or participating in a PRUN, add them to the potential rescuer count that will be sent to the downed player.
    if (IS_VALID_POTENTIAL_RESCUER(plr)) {
      potential_rescuer_count++;
    }

    // Add them to our sent-to list.
    ch->sent_docwagon_messages_to.insert(std::make_pair(GET_IDNUM_EVEN_IF_PROJECTING(plr), TRUE));
  }

  return potential_rescuer_count;
}

void alert_player_doctors_of_contract_withdrawal(struct char_data *ch, bool withdrawn_because_of_death, bool withdrawn_because_of_autodoc) {
  char speech_buf[500];

  if (!ch) {
    mudlog("SYSERR: NULL char to alert_player_doctors_of_contract_withdrawal()!", ch, LOG_SYSLOG, TRUE);
    return;
  }

  if (ch->sent_docwagon_messages_to.empty()) {
    // They hadn't actually alerted yet.
    return;
  }

  for (struct descriptor_data *d = descriptor_list; d; d = d->next) {
    if (!d->character || d->character == ch)
      continue;

    if (!AFF_FLAGGED(d->character, AFF_WEARING_ACTIVE_DOCWAGON_RECEIVER))
      continue;

    if (IS_IGNORING(d->character, is_blocking_ic_interaction_from, ch) || IS_IGNORING(ch, is_blocking_ic_interaction_from, d->character)) {
      // log_vfprintf("playerdoc-upped-debug: %s skipping %s due to ignore state", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      continue;
    }

    // We didn't message this person.
    if (ch->sent_docwagon_messages_to.find(GET_IDNUM_EVEN_IF_PROJECTING(d->character)) == ch->sent_docwagon_messages_to.end()) {
      // log_vfprintf("playerdoc-upped-debug: %s skipping %s -- not in list", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      continue;
    }

    const char *display_string = get_string_after_color_code_removal(CAP(get_char_representation_for_docwagon(ch, d->character)), NULL);

    if (withdrawn_because_of_death) {
      snprintf(speech_buf, sizeof(speech_buf), "Contract withdrawal notice: %s no longer has viable vital signs.", display_string);

      send_to_char(d->character,
                    "Your DocWagon receiver emits a sad beep and displays: \"^r%s^n\"\r\n",
                    capitalize(replace_too_long_words(d->character, NULL, speech_buf, SKILL_ENGLISH, "^r")));

      if (d->character->in_room) {
        act("$n's DocWagon receiver emits a sad beep.", FALSE, d->character, 0, 0, TO_ROOM);
      } else if (d->character->in_veh) {
        act("$n's DocWagon receiver emits a sad beep.", FALSE, d->character, 0, 0, TO_VEH);
      }
    } else if (withdrawn_because_of_autodoc) {
      snprintf(speech_buf, sizeof(speech_buf),
                "Contract withdrawal notice: %s has been picked up by first-party contractors.",
                display_string);

      send_to_char(d->character,
                    "Your DocWagon receiver beeps a corporate jingle and displays: \"^o%s^n\"\r\n",
                    capitalize(replace_too_long_words(d->character, NULL, speech_buf, SKILL_ENGLISH, "^o")));

      if (d->character->in_room) {
        act("$n's DocWagon receiver beeps a corporate jingle.", FALSE, d->character, 0, 0, TO_ROOM);
      } else if (d->character->in_veh) {
        act("$n's DocWagon receiver beeps a corporate jingle.", FALSE, d->character, 0, 0, TO_VEH);
      }
    } else {
      bool in_same_room = get_ch_in_room(d->character) == get_ch_in_room(ch);

      snprintf(speech_buf, sizeof(speech_buf),
                "Contract %s notice: %s is no longer incapacitated.%s",
                in_same_room ? "completion" : "withdrawal",
                display_string,
                in_same_room ? " Well done!" : "");

      send_to_char(d->character,
                    "Your DocWagon receiver emits a cheery beep and displays: \"%s%s^n\"\r\n",
                    in_same_room ? "^c" : "^o",
                    capitalize(replace_too_long_words(d->character, NULL, speech_buf, SKILL_ENGLISH, in_same_room ? "^c" : "^o")));

      if (d->character->in_room) {
        act("$n's DocWagon receiver emits a cheery beep.", FALSE, d->character, 0, 0, TO_ROOM);
      } else if (d->character->in_veh) {
        act("$n's DocWagon receiver emits a cheery beep.", FALSE, d->character, 0, 0, TO_VEH);
      }
    }
  }

  // Purge their Docwagon lists.
  ch->sent_docwagon_messages_to.clear();
  ch->received_docwagon_ack_from.clear();
}

bool handle_player_docwagon_track(struct char_data *ch, char *argument) {
  skip_spaces(&argument);

  // This only works for people with receivers.
  if (!AFF_FLAGGED(ch, AFF_WEARING_ACTIVE_DOCWAGON_RECEIVER) || !AWAKE(ch))
    return FALSE;

  for (struct descriptor_data *d = descriptor_list; d; d = d->next) {
    // Invalid person?
    if (!d->character || d->character == ch || GET_POS(d->character) != POS_MORTALLYW)
      continue;

    // Ignoring you, or you ignoring them?
    if (IS_IGNORING(d->character, is_blocking_ic_interaction_from, ch) || IS_IGNORING(ch, is_blocking_ic_interaction_from, d->character)) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, "DEBUG: Skipping %s: Ignoring or ignored.\r\n", GET_CHAR_NAME(d->character));
      }
      continue;
    }

    // Wrong target?
    if (!isname(argument, get_string_after_color_code_removal(get_char_representation_for_docwagon(d->character, ch), NULL))) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, "DEBUG: Skipping %s (%s): '%s' does not match their representation.\r\n", get_char_representation_for_docwagon(d->character, ch), GET_CHAR_NAME(d->character), argument);
      }
      continue;
    }

    // No modulator on?
    {
      bool has_modulator = FALSE;
      for (int wear_idx = 0; wear_idx < NUM_WEARS; wear_idx++) {
        struct obj_data *eq = GET_EQ(d->character, wear_idx);
        if (eq && GET_OBJ_TYPE(eq) == ITEM_DOCWAGON && GET_DOCWAGON_BONDED_IDNUM(eq) == GET_IDNUM(d->character)) {
          has_modulator = TRUE;
          break;
        }
      }
      if (!has_modulator) {
        if (access_level(ch, LVL_PRESIDENT)) {
          send_to_char(ch, "DEBUG: Skipping %s: No modulator.\r\n", GET_CHAR_NAME(d->character));
        }
        continue;
      }
    }

    send_to_char("You squint at the tiny screen on your DocWagon receiver to try and get a better idea of where your client is...\r\n", ch);

    // Show them the room name, room description, and exits.
    struct room_data *was_in_room = ch->in_room;
    struct veh_data *was_in_veh = ch->in_veh;
    ch->in_room = get_ch_in_room(d->character);
    ch->in_veh = NULL;

    // Room name.
    display_room_name(ch, ch->in_room, FALSE);

    // Room desc.
    display_room_desc(ch);

    // Room exits.
    disp_long_exits(ch, TRUE);

    // Reset their in_room to the stored value.
    ch->in_room = was_in_room;
    ch->in_veh = was_in_veh;

    return TRUE;
  }

  send_to_char(ch, "You don't see any DocWagon clients named '%s' available to be tracked.\r\n", argument);
  return TRUE;
}

const char *get_char_representation_for_docwagon(struct char_data *vict, struct char_data *viewer) {
  // Compose the perceived description.
  struct remem *mem_record;
  static char display_string[500];

  if (!GET_NAME(vict)) {
    mudlog_vfprintf(viewer, LOG_SYSLOG, "SYSERR: Got NULL-NAMED vict to get_char_representation_for_docwagon(%s, %s)!", GET_CHAR_NAME(vict), GET_CHAR_NAME(viewer));
    *display_string = '\0';
    return display_string;
  }

  strlcpy(display_string, decapitalize_a_an(GET_NAME(vict)), sizeof(display_string));

  if ((mem_record = safe_found_mem(viewer, vict)))
    snprintf(ENDOF(display_string), sizeof(display_string) - strlen(display_string), "^n ( %s )", CAP(mem_record->mem));
  else if (IS_SENATOR(viewer))
    snprintf(ENDOF(display_string), sizeof(display_string) - strlen(display_string), "^n ( %s )", GET_CHAR_NAME(vict));

  return display_string;
}

#define MODE_ACCEPT  1
#define MODE_DECLINE 2
#define MODE_LIST    3

ACMD(do_docwagon) {
  int mode = 0;
  char mode_switch[MAX_INPUT_LENGTH] = { 0 };
  char output[MAX_STRING_LENGTH] = { 0 };

  // This only works for people with receivers.
  FAILURE_CASE(!AFF_FLAGGED(ch, AFF_WEARING_ACTIVE_DOCWAGON_RECEIVER), "You need to be wearing a DocWagon receiver to use this command-- modulators aren't sufficient for this command. Did you mean ^WCOMEGETME^n?");

  skip_spaces(&argument);
  char *name = one_argument(argument, mode_switch);

  if (!*arg || !*mode_switch) {
    send_to_char("Syntax:\r\n"
                 "  ^WDOCWAGON LIST^n             (to see who all needs help)\r\n"
                 "  ^WDOCWAGON ACCEPT <name>^n    (to accept a pickup request)\r\n"
                 "  ^WDOCWAGON WITHDRAW <name>^n  (to withdraw your acceptance)\r\n"
                 "  ^WDOCWAGON LOCATE <name>^n    (to see where they are)\r\n"
                 "  ^WDOCWAGON OOC <message>^n    (to coordinate with other Docwagon retrievers)\r\n", ch);
    return;
  }

  if (is_abbrev(mode_switch, "accept") || is_abbrev(mode_switch, "acknowledge") || is_abbrev(mode_switch, "take")) {
    FAILURE_CASE(!*name, "Syntax: DOCWAGON ACCEPT <name>");
      mode = MODE_ACCEPT;
  }
  else if (is_abbrev(mode_switch, "decline") || is_abbrev(mode_switch, "withdraw") || is_abbrev(mode_switch, "reject") || is_abbrev(mode_switch, "drop")) {
    FAILURE_CASE(!*name, "Syntax: DOCWAGON DECLINE <name>");
    mode = MODE_DECLINE;
  }
  else if (is_abbrev(mode_switch, "list")) {
    mode = MODE_LIST;
  }
  else if (is_abbrev(mode_switch, "show") || is_abbrev(mode_switch, "locate") || is_abbrev(mode_switch, "track")) {
    FAILURE_CASE(!*name, "Syntax: DOCWAGON LOCATE <name>");
    handle_player_docwagon_track(ch, name);
    return;
  }
  else if (is_abbrev(mode_switch, "chat") || is_abbrev(mode_switch, "say") || is_abbrev(mode_switch, "message") || is_abbrev(mode_switch, "communicate") || is_abbrev(mode_switch, "ooc")) {
    FAILURE_CASE(!*name, "Syntax: DOCWAGON OOC <message>");
    send_docwagon_chat_message(ch, name, TRUE);
    return;
  }
  else {
    send_to_char("Syntax: DOCWAGON (ACCEPT|WITHDRAW) <name>\r\n", ch);
    return;
  }

  // Find the downed person.
  for (struct descriptor_data *d = descriptor_list; d; d = d->next) {
    // Invalid person?
    if (!d->character || d->character == ch || GET_POS(d->character) != POS_MORTALLYW)
      continue;

    // Couldn't have alerted in the first place?
    if (PRF_FLAGGED(d->character, PRF_DONT_ALERT_PLAYER_DOCTORS_ON_MORT)) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, " - Skipping %s: Opted out of system.\r\n", GET_CHAR_NAME(d->character));
      }
      continue;
    }

    // Being ignored?
    if (IS_IGNORING(d->character, is_blocking_ic_interaction_from, ch) || IS_IGNORING(ch, is_blocking_ic_interaction_from, d->character)) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, " - Skipping %s: Ignoring or ignored.\r\n", GET_CHAR_NAME(d->character));
      }
      continue;
    }

    // Has a modulator?
    {
      bool has_modulator = FALSE;
      for (int wear_idx = 0; wear_idx < NUM_WEARS; wear_idx++) {
        struct obj_data *eq = GET_EQ(d->character, wear_idx);
        if (eq && GET_OBJ_TYPE(eq) == ITEM_DOCWAGON && GET_DOCWAGON_BONDED_IDNUM(eq) == GET_IDNUM(d->character)) {
          has_modulator = TRUE;
          break;
        }
      }
      if (!has_modulator) {
        if (access_level(ch, LVL_PRESIDENT)) {
          send_to_char(ch, " - Skipping %s: No modulator.\r\n", GET_CHAR_NAME(d->character));
        }
        continue;
      }
    }

    // Short circuit: LIST has no further logic to evaluate, so just print.
    if (mode == MODE_LIST) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, " - Listing %s.\r\n", GET_CHAR_NAME(d->character));
      }
      snprintf(ENDOF(output), sizeof(output) - strlen(output), " - %s: %s\r\n",
               get_char_representation_for_docwagon(d->character, ch),
               get_location_string_for_room(get_ch_in_room(d->character)));
      continue;
    }

    // Wrong person? (does not apply to DOCWAGON LIST)
    if (!keyword_appears_in_char(name, d->character, TRUE, TRUE, FALSE)) {
      if (access_level(ch, LVL_PRESIDENT)) {
        send_to_char(ch, " - Skipping %s: Keyword '%s' not applicable.\r\n", GET_CHAR_NAME(d->character), name);
      }
      continue;
    }

    // They have not yet received a message from us. ACCEPT is valid, WITHDRAW is not.
    if (d->character->received_docwagon_ack_from.find(GET_IDNUM_EVEN_IF_PROJECTING(ch)) == d->character->received_docwagon_ack_from.end()) {
      if (mode == MODE_DECLINE) {
        send_to_char(ch, "You haven't messaged %s yet. Use DOCWAGON ACCEPT instead.\r\n", GET_CHAR_NAME(d->character));
        return;
      }
      send_to_char("You anonymously notify them that you're on the way.\r\n", ch);
      send_to_char(d->character, "Your DocWagon modulator buzzes-- a player with the DocWagon ID %5d has acknowledged your request for assistance and is on their way!\r\n", get_docwagon_faux_id(ch));
      snprintf(buf3, sizeof(buf3), "%s has accepted %s's contract.", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      send_docwagon_chat_message(ch, buf3, FALSE);
      d->character->received_docwagon_ack_from.insert(std::make_pair(GET_IDNUM_EVEN_IF_PROJECTING(ch), TRUE));
      mudlog_vfprintf(ch, LOG_SYSLOG, "%s has ^gaccepted^n %s's DocWagon contract.", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      return;
    }
    // They have already received a message. WITHDRAW is valid, ACCEPT is not.
    else {
      if (mode == MODE_ACCEPT) {
        send_to_char(ch, "You've already messaged %s.\r\n", GET_CHAR_NAME(d->character));
        return;
      }
      send_to_char("You anonymously notify them that you're no longer on the way.\r\n", ch);
      send_to_char(d->character, "Your DocWagon modulator buzzes-- a player with the DocWagon ID %5d is no longer able to respond to your contract.\r\n", get_docwagon_faux_id(ch));
      snprintf(buf3, sizeof(buf3), "%s has ^rwithdrawn^n from %s's contract.", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      send_docwagon_chat_message(ch, buf3, FALSE);
      d->character->received_docwagon_ack_from.erase(d->character->received_docwagon_ack_from.find(GET_IDNUM_EVEN_IF_PROJECTING(ch)));
      mudlog_vfprintf(ch, LOG_SYSLOG, "%s has dropped %s's DocWagon contract (command).", GET_CHAR_NAME(ch), GET_CHAR_NAME(d->character));
      return;
    }
  }

  if (mode == MODE_LIST) {
    if (*output) {
      send_to_char(ch, "Patients in need of assistance:\r\n%s\r\n", output);
    } else {
      send_to_char("Your receiver isn't picking up any distress signals.\r\n", ch);
    }
    return;
  }

  send_to_char(ch, "Your DocWagon receiver can't find anyone in need of assistance named '%s^n'.\r\n", name);
}

void send_docwagon_chat_message(struct char_data *ch, const char *message, bool is_chat) {
  for (struct descriptor_data *desc = descriptor_list; desc; desc = desc->next) {
    struct char_data *plr = desc->original ? desc->original : desc->character;

    if (!plr || IS_NPC(plr) || !plr->desc)
      continue;

    if (!AFF_FLAGGED(plr, AFF_WEARING_ACTIVE_DOCWAGON_RECEIVER) || !AWAKE(plr))
      continue;

    if (IS_IGNORING(plr, is_blocking_ic_interaction_from, ch) || IS_IGNORING(ch, is_blocking_ic_interaction_from, plr))
      continue;

    char message_buf[MAX_INPUT_LENGTH * 2];
    strlcpy(message_buf, "^R[^WDocwagon OOC^R]^n: ", sizeof(message_buf));

    if (is_chat) {
      char final_character = get_final_character_from_string(message);
      snprintf(ENDOF(message_buf), sizeof(message_buf) - strlen(message_buf),
               "%s sends, \"%s%s^n\"\r\n",
               GET_CHAR_NAME(ch),
               capitalize(message),
               ispunct(final_character) ? (final_character == '^' ? "^." : "") : ".");
    } else {
      snprintf(ENDOF(message_buf), sizeof(message_buf) - strlen(message_buf), "%s^n\r\n", message);
    }

    send_to_char(message_buf, plr);
    store_message_to_history(desc, COMM_CHANNEL_DOCWAGON_CHAT, str_dup(message_buf));
  }
}