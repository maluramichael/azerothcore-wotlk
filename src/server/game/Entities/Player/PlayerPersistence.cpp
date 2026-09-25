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
#include "AccountMgr.h"
#include "AchievementMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battlefield.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterDatabaseCleaner.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "GameEventMgr.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "InstanceSaveMgr.h"
#include "LFGMgr.h"
#include "Language.h"
#include "Log.h"
#include "LootItemStorage.h"
#include "MailMgr.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "OutdoorPvP.h"
#include "Pet.h"
#include "Player.h"
#include "QueryHolder.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ReputationMgr.h"
#include "ScriptMgr.h"
#include "ScriptObjectFwd.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "Transport.h"
#include "Unit.h"
#include "UpdateFieldFlags.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "GridNotifiersImpl.h"
/*********************************************************/
/***                    STORAGE SYSTEM                 ***/
/*********************************************************/

void Player::_LoadDeclinedNames(PreparedQueryResult result)
{
    if (!result)
        return;

    delete m_declinedname;
    m_declinedname = new DeclinedName;
    for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
        m_declinedname->name[i] = (*result)[i].Get<std::string>();
}

void Player::_LoadArenaTeamInfo()
{
    memset((void*)&m_uint32Values[PLAYER_FIELD_ARENA_TEAM_INFO_1_1], 0, sizeof(uint32) * MAX_ARENA_SLOT * ARENA_TEAM_END);

    for (auto const& itr : ArenaTeam::ArenaSlotByType)
        if (uint32 arenaTeamId = sCharacterCache->GetCharacterArenaTeamIdByGuid(GetGUID(), itr.second))
        {
            ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(arenaTeamId);
            if (!arenaTeam)
            {
                LOG_ERROR("bg.arena", "Player::_LoadArenaTeamInfo: Team with ID {} not found.", arenaTeamId);
                continue;
            }
            ArenaTeamMember const* member = arenaTeam->GetMember(GetGUID());
            if (!member)
            {
                LOG_ERROR("bg.arena", "Player::_LoadArenaTeamInfo: No members in the arena team ({}) was found.", arenaTeamId);
                continue;
            }
            uint8 slot = itr.second;

            SetArenaTeamInfoField(slot, ARENA_TEAM_ID, arenaTeamId);
            SetArenaTeamInfoField(slot, ARENA_TEAM_TYPE, arenaTeam->GetType());
            SetArenaTeamInfoField(slot, ARENA_TEAM_MEMBER, (arenaTeam->GetCaptain() == GetGUID()) ? 0 : 1);
            SetArenaTeamInfoField(slot, ARENA_TEAM_GAMES_WEEK, member->WeekGames);
            SetArenaTeamInfoField(slot, ARENA_TEAM_GAMES_SEASON, member->SeasonGames);
            SetArenaTeamInfoField(slot, ARENA_TEAM_WINS_SEASON, member->SeasonWins);
            SetArenaTeamInfoField(slot, ARENA_TEAM_PERSONAL_RATING, member->PersonalRating);
        }
}

void Player::_LoadEquipmentSets(PreparedQueryResult result)
{
    // SetQuery(PLAYER_LOGIN_QUERY_LOADEQUIPMENTSETS,   "SELECT setguid, setindex, name, iconname, item0, item1, item2, item3, item4, item5, item6, item7, item8, item9, item10, item11, item12, item13, item14, item15, item16, item17, item18 FROM character_equipmentsets WHERE guid = '{}' ORDER BY setindex", m_guid.GetCounter());
    if (!result)
        return;

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        EquipmentSet eqSet;

        eqSet.Guid      = fields[0].Get<uint64>();
        uint8 index    = fields[1].Get<uint8>();
        eqSet.Name      = fields[2].Get<std::string>();
        eqSet.IconName  = fields[3].Get<std::string>();
        eqSet.IgnoreMask = fields[4].Get<uint32>();
        eqSet.state     = EQUIPMENT_SET_UNCHANGED;

        for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
            eqSet.Items[i] = ObjectGuid::Create<HighGuid::Item>(fields[5 + i].Get<uint32>());

        m_EquipmentSets[index] = eqSet;

        ++count;

        if (count >= MAX_EQUIPMENT_SET_INDEX)                // client limit
            break;
    } while (result->NextRow());
}

void Player::_LoadEntryPointData(PreparedQueryResult result)
{
    if (!result)
        return;

    Field* fields = result->Fetch();
    m_entryPointData.joinPos = WorldLocation(fields[4].Get<uint32>(),  // Map
                                             fields[0].Get<float>(),   // X
                                             fields[1].Get<float>(),   // Y
                                             fields[2].Get<float>(),   // Z
                                             fields[3].Get<float>());  // Orientation

    m_entryPointData.taxiPath[0] = fields[5].Get<uint32>();
    m_entryPointData.taxiPath[1] = fields[6].Get<uint32>();
    m_entryPointData.mountSpell = fields[7].Get<uint32>();
}

bool Player::LoadPositionFromDB(uint32& mapid, float& x, float& y, float& z, float& o, bool& in_flight, ObjectGuid::LowType guid)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_POSITION);
    stmt->SetData(0, guid);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (!result)
        return false;

    Field* fields = result->Fetch();

    x = fields[0].Get<float>();
    y = fields[1].Get<float>();
    z = fields[2].Get<float>();
    o = fields[3].Get<float>();
    mapid = fields[4].Get<uint16>();
    in_flight = !fields[5].Get<std::string>().empty();

    return true;
}

