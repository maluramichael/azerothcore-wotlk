/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "Player.h"
#include "AccountMgr.h"
#include "AchievementMgr.h"
#include "AreaDefines.h"
#include "ArenaSpectator.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "ArenaSeasonMgr.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "BattlefieldWG.h"
#include "Battleground.h"
#include "BattlegroundAV.h"
#include "BattlegroundMgr.h"
#include "CellImpl.h"
#include "CharmInfo.h"
#include "Channel.h"
#include "CharacterCache.h"
#include "CharacterDatabaseCleaner.h"
#include "Chat.h"
#include "CombatLogPackets.h"
#include "Common.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "Formulas.h"
#include "GameEventMgr.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "LFGMgr.h"
#include "Log.h"
#include "LootItemStorage.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OutdoorPvP.h"
#include "OutdoorPvPMgr.h"
#include "Pet.h"
#include "PetitionMgr.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "Realm.h"
#include "ReputationMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TicketMgr.h"
#include "Tokenize.h"
#include "Trainer.h"
#include "Transport.h"
#include "Unit.h"
#include "UpdateData.h"
#include "Util.h"
#include "Vehicle.h"
#include "Weather.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "WorldState.h"
#include "WorldStateDefines.h"
#include "WorldStatePackets.h"
#include <cmath>
#include <queue>
#include "GridNotifiersImpl.h"

bool Player::IsImmuneToEnvironmentalDamage()
{
    // check for GM and death state included in isAttackableByAOE
    return (!isTargetableForAttack(false, nullptr)) || isTotalImmune();
}

uint32 Player::EnvironmentalDamage(EnviromentalDamage type, uint32 damage)
{
    // DAMAGE_FALL_TO_VOID bypasses all immunities (e.g. Divine Shield) to prevent
    // players from being stuck infinitely falling below the map
    if (type != DAMAGE_FALL_TO_VOID && IsImmuneToEnvironmentalDamage())
        return 0;

    // Absorb, resist some environmental damage type
    uint32 absorb = 0;
    uint32 resist = 0;

    switch (type)
    {
        case DAMAGE_LAVA:
        case DAMAGE_SLIME:
        {
            DamageInfo dmgInfo(this, this, damage, nullptr, type == DAMAGE_LAVA ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_NATURE, DIRECT_DAMAGE);
            Unit::CalcAbsorbResist(dmgInfo);
            absorb = dmgInfo.GetAbsorb();
            resist = dmgInfo.GetResist();
            damage = dmgInfo.GetDamage();
        }
        default:
            break;
    }

    Unit::DealDamageMods(this, damage, &absorb);

    WorldPackets::CombatLog::EnvironmentalDamageLog packet;
    packet.Victim = GetGUID();
    packet.Type = type != DAMAGE_FALL_TO_VOID ? type : DAMAGE_FALL;
    packet.Amount = damage;
    packet.Absorbed = absorb;
    packet.Resisted = resist;
    SendMessageToSet(packet.Write(), true);

    uint32 final_damage = Unit::DealDamage(this, this, damage, nullptr, SELF_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);

    if (!IsAlive())
    {
        if (type == DAMAGE_FALL)                               // DealDamage not apply item durability loss at self damage
        {
            LOG_DEBUG("entities.player", "Player::EnvironmentalDamage: Player '{}' ({}) fall to death, losing {} durability",
                GetName(), GetGUID().ToString(), sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) / 100.0f);
            DurabilityLossAll(sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) / 100.0f, false);
            // durability lost message
            SendDurabilityLoss();
        }

        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DEATHS_FROM, 1, type);
    }

    return final_damage;
}

void Player::setDeathState(DeathState s, bool /*despawn = false*/)
{
    uint32 ressSpellId = 0;

    bool cur = IsAlive();

    if (s == DeathState::JustDied)
    {
        if (!cur)
        {
            LOG_ERROR("entities.player", "setDeathState: attempt to kill a dead player {} ({})", GetName(), GetGUID().ToString());
            return;
        }

        // clear all pending spell cast requests when dying
        SpellQueue.clear();

        // drunken state is cleared on death
        SetDrunkValue(0);
        // lost combo points at any target (targeted combo points clear in Unit::setDeathState)
        ClearComboPoints();

        clearResurrectRequestData();

        //FIXME: is pet dismissed at dying or releasing spirit? if second, add setDeathState(DeathState::Dead) to HandleRepopRequestOpcode and define pet unsummon here with (s == DEAD)
        RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);

        // save value before aura remove in Unit::setDeathState
        ressSpellId = GetUInt32Value(PLAYER_SELF_RES_SPELL);

        // xinef: disable passive area auras!
        AddUnitState(UNIT_STATE_ISOLATED);

        // passive spell
        if (!ressSpellId)
            ressSpellId = GetResurrectionSpellId();
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DEATH_AT_MAP, 1);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DEATH, 1);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DEATH_IN_DUNGEON, 1);

        FailQuestsOnDeath();

        // Xinef: reset all death criterias
        ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_DEATH, 0);
    }
    // xinef: enable passive area auras!
    else if (s == DeathState::Alive)
        ClearUnitState(UNIT_STATE_ISOLATED);

    Unit::setDeathState(s);

    if (NeedSendSpectatorData())
        ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "STA", IsAlive() ? 1 : 0);

    // restore resurrection spell id for player after aura remove
    if (s == DeathState::JustDied && cur && ressSpellId)
        SetUInt32Value(PLAYER_SELF_RES_SPELL, ressSpellId);

    if (IsAlive() && !cur)
        //clear aura case after resurrection by another way (spells will be applied before next death)
        SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
}

void Player::SetRestState(uint32 triggerId)
{
    _innTriggerId = triggerId;
    _restTime = GameTime::GetGameTime().count();
    SetPlayerFlag(PLAYER_FLAGS_RESTING);
}

void Player::RemoveRestState()
{
    _innTriggerId = 0;
    _restTime = 0;
    RemovePlayerFlag(PLAYER_FLAGS_RESTING);
}

void Player::RegenerateAll()
{
    //if (m_regenTimer <= 500)
    //    return;

    m_regenTimerCount += m_regenTimer;
    m_foodEmoteTimerCount += m_regenTimer;

    Regenerate(POWER_ENERGY);

    Regenerate(POWER_MANA);

    // Runes act as cooldowns, and they don't need to send any data
    if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_ABILITY))
        for (uint8 i = 0; i < MAX_RUNES; ++i)
        {
            // xinef: implement grace
            if (int32 cd = GetRuneCooldown(i))
            {
                SetRuneCooldown(i, (cd > m_regenTimer) ? cd - m_regenTimer : 0);
                // start grace counter, player must be in combat and rune has to go off cooldown
                if (IsInCombat() && cd <= m_regenTimer)
                    SetGracePeriod(i, m_regenTimer - cd + 1); // added 1 because m_regenTimer-cd can be equal 0
            }
            // xinef: if grace is started, increase it but no more than cap
            else if (uint32 grace = GetGracePeriod(i))
            {
                if (grace < RUNE_GRACE_PERIOD)
                    SetGracePeriod(i, std::min<uint32>(grace + m_regenTimer, RUNE_GRACE_PERIOD));
            }
        }

    if (m_regenTimerCount >= 2000)
    {
        // Not in combat or they have regeneration
        if (!IsInCombat() || IsPolymorphed() || m_baseHealthRegen ||
                HasRegenDuringCombatAura() ||
                HasHealthRegenInCombatAura())
        {
            RegenerateHealth();
        }

        Regenerate(POWER_RAGE);
        if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_ABILITY))
            Regenerate(POWER_RUNIC_POWER);

        m_regenTimerCount -= 2000;
    }

    m_regenTimer = 0;

    // Handles the emotes for drinking and eating.
    // According to sniffs there is a background timer going on that repeats independed from the time window where the aura applies.
    // That's why we dont need to reset the timer on apply. In sniffs I have seen that the first call for the spell visual is totally random, then after
    // 5 seconds over and over again which confirms my theory that we have a independed timer.
    if (m_foodEmoteTimerCount >= 5000)
    {
        std::vector<AuraEffect*> auraList;
        AuraEffectList const& ModRegenAuras = GetAuraEffectsByType(SPELL_AURA_MOD_REGEN);
        AuraEffectList const& ModPowerRegenAuras = GetAuraEffectsByType(SPELL_AURA_MOD_POWER_REGEN);

        auraList.reserve(ModRegenAuras.size() + ModPowerRegenAuras.size());
        auraList.insert(auraList.end(), ModRegenAuras.begin(), ModRegenAuras.end());
        auraList.insert(auraList.end(), ModPowerRegenAuras.begin(), ModPowerRegenAuras.end());

        for (auto itr = auraList.begin(); itr != auraList.end(); ++itr)
        {
            // Food emote comes above drinking emote if we have to decide (mage regen food for example)
            if ((*itr)->GetBase()->HasEffectType(SPELL_AURA_MOD_REGEN) && (*itr)->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED)
            {
                SendPlaySpellVisual(SPELL_VISUAL_KIT_FOOD);
                break;
            }
            else if ((*itr)->GetBase()->HasEffectType(SPELL_AURA_MOD_POWER_REGEN) && (*itr)->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED)
            {
                SendPlaySpellVisual(SPELL_VISUAL_KIT_DRINK);
                break;
            }
        }
        m_foodEmoteTimerCount -= 5000;
    }
}

