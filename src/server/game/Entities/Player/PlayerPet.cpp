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

void Player::SetGameMaster(bool on)
{
    if (on)
    {
        m_ExtraFlags |= PLAYER_EXTRA_GM_ON;
        if (GetSession()->IsGMAccount())
            SetFaction(FACTION_FRIENDLY);
        SetPlayerFlag(PLAYER_FLAGS_GM);
        SetUnitFlag2(UNIT_FLAG2_ALLOW_CHEAT_SPELLS);

        if (Pet* pet = GetPet())
        {
            if (GetSession()->IsGMAccount())
                pet->SetFaction(FACTION_FRIENDLY);
        }
        if (HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
        {
            RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
            sScriptMgr->OnPlayerFfaPvpStateUpdate(this, false);
        }
        ResetContestedPvP();

        CombatStopWithPets();

        SetPhaseMask(uint32(PHASEMASK_ANYWHERE), false);    // see and visible in all phases
        SetServerSideVisibilityDetect(SERVERSIDE_VISIBILITY_GM, GetSession()->GetSecurity());
    }
    else
    {
        // restore phase
        uint32 newPhase = GetPhaseByAuras();

        if (!newPhase)
            newPhase = PHASEMASK_NORMAL;

        SetPhaseMask(newPhase, false);

        m_ExtraFlags &= ~ PLAYER_EXTRA_GM_ON;
        SetFactionForRace(getRace(true));
        RemovePlayerFlag(PLAYER_FLAGS_GM);
        RemoveUnitFlag2(UNIT_FLAG2_ALLOW_CHEAT_SPELLS);

        if (Pet* pet = GetPet())
            pet->SetFaction(GetFaction());

        // restore FFA PvP Server state
        if (sWorld->IsFFAPvPRealm())
        {
            if (!HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
            {
                SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
                sScriptMgr->OnPlayerFfaPvpStateUpdate(this, true);
            }
        }
        // restore FFA PvP area state, remove not allowed for GM mounts
        UpdateArea(m_areaUpdateId);

        SetServerSideVisibilityDetect(SERVERSIDE_VISIBILITY_GM, SEC_PLAYER);
    }

    UpdateObjectVisibility();
}

Pet* Player::GetPet() const
{
    if (ObjectGuid pet_guid = GetPetGUID())
    {
        if (!pet_guid.IsPet())
            return nullptr;

        Pet* pet = ObjectAccessor::GetPet(*this, pet_guid);

        if (!pet)
            return nullptr;

        if (IsInWorld())
            return pet;

        //there may be a guardian in slot
        //LOG_ERROR("entities.player", "Player::GetPet: Pet {} not exist.", pet_guid.ToString());
        //const_cast<Player*>(this)->SetPetGUID(0);
    }

    return nullptr;
}

Pet* Player::SummonPet(uint32 entry, float x, float y, float z, float ang, PetType petType, Milliseconds duration /*= 0ms*/, uint32 healthPct /*= 0*/)
{
    PetStable& petStable = GetOrInitPetStable();

    Pet* pet = new Pet(this, petType);

    if (petType == SUMMON_PET && pet->LoadPetFromDB(this, entry, 0, false, healthPct))
    {
        // Remove Demonic Sacrifice auras (known pet)
        Unit::AuraEffectList const& auraClassScripts = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
        for (Unit::AuraEffectList::const_iterator itr = auraClassScripts.begin(); itr != auraClassScripts.end();)
        {
            if ((*itr)->GetMiscValue() == 2228)
            {
                RemoveAurasDueToSpell((*itr)->GetId());
                itr = auraClassScripts.begin();
            }
            else
                ++itr;
        }

        if (duration > 0ms)
            pet->SetDuration(duration);

        // Generate a new name for the newly summoned ghoul
        if (pet->IsPetGhoul())
        {
            std::string new_name = sObjectMgr->GeneratePetNameLocale(entry, GetSession()->GetSessionDbLocaleIndex());
            if (!new_name.empty())
                pet->SetName(new_name);
        }

        return nullptr;
    }

    // petentry == 0 for hunter "call pet" (current pet summoned if any)
    if (!entry)
    {
        delete pet;
        return nullptr;
    }

    pet->Relocate(x, y, z, ang);
    if (!pet->IsPositionValid())
    {
        LOG_ERROR("misc", "Player::SummonPet: Pet ({}, Entry: {}) not summoned. Suggested coordinates aren't valid (X: {} Y: {})", pet->GetGUID().ToString(), pet->GetEntry(), pet->GetPositionX(), pet->GetPositionY());
        delete pet;
        return nullptr;
    }

    Map* map = GetMap();
    uint32 pet_number = sObjectMgr->GeneratePetNumber();
    if (!pet->Create(map->GenerateLowGuid<HighGuid::Pet>(), map, GetPhaseMask(), entry, pet_number))
    {
        LOG_ERROR("misc", "Player::SummonPet: No such creature entry {}", entry);
        delete pet;
        return nullptr;
    }

    if (petType == SUMMON_PET && petStable.CurrentPet)
        RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT);

    pet->SetCreatorGUID(GetGUID());
    pet->SetFaction(GetFaction());
    pet->setPowerType(POWER_MANA);
    pet->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
    pet->SetUInt32Value(UNIT_FIELD_BYTES_1, 0);
    pet->InitStatsForLevel(GetLevel());

    SetMinion(pet, true);

    if (petType == SUMMON_PET)
    {
        if (pet->GetCreatureTemplate()->type == CREATURE_TYPE_DEMON || pet->GetCreatureTemplate()->type == CREATURE_TYPE_UNDEAD)
        {
            pet->GetCharmInfo()->SetPetNumber(pet_number, true); // Show pet details tab (Shift+P) only for demons & undead
        }
        else
        {
            pet->GetCharmInfo()->SetPetNumber(pet_number, false);
        }

        pet->SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
        pet->SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, 1000);
        pet->SetFullHealth();
        pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
        pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count())); // cast can't be helped in this case
    }

    map->AddToMap(pet->ToCreature(), true);

    ASSERT(!petStable.CurrentPet && (petType != HUNTER_PET || !petStable.GetUnslottedHunterPet()));
    pet->FillPetInfo(&petStable.CurrentPet.emplace());

    if (petType == SUMMON_PET)
    {
        pet->InitPetCreateSpells();
        pet->InitTalentForLevel();
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
        PetSpellInitialize();

        // Remove Demonic Sacrifice auras (known pet)
        Unit::AuraEffectList const& auraClassScripts = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
        for (Unit::AuraEffectList::const_iterator itr = auraClassScripts.begin(); itr != auraClassScripts.end();)
        {
            if ((*itr)->GetMiscValue() == 2228)
            {
                RemoveAurasDueToSpell((*itr)->GetId());
                itr = auraClassScripts.begin();
            }
            else
                ++itr;
        }
    }

    if (duration > 0ms)
        pet->SetDuration(duration);

    if (NeedSendSpectatorData() && pet->GetCreatureTemplate()->family)
    {
        ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "PHP", (uint32)pet->GetHealthPct());
        ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "PET", pet->GetCreatureTemplate()->family);
    }

    return pet;
}