bool Player::LoadFromDB(ObjectGuid playerGuid, CharacterDatabaseQueryHolder const& holder)
{
    ////                                                     0     1        2     3     4        5      6    7      8     9    10    11         12         13           14         15         16
    //QueryResult* result = CharacterDatabase.Query("SELECT guid, account, name, race, class, gender, level, xp, money, skin, face, hairStyle, hairColor, facialStyle, bankSlots, restState, playerFlags, "
    // 17          18          19          20   21           22        23        24         25         26          27           28                 29
    //"position_x, position_y, position_z, map, orientation, taximask, cinematic, totaltime, leveltime, rest_bonus, logout_time, is_logout_resting, resettalents_cost, "
    // 30                 31       32       33       34       35         36           37             38        39    40      41                 42         43
    //"resettalents_time, trans_x, trans_y, trans_z, trans_o, transguid, extra_flags, stable_slots, at_login, zone, online, death_expire_time, taxi_path, instance_mode_mask, "
    // 44           45                46                 47                    48          49          50              51           52               53              54
    //"arenaPoints, totalHonorPoints, todayHonorPoints, yesterdayHonorPoints, totalKills, todayKills, yesterdayKills, chosenTitle, knownCurrencies, watchedFaction, drunk, "
    // 55      56      57      58      59      60      61      62      63           64                 65                 66             67              68      69
    //"health, power1, power2, power3, power4, power5, power6, power7, instance_id, talentGroupsCount, activeTalentGroup, exploredZones, equipmentCache, ammoId, knownTitles,
    // 70          71               72            73                     74
    //"actionBars, grantableLevels, innTriggerId, extraBonusTalentCount, UNIX_TIMESTAMP(creation_date) FROM characters WHERE guid = '{}'", guid);
    PreparedQueryResult result = holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_FROM);

    if (!result)
    {
        LOG_ERROR("entities.player", "Player ({}) not found in table `characters`, can't load. ", playerGuid.ToString());
        return false;
    }

    Field* fields = result->Fetch();

    uint32 dbAccountId = fields[1].Get<uint32>();

    // check if the character's account in the db and the logged in account match.
    // player should be able to load/delete character only with correct account!
    if (dbAccountId != GetSession()->GetAccountId())
    {
        LOG_ERROR("entities.player", "Player ({}) loading from wrong account (is: {}, should be: {})", playerGuid.ToString(), GetSession()->GetAccountId(), dbAccountId);
        return false;
    }

    if (holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_BANNED))
    {
        LOG_ERROR("entities.player", "Player ({}) is banned, can't load.", playerGuid.ToString());
        return false;
    }

    uint64 guid = playerGuid.GetRawValue();

    Object::_Create(playerGuid);

    m_name = fields[2].Get<std::string>();

    // check name limitations
    uint8 nameResult = ObjectMgr::CheckPlayerName(m_name);
    if (nameResult != CHAR_NAME_SUCCESS &&
        !(nameResult == CHAR_NAME_RESERVED && GetSession()->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_CHARACTER_CREATION_RESERVEDNAME)))
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ADD_AT_LOGIN_FLAG);
        stmt->SetData(0, uint16(AT_LOGIN_RENAME));
        stmt->SetData(1, guid);
        CharacterDatabase.Execute(stmt);
        return false;
    }

    uint8 Gender = fields[5].Get<uint8>();
    if (!IsValidGender(Gender))
    {
        LOG_ERROR("entities.player", "Player (GUID: {}) has wrong gender ({}), can't be loaded.", guid, Gender);
        return false;
    }

    // overwrite some data fields
    uint32 bytes0 = 0;
    bytes0 |= fields[3].Get<uint8>();                         // race
    bytes0 |= fields[4].Get<uint8>() << 8;                    // class
    bytes0 |= Gender << 16;                                 // gender
    SetUInt32Value(UNIT_FIELD_BYTES_0, bytes0);

    m_realRace = fields[3].Get<uint8>(); // set real race
    m_race = fields[3].Get<uint8>(); // set real race

    SetUInt32Value(UNIT_FIELD_LEVEL, fields[6].Get<uint8>());
    SetUInt32Value(PLAYER_XP, fields[7].Get<uint32>());

    if (!_LoadIntoDataField(fields[66].Get<std::string>(), PLAYER_EXPLORED_ZONES_1, PLAYER_EXPLORED_ZONES_SIZE))
    {
        LOG_WARN("entities.player.loading", "Player::LoadFromDB: Player ({}) has invalid exploredzones data ({}). Forcing partial load.", guid, fields[66].Get<std::string_view>());
    }

    if (!_LoadIntoDataField(fields[69].Get<std::string>(), PLAYER__FIELD_KNOWN_TITLES, KNOWN_TITLES_SIZE * 2))
    {
        LOG_WARN("entities.player.loading", "Player::LoadFromDB: Player ({}) has invalid knowntitles mask ({}). Forcing partial load.", guid, fields[69].Get<std::string_view>());
    }

    SetObjectScale(1.0f);
    SetFloatValue(UNIT_FIELD_HOVERHEIGHT, 1.0f);

    // load character creation date, relevant for achievements of type average
    SetCreationTime(fields[74].Get<Seconds>());

    // load achievements before anything else to prevent multiple gains for the same achievement/criteria on every loading (as loading does call UpdateAchievementCriteria)
    m_achievementMgr->LoadFromDB(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_ACHIEVEMENTS), holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_CRITERIA_PROGRESS), holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_OFFLINE_ACHIEVEMENTS_UPDATES));

    uint32 money = fields[8].Get<uint32>();
    if (money > MAX_MONEY_AMOUNT)
        money = MAX_MONEY_AMOUNT;
    SetMoney(money);

    SetByteValue(PLAYER_BYTES, 0, fields[9].Get<uint8>());
    SetByteValue(PLAYER_BYTES, 1, fields[10].Get<uint8>());
    SetByteValue(PLAYER_BYTES, 2, fields[11].Get<uint8>());
    SetByteValue(PLAYER_BYTES, 3, fields[12].Get<uint8>());
    SetByteValue(PLAYER_BYTES_2, 0, fields[13].Get<uint8>());
    SetByteValue(PLAYER_BYTES_2, 2, fields[14].Get<uint8>());
    SetByteValue(PLAYER_BYTES_2, 3, fields[15].Get<uint8>());
    SetByteValue(PLAYER_BYTES_3, 0, fields[5].Get<uint8>());
    SetByteValue(PLAYER_BYTES_3, 1, fields[54].Get<uint8>());
    ReplaceAllPlayerFlags((PlayerFlags)fields[16].Get<uint32>());

    RemovePlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME);
    RemovePlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME);

    if (GetSession()->IsAffectedByCAIS())
    {
        Seconds const accountPlayedTime = GetSession()->GetConsecutivePlayTime(GameTime::GetGameTime());

        if (accountPlayedTime >= PLAY_TIME_LIMIT_FULL)
            SetPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME);
        else if (accountPlayedTime >= PLAY_TIME_LIMIT_PARTIAL)
            SetPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME);
    }

    SetInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX, fields[53].Get<uint32>());

    SetUInt64Value(PLAYER_FIELD_KNOWN_CURRENCIES, fields[52].Get<uint64>());

    SetUInt32Value(PLAYER_AMMO_ID, fields[68].Get<uint32>());

    // set which actionbars the client has active - DO NOT REMOVE EVER AGAIN (can be changed though, if it does change fieldwise)
    SetByteValue(PLAYER_FIELD_BYTES, 2, fields[70].Get<uint8>());

    InitDisplayIds();

    // cleanup inventory related item value fields (its will be filled correctly in _LoadInventory)
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        SetGuidValue(PLAYER_FIELD_INV_SLOT_HEAD + (slot * 2), ObjectGuid::Empty);
        SetVisibleItemSlot(slot, nullptr);

        delete m_items[slot];
        m_items[slot] = nullptr;
    }

    LOG_DEBUG("entities.player.loading", "Load Basic value of player {} is: ", m_name);
    outDebugValues();

    //Need to call it to initialize m_team (m_team can be calculated from race)
    //Other way is to saves m_team into characters table.
    SetFactionForRace(getRace(true));

    // pussywizard: create empty instance bind containers if necessary
    sInstanceSaveMgr->PlayerCreateBoundInstancesMaps(playerGuid);

    // load home bind and check in same time class/race pair, it used later for restore broken positions
    if (!_LoadHomeBind(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_HOME_BIND)))
        return false;

    InitPrimaryProfessions();                               // to max set before any spell loaded

    // init saved position, and fix it later if problematic
    int32 transLowGUID = fields[35].Get<int32>();
    Relocate(fields[17].Get<float>(), fields[18].Get<float>(), fields[19].Get<float>(), fields[21].Get<float>());
    uint32 mapId = fields[20].Get<uint16>();
    uint32 instanceId = fields[63].Get<uint32>();

    uint32 dungeonDiff = fields[43].Get<uint8>() & 0x0F;
    if (dungeonDiff >= MAX_DUNGEON_DIFFICULTY)
        dungeonDiff = DUNGEON_DIFFICULTY_NORMAL;
    uint32 raidDiff = (fields[43].Get<uint8>() >> 4) & 0x0F;
    if (raidDiff >= MAX_RAID_DIFFICULTY)
        raidDiff = RAID_DIFFICULTY_10MAN_NORMAL;
    SetDungeonDifficulty(Difficulty(dungeonDiff));          // may be changed in _LoadGroup
    SetRaidDifficulty(Difficulty(raidDiff));                // may be changed in _LoadGroup

    std::string taxi_nodes = fields[42].Get<std::string>();

    auto RelocateToHomebind = [this, &mapId, &instanceId]() { mapId = m_homebindMapId; instanceId = 0; Relocate(m_homebindX, m_homebindY, m_homebindZ); };

    _LoadGroup();

    _LoadArenaTeamInfo();

    SetArenaPoints(fields[44].Get<uint32>());

    SetHonorPoints(fields[45].Get<uint32>());
    SetUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION, fields[46].Get<uint32>());
    SetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION, fields[47].Get<uint32>());
    SetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS, fields[48].Get<uint32>());
    SetUInt16Value(PLAYER_FIELD_KILLS, 0, fields[49].Get<uint16>());
    SetUInt16Value(PLAYER_FIELD_KILLS, 1, fields[50].Get<uint16>());

    _LoadInstanceTimeRestrictions(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_INSTANCE_LOCK_TIMES));
    _LoadEntryPointData(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_ENTRY_POINT));

    GetSession()->SetPlayer(this);
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);

    Map* map = nullptr;

    // pussywizard: group changed difficulty when player was offline, teleport to the enterance of new difficulty
    if (mapEntry && ((mapEntry->IsNonRaidDungeon() && dungeonDiff != GetDungeonDifficulty()) || (mapEntry->IsRaid() && raidDiff != GetRaidDifficulty())))
    {
        bool fixed = false;
        if (uint32 destInstId = sInstanceSaveMgr->PlayerGetDestinationInstanceId(this, mapId, GetDifficulty(mapEntry->IsRaid())))
        {
            instanceId = destInstId;
            if (AreaTriggerTeleport const* at = sObjectMgr->GetMapEntranceTrigger(mapId))
            {
                Relocate(at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
                fixed = true;
            }
        }
        if (!fixed)
        {
            RelocateToHomebind();
            mapEntry = sMapStore.LookupEntry(mapId);
        }
    }

    if (!mapEntry || !IsPositionValid())
    {
        LOG_ERROR("entities.player", "Player (guidlow {}) have invalid coordinates (MapId: {} X: {} Y: {} Z: {} O: {}). Teleport to default race/class locations.", guid, mapId, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
        RelocateToHomebind();
    }
        // Player was saved in Arena or Bg
    else if (mapEntry->IsBattlegroundOrArena())
    {
        // xinef: resurrect player, cant log in dead without corpse
        {
            if (HasSpiritOfRedemptionAura())
                RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            if (!IsAlive())
                ResurrectPlayer(1.0f);
        }

        WorldLocation const& _loc = GetEntryPoint();
        mapId = _loc.GetMapId();
        instanceId = 0;

        if (mapId == MAPID_INVALID)
        {
            RelocateToHomebind();
        }
        else
        {
            Relocate(&_loc);

            // xinef: restore taxi flight from entry point data
            if (m_entryPointData.HasTaxiPath())
            {
                m_taxi.AddTaxiDestination(m_entryPointData.taxiPath[0]);
                m_taxi.AddTaxiDestination(m_entryPointData.taxiPath[1]);
                m_entryPointData.ClearTaxiPath();
            }
        }
    }
        // currently we do not support transport in bg
    else if (transLowGUID != 0)
    {
        // transLowGUID > 0 ---> motion transport guid
        // transLowGUID < 0 ---> static transport spawn id
        Transport* transGO = nullptr;
        if (transLowGUID > 0)
        {
            ObjectGuid transGUID = ObjectGuid::Create<HighGuid::Mo_Transport>(transLowGUID);
            transGO = HashMapHolder<MotionTransport>::Find(transGUID);
        }
        else
        {
            map = sMapMgr->CreateMap(mapId, this);
            if (map)
            {
                auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(std::abs(transLowGUID));
                if (bounds.first != bounds.second)
                    transGO = bounds.first->second->ToTransport();
            }
        }

        // pussywizard: must be on map, for one world tick transport is not in map and has old GetMapId(), player would be added to old map and to the transport, multithreading crashfix
        if (transGO && transGO->IsInWorld() && transGO->FindMap())
            m_transport = transGO;

        if (m_transport)
        {
            float x = fields[31].Get<float>(), y = fields[32].Get<float>(), z = fields[33].Get<float>(), o = fields[34].Get<float>();
            m_movementInfo.transport.guid = m_transport->GetGUID();
            m_movementInfo.transport.pos.Relocate(x, y, z, o);
            m_transport->CalculatePassengerPosition(x, y, z, &o);

            if (!Acore::IsValidMapCoord(x, y, z, o) || std::fabs(m_movementInfo.transport.pos.GetPositionX()) > 75.0f || std::fabs(m_movementInfo.transport.pos.GetPositionY()) > 75.0f || std::fabs(m_movementInfo.transport.pos.GetPositionZ()) > 75.0f)
            {
                m_transport = nullptr;
                m_movementInfo.transport.Reset();
                m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
                RelocateToHomebind();
            }
            else
            {
                Relocate(x, y, z, o);
                mapId = m_transport->GetMapId();
                m_transport->AddPassenger(this);
                AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
            }
        }
        else
        {
            bool fixed = false;
            if (mapEntry->Instanceable())
                if (AreaTriggerTeleport const* at = sObjectMgr->GetMapEntranceTrigger(mapId))
                {
                    fixed = true;
                    Relocate(at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
                }
            if (!fixed)
            RelocateToHomebind();
        }
    }
        // currently we do not support taxi in instance
    else if (!taxi_nodes.empty())
    {
        instanceId = 0;
        if (!m_taxi.LoadTaxiDestinationsFromString(taxi_nodes, GetTeamId(true)))
        {
            // xinef: could no load valid data for taxi, relocate to homebind and clear
            m_taxi.ClearTaxiDestinations();
            RelocateToHomebind();
        }
    }

    // Map could be changed before
    mapEntry = sMapStore.LookupEntry(mapId);
    // client without expansion support
    if (mapEntry)
    {
        if (GetSession()->Expansion() < mapEntry->Expansion())
        {
            LOG_DEBUG("entities.player.loading", "Player {} using client without required expansion tried login at non accessible map {}", GetName(), mapId);
            RelocateToHomebind();
        }

        // check whether player was unbound or is bound to another instance
        if (instanceId)
        {
            InstanceSave* save = sInstanceSaveMgr->PlayerGetInstanceSave(GetGUID(), mapId, GetDifficulty(mapEntry->IsRaid()));
            if (!save || save->GetInstanceId() != instanceId)
                instanceId = 0;
        }
    }

    // if the player is in an instance and it has been reset in the meantime teleport him to the entrance
    if ((instanceId && !sInstanceSaveMgr->GetInstanceSave(instanceId) && !mapEntry->IsBattlegroundOrArena()) || (!instanceId && mapEntry->IsDungeon()))
    {
        AreaTriggerTeleport const* at = sObjectMgr->GetMapEntranceTrigger(mapId);
        if (at)
            Relocate(at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
        else
            RelocateToHomebind();
    }

    // NOW player must have valid map
    // load the player's map here if it's not already loaded
    if (!map)
        map = sMapMgr->CreateMap(mapId, this);

    if (!map)
    {
        instanceId = 0;
        AreaTriggerTeleport const* at = sObjectMgr->GetGoBackTrigger(mapId);
        if (at)
        {
            LOG_ERROR("entities.player", "Player (guidlow {}) is teleported to gobacktrigger (Map: {} X: {} Y: {} Z: {} O: {}).", guid, mapId, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
            Relocate(at->target_X, at->target_Y, at->target_Z, GetOrientation());
            mapId = at->target_mapId;
        }
        else
        {
            LOG_ERROR("entities.player", "Player (guidlow {}) is teleported to home (Map: {} X: {} Y: {} Z: {} O: {}).", guid, mapId, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
            RelocateToHomebind();
        }

        map = sMapMgr->CreateMap(mapId, this);
        if (!map)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(true), getClass());
            mapId = info->mapId;
            Relocate(info->positionX, info->positionY, info->positionZ, 0.0f);
            LOG_ERROR("entities.player", "Player (guidlow {}) have invalid coordinates (X: {} Y: {} Z: {} O: {}). Teleport to default race/class locations.", guid, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
            map = sMapMgr->CreateMap(mapId, this);
            if (!map)
            {
                LOG_ERROR("entities.player", "Player (guidlow {}) has invalid default map coordinates (X: {} Y: {} Z: {} O: {}). or instance couldn't be created", guid, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
                return false;
            }
        }
    }

    SetMap(map);
    StoreRaidMapDifficulty();

    UpdatePositionData();

    SaveRecallPosition();

    time_t now = GameTime::GetGameTime().count();
    time_t logoutTime = time_t(fields[27].Get<uint32>());

    // since last logout (in seconds)
    uint32 time_diff = uint32(now - logoutTime); //uint64 is excessive for a time_diff in seconds.. uint32 allows for 136~ year difference.

    // randomize first save time in range [CONFIG_INTERVAL_SAVE] around [CONFIG_INTERVAL_SAVE]
    // this must help in case next save after mass player load after server startup
    m_nextSave = urand(m_nextSave / 2, m_nextSave * 3 / 2);

    // set value, including drunk invisibility detection
    // calculate sobering. after 15 minutes logged out, the player will be sober again
    uint8 newDrunkValue = 0;
    if (time_diff < uint32(GetDrunkValue()) * 9)
        newDrunkValue = GetDrunkValue() - time_diff / 9;

    SetDrunkValue(newDrunkValue);

    m_cinematic = fields[23].Get<uint8>();
    m_Played_time[PLAYED_TIME_TOTAL] = fields[24].Get<uint32>();
    m_Played_time[PLAYED_TIME_LEVEL] = fields[25].Get<uint32>();

    m_resetTalentsCost = fields[29].Get<uint32>();
    m_resetTalentsTime = time_t(fields[30].Get<uint32>());

    m_taxi.LoadTaxiMask(fields[22].Get<std::string_view>());            // must be before InitTaxiNodesForLevel

    uint32 extraflags = fields[36].Get<uint16>();

    // Mirror before the gate below so saved bits survive when the gate
    // skips effect application; otherwise the next SaveToDB writes 0
    // over them.
    m_ExtraFlags = extraflags;

    _LoadPetStable(fields[37].Get<uint8>(), holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_PET_SLOTS));

    m_atLoginFlags = fields[38].Get<uint16>();

    if (HasAtLoginFlag(AT_LOGIN_RENAME))
    {
        LOG_ERROR("entities.player", "Player {} tried to login while forced to rename, can't load.'", GetGUID().ToString());
        return false;
    }

    // Honor system
    // Update Honor kills data
    m_lastHonorUpdateTime = logoutTime;
    UpdateHonorFields();

    m_deathExpireTime = time_t(fields[41].Get<uint32>());

    if (m_deathExpireTime > now + MAX_DEATH_COUNT * DEATH_EXPIRE_STEP)
        m_deathExpireTime = now + MAX_DEATH_COUNT * DEATH_EXPIRE_STEP - 1;

    // clear channel spell data (if saved at channel spell casting)
    SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, ObjectGuid::Empty);
    SetUInt32Value(UNIT_CHANNEL_SPELL, 0);

    // clear charm/summon related fields
    SetOwnerGUID(ObjectGuid::Empty);
    SetGuidValue(UNIT_FIELD_CHARMEDBY, ObjectGuid::Empty);
    SetGuidValue(UNIT_FIELD_CHARM, ObjectGuid::Empty);
    SetGuidValue(UNIT_FIELD_SUMMON, ObjectGuid::Empty);
    SetGuidValue(PLAYER_FARSIGHT, ObjectGuid::Empty);
    SetCreatorGUID(ObjectGuid::Empty);

    RemoveUnitFlag2(UNIT_FLAG2_FORCE_MOVEMENT);

    // reset some aura modifiers before aura apply
    SetUInt32Value(PLAYER_TRACK_CREATURES, 0);
    SetUInt32Value(PLAYER_TRACK_RESOURCES, 0);

    // make sure the unit is considered not in duel for proper loading
    SetGuidValue(PLAYER_DUEL_ARBITER, ObjectGuid::Empty);
    SetUInt32Value(PLAYER_DUEL_TEAM, 0);

    // reset stats before loading any modifiers
    InitStatsForLevel();
    InitGlyphsForLevel();
    InitTaxiNodesForLevel();
    InitRunes();

    sScriptMgr->OnPlayerLoadFromDB(this);

    // make sure the unit is considered out of combat for proper loading
    ClearInCombat();

    // rest bonus can only be calculated after InitStatsForLevel()
    _restBonus = fields[26].Get<float>();

    if (time_diff > 0)
    {
        //speed collect rest bonus in offline, in logout, far from tavern, city (section/in hour)
        float bubble0 = 0.031f;
        //speed collect rest bonus in offline, in logout, in tavern, city (section/in hour)
        float bubble1 = 0.125f;
        float bubble  = fields[28].Get<uint8>() > 0
                       ? bubble1 * sWorld->getRate(RATE_REST_OFFLINE_IN_TAVERN_OR_CITY)
                       : bubble0 * sWorld->getRate(RATE_REST_OFFLINE_IN_WILDERNESS);

        // Client automatically doubles the value sent so we have to divide it by 2
        SetRestBonus(GetRestBonus() + time_diff * ((float)GetUInt32Value(PLAYER_NEXT_LEVEL_XP) / 144000)*bubble);
    }

    uint32 innTriggerId = fields[72].Get<uint32>();
    if (innTriggerId)
    {
        SetRestFlag(REST_FLAG_IN_TAVERN, innTriggerId);
    }

    // load skills after InitStatsForLevel because it triggering aura apply also
    _LoadSkills(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SKILLS));
    UpdateSkillsForLevel(); //update skills after load, to make sure they are correctly update at player load

    // apply original stats mods before spell loading or item equipment that call before equip _RemoveStatsMods()

    m_specsCount = fields[64].Get<uint8>();
    m_activeSpec = fields[65].Get<uint8>();

    LearnDefaultSkills();
    LearnCustomSpells();

    _LoadSpells(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SPELLS));
    _LoadTalents(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_TALENTS));

    _LoadGlyphs(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_GLYPHS));
    _LoadGlyphAuras();
    _LoadAuras(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_AURAS), time_diff);
    // add ghost flag (must be after aura load: PLAYER_FLAGS_GHOST set in aura)
    if (HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        m_deathState = DeathState::Dead;
        AddUnitState(UNIT_STATE_ISOLATED);
    }

    // pussywizard: remove auras that are removed at map change (after _LoadAuras)
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_CHANGE_MAP);

    // after spell load, learn rewarded spell if need also
    _LoadQuestStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS));
    _LoadQuestStatusRewarded(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_REW));
    _LoadDailyQuestStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_DAILY_QUEST_STATUS));
    _LoadWeeklyQuestStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_WEEKLY_QUEST_STATUS));
    _LoadSeasonalQuestStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SEASONAL_QUEST_STATUS));
    _LoadMonthlyQuestStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_MONTHLY_QUEST_STATUS));
    _LoadRandomBGStatus(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_RANDOM_BG));

    // Extra Bonus Talent Points
    m_extraBonusTalentCount = fields[73].Get<uint8>();

    // after spell, bonus talents, and quest load
    InitTalentForLevel();

    // must be before inventory (some items required reputation check)
    m_reputationMgr->LoadFromDB(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_REPUTATION));

    // xinef: load mails before inventory, so problematic items can be added to already loaded mails
    _LoadMail(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_MAILS), holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS));

    _LoadInventory(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_INVENTORY), time_diff);

    // update items with duration and realtime
    UpdateItemDuration(time_diff, true);

    _LoadActions(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_ACTIONS));

    m_social = sSocialMgr->LoadFromDB(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SOCIAL_LIST), GetGUID());

    // check PLAYER_CHOSEN_TITLE compatibility with PLAYER__FIELD_KNOWN_TITLES
    // note: PLAYER__FIELD_KNOWN_TITLES updated at quest status loaded
    uint32 curTitle = fields[51].Get<uint32>();
    if (curTitle && !HasTitle(curTitle))
        curTitle = 0;

    SetUInt32Value(PLAYER_CHOSEN_TITLE, curTitle);

    // has to be called after last Relocate() in Player::LoadFromDB
    SetFallInformation(GameTime::GetGameTime().count(), GetPositionZ());

    _LoadSpellCooldowns(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SPELL_COOLDOWNS));

    // Spell code allow apply any auras to dead character in load time in aura/spell/item loading
    // Do now before stats re-calculation cleanup for ghost state unexpected auras
    if (!IsAlive())
        RemoveAllAurasOnDeath();
    else
        RemoveAllAurasRequiringDeadTarget();

    //apply all stat bonuses from items and auras
    SetCanModifyStats(true);
    UpdateAllStats();

    // restore remembered power/health values (but not more max values)
    uint32 savedHealth = fields[55].Get<uint32>();
    SetHealth(savedHealth > GetMaxHealth() ? GetMaxHealth() : savedHealth);
    for (uint8 i = 0; i < MAX_POWERS; ++i)
    {
        uint32 savedPower = fields[56 + i].Get<uint32>();
        SetPower(Powers(i), savedPower > GetMaxPower(Powers(i)) ? GetMaxPower(Powers(i)) : savedPower);
    }

    LOG_DEBUG("entities.player.loading", "The value of player {} after load item and aura is: ", m_name);
    outDebugValues();

    // GM state
    if (GetSession()->HasPermission(rbac::RBAC_PERM_RESTORE_SAVED_GM_STATE))
    {
        switch (sWorld->getIntConfig(CONFIG_GM_LOGIN_STATE))
        {
            default:
            case 0:
                break;             // disable
            case 1:
                SetGameMaster(true);
                break;             // enable
            case 2:                                         // save state
                if (extraflags & PLAYER_EXTRA_GM_ON)
                    SetGameMaster(true);
                break;
        }

        switch (sWorld->getIntConfig(CONFIG_GM_VISIBLE_STATE))
        {
            default:
            case 0:
                SetGMVisible(false);
                break;             // invisible
            case 1:
                break;             // visible
            case 2:                                         // save state
                if (extraflags & PLAYER_EXTRA_GM_INVISIBLE)
                    SetGMVisible(false);
                break;
        }

        switch (sWorld->getIntConfig(CONFIG_GM_CHAT))
        {
            default:
            case 0:
                break;                 // disable
            case 1:
                SetGMChat(true);
                break;                 // enable
            case 2:                                         // save state
                if (extraflags & PLAYER_EXTRA_GM_CHAT)
                    SetGMChat(true);
                break;
        }

        switch (sWorld->getIntConfig(CONFIG_GM_WHISPERING_TO))
        {
            default:
            case 0:
                break;         // disable
            case 1:
                SetAcceptWhispers(true);
                break;         // enable
            case 2:                                         // save state
                if (extraflags & PLAYER_EXTRA_ACCEPT_WHISPERS)
                    SetAcceptWhispers(true);
                break;
        }
    }

    // RaF stuff.
    m_grantableLevels = fields[71].Get<uint8>();
    if (GetSession()->IsARecruiter() || (GetSession()->GetRecruiterId() != 0))
        SetDynamicFlag(UNIT_DYNFLAG_REFER_A_FRIEND);

    if (m_grantableLevels > 0)
        SetByteValue(PLAYER_FIELD_BYTES, 1, 0x01);

    _LoadDeclinedNames(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_DECLINED_NAMES));

    //m_achievementMgr->CheckAllAchievementCriteria(); // pussywizard: disabled this

    _LoadEquipmentSets(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_EQUIPMENT_SETS));

    _LoadBrewOfTheMonth(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_BREW_OF_THE_MONTH));

    _LoadCharacterSettings(holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_CHARACTER_SETTINGS));

    // Players are immune to taunt
    ApplySpellImmune(0, IMMUNITY_STATE, SPELL_AURA_MOD_TAUNT, true);
    ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_ATTACK_ME, true);

    // Init charm info
    PrepareCharmAISpells();

    // Fix aurastate auras, depending on health!
    // Set aurastate manualy, prevents aura switching
    if (HealthBelowPct(20))
        SetFlag(UNIT_FIELD_AURASTATE, 1 << (AURA_STATE_HEALTHLESS_20_PERCENT - 1));
    if (HealthBelowPct(35))
        SetFlag(UNIT_FIELD_AURASTATE, 1 << (AURA_STATE_HEALTHLESS_35_PERCENT - 1));
    if (HealthAbovePct(75))
        SetFlag(UNIT_FIELD_AURASTATE, 1 << (AURA_STATE_HEALTH_ABOVE_75_PERCENT - 1));

    // unapply aura stats if dont meet requirements
    AuraApplicationMap const& Auras = GetAppliedAuras();
    for (AuraApplicationMap::const_iterator itr = Auras.begin(); itr != Auras.end(); ++itr)
    {
        // we assume that all auras are applied now, aurastate was modfied MANUALY preventing any apply/unapply state switching
        Aura* aura = itr->second->GetBase();
        SpellInfo const* m_spellInfo = aura->GetSpellInfo();
        if (m_spellInfo->CasterAuraState != AURA_STATE_HEALTHLESS_20_PERCENT &&
            m_spellInfo->CasterAuraState != AURA_STATE_HEALTHLESS_35_PERCENT &&
            m_spellInfo->CasterAuraState != AURA_STATE_HEALTH_ABOVE_75_PERCENT)
            continue;

        if (!HasAuraState((AuraStateType)m_spellInfo->CasterAuraState))
            aura->HandleAllEffects(itr->second, AURA_EFFECT_HANDLE_REAL, false);
    }
    return true;
}

