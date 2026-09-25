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

bool Player::RewardHonor(Unit* uVictim, uint32 groupsize, int32 honor, bool awardXP)
{
    // do not reward honor in arenas, but enable onkill spellproc
    if (InArena())
    {
        if (!uVictim || uVictim == this || !uVictim->IsPlayer())
            return false;

        if (GetBgTeamId() == uVictim->ToPlayer()->GetBgTeamId())
            return false;

        return true;
    }

    // 'Inactive' this aura prevents the player from gaining honor points and battleground tokens
    if (HasAura(SPELL_AURA_PLAYER_INACTIVE))
        return false;

    /* check if player has same IP
    if (uVictim && uVictim->IsPlayer())
    {
        if (GetSession()->GetRemoteAddress() == uVictim->ToPlayer()->GetSession()->GetRemoteAddress())
            return false;
    }
    */

    ObjectGuid victim_guid;
    int32 victim_rank = 0;

    // need call before fields update to have chance move yesterday data to appropriate fields before today data change.
    UpdateHonorFields();

    // do not reward honor in arenas, but return true to enable onkill spellproc
    if (InArena())
        return true;

    // Promote to float for calculations
    float honor_f = (float)honor;

    if (honor_f <= 0)
    {
        if (!uVictim || uVictim == this || uVictim->HasNoPVPCreditAura())
            return false;

        victim_guid = uVictim->GetGUID();

        if (uVictim->IsPlayer())
        {
            Player* victim = uVictim->ToPlayer();

            if (GetTeamId() == victim->GetTeamId() && !sWorld->IsFFAPvPRealm())
                return false;

            uint8 k_level = GetLevel();
            uint8 k_grey = Acore::XP::GetGrayLevel(k_level);
            uint8 v_level = victim->GetLevel();

            if (v_level <= k_grey)
                return false;

            victim_rank = victim->GetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_LIFETIME_MAX_PVP_RANK);

            uint32 killer_title = GetUInt32Value(PLAYER_CHOSEN_TITLE);

            sScriptMgr->OnPlayerVictimRewardBefore(this, victim, killer_title, victim_rank);

            honor_f = std::ceil(Acore::Honor::hk_honor_at_level_f(k_level) * (v_level - k_grey) / (k_level - k_grey));

            // count the number of playerkills in one day
            ApplyModUInt32Value(PLAYER_FIELD_KILLS, 1, true);
            // and those in a lifetime
            ApplyModUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS, 1, true);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_EARN_HONORABLE_KILL);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HK_CLASS, victim->getClass());
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HK_RACE, victim->getRace(true));
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HONORABLE_KILL_AT_AREA, GetAreaId());
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HONORABLE_KILL, 1, 0, victim);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_SPECIAL_PVP_KILL, 1, 0, victim);
            sScriptMgr->OnPlayerVictimRewardAfter(this, victim, killer_title, victim_rank, honor_f);
        }
        else
        {
            if (!uVictim->ToCreature()->IsRacialLeader())
                return false;

            honor_f = 100.0f;                               // ??? need more info
            victim_rank = 19;                               // HK: Leader
        }
    }

    if (uVictim)
    {
        if (groupsize > 1)
            honor_f /= groupsize;

        // apply honor multiplier from aura (not stacking-get highest)
        AddPct(honor_f, GetMaxPositiveAuraModifier(SPELL_AURA_MOD_HONOR_GAIN_PCT));
    }

    honor_f *= sWorld->getRate(RATE_HONOR);
    // Back to int now
    honor = int32(honor_f);
    // honor - for show honor points in log
    // victim_guid - for show victim name in log
    // victim_rank [1..4]  HK: <dishonored rank>
    // victim_rank [5..19] HK: <alliance\horde rank>
    // victim_rank [0, 20+] HK: <>
    WorldPacket data(SMSG_PVP_CREDIT, 4 + 8 + 4);
    data << honor;
    data << victim_guid;
    data << victim_rank;

    // Xinef: non quest case, quest honor obtain is send in quest reward packet
    if (uVictim || groupsize > 0)
        SendDirectMessage(&data);

    // add honor points
    ModifyHonorPoints(honor);

    ApplyModUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION, honor, true);

    // Xinef: Battleground experience
    if (awardXP)
        if (Battleground* bg = GetBattleground())
        {
            bg->UpdatePlayerScore(this, SCORE_BONUS_HONOR, honor, false); //false: prevent looping
            // Xinef: Only for BG activities
            if (!uVictim)
            {
                uint32 xp = static_cast<uint32>(honor * (3 + GetLevel() * 0.30f) * sWorld->getRate(RATE_XP_BATTLEGROUND_BONUS));
                sScriptMgr->OnPlayerGiveXP(this, xp, nullptr, PlayerXPSource::XPSOURCE_BATTLEGROUND);
                GiveXP(xp, nullptr);
            }
        }

    if (sWorld->getBoolConfig(CONFIG_PVP_TOKEN_ENABLE))
    {
        if (!uVictim || uVictim == this || uVictim->HasNoPVPCreditAura())
            return true;

        if (uVictim->IsPlayer())
        {
            // Check if allowed to receive it in current map
            uint8 MapType = sWorld->getIntConfig(CONFIG_PVP_TOKEN_MAP_TYPE);
            if ((MapType == 1 && !InBattleground() && !IsFFAPvP())
                    || (MapType == 2 && !IsFFAPvP())
                    || (MapType == 3 && !InBattleground()))
                return true;

            uint32 itemID = sWorld->getIntConfig(CONFIG_PVP_TOKEN_ID);
            int32 count = sWorld->getIntConfig(CONFIG_PVP_TOKEN_COUNT);

            if (AddItem(itemID, count))
                ChatHandler(GetSession()).PSendSysMessage("You have been awarded a token for slaying another player.");
        }
    }

    return true;
}

