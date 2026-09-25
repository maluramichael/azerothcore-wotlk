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

void Player::InitTalentForLevel()
{
    uint32 talentPointsForLevel = CalculateTalentsPoints();

    // if used more than have then reset
    if (m_usedTalentCount > talentPointsForLevel)
    {
        if (!GetSession()->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_MORE_TALENTS_THAN_ALLOWED))
            resetTalents(true);
        else
            SetFreeTalentPoints(0);
    }
    // else update amount of free points
    else
        SetFreeTalentPoints(talentPointsForLevel - m_usedTalentCount);

    if (!GetSession()->PlayerLoading())
        SendTalentsInfoData(false);                         // update at client
}

bool Player::addTalent(uint32 spellId, uint8 addSpecMask, uint8 oldTalentRank)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!SpellMgr::CheckSpellValid(spellInfo, spellId, true))
        return false;

    TalentSpellPos const* talentPos = GetTalentSpellPos(spellId);
    if (!talentPos)
        return false;

    TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentPos->talent_id);
    if (!talentInfo)
        return false;

    // xinef: remove old talent rank if any
    if (oldTalentRank)
    {
        _removeTalent(talentInfo->RankID[oldTalentRank - 1], addSpecMask);
        _removeTalentAurasAndSpells(talentInfo->RankID[oldTalentRank - 1]);
        SendLearnPacket(talentInfo->RankID[oldTalentRank - 1], false);
    }

    // xinef: add talent auras and spells
    if (GetActiveSpecMask() & addSpecMask)
        _addTalentAurasAndSpells(spellId);

    // xinef: find the spell on our talent map
    PlayerTalentMap::iterator itr = m_talents.find(spellId);

    // xinef: we do not have such a spell on our talent map
    if (itr == m_talents.end())
    {
        PlayerSpellState state = isBeingLoaded() ? PLAYERSPELL_UNCHANGED : PLAYERSPELL_NEW;
        PlayerTalent* newTalent = new PlayerTalent();
        newTalent->State = state;
        newTalent->specMask = addSpecMask;
        newTalent->talentID = talentInfo->TalentID;
        newTalent->inSpellBook = talentInfo->addToSpellBook && !spellInfo->HasAttribute(SPELL_ATTR0_PASSIVE) && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL);
        m_talents[spellId] = newTalent;

        if (GetActiveSpecMask() & addSpecMask)
            m_usedTalentCount += (talentPos->rank + 1) - oldTalentRank;

        return true;
    }
    // xinef: if current mask does not cover addMask, add it to iterator and save changes to DB
    else if (!(itr->second->specMask & addSpecMask))
    {
        itr->second->specMask |= addSpecMask;
        if (itr->second->State != PLAYERSPELL_NEW)
            itr->second->State = PLAYERSPELL_CHANGED;

        if (GetActiveSpecMask() & addSpecMask)
            m_usedTalentCount += (talentPos->rank + 1) - oldTalentRank;

        return true;
    }

    return false;
}

void Player::_removeTalent(uint32 spellId, uint8 specMask)
{
    PlayerTalentMap::iterator itr = m_talents.find(spellId);
    if (itr == m_talents.end() || itr->second->State == PLAYERSPELL_REMOVED)
        return;

    _removeTalent(itr, specMask);
}

void Player::_removeTalent(PlayerTalentMap::iterator& itr, uint8 specMask)
{
    // xinef: remove spec mask from iterator
    itr->second->specMask &= ~specMask;

    // xinef: if talent is not present in any spec - remove
    if (itr->second->specMask == 0)
    {
        if (itr->second->State == PLAYERSPELL_NEW)
        {
            delete itr->second;
            m_talents.erase(itr);
            return;
        }
        else
            itr->second->State = PLAYERSPELL_REMOVED;
    }
    // xinef: otherwise save changes to DB
    else if (itr->second->State != PLAYERSPELL_NEW)
        itr->second->State = PLAYERSPELL_CHANGED;
}

uint32 Player::resetTalentsCost() const
{
    // The first time reset costs 1 gold
    if (m_resetTalentsCost < 1 * GOLD)
        return 1 * GOLD;
    // then 5 gold
    else if (m_resetTalentsCost < 5 * GOLD)
        return 5 * GOLD;
    // After that it increases in increments of 5 gold
    else if (m_resetTalentsCost < 10 * GOLD)
        return 10 * GOLD;
    else
    {
        uint64 months = (GameTime::GetGameTime().count() - m_resetTalentsTime) / MONTH;
        if (months > 0)
        {
            // This cost will be reduced by a rate of 5 gold per month
            int32 new_cost = int32(m_resetTalentsCost - 5 * GOLD * months);
            // to a minimum of 10 gold.
            return (new_cost < 10 * GOLD ? 10 * GOLD : new_cost);
        }
        else
        {
            // After that it increases in increments of 5 gold
            int32 new_cost = m_resetTalentsCost + 5 * GOLD;
            // until it hits a cap of 50 gold.
            if (new_cost > 50 * GOLD)
                new_cost = 50 * GOLD;
            return new_cost;
        }
    }
}

bool Player::resetTalents(bool noResetCost)
{
    sScriptMgr->OnPlayerTalentsReset(this, noResetCost);

    // xinef: remove at login flag upon talents reset
    if (HasAtLoginFlag(AT_LOGIN_RESET_TALENTS))
        RemoveAtLoginFlag(AT_LOGIN_RESET_TALENTS, true);

    // xinef: get max available talent points amount
    uint32 talentPointsForLevel = CalculateTalentsPoints();

    // xinef: no talent points are used, return
    if (m_usedTalentCount == 0)
        return false;
    m_usedTalentCount = 0;

    // xinef: check if we have enough money
    uint32 resetCost = 0;
    if (!noResetCost && !sWorld->getBoolConfig(CONFIG_NO_RESET_TALENT_COST))
    {
        resetCost = resetTalentsCost();
        if (!HasEnoughMoney(resetCost))
        {
            SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, 0, 0, 0);
            return false;
        }
    }

    RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);

    // xinef: reset talents
    for (PlayerTalentMap::iterator iter = m_talents.begin(); iter != m_talents.end(); )
    {
        PlayerTalentMap::iterator itr = iter++;

        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        // xinef: talent not in current spec
        if (!(itr->second->specMask & GetActiveSpecMask()))
            continue;

        // xinef: remove talent auras
        _removeTalentAurasAndSpells(itr->first);

        // xinef: check if talent learns spell to spell book
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(itr->second->talentID);
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);

        bool removed = false;
        if (talentInfo->addToSpellBook)
            if (!spellInfo->HasAttribute(SPELL_ATTR0_PASSIVE) && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
            {
                removeSpell(itr->first, GetActiveSpecMask(), false);
                removed = true;
            }

        // Xinef: send unlearn spell packet at talent remove
        if (!removed)
            SendLearnPacket(itr->first, false);

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL)
                if (sSpellMgr->IsAdditionalTalentSpell(spellInfo->Effects[i].TriggerSpell))
                    removeSpell(spellInfo->Effects[i].TriggerSpell, GetActiveSpecMask(), false);

        // xinef: remove talent modifies m_talents, move itr to map begin
        _removeTalent(itr, GetActiveSpecMask());
    }

    // xinef: remove titan grip if player had it set
    if (m_canTitanGrip)
        SetCanTitanGrip(false);
    // xinef: remove dual wield if player does not have dual wield spell (shamans)
    if (!HasSpell(674) && CanDualWield())
        SetCanDualWield(false);

    AutoUnequipOffhandIfNeed();

    // pussywizard: removed saving to db, nothing important happens and saving only spells and talents may cause data integrity problems (eg. with skills saved to db)
    SetFreeTalentPoints(talentPointsForLevel);

    if (!noResetCost)
    {
        ModifyMoney(-(int32)resetCost);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GOLD_SPENT_FOR_TALENTS, resetCost);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_NUMBER_OF_TALENT_RESETS, 1);

        m_resetTalentsCost = resetCost;
        m_resetTalentsTime = GameTime::GetGameTime().count();
    }

    return true;
}