void Player::Regenerate(Powers power)
{
    uint32 maxValue = GetMaxPower(power);
    if (!maxValue)
        return;

    //If .cheat power is on always have the max power
    if (GetCommandStatus(CHEAT_POWER))
    {
        if (m_regenTimerCount >= 2000)
        {
            //Set the value to 0 first then set it to max to force resend of packet as for range clients keeps removing rage
            if (power == POWER_RAGE || power == POWER_RUNIC_POWER)
            {
                UpdateUInt32Value(static_cast<uint16>(UNIT_FIELD_POWER1) + power, 0);
            }

            SetPower(power, maxValue);
            return;
        }
    }

    uint32 curValue = GetPower(power);

    /// @todo: possible use of miscvalueb instead of amount
    if (HasAuraTypeWithMiscvalue(SPELL_AURA_PREVENT_REGENERATE_POWER, power + 1))
        return;

    float addvalue = 0.0f;

    switch (power)
    {
        case POWER_MANA:
            {
                bool recentCast = IsUnderLastManaUseEffect();
                float ManaIncreaseRate = sWorld->getRate(RATE_POWER_MANA);

                if (sWorld->getBoolConfig(CONFIG_LOW_LEVEL_REGEN_BOOST) && GetLevel() < 15)
                    ManaIncreaseRate = sWorld->getRate(RATE_POWER_MANA) * (2.066f - (GetLevel() * 0.066f));

                if (recentCast) // Trinity Updates Mana in intervals of 2s, which is correct
                    addvalue += GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER + AsUnderlyingType(POWER_MANA)) *  ManaIncreaseRate * 0.001f * m_regenTimer;
                else
                    addvalue += GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + AsUnderlyingType(POWER_MANA)) * ManaIncreaseRate * 0.001f * m_regenTimer;
            }
            break;
        case POWER_RAGE:                                    // Regenerate rage
            {
                if (!IsInCombat() && !HasInterruptRegenAura())
                {
                    float RageDecreaseRate = sWorld->getRate(RATE_POWER_RAGE_LOSS);
                    addvalue += -20 * RageDecreaseRate;               // 2 rage by tick (= 2 seconds => 1 rage/sec)
                }
            }
            break;
        case POWER_ENERGY:                                  // Regenerate energy (rogue)
            // Regen per second
            addvalue += (GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + AsUnderlyingType(POWER_ENERGY)) + 10.f);
            // Regen per millisecond
            addvalue *= 0.001f;
            // Milliseconds passed
            addvalue *= m_regenTimer;
            // Rate
            addvalue *= sWorld->getRate(RATE_POWER_ENERGY);
            break;
        case POWER_RUNIC_POWER:
            {
                if (!IsInCombat() && !HasInterruptRegenAura())
                {
                    float RunicPowerDecreaseRate = sWorld->getRate(RATE_POWER_RUNICPOWER_LOSS);
                    addvalue += -30 * RunicPowerDecreaseRate;         // 3 RunicPower by tick
                }
            }
            break;
        case POWER_RUNE:
        case POWER_FOCUS:
        case POWER_HAPPINESS:
            break;
        case POWER_HEALTH:
            return;
        default:
            break;
    }

    // Mana regen calculated in Player::UpdateManaRegen(), energy regen calculated in Player::UpdateEnergyRegen()
    if (power != POWER_MANA && power != POWER_ENERGY)
    {
        addvalue *= GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, power);

        // Butchery requires combat for this effect
        if (power != POWER_RUNIC_POWER || IsInCombat())
            addvalue += float(GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_POWER_REGEN, power) * ((power != POWER_ENERGY) ? m_regenTimerCount : m_regenTimer)) / (5.0f * IN_MILLISECONDS);
    }

    if (addvalue < 0.0f)
    {
        if (curValue == 0)
            return;
    }
    else if (addvalue > 0.0f)
    {
        if (curValue == maxValue)
            return;
    }
    else
        return;

    addvalue += m_powerFraction[power];
    uint32 integerValue = uint32(std::fabs(addvalue));

    if (addvalue < 0.0f)
    {
        if (curValue > integerValue)
        {
            curValue -= integerValue;
            m_powerFraction[power] = addvalue + integerValue;
        }
        else
        {
            curValue = 0;
            m_powerFraction[power] = 0;
        }
    }
    else
    {
        curValue += integerValue;

        if (curValue >= maxValue)
        {
            curValue = maxValue;
            m_powerFraction[power] = 0;
        }
        else
            m_powerFraction[power] = addvalue - integerValue;
    }

    if (m_regenTimerCount >= 2000 || curValue == 0 || curValue == maxValue)
        SetPower(power, curValue, true, true);
    else
        UpdateUInt32Value(UNIT_FIELD_POWER1 + AsUnderlyingType(power), curValue);
}

void Player::RegenerateHealth()
{
    uint32 curValue = GetHealth();
    uint32 maxValue = GetMaxHealth();

    if (curValue >= maxValue)
        return;

    float HealthIncreaseRate = sWorld->getRate(RATE_HEALTH);

    if (sWorld->getBoolConfig(CONFIG_LOW_LEVEL_REGEN_BOOST) && GetLevel() < 15)
        HealthIncreaseRate = sWorld->getRate(RATE_HEALTH) * (2.066f - (GetLevel() * 0.066f));

    float addvalue = 0.0f;

    // polymorphed case
    if (IsPolymorphed())
        addvalue = (float)GetMaxHealth() / 3;
    // normal regen case (maybe partly in combat case)
    else if (!IsInCombat() || HasRegenDuringCombatAura())
    {
        addvalue = OCTRegenHPPerSpirit() * HealthIncreaseRate;

        if (!IsStandState())
        {
            addvalue *= 1.33f;
        }

        addvalue *= GetTotalAuraMultiplier(SPELL_AURA_MOD_HEALTH_REGEN_PERCENT);

        if (!IsInCombat())
        {
            addvalue += GetTotalAuraModifier(SPELL_AURA_MOD_REGEN) * 2 * IN_MILLISECONDS / (5 * IN_MILLISECONDS);
        }
        else if (HasRegenDuringCombatAura())
        {
            ApplyPct(addvalue, GetTotalAuraModifier(SPELL_AURA_MOD_REGEN_DURING_COMBAT));
        }
    }

    // always regeneration bonus (including combat)
    addvalue += GetTotalAuraModifier(SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT);
    addvalue += m_baseHealthRegen / 2.5f;

    if (addvalue < 0)
        addvalue = 0;

    ModifyHealth(int32(addvalue));
}

