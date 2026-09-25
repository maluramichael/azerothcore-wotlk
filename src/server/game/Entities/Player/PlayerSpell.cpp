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

void Player::ProcessDelayedOperations()
{
    if (m_DelayedOperations == 0)
        return;

    if (m_DelayedOperations & DELAYED_RESURRECT_PLAYER)
    {
        ResurrectPlayer(0.0f, false);

        if (GetMaxHealth() > m_resurrectHealth)
            SetHealth(m_resurrectHealth);
        else
            SetFullHealth();

        if (GetMaxPower(POWER_MANA) > m_resurrectMana)
            SetPower(POWER_MANA, m_resurrectMana);
        else
            SetPower(POWER_MANA, GetMaxPower(POWER_MANA));

        SetPower(POWER_RAGE, 0);
        SetPower(POWER_ENERGY, GetMaxPower(POWER_ENERGY));

        SpawnCorpseBones();
    }

    if (m_DelayedOperations & DELAYED_SAVE_PLAYER)
        SaveToDB(false, false);

    if ((m_DelayedOperations & DELAYED_SPELL_CAST_DESERTER)
        && !GetAura(26013))
            CastSpell(this, 26013, true);

    if (m_DelayedOperations & DELAYED_BG_MOUNT_RESTORE)
    {
        if (m_entryPointData.mountSpell)
        {
            // xinef: remove shapeshift auras
            if (IsInDisallowedMountForm())
            {
                RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            }
            AddAura(m_entryPointData.mountSpell, this);
            m_entryPointData.mountSpell = 0;
        }
    }

    if (m_DelayedOperations & DELAYED_BG_TAXI_RESTORE)
    {
        if (m_entryPointData.HasTaxiPath())
        {
            m_taxi.AddTaxiDestination(m_entryPointData.taxiPath[0]);
            m_taxi.AddTaxiDestination(m_entryPointData.taxiPath[1]);
            m_entryPointData.ClearTaxiPath();
            ContinueTaxiFlight();
        }
    }

    if (m_DelayedOperations & DELAYED_BG_GROUP_RESTORE)
    {
        if (Group* g = GetGroup())
            g->SendUpdateToPlayer(GetGUID());
    }

    if (m_DelayedOperations & DELAYED_VEHICLE_TELEPORT)
    {
        if (Vehicle* vehicle = GetVehicle())
        {
            SeatMap::iterator itr = vehicle->GetSeatIteratorForPassenger(this);
            if (itr != vehicle->Seats.end())
                if (Unit* base = vehicle->GetBase())
                {
                    ExitVehicle();
                    base->HandleSpellClick(this, itr->first);
                }
        }
    }

    //we have executed ALL delayed ops, so clear the flag
    m_DelayedOperations = 0;
}

void Player::SendInitialSpells()
{
    uint32 curTime = GameTime::GetGameTimeMS().count();
    uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;

    uint16 spellCount = 0;

    WorldPacket data(SMSG_INITIAL_SPELLS, (1 + 2 + 4 * m_spells.size() + 2 + m_spellCooldowns.size() * (4 + 2 + 2 + 4 + 4)));
    data << uint8(0);

    std::size_t countPos = data.wpos();
    data << uint16(spellCount);                             // spell count placeholder

    for (PlayerSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        if (!itr->second->Active || !itr->second->IsInSpec(GetActiveSpec()))
            continue;

        data << uint32(itr->first);
        data << uint16(0);                                  // it's not slot id

        ++spellCount;
    }

    // Added spells from glyphs too (needed by spell tooltips)
    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
    {
        if (uint32 glyph = GetGlyph(i))
        {
            if (GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyph))
            {
                data << uint32(glyphEntry->SpellId);
                data << uint16(0); // it's not slot id

                ++spellCount;
            }
        }
    }

    // xinef: we have to send talents, but not those on m_spells list
    for (PlayerTalentMap::iterator itr = m_talents.begin(); itr != m_talents.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;

        // xinef: remove all active talent auras
        if (!(itr->second->specMask & GetActiveSpecMask()))
            continue;

        // xinef: already sent from m_spells
        if (itr->second->inSpellBook)
            continue;

        data << uint32(itr->first);
        data << uint16(0);                                  // it's not slot id

        ++spellCount;
    }

    data.put<uint16>(countPos, spellCount);                  // write real count value

    uint16 spellCooldowns = m_spellCooldowns.size();
    data << uint16(spellCooldowns);
    for (SpellCooldowns::const_iterator itr = m_spellCooldowns.begin(); itr != m_spellCooldowns.end(); ++itr)
    {
        if (!itr->second.needSendToClient)
            continue;

        SpellInfo const* sEntry = sSpellMgr->GetSpellInfo(itr->first);
        if (!sEntry)
            continue;

        data << uint32(itr->first);

        data << uint16(itr->second.itemid);                 // cast item id
        data << uint16(itr->second.category);               // spell category

        // send infinity cooldown in special format
        if (itr->second.end >= infTime)
        {
            data << uint32(1);                              // cooldown
            data << uint32(0x80000000);                     // category cooldown
            continue;
        }

        uint32 cooldown = itr->second.end > curTime ? itr->second.end - curTime : 0;
        data << uint32(itr->second.category ? 0 : cooldown);    // cooldown
        data << uint32(itr->second.category ? cooldown : 0);    // category cooldown
    }

    SendDirectMessage(&data);
}

void Player::SendUnlearnSpells()
{
    WorldPacket data(SMSG_SEND_UNLEARN_SPELLS, 4 + 4 * m_spells.size());

    uint32 spellCount = 0;
    size_t countPos = data.wpos();
    data << uint32(spellCount);

    for (auto const& itr : m_spells)
    {
        if (itr.second->State == PLAYERSPELL_REMOVED || itr.second->Active)
            continue;

        auto skillLineAbilities = sSpellMgr->GetSkillLineAbilityMapBounds(itr.first);
        if (skillLineAbilities.first == skillLineAbilities.second)
            continue;

        // Client already hides ranks that have a superseding rank
        bool hasSupercedingRank = false;
        for (auto slaItr = skillLineAbilities.first; slaItr != skillLineAbilities.second; ++slaItr)
        {
            if (slaItr->second->SupercededBySpell)
            {
                hasSupercedingRank = true;
                break;
            }
        }
        if (hasSupercedingRank)
            continue;

        uint32 nextRank = sSpellMgr->GetNextSpellInChain(itr.first);
        if (!nextRank || !HasSpell(nextRank))
            continue;

        data << uint32(itr.first);
        ++spellCount;
    }

    data.put<uint32>(countPos, spellCount);
    SendDirectMessage(&data);
}

bool Player::IsUnlearnNeededForSpell(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(spellId);
    if (spellInfo->IsRanked() && !spellInfo->IsStackableWithRanks())
    {
        auto skillLineAbilities = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        if (skillLineAbilities.first != skillLineAbilities.second)
        {
            for (auto itr = skillLineAbilities.first; itr != skillLineAbilities.second; ++itr)
                if (itr->second->SupercededBySpell)
                    return false;

            return true;
        }
    }
    return false;
}

void Player::_removeTalentAurasAndSpells(uint32 spellId)
{
    RemoveOwnedAura(spellId);

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        // pussywizard: remove pet auras
        if (PetAura const* petSpell = sSpellMgr->GetPetAura(spellId, i))
            RemovePetAura(petSpell);

        // pussywizard: remove all triggered auras
        if (spellInfo->Effects[i].TriggerSpell > 0)
            RemoveAurasDueToSpell(spellInfo->Effects[i].TriggerSpell);

        // xinef: remove temporary spells added by talent
        // xinef: recursively remove all learnt spells
        if (spellInfo->Effects[i].TriggerSpell > 0 && spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL)
        {
            removeSpell(spellInfo->Effects[i].TriggerSpell, SPEC_MASK_ALL, true);
            _removeTalentAurasAndSpells(spellInfo->Effects[i].TriggerSpell);
        }
    }
}

void Player::_addTalentAurasAndSpells(uint32 spellId)
{
    // pussywizard: spells learnt from talents are added as TEMPORARY, so not saved to db (only the talent itself is saved)
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && !sSpellMgr->IsAdditionalTalentSpell(spellInfo->Effects[i].TriggerSpell))
                _addSpell(spellInfo->Effects[i].TriggerSpell, SPEC_MASK_ALL, true);
    }
    else if (spellInfo->IsPassive() || (spellInfo->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) && spellInfo->Stances))
    {
        if (IsNeedCastPassiveSpellAtLearn(spellInfo))
            CastSpell(this, spellId, true);
    }
}

bool Player::addSpell(uint32 spellId, uint8 addSpecMask, bool updateActive, bool temporary /*= false*/, bool learnFromSkill /*= false*/)
{
    if (!_addSpell(spellId, addSpecMask, temporary, learnFromSkill))
        return false;

    if (!updateActive)
        return true;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId); // must exist, checked in _addSpell

    // pussywizard: now update active state for all ranks of this spell! and send packet to swap on action bar
    // pussywizard: assumption - it's in all specs, can't be a talent
    if (!spellInfo->IsStackableWithRanks() && spellInfo->IsRanked())
    {
        SpellInfo const* nextSpellInfo = sSpellMgr->GetSpellInfo(sSpellMgr->GetFirstSpellInChain(spellInfo->Id));
        while (nextSpellInfo)
        {
            PlayerSpellMap::iterator itr = m_spells.find(nextSpellInfo->Id);
            if (itr != m_spells.end() && itr->second->State != PLAYERSPELL_REMOVED && itr->second->Active)
            {
                if (nextSpellInfo->GetRank() < spellInfo->GetRank())
                {
                    itr->second->Active = false;

                    if (!isBeingLoaded() && IsUnlearnNeededForSpell(spellId))
                        SendUnlearnSpells();

                    if (IsInWorld())
                    {
                        WorldPacket data(SMSG_SUPERCEDED_SPELL, 4 + 4);
                        data << uint32(nextSpellInfo->Id);
                        data << uint32(spellInfo->Id);
                        SendDirectMessage(&data);
                    }
                    return false;
                }
                else if (nextSpellInfo->GetRank() > spellInfo->GetRank())
                {
                    PlayerSpellMap::iterator itr2 = m_spells.find(spellInfo->Id);
                    if (itr2 != m_spells.end())
                        itr2->second->Active = false;

                    if (!isBeingLoaded() && IsUnlearnNeededForSpell(spellId))
                        SendUnlearnSpells();

                    return false;
                }
            }
            nextSpellInfo = nextSpellInfo->GetNextRankSpell();
        }
    }

    if (!isBeingLoaded() && IsUnlearnNeededForSpell(spellId))
        SendUnlearnSpells();

    return true;
}

bool Player::CheckSkillLearnedBySpell(uint32 spellId)
{
    if (!sWorld->getBoolConfig(CONFIG_VALIDATE_SKILL_LEARNED_BY_SPELLS))
        return true;

    SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    uint32 errorSkill = 0;
    for (SkillLineAbilityMap::const_iterator sla = skill_bounds.first; sla != skill_bounds.second; ++sla)
    {
        SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(sla->second->SkillLine);
        if (!pSkill)
            continue;

        if (GetSkillRaceClassInfo(pSkill->id, getRace(), getClass()))
            return true;
        else
            errorSkill = pSkill->id;
    }

    if (errorSkill)
    {
        LOG_ERROR("entities.player", "Player {} (GUID: {}), has spell ({}) that teach skill ({}) which is invalid for the race/class combination (Race: {}, Class: {}). Will be deleted.",
            GetName(), GetGUID().GetCounter(), spellId, errorSkill, getRace(), getClass());

        return false;
    }
    return true;
}

bool Player::IsNeedCastPassiveSpellAtLearn(SpellInfo const* spellInfo) const
{
    // note: form passives activated with shapeshift spells be implemented by HandleShapeshiftBoosts instead of spell_learn_spell
    // talent dependent passives activated at form apply have proper stance data
    ShapeshiftForm form = GetShapeshiftForm();
    return (!spellInfo->Stances || (form && (spellInfo->Stances & (1 << (form - 1)))) ||
            (!form && spellInfo->HasAttribute(SPELL_ATTR2_ALLOW_WHILE_NOT_SHAPESHIFTED)));
}

void Player::learnSpell(uint32 spellId, bool temporary /*= false*/, bool learnFromSkill /*= false*/)
{
    if (!sScriptMgr->OnPlayerCanLearnSpell(this, spellId))
        return;

    // Xinef: don't allow to learn active spell once more
    if (HasActiveSpell(spellId))
    {
        LOG_DEBUG("entities.player", "Player ({}) tries to learn already active spell: {}", GetGUID().ToString(), spellId);
        return;
    }

    uint8 const specMask = GetLearnSpellSpecMask(spellId);

    bool const added = addSpell(spellId, specMask, true, temporary, learnFromSkill);
    if (added)
    {
        sScriptMgr->OnPlayerLearnSpell(this, spellId);

        // pussywizard: a system message "you have learnt spell X (rank Y)"
        if (IsInWorld())
            SendLearnPacket(spellId, true);
    }

    // pussywizard: rank stuff at the end!
    if (uint32 nextSpell = sSpellMgr->GetNextSpellInChain(spellId))
    {
        // pussywizard: lookup next rank in m_spells (the only talents on m_spella are for example pyroblast, that have all ranks restored upon learning rank 1)
        // pussywizard: next ranks must not be in current spec (otherwise no need to learn already learnt)
        PlayerSpellMap::iterator itr = m_spells.find(nextSpell);
        if (itr != m_spells.end() && itr->second->State != PLAYERSPELL_REMOVED && !itr->second->IsInSpec(m_activeSpec))
            learnSpell(nextSpell, temporary);
    }

    // xinef: if we learn new spell, check all spells requiring this spell, if we have such a spell, and it is not in current spec - learn it
    SpellsRequiringSpellMapBounds spellsRequiringSpell = sSpellMgr->GetSpellsRequiringSpellBounds(spellId);
    for (SpellsRequiringSpellMap::const_iterator itr = spellsRequiringSpell.first; itr != spellsRequiringSpell.second; ++itr)
    {
        PlayerSpellMap::iterator itr2 = m_spells.find(itr->second);
        if (itr2 != m_spells.end() && itr2->second->State != PLAYERSPELL_REMOVED && !itr2->second->IsInSpec(m_activeSpec))
            learnSpell(itr2->first, temporary);
    }
}