void Player::SetHonorPoints(uint32 value)
{
    if (value > sWorld->getIntConfig(CONFIG_MAX_HONOR_POINTS))
    {
        if (int32 copperPerPoint = sWorld->getIntConfig(CONFIG_MAX_HONOR_POINTS_MONEY_PER_POINT))
        {
            // Only convert points on login, not when awarded honor points.
            if (isBeingLoaded())
            {
                int32 excessPoints = value - sWorld->getIntConfig(CONFIG_MAX_HONOR_POINTS);
                ModifyMoney(excessPoints * copperPerPoint);
            }
        }

        value = sWorld->getIntConfig(CONFIG_MAX_HONOR_POINTS);
    }
    SetUInt32Value(PLAYER_FIELD_HONOR_CURRENCY, value);
    if (value)
        AddKnownCurrency(ITEM_HONOR_POINTS_ID);
}

void Player::SetArenaPoints(uint32 value)
{
    if (value > sWorld->getIntConfig(CONFIG_MAX_ARENA_POINTS))
        value = sWorld->getIntConfig(CONFIG_MAX_ARENA_POINTS);
    SetUInt32Value(PLAYER_FIELD_ARENA_CURRENCY, value);
    if (value)
        AddKnownCurrency(ITEM_ARENA_POINTS_ID);
}

void Player::ModifyHonorPoints(int32 value, CharacterDatabaseTransaction trans)
{
    int32 newValue = int32(GetHonorPoints()) + value;
    if (newValue < 0)
        newValue = 0;
    SetHonorPoints(uint32(newValue));

    if (trans)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_HONOR_POINTS);
        stmt->SetData(0, newValue);
        stmt->SetData(1, GetGUID().GetRawValue());
        trans->Append(stmt);
    }
}

void Player::ModifyArenaPoints(int32 value, CharacterDatabaseTransaction trans)
{
    int32 newValue = int32(GetArenaPoints()) + value;
    if (newValue < 0)
        newValue = 0;
    SetArenaPoints(uint32(newValue));

    if (trans)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_ARENA_POINTS);
        stmt->SetData(0, newValue);
        stmt->SetData(1, GetGUID().GetRawValue());
        trans->Append(stmt);
    }
}

