#ifndef __newmagic_h__
#define __newmagic_h__

extern struct char_data *create_elemental(struct char_data *ch, int type, int force, int idnum, int tradition);
extern void circle_build(struct char_data *ch, char *type, int force);
extern void lodge_build(struct char_data *ch, int force);
extern bool conjuring_drain(struct char_data *ch, int force);
extern void end_spirit_existance(struct char_data *ch, bool message);
extern bool check_spirit_sector(struct room_data *room, int type);
extern bool spell_drain(struct char_data *ch, int type, int force, int damage, bool minus_one_sustained=FALSE);
extern void totem_bonus(struct char_data *ch, int action, int type, int &target, int &skill);
extern void aspect_bonus(struct char_data *ch, int action, int spell_idx, int &target, int &skill);
extern void aspect_conjuring_bonus(struct char_data *ch, int action, int type, int &target, int &skill);
extern void mob_cast(struct char_data *ch, struct char_data *tch, struct obj_data *tobj, int spellnum, int level);
extern void end_sustained_spell(struct char_data *ch, struct sustain_data *sust);
extern void stop_spirit_power(struct char_data *spirit, int type);
extern char_data *find_spirit_by_id(int spiritid, long playerid);
extern void elemental_fulfilled_services(struct char_data *ch, struct char_data *mob, struct spirit_data *spirit);
extern int get_spell_affected_successes(struct char_data * ch, int type);
extern int get_max_usable_spell_successes(int spell, int force);
extern const char *warn_if_spell_under_potential(struct sustain_data *sust);
extern const char *get_spell_name(int spell, int subtype);
extern void set_casting_pools(struct char_data *ch, int casting, int drain, int spell_defense, int reflection, bool message);
extern bool check_spell_victim(struct char_data *ch, struct char_data *vict, int spell, char *buf);
extern bool create_sustained(struct char_data *ch, struct char_data *vict, int spell, int force, int sub, int success, int time_to_take_effect);
extern void end_all_sustained_spells_of_type_affecting_ch(int spell, int subtype, struct char_data *ch);
extern void end_all_spells_matching_function(struct char_data *ch, bool (*should_end_sust)(struct sustain_data));
extern void end_all_caster_records(struct char_data *ch, bool keep_sustained_by_other);
extern void end_all_spells_of_type_cast_by_ch(int spell, int subtype, struct char_data *ch);
extern void end_all_sustained_spells(struct char_data *ch);
extern bool spell_affecting_ch_is_cast_by_ch_or_group_member(struct char_data *ch, int spell_type);
extern void end_all_spells_cast_ON_ch(struct char_data *ch, bool keep_sustained_by_other);
extern void end_all_spells_cast_BY_ch(struct char_data *ch, bool keep_sustained_by_other);

#define DAMOBJ_NONE                     0
#define DAMOBJ_ACID                     1
#define DAMOBJ_AIR                      2
#define DAMOBJ_EARTH            3
#define DAMOBJ_FIRE                     4
#define DAMOBJ_ICE                      5
#define DAMOBJ_LIGHTNING        6
#define DAMOBJ_WATER            7
#define DAMOBJ_EXPLODE          8
#define DAMOBJ_PROJECTILE       9
#define DAMOBJ_CRUSH            10
#define DAMOBJ_SLASH            11
#define DAMOBJ_PIERCE           12
#define DAMOBJ_MANIPULATION     32

#define CONFUSION	            0
#define ENGULF		            1
#define CONCEAL		            2
#define MOVEMENTUP	          3
#define MOVEMENTDOWN	        4
#define NUM_SPIRIT_POWER_BITS 5

#define INIT_MAIN	             0
#define INIT_META	             1
#define INIT_GEAS	             2
#define INIT_CONFIRM_SIGNATURE 3

#define SPELLCASTING 0
#define CONJURING 1

// Because we define 'variable drain damage' in code as being a code below 0, we start out with -5 as our baseline
//  (equivalent to 'variable damage with 0 modifier'), then add to or subtract from that to indicate the +1, -1 etc.
#define VARIABLE_DRAIN_DAMAGE_CODE                 -5
#define PACK_VARIABLE_DRAIN_DAMAGE(modifier)       (VARIABLE_DRAIN_DAMAGE_CODE + modifier)
#define UNPACK_VARIABLE_DRAIN_DAMAGE(damage_code)  (-VARIABLE_DRAIN_DAMAGE_CODE + damage_code)

#define IS_COMBAT_ENTHRALLED_SHAMAN(ch) (GET_TRADITION(ch) == TRAD_SHAMANIC && (GET_TOTEM(ch) == TOTEM_BOAR || GET_TOTEM(ch) == TOTEM_POLECAT || GET_TOTEM(ch) == TOTEM_GATOR      \
                                                                                || GET_TOTEM(ch) == TOTEM_MOUNTAIN || GET_TOTEM(ch) == TOTEM_DOG))

#endif