uint8 Player::GetLearnSpellSpecMask(uint32 spellId) const
{
    uint32 const firstRankSpellId = sSpellMgr->GetFirstSpellInChain(spellId);

    bool const isTalentBasedSpell = GetTalentSpellCost(firstRankSpellId) > 0 || sSpellMgr->IsAdditionalTalentSpell(firstRankSpellId);

    // If this spell doesn't require any talents, learn it in all talent specs
    if (!isTalentBasedSpell)
        return SPEC_MASK_ALL;

    uint8 specMask = GetActiveSpecMask();

    // If the first rank of a talent-based spell has already been learned in another spec,
    // the following ranks should also be learned in that spec.
    if (m_spells.find(firstRankSpellId) != m_spells.end())
    {
        specMask |= m_spells.at(firstRankSpellId)->specMask;
    }

    // When learning a talent-based spell that has other spells as a requirement, it should not only be learned in the current spec,
    // but also in all other specs that have the required spells.
    // Example: Greater Blessing of Sanctuary has Blessing of Sanctuary as required spell.
    auto const spellsRequiredForSpellBounds = sSpellMgr->GetSpellsRequiredForSpellBounds(spellId);
    bool const spellHasRequiredSpells = (spellsRequiredForSpellBounds.begin() != spellsRequiredForSpellBounds.end());
    if (spellHasRequiredSpells)
    {
        uint8 requiredSpellsSpecMask = SPEC_MASK_ALL;
        for (SpellRequiredMap::const_iterator itr = spellsRequiredForSpellBounds.begin(); itr != spellsRequiredForSpellBounds.end(); ++itr)
        {
            uint32 const requiredSpellId = itr->second;
            bool const requiredSpellExistsAsPlayerSpell = (m_spells.find(requiredSpellId) != m_spells.end());

            // The required spell should usually exist at least in the current spec, but maybe we are learning a spell via GM command
            requiredSpellsSpecMask &= requiredSpellExistsAsPlayerSpell ? m_spells.at(requiredSpellId)->specMask : 0;
        }
        specMask |= requiredSpellsSpecMask;
    }

    return specMask;
}

void Player::removeSpell(uint32 spell_id, uint8 removeSpecMask, bool onlyTemporary)
{
    PlayerSpellMap::iterator itr = m_spells.find(spell_id);
    if (itr == m_spells.end())
        return;

    // pussywizard: nothing to do if already removed or not in specs of removeSpecMask
    if (itr->second->State == PLAYERSPELL_REMOVED || (itr->second->specMask & removeSpecMask) == 0)
        return;

    // pussywizard: avoid any possible bugs
    if (onlyTemporary && itr->second->State != PLAYERSPELL_TEMPORARY)
        return;

    // pussywizard: remove non-talent higher ranks (recursive)
    // pussywizard: do this at the beginning, not in the middle of removing!
    if (uint32 nextSpell = sSpellMgr->GetNextSpellInChain(spell_id))
        if (!GetTalentSpellPos(nextSpell))
            removeSpell(nextSpell, removeSpecMask, onlyTemporary);

    // xinef: if current spell has talentcost, remove spells requiring this spell
    uint32 firstRankSpellId = sSpellMgr->GetFirstSpellInChain(spell_id);
    if (GetTalentSpellCost(firstRankSpellId))
    {
        SpellsRequiringSpellMapBounds spellsRequiringSpell = sSpellMgr->GetSpellsRequiringSpellBounds(firstRankSpellId);
        for (auto spellsItr = spellsRequiringSpell.first; spellsItr != spellsRequiringSpell.second; ++spellsItr)
        {
            removeSpell(spellsItr->second, removeSpecMask, onlyTemporary);
        }
    }

    // pussywizard: re-search, it can be corrupted in prev loop
    itr = m_spells.find(spell_id);
    if (itr == m_spells.end())
        return;

    itr->second->specMask = (((uint8)itr->second->specMask) & ~removeSpecMask); // pussywizard: update specMask in map

    // pussywizard: some more conditions needed for spells like pyroblast (shouldn't be fully removed when not available in any spec, should stay in db with specMask = 0)
    if (GetTalentSpellCost(firstRankSpellId) == 0 && !sSpellMgr->IsAdditionalTalentSpell(firstRankSpellId) && itr->second->specMask == 0)
    {
        if (itr->second->State == PLAYERSPELL_NEW || itr->second->State == PLAYERSPELL_TEMPORARY)
        {
            delete itr->second;
            m_spells.erase(itr);
        }
        else
            itr->second->State = PLAYERSPELL_REMOVED;
    }
    else if (itr->second->State != PLAYERSPELL_NEW && itr->second->State != PLAYERSPELL_TEMPORARY)
        itr->second->State = PLAYERSPELL_CHANGED;

    // xinef: this is used for talents and they are not removed in removeSpell function...
    // xinef: however ill leave this here just in case
    // pussywizard: remove owned aura obtained from currently removed spell
    RemoveOwnedAura(spell_id);

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spell_id);
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        // pussywizard: remove pet auras
        if (PetAura const* petSpell = sSpellMgr->GetPetAura(spell_id, i))
            RemovePetAura(petSpell);

        // pussywizard: remove all triggered auras
        if (spellInfo->Effects[i].TriggerSpell > 0)
            RemoveAurasDueToSpell(spellInfo->Effects[i].TriggerSpell);
    }

    // pussywizard: update free primary prof points
    if (spellInfo->IsPrimaryProfessionFirstRank())
    {
        uint32 freeProfs = GetFreePrimaryProfessionPoints() + 1;
        if (freeProfs <= sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL))
            SetFreePrimaryProfessions(freeProfs);
    }

    // pussywizard: update 310 flyer
    if (Has310Flyer(false))
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED && spellInfo->Effects[i].CalcValue() == 310)
                Has310Flyer(true, spell_id);

    // pussywizard: remove dependent skill
    SpellLearnSkillNode const* spellLearnSkill = sSpellMgr->GetSpellLearnSkill(spell_id);
    if (spellLearnSkill)
    {
        uint32 prev_spell = sSpellMgr->GetPrevSpellInChain(spell_id);

        if (!prev_spell) // pussywizard: first rank, remove skill
            SetSkill(spellLearnSkill->skill, 0, 0, 0);
        else // pussywizard: search previous ranks
        {
            SpellLearnSkillNode const* prevSkill = sSpellMgr->GetSpellLearnSkill(prev_spell);
            while (!prevSkill && prev_spell)
            {
                prev_spell = sSpellMgr->GetPrevSpellInChain(prev_spell);
                prevSkill = sSpellMgr->GetSpellLearnSkill(sSpellMgr->GetFirstSpellInChain(prev_spell));
            }

            if (!prevSkill) // pussywizard: not found prev skill setting, remove skill
                SetSkill(spellLearnSkill->skill, 0, 0, 0);
            else // pussywizard: set to prev skill setting values
            {
                uint32 skill_value = GetPureSkillValue(prevSkill->skill);
                uint32 skill_max_value = GetPureMaxSkillValue(prevSkill->skill);
                uint32 new_skill_max_value = prevSkill->maxvalue == 0 ? GetMaxSkillValueForLevel() : prevSkill->maxvalue;

                if (skill_value > prevSkill->value)
                    skill_value = prevSkill->value;
                if (skill_max_value > new_skill_max_value)
                    skill_max_value = new_skill_max_value;

                SetSkill(prevSkill->skill, prevSkill->step, skill_value, skill_max_value);
            }
        }
    }
    else
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell_id);
        // most likely will never be used, haven't heard of cases where players unlearn a mount
        if (Has310Flyer(false) && spellInfo)
        {
            for (SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
            {
                SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(_spell_idx->second->SkillLine);
                if (!pSkill)
                    continue;

                if (_spell_idx->second->SkillLine == SKILL_MOUNTS)
                {
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    {
                        if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED &&
                            spellInfo->Effects[i].CalcValue() == 310)
                        {
                            Has310Flyer(true, spell_id);    // with true as first argument its also used to set/remove the flag
                            break;
                        }
                    }
                }
            }
        }
    }

    // pussywizard: remove from spell book (can't be replaced by previous rank, because such spells can't be unlearnt)
    if (!onlyTemporary || ((!spellInfo->HasAttribute(SpellAttr0(SPELL_ATTR0_PASSIVE | SPELL_ATTR0_DO_NOT_DISPLAY)) || !spellInfo->HasAnyAura()) && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL)))
    {
        sScriptMgr->OnPlayerForgotSpell(this, spell_id);
        SendLearnPacket(spell_id, false);
    }
}

void Player::RemoveSpellCooldown(uint32 spell_id, bool update /* = false */)
{
    m_spellCooldowns.erase(spell_id);

    if (update)
        SendClearCooldown(spell_id, this);
}

void Player::RemoveCategoryCooldown(uint32 cat)
{
    SpellCategoryStore::const_iterator i_scstore = sSpellsByCategoryStore.find(cat);
    if (i_scstore != sSpellsByCategoryStore.end())
        for (SpellCategorySet::const_iterator i_scset = i_scstore->second.begin(); i_scset != i_scstore->second.end(); ++i_scset)
            RemoveSpellCooldown(i_scset->second, true);
}

void Player::RemoveArenaSpellCooldowns(bool removeActivePetCooldowns)
{
    // remove cooldowns on spells that have < 10 min CD
    uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;
    SpellCooldowns::iterator itr, next;
    for (itr = m_spellCooldowns.begin(); itr != m_spellCooldowns.end(); itr = next)
    {
        next = itr;
        ++next;
        SpellInfo const* spellInfo = sSpellMgr->CheckSpellInfo(itr->first);
        if (!spellInfo)
        {
            continue;
        }

        if (spellInfo->HasAttribute(SPELL_ATTR4_IGNORE_DEFAULT_ARENA_RESTRICTIONS))
            RemoveSpellCooldown(itr->first, true);
        else if (spellInfo->RecoveryTime < 10 * MINUTE * IN_MILLISECONDS && spellInfo->CategoryRecoveryTime < 10 * MINUTE * IN_MILLISECONDS && itr->second.end < infTime// xinef: dont remove active cooldowns - bugz
                 && itr->second.maxduration < 10 * MINUTE * IN_MILLISECONDS) // xinef: dont clear cooldowns that have maxduration > 10 minutes (eg item cooldowns with no spell.dbc cooldown info)
            RemoveSpellCooldown(itr->first, true);
    }

    // pet cooldowns
    if (removeActivePetCooldowns)
        if (Pet* pet = GetPet())
        {
            // notify player
            for (CreatureSpellCooldowns::const_iterator itr2 = pet->m_CreatureSpellCooldowns.begin(); itr2 != pet->m_CreatureSpellCooldowns.end(); ++itr2)
                SendClearCooldown(itr2->first, pet);

            // actually clear cooldowns
            pet->m_CreatureSpellCooldowns.clear();
        }
}

void Player::RemoveAllSpellCooldown()
{
    uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;
    if (!m_spellCooldowns.empty())
    {
        for (SpellCooldowns::const_iterator itr = m_spellCooldowns.begin(); itr != m_spellCooldowns.end(); ++itr)
            if (itr->second.end < infTime)
                SendClearCooldown(itr->first, this);

        m_spellCooldowns.clear();
    }
}

void Player::_LoadSpellCooldowns(PreparedQueryResult result)
{
    // some cooldowns can be already set at aura loading...

    //QueryResult* result = CharacterDatabase.Query("SELECT spell, category, item, time FROM character_spell_cooldown WHERE guid = '{}'", GetGUID().GetCounter()());

    if (result)
    {
        time_t curTime = GameTime::GetGameTime().count();

        do
        {
            Field* fields = result->Fetch();
            uint32 spell_id = fields[0].Get<uint32>();
            uint16 category = fields[1].Get<uint16>();
            uint32 item_id  = fields[2].Get<uint32>();
            uint32 db_time  = fields[3].Get<uint32>();
            bool needSend   = fields[4].Get<bool>();

            if (!sSpellMgr->GetSpellInfo(spell_id))
            {
                LOG_ERROR("entities.player", "Player {} has unknown spell {} in `character_spell_cooldown`, skipping.", GetGUID().ToString(), spell_id);
                continue;
            }

            // skip outdated cooldown
            if (db_time <= curTime)
                continue;

            _AddSpellCooldown(spell_id, category, item_id, (db_time - curTime) * IN_MILLISECONDS, needSend);

            LOG_DEBUG("entities.player.loading", "Player ({}) spell {}, item {} cooldown loaded ({} secs).", GetGUID().ToString(), spell_id, item_id, uint32(db_time - curTime));
        } while (result->NextRow());
    }
}