void Player::ResetAllPowers()
{
    SetHealth(GetMaxHealth());
    if (HasActivePowerType(POWER_MANA))
    {
        SetPower(POWER_MANA, GetMaxPower(POWER_MANA));
    }
    if (HasActivePowerType(POWER_RAGE))
    {
        SetPower(POWER_RAGE, 0);
    }
    if (HasActivePowerType(POWER_ENERGY))
    {
        SetPower(POWER_ENERGY, GetMaxPower(POWER_ENERGY));
    }
    if (HasActivePowerType(POWER_RUNIC_POWER))
    {
        SetPower(POWER_RUNIC_POWER, 0);
    }
}

void Player::InitStatsForLevel(bool reapplyMods)
{
    if (reapplyMods)                                        //reapply stats values only on .reset stats (level) command
        _RemoveAllStatBonuses();

    PlayerClassLevelInfo classInfo;
    sObjectMgr->GetPlayerClassLevelInfo(getClass(), GetLevel(), &classInfo);

    PlayerLevelInfo info;
    sObjectMgr->GetPlayerLevelInfo(getRace(true), getClass(), GetLevel(), &info);

    uint32 maxPlayerLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    sScriptMgr->OnPlayerSetMaxLevel(this, maxPlayerLevel);
    SetUInt32Value(PLAYER_FIELD_MAX_LEVEL, maxPlayerLevel);
    SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(GetLevel()));

    // reset before any aura state sources (health set/aura apply)
    SetUInt32Value(UNIT_FIELD_AURASTATE, 0);

    UpdateSkillsForLevel();

    // set default cast time multiplier
    SetFloatValue(UNIT_MOD_CAST_SPEED, 1.0f);

    // reset size before reapply auras
    SetObjectScale(1.0f);

    // save base values (bonuses already included in stored stats
    for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
        SetCreateStat(Stats(i), info.stats[i]);

    for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
        SetStat(Stats(i), info.stats[i]);

    SetCreateHealth(classInfo.basehealth);

    //set create powers
    SetCreateMana(classInfo.basemana);

    SetArmor(int32(m_createStats[STAT_AGILITY] * 2));

    InitStatBuffMods();

    //reset rating fields values
    for (uint16 index = PLAYER_FIELD_COMBAT_RATING_1; index < PLAYER_FIELD_COMBAT_RATING_1 + MAX_COMBAT_RATING; ++index)
        SetUInt32Value(index, 0);

    SetUInt32Value(PLAYER_FIELD_MOD_HEALING_DONE_POS, 0);
    for (uint8 i = 0; i < 7; ++i)
    {
        SetInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_NEG + i, 0);
        SetInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + i, 0);
        SetFloatValue(PLAYER_FIELD_MOD_DAMAGE_DONE_PCT + i, 1.00f);
    }

    //reset attack power, damage and attack speed fields
    SetFloatValue(UNIT_FIELD_BASEATTACKTIME, 2000.0f);
    SetFloatValue(UNIT_FIELD_BASEATTACKTIME + 1, 2000.0f); // offhand attack time
    SetFloatValue(UNIT_FIELD_RANGEDATTACKTIME, 2000.0f);

    SetFloatValue(UNIT_FIELD_MINDAMAGE, 0.0f);
    SetFloatValue(UNIT_FIELD_MAXDAMAGE, 0.0f);
    SetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE, 0.0f);
    SetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE, 0.0f);
    SetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, 0.0f);
    SetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, 0.0f);

    SetInt32Value(UNIT_FIELD_ATTACK_POWER,            0);
    SetInt32Value(UNIT_FIELD_ATTACK_POWER_MODS,       0);
    SetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER, 0.0f);
    SetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER,     0);
    SetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS, 0);
    SetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER, 0.0f);

    // Base crit values (will be recalculated in UpdateAllStats() at loading and in _ApplyAllStatBonuses() at reset
    SetFloatValue(PLAYER_CRIT_PERCENTAGE, 0.0f);
    SetFloatValue(PLAYER_OFFHAND_CRIT_PERCENTAGE, 0.0f);
    SetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE, 0.0f);

    // Init spell schools (will be recalculated in UpdateAllStats() at loading and in _ApplyAllStatBonuses() at reset
    for (uint8 i = 0; i < 7; ++i)
        SetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + i, 0.0f);

    SetFloatValue(PLAYER_PARRY_PERCENTAGE, 0.0f);
    SetFloatValue(PLAYER_BLOCK_PERCENTAGE, 0.0f);
    SetUInt32Value(PLAYER_SHIELD_BLOCK, 0);

    // Dodge percentage
    SetFloatValue(PLAYER_DODGE_PERCENTAGE, 0.0f);

    // set armor (resistance 0) to original value (create_agility*2)
    SetArmor(int32(m_createStats[STAT_AGILITY] * 2));
    SetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + AsUnderlyingType(SPELL_SCHOOL_NORMAL), 0.0f);
    SetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + AsUnderlyingType(SPELL_SCHOOL_NORMAL), 0.0f);
    // set other resistance to original value (0)
    for (uint8 i = 1; i < MAX_SPELL_SCHOOL; ++i)
    {
        SetResistance(SpellSchools(i), 0);
        SetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + i, 0.0f);
        SetFloatValue(UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + i, 0.0f);
    }

    SetUInt32Value(PLAYER_FIELD_MOD_TARGET_RESISTANCE, 0);
    SetUInt32Value(PLAYER_FIELD_MOD_TARGET_PHYSICAL_RESISTANCE, 0);
    for (uint8 i = 0; i < MAX_SPELL_SCHOOL; ++i)
    {
        SetUInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + i, 0);
        SetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + i, 0.0f);
    }
    // Reset no reagent cost field
    for (uint8 i = 0; i < 3; ++i)
        SetUInt32Value(PLAYER_NO_REAGENT_COST_1 + i, 0);
    // Init data for form but skip reapply item mods for form
    InitDataForForm(reapplyMods);

    // save new stats
    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        SetMaxPower(Powers(i),  uint32(GetCreatePowers(Powers(i))));

    SetMaxHealth(classInfo.basehealth);                     // stamina bonus will applied later

    // cleanup mounted state (it will set correctly at aura loading if player saved at mount.
    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);

    // cleanup unit flags (will be re-applied if need at aura load).
    RemoveFlag(UNIT_FIELD_FLAGS,
               UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_DISABLE_MOVE  | UNIT_FLAG_NOT_ATTACKABLE_1 |
               UNIT_FLAG_LOOTING        | UNIT_FLAG_PET_IN_COMBAT | UNIT_FLAG_SILENCED         |
               UNIT_FLAG_PACIFIED       | UNIT_FLAG_STUNNED       | UNIT_FLAG_IN_COMBAT        |
               UNIT_FLAG_DISARMED       | UNIT_FLAG_CONFUSED      | UNIT_FLAG_FLEEING          |
               UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_SKINNABLE     | UNIT_FLAG_MOUNT            |
               UNIT_FLAG_TAXI_FLIGHT);
    SetImmuneToAll(false);
    SetUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED);   // must be set

    SetUnitFlag2(UNIT_FLAG2_REGENERATE_POWER);// must be set

    // cleanup player flags (will be re-applied if need at aura load), to avoid have ghost flag without ghost aura, for example.
    RemovePlayerFlag(PLAYER_FLAGS_AFK | PLAYER_FLAGS_DND | PLAYER_FLAGS_GM | PLAYER_FLAGS_GHOST | PLAYER_ALLOW_ONLY_ABILITY);

    RemoveStandFlags(UNIT_STAND_FLAGS_ALL);                 // one form stealth modified bytes
    if (HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
    {
        RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP | UNIT_BYTE2_FLAG_SANCTUARY);
        sScriptMgr->OnPlayerFfaPvpStateUpdate(this, false);

    }
    // restore if need some important flags
    SetUInt32Value(PLAYER_FIELD_BYTES2, 0);                 // flags empty by default

    if (reapplyMods)                                        // reapply stats values only on .reset stats (level) command
        _ApplyAllStatBonuses();

    // set current level health and mana/energy to maximum after applying all mods.
    SetFullHealth();
    SetPower(POWER_MANA, GetMaxPower(POWER_MANA));
    SetPower(POWER_ENERGY, GetMaxPower(POWER_ENERGY));
    if (GetPower(POWER_RAGE) > GetMaxPower(POWER_RAGE))
        SetPower(POWER_RAGE, GetMaxPower(POWER_RAGE));
    SetPower(POWER_FOCUS, 0);
    SetPower(POWER_HAPPINESS, 0);
    SetPower(POWER_RUNIC_POWER, 0);

    // update level to hunter/summon pet
    if (Pet* pet = GetPet())
        pet->SynchronizeLevelWithOwner();
}

