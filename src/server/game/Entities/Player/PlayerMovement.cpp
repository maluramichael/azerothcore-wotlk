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

void Player::SendTeleportAckPacket()
{
    WorldPacket data(MSG_MOVE_TELEPORT_ACK, 41);
    data << GetPackGUID();
    data << GetSession()->GetOrderCounter(); // movement counter
    BuildMovementPacket(&data);
    SendDirectMessage(&data);
    GetSession()->IncrementOrderCounter();
}

bool Player::TeleportTo(uint32 mapid, float x, float y, float z, float orientation, uint32 options /*= 0*/, Unit* target /*= nullptr*/, bool newInstance /*= false*/)
{
    if (!MapMgr::IsValidMapCoord(mapid, x, y, z, orientation))
    {
        LOG_ERROR("entities.player", "TeleportTo: invalid map ({}) or invalid coordinates (X: {}, Y: {}, Z: {}, O: {}) given when teleporting player ({}, name: {}, map: {}, X: {}, Y: {}, Z: {}, O: {}).",
                       mapid, x, y, z, orientation, GetGUID().ToString(), GetName(), GetMapId(), GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());
        return false;
    }

    if (!GetSession()->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_DISABLE_MAP) && sDisableMgr->IsDisabledFor(DISABLE_TYPE_MAP, mapid, this))
    {
        LOG_ERROR("entities.player", "Player ({}, name: {}) tried to enter a forbidden map {}", GetGUID().ToString(), GetName(), mapid);
        SendTransferAborted(mapid, TRANSFER_ABORT_MAP_NOT_ALLOWED);
        return false;
    }

    // preparing unsummon pet if lost (we must get pet before teleportation or will not find it later)
    Pet* pet = GetPet();

    MapEntry const* mEntry = sMapStore.LookupEntry(mapid);

    // don't let enter battlegrounds without assigned battleground id (for example through areatrigger)...
    if (!InBattleground() && mEntry->IsBattlegroundOrArena())
        return false;

    // pussywizard: arena spectator, prevent teleporting from arena to instance/etc
    if (GetMapId() != mapid && IsSpectator() && mEntry->Instanceable())
    {
        SendTransferAborted(mapid, TRANSFER_ABORT_MAP_NOT_ALLOWED);
        return false;
    }

    // client without expansion support
    if (GetSession()->Expansion() < mEntry->Expansion())
    {
        LOG_DEBUG("maps", "Player {} using client without required expansion tried teleport to non accessible map {}", GetName(), mapid);

        if (GetTransport())
        {
            m_transport->RemovePassenger(this);
            m_transport = nullptr;
            m_movementInfo.transport.Reset();
            m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
            RepopAtGraveyard();                             // teleport to near graveyard if on transport, looks blizz like :)
        }

        SendTransferAborted(mapid, TRANSFER_ABORT_INSUF_EXPAN_LVL, mEntry->Expansion());

        return false;                                       // normal client can't teleport to this map...
    }
    else
        LOG_DEBUG("maps", "Player {} is being teleported to map {}", GetName(), mapid);

    // xinef: do this here in case teleport failed in above checks
    if (!(options & TELE_TO_NOT_LEAVE_TAXI) && IsInFlight())
    {
        GetMotionMaster()->MovementExpired();
        CleanupAfterTaxiFlight();
    }

    if (!(options & TELE_TO_NOT_LEAVE_VEHICLE) && m_vehicle)
        ExitVehicle();

    // reset movement flags at teleport, because player will continue move with these flags after teleport
    SetUnitMovementFlags(GetUnitMovementFlags() & MOVEMENTFLAG_MASK_HAS_PLAYER_STATUS_OPCODE);
    DisableSpline();

    // Xinef: Remove all movement imparing effects auras, skip small teleport like blink
    if (mapid != GetMapId() || GetDistance2d(x, y) > 100)
    {
        RemoveAurasByType(SPELL_AURA_MOD_STUN);
        RemoveAurasByType(SPELL_AURA_MOD_FEAR);
        RemoveAurasByType(SPELL_AURA_MOD_CONFUSE);
        RemoveAurasByType(SPELL_AURA_MOD_ROOT);
        // remove auras that should be removed when being teleported
        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED);
    }

    if (m_transport)
    {
        if (options & TELE_TO_NOT_LEAVE_TRANSPORT)
            AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        else
        {
            m_transport->RemovePassenger(this);
            m_transport = nullptr;
            m_movementInfo.transport.Reset();
            m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        }
    }

    // The player was ported to another map and loses the duel immediately.
    // We have to perform this check before the teleport, otherwise the
    // ObjectAccessor won't find the flag.
    if (duel && GetMapId() != mapid && GetMap()->GetGameObject(GetGuidValue(PLAYER_DUEL_ARBITER)))
        DuelComplete(DUEL_FLED);

    if (!sScriptMgr->OnPlayerBeforeTeleport(this, mapid, x, y, z, orientation, options, target))
        return false;

    if (GetMapId() == mapid && !newInstance)
    {
        //lets reset far teleport flag if it wasn't reset during chained teleports
        SetSemaphoreTeleportFar(0);

        SetHasDelayedTeleport(false); // pussywizard: current teleport cancels stored one
        //if teleport spell is casted in Unit::Update() func
        //then we need to delay it until update process will be finished
        if (MustDelayTeleport())
        {
            SetHasDelayedTeleport(true);
            SetSemaphoreTeleportNear(GameTime::GetGameTime().count());
            //lets save teleport destination for player
            teleportStore_dest = WorldLocation(mapid, x, y, z, orientation);
            teleportStore_options = options;
            return true;
        }

        if (options & TELE_TO_WITH_PET)
            UnsummonPetTemporaryIfAny();

        if (!(options & TELE_TO_NOT_UNSUMMON_PET))
        {
            //same map, only remove pet if out of range for new position
            if (pet && !pet->IsWithinDist3d(x, y, z, GetMap()->GetVisibilityRange()))
                UnsummonPetTemporaryIfAny();
        }

        if (!(options & TELE_TO_NOT_LEAVE_COMBAT))
            CombatStop();

        // this will be used instead of the current location in SaveToDB
        teleportStore_dest = WorldLocation(mapid, x, y, z, orientation);
        SetFallInformation(GameTime::GetGameTime().count(), z);

        // code for finish transfer called in WorldSession::HandleMovementOpcodes()
        // at client packet MSG_MOVE_TELEPORT_ACK
        SetSemaphoreTeleportNear(GameTime::GetGameTime().count());
        // near teleport, triggering send MSG_MOVE_TELEPORT_ACK from client at landing
        if (!GetSession()->PlayerLogout())
        {
            SetCanTeleport(true);
            Position oldPos = GetPosition();
            Relocate(x, y, z, orientation);
            SendTeleportAckPacket();
            SendTeleportPacket(oldPos); // this automatically relocates to oldPos in order to broadcast the packet in the right place
        }
    }
    else
    {
        if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TELEPORT) && GetMapId() == MAP_EBON_HOLD && !IsGameMaster() && !HasSpell(50977))
        {
            SendTransferAborted(mapid, TRANSFER_ABORT_UNIQUE_MESSAGE, 1);
            return false;
        }

        // far teleport to another map
        Map* oldmap = IsInWorld() ? GetMap() : nullptr;
        // check if we can enter before stopping combat / removing pet / totems / interrupting spells

        // Check enter rights before map getting to avoid creating instance copy for player
        // this check not dependent from map instance copy and same for all instance copies of selected map
        if (!(options & TELE_TO_GM_MODE) && sMapMgr->PlayerCannotEnter(mapid, this, false))
            return false;

        // if PlayerCannotEnter -> CanEnter: checked above
        {
            //lets reset near teleport flag if it wasn't reset during chained teleports
            SetSemaphoreTeleportNear(0);

            SetHasDelayedTeleport(false); // pussywizard: current teleport cancels stored one
            //if teleport spell is casted in Unit::Update() func
            //then we need to delay it until update process will be finished
            if (MustDelayTeleport())
            {
                SetHasDelayedTeleport(true);
                SetSemaphoreTeleportFar(GameTime::GetGameTime().count());
                //lets save teleport destination for player
                teleportStore_dest = WorldLocation(mapid, x, y, z, orientation);
                teleportStore_options = options;
                return true;
            }

            SetSelection(ObjectGuid::Empty);

            CombatStop();

            // remove pet on map change
            if (pet)
                UnsummonPetTemporaryIfAny();

            // remove all dyn objects
            RemoveAllDynObjects();

            // stop spellcasting
            // not attempt interrupt teleportation spell at caster teleport
            if (!(options & TELE_TO_SPELL))
                if (IsNonMeleeSpellCast(true))
                    InterruptNonMeleeSpells(true);

            //remove auras before removing from map...
            RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_CHANGE_MAP | AURA_INTERRUPT_FLAG_MOVE | AURA_INTERRUPT_FLAG_TURNING);

            SetMapChangeOrderCounter();

            if (!GetSession()->PlayerLogout())
            {
                // send transfer packets
                WorldPacket data(SMSG_TRANSFER_PENDING, 4 + 4 + 4);
                data << uint32(mapid);
                if (m_transport)
                    data << m_transport->GetEntry() << GetMapId();

                SendDirectMessage(&data);
            }

            // remove from old map now
            if (oldmap)
                oldmap->RemovePlayerFromMap(this, false);

            teleportStore_dest = WorldLocation(mapid, x, y, z, orientation);
            SetFallInformation(GameTime::GetGameTime().count(), z);
            // if the player is saved before worldportack (at logout for example)
            // this will be used instead of the current location in SaveToDB

            if (!GetSession()->PlayerLogout())
            {
                SetCanTeleport(true);
                WorldPacket data(SMSG_NEW_WORLD, 4 + 4 + 4 + 4 + 4);
                data << uint32(mapid);
                if (m_transport)
                    data << m_movementInfo.transport.pos.PositionXYZOStream();
                else
                    data << teleportStore_dest.PositionXYZOStream();

                SendDirectMessage(&data);
                SendSavedInstances();
            }

            // move packet sent by client always after far teleport
            // code for finish transfer to new map called in WorldSession::HandleMoveWorldportAckOpcode at client packet
            SetSemaphoreTeleportFar(GameTime::GetGameTime().count());
        }
    }
    return true;
}