void Player::_SaveSpellCooldowns(CharacterDatabaseTransaction trans, bool logout)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SPELL_COOLDOWN);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    time_t curTime = GameTime::GetGameTime().count();
    uint32 curMSTime = GameTime::GetGameTimeMS().count();
    uint32 infTime = curMSTime + infinityCooldownDelayCheck;

    bool first_round = true;
    std::ostringstream ss;

    // remove outdated and save active
    for (SpellCooldowns::iterator itr = m_spellCooldowns.begin(); itr != m_spellCooldowns.end();)
    {
        // Xinef: dummy cooldown for procs
        if (itr->first == uint32(-1))
        {
            ++itr;
            continue;
        }

        if (itr->second.end <= curMSTime + 1000)
            m_spellCooldowns.erase(itr++);
        else if (itr->second.end <= infTime && (logout || itr->second.end > (curMSTime + 5 * MINUTE * IN_MILLISECONDS)))             // not save locked cooldowns, it will be reset or set at reload
        {
            if (first_round)
            {
                ss << "INSERT INTO character_spell_cooldown (guid, spell, category, item, time, needSend) VALUES ";
                first_round = false;
            }
            // next new/changed record prefix
            else
                ss << ',';

            uint64 cooldown = uint64(((itr->second.end - curMSTime) / IN_MILLISECONDS) + curTime);
            ss << '(' << GetGUID().GetCounter() << ',' << itr->first << ',' << itr->second.category << "," << itr->second.itemid << ',' << cooldown << ',' << (itr->second.needSendToClient ? '1' : '0') << ')';
            ++itr;
        }
        else
            ++itr;
    }
    // if something changed execute
    if (!first_round)
        trans->Append(ss.str().c_str());
}

bool Player::HasSpell(uint32 spell) const
{
    PlayerSpellMap::const_iterator itr = m_spells.find(spell);
    return (itr != m_spells.end() && itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(m_activeSpec));
}

bool Player::HasActiveSpell(uint32 spell) const
{
    PlayerSpellMap::const_iterator itr = m_spells.find(spell);
    return (itr != m_spells.end() && itr->second->State != PLAYERSPELL_REMOVED && itr->second->Active && itr->second->IsInSpec(m_activeSpec));
}

float Player::GetSpellCritFromIntellect()
{
    uint8 level = GetLevel();
    uint32 pclass = getClass();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;

    GtChanceToSpellCritBaseEntry const* critBase  = sGtChanceToSpellCritBaseStore.LookupEntry(pclass - 1);
    GtChanceToSpellCritEntry     const* critRatio = sGtChanceToSpellCritStore.LookupEntry((pclass - 1) * GT_MAX_LEVEL + level - 1);
    if (!critBase || !critRatio)
        return 0.0f;

    float crit = critBase->base + GetStat(STAT_INTELLECT) * critRatio->ratio;
    return crit * 100.0f;
}

void Player::CheckAreaExploreAndOutdoor()
{
    if (!IsAlive())
        return;

    if (IsInFlight())
        return;

    bool isOutdoor = IsOutdoors();
    uint32 areaId = GetAreaId();
    AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaId);

    if (sWorld->getBoolConfig(CONFIG_VMAP_INDOOR_CHECK) && _wasOutdoor != isOutdoor)
    {
        _wasOutdoor = isOutdoor;

        SpellAttr0 attrToRemove = isOutdoor ? SPELL_ATTR0_ONLY_INDOORS : SPELL_ATTR0_ONLY_OUTDOORS;
        SpellAttr0 attrToRecalculate = isOutdoor ? SPELL_ATTR0_ONLY_OUTDOORS : SPELL_ATTR0_ONLY_INDOORS;
        for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
        {
            Aura* aura = iter->second->GetBase();
            SpellInfo const* spell = aura->GetSpellInfo();
            if (spell->Attributes & attrToRemove)
            {
                // if passive - do not remove and just turn off all effects
                if (aura->IsPassive())
                {
                    aura->HandleAllEffects(iter->second, AURA_EFFECT_HANDLE_REAL, false);
                    ++iter;
                    continue;
                }

                RemoveAura(iter);
            }
            else if ((spell->Attributes & attrToRecalculate) && aura->IsPassive())
            {
                // if passive - turn on all effects
                aura->HandleAllEffects(iter->second, AURA_EFFECT_HANDLE_REAL, true);
                ++iter;
            }
            else
            {
                ++iter;
            }
        }
    }

    if (!sScriptMgr->OnPlayerCanAreaExploreAndOutdoor(this))
        return;

    if (!areaId)
        return;

    if (!areaEntry)
    {
        LOG_ERROR("entities.player", "Player '{}' ({}) discovered unknown area (x: {} y: {} z: {} map: {})",
                       GetName(), GetGUID().ToString(), GetPositionX(), GetPositionY(), GetPositionZ(), GetMapId());
        return;
    }

    uint32 offset = areaEntry->exploreFlag / 32;

    if (offset >= PLAYER_EXPLORED_ZONES_SIZE)
    {
        LOG_ERROR("entities.player", "Wrong area flag {} in map data for (X: {} Y: {}) point to field PLAYER_EXPLORED_ZONES_1 + {} ( {} must be < {} ).", areaEntry->flags, GetPositionX(), GetPositionY(), offset, offset, PLAYER_EXPLORED_ZONES_SIZE);
        return;
    }

    uint32 val = (uint32)(1 << (areaEntry->exploreFlag % 32));
    uint32 currFields = GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset);

    if (!(currFields & val))
    {
        SetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset, (uint32)(currFields | val));

        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_EXPLORE_AREA, areaId);

        if (areaEntry->area_level > 0)
        {
            uint8 playerLevel = GetLevel();
            sScriptMgr->OnPlayerBeforeGetLevelForXPGain(this, playerLevel);

            if (playerLevel >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            {
                SendExplorationExperience(areaId, 0);
            }
            else
            {
                int32 diff = int32(playerLevel) - areaEntry->area_level;
                uint32 XP = 0;
                if (diff < -5)
                {
                    XP = uint32(sObjectMgr->GetBaseXP(playerLevel + 5) * sWorld->getRate(RATE_XP_EXPLORE));
                }
                else if (diff > 5)
                {
                    int32 exploration_percent = (100 - ((diff - 5) * 5));
                    if (exploration_percent > 100)
                        exploration_percent = 100;
                    else if (exploration_percent < 0)
                        exploration_percent = 0;

                    XP = uint32(sObjectMgr->GetBaseXP(areaEntry->area_level) * exploration_percent / 100 * sWorld->getRate(RATE_XP_EXPLORE));
                }
                else
                {
                    XP = uint32(sObjectMgr->GetBaseXP(areaEntry->area_level) * sWorld->getRate(RATE_XP_EXPLORE));
                }

                sScriptMgr->OnPlayerGiveXP(this, XP, nullptr, PlayerXPSource::XPSOURCE_EXPLORE);
                GiveXP(XP, nullptr);
                SendExplorationExperience(areaId, XP);
            }
            LOG_DEBUG("entities.player", "Player {} discovered a new area: {}", GetGUID().ToString(), areaId);
        }
    }
}

void Player::CastAllObtainSpells()
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ApplyItemObtainSpells(item, true);

    for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        Bag* bag = GetBagByPos(i);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (Item* item = bag->GetItemByPos(slot))
                ApplyItemObtainSpells(item, true);
    }
}

void Player::ApplyItemObtainSpells(Item* item, bool apply)
{
    ItemTemplate const* itemTemplate = item->GetTemplate();
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (itemTemplate->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_NO_DELAY_USE) // On obtain trigger
            continue;

        int32 const spellId = itemTemplate->Spells[i].SpellId;
        if (spellId <= 0)
            continue;

        if (apply)
        {
            if (!HasAura(spellId))
                CastSpell(this, spellId, true, item);
        }
        else
            RemoveAurasDueToSpell(spellId);
    }
}

void Player::UpdateItemObtainSpells(Item* item, uint8 bag, uint8 slot)
{
    if (IsBankPos(bag, slot))
        ApplyItemObtainSpells(item, false);
    else if (bag == INVENTORY_SLOT_BAG_0 || (bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END))
        ApplyItemObtainSpells(item, true);
}

void Player::UpdateWeaponDependentCritAuras(WeaponAttackType attackType)
{
    BaseModGroup modGroup;
    switch (attackType)
    {
        case BASE_ATTACK:
            modGroup = CRIT_PERCENTAGE;
            break;
        case OFF_ATTACK:
            modGroup = OFFHAND_CRIT_PERCENTAGE;
            break;
        case RANGED_ATTACK:
            modGroup = RANGED_CRIT_PERCENTAGE;
            break;
        default:
            ABORT();
            break;
    }

    float amount = 0.0f;
    amount += GetTotalAuraModifier(SPELL_AURA_MOD_WEAPON_CRIT_PERCENT, std::bind(&Unit::CheckAttackFitToAuraRequirement, this, attackType, std::placeholders::_1));

    // these auras don't have item requirement (only Combat Expertise in 3.3.5a)
    amount += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_PCT);

    SetBaseModFlatValue(modGroup, amount);
}

void Player::UpdateAllWeaponDependentCritAuras()
{
    for (uint8 i = BASE_ATTACK; i < MAX_ATTACK; ++i)
        UpdateWeaponDependentCritAuras(WeaponAttackType(i));
}

void Player::UpdateWeaponDependentAuras(WeaponAttackType attackType)
{
    UpdateWeaponDependentCritAuras(attackType);
    UpdateDamageDoneMods(attackType);
    UpdateDamagePctDoneMods(attackType);
}

void Player::ApplyItemDependentAuras(Item* item, bool apply)
{
    if (apply)
    {
        for (auto [spellId, playerSpell]: GetSpellMap())
        {
            if (playerSpell->State == PLAYERSPELL_REMOVED)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->IsPassive() || spellInfo->EquippedItemClass < 0)
                continue;

            if (!HasAura(spellId) && HasItemFitToSpellRequirements(spellInfo))
                AddAura(spellId, this);  // no SMSG_SPELL_GO in sniff found
        }

        // Check talents (they are stored separately from regular spells)
        for (auto [spellId, playerTalent] : GetTalentMap())
        {
            if (playerTalent->State == PLAYERSPELL_REMOVED)
                continue;

            if (!(playerTalent->IsInSpec(GetActiveSpec())))
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->IsPassive() || spellInfo->EquippedItemClass < 0)
                continue;

            if (!HasAura(spellId) && HasItemFitToSpellRequirements(spellInfo))
                AddAura(spellId, this);
        }
    }
    else
        RemoveItemDependentAurasAndCasts(item);
}

bool Player::CheckAttackFitToAuraRequirement(WeaponAttackType attackType, AuraEffect const* aurEff) const
{
    SpellInfo const* spellInfo = aurEff->GetSpellInfo();
    if (spellInfo->EquippedItemClass == -1)
        return true;

    Item* item = GetWeaponForAttack(attackType, true);
    if (!item || !item->IsFitToSpellRequirements(spellInfo))
        return false;

    return true;
}

void Player::ApplyItemEquipSpell(Item* item, bool apply, bool form_change)
{
    if (!item)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;

    for (auto const& spellData : proto->Spells)
    {
         // no spell
        if (!spellData.SpellId)
            continue;

        // wrong triggering type
        if (apply)
        {
            // Only apply "On Equip" spells
            if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_EQUIP)
                continue;
        }
        else
        {
            // Do not remove "Use" spells in these special cases:
            // 1. During form changes (e.g., druid shapeshifting)
            // 2. When the spell comes from an item with negative charges, which means its effect should persist after the item is consumed or removed.
            if (spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && (form_change || spellData.SpellCharges < 0))
                continue;
        }

        // check if it is valid spell
        SpellInfo const* spellproto = sSpellMgr->GetSpellInfo(spellData.SpellId);
        if (!spellproto)
            continue;

        ApplyEquipSpell(spellproto, item, apply, form_change);
    }
}

void Player::ApplyEquipSpell(SpellInfo const* spellInfo, Item* item, bool apply, bool form_change)
{
    if (apply)
    {
        if (!sScriptMgr->OnPlayerCanApplyEquipSpell(this, spellInfo, item, apply, form_change))
            return;

        // Cannot be used in this stance/form
        if (spellInfo->CheckShapeshift(GetShapeshiftForm()) != SPELL_CAST_OK)
            return;

        if (form_change)                                    // check aura active state from other form
        {
            AuraApplicationMapBounds range = GetAppliedAuras().equal_range(spellInfo->Id);
            for (AuraApplicationMap::const_iterator itr = range.first; itr != range.second; ++itr)
                if (!item || itr->second->GetBase()->GetCastItemGUID() == item->GetGUID())
                    return;
        }

        LOG_DEBUG("entities.player", "WORLD: cast {} Equip spellId - {}", (item ? "item" : "itemset"), spellInfo->Id);

        CastSpell(this, spellInfo, true, item);
    }
    else
    {
        if (form_change)                                     // check aura compatibility
        {
            // Cannot be used in this stance/form
            if (spellInfo->CheckShapeshift(GetShapeshiftForm()) == SPELL_CAST_OK)
                return;                                     // and remove only not compatible at form change
        }

        if (item)
            RemoveAurasDueToItemSpell(spellInfo->Id, item->GetGUID());  // un-apply all spells, not only at-equipped
        else
            RemoveAurasDueToSpell(spellInfo->Id);           // un-apply spell (item set case)
    }
}