bool Player::HasActivePowerType(Powers power)
{
    if (sScriptMgr->OnPlayerHasActivePowerType(this, power))
        return true;
    else
        return (getPowerType() == power);
}

void Player::BuildCreateUpdateBlockForPlayer(UpdateData* data, Player* target)
{
    if (target == this)
    {
        for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i)
        {
            if (!m_items[i])
                continue;

            m_items[i]->BuildCreateUpdateBlockForPlayer(data, target);
        }

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (!m_items[i])
                continue;

            m_items[i]->BuildCreateUpdateBlockForPlayer(data, target);
        }
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
        {
            if (!m_items[i])
                continue;

            m_items[i]->BuildCreateUpdateBlockForPlayer(data, target);
        }
    }

    Unit::BuildCreateUpdateBlockForPlayer(data, target);
}

void Player::UpdateDamageDoneMods(WeaponAttackType attackType, int32 skipEnchantSlot /*= -1*/)
{
    Unit::UpdateDamageDoneMods(attackType, skipEnchantSlot);

    UnitMods unitMod;
    switch (attackType)
    {
        case BASE_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_MAINHAND;
            break;
        case OFF_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_OFFHAND;
            break;
        case RANGED_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_RANGED;
            break;
        default:
            ABORT();
            break;
    }

    float amount = 0.0f;
    Item* item = GetWeaponForAttack(attackType, true);
    if (!item)
        return;

    for (uint8 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
    {
        if (skipEnchantSlot == slot)
            continue;

        SpellItemEnchantmentEntry const* enchantmentEntry = sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(EnchantmentSlot(slot)));
        if (!enchantmentEntry)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            switch (enchantmentEntry->type[i])
            {
                case ITEM_ENCHANTMENT_TYPE_DAMAGE:
                    amount += enchantmentEntry->amount[i];
                    break;
                case ITEM_ENCHANTMENT_TYPE_TOTEM:
                    if (IsClass(CLASS_SHAMAN, CLASS_CONTEXT_ABILITY))
                        amount += enchantmentEntry->amount[i] * item->GetTemplate()->Delay / 1000.0f;
                    break;
                default:
                    break;
            }
        }
    }

    HandleStatFlatModifier(unitMod, TOTAL_VALUE, amount, true);
}

uint32 Player::GetShieldBlockValue() const
{
    float value = (m_auraBaseFlatMod[SHIELD_BLOCK_VALUE] + GetStat(STAT_STRENGTH) * 0.5f - 10) * m_auraBasePctMod[SHIELD_BLOCK_VALUE];

    value = (value < 0) ? 0 : value;

    return uint32(value);
}

float Player::GetMeleeCritFromAgility()
{
    uint8 level = GetLevel();
    uint32 pclass = getClass();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    GtChanceToMeleeCritBaseEntry const* critBase  = sGtChanceToMeleeCritBaseStore.LookupEntry(pclass - 1);
    GtChanceToMeleeCritEntry     const* critRatio = sGtChanceToMeleeCritStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    if (!critBase || !critRatio)
        return 0.0f;

    float crit = critBase->base + GetStat(STAT_AGILITY) * critRatio->ratio;
    return crit * 100.0f;
}

void Player::GetDodgeFromAgility(float& diminishing, float& nondiminishing)
{
    // Table for base dodge values
    const float dodge_base[MAX_CLASSES] =
    {
        0.036640f, // Warrior
        0.034943f, // Paladi
        -0.040873f, // Hunter
        0.020957f, // Rogue
        0.034178f, // Priest
        0.036640f, // DK
        0.021080f, // Shaman
        0.036587f, // Mage
        0.024211f, // Warlock
        0.0f,      // ??
        0.056097f  // Druid
    };
    // Crit/agility to dodge/agility coefficient multipliers; 3.2.0 increased required agility by 15%
    const float crit_to_dodge[MAX_CLASSES] =
    {
        0.85f / 1.15f,  // Warrior
        1.00f / 1.15f,  // Paladin
        1.11f / 1.15f,  // Hunter
        2.00f / 1.15f,  // Rogue
        1.00f / 1.15f,  // Priest
        0.85f / 1.15f,  // DK
        1.60f / 1.15f,  // Shaman
        1.00f / 1.15f,  // Mage
        0.97f / 1.15f,  // Warlock (?)
        0.0f,           // ??
        2.00f / 1.15f   // Druid
    };

    uint8 level = GetLevel();
    uint32 pclass = getClass();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    // Dodge per agility is proportional to crit per agility, which is available from DBC files
    GtChanceToMeleeCritEntry  const* dodgeRatio = sGtChanceToMeleeCritStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    if (!dodgeRatio || pclass > MAX_CLASSES)
        return;

    /// @todo: research if talents/effects that increase total agility by x% should increase non-diminishing part
    float base_agility = GetCreateStat(STAT_AGILITY) * GetPctModifierValue(UnitMods(UNIT_MOD_STAT_START + AsUnderlyingType(STAT_AGILITY)), BASE_PCT);
    float bonus_agility = GetStat(STAT_AGILITY) - base_agility;

    // calculate diminishing (green in char screen) and non-diminishing (white) contribution
    diminishing = 100.0f * bonus_agility * dodgeRatio->ratio * crit_to_dodge[pclass - 1];
    nondiminishing = 100.0f * (dodge_base[pclass - 1] + base_agility * dodgeRatio->ratio * crit_to_dodge[pclass - 1]);
}

float Player::GetRatingMultiplier(CombatRating cr) const
{
    uint8 level = GetLevel();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    GtCombatRatingsEntry const* Rating = sGtCombatRatingsStore.LookupEntry(cr * GT_MAX_LEVEL + level - 1);
    // gtOCTClassCombatRatingScalarStore.dbc starts with 1, CombatRating with zero, so cr+1
    GtOCTClassCombatRatingScalarEntry const* classRating = sGtOCTClassCombatRatingScalarStore.LookupEntry((getClass() - 1) * GT_MAX_RATING + cr + 1);
    if (!Rating || !classRating)
        return 1.0f;                                        // By default use minimum coefficient (not must be called)

    return classRating->ratio / Rating->ratio;
}

float Player::GetRatingBonusValue(CombatRating cr) const
{
    return float(GetUInt32Value(static_cast<uint16>(PLAYER_FIELD_COMBAT_RATING_1) + cr)) * GetRatingMultiplier(cr);
}

float Player::GetExpertiseDodgeOrParryReduction(WeaponAttackType attType) const
{
    switch (attType)
    {
        case BASE_ATTACK:
            return m_Expertise / 4.0f;
        case OFF_ATTACK:
            return m_OffhandExpertise / 4.0f;
        default:
            break;
    }
    return 0.0f;
}

