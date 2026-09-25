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

bool Player::IsGroupVisibleFor(Player const* p) const
{
    switch (sWorld->getIntConfig(CONFIG_GROUP_VISIBILITY))
    {
        default:
            return IsInSameGroupWith(p);
        case 1:
            return IsInSameRaidWith(p);
        case 2:
            return GetTeamId() == p->GetTeamId();
    }
}

bool Player::IsInSameGroupWith(Player const* p) const
{
    return p == this || (GetGroup() &&
                         GetGroup() == p->GetGroup() &&
                         GetGroup()->SameSubGroup(this, p));
}

void Player::UninviteFromGroup()
{
    Group* group = GetGroupInvite();
    if (!group)
        return;

    group->RemoveInvite(this);

    if (group->IsCreated())
    {
        if (group->GetMembersCount() <= 1)                       // group has just 1 member => disband
        {
            group->Disband(true);
            group = nullptr; // gets deleted in disband
        }
    }
    else
    {
        if (group->GetInviteeCount() <= 1)
        {
            group->RemoveAllInvites();
            delete group;
            group = nullptr;
        }
    }
}

void Player::RemoveFromGroup(Group* group, ObjectGuid guid, RemoveMethod method /* = GROUP_REMOVEMETHOD_DEFAULT*/, ObjectGuid kicker /* = ObjectGuid::Empty */, char const* reason /* = nullptr */)
{
    if (group)
    {
        group->RemoveMember(guid, method, kicker, reason);
        group = nullptr;
    }
}

bool Player::CanJoinConstantChannelInZone(ChatChannelsEntry const* channel, AreaTableEntry const* zone)
{
    // Player can join LFG anywhere
    if (channel->flags & CHANNEL_DBC_FLAG_LFG && sWorld->getBoolConfig(CONFIG_LFG_LOCATION_ALL))
        return true;

    if (channel->flags & CHANNEL_DBC_FLAG_ZONE_DEP && zone->flags & AREA_FLAG_ARENA_INSTANCE)
        return false;

    if ((channel->flags & CHANNEL_DBC_FLAG_CITY_ONLY) && (!(zone->flags & AREA_FLAG_SLAVE_CAPITAL)))
        return false;

    if ((channel->flags & CHANNEL_DBC_FLAG_GUILD_REQ) && GetGuildId())
        return false;

    return true;
}

void Player::JoinedChannel(Channel* c)
{
    m_channels.push_back(c);
}

void Player::LeftChannel(Channel* c)
{
    m_channels.remove(c);
}

void Player::CleanupChannels()
{
    while (!m_channels.empty())
    {
        Channel* ch = *m_channels.begin();
        m_channels.erase(m_channels.begin());               // remove from player's channel list
        ch->LeaveChannel(this, false);                     // not send to client, not remove from player's channel list
    }
}

void Player::ClearChannelWatch()
{
    for (JoinedChannelsList::iterator itr = m_channels.begin(); itr != m_channels.end(); ++itr)
        (*itr)->RemoveWatching(this);
}

void Player::UpdateBaseModGroup(BaseModGroup modGroup)
{
    if (!CanModifyStats())
        return;

    switch (modGroup)
    {
        case CRIT_PERCENTAGE:
            UpdateCritPercentage(BASE_ATTACK);
            break;
        case RANGED_CRIT_PERCENTAGE:
            UpdateCritPercentage(RANGED_ATTACK);
            break;
        case OFFHAND_CRIT_PERCENTAGE:
            UpdateCritPercentage(OFF_ATTACK);
            break;
        case SHIELD_BLOCK_VALUE:
            UpdateShieldBlockValue();
            break;
        default:
            break;
    }
}

void Player::Whisper(std::string_view text, Language language, Player* target, bool /*= false*/)
{
    ASSERT(target);

    bool isAddonMessage = language == LANG_ADDON;

    if (!isAddonMessage)                                    // if not addon data
        language = LANG_UNIVERSAL;                          // whispers should always be readable

    std::string _text(text);

    if (!sScriptMgr->OnPlayerCanUseChat(this, CHAT_MSG_WHISPER, language, _text, target))
        return;

    bool isFiltered = sWorld->getBoolConfig(CONFIG_CHAT_FILTER_WHISPER) && IsChatFiltered(text);

    WorldPacket data;
    if (!isFiltered || isAddonMessage)
    {
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, language, this, this, _text);
        target->SendDirectMessage(&data);
    }

    // rest stuff shouldn't happen in case of addon message
    if (isAddonMessage)
        return;

    ChatMsg msgType = CHAT_MSG_WHISPER_INFORM;
    if (isFiltered)
        msgType = CHAT_MSG_FILTERED;

    ChatHandler::BuildChatPacket(data, msgType, Language(language), target, target, _text);
    SendDirectMessage(&data);

    if (!isAcceptWhispers() && !IsGameMaster() && !target->IsGameMaster())
    {
        SetAcceptWhispers(true);
        ChatHandler(GetSession()).SendSysMessage(LANG_COMMAND_WHISPERON);
    }

    // announce afk or dnd message
    if (target->isAFK())
    {
        ChatHandler(GetSession()).PSendSysMessage(LANG_PLAYER_AFK, target->GetName(), target->autoReplyMsg);
    }
    else if (target->isDND())
    {
        ChatHandler(GetSession()).PSendSysMessage(LANG_PLAYER_DND, target->GetName(), target->autoReplyMsg);
    }
}