void Player::_LoadActions(PreparedQueryResult result)
{
    m_actionButtons.clear();

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 button = fields[0].Get<uint8>();
            uint32 action = fields[1].Get<uint32>();
            uint8 type = fields[2].Get<uint8>();

            if (ActionButton* ab = addActionButton(button, action, type))
                ab->uState = ACTIONBUTTON_UNCHANGED;
            else
            {

                LOG_ERROR("entities.player", "ActionButton loading problem, will be deleted from db. player: {}, guid: {}, button: {}, action: {}, type: {}", GetName(), GetGUID().GetCounter(), button, action, type);

                // Will deleted in DB at next save (it can create data until save but marked as deleted)
                m_actionButtons[button].uState = ACTIONBUTTON_DELETED;
            }
        } while (result->NextRow());
    }
}

void Player::_LoadAuras(PreparedQueryResult result, uint32 timediff)
{
    LOG_DEBUG("entities.player.loading", "Loading auras for player {}", GetGUID().ToString());

    /*                                                     0           1         2           3      4                5           6        7        8        9             10            11
    QueryResult* result = CharacterDatabase.Query("SELECT casterGuid, itemGuid, spell, effectMask, recalculateMask, stackCount, amount0, amount1, amount2, base_amount0, base_amount1, base_amount2,
                                                    12           13          14
                                                    maxDuration, remainTime, remainCharges FROM character_aura WHERE guid = '{}'", GetGUID().GetCounter());
    */

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            int32 damage[3];
            int32 baseDamage[3];
            ObjectGuid caster_guid = ObjectGuid(fields[0].Get<uint64>());
            ObjectGuid itemGuid = ObjectGuid(fields[1].Get<uint64>());
            uint32 spellid = fields[2].Get<uint32>();
            uint8 effmask = fields[3].Get<uint8>();
            uint8 recalculatemask = fields[4].Get<uint8>();
            uint8 stackcount = fields[5].Get<uint8>();
            damage[0] = fields[6].Get<int32>();
            damage[1] = fields[7].Get<int32>();
            damage[2] = fields[8].Get<int32>();
            baseDamage[0] = fields[9].Get<int32>();
            baseDamage[1] = fields[10].Get<int32>();
            baseDamage[2] = fields[11].Get<int32>();
            int32 maxduration = fields[12].Get<int32>();
            int32 remaintime = fields[13].Get<int32>();
            uint8 remaincharges = fields[14].Get<uint8>();

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
            if (!spellInfo)
            {
                LOG_ERROR("entities.player", "Unknown aura (spellid {}), ignore.", spellid);
                continue;
            }

            // Xinef: leave this
            if (spellInfo->HasAura(SPELL_AURA_MOUNTED))
            {
                SetMountBlockId(spellInfo->Id);
                continue;
            }

            // negative effects should continue counting down after logout
            if (remaintime != -1 && ((!spellInfo->IsPositive() && spellInfo->Id != 15007) || spellInfo->HasAttribute(SPELL_ATTR4_AURA_EXPIRES_OFFLINE))) // Xinef: resurrection sickness should not tick when logged off
            {
                if (remaintime / IN_MILLISECONDS <= int32(timediff))
                    continue;

                remaintime -= timediff * IN_MILLISECONDS;
            }

            // prevent wrong values of remaincharges
            if (spellInfo->ProcCharges)
            {
                // we have no control over the order of applying auras and modifiers allow auras
                // to have more charges than value in SpellInfo
                if (remaincharges <= 0/* || remaincharges > spellproto->procCharges*/)
                    remaincharges = spellInfo->ProcCharges;
            }
            else
                remaincharges = 0;

            if (Aura* aura = Aura::TryCreate(spellInfo, effmask, this, nullptr, &baseDamage[0], nullptr, caster_guid, itemGuid))
            {
                if (!aura->CanBeSaved())
                {
                    aura->Remove();
                    continue;
                }

                aura->SetLoadedState(maxduration, remaintime, remaincharges, stackcount, recalculatemask, &damage[0]);
                aura->ApplyForTargets();
                LOG_DEBUG("entities.player", "Added aura spellid {}, effectmask {}", spellInfo->Id, effmask);
            }
        } while (result->NextRow());
    }
}