void Player::RemovePet(Pet* pet, PetSaveMode mode, bool returnreagent)
{
    if (!pet)
        pet = GetPet();

    if (pet)
    {
        LOG_DEBUG("entities.pet", "RemovePet {}, {}, {}", pet->GetEntry(), mode, returnreagent);
        if (pet->m_removed)
            return;
    }

    if (returnreagent && (pet || (m_temporaryUnsummonedPetNumber && (!m_session || !m_session->PlayerLogout()))) && !InBattleground())
    {
        //returning of reagents only for players, so best done here
        uint32 spellId = pet ? pet->GetUInt32Value(UNIT_CREATED_BY_SPELL) : m_oldpetspell;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);

        if (spellInfo)
        {
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (spellInfo->Reagent[i] > 0)
                {
                    ItemPosCountVec dest;                   //for succubus, voidwalker, felhunter and felguard credit soulshard when despawn reason other than death (out of range, logout)
                    InventoryResult msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, spellInfo->Reagent[i], spellInfo->ReagentCount[i]);
                    if (msg == EQUIP_ERR_OK)
                    {
                        Item* item = StoreNewItem(dest, spellInfo->Reagent[i], true);
                        if (IsInWorld())
                            SendNewItem(item, spellInfo->ReagentCount[i], true, false);
                    }
                }
            }
        }
        m_temporaryUnsummonedPetNumber = 0;
    }

    if (!pet)
    {
        if (mode == PET_SAVE_NOT_IN_SLOT && m_petStable && m_petStable->CurrentPet)
        {
            // Handle removing pet while it is in "temporarily unsummoned" state, for example on mount
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_PET_SLOT_BY_ID);
            stmt->SetData(0, PET_SAVE_NOT_IN_SLOT);
            stmt->SetData(1, GetGUID().GetRawValue());
            stmt->SetData(2, m_petStable->CurrentPet->PetNumber);
            CharacterDatabase.Execute(stmt);

            m_petStable->UnslottedPets.push_back(std::move(*m_petStable->CurrentPet));
            m_petStable->CurrentPet.reset();
        }

        return;
    }
    else
    {
        pet->CombatStop();

        // only if current pet in slot
        pet->SavePetToDB(mode);

        if (m_petStable->CurrentPet && m_petStable->CurrentPet->PetNumber == pet->GetCharmInfo()->GetPetNumber())
        {
            if (mode == PET_SAVE_NOT_IN_SLOT)
            {
                m_petStable->UnslottedPets.push_back(std::move(*m_petStable->CurrentPet));
                m_petStable->CurrentPet.reset();
            }
            else if (mode == PET_SAVE_AS_DELETED)
                m_petStable->CurrentPet.reset();
            // else if (stable slots) handled in opcode handlers due to required swaps
            // else (current pet) doesnt need to do anything
        }

        SetMinion(pet, false);

        pet->AddObjectToRemoveList();
        pet->m_removed = true;

        if (pet->isControlled())
        {
            WorldPacket data(SMSG_PET_SPELLS, 8);
            data << uint64(0);
            SendDirectMessage(&data);

            if (GetGroup())
                SetGroupUpdateFlag(GROUP_UPDATE_PET);
        }

        if (NeedSendSpectatorData() && pet->GetCreatureTemplate()->family)
        {
            ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "PHP", 0);
            ArenaSpectator::SendCommand_UInt32Value(FindMap(), GetGUID(), "PET", 0);
        }
    }
}