void Player::CastItemCombatSpell(Unit* target, WeaponAttackType attType, uint32 procVictim, uint32 procEx)
{
    if (!target || !target->IsAlive() || target == this)
        return;

    // Xinef: do not use disarmed weapons, special exception - shaman ghost wolf form
    // Xinef: normal forms proc on hit enchants / built in item bonuses
    if (!CanUseAttackType(attType) || GetShapeshiftForm() == FORM_GHOSTWOLF)
        return;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        // If usable, try to cast item spell
        if (Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (!item->IsBroken())
                if (ItemTemplate const* proto = item->GetTemplate())
                {
                    // Additional check for weapons
                    if (proto->Class == ITEM_CLASS_WEAPON)
                    {
                        // offhand item cannot proc from main hand hit etc
                        EquipmentSlots slot;
                        switch (attType)
                        {
                            case BASE_ATTACK:
                                slot = EQUIPMENT_SLOT_MAINHAND;
                                break;
                            case OFF_ATTACK:
                                slot = EQUIPMENT_SLOT_OFFHAND;
                                break;
                            case RANGED_ATTACK:
                                slot = EQUIPMENT_SLOT_RANGED;
                                break;
                            default:
                                slot = EQUIPMENT_SLOT_END;
                                break;
                        }
                        if (slot != i)
                            continue;
                    }

                    CastItemCombatSpell(target, attType, procVictim, procEx, item, proto);
                }
    }
}

void Player::CastItemCombatSpell(Unit* target, WeaponAttackType attType, uint32 procVictim, uint32 procEx, Item* item, ItemTemplate const* proto)
{
    if (!sScriptMgr->OnPlayerCanCastItemCombatSpell(this, target, attType, procVictim, procEx, item, proto))
        return;

    // Can do effect if any damage done to target
    if (procVictim & PROC_FLAG_TAKEN_DAMAGE)
        //if (damageInfo->procVictim & PROC_FLAG_TAKEN_ANY_DAMAGE)
    {
        for (uint8 i = 0; i < MAX_ITEM_SPELLS; ++i)
        {
            _Spell const& spellData = proto->Spells[i];

            // no spell
            if (!spellData.SpellId)
                continue;

            // wrong triggering type
            if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_CHANCE_ON_HIT)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId);
            if (!spellInfo)
            {
                LOG_ERROR("entities.player", "WORLD: unknown Item spellid {}", spellData.SpellId);
                continue;
            }

            float chance = (float)spellInfo->ProcChance;

            if (spellData.SpellPPMRate)
            {
                uint32 WeaponSpeed = GetAttackTime(attType);
                chance = GetPPMProcChance(WeaponSpeed, spellData.SpellPPMRate, spellInfo);
            }
            else if (chance > 100.0f)
            {
                chance = GetWeaponProcChance();
            }

            if (roll_chance_f(chance) && sScriptMgr->OnCastItemCombatSpell(this, target, spellInfo, item))
                CastSpell(target, spellInfo->Id, TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD), item);
        }
    }

    // item combat enchantments
    for (uint8 e_slot = 0; e_slot < MAX_ENCHANTMENT_SLOT; ++e_slot)
    {
        uint32 enchant_id = item->GetEnchantmentId(EnchantmentSlot(e_slot));
        SpellItemEnchantmentEntry const* pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
        if (!pEnchant)
            continue;

        for (uint8 s = 0; s < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++s)
        {
            if (pEnchant->type[s] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
                continue;

            SpellEnchantProcEntry const* entry = sSpellMgr->GetSpellEnchantProcEvent(enchant_id);

            if (entry && entry->procEx)
            {
                // Check hit/crit/dodge/parry requirement
                if ((entry->procEx & procEx) == 0)
                    continue;
            }
            else
            {
                // Can do effect if any damage done to target
                if (!(procVictim & PROC_FLAG_TAKEN_DAMAGE))
                    //if (!(damageInfo->procVictim & PROC_FLAG_TAKEN_ANY_DAMAGE))
                    continue;
            }

            if (entry && (entry->attributeMask & ENCHANT_PROC_ATTR_WHITE_HIT) && (procVictim & SPELL_PROC_FLAG_MASK))
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(pEnchant->spellid[s]);
            if (!spellInfo)
            {
                LOG_ERROR("entities.player", "Player::CastItemCombatSpell({}, name: {}, enchant: {}): unknown spell {} is casted, ignoring...",
                               GetGUID().ToString(), GetName(), pEnchant->ID, pEnchant->spellid[s]);
                continue;
            }

            if (entry && (entry->attributeMask & ENCHANT_PROC_ATTR_EXCLUSIVE) != 0)
            {
                Unit* checkTarget = spellInfo->IsPositive() ? this : target;
                if (checkTarget->HasAura(spellInfo->Id, GetGUID()))
                {
                    continue;
                }
            }

            float chance = pEnchant->amount[s] != 0 ? float(pEnchant->amount[s]) : GetWeaponProcChance();

            if (entry)
            {
                if (entry->PPMChance)
                    chance = GetPPMProcChance(GetAttackTime(attType), entry->PPMChance, spellInfo);
                else if (entry->customChance)
                    chance = (float)entry->customChance;
            }

            // Apply spell mods
            ApplySpellMod(pEnchant->spellid[s], SPELLMOD_CHANCE_OF_SUCCESS, chance);

            // Shiv has 100% chance to apply the poison
            if (FindCurrentSpellBySpellId(5938) && e_slot == TEMP_ENCHANTMENT_SLOT)
                chance = 100.0f;

            if (roll_chance_f(chance))
            {
                // Xinef: implement enchant charges
                if (uint32 charges = item->GetEnchantmentCharges(EnchantmentSlot(e_slot)))
                {
                    if (!--charges)
                    {
                        ApplyEnchantment(item, EnchantmentSlot(e_slot), false);
                        item->ClearEnchantment(EnchantmentSlot(e_slot));
                    }
                    else
                        item->SetEnchantmentCharges(EnchantmentSlot(e_slot), charges);
                }

                Unit* unitTarget = spellInfo->IsPositive() ? this : target;
                CastSpell(unitTarget, spellInfo, TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD), item);
            }
        }
    }
}

void Player::CastItemUseSpell(Item* item, SpellCastTargets const& targets, uint8 cast_count, uint32 glyphIndex)
{
    if (!sScriptMgr->OnPlayerCanCastItemUseSpell(this, item, targets, cast_count, glyphIndex))
        return;

    ItemTemplate const* proto = item->GetTemplate();
    // special learning case
    if (proto->Spells[0].SpellId == 483 || proto->Spells[0].SpellId == 55884)
    {
        uint32 learn_spell_id = proto->Spells[0].SpellId;
        uint32 learning_spell_id = proto->Spells[1].SpellId;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(learn_spell_id);
        if (!spellInfo)
        {
            LOG_ERROR("entities.player", "Player::CastItemUseSpell: Item (Entry: {}) in have wrong spell id {}, ignoring ", proto->ItemId, learn_spell_id);
            SendEquipError(EQUIP_ERR_NONE, item, nullptr);
            return;
        }

        Spell* spell = new Spell(this, spellInfo, TRIGGERED_NONE);
        spell->m_CastItem = item;
        spell->m_cast_count = cast_count;                   //set count of casts
        spell->SetSpellValue(SPELLVALUE_BASE_POINT0, learning_spell_id);
        spell->prepare(&targets);
        return;
    }

    // use triggered flag only for items with many spell casts and for not first cast
    uint8 count = 0;

    std::list<Spell*> pushSpells;
    // item spells casted at use
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = proto->Spells[i];

        // no spell
        if (!spellData.SpellId)
            continue;

        // wrong triggering type
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId);
        if (!spellInfo)
        {
            LOG_ERROR("entities.player", "Player::CastItemUseSpell: Item (Entry: {}) in have wrong spell id {}, ignoring", proto->ItemId, spellData.SpellId);
            continue;
        }

        if (HasSpellCooldown(spellInfo->Id))
        {
            // Notify client so it can clean up the pending spell cast.
            // Without this the client orphans the cast and blocks auto-attack.
            Spell::SendCastResult(ToPlayer(), spellInfo, cast_count,
                SPELL_FAILED_NOT_READY);
            continue;
        }

        Spell* spell = new Spell(this, spellInfo, (count > 0) ? TRIGGERED_FULL_MASK : TRIGGERED_NONE);
        spell->m_CastItem = item;
        spell->m_cast_count = cast_count;                   // set count of casts
        spell->m_glyphIndex = glyphIndex;                   // glyph index
        spell->InitExplicitTargets(targets);

        // Xinef: dont allow to cast such spells, it may happen that spell possess 2 spells, one for players and one for items / gameobjects
        // Xinef: if first one is cast on player, it may be deleted thus resulting in crash because second spell has saved pointer to the item
        // Xinef: there is one problem with scripts which wont be loaded at the moment of call
        SpellCastResult result = spell->CheckCast(true);
        if (result != SPELL_CAST_OK)
        {
            spell->SendCastResult(result);
            delete spell;
            continue;
        }

        pushSpells.push_back(spell);
        //spell->prepare(&targets);

        ++count;
    }

    // Item enchantments spells casted at use
    for (uint8 e_slot = 0; e_slot < MAX_ENCHANTMENT_SLOT; ++e_slot)
    {
        uint32 enchant_id = item->GetEnchantmentId(EnchantmentSlot(e_slot));
        SpellItemEnchantmentEntry const* pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
        if (!pEnchant)
            continue;
        for (uint8 s = 0; s < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++s)
        {
            if (pEnchant->type[s] != ITEM_ENCHANTMENT_TYPE_USE_SPELL)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(pEnchant->spellid[s]);
            if (!spellInfo)
            {
                LOG_ERROR("entities.player", "Player::CastItemUseSpell Enchant {}, cast unknown spell {}", pEnchant->ID, pEnchant->spellid[s]);
                continue;
            }

            if (HasSpellCooldown(spellInfo->Id))
            {
                Spell::SendCastResult(ToPlayer(), spellInfo, cast_count,
                    SPELL_FAILED_NOT_READY);
                continue;
            }

            Spell* spell = new Spell(this, spellInfo, (count > 0) ? TRIGGERED_FULL_MASK : TRIGGERED_NONE);
            spell->m_CastItem = item;
            spell->m_cast_count = cast_count;               // set count of casts
            spell->m_glyphIndex = glyphIndex;               // glyph index
            spell->InitExplicitTargets(targets);

            // Xinef: dont allow to cast such spells, it may happen that spell possess 2 spells, one for players and one for items / gameobjects
            // Xinef: if first one is cast on player, it may be deleted thus resulting in crash because second spell has saved pointer to the item
            // Xinef: there is one problem with scripts which wont be loaded at the moment of call
            SpellCastResult result = spell->CheckCast(true);
            if (result != SPELL_CAST_OK)
            {
                spell->SendCastResult(result);
                delete spell;
                continue;
            }

            pushSpells.push_back(spell);
            //spell->prepare(&targets);

            ++count;
        }
    }

    // xinef: send all spells in one go, prevents crash because container is not set
    for (std::list<Spell*>::const_iterator itr = pushSpells.begin(); itr != pushSpells.end(); ++itr)
        (*itr)->prepare(&targets);
}

void Player::StopCastingCharm(Aura* except /*= nullptr*/)
{
    Unit* charm = GetCharm();
    if (!charm)
    {
        return;
    }

    if (charm->IsCreature())
    {
        if (charm->ToCreature()->HasUnitTypeMask(UNIT_MASK_PUPPET))
        {
            ((Puppet*)charm)->UnSummon();
        }
        else if (charm->IsVehicle())
        {
            ExitVehicle();
        }
    }

    if (GetCharmGUID())
    {
        charm->RemoveAurasByType(SPELL_AURA_MOD_CHARM, ObjectGuid::Empty, except);
        charm->RemoveAurasByType(SPELL_AURA_MOD_POSSESS_PET, ObjectGuid::Empty, except);
        charm->RemoveAurasByType(SPELL_AURA_MOD_POSSESS, ObjectGuid::Empty, except);
        charm->RemoveAurasByType(SPELL_AURA_AOE_CHARM, ObjectGuid::Empty, except);
    }

    if (GetCharmGUID())
    {
        LOG_FATAL("entities.player", "Player {} ({} is not able to uncharm unit ({})", GetName(), GetGUID().ToString(), GetCharmGUID().ToString());

        if (charm->GetCharmerGUID())
        {
            LOG_FATAL("entities.player", "Charmed unit has charmer {}", charm->GetCharmerGUID().ToString());
            ABORT();
        }
        else
        {
            SetCharm(charm, false);
        }
    }
}

void Player::PetSpellInitialize()
{
    Pet* pet = GetPet();

    if (!pet)
        return;

    LOG_DEBUG("entities.pet", "Pet Spells Groups");

    CharmInfo* charmInfo = pet->GetCharmInfo();

    WorldPacket data(SMSG_PET_SPELLS, 8 + 2 + 4 + 4 + 4 * MAX_UNIT_ACTION_BAR_INDEX + 1 + 1);
    data << pet->GetGUID();
    data << uint16(pet->GetCreatureTemplate()->family);         // creature family (required for pet talents)
    data << uint32(pet->GetDuration().count());
    data << uint8(pet->GetReactState());
    data << uint8(charmInfo->GetCommandState());
    data << uint16(0); // Flags, mostly unknown

    // action bar loop
    charmInfo->BuildActionBar(&data);

    std::size_t spellsCountPos = data.wpos();

    // spells count
    uint8 addlist = 0;
    data << uint8(addlist);                                 // placeholder

    if (pet->IsPermanentPetFor(this))
    {
        // spells loop
        for (PetSpellMap::iterator itr = pet->m_spells.begin(); itr != pet->m_spells.end(); ++itr)
        {
            if (itr->second.state == PETSPELL_REMOVED)
                continue;

            data << uint32(MAKE_UNIT_ACTION_BUTTON(itr->first, itr->second.active));
            ++addlist;
        }
    }

    data.put<uint8>(spellsCountPos, addlist);

    uint8 cooldownsCount = pet->m_CreatureSpellCooldowns.size();
    data << uint8(cooldownsCount);

    uint32 curTime = GameTime::GetGameTimeMS().count();
    uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;

    for (CreatureSpellCooldowns::const_iterator itr = pet->m_CreatureSpellCooldowns.begin(); itr != pet->m_CreatureSpellCooldowns.end(); ++itr)
    {
        uint16 category = itr->second.category;
        uint32 cooldown = (itr->second.end > curTime) ? itr->second.end - curTime : 0;

        data << uint32(itr->first);                         // spellid
        data << uint16(itr->second.category);               // spell category

        // send infinity cooldown in special format
        if (itr->second.end >= infTime)
        {
            data << uint32(1);                              // cooldown
            data << uint32(0x80000000);                     // category cooldown
            continue;
        }

        data << uint32(category ? 0 : cooldown);            // cooldown
        data << uint32(category ? cooldown : 0);            // category cooldown
    }

    SendDirectMessage(&data);
}