void Player::SetFreeTalentPoints(uint32 points)
{
    sScriptMgr->OnPlayerFreeTalentPointsChanged(this, points);
    SetUInt32Value(PLAYER_CHARACTER_POINTS1, points);
}

bool Player::HasTalent(uint32 spell, uint8 spec) const
{
    PlayerTalentMap::const_iterator itr = m_talents.find(spell);
    return (itr != m_talents.end() && itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(spec));
}

void Player::ModifySkillBonus(uint32 skillid, int32 val, bool talent)
{
    SkillStatusMap::const_iterator itr = mSkillStatus.find(skillid);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return;

    uint32 bonusIndex = PLAYER_SKILL_BONUS_INDEX(itr->second.pos);

    uint32 bonus_val = GetUInt32Value(bonusIndex);
    int16 temp_bonus = SKILL_TEMP_BONUS(bonus_val);
    int16 perm_bonus = SKILL_PERM_BONUS(bonus_val);

    if (talent)                                          // permanent bonus stored in high part
        SetUInt32Value(bonusIndex, MAKE_SKILL_BONUS(temp_bonus, perm_bonus + val));
    else                                                // temporary/item bonus stored in low part
        SetUInt32Value(bonusIndex, MAKE_SKILL_BONUS(temp_bonus + val, perm_bonus));
}

void Player::SetSkill(uint16 id, uint16 step, uint16 newVal, uint16 maxVal)
{
    if (!id)
        return;

    uint16 currVal;
    SkillStatusMap::iterator itr = mSkillStatus.find(id);

    //has skill
    if (itr != mSkillStatus.end() && itr->second.uState != SKILL_DELETED)
    {
        currVal = SKILL_VALUE(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos)));
        if (newVal)
        {
            // if skill value is going down, update enchantments before setting the new value
            if (newVal < currVal)
                UpdateSkillEnchantments(id, currVal, newVal);
            // update step
            SetUInt32Value(PLAYER_SKILL_INDEX(itr->second.pos), MAKE_PAIR32(id, step));
            // update value
            SetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos), MAKE_SKILL_VALUE(newVal, maxVal));
            if (itr->second.uState != SKILL_NEW)
                itr->second.uState = SKILL_CHANGED;
            learnSkillRewardedSpells(id, newVal);
            // if skill value is going up, update enchantments after setting the new value
            if (newVal > currVal)
                UpdateSkillEnchantments(id, currVal, newVal);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL, id);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LEVEL, id);
        }
        else                                                //remove
        {
            //remove enchantments needing this skill
            UpdateSkillEnchantments(id, currVal, 0);
            // clear skill fields
            SetUInt32Value(PLAYER_SKILL_INDEX(itr->second.pos), 0);
            SetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos), 0);
            SetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos), 0);

            // mark as deleted or simply remove from map if not saved yet
            if (itr->second.uState != SKILL_NEW)
                itr->second.uState = SKILL_DELETED;
            else
                mSkillStatus.erase(itr);

            // remove all spells that related to this skill
            for (SkillLineAbilityEntry const* pAbility : GetSkillLineAbilitiesBySkillLine(id))
            {
                removeSpell(sSpellMgr->GetFirstSpellInChain(pAbility->Spell), SPEC_MASK_ALL, false);
                RemoveAurasDueToSpell(pAbility->Spell);
            }
        }
        sScriptMgr->OnPlayerSetSkill(this, id, currVal, maxVal, step, newVal);
    }
    else if (newVal)                                        //add
    {
        currVal = 0;
        for (int i = 0; i < PLAYER_MAX_SKILLS; ++i)
            if (!GetUInt32Value(PLAYER_SKILL_INDEX(i)))
            {
                SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(id);
                if (!pSkill)
                {
                    LOG_ERROR("entities.player", "Skill not found in SkillLineStore: skill #{}", id);
                    return;
                }

                SetUInt32Value(PLAYER_SKILL_INDEX(i), MAKE_PAIR32(id, step));
                SetUInt32Value(PLAYER_SKILL_VALUE_INDEX(i), MAKE_SKILL_VALUE(newVal, maxVal));
                UpdateSkillEnchantments(id, currVal, newVal);

                // insert new entry or update if not deleted old entry yet
                if (itr != mSkillStatus.end())
                {
                    itr->second.pos = i;
                    itr->second.uState = SKILL_CHANGED;
                }
                else
                    mSkillStatus.insert(SkillStatusMap::value_type(id, SkillStatusData(i, SKILL_NEW)));

                // apply skill bonuses
                SetUInt32Value(PLAYER_SKILL_BONUS_INDEX(i), 0);

                // temporary bonuses
                AuraEffectList const& mModSkill = GetAuraEffectsByType(SPELL_AURA_MOD_SKILL);
                for (AuraEffectList::const_iterator j = mModSkill.begin(); j != mModSkill.end(); ++j)
                    if ((*j)->GetMiscValue() == int32(id))
                        (*j)->HandleEffect(this, AURA_EFFECT_HANDLE_SKILL, true);

                // permanent bonuses
                AuraEffectList const& mModSkillTalent = GetAuraEffectsByType(SPELL_AURA_MOD_SKILL_TALENT);
                for (AuraEffectList::const_iterator j = mModSkillTalent.begin(); j != mModSkillTalent.end(); ++j)
                    if ((*j)->GetMiscValue() == int32(id))
                        (*j)->HandleEffect(this, AURA_EFFECT_HANDLE_SKILL, true);

                // Learn all spells for skill
                learnSkillRewardedSpells(id, newVal);
                UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL, id);
                UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LEVEL, id);
                sScriptMgr->OnPlayerSetSkill(this, id, currVal, maxVal, step, newVal);
                return;
            }
    }
}