bool Player::CanPetResurrect()
{
    PetStable* const petStable = GetPetStable();
    if (!petStable)
    {
        // No pets
        return false;
    }

    auto const& currectPet = petStable->CurrentPet;
    auto const& unslottedHunterPet = petStable->GetUnslottedHunterPet();

    if (!currectPet && !unslottedHunterPet)
    {
        // No pets
        return false;
    }

    // Check current pet
    if (currectPet && !currectPet->Health)
    {
        return true;
    }

    // Check dismiss/unslotted hunter pet
    if (unslottedHunterPet && !unslottedHunterPet->Health)
    {
        return true;
    }

    return false;
}

bool Player::IsExistPet()
{
    PetStable* const petStable = GetPetStable();
    return petStable && (petStable->CurrentPet || petStable->GetUnslottedHunterPet());
}

Pet* Player::CreatePet(Creature* creatureTarget, uint32 spellID /*= 0*/)
{
    if (IsExistPet())
    {
        return nullptr;
    }

    if (!creatureTarget || creatureTarget->IsPet() || creatureTarget->IsPlayer())
    {
        return nullptr;
    }

    CreatureTemplate const* creatrueTemplate = sObjectMgr->GetCreatureTemplate(creatureTarget->GetEntry());
    if (!creatrueTemplate->family)
    {
        // Creatures with family 0 crashes the server
        return nullptr;
    }

    // Everything looks OK, create new pet
    Pet* pet = CreateTamedPetFrom(creatureTarget, spellID);
    if (!pet)
    {
        return nullptr;
    }

    // "kill" original creature
    creatureTarget->DespawnOrUnsummon();

    // calculate proper level
    uint8 level = (creatureTarget->GetLevel() < (GetLevel() - 5)) ? (GetLevel() - 5) : GetLevel();

    // prepare visual effect for levelup
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, level - 1);

    // add to world
    pet->GetMap()->AddToMap(pet->ToCreature());

    // visual effect for levelup
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, level);

    // caster have pet now
    SetMinion(pet, true);

    pet->InitTalentForLevel();

    pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    PetSpellInitialize();

    return pet;
}