void Player::PossessSpellInitialize()
{
    Unit* charm = GetCharm();
    if (!charm)
        return;

    CharmInfo* charmInfo = charm->GetCharmInfo();

    if (!charmInfo)
    {
        LOG_ERROR("entities.player", "Player::PossessSpellInitialize(): charm ({}) has no charminfo!", charm->GetGUID().ToString());
        return;
    }

    WorldPacket data(SMSG_PET_SPELLS, 8 + 2 + 4 + 4 + 4 * MAX_UNIT_ACTION_BAR_INDEX + 1 + 1);
    data << charm->GetGUID();
    data << uint16(0);
    data << uint32(0);
    data << uint32(0);

    charmInfo->BuildActionBar(&data);

    data << uint8(0);                                       // spells count
    data << uint8(0);                                       // cooldowns count

    SendDirectMessage(&data);
}

void Player::VehicleSpellInitialize()
{
    Creature* vehicle = GetVehicleCreatureBase();
    if (!vehicle)
        return;

    uint8 cooldownCount = vehicle->m_CreatureSpellCooldowns.size();

    WorldPacket data(SMSG_PET_SPELLS, 8 + 2 + 4 + 4 + 4 * 10 + 1 + 1 + cooldownCount * (4 + 2 + 4 + 4));
    data << vehicle->GetGUID();                             // Guid
    data << uint16(0);                                      // Pet Family (0 for all vehicles)
    data << uint32(vehicle->IsSummon() ? vehicle->ToTempSummon()->GetTimer() : 0); // Duration
    // The following three segments are read by the client as one uint32
    data << uint8(vehicle->GetReactState());                // React State
    data << uint8(0);                                       // Command State
    data << uint16(0x800);                                  // DisableActions (set for all vehicles)

    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; ++i)
    {
        uint32 spellId = vehicle->m_spells[i];
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
        {
            data << uint16(0) << uint8(0) << uint8(i + 8);
            continue;
        }

        ConditionList conditions = sConditionMgr->GetConditionsForVehicleSpell(vehicle->GetEntry(), spellId);
        if (!sConditionMgr->IsObjectMeetToConditions(this, vehicle, conditions))
        {
            LOG_DEBUG("condition", "VehicleSpellInitialize: conditions not met for Vehicle entry {} spell {}", vehicle->ToCreature()->GetEntry(), spellId);
            data << uint16(0) << uint8(0) << uint8(i + 8);
            continue;
        }

        if (spellInfo->IsPassive())
            vehicle->CastSpell(vehicle, spellId, true);

        data << uint32(MAKE_UNIT_ACTION_BUTTON(spellId, i + 8));
    }

    for (uint32 i = MAX_CREATURE_SPELLS; i < MAX_SPELL_CONTROL_BAR; ++i)
        data << uint32(0);

    data << uint8(0); // Auras?

    // Cooldowns
    data << uint8(cooldownCount);

    uint32 curTime = GameTime::GetGameTimeMS().count();
    uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;

    for (CreatureSpellCooldowns::const_iterator itr = vehicle->m_CreatureSpellCooldowns.begin(); itr != vehicle->m_CreatureSpellCooldowns.end(); ++itr)
    {
        uint16 category = itr->second.category;
        uint32 cooldown = (itr->second.end > curTime) ? itr->second.end - curTime : 0;

        data << uint32(itr->first);              // spellid
        data << uint16(itr->second.category);    // spell category

        // send infinity cooldown in special format
        if (itr->second.end >= infTime)
        {
            data << uint32(1);                  // cooldown
            data << uint32(0x80000000);         // category cooldown
            continue;
        }

        data << uint32(category ? 0 : cooldown); // cooldown
        data << uint32(category ? cooldown : 0); // category cooldown
    }

    SendDirectMessage(&data);
}

void Player::CharmSpellInitialize()
{
    Unit* charm = GetFirstControlled();
    if (!charm)
        return;

    CharmInfo* charmInfo = charm->GetCharmInfo();
    if (!charmInfo)
    {
        LOG_ERROR("entities.player", "Player::CharmSpellInitialize(): the player's charm ({}) has no charminfo!", charm->GetGUID().ToString());
        return;
    }

    uint8 addlist = 0;
    if (!charm->IsPlayer())
    {
        //CreatureInfo const* cinfo = charm->ToCreature()->GetCreatureTemplate();
        //if (cinfo && cinfo->type == CREATURE_TYPE_DEMON && getClass() == CLASS_WARLOCK)
        {
            for (uint32 i = 0; i < MAX_SPELL_CHARM; ++i)
                if (charmInfo->GetCharmSpell(i)->GetAction())
                    ++addlist;
        }
    }

    WorldPacket data(SMSG_PET_SPELLS, 8 + 2 + 4 + 4 + 4 * MAX_UNIT_ACTION_BAR_INDEX + 1 + 4 * addlist + 1);
    data << charm->GetGUID();
    data << uint16(0);
    data << uint32(0);

    if (!charm->IsPlayer())
        data << uint8(charm->ToCreature()->GetReactState()) << uint8(charmInfo->GetCommandState()) << uint16(0);
    else
        data << uint32(0);

    charmInfo->BuildActionBar(&data);

    data << uint8(addlist);

    if (addlist)
    {
        for (uint32 i = 0; i < MAX_SPELL_CHARM; ++i)
        {
            CharmSpellInfo* cspell = charmInfo->GetCharmSpell(i);
            if (cspell->GetAction())
                data << uint32(cspell->packedData);
        }
    }

    data << uint8(0);                                       // cooldowns count

    SendDirectMessage(&data);
}

bool Player::HasSpellMod(SpellModifier* mod, Spell* spell)
{
    if (!mod || !spell)
        return false;

    return spell->m_appliedMods.find(mod->ownerAura) != spell->m_appliedMods.end();
}

bool Player::IsAffectedBySpellmod(SpellInfo const* spellInfo, SpellModifier* mod, Spell* spell)
{
    if (!mod || !spellInfo)
        return false;

    // First time this aura applies a mod to us and is out of charges
    if (spell && mod->ownerAura && mod->ownerAura->IsUsingCharges() && !mod->ownerAura->GetCharges() && !spell->m_appliedMods.count(mod->ownerAura))
        return false;

    // +duration to infinite duration spells making them limited
    if (mod->op == SPELLMOD_DURATION && spellInfo->GetDuration() == -1)
        return false;

    return spellInfo->IsAffectedBySpellMod(mod);
}

template <class T>
void Player::ApplySpellMod(uint32 spellId, SpellModOp op, T& basevalue, Spell* spell, bool temporaryPet)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return;

    float totalmul = 1.0f;
    int32 totalflat = 0;

    auto calculateSpellMod = [&](SpellModifier* mod)
    {
        // xinef: temporary pets cannot use charged mods of owner, needed for mirror image QQ they should use their own auras
        if (temporaryPet && mod->ownerAura && mod->ownerAura->IsUsingCharges())
            return;

        // skip if already instant or cost is free
        if (mod->op == SPELLMOD_CASTING_TIME || mod->op == SPELLMOD_COST)
        {
            float currentVal = ((float)basevalue + (float)totalflat) * totalmul;
            if (currentVal <= 0.0f)
                return;
        }

        if (mod->type == SPELLMOD_FLAT)
        {
            // xinef: do not allow to consume more than one 100% crit increasing spell
            if (mod->op == SPELLMOD_CRITICAL_CHANCE && totalflat >= 100)
                return;

            int32 flatValue = mod->value;

            // SPELL_MOD_THREAT - divide by 100 (in packets we send threat * 100)
            if (mod->op == SPELLMOD_THREAT)
                flatValue /= 100;

            totalflat += flatValue;
        }
        else if (mod->type == SPELLMOD_PCT)
        {
            // skip percent mods for null basevalue (most important for spell mods with charges)
            if (basevalue == T(0) || totalmul == 0.0f)
                return;

            // special case (skip > 10sec spell casts for instant cast setting)
            if (mod->op == SPELLMOD_CASTING_TIME && basevalue >= T(10000) && mod->value <= -100)
                return;
            // xinef: special exception for surge of light, dont affect crit chance if previous mods were not applied
            else if (mod->op == SPELLMOD_CRITICAL_CHANCE && !HasSpellModApplied(mod, spell))
                return;
            // xinef: special case for backdraft gcd reduce with backlast time reduction, dont affect gcd if cast time was not applied
            else if (mod->op == SPELLMOD_GLOBAL_COOLDOWN && !HasSpellModApplied(mod, spell))
                return;

            // xinef: those two mods should be multiplicative (Glyph of Renew)
            if (mod->op == SPELLMOD_DAMAGE || mod->op == SPELLMOD_DOT)
                totalmul *= CalculatePct(1.0f, 100.0f + mod->value);
            else
                totalmul += CalculatePct(1.0f, mod->value);
        }

        ApplyModToSpell(mod, spell);
    };

    // Drop charges for triggering spells instead of triggered ones
    if (m_spellModTakingSpell)
        spell = m_spellModTakingSpell;

    for (auto mod : m_spellMods[op])
    {
        if (!IsAffectedBySpellmod(spellInfo, mod, spell))
            continue;

        calculateSpellMod(mod);
    }

    if (op == SPELLMOD_CASTING_TIME || op == SPELLMOD_DURATION || op == SPELLMOD_COST)
        basevalue = (basevalue + totalflat) > 0 ? (basevalue + totalflat) * totalmul : 0;
    else
        basevalue = (basevalue * totalmul) + totalflat;
}

template AC_GAME_API void Player::ApplySpellMod(uint32 spellId, SpellModOp op, int32& basevalue, Spell* spell, bool temporaryPet);

template AC_GAME_API void Player::ApplySpellMod(uint32 spellId, SpellModOp op, uint32& basevalue, Spell* spell, bool temporaryPet);

template AC_GAME_API void Player::ApplySpellMod(uint32 spellId, SpellModOp op, float& basevalue, Spell* spell, bool temporaryPet);

void Player::AddSpellMod(SpellModifier* mod, bool apply)
{
    LOG_DEBUG("spells.aura", "Player::AddSpellMod {}", mod->spellId);
    uint16 Opcode = (mod->type == SPELLMOD_FLAT) ? SMSG_SET_FLAT_SPELL_MODIFIER : SMSG_SET_PCT_SPELL_MODIFIER;

    int i = 0;
    flag96 _mask = 0;
    for (int eff = 0; eff < 96; ++eff)
    {
        if (eff != 0 && eff % 32 == 0)
            _mask[i++] = 0;

        _mask[i] = uint32(1) << (eff - (32 * i));
        if (mod->mask & _mask)
        {
            int32 val = 0;
            for (SpellModContainer::iterator itr = m_spellMods[mod->op].begin(); itr != m_spellMods[mod->op].end(); ++itr)
            {
                if ((*itr)->type == mod->type && (*itr)->mask & _mask)
                    val += (*itr)->value;
            }
            val += apply ? mod->value : -(mod->value);
            WorldPacket data(Opcode, (1 + 1 + 4));
            data << uint8(eff);
            data << uint8(mod->op);
            data << int32(val);
            SendDirectMessage(&data);
        }
    }

    if (apply)
    {
        m_spellMods[mod->op].insert(mod);
    }
    else
    {
        m_spellMods[mod->op].erase(mod);
        // mods bound to aura will be removed in AuraEffect::~AuraEffect
        if (!mod->ownerAura)
            delete mod;
    }
}

void Player::RestoreSpellMods(Spell* spell, uint32 ownerAuraId, Aura* aura)
{
    if (!spell || spell->m_appliedMods.empty())
        return;

    std::list<Aura*> aurasQueue;

    for (uint8 i = 0; i < MAX_SPELLMOD; ++i)
    {
        for (SpellModContainer::iterator itr = m_spellMods[i].begin(); itr != m_spellMods[i].end(); ++itr)
        {
            SpellModifier* mod = *itr;

            // Spellmods without aura set cannot be charged
            if (!mod->ownerAura || !mod->ownerAura->IsUsingCharges())
                continue;

            // Restore only specific owner aura mods
            if (ownerAuraId && (ownerAuraId != mod->ownerAura->GetSpellInfo()->Id))
                continue;

            if (aura && mod->ownerAura != aura)
                continue;

            // Check if mod affected this spell
            // First, check if the mod aura applied at least one spellmod to this spell
            Spell::UsedSpellMods::iterator iterMod = spell->m_appliedMods.find(mod->ownerAura);
            if (iterMod == spell->m_appliedMods.end())
                continue;
            // Second, check if the current mod is one of those applied by the mod aura
            if (!(mod->mask & spell->m_spellInfo->SpellFamilyFlags))
                continue;

            // remove from list - This will be done after all mods have been gone through
            // to ensure we iterate over all mods of an aura before removing said aura
            // from applied mods (Else, an aura with two mods on the current spell would
            // only see the first of its modifier restored)
            aurasQueue.push_back(mod->ownerAura);
        }
    }

    for (std::list<Aura*>::iterator itr = aurasQueue.begin(); itr != aurasQueue.end(); ++itr)
    {
        Spell::UsedSpellMods::iterator iterMod = spell->m_appliedMods.find(*itr);
        if (iterMod != spell->m_appliedMods.end())
            spell->m_appliedMods.erase(iterMod);
    }

    // Xinef: clear the list just do be sure
    if (!ownerAuraId && !aura)
        spell->m_appliedMods.clear();
}