void Player::_LoadGlyphAuras()
{
    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
    {
        if (uint32 glyph = GetGlyph(i))
        {
            if (GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyph))
            {
                if (GlyphSlotEntry const* glyphSlotEntry = sGlyphSlotStore.LookupEntry(GetGlyphSlot(i)))
                {
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(glyphEntry->SpellId);
                    if (glyphEntry->TypeFlags == glyphSlotEntry->TypeFlags)
                    {
                        if (!spellInfo->Stances)
                            CastSpell(this, glyphEntry->SpellId, TriggerCastFlags(TRIGGERED_FULL_MASK & ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
                        continue;
                    }
                    else
                        LOG_ERROR("entities.player", "Player {} has glyph with typeflags {} in slot with typeflags {}, removing.", m_name, glyphEntry->TypeFlags, glyphSlotEntry->TypeFlags);
                }
                else
                    LOG_ERROR("entities.player", "Player {} has not existing glyph slot entry {} on index {}", m_name, GetGlyphSlot(i), i);
            }
            else
                LOG_ERROR("entities.player", "Player {} has not existing glyph entry {} on index {}", m_name, glyph, i);

            // On any error remove glyph
            SetGlyph(i, 0, true);
        }
    }
}

void Player::LoadCorpse(PreparedQueryResult result)
{
    if (IsAlive() || HasAtLoginFlag(AT_LOGIN_RESURRECT))
        SpawnCorpseBones(false);

    if (!IsAlive())
    {
        if (result && !HasAtLoginFlag(AT_LOGIN_RESURRECT))
        {
            Field* fields = result->Fetch();
            _corpseLocation.WorldRelocate(fields[0].Get<uint16>(), fields[1].Get<float>(), fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>());
            ApplyModFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_RELEASE_TIMER, !sMapStore.LookupEntry(_corpseLocation.GetMapId())->Instanceable());
        }
        else
            ResurrectPlayer(0.5f);
    }

    RemoveAtLoginFlag(AT_LOGIN_RESURRECT);
}