bool Player::TeleportToEntryPoint()
{
    ScheduleDelayedOperation(DELAYED_BG_MOUNT_RESTORE);
    ScheduleDelayedOperation(DELAYED_BG_TAXI_RESTORE);
    ScheduleDelayedOperation(DELAYED_BG_GROUP_RESTORE);

    WorldLocation loc = m_entryPointData.joinPos;
    m_entryPointData.joinPos.m_mapId = MAPID_INVALID;

    if (loc.m_mapId == MAPID_INVALID)
    {
        return TeleportTo(m_homebindMapId, m_homebindX, m_homebindY, m_homebindZ, GetOrientation());
    }

    return TeleportTo(loc);
}

bool Player::IsFalling() const
{
    // Xinef: Added !IsInFlight check
    return GetPositionZ() < m_lastFallZ && !IsInFlight();
}

void Player::SaveRecallPosition()
{
    m_recallMap = GetMapId();
    m_recallX = GetPositionX();
    m_recallY = GetPositionY();
    m_recallZ = GetPositionZ();
    m_recallO = GetOrientation();
}

bool Player::ActivateTaxiPathTo(std::vector<uint32> const& nodes, Creature* npc /*= nullptr*/, uint32 spellid /*= 1*/)
{
    if (nodes.size() < 2)
        return false;

    // not let cheating with start flight in time of logout process || while in combat || has type state: stunned || has type state: root
    if (GetSession()->IsLoggingOut() || IsInCombat() || HasUnitState(UNIT_STATE_STUNNED) || HasUnitState(UNIT_STATE_ROOT))
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
        return false;
    }

    if (HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return false;

    // taximaster case
    if (npc)
    {
        // not let cheating with start flight mounted
        if (IsMounted())
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERALREADYMOUNTED);
            return false;
        }

        if (IsInDisallowedMountForm())
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERSHAPESHIFTED);
            return false;
        }

        // not let cheating with start flight in time of logout process || if casting not finished || while in combat || if not use Spell's with EffectSendTaxi
        if (IsNonMeleeSpellCast(false))
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
            return false;
        }
    }
    // cast case or scripted call case
    else
    {
        RemoveAurasByType(SPELL_AURA_MOUNTED);

        if (IsInDisallowedMountForm())
            RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

        if (Spell* spell = GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->m_spellInfo->Id != spellid)
                InterruptSpell(CURRENT_GENERIC_SPELL, false);

        InterruptSpell(CURRENT_AUTOREPEAT_SPELL, false);

        if (Spell* spell = GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (spell->m_spellInfo->Id != spellid)
                InterruptSpell(CURRENT_CHANNELED_SPELL, true);
    }

    uint32 sourcenode = nodes[0];

    // starting node too far away (cheat?)
    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(sourcenode);
    if (!node)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXINOSUCHPATH);
        return false;
    }

    // Prepare to flight start now

    // stop combat at start taxi flight if any
    CombatStop();

    StopCastingCharm();
    StopCastingBindSight();
    ExitVehicle();

    // stop trade (client cancel trade at taxi map open but cheating tools can be used for reopen it)
    TradeCancel(true);

    // clean not finished taxi path if any
    m_taxi.ClearTaxiDestinations();

    // 0 element current node
    m_taxi.AddTaxiDestination(sourcenode);

    // fill destinations path tail
    uint32 sourcepath = 0;
    uint32 totalcost = 0;
    uint32 firstcost = 0;

    uint32 prevnode = sourcenode;
    uint32 lastnode = 0;

    for (uint32 i = 1; i < nodes.size(); ++i)
    {
        uint32 path, cost;

        lastnode = nodes[i];
        sObjectMgr->GetTaxiPath(prevnode, lastnode, path, cost);

        if (!path)
        {
            m_taxi.ClearTaxiDestinations();
            return false;
        }

        totalcost += cost;
        if (i == 1)
            firstcost = cost;

        if (prevnode == sourcenode)
            sourcepath = path;

        m_taxi.AddTaxiDestination(lastnode);

        prevnode = lastnode;
    }

    // get mount model (in case non taximaster (npc == nullptr) allow more wide lookup)
    //
    // Hack-Fix for Alliance not being able to use Acherus taxi. There is
    // only one mount ID for both sides. Probably not good to use 315 in case DBC nodes
    // change but I couldn't find a suitable alternative. OK to use class because only DK
    // can use this taxi.
    uint32 mount_display_id = sObjectMgr->GetTaxiMountDisplayId(sourcenode, GetTeamId(true), npc == nullptr || (sourcenode == 315 && IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TAXI)));

    // in spell case allow 0 model
    if ((mount_display_id == 0 && spellid == 0) || sourcepath == 0)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXIUNSPECIFIEDSERVERERROR);
        m_taxi.ClearTaxiDestinations();
        return false;
    }

    uint32 money = GetMoney();

    if (npc)
    {
        float discount = GetReputationPriceDiscount(npc);
        totalcost = uint32(ceil(totalcost * discount));
        firstcost = uint32(ceil(firstcost * discount));
        m_taxi.SetFlightMasterFactionTemplateId(npc->GetFaction());
    }
    else
    {
        m_taxi.SetFlightMasterFactionTemplateId(0);
    }

    if (money < totalcost)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXINOTENOUGHMONEY);
        m_taxi.ClearTaxiDestinations();
        return false;
    }

    //Checks and preparations done, DO FLIGHT
    UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_FLIGHT_PATHS_TAKEN, 1);

    // prevent stealth flight
    //RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TALK);

    // Xinef: dont use instant flight paths if spellid is present (custom calls use spellid = 1)
    if ((sWorld->getIntConfig(CONFIG_INSTANT_TAXI) == 1 || (sWorld->getIntConfig(CONFIG_INSTANT_TAXI) == 2 && m_isInstantFlightOn)) && !spellid)
    {
        TaxiNodesEntry const* lastPathNode = sTaxiNodesStore.LookupEntry(nodes[nodes.size() - 1]);
        m_taxi.ClearTaxiDestinations();
        ModifyMoney(-(int32)totalcost);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GOLD_SPENT_FOR_TRAVELLING, totalcost);
        TeleportTo(lastPathNode->map_id, lastPathNode->x, lastPathNode->y, lastPathNode->z, GetOrientation());
        return false;
    }
    else
    {
        m_flightSpellActivated = spellid;
        ModifyMoney(-(int32)firstcost);
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GOLD_SPENT_FOR_TRAVELLING, firstcost);
        GetSession()->SendActivateTaxiReply(ERR_TAXIOK);
        GetSession()->SendDoFlight(mount_display_id, sourcepath);
    }
    return true;
}