bool Player::HasSkill(uint32 skill) const
{
    if (!skill)
        return false;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    return (itr != mSkillStatus.end() && itr->second.uState != SKILL_DELETED);
}

uint16 Player::GetSkillStep(uint16 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    return PAIR32_HIPART(GetUInt32Value(PLAYER_SKILL_INDEX(itr->second.pos)));
}

uint16 Player::GetSkillValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    uint32 bonus = GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos));

    int32 result = int32(SKILL_VALUE(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos))));
    result += SKILL_TEMP_BONUS(bonus);
    result += SKILL_PERM_BONUS(bonus);
    return result < 0 ? 0 : result;
}

uint16 Player::GetMaxSkillValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    uint32 bonus = GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos));

    int32 result = int32(SKILL_MAX(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos))));
    sScriptMgr->OnPlayerGetMaxSkillValue(const_cast<Player*>(this), skill, result, false);
    result += SKILL_TEMP_BONUS(bonus);
    result += SKILL_PERM_BONUS(bonus);
    return result < 0 ? 0 : result;
}

uint16 Player::GetPureMaxSkillValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    int32 result = int32(SKILL_MAX(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos))));

    sScriptMgr->OnPlayerGetMaxSkillValue(const_cast<Player*>(this), skill, result, true);

    return result < 0 ? 0 : result;
}

uint16 Player::GetBaseSkillValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    int32 result = int32(SKILL_VALUE(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos))));
    result += SKILL_PERM_BONUS(GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos)));
    return result < 0 ? 0 : result;
}

uint16 Player::GetPureSkillValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    return SKILL_VALUE(GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos)));
}

int16 Player::GetSkillPermBonusValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    return SKILL_PERM_BONUS(GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos)));
}

int16 Player::GetSkillTempBonusValue(uint32 skill) const
{
    if (!skill)
        return 0;

    SkillStatusMap::const_iterator itr = mSkillStatus.find(skill);
    if (itr == mSkillStatus.end() || itr->second.uState == SKILL_DELETED)
        return 0;

    return SKILL_TEMP_BONUS(GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(itr->second.pos)));
}

void Player::RewardExtraBonusTalentPoints(uint32 bonusTalentPoints)
{
    if (bonusTalentPoints)
    {
        m_extraBonusTalentCount += bonusTalentPoints;
    }
}

void Player::SendTalentWipeConfirm(ObjectGuid guid)
{
    WorldPacket data(MSG_TALENT_WIPE_CONFIRM, (8 + 4));
    data << guid;
    uint32 cost = sWorld->getBoolConfig(CONFIG_NO_RESET_TALENT_COST) ? 0 : resetTalentsCost();
    data << cost;
    SendDirectMessage(&data);
}

void Player::ResetPetTalents()
{
    // This needs another gossip option + NPC text as a confirmation.
    // The confirmation gossip listid has the text: "Yes, please do."
    Pet* pet = GetPet();

    if (!pet || pet->getPetType() != HUNTER_PET || pet->m_usedTalentCount == 0)
        return;

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        LOG_ERROR("entities.player", "Object ({}) is considered pet-like but doesn't have a charminfo!", pet->GetGUID().ToString());
        return;
    }
    pet->resetTalents();
    SendTalentsInfoData(true);
}

void Player::LearnDefaultSkills()
{
    // learn default race/class skills
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(), getClass());
    for (PlayerCreateInfoSkills::const_iterator itr = info->skills.begin(); itr != info->skills.end(); ++itr)
    {
        uint32 skillId = itr->SkillId;
        if (HasSkill(skillId))
            continue;

        LearnDefaultSkill(skillId, itr->Rank);
    }
}

void Player::LearnDefaultSkill(uint32 skillId, uint16 rank)
{
    SkillRaceClassInfoEntry const* rcInfo = GetSkillRaceClassInfo(skillId, getRace(), getClass());
    if (!rcInfo)
        return;

    LOG_DEBUG("entities.player.loading", "PLAYER (Class: {} Race: {}): Adding initial skill, id = {}", uint32(getClass()), uint32(getRace()), skillId);
    switch (GetSkillRangeType(rcInfo))
    {
        case SKILL_RANGE_LANGUAGE:
            SetSkill(skillId, 0, 300, 300);
            break;
        case SKILL_RANGE_LEVEL:
        {
            uint16 skillValue = 1;
            uint16 maxValue = GetMaxSkillValueForLevel();
            if (sWorld->getBoolConfig(CONFIG_ALWAYS_MAXSKILL) && !IsProfessionOrRidingSkill(skillId))
            {
                skillValue = maxValue;
            }
            else if (rcInfo->Flags & SKILL_FLAG_ALWAYS_MAX_VALUE)
            {
                skillValue = maxValue;
            }
            else if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_SKILL))
            {
                skillValue = std::min(std::max<uint16>({ 1, uint16((GetLevel() - 1) * 5) }), maxValue);
            }
            else if (skillId == SKILL_FIST_WEAPONS)
            {
                skillValue = std::max<uint16>(1, GetSkillValue(SKILL_UNARMED));
            }
            else if (skillId == SKILL_LOCKPICKING)
            {
                skillValue = std::max<uint16>(1, GetSkillValue(SKILL_LOCKPICKING));
            }

            SetSkill(skillId, 0, skillValue, maxValue);
            break;
        }
        case SKILL_RANGE_MONO:
            SetSkill(skillId, 0, 1, 1);
            break;
        case SKILL_RANGE_RANK:
        {
            if (!rank)
            {
                break;
            }

            SkillTiersEntry const* tier = sSkillTiersStore.LookupEntry(rcInfo->SkillTierID);
            uint16 maxValue = tier->Value[std::max<int32>(rank - 1, 0)];
            uint16 skillValue = 1;
            if (rcInfo->Flags & SKILL_FLAG_ALWAYS_MAX_VALUE)
            {
                skillValue = maxValue;
            }
            else if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_SKILL))
            {
                skillValue = std::min(std::max<uint16>({ uint16(1), uint16((GetLevel() - 1) * 5) }), maxValue);
            }

            SetSkill(skillId, rank, skillValue, maxValue);
            break;
        }
        default:
            break;
    }
}

uint32 Player::GetBaseWeaponSkillValue(WeaponAttackType attType) const
{
    Item* item = GetWeaponForAttack(attType, true);

    // unarmed only with base attack
    if (attType != BASE_ATTACK && !item)
        return 0;

    // weapon skill or (unarmed for base attack)
    uint32  skill = item ? item->GetSkill() : uint32(SKILL_UNARMED);
    return GetBaseSkillValue(skill);
}