Pet* Player::CreatePet(uint32 creatureEntry, uint32 spellID /*= 0*/)
{
    if (IsExistPet())
    {
        return nullptr;
    }

    CreatureTemplate const* creatrueTemplate = sObjectMgr->GetCreatureTemplate(creatureEntry);
    if (!creatrueTemplate->family)
    {
        // Creatures with family 0 crashes the server
        return nullptr;
    }

    // Everything looks OK, create new pet
    Pet* pet = CreateTamedPetFrom(creatureEntry, spellID);
    if (!pet)
    {
        return nullptr;
    }

    // prepare visual effect for levelup
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, GetLevel() - 1);

    // add to world
    pet->GetMap()->AddToMap(pet->ToCreature());

    // visual effect for levelup
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, GetLevel());

    // caster have pet now
    SetMinion(pet, true);

    pet->InitTalentForLevel();

    pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    PetSpellInitialize();

    return pet;
}

void Player::RemovePetitionsAndSigns(ObjectGuid guid, uint32 type)
{
    SignatureContainer* signatureStore = sPetitionMgr->GetSignatureStore();
    for (SignatureContainer::iterator itr = signatureStore->begin(); itr != signatureStore->end(); ++itr)
    {
        SignatureMap::iterator signItr = itr->second.signatureMap.find(guid);
        if (signItr != itr->second.signatureMap.end())
        {
            Petition const* petition = sPetitionMgr->GetPetition(itr->first);
            if (!petition || (type != 10 && type != petition->petitionType))
                continue;

            // erase this
            itr->second.signatureMap.erase(signItr);

            // send update if charter owner in game
            Player* owner = ObjectAccessor::FindConnectedPlayer(petition->ownerGuid);
            if (owner)
                owner->GetSession()->SendPetitionQueryOpcode(petition->petitionGuid);
        }
    }

    if (type == 10)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ALL_PETITION_SIGNATURES);
        stmt->SetData(0, guid.GetCounter());
        CharacterDatabase.Execute(stmt);
    }
    else
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_SIGNATURE);
        stmt->SetData(0, guid.GetCounter());
        stmt->SetData(1, uint8(type));
        CharacterDatabase.Execute(stmt);
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (type == 10)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_BY_OWNER);
        stmt->SetData(0, guid.GetCounter());
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_SIGNATURE_BY_OWNER);
        stmt->SetData(0, guid.GetCounter());
        trans->Append(stmt);

        // xinef: clear petition store
        sPetitionMgr->RemovePetitionByOwnerAndType(guid, 0);
    }
    else
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_BY_OWNER_AND_TYPE);
        stmt->SetData(0, guid.GetCounter());
        stmt->SetData(1, uint8(type));
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_SIGNATURE_BY_OWNER_AND_TYPE);
        stmt->SetData(0, guid.GetCounter());
        stmt->SetData(1, uint8(type));
        trans->Append(stmt);

        // xinef: clear petition store
        sPetitionMgr->RemovePetitionByOwnerAndType(guid, uint8(type));
    }
    CharacterDatabase.CommitTransaction(trans);
}

void Player::SummonIfPossible(bool agree, ObjectGuid summoner_guid)
{
    if (!agree)
    {
        m_summon_expire = 0;
        return;
    }

    // expire and auto declined
    if (m_summon_expire < GameTime::GetGameTime().count())
        return;

    // drop flag at summon
    // this code can be reached only when GM is summoning player who carries flag, because player should be immune to summoning spells when he carries flag
    if (Battleground* bg = GetBattleground())
        bg->EventPlayerDroppedFlag(this);

    m_summon_expire = 0;

    UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ACCEPTED_SUMMONINGS, 1);

    TeleportTo(m_summon_mapid, m_summon_x, m_summon_y, m_summon_z, GetOrientation(), 0, ObjectAccessor::FindPlayer(summoner_guid));
}

void Player::UnsummonPetTemporaryIfAny()
{
    Pet* pet = GetPet();
    if (!pet)
        return;

    if (!m_temporaryUnsummonedPetNumber && pet->isControlled() && !pet->isTemporarySummoned())
    {
        m_temporaryUnsummonedPetNumber = pet->GetCharmInfo()->GetPetNumber();
        SetLastPetSpell(pet->GetUInt32Value(UNIT_CREATED_BY_SPELL));
    }

    RemovePet(pet, PET_SAVE_AS_CURRENT);
}