bool Player::ActivateTaxiPathTo(uint32 taxi_path_id, uint32 spellid /*= 1*/)
{
    TaxiPathEntry const* entry = sTaxiPathStore.LookupEntry(taxi_path_id);
    if (!entry)
        return false;

    std::vector<uint32> nodes;

    nodes.resize(2);
    nodes[0] = entry->from;
    nodes[1] = entry->to;

    return ActivateTaxiPathTo(nodes, nullptr, spellid);
}

void Player::CleanupAfterTaxiFlight()
{
    // For spells that trigger flying paths remove them at arrival
    if (m_flightSpellActivated)
    {
        this->RemoveAurasDueToSpell(m_flightSpellActivated);
        m_flightSpellActivated = 0;
    }
    m_taxi.ClearTaxiDestinations();        // not destinations, clear source node
    Dismount();
    RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);
}

void Player::ContinueTaxiFlight()
{
    uint32 sourceNode = m_taxi.GetTaxiSource();
    if (!sourceNode)
        return;

    LOG_DEBUG("entities.unit", "WORLD: Restart character {} taxi flight", GetGUID().ToString());

    uint32 mountDisplayId = sObjectMgr->GetTaxiMountDisplayId(sourceNode, GetTeamId(true), true);
    if (!mountDisplayId)
        return;

    uint32 path = m_taxi.GetCurrentTaxiPath();

    // search appropriate start path node
    uint32 startNode = 0;

    TaxiPathNodeList const& nodeList = sTaxiPathNodesByPath[path];

    // Use triangle inequality to find the segment the player is on.
    // When distPrev + distNext < distNodes, the player projects between
    // the two nodes of the segment. Resume from the end node (i).
    float distPrev;
    float distNext =
        (nodeList[0]->x - GetPositionX()) * (nodeList[0]->x - GetPositionX()) +
        (nodeList[0]->y - GetPositionY()) * (nodeList[0]->y - GetPositionY()) +
        (nodeList[0]->z - GetPositionZ()) * (nodeList[0]->z - GetPositionZ());

    for (uint32 i = 1; i < nodeList.size(); ++i)
    {
        TaxiPathNodeEntry const* node = nodeList[i];
        TaxiPathNodeEntry const* prevNode = nodeList[i - 1];

        // skip nodes at another map
        if (node->mapid != GetMapId())
            continue;

        distPrev = distNext;
        distNext =
            (node->x - GetPositionX()) * (node->x - GetPositionX()) +
            (node->y - GetPositionY()) * (node->y - GetPositionY()) +
            (node->z - GetPositionZ()) * (node->z - GetPositionZ());

        float distNodes =
            (node->x - prevNode->x) * (node->x - prevNode->x) +
            (node->y - prevNode->y) * (node->y - prevNode->y) +
            (node->z - prevNode->z) * (node->z - prevNode->z);

        if (distPrev + distNext < distNodes)
        {
            startNode = i;
            break;
        }
    }

    // xinef: no proper node was found
    if (startNode == 0)
    {
        m_taxi.ClearTaxiDestinations();
        return;
    }

    if (IsInDisallowedMountForm())
    {
        RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    }

    if (IsMounted())
    {
        RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    SetCanTeleport(true);

    GetSession()->SendDoFlight(mountDisplayId, path, startNode);
}

WorldLocation Player::GetStartPosition() const
{
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(true), getClass());
    uint32 mapId = info->mapId;
    if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT) && HasSpell(50977))
        return WorldLocation(0, 2352.0f, -5709.0f, 154.5f, 0.0f);
    return WorldLocation(mapId, info->positionX, info->positionY, info->positionZ, 0);
}