void Player::Whisper(uint32 textId, Player* target, bool isBossWhisper)
{
    if (!target)
        return;

    BroadcastText const* bct = sObjectMgr->GetBroadcastText(textId);
    if (!bct)
    {
        LOG_ERROR("entities.unit", "Player::Whisper: `broadcast_text` was not {} found", textId);
        return;
    }

    LocaleConstant locale = target->GetSession()->GetSessionDbLocaleIndex();
    WorldPacket data;
    if (isBossWhisper)
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_BOSS_WHISPER, LANG_UNIVERSAL, this, target, bct->GetText(locale, getGender()), 0, "", locale);
    else
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_UNIVERSAL, this, target, bct->GetText(locale, getGender()), 0, "", locale);
    target->SendDirectMessage(&data);
}

void Player::SetGroup(Group* group, int8 subgroup)
{
    if (!group)
        m_group.unlink();
    else
    {
        // never use SetGroup without a subgroup unless you specify nullptr for group
        ASSERT(subgroup >= 0);
        m_group.link(group, this);
        m_group.setSubGroup((uint8)subgroup);
    }

    UpdateObjectVisibility(false);
}

void Player::SendUpdateToOutOfRangeGroupMembers()
{
    if (m_groupUpdateMask == GROUP_UPDATE_FLAG_NONE)
        return;
    if (Group* group = GetGroup())
        group->UpdatePlayerOutOfRange(this);

    m_groupUpdateMask = GROUP_UPDATE_FLAG_NONE;
    m_auraRaidUpdateMask = 0;
    if (Pet* pet = GetPet())
        pet->ResetAuraUpdateMaskForRaid();
}

bool Player::GetsRecruitAFriendBonus(bool forXP)
{
    bool recruitAFriend = false;
    if (GetLevel() <= sWorld->getIntConfig(CONFIG_MAX_RECRUIT_A_FRIEND_BONUS_PLAYER_LEVEL) || !forXP)
    {
        if (Group* group = this->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* player = itr->GetSource();
                if (!player || !player->IsInMap(this))
                    continue;

                if (!player->IsAtRecruitAFriendDistance(this))
                    continue;                               // member (alive or dead) or his corpse at req. distance

                if (forXP)
                {
                    // level must be allowed to get RaF bonus
                    if (player->GetLevel() > sWorld->getIntConfig(CONFIG_MAX_RECRUIT_A_FRIEND_BONUS_PLAYER_LEVEL))
                        continue;

                    // level difference must be small enough to get RaF bonus, UNLESS we are lower level
                    if (player->GetLevel() < GetLevel())
                        if (uint8(GetLevel() - player->GetLevel()) > sWorld->getIntConfig(CONFIG_MAX_RECRUIT_A_FRIEND_BONUS_PLAYER_LEVEL_DIFFERENCE))
                            continue;
                }

                bool ARecruitedB = (player->GetSession()->GetRecruiterId() == GetSession()->GetAccountId());
                bool BRecruitedA = (GetSession()->GetRecruiterId() == player->GetSession()->GetAccountId());
                if (ARecruitedB || BRecruitedA)
                {
                    recruitAFriend = true;
                    break;
                }
            }
        }
    }
    return recruitAFriend;
}

void Player::RewardPlayerAndGroupAtKill(Unit* victim, bool isBattleGround)
{
    KillRewarder(this, victim, isBattleGround).Reward();
}

void Player::RewardPlayerAndGroupAtEvent(uint32 creature_id, WorldObject* pRewardSource)
{
    if (!pRewardSource)
        return;

    ObjectGuid creature_guid;
    if (pRewardSource->IsCreature() && pRewardSource->GetEntry() == creature_id)
        creature_guid = pRewardSource->GetGUID();

    // prepare data for near group iteration
    if (Group* group = GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* player = itr->GetSource();
            if (!player)
                continue;

            if (!player->IsAtGroupRewardDistance(pRewardSource))
                continue;                               // member (alive or dead) or his corpse at req. distance

            // quest objectives updated only for alive group member or dead but with not released body
            if (player->IsAlive() || !player->GetCorpse())
                player->KilledMonsterCredit(creature_id, creature_guid);
        }
    }
    else                                                    // if (!group)
        KilledMonsterCredit(creature_id, creature_guid);
}

bool Player::IsAtGroupRewardDistance(WorldObject const* pRewardSource) const
{
    WorldObject const* player = GetCorpse();
    if (!player || IsAlive())
    {
        player = this;
    }

    if (!pRewardSource || !player->IsInMap(pRewardSource))
    {
        return false;
    }

    if (pRewardSource->GetMap()->IsDungeon())
    {
        return true;
    }

    return pRewardSource->GetDistance(player) <= sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE);
}