void Player::RestoreAllSpellMods(uint32 ownerAuraId, Aura* aura)
{
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
        if (m_currentSpells[i])
            RestoreSpellMods(m_currentSpells[i], ownerAuraId, aura);
}

void Player::RemoveSpellMods(Spell* spell)
{
    if (!spell)
        return;

    if (spell->m_appliedMods.empty())
        return;

    for (uint8 i = 0; i < MAX_SPELLMOD; ++i)
    {
        for (SpellModContainer::const_iterator itr = m_spellMods[i].begin(); itr != m_spellMods[i].end();)
        {
            SpellModifier* mod = *itr;
            ++itr;

            // don't handle spells with spell_proc entry defined
            // this is a temporary workaround, because all spellmods should be handled like that
            if (sSpellMgr->GetSpellProcEntry(mod->spellId))
            {
                continue;
            }

            // spellmods without aura set cannot be charged
            if (!mod->ownerAura || !mod->ownerAura->IsUsingCharges())
                continue;

            // check if mod affected this spell
            Spell::UsedSpellMods::iterator iterMod = spell->m_appliedMods.find(mod->ownerAura);
            if (iterMod == spell->m_appliedMods.end())
                continue;

            // remove from list
            // leave this here, if spell have two mods it will remove 2 charges - wrong
            spell->m_appliedMods.erase(iterMod);

            if (mod->ownerAura->DropCharge(AURA_REMOVE_BY_EXPIRE))
                itr = m_spellMods[i].begin();
        }
    }
}

void Player::ApplyModToSpell(SpellModifier* mod, Spell* spell)
{
    if (!spell)
        return;

    // don't do anything with no charges
    if (mod->ownerAura->IsUsingCharges() && !mod->ownerAura->GetCharges())
        return;

    // register inside spell, proc system uses this to drop charges
    spell->m_appliedMods.insert(mod->ownerAura);
}

bool Player::HasSpellModApplied(SpellModifier* mod, Spell* spell)
{
    if (!spell)
        return false;

    return spell->m_appliedMods.count(mod->ownerAura) != 0;
}

void Player::SetSpellModTakingSpell(Spell* spell, bool apply)
{
    if (apply && m_spellModTakingSpell != nullptr)
        return;

    if (!apply && (!m_spellModTakingSpell || m_spellModTakingSpell != spell))
        return;

    m_spellModTakingSpell = apply ? spell : nullptr;
}

void Player::ProhibitSpellSchool(SpellSchoolMask idSchoolMask, uint32 unTimeMs)
{
    PacketCooldowns cooldowns;
    WorldPacket data;

    for (PlayerSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED)
            continue;
        uint32 unSpellId = itr->first;
        SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(unSpellId);

        // Not send cooldown for this spells
        if (spellInfo->IsCooldownStartedOnEvent())
            continue;

        if (spellInfo->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
            continue;

        if ((idSchoolMask & spellInfo->GetSchoolMask()) && GetSpellCooldownDelay(unSpellId) < unTimeMs)
        {
            cooldowns[unSpellId] = unTimeMs;
            AddSpellCooldown(unSpellId, 0, unTimeMs, true);
        }
    }

    if (!cooldowns.empty())
    {
        BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, cooldowns);
        SendDirectMessage(&data);
    }
}

void Player::AddSpellAndCategoryCooldowns(SpellInfo const* spellInfo, uint32 itemId, Spell* spell, bool infinityCooldown)
{
    // init cooldown values
    uint32 cat   = 0;
    int32 rec    = -1;
    int32 catrec = -1;

    // some special item spells without correct cooldown in SpellInfo
    // cooldown information stored in item prototype
    // This used in same way in WorldSession::HandleItemQuerySingleOpcode data sending to client.

    if (itemId)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
        {
            for (uint8 idx = 0; idx < MAX_ITEM_SPELLS; ++idx)
            {
                if (uint32(proto->Spells[idx].SpellId) == spellInfo->Id)
                {
                    cat    = proto->Spells[idx].SpellCategory;
                    rec    = proto->Spells[idx].SpellCooldown;
                    catrec = proto->Spells[idx].SpellCategoryCooldown;
                    break;
                }
            }
        }
    }

    // if no cooldown found above then base at DBC data
    if (rec < 0 && catrec < 0)
    {
        cat = spellInfo->GetCategory();
        rec = spellInfo->RecoveryTime;
        catrec = spellInfo->CategoryRecoveryTime;
    }

    time_t catrecTime;
    time_t recTime;

    bool needsCooldownPacket = false;

    // overwrite time for selected category
    if (infinityCooldown)
    {
        // use +MONTH as infinity mark for spell cooldown (will checked as MONTH/2 at save ans skipped)
        // but not allow ignore until reset or re-login
        catrecTime = catrec > 0 ? infinityCooldownDelay : 0;
        recTime    = rec    > 0 ? infinityCooldownDelay : catrecTime;
    }
    else
    {
        // shoot spells used equipped item cooldown values already assigned in GetAttackTime(RANGED_ATTACK)
        // prevent 0 cooldowns set by another way
        if (rec <= 0 && catrec <= 0 && (cat == 76 || (spellInfo->IsAutoRepeatRangedSpell() && spellInfo->Id != 75)))
            rec = GetAttackTime(RANGED_ATTACK);

        // Now we have cooldown data (if found any), time to apply mods
        if (rec > 0)
            ApplySpellMod(spellInfo->Id, SPELLMOD_COOLDOWN, rec, spell);

        if (catrec > 0 && !spellInfo->HasAttribute(SPELL_ATTR6_NO_CATEGORY_COOLDOWN_MODS))
        {
            ApplySpellMod(spellInfo->Id, SPELLMOD_COOLDOWN, catrec, spell);
        }

        if (int32 cooldownMod = GetTotalAuraModifier(SPELL_AURA_MOD_COOLDOWN))
        {
            // Apply SPELL_AURA_MOD_COOLDOWN only to own spells
            if (HasSpell(spellInfo->Id))
            {
                needsCooldownPacket = true;
                rec += cooldownMod * IN_MILLISECONDS;   // SPELL_AURA_MOD_COOLDOWN does not affect category cooldows, verified with shaman shocks
            }
        }

        // replace negative cooldowns by 0
        if (rec < 0) rec = 0;
        if (catrec < 0) catrec = 0;

        // no cooldown after applying spell mods
        if (rec == 0 && catrec == 0)
            return;

        catrecTime = catrec ? catrec : 0;
        recTime    = rec ? rec : catrecTime;
    }

    // category spells
    if (cat && catrec > 0)
    {
        _AddSpellCooldown(spellInfo->Id, 0, itemId, recTime, true, true);
        if (needsCooldownPacket)
        {
            WorldPacket data;
            BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, spellInfo->Id, recTime);
            SendDirectMessage(&data);
        }

        PacketCooldowns forcedCategoryCooldowns;

        SpellCategoryStore::const_iterator i_scstore = sSpellsByCategoryStore.find(cat);
        if (i_scstore != sSpellsByCategoryStore.end())
        {
            for (SpellCategorySet::const_iterator i_scset = i_scstore->second.begin(); i_scset != i_scstore->second.end(); ++i_scset)
            {
                if (i_scset->second == spellInfo->Id) // skip main spell, already handled above
                {
                    continue;
                }

                // If spell category is applied by item, then other spells should be exists in item templates
                if ((itemId > 0) != i_scset->first)
                {
                    continue;
                }

                // Only within the same spellfamily
                SpellInfo const* categorySpellInfo = sSpellMgr->GetSpellInfo(i_scset->second);
                if (!categorySpellInfo || categorySpellInfo->SpellFamilyName != spellInfo->SpellFamilyName)
                {
                    continue;
                }

                _AddSpellCooldown(i_scset->second, cat, itemId, catrecTime, !spellInfo->IsCooldownStartedOnEvent() && catrec && rec && catrec != rec);

                if (spellInfo->HasAttribute(SPELL_ATTR0_CU_FORCE_SEND_CATEGORY_COOLDOWNS))
                {
                    forcedCategoryCooldowns[i_scset->second] = catrecTime;
                }
            }
        }

        if (!forcedCategoryCooldowns.empty())
        {
            WorldPacket data;
            BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, forcedCategoryCooldowns);
            SendDirectMessage(&data);
        }
    }
    else
    {
        // self spell cooldown
        if (recTime > 0)
        {
            _AddSpellCooldown(spellInfo->Id, 0, itemId, recTime, true, true);

            if (needsCooldownPacket)
            {
                WorldPacket data;
                BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, spellInfo->Id, rec);
                SendDirectMessage(&data);
            }
        }
    }
}

void Player::_AddSpellCooldown(uint32 spellid, uint16 categoryId, uint32 itemid, uint32 end_time, bool needSendToClient, bool forceSendToSpectator)
{
    SpellCooldown sc;
    sc.end = GameTime::GetGameTimeMS().count() + end_time;
    sc.category = categoryId;
    sc.itemid = itemid;
    sc.maxduration = end_time;
    sc.sendToSpectator = false;
    sc.needSendToClient = needSendToClient;

    if (end_time >= SPECTATOR_COOLDOWN_MIN * IN_MILLISECONDS && end_time <= SPECTATOR_COOLDOWN_MAX * IN_MILLISECONDS)
    {
        if (NeedSendSpectatorData() && forceSendToSpectator && (itemid || HasActiveSpell(spellid)))
        {
            sc.sendToSpectator = true;
            ArenaSpectator::SendCommand_Cooldown(FindMap(), GetGUID(), "ACD", spellid, end_time / IN_MILLISECONDS, end_time / IN_MILLISECONDS);
        }
    }

    m_spellCooldowns[spellid] = std::move(sc);
}

void Player::ModifySpellCooldown(uint32 spellId, int32 cooldown)
{
    SpellCooldowns::iterator itr = m_spellCooldowns.find(spellId);
    if (itr == m_spellCooldowns.end())
        return;

    itr->second.end += cooldown;

    WorldPacket data(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
    data << uint32(spellId);            // Spell ID
    data << GetGUID();                  // Player GUID
    data << int32(cooldown);            // Cooldown mod in milliseconds
    SendDirectMessage(&data);
}

void Player::SendCooldownEvent(SpellInfo const* spellInfo, uint32 itemId /*= 0*/, Spell* spell /*= nullptr*/, bool setCooldown /*= true*/)
{
    // start cooldowns at server side, if any
    if (setCooldown)
        AddSpellAndCategoryCooldowns(spellInfo, itemId, spell);

    // Send activate cooldown timer (possible 0) at client side
    WorldPacket data(SMSG_COOLDOWN_EVENT, 4 + 8);
    data << uint32(spellInfo->Id);
    data << GetGUID();
    SendDirectMessage(&data);
}

void Player::SetEntryPoint()
{
    m_entryPointData.joinPos.m_mapId = MAPID_INVALID;
    m_entryPointData.ClearTaxiPath();

    if (!m_taxi.empty())
    {
        m_entryPointData.mountSpell  = 0;
        m_entryPointData.joinPos = WorldLocation(GetMapId(), GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());

        m_entryPointData.taxiPath[0] = m_taxi.GetTaxiSource();
        m_entryPointData.taxiPath[1] = m_taxi.GetTaxiDestination();
    }
    else
    {
        if (IsMounted())
        {
            AuraEffectList const& auras = GetAuraEffectsByType(SPELL_AURA_MOUNTED);
            if (!auras.empty())
                m_entryPointData.mountSpell = (*auras.begin())->GetId();
        }
        else
            m_entryPointData.mountSpell = 0;

        if (GetMap()->IsDungeon())
        {
            if (GraveyardStruct const* entry = sGraveyard->GetClosestGraveyard(this, GetTeamId()))
                m_entryPointData.joinPos = WorldLocation(entry->Map, entry->x, entry->y, entry->z, 0.0f);
        }
        else if (!GetMap()->IsBattlegroundOrArena())
            m_entryPointData.joinPos = WorldLocation(GetMapId(), GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
    }

    if (m_entryPointData.joinPos.m_mapId == MAPID_INVALID)
        m_entryPointData.joinPos = WorldLocation(m_homebindMapId, m_homebindX, m_homebindY, m_homebindZ, 0.0f);
}

void Player::ApplyEquipCooldown(Item* pItem)
{
    if (pItem->GetTemplate()->HasFlag(ITEM_FLAG_NO_EQUIP_COOLDOWN))
        return;

    if (GetCommandStatus(CHEAT_COOLDOWN))
        return;

    TimePoint const cooldownStart = std::chrono::steady_clock::now();
    auto applyProcCooldown = [this, pItem, cooldownStart](uint32 spellId)
    {
        SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(spellId);
        if (!procEntry)
            return;

        if (Aura* itemAura = GetAura(spellId, GetGUID(), pItem->GetGUID()))
            itemAura->AddProcCooldown(cooldownStart + procEntry->Cooldown);
    };

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = pItem->GetTemplate()->Spells[i];

        // no spell
        if (!spellData.SpellId)
            continue;

        // apply proc cooldown to equip auras if we have any
        if (spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
        {
            applyProcCooldown(spellData.SpellId);
            continue;
        }

        // wrong triggering type (note: ITEM_SPELLTRIGGER_ON_NO_DELAY_USE not have cooldown)
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        // xinef: dont apply equip cooldown if spell on item has insignificant cooldown
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId);
        if (spellData.SpellCooldown <= 3000 && spellData.SpellCategoryCooldown <= 3000 && (!spellInfo || (spellInfo->RecoveryTime <= 3000 && spellInfo->CategoryRecoveryTime <= 3000)))
            continue;

        // Don't replace longer cooldowns by equip cooldown if we have any.
        SpellCooldowns::iterator itr = m_spellCooldowns.find(spellData.SpellId);
        if (itr != m_spellCooldowns.end() && itr->second.itemid == pItem->GetEntry() && itr->second.end > GameTime::GetGameTimeMS().count() + 30 * IN_MILLISECONDS)
            continue;

        // xinef: dont apply eqiup cooldown for spells with this attribute
        if (spellInfo && spellInfo->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL))
            continue;

        AddSpellCooldown(spellData.SpellId, pItem->GetEntry(), 30 * IN_MILLISECONDS, true, true);

        WorldPacket data(SMSG_ITEM_COOLDOWN, 12);
        data << pItem->GetGUID();
        data << uint32(spellData.SpellId);
        SendDirectMessage(&data);
    }

    // Enchantment equip spells are not included in the item template spell list.
    for (uint8 enchantmentSlot = 0; enchantmentSlot < MAX_ENCHANTMENT_SLOT; ++enchantmentSlot)
    {
        uint32 enchantmentId = pItem->GetEnchantmentId(EnchantmentSlot(enchantmentSlot));
        if (!enchantmentId)
            continue;

        SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantmentId);
        if (!enchantment)
            continue;

        for (uint8 effect = 0; effect < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effect)
            if (enchantment->type[effect] == ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL && enchantment->spellid[effect])
                applyProcCooldown(enchantment->spellid[effect]);
    }
}