void Player::SetMover(Unit* target)
{
    if (this != target && target->m_movedByPlayer && target->m_movedByPlayer != target && target->m_movedByPlayer != this)
    {
        LOG_INFO("misc", "Player::SetMover (A1) - {}, {}, {}, {}, {}, {}, {}, {}", GetGUID().ToString(), GetMapId(), GetInstanceId(), FindMap()->GetId(), IsInWorld() ? 1 : 0, IsDuringRemoveFromWorld() ? 1 : 0, IsBeingTeleported() ? 1 : 0, isBeingLoaded() ? 1 : 0);
        LOG_INFO("misc", "Player::SetMover (A2) - {}, {}, {}, {}, {}, {}, {}, {}", target->GetGUID().ToString(), target->GetMapId(), target->GetInstanceId(), target->FindMap()->GetId(), target->IsInWorld() ? 1 : 0, target->IsDuringRemoveFromWorld() ? 1 : 0, (target->ToPlayer() && target->ToPlayer()->IsBeingTeleported() ? 1 : 0), target->isBeingLoaded() ? 1 : 0);
        LOG_INFO("misc", "Player::SetMover (A3) - {}, {}, {}, {}, {}, {}, {}, {}", target->m_movedByPlayer->GetGUID().ToString(), target->m_movedByPlayer->GetMapId(), target->m_movedByPlayer->GetInstanceId(), target->m_movedByPlayer->FindMap()->GetId(), target->m_movedByPlayer->IsInWorld() ? 1 : 0, target->m_movedByPlayer->IsDuringRemoveFromWorld() ? 1 : 0, target->m_movedByPlayer->ToPlayer()->IsBeingTeleported() ? 1 : 0, target->m_movedByPlayer->isBeingLoaded() ? 1 : 0);
    }
    if (this != target && (!target->IsInWorld() || target->IsDuringRemoveFromWorld() || GetMapId() != target->GetMapId() || GetInstanceId() != target->GetInstanceId()))
    {
        LOG_INFO("misc", "Player::SetMover (B1) - {}, {}, {}, {}, {}, {}, {}, {}", GetGUID().ToString(), GetMapId(), GetInstanceId(), FindMap()->GetId(), IsInWorld() ? 1 : 0, IsDuringRemoveFromWorld() ? 1 : 0, IsBeingTeleported() ? 1 : 0, isBeingLoaded() ? 1 : 0);
        LOG_INFO("misc", "Player::SetMover (B2) - {}, {}, {}, {}, {}, {}, {}, {}", target->GetGUID().ToString(), target->GetMapId(), target->GetInstanceId(), target->FindMap()->GetId(), target->IsInWorld() ? 1 : 0, target->IsDuringRemoveFromWorld() ? 1 : 0, (target->ToPlayer() && target->ToPlayer()->IsBeingTeleported() ? 1 : 0), target->isBeingLoaded() ? 1 : 0);
    }
    m_mover->m_movedByPlayer = nullptr;
    if (m_mover->IsCreature())
        m_mover->GetMotionMaster()->Initialize();

    m_mover = target;
    m_mover->m_movedByPlayer = this;
    if (m_mover->IsCreature())
        m_mover->GetMotionMaster()->Initialize();
}