void Player::ResummonPetTemporaryUnSummonedIfAny()
{
    if (!m_temporaryUnsummonedPetNumber || IsSpectator())
        return;

    // not resummon in not appropriate state
    if (IsPetNeedBeTemporaryUnsummoned())
        return;

    if (GetPetGUID())
        return;

    if (!CanResummonPet(GetLastPetSpell()))
        return;

    Pet* newPet = new Pet(this);
    if (!newPet->LoadPetFromDB(this, 0, m_temporaryUnsummonedPetNumber, true))
        delete newPet;

    m_temporaryUnsummonedPetNumber = 0;
}

bool Player::CanResummonPet(uint32 spellid)
{
    if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_PET))
    {
        if (CanSeeDKPet())
            return true;
        else if (spellid == 52150) // Raise Dead
            return false;
    }

    if (IsClass(CLASS_MAGE, CLASS_CONTEXT_PET))
    {
        if (HasSpell(31687) && HasAura(70937))  //Has [Summon Water Elemental] spell and [Glyph of Eternal Water].
            return true;
    }

    if (IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
    {
        return true;
    }

    return HasSpell(spellid);
}

void Player::_LoadPetStable(uint8 petStableSlots, PreparedQueryResult result)
{
    if (!petStableSlots && !result)
        return;

    m_petStable = std::make_unique<PetStable>();
    m_petStable->MaxStabledPets = petStableSlots;

    if (m_petStable->MaxStabledPets > MAX_PET_STABLES)
    {
        LOG_ERROR("entities.player", "Player::LoadFromDB: Player ({}) can't have more stable slots than {}, but has {} in DB",
            GetGUID().ToString(), MAX_PET_STABLES, m_petStable->MaxStabledPets);

        m_petStable->MaxStabledPets = MAX_PET_STABLES;
    }

    //         0      1        2      3    4           5     6     7        8          9       10            11      12        13              14       15
    // SELECT id, entry, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, curhappiness, abdata, savetime, CreatedBySpell, PetType FROM character_pet WHERE owner = ?
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            PetStable::PetInfo petInfo;
            petInfo.PetNumber = fields[0].Get<uint32>();
            petInfo.CreatureId = fields[1].Get<uint32>();
            petInfo.DisplayId = fields[2].Get<uint32>();
            petInfo.Level = fields[3].Get<uint16>();
            petInfo.Experience = fields[4].Get<uint32>();
            petInfo.ReactState = ReactStates(fields[5].Get<uint8>());
            PetSaveMode slot = PetSaveMode(fields[6].Get<uint8>());
            petInfo.Name = fields[7].Get<std::string>();
            petInfo.WasRenamed = fields[8].Get<bool>();
            petInfo.Health = fields[9].Get<uint32>();
            petInfo.Mana = fields[10].Get<uint32>();
            petInfo.Happiness = fields[11].Get<uint32>();
            petInfo.ActionBar = fields[12].Get<std::string>();
            petInfo.LastSaveTime = fields[13].Get<uint32>();
            petInfo.CreatedBySpellId = fields[14].Get<uint32>();
            petInfo.Type = PetType(fields[15].Get<uint8>());

            if (slot == PET_SAVE_AS_CURRENT)
                m_petStable->CurrentPet = std::move(petInfo);
            else if (slot >= PET_SAVE_FIRST_STABLE_SLOT && slot <= PET_SAVE_LAST_STABLE_SLOT)
                m_petStable->StabledPets[slot - 1] = std::move(petInfo);
            else if (slot == PET_SAVE_NOT_IN_SLOT)
                m_petStable->UnslottedPets.push_back(std::move(petInfo));

        } while (result->NextRow());
    }
}

void Player::SetSummonPoint(uint32 mapid, float x, float y, float z, uint32 delay /*= 0*/, bool asSpectator /*= false*/)
{
    m_summon_expire = GameTime::GetGameTime().count() + (delay ? delay : MAX_PLAYER_SUMMON_DELAY);
    m_summon_mapid = mapid;
    m_summon_x = x;
    m_summon_y = y;
    m_summon_z = z;
    m_summon_asSpectator = asSpectator;
}