void Player::InitGlyphsForLevel()
{
    for (uint32 i = 0; i < sGlyphSlotStore.GetNumRows(); ++i)
        if (GlyphSlotEntry const* gs = sGlyphSlotStore.LookupEntry(i))
            if (gs->Order)
                SetGlyphSlot(gs->Order - 1, gs->Id);

    uint8 level = GetLevel();
    uint32 value = 0;

    // 0x3F = 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 for 80 level
    if (level >= 15)
        value |= (0x01 | 0x02);
    if (level >= 30)
        value |= 0x08;
    if (level >= 50)
        value |= 0x04;
    if (level >= 70)
        value |= 0x10;
    if (level >= 80)
        value |= 0x20;

    SetUInt32Value(PLAYER_GLYPHS_ENABLED, value);
}

uint32 Player::CalculateTalentsPoints() const
{
    uint32 base_talent = GetLevel() < 10 ? 0 : GetLevel() - 9;

    uint32 talentPointsForLevel = 0;
    if (!IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TALENT_POINT_CALC) || GetMapId() != MAP_EBON_HOLD)
    {
        talentPointsForLevel = base_talent;
    }
    else
    {
        talentPointsForLevel = GetLevel() < 56 ? 0 : GetLevel() - 55;
        talentPointsForLevel += m_questRewardTalentCount;

        if (talentPointsForLevel > base_talent)
        {
            talentPointsForLevel = base_talent;
        }
    }

    talentPointsForLevel += m_extraBonusTalentCount;
    sScriptMgr->OnPlayerCalculateTalentsPoints(this, talentPointsForLevel);
    return uint32(talentPointsForLevel * sWorld->getRate(RATE_TALENT));
}

void Player::_LoadSkills(PreparedQueryResult result)
{
    //                                                           0      1      2
    // SetQuery(PLAYER_LOGIN_QUERY_LOADSKILLS,          "SELECT skill, value, max FROM character_skills WHERE guid = '{}'", m_guid.GetCounter());

    uint32 count = 0;
    std::unordered_map<uint32, uint32> loadedSkillValues;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint16 skill    = fields[0].Get<uint16>();
            uint16 value    = fields[1].Get<uint16>();
            uint16 max      = fields[2].Get<uint16>();

            SkillRaceClassInfoEntry const* rcEntry = GetSkillRaceClassInfo(skill, getRace(), getClass());
            if (!rcEntry)
            {
                LOG_ERROR("entities.player", "Player {} (GUID: {}), has skill ({}) that is invalid for the race/class combination (Race: {}, Class: {}). Will be deleted.",
                    GetName(), GetGUID().GetCounter(), skill, getRace(), getClass());

                // Mark skill for deletion in the database
                mSkillStatus.insert(SkillStatusMap::value_type(skill, SkillStatusData(0, SKILL_DELETED)));
                continue;
            }

            // set fixed skill ranges
            switch (GetSkillRangeType(rcEntry))
            {
                case SKILL_RANGE_LANGUAGE:                      // 300..300
                    value = max = 300;
                    break;
                case SKILL_RANGE_MONO:                          // 1..1, grey monolite bar
                    value = max = 1;
                    break;
                case SKILL_RANGE_LEVEL:
                    max = GetMaxSkillValueForLevel();
                default:
                    break;
            }

            if (value == 0)
            {
                LOG_ERROR("entities.player", "Player {} (GUID: {}), has skill ({}) with value 0. Will be deleted.",
                    GetName(), GetGUID().GetCounter(), skill);

                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHARACTER_SKILL);

                stmt->SetData(0, GetGUID().GetRawValue());
                stmt->SetData(1, skill);

                CharacterDatabase.Execute(stmt);

                continue;
            }

            uint16 skillStep = 0;
            if (SkillTiersEntry const* skillTier = sSkillTiersStore.LookupEntry(rcEntry->SkillTierID))
            {
                for (uint32 i = 0; i < MAX_SKILL_STEP; ++i)
                {
                    if (skillTier->Value[skillStep] == max)
                    {
                        skillStep = i + 1;
                        break;
                    }
                }
            }

            SetUInt32Value(PLAYER_SKILL_INDEX(count), MAKE_PAIR32(skill, skillStep));

            SetUInt32Value(PLAYER_SKILL_VALUE_INDEX(count), MAKE_SKILL_VALUE(value, max));
            SetUInt32Value(PLAYER_SKILL_BONUS_INDEX(count), 0);

            mSkillStatus.insert(SkillStatusMap::value_type(skill, SkillStatusData(count, SKILL_UNCHANGED)));

            loadedSkillValues[skill] = value;

            ++count;

            if (count >= PLAYER_MAX_SKILLS)                      // client limit
            {
                LOG_ERROR("entities.player", "Character {} has more than {} skills.", GetGUID().ToString(), PLAYER_MAX_SKILLS);
                break;
            }
        } while (result->NextRow());
    }

    // Learn skill rewarded spells after all skills have been loaded to prevent learning a skill from them before its loaded with proper value from DB
    for (auto& skill : loadedSkillValues)
    {
        learnSkillRewardedSpells(skill.first, skill.second);
    }

    for (; count < PLAYER_MAX_SKILLS; ++count)
    {
        SetUInt32Value(PLAYER_SKILL_INDEX(count), 0);
        SetUInt32Value(PLAYER_SKILL_VALUE_INDEX(count), 0);
        SetUInt32Value(PLAYER_SKILL_BONUS_INDEX(count), 0);
    }
}