void Player::_LoadInventory(PreparedQueryResult result, uint32 timeDiff)
{
    //QueryResult* result = CharacterDatabase.Query("SELECT data, text, bag, slot, item, item_template FROM character_inventory JOIN item_instance ON character_inventory.item = item_instance.guid WHERE character_inventory.guid = '{}' ORDER BY bag, slot", GetGUID().GetCounter());
    //NOTE: the "order by `bag`" is important because it makes sure
    //the bagMap is filled before items in the bags are loaded
    //NOTE2: the "order by `slot`" is needed because mainhand weapons are (wrongly?)
    //expected to be equipped before offhand items (TODO: fixme)

    if (result)
    {
        uint32 zoneId = GetZoneId();

        std::map<ObjectGuid::LowType, Bag*> bagMap;                 // fast guid lookup for bags
        std::map<ObjectGuid::LowType, Item*> invalidBagMap;         // fast guid lookup for bags
        std::list<Item*> problematicItems;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        // Prevent items from being added to the queue while loading
        m_itemUpdateQueueBlocked = true;
        do
        {
            Field* fields = result->Fetch();
            if (Item* item = _LoadItem(trans, zoneId, timeDiff, fields))
            {
                ObjectGuid::LowType bagGuid  = fields[11].Get<uint32>();
                uint8  slot     = fields[12].Get<uint8>();

                uint8 err = EQUIP_ERR_OK;
                // Item is not in bag
                if (!bagGuid)
                {
                    item->SetContainer(nullptr);
                    item->SetSlot(slot);

                    if (IsInventoryPos(INVENTORY_SLOT_BAG_0, slot))
                    {
                        ItemPosCountVec dest;
                        err = CanStoreItem(INVENTORY_SLOT_BAG_0, slot, dest, item, false);
                        if (err == EQUIP_ERR_OK)
                            item = StoreItem(dest, item, true);
                    }
                    else if (IsEquipmentPos(INVENTORY_SLOT_BAG_0, slot))
                    {
                        uint16 dest;
                        if (sScriptMgr->OnPlayerCheckItemInSlotAtLoadInventory(this, item, slot, err, dest))
                            err = CanEquipItem(slot, dest, item, false, false);
                        if (err == EQUIP_ERR_OK)
                            QuickEquipItem(dest, item);
                    }
                    else if (IsBankPos(INVENTORY_SLOT_BAG_0, slot))
                    {
                        ItemPosCountVec dest;
                        err = CanBankItem(INVENTORY_SLOT_BAG_0, slot, dest, item, false, false);
                        if (err == EQUIP_ERR_OK)
                            item = BankItem(dest, item, true);
                    }

                    // Remember bags that may contain items in them
                    if (err == EQUIP_ERR_OK)
                    {
                        if (IsBagPos(item->GetPos()))
                            if (Bag* pBag = item->ToBag())
                                bagMap[item->GetGUID().GetCounter()] = pBag;
                    }
                    else if (IsBagPos(item->GetPos()))
                        if (item->IsBag())
                            invalidBagMap[item->GetGUID().GetCounter()] = item;
                }
                else
                {
                    item->SetSlot(NULL_SLOT);
                    // Item is in the bag, find the bag
                    std::map<ObjectGuid::LowType, Bag*>::iterator itr = bagMap.find(bagGuid);
                    if (itr != bagMap.end())
                    {
                        ItemPosCountVec dest;
                        err = CanStoreItem(itr->second->GetSlot(), slot, dest, item);
                        if (err == EQUIP_ERR_OK)
                            item = StoreItem(dest, item, true);
                    }
                    else if (invalidBagMap.find(bagGuid) != invalidBagMap.end())
                    {
                        std::map<ObjectGuid::LowType, Item*>::iterator iterator = invalidBagMap.find(bagGuid);
                        if (std::find(problematicItems.begin(), problematicItems.end(), iterator->second) != problematicItems.end())
                        {
                            err = EQUIP_ERR_INT_BAG_ERROR;
                        }
                    }
                    else
                    {
                        LOG_ERROR("entities.player", "Player::_LoadInventory: player ({}, name: '{}') has item ({}, entry: {}) which doesnt have a valid bag (Bag GUID: {}, slot: {}). Possible cheat?",
                                  GetGUID().ToString(), GetName(), item->GetGUID().ToString(), item->GetEntry(), bagGuid, slot);
                        item->DeleteFromInventoryDB(trans);
                        delete item;
                        continue;
                    }
                }

                // Item's state may have changed after storing
                if (err == EQUIP_ERR_OK)
                    item->SetState(ITEM_UNCHANGED, this);
                else
                {
                    LOG_ERROR("entities.player", "Player::_LoadInventory: player ({}, name: '{}') has item ({}, entry: {}) which can't be loaded into inventory (Bag GUID: {}, slot: {}) by reason {}. Item will be sent by mail.",
                              GetGUID().ToString(), GetName(), item->GetGUID().ToString(), item->GetEntry(), bagGuid, slot, err);
                    item->DeleteFromInventoryDB(trans);
                    problematicItems.push_back(item);
                }
            }
        } while (result->NextRow());

        m_itemUpdateQueueBlocked = false;

        // Send problematic items by mail
        while (!problematicItems.empty())
        {
            std::string subject = GetSession()->GetAcoreString(LANG_NOT_EQUIPPED_ITEM);

            MailDraft draft(subject, "There were problems with equipping item(s).");
            for (uint8 i = 0; !problematicItems.empty() && i < MAX_MAIL_ITEMS; ++i)
            {
                draft.AddItem(problematicItems.front());
                problematicItems.pop_front();
            }
            draft.SendMailTo(trans, this, MailSender(this, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
        }
        CharacterDatabase.CommitTransaction(trans);
    }
    //if (IsAlive())
    _ApplyAllItemMods();
}

Item* Player::_LoadItem(CharacterDatabaseTransaction trans, uint32 zoneId, uint32 timeDiff, Field* fields)
{
    Item* item = nullptr;
    ObjectGuid::LowType itemGuid  = fields[13].Get<uint32>();
    uint32 itemEntry = fields[14].Get<uint32>();
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry))
    {
        bool remove = false;
        item = NewItemOrBag(proto);
        if (item->LoadFromDB(itemGuid, GetGUID(), fields, itemEntry))
        {
            CharacterDatabasePreparedStatement* stmt = nullptr;

            // Do not allow to have item limited to another map/zone in alive state
            if (IsAlive() && item->IsLimitedToAnotherMapOrZone(GetMapId(), zoneId))
            {
                LOG_DEBUG("entities.player.loading", "Player::_LoadInventory: player ({}, name: '{}', map: {}) has item ({}, entry: {}) limited to another map ({}). Deleting item.",
                          GetGUID().ToString(), GetName(), GetMapId(), item->GetGUID().ToString(), item->GetEntry(), zoneId);
                remove = true;
            }
                // "Conjured items disappear if you are logged out for more than 15 minutes"
            else if (timeDiff > 15 * MINUTE && proto->HasFlag(ITEM_FLAG_CONJURED))
            {
                LOG_DEBUG("entities.player.loading", "Player::_LoadInventory: player ({}, name: '{}', diff: {}) has conjured item ({}, entry: {}) with expired lifetime (15 minutes). Deleting item.",
                          GetGUID().ToString(), GetName(), timeDiff, item->GetGUID().ToString(), item->GetEntry());
                remove = true;
            }
            else if (item->IsRefundable())
            {
                if (item->GetPlayedTime() > (2 * HOUR))
                {
                    LOG_DEBUG("entities.player.loading", "Player::_LoadInventory: player ({}, name: '{}') has item ({}, entry: {}) with expired refund time ({}). Deleting refund data and removing refundable flag.",
                              GetGUID().ToString(), GetName(), item->GetGUID().ToString(), item->GetEntry(), item->GetPlayedTime());
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_REFUND_INSTANCE);
                    stmt->SetData(0, item->GetGUID().GetCounter());
                    trans->Append(stmt);

                    item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
                }
                else
                {
                    // xinef: sync query
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEM_REFUNDS);
                    stmt->SetData(0, item->GetGUID().GetCounter());
                    stmt->SetData(1, GetGUID().GetRawValue());
                    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
                    {
                        item->SetRefundRecipient((*result)[0].Get<uint32>());
                        item->SetPaidMoney((*result)[1].Get<uint32>());
                        item->SetPaidExtendedCost((*result)[2].Get<uint16>());
                        AddRefundReference(item->GetGUID());
                    }
                    else
                    {
                        LOG_DEBUG("entities.player.loading", "Player::_LoadInventory: player ({}, name: '{}') has item ({}, entry: {}) with refundable flags, but without data in item_refund_instance. Removing flag.",
                                  GetGUID().ToString(), GetName(), item->GetGUID().ToString(), item->GetEntry());
                        item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
                    }
                }
            }
            else if (item->IsBOPTradable())
            {
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEM_BOP_TRADE);
                stmt->SetData(0, item->GetGUID().GetCounter());

                if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
                {
                    AllowedLooterSet looters;
                    for (std::string_view guidStr : Acore::Tokenize((*result)[0].Get<std::string_view>(), ' ', false))
                    {
                        if (Optional<ObjectGuid::LowType> guid = Acore::StringTo<ObjectGuid::LowType>(guidStr))
                        {
                            looters.insert(ObjectGuid::Create<HighGuid::Player>(*guid));
                        }
                        else
                        {
                            LOG_WARN("entities.player.loading", "Player::_LoadInventory: invalid item_soulbound_trade_data GUID '{}' for item {}. Skipped.", guidStr, item->GetGUID().ToString());
                        }
                    }

                    if (looters.size() > 1 && item->GetTemplate()->GetMaxStackSize() == 1 && item->IsSoulBound())
                    {
                        item->SetSoulboundTradeable(looters);
                        AddTradeableItem(item);
                    }
                    else
                        item->ClearSoulboundTradeable(this);
                }
                else
                {
                    LOG_DEBUG("entities.player.loading", "Player::_LoadInventory: player ({}, name: '{}') has item ({}, entry: {}) with ITEM_FIELD_FLAG_BOP_TRADEABLE flag, but without data in item_soulbound_trade_data. Removing flag.",
                              GetGUID().ToString(), GetName(), item->GetGUID().ToString(), item->GetEntry());
                    item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_BOP_TRADEABLE);
                }
            }
            else if (proto->HolidayId)
            {
                // matches on holiday id alone, unlike IsHolidayActive(): a stage aware check here
                // would delete holiday items on login while a building stage runs
                remove = true;
                GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();
                GameEventMgr::ActiveEvents const& activeEventsList = sGameEventMgr->GetActiveEventList();
                for (GameEventMgr::ActiveEvents::const_iterator itr = activeEventsList.begin(); itr != activeEventsList.end(); ++itr)
                {
                    if (uint32(events[*itr].HolidayId) == proto->HolidayId)
                    {
                        remove = false;
                        break;
                    }
                }
            }
        }
        else
        {
            LOG_ERROR("entities.player", "Player::_LoadInventory: player ({}, name: '{}') has broken item (GUID: {}, entry: {}) in inventory. Deleting item.",
                      GetGUID().ToString(), GetName(), itemGuid, itemEntry);
            remove = true;
        }
        // Remove item from inventory if necessary
        if (remove)
        {
            Item::DeleteFromInventoryDB(trans, itemGuid);
            item->FSetState(ITEM_REMOVED);
            item->SaveToDB(trans);                           // it also deletes item object!
            item = nullptr;
        }
    }
    else
    {
        LOG_ERROR("entities.player", "Player::_LoadInventory: player ({}, name: '{}') has unknown item (entry: {}) in inventory. Deleting item.",
                  GetGUID().ToString(), GetName(), itemEntry);
        Item::DeleteFromInventoryDB(trans, itemGuid);
        Item::DeleteFromDB(trans, itemGuid);
    }
    return item;
}

Item* Player::_LoadMailedItem(ObjectGuid const& playerGuid, Player* player, uint32 mailId, Mail* mail, Field* fields)
{
    ObjectGuid::LowType itemGuid = fields[11].Get<uint32>();
    uint32 itemEntry = fields[12].Get<uint32>();

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto)
    {
        LOG_ERROR("entities.player", "Player {} ({}) has unknown item in mailed items (GUID: {}, Entry: {}) in mail ({}), deleted.",
            player ? player->GetName() : "<unknown>", playerGuid.ToString(), itemGuid, itemEntry, mailId);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_INVALID_MAIL_ITEM);
        stmt->SetData(0, itemGuid);
        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);
        return nullptr;
    }

    Item* item = NewItemOrBag(proto);

    ObjectGuid ownerGuid = fields[13].Get<uint32>() ? ObjectGuid::Create<HighGuid::Player>(fields[13].Get<uint32>()) : ObjectGuid::Empty;
    if (!item->LoadFromDB(itemGuid, ownerGuid, fields, itemEntry))
    {
        LOG_ERROR("entities.player", "Player::_LoadMailedItems: Item (GUID: {}) in mail ({}) doesn't exist, deleted from mail.", itemGuid, mailId);

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
        stmt->SetData(0, itemGuid);
        CharacterDatabase.Execute(stmt);

        item->FSetState(ITEM_REMOVED);

        CharacterDatabaseTransaction temp = CharacterDatabaseTransaction(nullptr);
        item->SaveToDB(temp);
        return nullptr;
    }

    // Rehydrate looters for BoP-tradeable mail items; only the LFG mail path writes this flag, gated on the same config.
    if (item->IsBOPTradable() && sWorld->getBoolConfig(CONFIG_SET_BOP_ITEM_TRADEABLE))
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEM_BOP_TRADE);
        stmt->SetData(0, item->GetGUID().GetCounter());

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            AllowedLooterSet looters;
            for (std::string_view guidStr : Acore::Tokenize((*result)[0].Get<std::string_view>(), ' ', false))
            {
                if (Optional<ObjectGuid::LowType> guid = Acore::StringTo<ObjectGuid::LowType>(guidStr))
                    looters.insert(ObjectGuid::Create<HighGuid::Player>(*guid));
                else
                    LOG_WARN("entities.player.loading", "Player::_LoadMailedItem: invalid item_soulbound_trade_data GUID '{}' for item {}. Skipped.", guidStr, item->GetGUID().ToString());
            }

            if (looters.size() > 1 && proto->GetMaxStackSize() == 1 && item->IsSoulBound())
                item->SetSoulboundTradeable(looters);
            else
                item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_BOP_TRADEABLE);
        }
        else
            item->RemoveFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_BOP_TRADEABLE);
    }

    if (mail)
    {
        mail->AddItem(itemGuid, itemEntry);
    }

    if (player)
    {
        player->AddMItem(item);
    }

    return item;
}

void Player::_LoadMail(PreparedQueryResult mailsResult, PreparedQueryResult mailItemsResult)
{
    time_t cur_time = GameTime::GetGameTime().count();

    m_mail.clear();

    std::unordered_map<uint32, Mail*> mailById;

    if (mailsResult)
    {
        do
        {
            Field* fields = mailsResult->Fetch();
            Mail* m = new Mail;

            m->messageID      = fields[0].Get<uint32>();
            m->messageType    = fields[1].Get<uint8>();
            m->sender         = fields[2].Get<uint32>();
            m->receiver       = fields[3].Get<uint32>();
            m->subject        = fields[4].Get<std::string>();
            m->body           = fields[5].Get<std::string>();
            m->expire_time    = time_t(fields[6].Get<uint32>());
            m->deliver_time   = time_t(fields[7].Get<uint32>());
            m->money          = fields[8].Get<uint32>();
            m->COD            = fields[9].Get<uint32>();
            m->checked        = fields[10].Get<uint8>();
            m->stationery     = fields[11].Get<uint8>();
            m->mailTemplateId = fields[12].Get<int16>();
            bool has_items    = fields[13].Get<bool>();

            if (cur_time > m->expire_time)
            {
                // Drop empty expired mail now; mail with items or money is left
                // for ReturnOrDeleteOldMails, which owns return-to-sender handling
                if (!has_items && !m->money && !m->COD)
                {
                    LOG_DEBUG("entities.player", "Player::_LoadMail: Mail ({}) has expired - deleted.", m->messageID);
                    sMailMgr->DeleteEmptyExpiredMail(m->messageID, GetGUID().GetCounter());
                }
                else
                    LOG_DEBUG("entities.player", "Player::_LoadMail: Mail ({}) has expired - ignored.", m->messageID);

                delete m;
                continue;
            }

            if (m->mailTemplateId && !sMailTemplateStore.LookupEntry(m->mailTemplateId))
            {
                LOG_ERROR("entities.player", "Player::_LoadMail: Mail ({}) has nonexistent MailTemplateId ({}), remove at load", m->messageID, m->mailTemplateId);
                m->mailTemplateId = 0;
            }

            m->state = MAIL_STATE_UNCHANGED;

            m_mail.push_back(m);
            mailById[m->messageID] = m;
        } while (mailsResult->NextRow());
    }

    if (mailItemsResult)
    {
        do
        {
            Field* fields = mailItemsResult->Fetch();
            uint32 mailId = fields[14].Get<uint32>();
            _LoadMailedItem(GetGUID(), this, mailId, mailById[mailId], fields);
        } while (mailItemsResult->NextRow());
    }

    UpdateNextMailTimeAndUnreads();
}