void Player::resetSpells()
{
    // not need after this call
    if (HasAtLoginFlag(AT_LOGIN_RESET_SPELLS))
        RemoveAtLoginFlag(AT_LOGIN_RESET_SPELLS, true);

    // make full copy of map (spells removed and marked as deleted at another spell remove
    // and we can't use original map for safe iterative with visit each spell at loop end
    PlayerSpellMap spellMap = GetSpellMap();

    for (PlayerSpellMap::const_iterator iter = spellMap.begin(); iter != spellMap.end(); ++iter)
        removeSpell(iter->first, SPEC_MASK_ALL, false);

    LearnDefaultSkills();
    LearnCustomSpells();
    learnQuestRewardedSpells();
}

void Player::LearnCustomSpells()
{
    if (!sWorld->getBoolConfig(CONFIG_START_CUSTOM_SPELLS))
    {
        return;
    }

    // learn default race/class spells
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(), getClass());
    ASSERT(info);
    for (PlayerCreateInfoSpells::const_iterator itr = info->customSpells.begin(); itr != info->customSpells.end(); ++itr)
    {
        uint32 tspell = *itr;
        LOG_DEBUG("entities.player.loading", "Player::LearnCustomSpells: Player '{}' ({}, Class: {} Race: {}): Adding initial spell (SpellID: {})",
            GetName(), GetGUID().ToString(), uint32(getClass()), uint32(getRace()), tspell);
        if (!IsInWorld())                                   // will send in INITIAL_SPELLS in list anyway at map add
        {
            addSpell(tspell, SPEC_MASK_ALL, true);
        }
        else                                               // but send in normal spell in game learn case
        {
            learnSpell(tspell);
        }
    }
}

void Player::learnQuestRewardedSpells(Quest const* quest)
{
    // xinef: quest does not learn anything
    int32 spellId = quest->GetRewSpellCast();
    if (!spellId)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return;

    // xinef: find effect with learn spell and check if we have this spell
    bool found = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && spellInfo->Effects[i].TriggerSpell && !HasSpell(spellInfo->Effects[i].TriggerSpell))
        {
            // pusywizard: don't re-add profession specialties!
            if (SpellInfo const* triggeredInfo = sSpellMgr->GetSpellInfo(spellInfo->Effects[i].TriggerSpell))
                if (triggeredInfo->Effects[0].Effect == SPELL_EFFECT_TRADE_SKILL)
                    break; // pussywizard: break and not cast the spell (found is false)

            found = true;
            break;
        }

    // xinef: we know the spell, return
    if (!found)
        return;

    if (!SatisfyQuestSkill(quest, false))
        return;

    CastSpell(this, spellId, true);
}

void Player::learnQuestRewardedSpells()
{
    // learn spells received from quest completing
    for (RewardedQuestSet::const_iterator itr = m_RewardedQuests.begin(); itr != m_RewardedQuests.end(); ++itr)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(*itr);
        if (!quest)
            continue;

        learnQuestRewardedSpells(quest);
    }
}

void Player::learnSkillRewardedSpells(uint32 skill_id, uint32 skill_value)
{
    uint32 raceMask  = getRaceMask();
    uint32 classMask = getClassMask();

    // Get all abilities for this skill and sort by MinSkillLineRank (lowest to highest)
    auto abilities = GetSkillLineAbilitiesBySkillLine(skill_id);
    std::vector<SkillLineAbilityEntry const*> sortedAbilities(abilities.begin(), abilities.end());
    std::sort(sortedAbilities.begin(), sortedAbilities.end(),
        [](SkillLineAbilityEntry const* a, SkillLineAbilityEntry const* b)
        {
            return a->MinSkillLineRank < b->MinSkillLineRank;
        });

    for (SkillLineAbilityEntry const* pAbility : sortedAbilities)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(pAbility->Spell);
        if (!spellInfo)
        {
            continue;
        }

        if (pAbility->AcquireMethod != SKILL_LINE_ABILITY_LEARNED_ON_SKILL_VALUE && pAbility->AcquireMethod != SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN)
        {
            continue;
        }

        // Check race if set
        if (pAbility->RaceMask && !(pAbility->RaceMask & raceMask))
        {
            continue;
        }

        // Check class if set
        if (pAbility->ClassMask && !(pAbility->ClassMask & classMask))
        {
            continue;
        }

        // need unlearn spell
        if (skill_value < pAbility->MinSkillLineRank && pAbility->AcquireMethod == SKILL_LINE_ABILITY_LEARNED_ON_SKILL_VALUE)
        {
            removeSpell(pAbility->Spell, GetActiveSpec(), true);
        }
        // need learn
        else
        {
            //used to avoid double Seal of Righteousness on paladins, it's the only player spell which has both spell and forward spell in auto learn
            if (pAbility->AcquireMethod == SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN && pAbility->SupercededBySpell)
            {
                bool skipCurrent = false;
                auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(pAbility->SupercededBySpell);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                {
                    if (itr->second->AcquireMethod == SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN && skill_value >= itr->second->MinSkillLineRank)
                    {
                        skipCurrent = true;
                        break;
                    }
                }
                if (skipCurrent)
                {
                    continue;
                }
            }

            if (!IsInWorld())
            {
                addSpell(pAbility->Spell, SPEC_MASK_ALL, true, true);
            }
            else
            {
                learnSpell(pAbility->Spell, true, true);
            }
        }
    }
}

void Player::GetAurasForTarget(Unit* target, bool force /*= false*/)
{
    if (!target || (!force && target->GetVisibleAuras()->empty()))    // speedup things
        return;

    WorldPacket data(SMSG_AURA_UPDATE_ALL);
    data<< target->GetPackGUID();

    Unit::VisibleAuraMap const* visibleAuras = target->GetVisibleAuras();
    for (Unit::VisibleAuraMap::const_iterator itr = visibleAuras->begin(); itr != visibleAuras->end(); ++itr)
    {
        AuraApplication* auraApp = itr->second;
        auraApp->BuildUpdatePacket(data, false);
    }

    SendDirectMessage(&data);
}

bool Player::IsSpellFitByClassAndRace(uint32 spell_id) const
{
    uint32 racemask  = getRaceMask();
    uint32 classmask = getClassMask();

    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell_id);
    if (bounds.first == bounds.second)
        return true;

    for (SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
    {
        // skip wrong race skills
        if (_spell_idx->second->RaceMask && (_spell_idx->second->RaceMask & racemask) == 0)
            continue;

        // skip wrong class skills
        if (_spell_idx->second->ClassMask && (_spell_idx->second->ClassMask & classmask) == 0)
            continue;

        // skip wrong class and race skill saved in SkillRaceClassInfo.dbc
        if (!GetSkillRaceClassInfo(_spell_idx->second->SkillLine, getRace(), getClass()))
            continue;

        return true;
    }

    return false;
}

bool Player::HasItemFitToSpellRequirements(SpellInfo const* spellInfo, Item const* ignoreItem) const
{
    if (spellInfo->EquippedItemClass < 0)
        return true;

    // scan other equipped items for same requirements (mostly 2 daggers/etc)
    // for optimize check 2 used cases only
    switch (spellInfo->EquippedItemClass)
    {
        case ITEM_CLASS_WEAPON:
            {
                for (uint8 i = EQUIPMENT_SLOT_MAINHAND; i < EQUIPMENT_SLOT_TABARD; ++i)
                    if (Item* item = GetUseableItemByPos(INVENTORY_SLOT_BAG_0, i))
                        if (item != ignoreItem && item->IsFitToSpellRequirements(spellInfo))
                            return true;

                // Keep active non-passive auras (e.g. Bladestorm) when disarmed
                if (!spellInfo->IsPassive())
                {
                    bool hasWeaponInSlot = false;
                    for (uint8 i = EQUIPMENT_SLOT_MAINHAND; i < EQUIPMENT_SLOT_TABARD; ++i)
                        if (Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                            if (item != ignoreItem && item->IsFitToSpellRequirements(spellInfo))
                            {
                                hasWeaponInSlot = true;
                                break;
                            }

                    if (!hasWeaponInSlot)
                        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                            if (spellInfo->Effects[i].IsAura())
                                return true;
                }

                break;
            }
        case ITEM_CLASS_ARMOR:
            {
                // most used check: shield only
                if (spellInfo->EquippedItemSubClassMask & (1 << ITEM_SUBCLASS_ARMOR_SHIELD))
                {
                    if (Item* item = GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                        if (item != ignoreItem && item->IsFitToSpellRequirements(spellInfo))
                            return true;

                    // Keep active non-passive auras (e.g. Shield Wall) when disarmed
                    if (!spellInfo->IsPassive() && HasAuraType(SPELL_AURA_MOD_DISARM_OFFHAND) && GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                    {
                        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                            if (spellInfo->Effects[i].IsAura())
                                return true;
                    }
                }

                // tabard not have dependent spells
                for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_MAINHAND; ++i)
                    if (Item* item = GetUseableItemByPos(INVENTORY_SLOT_BAG_0, i))
                        if (item != ignoreItem && item->IsFitToSpellRequirements(spellInfo))
                            return true;

                // ranged slot can have some armor subclasses
                if (Item* item = GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
                    if (item != ignoreItem && item->IsFitToSpellRequirements(spellInfo))
                        return true;

                break;
            }
        default:
            LOG_ERROR("entities.player", "HasItemFitToSpellRequirements: Not handled spell requirement for item class {}", spellInfo->EquippedItemClass);
            break;
    }

    return false;
}

bool Player::CanNoReagentCast(SpellInfo const* spellInfo) const
{
    // don't take reagents for spells with SPELL_ATTR5_NO_REAGENT_COST_WITH_AURA
    if (spellInfo->HasAttribute(SPELL_ATTR5_NO_REAGENT_COST_WITH_AURA) && HasUnitFlag(UNIT_FLAG_PREPARATION))
        return true;

    // Check no reagent use mask
    flag96 noReagentMask;
    noReagentMask[0] = GetUInt32Value(PLAYER_NO_REAGENT_COST_1);
    noReagentMask[1] = GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + 1);
    noReagentMask[2] = GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + 2);
    if (spellInfo->SpellFamilyFlags  & noReagentMask)
        return true;

    return false;
}

void Player::RemoveItemDependentAurasAndCasts(Item* pItem)
{
    for (AuraMap::iterator itr = m_ownedAuras.begin(); itr != m_ownedAuras.end();)
    {
        Aura* aura = itr->second;

        // skip not self applied auras
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (aura->GetCasterGUID() != GetGUID())
        {
            ++itr;
            continue;
        }

        // skip if not item dependent or have alternative item
        if (HasItemFitToSpellRequirements(spellInfo, pItem))
        {
            ++itr;
            continue;
        }

        // no alt item, remove aura, restart check
        RemoveOwnedAura(itr);
    }

    // currently casted spells can be dependent from item
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
        if (Spell* spell = GetCurrentSpell(CurrentSpellTypes(i)))
            if (spell->getState() != SPELL_STATE_DELAYED && !HasItemFitToSpellRequirements(spell->m_spellInfo, pItem))
                InterruptSpell(CurrentSpellTypes(i));
}

uint32 Player::GetResurrectionSpellId()
{
    // search priceless resurrection possibilities
    uint32 prio = 0;
    uint32 spell_id = 0;
    AuraEffectList const& dummyAuras = GetAuraEffectsByType(SPELL_AURA_DUMMY);
    for (AuraEffectList::const_iterator itr = dummyAuras.begin(); itr != dummyAuras.end(); ++itr)
    {
        // Soulstone Resurrection                           // prio: 3 (max, non death persistent)
        if (prio < 2 && (*itr)->GetSpellInfo()->SpellVisual[0] == 99 && (*itr)->GetSpellInfo()->SpellIconID == 92)
        {
            switch ((*itr)->GetId())
            {
                case 20707:
                    spell_id =  3026;
                    break;        // rank 1
                case 20762:
                    spell_id = 20758;
                    break;        // rank 2
                case 20763:
                    spell_id = 20759;
                    break;        // rank 3
                case 20764:
                    spell_id = 20760;
                    break;        // rank 4
                case 20765:
                    spell_id = 20761;
                    break;        // rank 5
                case 27239:
                    spell_id = 27240;
                    break;        // rank 6
                case 47883:
                    spell_id = 47882;
                    break;        // rank 7
                default:
                    LOG_ERROR("entities.player", "Unhandled spell {}: S.Resurrection", (*itr)->GetId());
                    continue;
            }

            prio = 3;
        }
        // Twisting Nether                                  // prio: 2 (max)
        else if ((*itr)->GetId() == 23701 && roll_chance_i(10))
        {
            prio = 2;
            spell_id = 23700;
        }
    }

    // Reincarnation (passive spell)  // prio: 1                  // Glyph of Renewed Life
    if (prio < 1 && HasSpell(20608) && !HasSpellCooldown(21169) && (HasAura(58059) || HasItemCount(17030)))
        spell_id = 21169;

    return spell_id;
}

void Player::StopCastingBindSight(Aura* except /*= nullptr*/)
{
    if (WorldObject* target = GetViewpoint())
    {
        if (target->IsUnit())
        {
            ((Unit*)target)->RemoveAurasByType(SPELL_AURA_BIND_SIGHT, GetGUID(), except);
            ((Unit*)target)->RemoveAurasByType(SPELL_AURA_MOD_POSSESS, GetGUID(), except);
            ((Unit*)target)->RemoveAurasByType(SPELL_AURA_MOD_POSSESS_PET, GetGUID(), except);
        }
    }
}

bool Player::isTotalImmune() const
{
    AuraEffectList const& immune = GetAuraEffectsByType(SPELL_AURA_SCHOOL_IMMUNITY);

    uint32 immuneMask = 0;
    for (AuraEffectList::const_iterator itr = immune.begin(); itr != immune.end(); ++itr)
    {
        immuneMask |= (*itr)->GetMiscValue();
        if (immuneMask & SPELL_SCHOOL_MASK_ALL)            // total immunity
            return true;
    }
    return false;
}

uint32 Player::GetRuneBaseCooldown(uint8 index, bool skipGrace)
{
    uint8 rune = GetBaseRune(index);
    uint32 cooldown = RUNE_BASE_COOLDOWN;
    if (!skipGrace)
        cooldown -= GetGracePeriod(index) < 250 ? 0 : GetGracePeriod(index) - 250;  // xinef: reduce by grace period, treat first 250ms as instant use of rune

    AuraEffectList const& regenAura = GetAuraEffectsByType(SPELL_AURA_MOD_POWER_REGEN_PERCENT);
    for (AuraEffectList::const_iterator i = regenAura.begin(); i != regenAura.end(); ++i)
    {
        if ((*i)->GetMiscValue() == POWER_RUNE && (*i)->GetMiscValueB() == rune)
            cooldown = cooldown * (100 - (*i)->GetAmount()) / 100;
    }

    return cooldown;
}

void Player::RemoveRunesByAuraEffect(AuraEffect const* aura)
{
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (m_runes->runes[i].ConvertAura == aura)
        {
            ConvertRune(i, GetBaseRune(i));
            SetRuneConvertAura(i, nullptr);
        }
    }
}