void Player::LearnTalent(uint32 talentId, uint32 talentRank, bool command /*= false*/)
{
    uint32 CurTalentPoints = GetFreeTalentPoints();

    if (!command)
    {
        // xinef: check basic data
        if (!CurTalentPoints)
        {
            return;
        }

        if (talentRank >= MAX_TALENT_RANK)
        {
            return;
        }
    }

    TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
    if (!talentInfo)
        return;

    if (!sScriptMgr->OnPlayerCanLearnTalent(this, talentInfo, talentRank))
        return;

    TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
    if (!talentTabInfo)
        return;

    // xinef: prevent learn talent for different class (cheating)
    if ((getClassMask() & talentTabInfo->ClassMask) == 0)
        return;

    // xinef: find current talent rank
    uint32 currentTalentRank = 0;
    for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
    {
        if (talentInfo->RankID[rank] && HasTalent(talentInfo->RankID[rank], GetActiveSpec()))
        {
            currentTalentRank = rank + 1;
            break;
        }
    }

    // xinef: we already have same or higher rank talent learned
    if (currentTalentRank >= talentRank + 1)
        return;

    uint32 talentPointsChange = (talentRank - currentTalentRank + 1);
    if (!command)
    {
        // xinef: check if we have enough free talent points
        if (CurTalentPoints < talentPointsChange)
        {
            return;
        }
    }

    // xinef: check if talent deponds on another talent
    if (talentInfo->DependsOn > 0)
        if (TalentEntry const* depTalentInfo = sTalentStore.LookupEntry(talentInfo->DependsOn))
        {
            bool hasEnoughRank = false;
            for (uint8 rank = talentInfo->DependsOnRank; rank < MAX_TALENT_RANK; rank++)
            {
                if (depTalentInfo->RankID[rank] != 0)
                    if (HasTalent(depTalentInfo->RankID[rank], GetActiveSpec()))
                    {
                        hasEnoughRank = true;
                        break;
                    }
            }

            // xinef: does not have enough talent points spend in required talent
            if (!hasEnoughRank)
                return;
        }

    if (!command)
    {
        // xinef: check amount of points spent in current talent tree
        // xinef: be smart and quick
        uint32 spentPoints = 0;
        if (talentInfo->Row > 0)
        {
            PlayerTalentMap const& talentMap = GetTalentMap();
            for (PlayerTalentMap::const_iterator itr = talentMap.begin(); itr != talentMap.end(); ++itr)
                if (TalentSpellPos const* talentPos = GetTalentSpellPos(itr->first))
                    if (TalentEntry const* itrTalentInfo = sTalentStore.LookupEntry(talentPos->talent_id))
                        if (itrTalentInfo->TalentTab == talentInfo->TalentTab)
                            if (itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(GetActiveSpec())) // pussywizard
                                spentPoints += talentPos->rank + 1;
        }

        // xinef: we do not have enough talent points to add talent of this tier
        if (spentPoints < (talentInfo->Row * MAX_TALENT_RANK))
            return;
    }

    // xinef: hacking attempt, tries to learn unknown rank
    uint32 spellId = talentInfo->RankID[talentRank];
    if (spellId == 0)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return;

    bool learned = false;

    // xinef: if talent info has special marker in dbc - add to spell book
    if (talentInfo->addToSpellBook)
        if (!spellInfo->HasAttribute(SPELL_ATTR0_PASSIVE) && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
        {
            learnSpell(spellId);
            learned = true;
        }

    if (!learned)
        SendLearnPacket(spellId, true);

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL)
            if (sSpellMgr->IsAdditionalTalentSpell(spellInfo->Effects[i].TriggerSpell))
                learnSpell(spellInfo->Effects[i].TriggerSpell);

    addTalent(spellId, GetActiveSpecMask(), currentTalentRank);

    if (!command)
    {
        SetFreeTalentPoints(CurTalentPoints - talentPointsChange);
    }

    sScriptMgr->OnPlayerLearnTalents(this, talentId, talentRank, spellId);
}