void Player::LoadPet()
{
    //fixme: the pet should still be loaded if the player is not in world
    // just not added to the map
    if (m_petStable && IsInWorld())
    {
        Pet* pet = new Pet(this);
        if (!pet->LoadPetFromDB(this, 0, 0, true))
            delete pet;
    }
}

void Player::_LoadQuestStatus(PreparedQueryResult result)
{
    uint16 slot = 0;

    ////                                                       0      1       2        3        4           5          6         7           8           9           10
    //QueryResult* result = CharacterDatabase.Query("SELECT quest, status, explored, timer, mobcount1, mobcount2, mobcount3, mobcount4, itemcount1, itemcount2, itemcount3,
    //                                                    11          12          13           14
    //                                                itemcount4, itemcount5, itemcount6, playercount FROM character_queststatus WHERE guid = '{}'", GetGUID().GetCounter());

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();

            uint32 quest_id = fields[0].Get<uint32>();
            // used to be new, no delete?
            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (quest)
            {
                // find or create
                QuestStatusData& questStatusData = m_QuestStatus[quest_id];

                uint8 qstatus = fields[1].Get<uint8>();
                if (qstatus < MAX_QUEST_STATUS)
                    questStatusData.Status = QuestStatus(qstatus);
                else
                {
                    questStatusData.Status = QUEST_STATUS_INCOMPLETE;
                    LOG_ERROR("entities.player", "Player {} ({}) has invalid quest {} status ({}), replaced by QUEST_STATUS_INCOMPLETE(3).",
                              GetName(), GetGUID().ToString(), quest_id, qstatus);
                }

                questStatusData.Explored = (fields[2].Get<uint8>() > 0);

                time_t quest_time = time_t(fields[3].Get<uint32>());

                if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) && !GetQuestRewardStatus(quest_id))
                {
                    AddTimedQuest(quest_id);

                    if (quest_time <= GameTime::GetGameTime().count())
                        questStatusData.Timer = 1;
                    else
                        questStatusData.Timer = uint32((quest_time - GameTime::GetGameTime().count()) * IN_MILLISECONDS);
                }
                else
                    quest_time = 0;

                for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    questStatusData.CreatureOrGOCount[i] = fields[4 + i].Get<uint16>();

                for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    questStatusData.ItemCount[i] = fields[8 + i].Get<uint16>();

                questStatusData.PlayerCount = fields[14].Get<uint16>();

                // add to quest log
                if (slot < MAX_QUEST_LOG_SIZE && questStatusData.Status != QUEST_STATUS_NONE)
                {
                    SetQuestSlot(slot, quest_id, uint32(quest_time)); // cast can't be helped

                    if (questStatusData.Status == QUEST_STATUS_COMPLETE)
                        SetQuestSlotState(slot, QUEST_STATE_COMPLETE);
                    else if (questStatusData.Status == QUEST_STATUS_FAILED)
                        SetQuestSlotState(slot, QUEST_STATE_FAIL);

                    for (uint8 idx = 0; idx < QUEST_OBJECTIVES_COUNT; ++idx)
                        if (questStatusData.CreatureOrGOCount[idx])
                            SetQuestSlotCounter(slot, idx, questStatusData.CreatureOrGOCount[idx]);

                    if (questStatusData.PlayerCount)
                        SetQuestSlotCounter(slot, QUEST_PVP_KILL_SLOT, questStatusData.PlayerCount);

                    ++slot;
                }

                LOG_DEBUG("entities.player.loading", "Quest status is ({}) for quest ({}) for player ({})", questStatusData.Status, quest_id, GetGUID().ToString());
            }
        } while (result->NextRow());
    }

    // clear quest log tail
    for (uint16 i = slot; i < MAX_QUEST_LOG_SIZE; ++i)
        SetQuestSlot(i, 0);
}

void Player::_LoadQuestStatusRewarded(PreparedQueryResult result)
{
    // SELECT quest FROM character_queststatus_rewarded WHERE guid = ?

    if (result)
    {
        m_RewardedQuests.rehash(result->GetRowCount());
        do
        {
            Field* fields = result->Fetch();

            uint32 quest_id = fields[0].Get<uint32>();
            // used to be new, no delete?
            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (quest)
            {
                // learn rewarded spell if unknown
                learnQuestRewardedSpells(quest);

                // set rewarded title if any
                if (quest->GetCharTitleId())
                {
                    if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(quest->GetCharTitleId()))
                        SetTitle(titleEntry);
                }

                if (quest->GetBonusTalents())
                    m_questRewardTalentCount += quest->GetBonusTalents();
            }

            m_RewardedQuests.insert(quest_id);
        } while (result->NextRow());
    }
}

void Player::_LoadDailyQuestStatus(PreparedQueryResult result)
{
    for (uint32 quest_daily_idx = 0; quest_daily_idx < PLAYER_MAX_DAILY_QUESTS; ++quest_daily_idx)
        SetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx, 0);

    m_DFQuests.clear();

    //QueryResult* result = CharacterDatabase.Query("SELECT quest, time FROM character_queststatus_daily WHERE guid = '{}'", GetGUID().GetCounter());

    if (result)
    {
        uint32 quest_daily_idx = 0;

        do
        {
            Field* fields = result->Fetch();
            if (Quest const* qQuest = sObjectMgr->GetQuestTemplate(fields[0].Get<uint32>()))
            {
                if (qQuest->IsDFQuest())
                {
                    m_DFQuests.insert(qQuest->GetQuestId());
                    m_lastDailyQuestTime = time_t(fields[1].Get<uint32>());
                    continue;
                }
            }

            if (quest_daily_idx >= PLAYER_MAX_DAILY_QUESTS)  // max amount with exist data in query
            {
                LOG_ERROR("entities.player", "Player ({}) have more 25 daily quest records in `charcter_queststatus_daily`", GetGUID().ToString());
                break;
            }

            uint32 quest_id = fields[0].Get<uint32>();

            // save _any_ from daily quest times (it must be after last reset anyway)
            m_lastDailyQuestTime = time_t(fields[1].Get<uint32>());

            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (!quest)
                continue;

            SetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx, quest_id);
            ++quest_daily_idx;

            LOG_DEBUG("entities.player.loading", "Daily quest ({}) cooldown for player ({})", quest_id, GetGUID().ToString());
        } while (result->NextRow());
    }

    m_DailyQuestChanged = false;
}

void Player::_LoadWeeklyQuestStatus(PreparedQueryResult result)
{
    m_weeklyquests.clear();

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 quest_id = fields[0].Get<uint32>();
            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (!quest)
                continue;

            m_weeklyquests.insert(quest_id);
            LOG_DEBUG("entities.player.loading", "Weekly quest ({}) cooldown for player ({})", quest_id, GetGUID().ToString());
        } while (result->NextRow());
    }

    m_WeeklyQuestChanged = false;
}

void Player::_LoadSeasonalQuestStatus(PreparedQueryResult result)
{
    m_seasonalquests.clear();

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 quest_id = fields[0].Get<uint32>();
            uint32 event_id = fields[1].Get<uint32>();
            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (!quest)
                continue;

            m_seasonalquests[event_id].insert(quest_id);
            LOG_DEBUG("entities.player.loading", "Seasonal quest ({}) cooldown for player ({})", quest_id, GetGUID().ToString());
        } while (result->NextRow());
    }

    m_SeasonalQuestChanged = false;
}

void Player::_LoadMonthlyQuestStatus(PreparedQueryResult result)
{
    m_monthlyquests.clear();

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 quest_id = fields[0].Get<uint32>();
            Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
            if (!quest)
                continue;

            m_monthlyquests.insert(quest_id);
            LOG_DEBUG("entities.player.loading", "Monthly quest ({}) cooldown for player ({})", quest_id, GetGUID().ToString());
        } while (result->NextRow());
    }

    m_MonthlyQuestChanged = false;
}

void Player::_LoadSpells(PreparedQueryResult result)
{
    //QueryResult* result = CharacterDatabase.Query("SELECT spell, specMask FROM character_spell WHERE guid = '{}'", GetGUID().GetCounter());

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 spellId = fields[0].Get<uint32>();
            uint8 specMask = fields[1].Get<uint8>();

            if (CheckSkillLearnedBySpell(spellId))
                addSpell(spellId, specMask, true);
            else
            {
                // Spell was never addSpell()'d, so removeSpell is often a no-op and would
                // leave an orphan character_spell row (MySQL 1062 on later re-learn/save).
                removeSpell(spellId, SPEC_MASK_ALL, false);

                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SPELL_BY_SPELL);
                stmt->SetData(0, GetGUID().GetRawValue());
                stmt->SetData(1, spellId);
                CharacterDatabase.Execute(stmt);
            }
        } while (result->NextRow());
    }
}

void Player::_LoadGroup()
{
    if (ObjectGuid groupId = sCharacterCache->GetCharacterGroupGuidByGuid(GetGUID()))
    {
        if (Group* group = sGroupMgr->GetGroupByGUID(groupId.GetCounter()))
        {
            if (group->GetMemberGroup(GetGUID()) <= MAX_RAID_SUBGROUPS)
            {
                if (group->IsLeader(GetGUID()))
                {
                    SetPlayerFlag(PLAYER_FLAGS_GROUP_LEADER);
                }

                uint8 subgroup = group->GetMemberGroup(GetGUID());
                SetGroup(group, subgroup);

                // the group leader may change the instance difficulty while the player is offline
                SetDungeonDifficulty(group->GetDungeonDifficulty());
                SetRaidDifficulty(group->GetRaidDifficulty());
            }
        }
    }

    if (!GetGroup() || !GetGroup()->IsLeader(GetGUID()))
        RemovePlayerFlag(PLAYER_FLAGS_GROUP_LEADER);
}

bool Player::_LoadHomeBind(PreparedQueryResult result)
{
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(true), getClass());
    if (!info)
    {
        LOG_ERROR("entities.player", "Player (Name {}) has incorrect race/class pair. Can't be loaded.", GetName());
        return false;
    }

    bool ok = false;
    // SELECT mapId, zoneId, posX, posY, posZ FROM character_homebind WHERE guid = ?
    if (result)
    {
        Field* fields = result->Fetch();

        m_homebindMapId = fields[0].Get<uint16>();
        m_homebindAreaId = fields[1].Get<uint16>();
        m_homebindX = fields[2].Get<float>();
        m_homebindY = fields[3].Get<float>();
        m_homebindZ = fields[4].Get<float>();

        MapEntry const* bindMapEntry = sMapStore.LookupEntry(m_homebindMapId);

        // accept saved data only for valid position (and non instanceable), and accessable
        if (MapMgr::IsValidMapCoord(m_homebindMapId, m_homebindX, m_homebindY, m_homebindZ) &&
            !bindMapEntry->Instanceable() && GetSession()->Expansion() >= bindMapEntry->Expansion())
            ok = true;
        else
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_HOMEBIND);
            stmt->SetData(0, GetGUID().GetRawValue());
            CharacterDatabase.Execute(stmt);
        }
    }

    if (!ok)
    {
        m_homebindMapId = info->mapId;
        m_homebindAreaId = info->areaId;
        m_homebindX = info->positionX;
        m_homebindY = info->positionY;
        m_homebindZ = info->positionZ;

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PLAYER_HOMEBIND);
        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, m_homebindMapId);
        stmt->SetData(2, m_homebindAreaId);
        stmt->SetData (3, m_homebindX);
        stmt->SetData (4, m_homebindY);
        stmt->SetData (5, m_homebindZ);
        CharacterDatabase.Execute(stmt);
    }

    LOG_DEBUG("entities.player", "Setting player home position - mapid: {}, areaid: {}, X: {}, Y: {}, Z: {}",
              m_homebindMapId, m_homebindAreaId, m_homebindX, m_homebindY, m_homebindZ);
    return true;
}