static constexpr float   FALL_DMG_EQU_SLOPE         = 0.018f;

static constexpr float   FALL_DMG_EQU_INTERCEPT     = -0.2426f;

static constexpr float   MIN_FALL_DMG_DIST          = 13.48f;

static constexpr uint32  SPELL_GUST_OF_WIND         = 43621;

static constexpr uint32  SPELL_DIVINE_PROTECTION    = 498;

void Player::HandleFall(MovementInfo const& movementInfo)
{
    // calculate total z distance of the fall
    float z_diff = m_lastFallZ - movementInfo.pos.GetPositionZ();

    //Players with low fall distance, Feather Fall or physical immunity (charges used) are ignored
    if (z_diff >= MIN_FALL_DMG_DIST && !isDead() && !IsGameMaster() && !GetCommandStatus(CHEAT_GOD) &&
            !HasHoverAura() && !HasFeatherFallAura() &&
            !HasFlyAura())
    {
        //Safe fall, fall height reduction
        int32 safe_fall = GetTotalAuraModifier(SPELL_AURA_SAFE_FALL);

        float damageperc = FALL_DMG_EQU_SLOPE * (z_diff - safe_fall) + FALL_DMG_EQU_INTERCEPT;
        uint32 original_health = GetHealth(), final_damage = 0;

        if (damageperc > 0 && !IsImmunedToDamageOrSchool(SPELL_SCHOOL_MASK_NORMAL))
        {
            uint32 damage = (uint32)(damageperc * GetMaxHealth() * sWorld->getRate(RATE_DAMAGE_FALL));

            if (damage > 0)
            {
                //Prevent fall damage from being more than the player maximum health
                if (damage > GetMaxHealth())
                    damage = GetMaxHealth();

                if (HasAura(SPELL_GUST_OF_WIND))
                    damage = GetMaxHealth() / 2;

                if (HasAura(SPELL_DIVINE_PROTECTION))
                    damage /= 2;

                final_damage = EnvironmentalDamage(DAMAGE_FALL, damage);
            }

            //Z given by moveinfo, LastZ, FallTime, WaterZ, MapZ, Damage, Safefall reduction
            LOG_DEBUG("entities.player", "FALLDAMAGE mZ={} z={} fallTime={} damage={} SF={}", movementInfo.pos.GetPositionZ(), GetPositionZ(), movementInfo.fallTime, damage, safe_fall);
        }

        // recheck alive, might have died of EnvironmentalDamage, avoid cases when player die in fact like Spirit of Redemption case
        if (IsAlive() && final_damage < original_health)
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_FALL_WITHOUT_DYING, uint32(z_diff * 100));
    }
}