uint32 Player::GetArenaTeamIdFromDB(ObjectGuid guid, uint8 type)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ARENA_TEAM_ID_BY_PLAYER_GUID);
    stmt->SetData(0, guid.GetCounter());
    stmt->SetData(1, type);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (!result)
        return 0;

    uint32 id = (*result)[0].Get<uint32>();
    return id;
}

bool Player::IsOutdoorPvPActive()
{
    return IsAlive() && !HasInvisibilityAura() && !HasStealthAura() && IsPvP() && !HasUnitMovementFlag(MOVEMENTFLAG_FLYING) && !IsInFlight();
}

void Player::LeaveAllArenaTeams(ObjectGuid guid)
{
    // xinef: sync query
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PLAYER_ARENA_TEAMS);
    stmt->SetData(0, guid.GetCounter());
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 arenaTeamId = fields[0].Get<uint32>();
        if (arenaTeamId != 0)
        {
            ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(arenaTeamId);
            if (arenaTeam)
                arenaTeam->DelMember(guid, true);
        }
    } while (result->NextRow());
}

void Player::LeaveBattleground(Battleground* bg)
{
    if (!bg)
        bg = GetBattleground();

    if (!bg)
        return;

    // Deserter tracker - leave BG
    if (bg->isBattleground() && (bg->GetStatus() == STATUS_IN_PROGRESS || bg->GetStatus() == STATUS_WAIT_JOIN))
    {
        if (sWorld->getBoolConfig(CONFIG_BATTLEGROUND_TRACK_DESERTERS))
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_DESERTER_TRACK);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, BG_DESERTION_TYPE_LEAVE_BG);
            CharacterDatabase.Execute(stmt);
        }
        sScriptMgr->OnPlayerBattlegroundDesertion(this, BG_DESERTION_TYPE_LEAVE_BG);
    }

    if (bg->isArena() && (bg->GetStatus() == STATUS_IN_PROGRESS || bg->GetStatus() == STATUS_WAIT_JOIN))
        sScriptMgr->OnPlayerBattlegroundDesertion(this, ARENA_DESERTION_TYPE_LEAVE_BG);

    // xinef: reset corpse reclaim time
    m_deathExpireTime = GameTime::GetGameTime().count();

    // Remove all dots
    RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE);
    RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
    RemoveAurasByType(SPELL_AURA_PERIODIC_LEECH);

    // pussywizard: clear movement, because after porting player will move to arena cords
    GetMotionMaster()->MovementExpired();
    StopMoving();
    TeleportToEntryPoint();
}

bool Player::CanJoinToBattleground(Battleground const* bg) const
{
    // check Deserter debuff
    if (HasAura(26013))
        return false;

    if (bg->isArena())
        return GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_ARENAS);
    if (bg->GetBgTypeID() == BATTLEGROUND_RB)
        return GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_RANDOM_BG);
    return GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_NORMAL_BG);
}

void Player::ReportedAfkBy(Player* reporter)
{
    Battleground* bg = GetBattleground();
    // Battleground also must be in progress!
    if (!bg || bg != reporter->GetBattleground() || GetTeamId() != reporter->GetTeamId() || bg->GetStatus() != STATUS_IN_PROGRESS)
        return;

    // Xinef: 2 minutes startup + 2 minute of match
    if (bg->GetStartTime() < sWorld->getIntConfig(CONFIG_BATTLEGROUND_REPORT_AFK_TIMER) * MINUTE * IN_MILLISECONDS)
        return;

    // check if player has 'Idle' or 'Inactive' debuff
    if (m_bgData.bgAfkReporter.find(reporter->GetGUID()) == m_bgData.bgAfkReporter.end() && !HasAura(43680) && !HasAura(43681) && reporter->CanReportAfkDueToLimit())
    {
        m_bgData.bgAfkReporter.insert(reporter->GetGUID());
        // by default 3 players have to complain to apply debuff
        if (m_bgData.bgAfkReporter.size() >= sWorld->getIntConfig(CONFIG_BATTLEGROUND_REPORT_AFK))
        {
            // cast 'Idle' spell
            CastSpell(this, 43680, true);
            m_bgData.bgAfkReporter.clear();
        }
    }
}