void Player::SaveToDB(bool create, bool logout)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    SaveToDB(trans, create, logout);

    CharacterDatabase.CommitTransaction(trans);
}

void Player::SaveToDB(CharacterDatabaseTransaction trans, bool create, bool logout)
{
    // delay auto save at any saves (manual, in code, or autosave)
    m_nextSave = sWorld->getIntConfig(CONFIG_INTERVAL_SAVE);

    //lets allow only players in world to be saved
    if (IsBeingTeleportedFar())
    {
        ScheduleDelayedOperation(DELAYED_SAVE_PLAYER);
        return;
    }

    // pussywizard: full save now, so clear partial additional saves
    m_additionalSaveTimer = 0;
    m_additionalSaveMask = 0;

    // first save/honor gain after midnight will also update the player's honor fields
    UpdateHonorFields();

    LOG_DEBUG("entities.unit", "The value of player {} at save: ", m_name);
    outDebugValues();

    if (!create)
        sScriptMgr->OnPlayerSave(this);

    _SaveCharacter(create, trans);

    if (m_mailsUpdated)                                     //save mails only when needed
        _SaveMail(trans);

    _SaveEntryPoint(trans);
    _SaveInventory(trans);
    _SaveQuestStatus(trans);
    _SaveDailyQuestStatus(trans);
    _SaveWeeklyQuestStatus(trans);
    _SaveSeasonalQuestStatus(trans);
    _SaveMonthlyQuestStatus(trans);
    _SaveTalents(trans);
    _SaveSpells(trans);
    _SaveSpellCooldowns(trans, logout);
    _SaveActions(trans);
    _SaveAuras(trans, logout);
    _SaveSkills(trans);
    m_achievementMgr->SaveToDB(trans);
    m_reputationMgr->SaveToDB(trans);
    _SaveEquipmentSets(trans);
    GetSession()->SaveTutorialsData(trans);                 // changed only while character in game
    _SaveGlyphs(trans);
    _SaveInstanceTimeRestrictions(trans);
    _SavePlayerSettings(trans);

    // check if stats should only be saved on logout
    // save stats can be out of transaction
    if (m_session->IsLoggingOut() || !sWorld->getBoolConfig(CONFIG_STATS_SAVE_ONLY_ON_LOGOUT))
        _SaveStats(trans);

    // save pet (hunter pet level and experience and all type pets health/mana).
    if (Pet* pet = GetPet())
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
}

void Player::SaveInventoryAndGoldToDB(CharacterDatabaseTransaction trans)
{
    _SaveInventory(trans);
    SaveGoldToDB(trans);
}

void Player::SaveGoldToDB(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_MONEY);
    stmt->SetData(0, GetMoney());
    stmt->SetData(1, GetGUID().GetRawValue());
    trans->Append(stmt);
}

void Player::_SaveActions(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;

    for (ActionButtonList::iterator itr = m_actionButtons.begin(); itr != m_actionButtons.end();)
    {
        switch (itr->second.uState)
        {
            case ACTIONBUTTON_NEW:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_ACTION);
                stmt->SetData(0, GetGUID().GetRawValue());
                stmt->SetData(1, m_activeSpec);
                stmt->SetData(2, itr->first);
                stmt->SetData(3, itr->second.GetAction());
                stmt->SetData(4, uint8(itr->second.GetType()));
                trans->Append(stmt);

                itr->second.uState = ACTIONBUTTON_UNCHANGED;
                ++itr;
                break;
            case ACTIONBUTTON_CHANGED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_ACTION);
                stmt->SetData(0, itr->second.GetAction());
                stmt->SetData(1, uint8(itr->second.GetType()));
                stmt->SetData(2, GetGUID().GetRawValue());
                stmt->SetData(3, itr->first);
                stmt->SetData(4, m_activeSpec);
                trans->Append(stmt);

                itr->second.uState = ACTIONBUTTON_UNCHANGED;
                ++itr;
                break;
            case ACTIONBUTTON_DELETED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_ACTION_BY_BUTTON_SPEC);
                stmt->SetData(0, GetGUID().GetRawValue());
                stmt->SetData(1, itr->first);
                stmt->SetData(2, m_activeSpec);
                trans->Append(stmt);

                m_actionButtons.erase(itr++);
                break;
            default:
                ++itr;
                break;
        }
    }
}

void Player::_SaveAuras(CharacterDatabaseTransaction trans, bool logout)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_AURA);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    for (AuraMap::const_iterator itr = m_ownedAuras.begin(); itr != m_ownedAuras.end(); ++itr)
    {
        if (!itr->second->CanBeSaved())
            continue;

        Aura* aura = itr->second;
        if (!logout && aura->GetDuration() < 60 * IN_MILLISECONDS )
            continue;

        int32 damage[MAX_SPELL_EFFECTS];
        int32 baseDamage[MAX_SPELL_EFFECTS];
        uint8 effMask = 0;
        uint8 recalculateMask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (AuraEffect const* effect = aura->GetEffect(i))
            {
                baseDamage[i] = effect->GetBaseAmount();
                damage[i] = effect->GetAmount();
                effMask |= 1 << i;
                if (effect->CanBeRecalculated())
                    recalculateMask |= 1 << i;
            }
            else
            {
                baseDamage[i] = 0;
                damage[i] = 0;
            }
        }

        uint8 index = 0;
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_AURA);
        stmt->SetData(index++, GetGUID().GetRawValue());
        stmt->SetData(index++, itr->second->GetCasterGUID().GetRawValue());
        stmt->SetData(index++, itr->second->GetCastItemGUID().GetRawValue());
        stmt->SetData(index++, itr->second->GetId());
        stmt->SetData(index++, effMask);
        stmt->SetData(index++, recalculateMask);
        stmt->SetData(index++, itr->second->GetStackAmount());
        stmt->SetData(index++, damage[0]);
        stmt->SetData(index++, damage[1]);
        stmt->SetData(index++, damage[2]);
        stmt->SetData(index++, baseDamage[0]);
        stmt->SetData(index++, baseDamage[1]);
        stmt->SetData(index++, baseDamage[2]);
        stmt->SetData(index++, itr->second->GetMaxDuration());
        stmt->SetData(index++, itr->second->GetDuration());
        stmt->SetData(index, itr->second->GetCharges());
        trans->Append(stmt);
    }
}

void Player::_SaveInventory(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;
    // force items in buyback slots to new state
    // and remove those that aren't already
    for (uint8 i = BUYBACK_SLOT_START; i < BUYBACK_SLOT_END; ++i)
    {
        Item* item = m_items[i];
        if (!item)
            continue;

        if (item->GetState() == ITEM_NEW)
        {
            // Xinef: item is removed, remove loot from storage if any
            if (item->GetTemplate()->HasFlag(ITEM_FLAG_HAS_LOOT))
                sLootItemStorage->RemoveStoredLoot(item->GetGUID());
            continue;
        }

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INVENTORY_BY_ITEM);
        stmt->SetData(0, item->GetGUID().GetCounter());
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
        stmt->SetData(0, item->GetGUID().GetCounter());
        trans->Append(stmt);
        m_items[i]->FSetState(ITEM_NEW);

        // Xinef: item is removed, remove loot from storage if any
        if (item->GetTemplate()->HasFlag(ITEM_FLAG_HAS_LOOT))
            sLootItemStorage->RemoveStoredLoot(item->GetGUID());
    }

    // Updated played time for refundable items. We don't do this in Player::Update because there's simply no need for it,
    // the client auto counts down in real time after having received the initial played time on the first
    // SMSG_ITEM_REFUND_INFO_RESPONSE packet.
    // Item::UpdatePlayedTime is only called when needed, which is in DB saves, and item refund info requests.
    RefundableItemsSet::iterator i_next;
    for (RefundableItemsSet::iterator itr = m_refundableItems.begin(); itr != m_refundableItems.end(); itr = i_next)
    {
        // use copy iterator because itr may be invalid after operations in this loop
        i_next = itr;
        ++i_next;

        Item* iPtr = GetItemByGuid((*itr));
        if (iPtr)
        {
            iPtr->UpdatePlayedTime(this);
            continue;
        }
        else
        {
            LOG_ERROR("entities.player", "Can't find item {} but is in refundable storage for player {} ! Removing.", (*itr).ToString(), GetGUID().ToString());
            m_refundableItems.erase(itr);
        }
    }

    // update enchantment durations
    for (EnchantDurationList::iterator itr = m_enchantDuration.begin(); itr != m_enchantDuration.end(); ++itr)
        itr->item->SetEnchantmentDuration(itr->slot, itr->leftduration, this);

    // if no changes
    if (m_itemUpdateQueue.empty())
        return;

    uint64 guid = GetGUID().GetRawValue();
    for (std::size_t i = 0; i < m_itemUpdateQueue.size(); ++i)
    {
        Item* item = m_itemUpdateQueue[i];
        if (!item)
            continue;

        Bag* container = item->GetContainer();
        ObjectGuid::LowType bag_guid = container ? container->GetGUID().GetCounter() : 0;

        if (item->GetState() != ITEM_REMOVED)
        {
            Item* test = GetItemByPos(item->GetBagSlot(), item->GetSlot());
            if (!test)
            {
                ObjectGuid::LowType bagTestGUID = 0;
                if (Item* test2 = GetItemByPos(INVENTORY_SLOT_BAG_0, item->GetBagSlot()))
                    bagTestGUID = test2->GetGUID().GetCounter();
                LOG_ERROR("entities.player", "Player(GUID: {} Name: {})::_SaveInventory - the bag({}) and slot({}) values for the item {} (state {}) are incorrect, the player doesn't have an item at that position!",
                          guid, GetName(), item->GetBagSlot(), item->GetSlot(), item->GetGUID().ToString(), (int32)item->GetState());
                // according to the test that was just performed nothing should be in this slot, delete
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INVENTORY_BY_BAG_SLOT);
                stmt->SetData(0, bagTestGUID);
                stmt->SetData(1, item->GetSlot());
                stmt->SetData(2, guid);
                trans->Append(stmt);

                RemoveTradeableItem(item); // pussywizard
                RemoveEnchantmentDurationsReferences(item); // pussywizard
                RemoveItemDurations(item); // pussywizard

                // also THIS item should be somewhere else, cheat attempt
                item->FSetState(ITEM_REMOVED); // we are IN updateQueue right now, can't use SetState which modifies the queue
                DeleteRefundReference(item->GetGUID());
                // don't skip, let the switch delete it
                continue;
            }
            else if (test != item)
            {
                LOG_ERROR("entities.player", "Player(GUID: {} Name: {})::_SaveInventory - the bag({}) and slot({}) values for the item ({}) are incorrect, the item ({}) is there instead!",
                          guid, GetName(), item->GetBagSlot(), item->GetSlot(), item->GetGUID().ToString(), test->GetGUID().ToString());
                // save all changes to the item...
                if (item->GetState() != ITEM_NEW) // only for existing items, no dupes
                    item->SaveToDB(trans);
                // ...but do not save position in invntory
                continue;
            }
        }

        switch (item->GetState())
        {
            case ITEM_NEW:
            case ITEM_CHANGED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_INVENTORY_ITEM);
                stmt->SetData(0, guid);
                stmt->SetData(1, bag_guid);
                stmt->SetData (2, item->GetSlot());
                stmt->SetData(3, item->GetGUID().GetCounter());
                trans->Append(stmt);
                break;
            case ITEM_REMOVED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INVENTORY_BY_ITEM);
                stmt->SetData(0, item->GetGUID().GetCounter());
                trans->Append(stmt);
            case ITEM_UNCHANGED:
                break;
        }

        item->SaveToDB(trans);                                   // item have unchanged inventory record and can be save standalone
    }
    m_itemUpdateQueue.clear();
}