void Player::LearnPetTalent(ObjectGuid petGuid, uint32 talentId, uint32 talentRank)
{
    Pet* pet = GetPet();

    if (!pet)
        return;

    if (petGuid != pet->GetGUID())
        return;

    uint32 CurTalentPoints = pet->GetFreeTalentPoints();

    if (CurTalentPoints == 0)
        return;

    if (talentRank >= MAX_PET_TALENT_RANK)
        return;

    TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);

    if (!talentInfo)
        return;

    TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);

    if (!talentTabInfo)
        return;

    CreatureTemplate const* ci = pet->GetCreatureTemplate();

    if (!ci)
        return;

    CreatureFamilyEntry const* pet_family = sCreatureFamilyStore.LookupEntry(ci->family);

    if (!pet_family)
        return;

    if (pet_family->petTalentType < 0)                       // not hunter pet
        return;

    // prevent learn talent for different family (cheating)
    if (!((1 << pet_family->petTalentType) & talentTabInfo->petTalentMask))
        return;

    // find current max talent rank (0~5)
    uint8 curtalent_maxrank = 0; // 0 = not learned any rank
    for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
    {
        if (talentInfo->RankID[rank] && pet->HasSpell(talentInfo->RankID[rank]))
        {
            curtalent_maxrank = (rank + 1);
            break;
        }
    }

    // we already have same or higher talent rank learned
    if (curtalent_maxrank >= (talentRank + 1))
        return;

    // check if we have enough talent points
    if (CurTalentPoints < (talentRank - curtalent_maxrank + 1))
        return;

    // Check if it requires another talent
    if (talentInfo->DependsOn > 0)
    {
        if (TalentEntry const* depTalentInfo = sTalentStore.LookupEntry(talentInfo->DependsOn))
        {
            bool hasEnoughRank = false;
            for (uint8 rank = talentInfo->DependsOnRank; rank < MAX_TALENT_RANK; rank++)
            {
                if (depTalentInfo->RankID[rank] != 0)
                    if (pet->HasSpell(depTalentInfo->RankID[rank]))
                        hasEnoughRank = true;
            }
            if (!hasEnoughRank)
                return;
        }
    }

    // Find out how many points we have in this field
    uint32 spentPoints = 0;

    uint32 tTab = talentInfo->TalentTab;
    if (talentInfo->Row > 0)
    {
        uint32 numRows = sTalentStore.GetNumRows();
        for (uint32 i = 0; i < numRows; ++i)          // Loop through all talents.
        {
            // Someday, someone needs to revamp
            TalentEntry const* tmpTalent = sTalentStore.LookupEntry(i);
            if (tmpTalent)                                  // the way talents are tracked
            {
                if (tmpTalent->TalentTab == tTab)
                {
                    for (uint8 rank = 0; rank < MAX_TALENT_RANK; rank++)
                    {
                        if (tmpTalent->RankID[rank] != 0)
                        {
                            if (pet->HasSpell(tmpTalent->RankID[rank]))
                            {
                                spentPoints += (rank + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    // not have required min points spent in talent tree
    if (spentPoints < (talentInfo->Row * MAX_PET_TALENT_RANK))
        return;

    // spell not set in talent.dbc
    uint32 spellid = talentInfo->RankID[talentRank];
    if (spellid == 0)
    {
        LOG_ERROR("entities.player", "Talent.dbc have for talent: {} Rank: {} spell id = 0", talentId, talentRank);
        return;
    }

    // already known
    if (pet->HasSpell(spellid))
        return;

    // learn! (other talent ranks will unlearned at learning)
    pet->learnSpell(spellid);
    LOG_DEBUG("entities.player", "PetTalentID: {} Rank: {} Spell: {}\n", talentId, talentRank, spellid);

    // update free talent points
    pet->SetFreeTalentPoints(CurTalentPoints - (talentRank - curtalent_maxrank + 1));
}

void Player::BuildPlayerTalentsInfoData(WorldPacket* data)
{
    *data << uint32(GetFreeTalentPoints());                 // unspentTalentPoints
    *data << uint8(m_specsCount);                           // talent group count (0, 1 or 2)
    *data << uint8(m_activeSpec);                           // talent group index (0 or 1)

    if (m_specsCount > MAX_TALENT_SPECS)
        m_specsCount = MAX_TALENT_SPECS;

    for (uint32 specIdx = 0; specIdx < m_specsCount; ++specIdx)
    {
        uint8 talentIdCount = 0;
        std::size_t pos = data->wpos();
        *data << uint8(talentIdCount);                      // [PH], talentIdCount

        PlayerTalentMap const& talentMap = GetTalentMap();
        for (PlayerTalentMap::const_iterator itr = talentMap.begin(); itr != talentMap.end(); ++itr)
            if (TalentSpellPos const* talentPos = GetTalentSpellPos(itr->first))
                if (itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(specIdx)) // pussywizard
                {
                    *data << uint32(talentPos->talent_id);  // Talent.dbc
                    *data << uint8(talentPos->rank);        // talentMaxRank (0-4)
                    ++talentIdCount;
                }

        data->put<uint8>(pos, talentIdCount);               // put real count

        *data << uint8(MAX_GLYPH_SLOT_INDEX);               // glyphs count

        for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
            *data << uint16(m_Glyphs[specIdx][i]);          // GlyphProperties.dbc
    }
}

void Player::BuildPetTalentsInfoData(WorldPacket* data)
{
    uint32 unspentTalentPoints = 0;
    std::size_t pointsPos = data->wpos();
    *data << uint32(unspentTalentPoints);                   // [PH], unspentTalentPoints

    uint8 talentIdCount = 0;
    std::size_t countPos = data->wpos();
    *data << uint8(talentIdCount);                          // [PH], talentIdCount

    Pet* pet = GetPet();
    if (!pet)
        return;

    unspentTalentPoints = pet->GetFreeTalentPoints();

    data->put<uint32>(pointsPos, unspentTalentPoints);      // put real points

    CreatureTemplate const* ci = pet->GetCreatureTemplate();
    if (!ci)
        return;

    CreatureFamilyEntry const* pet_family = sCreatureFamilyStore.LookupEntry(ci->family);
    if (!pet_family || pet_family->petTalentType < 0)
        return;

    for (uint32 talentTabId = 1; talentTabId < sTalentTabStore.GetNumRows(); ++talentTabId)
    {
        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentTabId);
        if (!talentTabInfo)
            continue;

        if (!((1 << pet_family->petTalentType) & talentTabInfo->petTalentMask))
            continue;

        for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
            if (!talentInfo)
                continue;

            // skip another tab talents
            if (talentInfo->TalentTab != talentTabId)
                continue;

            // find max talent rank (0~4)
            int8 curtalent_maxrank = -1;
            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (talentInfo->RankID[rank] && pet->HasSpell(talentInfo->RankID[rank]))
                {
                    curtalent_maxrank = rank;
                    break;
                }
            }

            // not learned talent
            if (curtalent_maxrank < 0)
                continue;

            *data << uint32(talentInfo->TalentID);          // Talent.dbc
            *data << uint8(curtalent_maxrank);              // talentMaxRank (0-4)

            ++talentIdCount;
        }

        data->put<uint8>(countPos, talentIdCount);          // put real count

        break;
    }
}

void Player::SendTalentsInfoData(bool pet)
{
    WorldPacket data(SMSG_TALENTS_INFO, 50);
    data << uint8(pet ? 1 : 0);
    if (pet)
        BuildPetTalentsInfoData(&data);
    else
        BuildPlayerTalentsInfoData(&data);
    SendDirectMessage(&data);
}

void Player::_LoadGlyphs(PreparedQueryResult result)
{
    // SELECT talentGroup, glyph1, glyph2, glyph3, glyph4, glyph5, glyph6 from character_glyphs WHERE guid = '%u'
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        uint8 spec = fields[0].Get<uint8>();
        if (spec >= m_specsCount)
            continue;

        m_Glyphs[spec][0] = fields[1].Get<uint16>();
        m_Glyphs[spec][1] = fields[2].Get<uint16>();
        m_Glyphs[spec][2] = fields[3].Get<uint16>();
        m_Glyphs[spec][3] = fields[4].Get<uint16>();
        m_Glyphs[spec][4] = fields[5].Get<uint16>();
        m_Glyphs[spec][5] = fields[6].Get<uint16>();
    } while (result->NextRow());
}

void Player::_SaveGlyphs(CharacterDatabaseTransaction trans)
{
    if (!NeedToSaveGlyphs())
        return;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_GLYPHS);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    for (uint8 spec = 0; spec < m_specsCount; ++spec)
    {
        uint8 index = 0;

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_GLYPHS);
        stmt->SetData(index++, GetGUID().GetRawValue());
        stmt->SetData(index++, spec);

        for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
            stmt->SetData(index++, uint16(m_Glyphs[spec][i]));

        trans->Append(stmt);
    }

    SetNeedToSaveGlyphs(false);
}

void Player::_LoadTalents(PreparedQueryResult result)
{
    // SetQuery(PLAYER_LOGIN_QUERY_LOADTALENTS, "SELECT spell, specMask FROM character_talent WHERE guid = '{}'", m_guid.GetCounter());
    if (result)
    {
        do
        {
            // xinef: checked
            uint32 spellId = (*result)[0].Get<uint32>();
            uint8 specMask = (*result)[1].Get<uint8>();
            addTalent(spellId, specMask, 0);
            TalentSpellPos const* talentPos = GetTalentSpellPos(spellId);
            ASSERT(talentPos);

        } while (result->NextRow());
    }
}

void Player::_SaveTalents(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;

    for (PlayerTalentMap::iterator itr = m_talents.begin(); itr != m_talents.end();)
    {
        // xinef: skip temporary spells
        if (itr->second->State == PLAYERSPELL_TEMPORARY)
        {
            ++itr;
            continue;
        }

        // xinef: delete statement for removed / updated talent
        if (itr->second->State == PLAYERSPELL_REMOVED || itr->second->State == PLAYERSPELL_CHANGED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_TALENT_BY_SPELL);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, itr->first);
            trans->Append(stmt);
        }

        // xinef: insert statement for new / updated spell
        if (itr->second->State == PLAYERSPELL_NEW || itr->second->State == PLAYERSPELL_CHANGED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_TALENT);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, itr->first);
            stmt->SetData(2, itr->second->specMask);
            trans->Append(stmt);
        }

        if (itr->second->State == PLAYERSPELL_REMOVED)
        {
            delete itr->second;
            m_talents.erase(itr++);
        }
        else
        {
            itr->second->State = PLAYERSPELL_UNCHANGED;
            ++itr;
        }
    }
}