Battleground* Player::GetBattleground(bool create) const
{
    if (GetBattlegroundId() == 0)
        return nullptr;

    Battleground* bg = sBattlegroundMgr->GetBattleground(GetBattlegroundId(), GetBattlegroundTypeId());
    return (create || (bg && bg->FindBgMap()) ? bg : nullptr);
}

bool Player::InBattlegroundQueue(bool ignoreArena) const
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].bgQueueTypeId != BATTLEGROUND_QUEUE_NONE &&
            (!ignoreArena || (_BgBattlegroundQueueID[i].bgQueueTypeId != BATTLEGROUND_QUEUE_2v2 &&
                _BgBattlegroundQueueID[i].bgQueueTypeId != BATTLEGROUND_QUEUE_3v3 &&
                _BgBattlegroundQueueID[i].bgQueueTypeId != BATTLEGROUND_QUEUE_5v5)))
            return true;
    return false;
}

BattlegroundQueueTypeId Player::GetBattlegroundQueueTypeId(uint32 index) const
{
    return _BgBattlegroundQueueID[index].bgQueueTypeId;
}

uint32 Player::GetBattlegroundQueueIndex(BattlegroundQueueTypeId bgQueueTypeId) const
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == bgQueueTypeId)
            return i;

    return PLAYER_MAX_BATTLEGROUND_QUEUES;
}

bool Player::IsInvitedForBattlegroundQueueType(BattlegroundQueueTypeId bgQueueTypeId) const
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == bgQueueTypeId)
            return _BgBattlegroundQueueID[i].invitedToInstance != 0;

    return false;
}

bool Player::InBattlegroundQueueForBattlegroundQueueType(BattlegroundQueueTypeId bgQueueTypeId) const
{
    return GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES;
}

uint32 Player::AddBattlegroundQueueId(BattlegroundQueueTypeId val)
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == BATTLEGROUND_QUEUE_NONE || _BgBattlegroundQueueID[i].bgQueueTypeId == val)
        {
            _BgBattlegroundQueueID[i].bgQueueTypeId = val;
            _BgBattlegroundQueueID[i].invitedToInstance = 0;
            return i;
        }
    }

    return PLAYER_MAX_BATTLEGROUND_QUEUES;
}

bool Player::HasFreeBattlegroundQueueId() const
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            return true;

    return false;
}

void Player::RemoveBattlegroundQueueId(BattlegroundQueueTypeId val)
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == val)
        {
            _BgBattlegroundQueueID[i].bgQueueTypeId = BATTLEGROUND_QUEUE_NONE;
            _BgBattlegroundQueueID[i].invitedToInstance = 0;
            return;
        }
    }
}

void Player::SetInviteForBattlegroundQueueType(BattlegroundQueueTypeId bgQueueTypeId, uint32 instanceId)
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].bgQueueTypeId == bgQueueTypeId)
            _BgBattlegroundQueueID[i].invitedToInstance = instanceId;
}

bool Player::IsInvitedForBattlegroundInstance(uint32 instanceId) const
{
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (_BgBattlegroundQueueID[i].invitedToInstance == instanceId)
            return true;

    return false;
}

bool Player::InArena() const
{
    Battleground* bg = GetBattleground();
    if (!bg || !bg->isArena())
        return false;

    return true;
}