void Player::RestoreBaseRune(uint8 index)
{
    AuraEffect const* aura = m_runes->runes[index].ConvertAura;
    // If rune was converted by a non-pasive aura that still active we should keep it converted
    if (aura && !aura->GetSpellInfo()->HasAttribute(SPELL_ATTR0_PASSIVE))
        return;
    ConvertRune(index, GetBaseRune(index));
    SetRuneConvertAura(index, nullptr);
    // Don't drop passive talents providing rune convertion
    if (!aura || aura->GetAuraType() != SPELL_AURA_CONVERT_RUNE)
        return;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (aura == m_runes->runes[i].ConvertAura)
            return;
    }
    aura->GetBase()->Remove();
}

void Player::ConvertRune(uint8 index, RuneType newType)
{
    SetCurrentRune(index, newType);

    WorldPacket data(SMSG_CONVERT_RUNE, 2);
    data << uint8(index);
    data << uint8(newType);
    SendDirectMessage(&data);
}

void Player::ResyncRunes(uint8 count)
{
    WorldPacket data(SMSG_RESYNC_RUNES, 4 + count * 2);
    data << uint32(count);
    for (uint32 i = 0; i < count; ++i)
    {
        data << uint8(GetCurrentRune(i));                   // rune type
        data << uint8(255 - (GetRuneCooldown(i) * 51));     // passed cooldown time (0-255)
    }
    SendDirectMessage(&data);
}

void Player::AddRunePower(uint8 index)
{
    WorldPacket data(SMSG_ADD_RUNE_POWER, 4);
    data << uint32(1 << index);                             // mask (0x00-0x3F probably)
    SendDirectMessage(&data);
}

static RuneType runeSlotTypes[MAX_RUNES] =
{
    /*0*/ RUNE_BLOOD,
    /*1*/ RUNE_BLOOD,
    /*2*/ RUNE_UNHOLY,
    /*3*/ RUNE_UNHOLY,
    /*4*/ RUNE_FROST,
    /*5*/ RUNE_FROST
};

void Player::InitRunes()
{
    if (!IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_ABILITY))
        return;

    m_runes = new Runes;

    m_runes->runeState = 0;
    m_runes->lastUsedRune = RUNE_BLOOD;

    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        SetBaseRune(i, runeSlotTypes[i]);                              // init base types
        SetCurrentRune(i, runeSlotTypes[i]);                           // init current types
        SetRuneCooldown(i, 0);                                         // reset cooldowns
        SetGracePeriod(i, 0);                                          // xinef: reset grace period
        SetRuneConvertAura(i, nullptr);
        m_runes->SetRuneState(i);
    }

    for (uint8 i = 0; i < NUM_RUNE_TYPES; ++i)
        SetFloatValue(PLAYER_RUNE_REGEN_1 + i, 0.1f);
}

bool Player::IsBaseRuneSlotsOnCooldown(RuneType runeType) const
{
    for (uint8 i = 0; i < MAX_RUNES; ++i)
        if (GetBaseRune(i) == runeType && GetRuneCooldown(i) == 0)
            return false;

    return true;
}

void Player::learnSpellHighRank(uint32 spellid)
{
    learnSpell(spellid);

    if (uint32 next = sSpellMgr->GetNextSpellInChain(spellid))
        learnSpellHighRank(next);
}

bool Player::CanSeeSpellClickOn(Creature const* c) const
{
    if (!c->HasNpcFlag(UNIT_NPC_FLAG_SPELLCLICK))
        return false;

    SpellClickInfoMapBounds clickPair = sObjectMgr->GetSpellClickInfoMapBounds(c->GetEntry());
    if (clickPair.first == clickPair.second)
        return false;

    for (SpellClickInfoContainer::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (!itr->second.IsFitToRequirements(this, c))
            return false;

        ConditionList conds = sConditionMgr->GetConditionsForSpellClickEvent(c->GetEntry(), itr->second.spellId);
        ConditionSourceInfo info = ConditionSourceInfo(const_cast<Player*>(this), const_cast<Creature*>(c));
        if (sConditionMgr->IsObjectMeetToConditions(info, conds))
            return true;
    }

    return false;
}

void Player::SendClearCooldown(uint32 spell_id, Unit* target)
{
    WorldPacket data(SMSG_CLEAR_COOLDOWN, 4 + 8);
    data << uint32(spell_id);
    data << target->GetGUID();
    SendDirectMessage(&data);

    if (target == this && NeedSendSpectatorData())
        ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "RCD", spell_id);
}

void Player::PrepareCharmAISpells()
{
    for (int i = 0; i < NUM_CAI_SPELLS; ++i)
        m_charmAISpells[i] = 0;

    uint32 damage_type[4] = {0, 0, 0, 0};
    uint32 periodic_damage = 0;

    for (PlayerSpellMap::iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active || !itr->second->IsInSpec(GetActiveSpec()))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
        if (!spellInfo)
            continue;

        if (!spellInfo->SpellFamilyName || spellInfo->IsPassive() || spellInfo->NeedsComboPoints() || (spellInfo->Stances && !spellInfo->HasAttribute(SPELL_ATTR2_ALLOW_WHILE_NOT_SHAPESHIFTED)))
            continue;

        float cast = spellInfo->CalcCastTime() / 1000.0f;
        if (cast > 3.0f)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                int32 dmg = CalculateSpellDamage(this, spellInfo, i);
                uint8 offset = 0;
                if (cast)
                {
                    dmg = dmg / cast;
                    offset = 2;
                }

                if ((int32)damage_type[offset] < dmg)
                {
                    if (!m_charmAISpells[SPELL_INSTANT_DAMAGE + offset] || !spellInfo->IsHighRankOf(sSpellMgr->GetSpellInfo(m_charmAISpells[SPELL_INSTANT_DAMAGE + offset])) || urand(0, 1))
                        if (damage_type[1 + offset] < damage_type[offset])
                        {
                            m_charmAISpells[SPELL_INSTANT_DAMAGE2 + offset] = m_charmAISpells[SPELL_INSTANT_DAMAGE + offset];
                            damage_type[1 + offset] = damage_type[offset];
                        }

                    m_charmAISpells[SPELL_INSTANT_DAMAGE + offset] = spellInfo->Id;
                    damage_type[offset] = dmg;
                }
                else if ((int32)damage_type[1 + offset] < dmg)
                {
                    if (m_charmAISpells[SPELL_INSTANT_DAMAGE + offset] && sSpellMgr->GetSpellInfo(m_charmAISpells[SPELL_INSTANT_DAMAGE + offset])->IsHighRankOf(spellInfo) && urand(0, 1))
                        continue;

                    m_charmAISpells[SPELL_INSTANT_DAMAGE2 + offset] = spellInfo->Id;
                    damage_type[1 + offset] = dmg;
                }
                break;
            }
            else if (spellInfo->HasAttribute(SPELL_ATTR7_ATTACK_ON_CHARGE_TO_UNIT))
            {
                m_charmAISpells[SPELL_T_CHARGE] = spellInfo->Id;
                break;
            }
            else if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_INCREASE_SPEED)
            {
                m_charmAISpells[SPELL_FAST_RUN] = spellInfo->Id;
                break;
            }
            else if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_SCHOOL_IMMUNITY)
            {
                m_charmAISpells[SPELL_IMMUNITY] = spellInfo->Id;
                break;
            }
            else if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
            {
                if ((int32)periodic_damage < CalculateSpellDamage(this, spellInfo, i))
                {
                    m_charmAISpells[SPELL_DOT_DAMAGE] = spellInfo->Id;
                    break;
                }
            }
            else if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_STUN)
            {
                m_charmAISpells[SPELL_T_STUN] = spellInfo->Id;
                break;
            }
            else if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_ROOT || spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_FEAR)
            {
                m_charmAISpells[SPELL_ROOT_OR_FEAR] = spellInfo->Id;
                break;
            }
        }
    }

    // The selection above can leave a lower rank of a spell chain in a slot (e.g. the
    // secondary damage slot for a caster whose top spells are ranks of one chain). While
    // charmed the player should cast the highest rank it knows, so walk each slot up its
    // chain to the top learned rank.
    for (uint32& charmSpellId : m_charmAISpells)
    {
        if (!charmSpellId)
            continue;

        while (uint32 nextRank = sSpellMgr->GetNextSpellInChain(charmSpellId))
        {
            if (!HasActiveSpell(nextRank))
                break;

            charmSpellId = nextRank;
        }
    }
}

bool Player::HasCasterSpec()
{
    switch (GetSpec(GetActiveSpec()))
    {
        case TALENT_TREE_PRIEST_SHADOW:
        case TALENT_TREE_SHAMAN_ELEMENTAL:
        case TALENT_TREE_MAGE_ARCANE:
        case TALENT_TREE_MAGE_FIRE:
        case TALENT_TREE_MAGE_FROST:
        case TALENT_TREE_WARLOCK_AFFLICTION:
        case TALENT_TREE_WARLOCK_DEMONOLOGY:
        case TALENT_TREE_WARLOCK_DESTRUCTION:
        case TALENT_TREE_DRUID_BALANCE:
        case TALENT_TREE_HUNTER_BEAST_MASTERY:
        case TALENT_TREE_HUNTER_MARKSMANSHIP:
        case TALENT_TREE_HUNTER_SURVIVAL:
            return true;
        default:
            break;
    }
    return false;
}

bool Player::HasSpellCooldown(uint32 spell_id) const
{
    SpellCooldowns::const_iterator itr = m_spellCooldowns.find(spell_id);
    return itr != m_spellCooldowns.end() && itr->second.end > getMSTime();
}

bool Player::HasSpellItemCooldown(uint32 spell_id, uint32 itemid) const
{
    SpellCooldowns::const_iterator itr = m_spellCooldowns.find(spell_id);
    return itr != m_spellCooldowns.end() && itr->second.end > getMSTime() && itr->second.itemid == itemid;
}

uint32 Player::GetSpellCooldownDelay(uint32 spell_id) const
{
    SpellCooldowns::const_iterator itr = m_spellCooldowns.find(spell_id);
    return uint32(itr != m_spellCooldowns.end() && itr->second.end > getMSTime() ? itr->second.end - getMSTime() : 0);
}