void Player::ActivateSpec(uint8 spec)
{
    // xinef: some basic checks
    if (GetActiveSpec() == spec)
        return;

    if (spec > GetSpecsCount())
        return;

    // xinef: interrupt currently casted spell just in case
    if (IsNonMeleeSpellCast(false))
        InterruptNonMeleeSpells(false);

    // xinef: save current actions order
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    _SaveActions(trans);
    CharacterDatabase.CommitTransaction(trans);

    // xinef: remove pet, it will be resummoned later
    if (Pet* pet = GetPet())
        RemovePet(pet, PET_SAVE_NOT_IN_SLOT);

    // xinef: remove other summoned units and clear reactives
    ClearAllReactives();
    UnsummonAllTotems();
    RemoveAllControlled();

    // xinef: let client clear his current Actions
    SendActionButtons(2);
    uint8 oldSpec = GetActiveSpec();

    std::unordered_set<uint32> removedSpecAuras;

    // xinef: reset talent auras
    for (PlayerTalentMap::iterator itr = m_talents.begin(); itr != m_talents.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        // xinef: remove all active talent auras
        if (!(itr->second->specMask & GetActiveSpecMask()))
            continue;

        _removeTalentAurasAndSpells(itr->first);

        // pussywizard: was => isn't
        if (!itr->second->IsInSpec(spec) && !itr->second->inSpellBook)
            SendLearnPacket(itr->first, false);

        removedSpecAuras.insert(itr->first);
    }

    // xinef: remove glyph auras
    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
        if (uint32 glyphId = m_Glyphs[GetActiveSpec()][slot])
            if (GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyphId))
            {
                RemoveAurasDueToSpell(glyphEntry->SpellId);
                removedSpecAuras.insert(glyphEntry->SpellId);
            }

    // xinef: set active spec as new one
    SetActiveSpec(spec);
    uint32 spentTalents = 0;

    // xinef: add talent auras
    for (PlayerTalentMap::iterator itr = m_talents.begin(); itr != m_talents.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        // xinef: talent not in new spec
        if (!(itr->second->specMask & GetActiveSpecMask()))
            continue;

        // pussywizard: wasn't => is
        if (!itr->second->IsInSpec(oldSpec) && !itr->second->inSpellBook)
            SendLearnPacket(itr->first, true);

        _addTalentAurasAndSpells(itr->first);
        TalentSpellPos const* talentPos = GetTalentSpellPos(itr->first);
        spentTalents += talentPos->rank + 1;

        removedSpecAuras.erase(itr->first);
    }

    // pussywizard: remove spells that are in previous spec, but are not present in new one (or are in new spec, but not in the old one)
    for (PlayerSpellMap::iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if (!itr->second->Active || itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        // pussywizard: was => isn't
        if (itr->second->IsInSpec(oldSpec) && !itr->second->IsInSpec(spec))
        {
            SendLearnPacket(itr->first, false);
            // We want to remove all auras of the unlearned spell
            _removeTalentAurasAndSpells(itr->first);

            removedSpecAuras.insert(itr->first);
        }
        // pussywizard: wasn't => is
        else if (!itr->second->IsInSpec(oldSpec) && itr->second->IsInSpec(spec))
        {
            SendLearnPacket(itr->first, true);

            removedSpecAuras.erase(itr->first);
        }
    }

    // xinef: apply glyphs from second spec
    if (GetActiveSpec() != oldSpec)
    {
        for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
        {
            uint32 glyphId = m_Glyphs[GetActiveSpec()][slot];
            if (glyphId)
            {
                if (GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyphId))
                {
                    CastSpell(this, glyphEntry->SpellId, TriggerCastFlags(TRIGGERED_FULL_MASK & ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
                    removedSpecAuras.erase(glyphEntry->SpellId);
                }
            }

            SetGlyph(slot, glyphId, true);
        }
    }

    // Remove auras triggered/activated by talents/glyphs
    // Mostly explicit casts in dummy aura scripts
    if (!removedSpecAuras.empty())
    {
        for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
        {
            Aura* aura = iter->second;
            if (SpellInfo const* triggeredByAuraSpellInfo = aura->GetTriggeredByAuraSpellInfo())
            {
                if (removedSpecAuras.find(triggeredByAuraSpellInfo->Id) != removedSpecAuras.end())
                {
                    RemoveOwnedAura(iter);
                    continue;
                }
            }
            ++iter;
        }
    }

    m_usedTalentCount = spentTalents;
    InitTalentForLevel();

    // load them asynchronously
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARACTER_ACTIONS_SPEC);
        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, m_activeSpec);

        WorldSession* mySess = GetSession();
        mySess->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(stmt)
        .WithPreparedCallback([mySess](PreparedQueryResult result)
        {
            // safe callback, we can't pass this pointer directly
            // in case player logs out before db response (player would be deleted in that case)
            if (Player* thisPlayer = mySess->GetPlayer())
                thisPlayer->LoadActions(result);
        }));
    }

    // xinef: reset power
    Powers pw = getPowerType();
    if (pw != POWER_MANA)
        SetPower(POWER_MANA, 0); // Mana must be 0 even if it isn't the active power type.
    SetPower(pw, 0);

    // xinef: remove titan grip if player had it set and does not have appropriate talent
    if (!HasTalent(46917, GetActiveSpec()) && m_canTitanGrip)
        SetCanTitanGrip(false);
    // xinef: remove dual wield if player does not have dual wield spell (shamans)
    if (!HasSpell(674) && CanDualWield())
        SetCanDualWield(false);

    AutoUnequipOffhandIfNeed();

    // Xinef: Patch 3.2.0: Switching spec removes paladins spell Righteous Fury (25780)
    if (IsClass(CLASS_PALADIN, CLASS_CONTEXT_ABILITY))
        RemoveAurasDueToSpell(25780);

    // Xinef: Remove talented single target auras at other targets
    AuraList& scAuras = GetSingleCastAuras();
    for (AuraList::iterator iter = scAuras.begin(); iter != scAuras.end();)
    {
        Aura* aura = *iter;
        if (!HasActiveSpell(aura->GetId()) && !HasTalent(aura->GetId(), GetActiveSpec()) && !aura->GetCastItemGUID())
        {
            aura->Remove();
            iter = scAuras.begin();
        }
        else
            ++iter;
    }

    // Recheck shapeshift bonus auras: drop and re-apply form-tied passives
    // so buffs from talents missing in the new spec (e.g. Master Shapeshifter) go away
    Unit::AuraEffectList const& shapeshiftAuras = GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT);
    for (AuraEffect* aurEff : shapeshiftAuras)
    {
        aurEff->HandleShapeshiftBoosts(this, false);
        aurEff->HandleShapeshiftBoosts(this, true);
    }

    sScriptMgr->OnPlayerAfterSpecSlotChanged(this, GetActiveSpec());
}

void Player::GetTalentTreePoints(uint8 (&specPoints)[3]) const
{
    PlayerTalentMap const& talentMap = GetTalentMap();
    for (PlayerTalentMap::const_iterator itr = talentMap.begin(); itr != talentMap.end(); ++itr)
        if (itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(GetActiveSpec()))
            if (TalentEntry const* talentInfo = sTalentStore.LookupEntry(itr->second->talentID))
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talentInfo->TalentTab))
                    if (tab->tabpage < 3)
                    {
                        // find current talent rank
                        uint8 currentTalentRank = 0;
                        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                            if (talentInfo->RankID[rank] && itr->first == talentInfo->RankID[rank])
                            {
                                currentTalentRank = rank + 1;
                                break;
                            }
                        specPoints[tab->tabpage] += currentTalentRank;
                    }
}