void Player::_SaveMail(CharacterDatabaseTransaction trans)
{
    if (!GetMailSize() || !m_mailsUpdated)
    {
        return;
    }

    CharacterDatabasePreparedStatement* stmt = nullptr;

    for (PlayerMails::iterator itr = m_mail.begin(); itr != m_mail.end(); ++itr)
    {
        Mail* m = (*itr);
        if (m->state == MAIL_STATE_CHANGED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_MAIL);
            stmt->SetData(0, uint8(m->HasItems() ? 1 : 0));
            stmt->SetData(1, uint32(m->expire_time));
            stmt->SetData(2, uint32(m->deliver_time));
            stmt->SetData(3, m->money);
            stmt->SetData(4, m->COD);
            stmt->SetData(5, uint8(m->checked));
            stmt->SetData(6, m->messageID);

            trans->Append(stmt);

            if (!m->removedItems.empty())
            {
                for (std::vector<uint32>::iterator itr2 = m->removedItems.begin(); itr2 != m->removedItems.end(); ++itr2)
                {
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
                    stmt->SetData(0, *itr2);
                    trans->Append(stmt);
                }
                m->removedItems.clear();
            }
            m->state = MAIL_STATE_UNCHANGED;
        }
        else if (m->state == MAIL_STATE_DELETED)
        {
            if (m->HasItems())
            {
                for (MailItemInfoVec::iterator itr2 = m->items.begin(); itr2 != m->items.end(); ++itr2)
                {
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
                    stmt->SetData(0, itr2->item_guid);
                    trans->Append(stmt);
                }
            }
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
            stmt->SetData(0, m->messageID);
            trans->Append(stmt);

            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
            stmt->SetData(0, m->messageID);
            trans->Append(stmt);
        }
    }

    //deallocate deleted mails...
    for (PlayerMails::iterator itr = m_mail.begin(); itr != m_mail.end();)
    {
        if ((*itr)->state == MAIL_STATE_DELETED)
        {
            Mail* m = *itr;
            m_mail.erase(itr);
            delete m;
            itr = m_mail.begin();
        }
        else
            ++itr;
    }

    m_mailsUpdated = false;
}

void Player::_SaveQuestStatus(CharacterDatabaseTransaction trans)
{
    bool isTransaction = static_cast<bool>(trans);
    if (!isTransaction)
        trans = CharacterDatabase.BeginTransaction();

    QuestStatusSaveMap::iterator saveItr;
    QuestStatusMap::iterator statusItr;
    CharacterDatabasePreparedStatement* stmt = nullptr;

    bool keepAbandoned = !(sWorld->GetCleaningFlags() & CharacterDatabaseCleaner::CLEANING_FLAG_QUESTSTATUS);

    for (saveItr = m_QuestStatusSave.begin(); saveItr != m_QuestStatusSave.end(); ++saveItr)
    {
        if (saveItr->second)
        {
            statusItr = m_QuestStatus.find(saveItr->first);
            if (statusItr != m_QuestStatus.end() && (keepAbandoned || statusItr->second.Status != QUEST_STATUS_NONE))
            {
                uint8 index = 0;
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_CHAR_QUESTSTATUS);

                stmt->SetData(index++, GetGUID().GetRawValue());
                stmt->SetData(index++, statusItr->first);
                stmt->SetData(index++, uint8(statusItr->second.Status));
                stmt->SetData(index++, statusItr->second.Explored);
                stmt->SetData(index++, uint32(statusItr->second.Timer / IN_MILLISECONDS + GameTime::GetGameTime().count()));

                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
                    stmt->SetData(index++, statusItr->second.CreatureOrGOCount[i]);

                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
                    stmt->SetData(index++, statusItr->second.ItemCount[i]);

                stmt->SetData(index, statusItr->second.PlayerCount);
                trans->Append(stmt);
            }
        }
        else
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS_BY_QUEST);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, saveItr->first);
            trans->Append(stmt);
        }
    }

    m_QuestStatusSave.clear();

    for (saveItr = m_RewardedQuestsSave.begin(); saveItr != m_RewardedQuestsSave.end(); ++saveItr)
    {
        if (saveItr->second)
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_QUESTSTATUS_REWARDED);
        else // xinef: what the is this? quest can be removed by spelleffect if (!keepAbandoned)
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS_REWARDED_BY_QUEST);

        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, saveItr->first);
        trans->Append(stmt);
    }

    m_RewardedQuestsSave.clear();

    if (!isTransaction)
        CharacterDatabase.CommitTransaction(trans);
}

void Player::_SaveDailyQuestStatus(CharacterDatabaseTransaction trans)
{
    if (!m_DailyQuestChanged)
        return;

    m_DailyQuestChanged = false;

    // save last daily quest time for all quests: we need only mostly reset time for reset check anyway

    // we don't need transactions here.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_DAILY_CHAR);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);
    for (uint32 quest_daily_idx = 0; quest_daily_idx < PLAYER_MAX_DAILY_QUESTS; ++quest_daily_idx)
    {
        if (GetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx))
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_DAILYQUESTSTATUS);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, GetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx));
            stmt->SetData(2, uint64(m_lastDailyQuestTime));
            trans->Append(stmt);
        }
    }

    if (!m_DFQuests.empty())
    {
        for (DFQuestsDoneList::iterator itr = m_DFQuests.begin(); itr != m_DFQuests.end(); ++itr)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_DAILYQUESTSTATUS);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, (*itr));
            stmt->SetData(2, uint64(m_lastDailyQuestTime));
            trans->Append(stmt);
        }
    }
}

void Player::_SaveWeeklyQuestStatus(CharacterDatabaseTransaction trans)
{
    if (!m_WeeklyQuestChanged || m_weeklyquests.empty())
        return;

    // we don't need transactions here.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_WEEKLY_CHAR);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    for (QuestSet::const_iterator iter = m_weeklyquests.begin(); iter != m_weeklyquests.end(); ++iter)
    {
        uint32 quest_id  = *iter;

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_WEEKLYQUESTSTATUS);
        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, quest_id);
        trans->Append(stmt);
    }

    m_WeeklyQuestChanged = false;
}

void Player::_SaveSeasonalQuestStatus(CharacterDatabaseTransaction trans)
{
    if (!m_SeasonalQuestChanged)
    {
        return;
    }

    // we don't need transactions here.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_SEASONAL_CHAR);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    m_SeasonalQuestChanged = false;

    if (m_seasonalquests.empty())
    {
        return;
    }

    for (SeasonalEventQuestMap::const_iterator iter = m_seasonalquests.begin(); iter != m_seasonalquests.end(); ++iter)
    {
        uint16 eventId = iter->first;

        for (SeasonalQuestSet::const_iterator itr = iter->second.begin(); itr != iter->second.end(); ++itr)
        {
            uint32 questId = *itr;

            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_SEASONALQUESTSTATUS);
            stmt->SetArguments(GetGUID().GetRawValue(), questId, eventId);
            trans->Append(stmt);
        }
    }
}

void Player::_SaveMonthlyQuestStatus(CharacterDatabaseTransaction trans)
{
    if (!m_MonthlyQuestChanged || m_monthlyquests.empty())
        return;

    // we don't need transactions here.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_MONTHLY_CHAR);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    for (QuestSet::const_iterator iter = m_monthlyquests.begin(); iter != m_monthlyquests.end(); ++iter)
    {
        uint32 quest_id = *iter;
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER_MONTHLYQUESTSTATUS);
        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, quest_id);
        trans->Append(stmt);
    }

    m_MonthlyQuestChanged = false;
}

void Player::_SaveSkills(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;
    // we don't need transactions here.
    for (SkillStatusMap::iterator itr = mSkillStatus.begin(); itr != mSkillStatus.end();)
    {
        if (itr->second.uState == SKILL_UNCHANGED)
        {
            ++itr;
            continue;
        }

        if (itr->second.uState == SKILL_DELETED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SKILL_BY_SKILL);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, itr->first);
            trans->Append(stmt);

            mSkillStatus.erase(itr++);
            continue;
        }

        uint32 valueData = GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(itr->second.pos));
        uint16 value = SKILL_VALUE(valueData);
        uint16 max = SKILL_MAX(valueData);

        switch (itr->second.uState)
        {
            case SKILL_NEW:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_SKILLS);
                stmt->SetData(0, GetGUID().GetRawValue());
                stmt->SetData(1, uint16(itr->first));
                stmt->SetData(2, value);
                stmt->SetData(3, max);
                trans->Append(stmt);

                break;
            case SKILL_CHANGED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_CHAR_SKILLS);
                stmt->SetData(0, value);
                stmt->SetData(1, max);
                stmt->SetData(2, GetGUID().GetRawValue());
                stmt->SetData(3, uint16(itr->first));
                trans->Append(stmt);

                break;
            default:
                break;
        }
        itr->second.uState = SKILL_UNCHANGED;

        ++itr;
    }
}

void Player::_SaveSpells(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;

    for (PlayerSpellMap::iterator itr = m_spells.begin(); itr != m_spells.end();)
    {
        // xinef: skip temporary spells
        if (itr->second->State == PLAYERSPELL_TEMPORARY)
        {
            ++itr;
            continue;
        }

        // xinef: Delete statement for removed / updated spell
        if (itr->second->State == PLAYERSPELL_REMOVED || itr->second->State == PLAYERSPELL_CHANGED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SPELL_BY_SPELL);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, itr->first);
            trans->Append(stmt);
        }

        // xinef: insert statement for new / updated spell
        if (itr->second->State == PLAYERSPELL_NEW || itr->second->State == PLAYERSPELL_CHANGED)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_SPELL);
            stmt->SetData(0, GetGUID().GetRawValue());
            stmt->SetData(1, itr->first);
            stmt->SetData(2, itr->second->specMask);
            trans->Append(stmt);
        }

        if (itr->second->State == PLAYERSPELL_REMOVED)
        {
            delete itr->second;
            m_spells.erase(itr++);
        }
        else
        {
            itr->second->State = PLAYERSPELL_UNCHANGED;
            ++itr;
        }
    }
}

void Player::_SaveStats(CharacterDatabaseTransaction trans)
{
    // check if stat saving is enabled and if char level is high enough
    if (!sWorld->getIntConfig(CONFIG_MIN_LEVEL_STAT_SAVE) || GetLevel() < sWorld->getIntConfig(CONFIG_MIN_LEVEL_STAT_SAVE))
        return;

    CharacterDatabasePreparedStatement* stmt = nullptr;

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_STATS);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    uint8 index = 0;

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_STATS);
    stmt->SetData(index++, GetGUID().GetRawValue());
    stmt->SetData(index++, GetMaxHealth());

    for (uint8 i = 0; i < MAX_POWERS; ++i)
        stmt->SetData(index++, GetMaxPower(Powers(i)));

    for (uint8 i = 0; i < MAX_STATS; ++i)
        stmt->SetData(index++, GetStat(Stats(i)));

    for (int i = 0; i < MAX_SPELL_SCHOOL; ++i)
        stmt->SetData(index++, GetResistance(SpellSchools(i)));

    stmt->SetData(index++, GetFloatValue(PLAYER_BLOCK_PERCENTAGE));
    stmt->SetData(index++, GetFloatValue(PLAYER_DODGE_PERCENTAGE));
    stmt->SetData(index++, GetFloatValue(PLAYER_PARRY_PERCENTAGE));
    stmt->SetData(index++, GetFloatValue(PLAYER_CRIT_PERCENTAGE));
    stmt->SetData(index++, GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE));
    stmt->SetData(index++, GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1));
    stmt->SetData(index++, GetUInt32Value(UNIT_FIELD_ATTACK_POWER));
    stmt->SetData(index++, GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER));
    stmt->SetData(index++, GetBaseSpellPowerBonus());
    stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + static_cast<uint16>(CR_CRIT_TAKEN_SPELL)));

    trans->Append(stmt);
}