bool Player::IsAtRecruitAFriendDistance(WorldObject const* pOther) const
{
    if (!pOther)
        return false;
    WorldObject const* player = GetCorpse();
    if (!player || IsAlive())
        player = this;

    if (player->GetMapId() != pOther->GetMapId() || player->GetInstanceId() != pOther->GetInstanceId())
        return false;

    return pOther->GetDistance(player) <= sWorld->getFloatConfig(CONFIG_MAX_RECRUIT_A_FRIEND_DISTANCE);
}

Player* Player::GetNextRandomRaidMember(float radius)
{
    Group* group = GetGroup();
    if (!group)
        return nullptr;

    std::vector<Player*> nearMembers;
    nearMembers.reserve(group->GetMembersCount());

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* Target = itr->GetSource();

        // IsHostileTo check duel and controlled by enemy
        if (Target && Target != this && IsWithinDistInMap(Target, radius) &&
                !Target->HasInvisibilityAura() && !IsHostileTo(Target))
            nearMembers.push_back(Target);
    }

    if (nearMembers.empty())
        return nullptr;

    uint32 randTarget = urand(0, nearMembers.size() - 1);
    return nearMembers[randTarget];
}

PartyResult Player::CanUninviteFromGroup(ObjectGuid targetPlayerGUID) const
{
    Group const* grp = GetGroup();
    if (!grp)
        return ERR_NOT_IN_GROUP;

    if (grp->isLFGGroup(true))
    {
        ObjectGuid gguid = grp->GetGUID();
        if (!sLFGMgr->GetKicksLeft(gguid))
            return ERR_PARTY_LFG_BOOT_LIMIT;

        lfg::LfgState state = sLFGMgr->GetState(gguid);
        if (state == lfg::LFG_STATE_BOOT)
            return ERR_PARTY_LFG_BOOT_IN_PROGRESS;

        if (grp->GetMembersCount() <= lfg::LFG_GROUP_KICK_VOTES_NEEDED)
            return ERR_PARTY_LFG_BOOT_TOO_FEW_PLAYERS;

        if (state == lfg::LFG_STATE_FINISHED_DUNGEON)
            return ERR_PARTY_LFG_BOOT_DUNGEON_COMPLETE;

        if (grp->isRollLootActive() && ObjectAccessor::FindConnectedPlayer(targetPlayerGUID))
            return ERR_PARTY_LFG_BOOT_LOOT_ROLLS;

        /// @todo: Should also be sent when anyone has recently left combat, with an aprox ~5 seconds timer.
        for (GroupReference const* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (itr->GetSource() && itr->GetSource()->IsInMap(this) && itr->GetSource()->IsInCombat())
                return ERR_PARTY_LFG_BOOT_IN_COMBAT;

        if (Player* target = ObjectAccessor::FindConnectedPlayer(targetPlayerGUID))
        {
            if (Aura* dungeonCooldownAura = target->GetAura(lfg::LFG_SPELL_DUNGEON_COOLDOWN))
            {
                int32 elapsedTime = dungeonCooldownAura->GetMaxDuration() - dungeonCooldownAura->GetDuration();
                if (static_cast<int32>(sWorld->getIntConfig(CONFIG_LFG_KICK_PREVENTION_TIMER)) > elapsedTime)
                {
                    return ERR_PARTY_LFG_BOOT_NOT_ELIGIBLE_S;
                }
            }
        }

        /* Missing support for these types
            return ERR_PARTY_LFG_BOOT_COOLDOWN_S;
        */
    }
    else
    {
        if (!grp->IsLeader(GetGUID()) && !grp->IsAssistant(GetGUID()))
            return ERR_NOT_LEADER;

        if (InBattleground())
            return ERR_INVITE_RESTRICTED;

        // BF raids are owned by the Battlefield system; leaders/assistants must not
        // be able to kick members (would drop their team assignment mid-battle).
        if (grp->isBFGroup())
            return ERR_NOT_LEADER;
    }

    return ERR_PARTY_RESULT_OK;
}

void Player::SetOriginalGroup(Group* group, int8 subgroup)
{
    if (!group)
        m_originalGroup.unlink();
    else
    {
        // never use SetOriginalGroup without a subgroup unless you specify nullptr for group
        ASSERT(subgroup >= 0);
        m_originalGroup.link(group, this);
        m_originalGroup.setSubGroup((uint8)subgroup);
    }
}

bool Player::IsInWhisperWhiteList(ObjectGuid guid)
{
    for (auto const& itr : WhisperList)
    {
        if (itr == guid)
        {
            return true;
        }
    }

    return false;
}

Guild* Player::GetGuild() const
{
    uint32 guildId = GetGuildId();
    return guildId ? sGuildMgr->GetGuildById(guildId) : nullptr;
}