uint8 Player::GetMostPointsTalentTree() const
{
    uint32 specPoints[3] = {0, 0, 0};
    PlayerTalentMap const& talentMap = GetTalentMap();
    for (PlayerTalentMap::const_iterator itr = talentMap.begin(); itr != talentMap.end(); ++itr)
        if (itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(GetActiveSpec()))
            if (TalentEntry const* talentInfo = sTalentStore.LookupEntry(itr->second->talentID))
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talentInfo->TalentTab))
                    if (tab->tabpage < 3)
                    {
                        // find current talent rank
                        uint8 currentTalentRank = 0;
                        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                            if (talentInfo->RankID[rank] && itr->first == talentInfo->RankID[rank])
                            {
                                currentTalentRank = rank + 1;
                                break;
                            }
                        specPoints[tab->tabpage] += currentTalentRank;
                    }
    uint8 maxIndex = 0;
    uint8 maxCount = specPoints[0];
    for (uint8 i = 1; i < 3; ++i)
        if (specPoints[i] > maxCount)
        {
            maxIndex = i;
            maxCount = specPoints[i];
        }
    return maxIndex;
}

void Player::SetIsSpectator(bool on)
{
    if (on)
    {
        AddAura(SPECTATOR_SPELL_SPEED, this);
        m_ExtraFlags |= PLAYER_EXTRA_SPECTATOR_ON;
        AddUnitState(UNIT_STATE_ISOLATED);
        //SetFaction(1100);
        SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        if (HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
        {
            RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
            sScriptMgr->OnPlayerFfaPvpStateUpdate(this, false);
        }
        ResetContestedPvP();
        SetDisplayId(23691);
    }
    else
    {
        RemoveAurasDueToSpell(SPECTATOR_SPELL_SPEED);
        if (IsSpectator())
            ClearUnitState(UNIT_STATE_ISOLATED);
        m_ExtraFlags &= ~PLAYER_EXTRA_SPECTATOR_ON;
        RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        RestoreDisplayId();

        if (!IsGameMaster())
        {
            //SetFactionForRace(getRace());

            // restore FFA PvP Server state
            // Xinef: it will be removed if necessery in UpdateArea called in WorldPortOpcode
            if (sWorld->IsFFAPvPRealm())
            {
                if (!HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
                {
                    SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
                    sScriptMgr->OnPlayerFfaPvpStateUpdate(this, true);

                }
            }
        }
    }
}

bool Player::NeedSendSpectatorData() const
{
    if (FindMap() && FindMap()->IsBattleArena() && !IsSpectator())
    {
        Battleground* bg = ((BattlegroundMap*)FindMap())->GetBG();
        if (bg && bg->HaveSpectators() && bg->GetStatus() == STATUS_IN_PROGRESS && !bg->GetPlayers().empty())
            if (bg->GetPlayers().find(GetGUID()) != bg->GetPlayers().end())
                return true;
    }
    return false;
}

uint32 Player::GetSpec(int8 spec)
{
    uint32 mostTalentTabId = 0;
    uint32 mostTalentCount = 0;
    uint32 specIdx = 0;

    if (m_specsCount) // not all instances of Player have a spec for some reason
    {
        if (spec < 0)
            specIdx = m_activeSpec;
        else
            specIdx = spec;
        // find class talent tabs (all players have 3 talent tabs)
        uint32 const* talentTabIds = GetTalentTabPages(getClass());

        for (uint8 i = 0; i < MAX_TALENT_TABS; ++i)
        {
            uint32 talentCount = 0;
            uint32 talentTabId = talentTabIds[i];
            for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
            {
                TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
                if (!talentInfo)
                    continue;

                // skip another tab talents
                if (talentInfo->TalentTab != talentTabId)
                    continue;

                // find max talent rank (0~4)
                int8 curtalent_maxrank = -1;
                for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
                {
                    if (talentInfo->RankID[rank] && HasTalent(talentInfo->RankID[rank], specIdx))
                    {
                        curtalent_maxrank = rank;
                        break;
                    }
                }

                // not learned talent
                if (curtalent_maxrank < 0)
                    continue;

                talentCount += curtalent_maxrank + 1;
            }

            if (mostTalentCount < talentCount)
            {
                mostTalentCount = talentCount;
                mostTalentTabId = talentTabId;
            }
        }
    }
    return mostTalentTabId;
}

bool Player::HasTankSpec()
{
    switch (GetSpec())
    {
        case TALENT_TREE_WARRIOR_PROTECTION:
        case TALENT_TREE_PALADIN_PROTECTION:
        case TALENT_TREE_DEATH_KNIGHT_BLOOD:
            return true;
        case TALENT_TREE_DRUID_FERAL_COMBAT:
            if (GetShapeshiftForm() == FORM_BEAR || GetShapeshiftForm() == FORM_DIREBEAR)
                return true;
            break;
        default:
            break;
    }
    return false;
}

bool Player::HasMeleeSpec()
{
    switch (GetSpec(GetActiveSpec()))
    {
        case TALENT_TREE_WARRIOR_ARMS:
        case TALENT_TREE_WARRIOR_FURY:
        case TALENT_TREE_PALADIN_RETRIBUTION:
        case TALENT_TREE_ROGUE_ASSASSINATION:
        case TALENT_TREE_ROGUE_COMBAT:
        case TALENT_TREE_ROGUE_SUBTLETY:
        case TALENT_TREE_DEATH_KNIGHT_FROST:
        case TALENT_TREE_DEATH_KNIGHT_UNHOLY:
        case TALENT_TREE_SHAMAN_ENHANCEMENT:
            return true;
        case TALENT_TREE_DRUID_FERAL_COMBAT:
            if (GetShapeshiftForm() == FORM_CAT)
                return true;
        default:
            break;
    }
    return false;
}

bool Player::HasHealSpec()
{
    switch (GetSpec(GetActiveSpec()))
    {
        case TALENT_TREE_PALADIN_HOLY:
        case TALENT_TREE_PRIEST_DISCIPLINE:
        case TALENT_TREE_PRIEST_HOLY:
        case TALENT_TREE_SHAMAN_RESTORATION:
        case TALENT_TREE_DRUID_RESTORATION:
            return true;
        default:
            break;
    }
    return false;
}

uint16 Player::GetMaxSkillValueForLevel() const
{
    uint16 result = Unit::GetMaxSkillValueForLevel();

    sScriptMgr->OnPlayerGetMaxSkillValueForLevel(const_cast<Player*>(this), result);

    return result;
}

bool Player::IsSummonAsSpectator() const
{
    return m_summon_asSpectator && m_summon_expire >= GameTime::GetGameTime().count();
}