float Player::OCTRegenHPPerSpirit()
{
    uint8 level = GetLevel();
    uint32 pclass = getClass();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    GtOCTRegenHPEntry     const* baseRatio = sGtOCTRegenHPStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    GtRegenHPPerSptEntry  const* moreRatio = sGtRegenHPPerSptStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    if (!baseRatio || !moreRatio)
        return 0.0f;

    // Formula from PaperDollFrame script
    float spirit = GetStat(STAT_SPIRIT);
    float baseSpirit = spirit;
    if (baseSpirit > 50)
        baseSpirit = 50;
    float moreSpirit = spirit - baseSpirit;
    float regen = (baseSpirit * baseRatio->ratio + moreSpirit * moreRatio->ratio) * 2;
    return regen;
}

float Player::OCTRegenMPPerSpirit()
{
    uint8 level = GetLevel();
    uint32 pclass = getClass();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    //    GtOCTRegenMPEntry     const* baseRatio = sGtOCTRegenMPStore.LookupEntry((pclass-1)*GT_MAX_LEVEL + level-1);
    GtRegenMPPerSptEntry  const* moreRatio = sGtRegenMPPerSptStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    if (!moreRatio)
        return 0.0f;

    // Formula get from PaperDollFrame script
    float spirit    = GetStat(STAT_SPIRIT);
    float regen     = spirit * moreRatio->ratio;
    return regen;
}

void Player::ApplyRatingMod(CombatRating cr, int32 value, bool apply)
{
    float oldRating = m_baseRatingValue[cr];
    m_baseRatingValue[cr] += (apply ? value : -value);
    // explicit affected values
    if (cr == CR_HASTE_MELEE || cr == CR_HASTE_RANGED || cr == CR_HASTE_SPELL)
    {
        float const mult = GetRatingMultiplier(cr);
        float const oldVal = oldRating * mult;
        float const newVal = m_baseRatingValue[cr] * mult;
        switch (cr)
        {
            case CR_HASTE_MELEE:
                ApplyAttackTimePercentMod(BASE_ATTACK, oldVal, false);
                ApplyAttackTimePercentMod(OFF_ATTACK, oldVal, false);
                ApplyAttackTimePercentMod(BASE_ATTACK, newVal, true);
                ApplyAttackTimePercentMod(OFF_ATTACK, newVal, true);
                break;
            case CR_HASTE_RANGED:
                ApplyAttackTimePercentMod(RANGED_ATTACK, oldVal, false);
                ApplyAttackTimePercentMod(RANGED_ATTACK, newVal, true);
                break;
            case CR_HASTE_SPELL:
                ApplyCastTimePercentMod(oldVal, false);
                ApplyCastTimePercentMod(newVal, true);
                break;
            default:
                break;
        }
    }

    UpdateRating(cr);
}

void Player::SetRegularAttackTime()
{
    for (uint8 i = 0; i < MAX_ATTACK; ++i)
    {
        Item* tmpitem = GetWeaponForAttack(WeaponAttackType(i), true);
        if (tmpitem && !tmpitem->IsBroken())
        {
            ItemTemplate const* proto = tmpitem->GetTemplate();
            if (proto->Delay)
                SetAttackTime(WeaponAttackType(i), proto->Delay);
        }
        else
            SetAttackTime(WeaponAttackType(i), BASE_ATTACK_TIME);  // If there is no weapon reset attack time to base (might have been changed from forms)
    }
}

void Player::_ApplyWeaponDamage(uint8 slot, ItemTemplate const* proto, ScalingStatValuesEntry const* ssv, bool apply)
{
    uint32 CustomScalingStatValue = 0;

    sScriptMgr->OnPlayerCustomScalingStatValueBefore(this, proto, slot, apply, CustomScalingStatValue);

    uint32 ScalingStatValue = proto->ScalingStatValue > 0 ? proto->ScalingStatValue : CustomScalingStatValue;

    // following part fix disarm issue
    // that doesn't apply the scaling after disarmed
    if (!ssv)
    {
        ScalingStatDistributionEntry const* ssd = proto->ScalingStatDistribution ? sScalingStatDistributionStore.LookupEntry(proto->ScalingStatDistribution) : nullptr;

        // req. check at equip, but allow use for extended range if range limit max level, set proper level
        uint32 ssd_level = GetLevel();

        if (ssd && ssd_level > ssd->MaxLevel)
            ssd_level = ssd->MaxLevel;

        ssv = ScalingStatValue ? sScalingStatValuesStore.LookupEntry(ssd_level) : nullptr;
    }

    WeaponAttackType attType = Player::GetAttackBySlot(slot);
    if (!IsInFeralForm() && apply && !CanUseAttackType(attType))
    {
        return;
    }

    for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
    {
        float minDamage = proto->Damage[i].DamageMin;
        float maxDamage = proto->Damage[i].DamageMax;

        // If set dpsMod in ScalingStatValue use it for min (70% from average), max (130% from average) damage
        if (ssv && i == 0) // scaling stats only for first damage
        {
            int32 extraDPS = ssv->getDPSMod(ScalingStatValue);
            if (extraDPS)
            {
                float average = extraDPS * proto->Delay / 1000.0f;
                float mod = ssv->IsTwoHand(proto->ScalingStatValue) ? 0.2f : 0.3f;

                minDamage = (1.0f - mod) * average;
                maxDamage = (1.0f + mod) * average;
            }
        }

        if (apply)
        {
            sScriptMgr->OnPlayerApplyWeaponDamage(this, slot, proto, minDamage, maxDamage, i);

            if (minDamage > 0.f)
            {
                SetBaseWeaponDamage(attType, MINDAMAGE, minDamage, i);
            }

            if (maxDamage > 0.f)
            {
                SetBaseWeaponDamage(attType, MAXDAMAGE, maxDamage, i);
            }
        }
    }

    if (!apply)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
        {
            SetBaseWeaponDamage(attType, MINDAMAGE, 0.f, i);
            SetBaseWeaponDamage(attType, MAXDAMAGE, 0.f, i);
        }

        if (attType == BASE_ATTACK)
        {
            SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, BASE_MINDAMAGE);
            SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, BASE_MAXDAMAGE);
        }
    }

    if (proto->Delay && !IsInFeralForm())
    {
        if (slot == EQUIPMENT_SLOT_RANGED)
            SetAttackTime(RANGED_ATTACK, apply ? proto->Delay : BASE_ATTACK_TIME);
        else if (slot == EQUIPMENT_SLOT_MAINHAND)
            SetAttackTime(BASE_ATTACK, apply ? proto->Delay : BASE_ATTACK_TIME);
        else if (slot == EQUIPMENT_SLOT_OFFHAND)
            SetAttackTime(OFF_ATTACK, apply ? proto->Delay : BASE_ATTACK_TIME);
    }

    // No need to modify any physical damage for ferals as it is calculated from stats only
    if (IsInFeralForm())
        return;

    if (CanModifyStats() && (GetWeaponDamageRange(attType, MAXDAMAGE) || proto->Delay))
        UpdateDamagePhysical(attType);
}

SpellSchoolMask Player::GetMeleeDamageSchoolMask(WeaponAttackType attackType /*= BASE_ATTACK*/, uint8 damageIndex /*= 0*/) const
{
    if (Item const* weapon = GetWeaponForAttack(attackType, true))
    {
        return SpellSchoolMask(1 << weapon->GetTemplate()->Damage[damageIndex].DamageType);
    }

    return SPELL_SCHOOL_MASK_NORMAL;
}