void Player::SetBattlegroundId(uint32 id, BattlegroundTypeId bgTypeId, uint32 queueSlot, bool invited, bool isRandom, TeamId teamId)
{
    m_bgData.bgInstanceID = id;
    m_bgData.bgTypeID = bgTypeId;
    m_bgData.bgQueueSlot = queueSlot;
    m_bgData.isInvited = invited;
    m_bgData.bgIsRandom = isRandom;

    m_bgData.bgTeamId = teamId;
    SetByteValue(PLAYER_BYTES_3, 3, uint8(teamId == TEAM_ALLIANCE ? 1 : 0));
}

OutdoorPvP* Player::GetOutdoorPvP() const
{
    return sOutdoorPvPMgr->GetOutdoorPvPToZoneId(GetZoneId());
}

bool Player::isHonorOrXPTarget(Unit* victim) const
{
    uint8 v_level = victim->GetLevel();
    uint8 k_grey  = Acore::XP::GetGrayLevel(GetLevel());

    // Victim level less gray level
    if (v_level <= k_grey)
        return false;

    if (victim->IsCreature())
        if (victim->IsTotem() || victim->IsCritter() || victim->IsPet() || victim->ToCreature()->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_XP))
            return false;

    return true;
}

void Player::SetBattlegroundOrBattlefieldRaid(Group* group, int8 subgroup)
{
    //we must move references from m_group to m_originalGroup
    if (GetGroup() && (GetGroup()->isBGGroup() || GetGroup()->isBFGroup()))
    {
        LOG_INFO("misc", "Player::SetBattlegroundOrBattlefieldRaid - current group is {} group!", (GetGroup()->isBGGroup() ? "BG" : "BF"));
        //ABORT(); // pussywizard: origanal group can never be bf/bg group
    }

    SetOriginalGroup(GetGroup(), GetSubGroup());

    m_group.unlink();
    m_group.link(group, this);
    m_group.setSubGroup((uint8)subgroup);
}

void Player::RemoveFromBattlegroundOrBattlefieldRaid()
{
    //remove existing reference
    m_group.unlink();
    if (Group* group = GetOriginalGroup())
    {
        m_group.link(group, this);
        m_group.setSubGroup(GetOriginalSubGroup());
    }
    SetOriginalGroup(nullptr);
}

bool Player::CanUseBattlegroundObject(GameObject* gameobject) const
{
    // It is possible to call this method will a nullptr pointer, only skipping faction check.
    if (gameobject)
    {
        FactionTemplateEntry const* playerFaction = GetFactionTemplateEntry();
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(gameobject->GetUInt32Value(GAMEOBJECT_FACTION));

        if (playerFaction && faction && !playerFaction->IsFriendlyTo(*faction))
            return false;
    }

    /**
     * @bug
     * sometimes when player clicks on flag in AB - client won't send gameobject_use, only gameobject_report_use packet
     * Note: Mount, stealth and invisibility will be removed when used
     */
    return (!isTotalImmune() &&                            // Damage immune
            !HasAura(SPELL_RECENTLY_DROPPED_FLAG) &&       // Still has recently held flag debuff
            IsAlive());                                    // Alive
}

void Player::SetArenaTeamInfoField(uint8 slot, ArenaTeamInfoType type, uint32 value)
{
    if (sScriptMgr->OnPlayerNotSetArenaTeamInfoField(this, slot, type, value))
        SetUInt32Value(PLAYER_FIELD_ARENA_TEAM_INFO_1_1 + (slot * ARENA_TEAM_END) + type, value);
}

uint32 Player::GetArenaTeamId(uint8 slot) const
{
    uint32 result = GetUInt32Value(PLAYER_FIELD_ARENA_TEAM_INFO_1_1 + (slot * ARENA_TEAM_END) + ARENA_TEAM_ID);

    sScriptMgr->OnPlayerGetArenaTeamId(const_cast<Player*>(this), slot, result);

    return result;
}

bool Player::IsFFAPvP()
{
    bool result = Unit::IsFFAPvP();

    sScriptMgr->OnPlayerIsFFAPvP(this, result);

    return result;
}

bool Player::IsPvP()
{
    bool result = Unit::IsPvP();

    sScriptMgr->OnPlayerIsPvP(this, result);

    return result;
}