void Player::SendInitWorldStates(uint32 zoneId, uint32 areaId)
{
    // data depends on zoneid/mapid...
    uint32 mapId = GetMapId();
    Battleground* battleground = GetBattleground();
    OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
    InstanceScript* instance = GetInstanceScript();
    Battlefield* battlefield = sBattlefieldMgr->GetBattlefieldToZoneId(zoneId);

    LOG_DEBUG("network", "Sending SMSG_INIT_WORLD_STATES to Map: {}, Zone: {}", mapId, zoneId);

    WorldPackets::WorldState::InitWorldStates packet;
    packet.MapID = mapId;
    packet.ZoneID = zoneId;
    packet.AreaID = areaId;

    packet.Worldstates.reserve(8);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_WINTERSPRING, 0);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_AZSHARA, 0);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_BLASTED_LANDS, 0);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_BURNING_STEPPES, 0);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_TANARIS, 0);
    packet.Worldstates.emplace_back(WORLD_STATE_SCOURGE_INVASION_EASTERN_PLAGUELANDS, 0);

    // 7 1 - Arena season in progress, 0 - end of season
    packet.Worldstates.emplace_back(WORLD_STATE_ARENA_SEASON_PROGRESS, sArenaSeasonMgr->GetSeasonState() == ARENA_SEASON_STATE_IN_PROGRESS);
    // 8 Arena season id
    packet.Worldstates.emplace_back(WORLD_STATE_ARENA_SEASON_ID, sArenaSeasonMgr->GetCurrentSeason());

    if (mapId == MAP_OUTLAND)
    {
        packet.Worldstates.reserve(3);
        packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_TOWER_SLIDER_DISPLAY, 0);
        packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_GUARDS_MAX, 15);
        packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_GUARDS_LEFT, 15);
    }

    if (Player::bgZoneIdToFillWorldStates.find(zoneId) != Player::bgZoneIdToFillWorldStates.end())
    {
        Player::bgZoneIdToFillWorldStates[zoneId](battleground, packet);
    }
    else
    {
        // insert <field> <value>
        switch (zoneId)
        {
            case AREA_DUN_MOROGH:
            case AREA_WETLANDS:
            case AREA_ELWYNN_FOREST:
            case AREA_LOCH_MODAN:
            case AREA_WESTFALL:
            case AREA_SEARING_GORGE:
            case AREA_STORMWIND_CITY:
            case AREA_IRONFORGE:
            case AREA_DEEPRUN_TRAM:
            case AREA_SHATTRATH_CITY:
                break;
            case AREA_EASTERN_PLAGUELANDS:
                if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_EP)
                    outdoorPvP->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(32);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UI_TOWER_SLIDER_DISPLAY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UI_TOWER_COUNT_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UI_TOWER_COUNT_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UI_TOWER_SLIDER_POS, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UI_TOWER_SLIDER_N, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_CROWNGUARDTOWER_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_CROWNGUARDTOWER_N_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_CROWNGUARDTOWER_N_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_7, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_8, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_CROWNGUARDTOWER_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_CROWNGUARDTOWER_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_EASTWALLTOWER_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_EASTWALLTOWER_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_0, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_1, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_EASTWALLTOWER_N_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_EASTWALLTOWER_N_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_EASTWALLTOWER_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_NORTHPASSTOWER_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_NORTHPASSTOWER_N_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_NORTHPASSTOWER_N_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_2, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_3, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_NORTHPASSTOWER_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_NORTHPASSTOWER_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_PLAGUEWOODTOWER_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_PLAGUEWOODTOWER_N_A, 0);
                    //packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_4, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_5, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_UNK_6, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_PLAGUEWOODTOWER_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_EP_PLAGUEWOODTOWER_H, 0);

                break;
            case AREA_SILITHUS:
                if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_SI)
                    outdoorPvP->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_SI_GATHERED_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_SI_GATHERED_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_SI_SILITHYST_MAX, 0);
                }
                // unknown, aq opening?
                packet.Worldstates.reserve(4);
                packet.Worldstates.emplace_back(WORLD_STATE_AHNQIRAJ_SANDWORM_N, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_AHNQIRAJ_SANDWORM_S, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_AHNQIRAJ_SANDWORM_SW, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_AHNQIRAJ_SANDWORM_E, 0);
                break;
            case AREA_ALTERAC_VALLEY:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_AV)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(75);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_SNOWFALL_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFHUT_H_C, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFHUT_A_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_AID_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFE_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFW_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFE_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFW_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_N_MINE_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_PIKEGRAVE_H_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_PIKEGRAVE_A_C, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEHEART_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEHEART_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_5, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_TOWERPOINT_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_4, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_TOWERPOINT_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFE_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFW_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_3, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_TOWERPOINT_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEH_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEWING_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNN_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNS_ASSAULTED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEH_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEWING_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNS_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNN_UNUSED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEH_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_1, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_0, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STORMPIKE_COMMANDERS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEHEART_A_C, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEHEART_H_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STORMPIKE_LIEUTENANTS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEWING_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNN_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNS_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_UNK_2, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_TOWERPOINT_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFE_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFW_DESTROYED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_STONEH_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEWING_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNN_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_DUNS_CONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_N_MINE_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_N_MINE_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_S_MINE_N, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_S_MINE_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_S_MINE_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_H_C, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_ICEBLOOD_A_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_SNOWFALL_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_SNOWFALL_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_SNOWFALL_H_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_SNOWFALL_A_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLF_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLF_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLF_H_C, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLF_A_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_PIKEGRAVE_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_PIKEGRAVE_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFHUT_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_FROSTWOLFHUT_A_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_AID_H_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_AID_H_C, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AV_AID_A_C, 1);
                }
                break;
            case AREA_WARSONG_GULCH:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_WS)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(8);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_FLAG_CAPTURES_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_FLAG_CAPTURES_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_UNK_0, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_UNK_1, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_UNK_2, 2);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_FLAG_CAPTURES_MAX, 3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_FLAG_STATE_HORDE, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_WS_FLAG_STATE_ALLIANCE, 1);
                }
                break;
            case AREA_ARATHI_BASIN:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_AB)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(32);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_STABLE_STATE_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_STABLE_STATE_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_STABLE_STATE_CONTROLLED_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_STABLE_STATE_CONTROLLED_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_FARM_STATE_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_FARM_STATE_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_FARM_STATE_CONTROLLED_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_FARM_STATE_CONTROLLED_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_RESOURCES_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_RESOURCES_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_OCCUPIED_BASES_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_OCCUPIED_BASES_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_RESOURCES_MAX, 1600);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_BLACKSMITH_STATE_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_BLACKSMITH_STATE_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_BLACKSMITH_STATE_CONTROLLED_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_BLACKSMITH_STATE_CONTROLLED_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_GOLDMINE_STATE_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_GOLDMINE_STATE_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_GOLDMINE_STATE_CONTROLLED_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_GOLDMINE_STATE_CONTROLLED_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_LUMBERMILL_STATE_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_LUMBERMILL_STATE_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_LUMBERMILL_STATE_CONTROLLED_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_LUMBERMILL_STATE_CONTROLLED_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_STABLE_ICON, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_GOLDMINE_ICON, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_LUMBERMILL_ICON, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_FARM_ICON, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_BLACKSMITH_ICON, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_UNK, 2);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_AB_RESOURCES_WARNING, 1400); // warning limit (1400)
                }
                break;
            case AREA_EYE_OF_THE_STORM:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_EY)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(32);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_HORDE_BASE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_ALLIANCE_BASE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_MAGE_TOWER_HORDE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_MAGE_TOWER_ALLIANCE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FEL_REAVER_HORDE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FEL_REAVER_ALLIANCE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_DRAENEI_RUINS_ALLIANCE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_DRAENEI_RUINS_HORDE_CONFLICT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_UNK_2, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_UNK_1, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_DRAENEI_RUINS_HORDE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_DRAENEI_RUINS_ALLIANCE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_DRAENEI_RUINS_UNCONTROL, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_MAGE_TOWER_ALLIANCE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_MAGE_TOWER_HORDE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_MAGE_TOWER_UNCONTROL, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FEL_REAVER_HORDE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FEL_REAVER_ALLIANCE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FEL_REAVER_UNCONTROL, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_BLOOD_ELF_HORDE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_BLOOD_ELF_ALLIANCE_CONTROL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_BLOOD_ELF_UNCONTROL, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FLAG, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FLAG_STATE_HORDE, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_FLAG_STATE_ALLIANCE, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_HORDE_RESOURCES, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_ALLIANCE_RESOURCES, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_UNK_0, 142);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_PROGRESS_BAR_PERCENT_GREY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_PROGRESS_BAR_STATUS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_PROGRESS_BAR_SHOW, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_EY_UNK_3, 379);
                    // missing unknowns
                }
                break;
            // any of these needs change! the client remembers the prev setting!
            // ON EVERY ZONE LEAVE, RESET THE OLD ZONE'S WORLD STATE, BUT AT LEAST THE UI STUFF!
            case AREA_HELLFIRE_PENINSULA:
                if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_HP)
                    outdoorPvP->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(16);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_DISPLAY_A, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_DISPLAY_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_BROKENHILL_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_BROKENHILL_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_BROKENHILL_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_OVERLOOK_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_OVERLOOK_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_OVERLOOK_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_COUNT_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_COUNT_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_SLIDER_N, 100);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_SLIDER_POS, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_UI_TOWER_SLIDER_DISPLAY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_STADIUM_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_STADIUM_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_HP_STADIUM_H, 1);
                }
                break;
        case AREA_NAGRAND:
            if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_NA)
                outdoorPvP->FillInitialWorldStates(packet);
            else
            {
                packet.Worldstates.reserve(28);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_HORDE_GUARDS_SHOW, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_ALLIANCE_GUARDS_SHOW, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_GUARDS_MAX, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_GUARDS_LEFT, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_TOWER_SLIDER_DISPLAY, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_TOWER_SLIDER_POS, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_UI_SLIDER_N, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_NORTH_NEU_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_NORTH_NEU_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_NORTH_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_NORTH_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_SOUTH_NEU_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_SOUTH_NEU_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_SOUTH_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_SOUTH_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_WEST_NEU_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_WEST_NEU_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_WEST_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_WEST_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_EAST_NEU_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_EAST_NEU_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_EAST_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_WYVERN_EAST_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_HALAA_NEUTRAL, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_HALAA_NEU_A, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_HALAA_NEU_H, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_HALAA_HORDE, 0);
                packet.Worldstates.emplace_back(WORLD_STATE_OPVP_NA_MAP_HALAA_ALLIANCE, 0);
            }
                break;
            case AREA_TEROKKAR_FOREST:
                if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_TF)
                    outdoorPvP->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(28);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWER_SLIDER_POS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWER_SLIDER_N, 20);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWER_SLIDER_DISPLAY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWER_COUNT_H, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWER_COUNT_A, 5);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_TOWERS_CONTROLLED_DISPLAY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_14, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_13, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_12, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_11, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_10, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_09, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_08, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_07, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_06, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_15, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_05, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_04, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_03, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_02, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_01, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_TOWER_NUM_00, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_TIME_MINUTES_FIRST_DIGIT, 5);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_TIME_MINUTES_SECOND_DIGIT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_TIME_HOURS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_DISPLAY_NEUTRAL, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_DISPLAY_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_TF_UI_LOCKED_DISPLAY_ALLIANCE, 1);
                }
                break;
            case AREA_ZANGARMARSH:
                if (outdoorPvP && outdoorPvP->GetTypeId() == OUTDOOR_PVP_ZM)
                    outdoorPvP->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(26);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_N_W, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_POS_W, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_DISPLAY_W, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UNK, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_EAST_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_EAST_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_EAST_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_GRAVEYARD_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_GRAVEYARD_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_GRAVEYARD_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_WEST_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_WEST_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_TOWER_WEST_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_N_E, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_POS_E, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_SLIDER_DISPLAY_E, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_EAST_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_EAST_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_EAST_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_WEST_N, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_WEST_H, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_UI_TOWER_WEST_A, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_HORDE_FLAG_READY, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_HORDE_FLAG_NOT_READY, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_ALLIANCE_FLAG_NOT_READY, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_OPVP_ZM_MAP_ALLIANCE_FLAG_READY, 0);
                }
                break;
            case AREA_NAGRAND_ARENA:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_NA)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_NA_ARENA_GOLD, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_NA_ARENA_GREEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_NA_ARENA_SHOW, 0);
                }
                break;
            case AREA_BLADES_EDGE_ARENA:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_BE)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_BE_ARENA_GOLD, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_BE_ARENA_GREEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_BE_ARENA_SHOW, 0);
                }
                break;
            case AREA_RUINS_OF_LORDAERON:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_RL)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RL_ARENA_GOLD, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RL_ARENA_GREEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RL_ARENA_SHOW, 0);
                }
                break;
            case AREA_DALARAN_ARENA:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_DS)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_DS_ARENA_GOLD, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_DS_ARENA_GREEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_DS_ARENA_SHOW, 0);
                }
                break;
            case AREA_STRAND_OF_THE_ANCIENTS:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_SA)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(24);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_ANCIENT_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_YELLOW_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_GREEN_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_BLUE_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_RED_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_PURPLE_GATE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_BONUS_TIMER, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_HORDE_ATTACKER, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_ENABLE_TIMER, 0);

                    // End Round timer, example: 19:59 -> A:BC
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_TIMER_SECONDS_SECOND_DIGIT, 0); // C
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_TIMER_SECONDS_FIRST_DIGIT, 0); // B
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_TIMER_MINUTES, 0); // A

                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_CENTER_GY_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_RIGHT_GY_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_LEFT_GY_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_CENTER_GY_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_LEFT_GY_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_RIGHT_GY_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_HORDE_DEFENSE_TOKEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_ALLIANCE_DEFENSE_TOKEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_LEFT_ATTACK_TOKEN_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_RIGHT_ATTACK_TOKEN_HORDE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_RIGHT_ATTACK_TOKEN_ALLIANCE, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_SA_LEFT_ATTACK_TOKEN_ALLIANCE, 0);
                    // missing unknowns
                }
                break;
            case ARENA_THE_RING_OF_VALOR:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_RV)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RV_ARENA_GREEN, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RV_ARENA_GOLD, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_RV_ARENA_SHOW, 0);
                }
                break;
            case AREA_ISLE_OF_CONQUEST:
                if (battleground && battleground->GetBgTypeID(true) == BATTLEGROUND_IC)
                    battleground->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(18);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_ALLIANCE_REINFORCEMENT_SET, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_HORDE_REINFORCEMENT_SET, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_ALLIANCE_REINFORCEMENT, 300);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_HORDE_REINFORCEMENT, 300);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_FRONT_H_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_WEST_H_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_EAST_H_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_FRONT_A_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_WEST_A_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_EAST_A_WS_OPEN, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_GATE_FRONT_H_WS_CLOSED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_DOCKS_UNCONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_HANGAR_UNCONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_QUARRY_UNCONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_REFINERY_UNCONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_WORKSHOP_UNCONTROLLED, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_UNK, 1);
                    packet.Worldstates.emplace_back(WORLD_STATE_BATTLEGROUND_IC_HORDE_KEEP_CONTROLLED_H, 1);
                }
                break;
            case AREA_THE_RUBY_SANCTUM:
                if (instance)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_RUBY_SANCTUM_CORPOREALITY_MATERIAL, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_RUBY_SANCTUM_CORPOREALITY_TWILIGHT, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_RUBY_SANCTUM_CORPOREALITY_TOGGLE, 0);
                }
                break;
            case AREA_ICECROWN_CITADEL:
                if (instance && mapId == MAP_ICECROWN_CITADEL)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(5);
                    packet.Worldstates.emplace_back(WORLD_STATE_ICECROWN_CITADEL_SHOW_TIMER, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_ICECROWN_CITADEL_EXECUTION_TIME, 30);
                    packet.Worldstates.emplace_back(WORLD_STATE_ICECROWN_CITADEL_SHOW_ATTEMPTS, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_ICECROWN_CITADEL_ATTEMPTS_REMAINING, 50);
                    packet.Worldstates.emplace_back(WORLD_STATE_ICECROWN_CITADEL_ATTEMPTS_MAX, 50);
                }
                break;
            case AREA_THE_CULLING_OF_STRATHOLME:
                if (instance && mapId == MAP_THE_CULLING_OF_STRATHOLME)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(5);
                    packet.Worldstates.emplace_back(WORLD_STATE_CULLING_OF_STRATHOLME_SHOW_CRATES, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_CULLING_OF_STRATHOLME_CRATES_REVEALED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_CULLING_OF_STRATHOLME_WAVE_COUNT, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_CULLING_OF_STRATHOLME_TIME_GUARDIAN, 25);
                    packet.Worldstates.emplace_back(WORLD_STATE_CULLING_OF_STRATHOLME_TIME_GUARDIAN_SHOW, 0);
                }
                break;
            case AREA_THE_OCULUS:
                if (instance && mapId == MAP_THE_OCULUS)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(2);
                    packet.Worldstates.emplace_back(WORLD_STATE_OCULUS_CENTRIFUGE_CONSTRUCT_SHOW, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_OCULUS_CENTRIFUGE_CONSTRUCT_AMOUNT, 0);
                }
                break;
            case AREA_ULDUAR:
                if (instance && mapId == MAP_ULDUAR)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(2);
                    packet.Worldstates.emplace_back(WORLD_STATE_ULDUAR_ALGALON_TIMER_ENABLED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_ULDUAR_ALGALON_DESPAWN_TIMER, 0);
                }
                break;
            case AREA_THE_VIOLET_HOLD:
                if (instance)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(3);
                    packet.Worldstates.emplace_back(WORLD_STATE_VIOLET_HOLD_SHOW, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_VIOLET_HOLD_PRISON_STATE, 100);
                    packet.Worldstates.emplace_back(WORLD_STATE_VIOLET_HOLD_WAVE_COUNT, 0);
                }
                break;
            case AREA_HALLS_OF_REFLECTION:
                if (instance && mapId == MAP_HALLS_OF_REFLECTION)
                    instance->FillInitialWorldStates(packet);
                else
                {
                    packet.Worldstates.reserve(2);
                    packet.Worldstates.emplace_back(WORLD_STATE_HALLS_OF_REFLECTION_WAVES_ENABLED, 0);
                    packet.Worldstates.emplace_back(WORLD_STATE_HALLS_OF_REFLECTION_WAVE_COUNT, 0);
                }
                break;
            case AREA_PLAGUELANDS_THE_SCARLET_ENCLAVE: // (DK starting zone)
                // Get Mograine, GUID and ENTRY should NEVER change
                if (Creature* mograine = ObjectAccessor::GetCreature(*this, ObjectGuid::Create<HighGuid::Unit>(29173, 130956)))
                {
                    if (CreatureAI* mograineAI = mograine->AI())
                    {
                        packet.Worldstates.reserve(6);
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_DEFENDERS_COUNT, mograineAI->GetData(3590));
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_SCOURGE_COUNT, mograineAI->GetData(3591));
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_SOLDIERS_ENABLE, mograineAI->GetData(3592));
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_COUNTDOWN_ENABLE, mograineAI->GetData(3603));
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_COUNTDOWN_TIME, mograineAI->GetData(3604));
                        packet.Worldstates.emplace_back(WORLD_STATE_BATTLE_FOR_LIGHTS_HOPE_EVENT_BEGIN_ENABLE, mograineAI->GetData(3605));
                    }
                }
                break;
            case AREA_WINTERGRASP:
                if (battlefield && battlefield->GetTypeId() == BATTLEFIELD_WG)
                {
                    battlefield->FillInitialWorldStates(packet);
                    break;
                }
            default:
                break;
            }
        }
    }

    sWorldState->FillInitialWorldStates(packet, zoneId, areaId);
    SendDirectMessage(packet.Write());
    SendBGWeekendWorldStates();
    SendBattlefieldWorldStates();
}

void Player::SendBGWeekendWorldStates()
{
    for (uint32 i = 1; i < sBattlemasterListStore.GetNumRows(); ++i)
    {
        BattlemasterListEntry const* bl = sBattlemasterListStore.LookupEntry(i);
        if (bl && bl->HolidayWorldStateId)
        {
            if (BattlegroundMgr::IsBGWeekend((BattlegroundTypeId)bl->id))
                SendUpdateWorldState(bl->HolidayWorldStateId, 1);
            else
                SendUpdateWorldState(bl->HolidayWorldStateId, 0);
        }
    }
}

void Player::SendBattlefieldWorldStates()
{
    /// Send misc stuff that needs to be sent on every login, like the battle timers.
    if (sWorld->getIntConfig(CONFIG_WINTERGRASP_ENABLE) == 1)
    {
        if (BattlefieldWG* wg = (BattlefieldWG*)sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG))
        {
            wg->SendUpdateWorldStates(this);
        }
    }
}

void Player::SendTaxiNodeStatusMultiple()
{
    DoForAllVisibleWorldObjects([this](WorldObject* worldObject)
    {
        Creature* creature = worldObject->ToCreature();
        // reaction must be checked both ways: the Dark Portal flight masters are neutral toward opposite-faction players, while players are hostile toward them
        if (!creature || creature->GetReactionTo(this) <= REP_UNFRIENDLY || IsHostileTo(creature))
            return;

        if (!creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
            return;

        uint32 nearestNode = sObjectMgr->GetNearestTaxiNode(*creature, GetTeamId(true));
        if (!nearestNode)
            return;

        WorldPacket data(SMSG_TAXINODE_STATUS, 9);
        data << creature->GetGUID();
        data << uint8(m_taxi.IsTaximaskNodeKnown(nearestNode) ? 1 : 0);
        SendDirectMessage(&data);
    });
}

uint32 Player::GetMaxPersonalArenaRatingRequirement(uint32 minarenaslot) const
{
    // returns the maximal personal arena rating that can be used to purchase items requiring this condition
    // the personal rating of the arena team must match the required limit as well
    // so return max[in arenateams](min(personalrating[teamtype], teamrating[teamtype]))
    uint32 max_personal_rating = 0;
    for (uint8 i = minarenaslot; i < MAX_ARENA_SLOT; ++i)
    {
        if (ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamId(i)))
        {
            uint32 p_rating = GetArenaPersonalRating(i);
            uint32 t_rating = at->GetRating();
            p_rating = p_rating < t_rating ? p_rating : t_rating;
            if (max_personal_rating < p_rating)
                max_personal_rating = p_rating;
        }
    }

    sScriptMgr->OnPlayerGetMaxPersonalArenaRatingRequirement(this, minarenaslot, max_personal_rating);

    return max_personal_rating;
}

void Player::SetCanParry(bool value)
{
    if (m_canParry == value)
        return;

    m_canParry = value;
    UpdateParryPercentage();
}

void Player::SetCanBlock(bool value)
{
    if (m_canBlock == value)
        return;

    m_canBlock = value;
    UpdateBlockPercentage();
}

void Player::CheckAllAchievementCriteria()
{
    m_achievementMgr->CheckAllAchievementCriteria();
}

void Player::ResetAchievementCriteria(AchievementCriteriaCondition condition, uint32 value, bool evenIfCriteriaComplete /* = false*/)
{
    m_achievementMgr->ResetAchievementCriteria(condition, value, evenIfCriteriaComplete);
}

void Player::_LoadRandomBGStatus(PreparedQueryResult result)
{
    if (result)
        m_IsBGRandomWinner = true;
}

uint32 Player::GetArenaPersonalRating(uint8 slot) const
{
    uint32 result = GetUInt32Value(PLAYER_FIELD_ARENA_TEAM_INFO_1_1 + (slot * ARENA_TEAM_END) + ARENA_TEAM_PERSONAL_RATING);

    sScriptMgr->OnPlayerGetArenaPersonalRating(const_cast<Player*>(this), slot, result);

    return result;
}
