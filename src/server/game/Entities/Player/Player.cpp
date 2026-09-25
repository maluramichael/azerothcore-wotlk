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

/// @todo: this import is not necessary for compilation and marked as unused by the IDE
//  however, for some reasons removing it would cause a damn linking issue
//  there is probably some underlying problem with imports which should properly addressed
//  see: https://github.com/azerothcore/azerothcore-wotlk/issues/9766
#include "GridNotifiersImpl.h"

enum CharacterFlags
{
    CHARACTER_FLAG_NONE                 = 0x00000000,
    CHARACTER_FLAG_UNK1                 = 0x00000001,
    CHARACTER_FLAG_RESTING              = 0x00000002,
    CHARACTER_LOCKED_FOR_TRANSFER       = 0x00000004,
    CHARACTER_FLAG_UNK4                 = 0x00000008,
    CHARACTER_FLAG_UNK5                 = 0x00000010,
    CHARACTER_FLAG_UNK6                 = 0x00000020,
    CHARACTER_FLAG_UNK7                 = 0x00000040,
    CHARACTER_FLAG_UNK8                 = 0x00000080,
    CHARACTER_FLAG_UNK9                 = 0x00000100,
    CHARACTER_FLAG_UNK10                = 0x00000200,
    CHARACTER_FLAG_HIDE_HELM            = 0x00000400,
    CHARACTER_FLAG_HIDE_CLOAK           = 0x00000800,
    CHARACTER_FLAG_UNK13                = 0x00001000,
    CHARACTER_FLAG_GHOST                = 0x00002000,
    CHARACTER_FLAG_RENAME               = 0x00004000,
    CHARACTER_FLAG_UNK16                = 0x00008000,
    CHARACTER_FLAG_UNK17                = 0x00010000,
    CHARACTER_FLAG_UNK18                = 0x00020000,
    CHARACTER_FLAG_UNK19                = 0x00040000,
    CHARACTER_FLAG_UNK20                = 0x00080000,
    CHARACTER_FLAG_UNK21                = 0x00100000,
    CHARACTER_FLAG_UNK22                = 0x00200000,
    CHARACTER_FLAG_UNK23                = 0x00400000,
    CHARACTER_FLAG_UNK24                = 0x00800000,
    CHARACTER_FLAG_LOCKED_BY_BILLING    = 0x01000000,
    CHARACTER_FLAG_DECLINED             = 0x02000000,
    CHARACTER_FLAG_UNK27                = 0x04000000,
    CHARACTER_FLAG_UNK28                = 0x08000000,
    CHARACTER_FLAG_UNK29                = 0x10000000,
    CHARACTER_FLAG_UNK30                = 0x20000000,
    CHARACTER_FLAG_UNK31                = 0x40000000,
    CHARACTER_FLAG_UNK32                = 0x80000000
};

enum CharacterCustomizeFlags
{
    CHAR_CUSTOMIZE_FLAG_NONE            = 0x00000000,
    CHAR_CUSTOMIZE_FLAG_CUSTOMIZE       = 0x00000001,       // name, gender, etc...
    CHAR_CUSTOMIZE_FLAG_FACTION         = 0x00010000,       // name, gender, faction, etc...
    CHAR_CUSTOMIZE_FLAG_RACE            = 0x00100000        // name, gender, race, etc...
};

static uint32 copseReclaimDelay[MAX_DEATH_COUNT] = { 30, 60, 120 };

// we can disable this warning for this since it only
// causes undefined behavior when passed to the base class constructor
#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif
Player::Player(WorldSession* session): Unit(), m_mover(this), _cinematicMgr(*this)
{
#ifdef _MSC_VER
#pragma warning(default:4355)
#endif

    m_objectType |= TYPEMASK_PLAYER;
    m_objectTypeId = TYPEID_PLAYER;

    m_valuesCount = PLAYER_END;

    m_session = session;

    m_ingametime = 0;

    m_ExtraFlags = 0;

    m_spellModTakingSpell = nullptr;
    //m_pad = 0;

    // players always accept
    if (!GetSession()->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS))
        SetAcceptWhispers(true);

    m_usedTalentCount = 0;
    m_questRewardTalentCount = 0;
    m_extraBonusTalentCount = 0;

    m_regenTimer = 0;
    m_regenTimerCount = 0;
    m_foodEmoteTimerCount = 0;
    m_weaponChangeTimer = 0;

    m_zoneUpdateId = uint32(-1);
    m_zoneUpdateTimer = 0;

    m_nextSave = sWorld->getIntConfig(CONFIG_INTERVAL_SAVE);

    m_areaUpdateId = 0;
    m_team = TEAM_NEUTRAL;

    m_needZoneUpdate = false;

    m_additionalSaveTimer = 0;
    m_additionalSaveMask = 0;
    m_hostileReferenceCheckTimer = 15000;

    clearResurrectRequestData();

    memset(m_items, 0, sizeof(Item*)*PLAYER_SLOTS_COUNT);

    m_social = nullptr;

    // group is initialized in the reference constructor
    SetGroupInvite(nullptr);
    m_groupUpdateMask = 0;
    m_auraRaidUpdateMask = 0;
    m_bPassOnGroupLoot = false;

    m_GuildIdInvited = 0;
    m_ArenaTeamIdInvited = 0;

    m_atLoginFlags = AT_LOGIN_NONE;

    mSemaphoreTeleport_Near = 0;
    mSemaphoreTeleport_Far = 0;

    m_DelayedOperations = 0;
    m_bMustDelayTeleport = false;
    m_bHasDelayedTeleport = false;
    teleportStore_options = 0;
    m_canTeleport = false;
    m_canKnockback = false;

    m_trade = nullptr;

    m_cinematic = 0;

    PlayerTalkClass = new PlayerMenu(GetSession());
    m_currentBuybackSlot = BUYBACK_SLOT_START;

    m_DailyQuestChanged = false;
    m_lastDailyQuestTime = 0;

    for (uint8 i = 0; i < MAX_TIMERS; i++)
        m_MirrorTimer[i] = DISABLED_MIRROR_TIMER;

    m_MirrorTimerFlags = UNDERWATER_NONE;
    m_MirrorTimerFlagsLast = UNDERWATER_NONE;
    m_isInWater = false;
    m_drunkTimer = 0;
    m_deathTimer = 0;
    m_deathExpireTime = 0;

    m_flightSpellActivated = 0;

    m_swingErrorMsg = 0;

    for (uint8 j = 0; j < PLAYER_MAX_BATTLEGROUND_QUEUES; ++j)
    {
        _BgBattlegroundQueueID[j].bgQueueTypeId = BATTLEGROUND_QUEUE_NONE;
        _BgBattlegroundQueueID[j].invitedToInstance = 0;
    }

    m_logintime = GameTime::GetGameTime().count();
    m_Last_tick = m_logintime;
    m_Played_time[PLAYED_TIME_TOTAL] = 0;
    m_Played_time[PLAYED_TIME_LEVEL] = 0;
    m_WeaponProficiency = 0;
    m_ArmorProficiency = 0;
    m_canParry = false;
    m_canBlock = false;
    m_canTitanGrip = false;
    m_ammoDPS = 0.0f;

    m_temporaryUnsummonedPetNumber = 0;
    //cache for UNIT_CREATED_BY_SPELL to allow
    //returning reagents for temporarily removed pets
    //when dying/logging out
    m_oldpetspell = 0;
    m_lastpetnumber = 0;

    ////////////////////Rest System/////////////////////
    _restTime = 0;
    _innTriggerId = 0;
    _restBonus = 0;
    _restFlagMask = 0;
    ////////////////////Rest System/////////////////////

    m_mailsUpdated = false;
    unReadMails = 0;
    m_nextMailDelivereTime = time_t(0);

    m_resetTalentsCost = 0;
    m_resetTalentsTime = 0;
    m_itemUpdateQueueBlocked = false;

    for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
        m_forced_speed_changes[i] = 0;

    /////////////////// Instance System /////////////////////

    m_HomebindTimer = 0;
    m_InstanceValid = true;
    m_dungeonDifficulty = DUNGEON_DIFFICULTY_NORMAL;
    m_raidDifficulty = RAID_DIFFICULTY_10MAN_NORMAL;
    m_raidMapDifficulty = RAID_DIFFICULTY_10MAN_NORMAL;

    m_lastPotionId = 0;

    m_activeSpec = 0;
    m_specsCount = 1;

    for (uint8 i = 0; i < MAX_TALENT_SPECS; ++i)
    {
        for (uint8 g = 0; g < MAX_GLYPH_SLOT_INDEX; ++g)
            m_Glyphs[i][g] = 0;
    }

    for (uint8 i = 0; i < BASEMOD_END; ++i)
    {
        m_auraBaseFlatMod[i] = 0.0f;
        m_auraBasePctMod[i] = 1.0f;
    }

    for (uint8 i = 0; i < MAX_COMBAT_RATING; i++)
        m_baseRatingValue[i] = 0;

    m_baseSpellPower = 0;
    m_baseSpellDamage = 0;
    m_baseSpellHealing = 0;
    m_baseFeralAP = 0;
    m_baseManaRegen = 0;
    m_baseHealthRegen = 0;
    m_spellPenetrationItemMod = 0;

    // Honor System
    m_lastHonorUpdateTime = GameTime::GetGameTime().count();

    m_IsBGRandomWinner = false;

    // Player summoning
    m_summon_expire = 0;
    m_summon_mapid = 0;
    m_summon_x = 0.0f;
    m_summon_y = 0.0f;
    m_summon_z = 0.0f;
    m_summon_asSpectator = false;

    //m_mover = this;
    m_movedByPlayer.Initialize(this);
    m_seer = this;

    m_recallMap = 0;
    m_recallX = 0;
    m_recallY = 0;
    m_recallZ = 0;
    m_recallO = 0;

    m_homebindMapId = 0;
    m_homebindAreaId = 0;
    m_homebindX = 0;
    m_homebindY = 0;
    m_homebindZ = 0;

    m_contestedPvPTimer = 0;

    m_declinedname = nullptr;

    m_isActive = true;

    m_runes = nullptr;

    m_lastFallTime = 0;
    m_lastFallZ = 0;

    m_grantableLevels = 0;

    m_ControlledByPlayer = true;

    sWorldSessionMgr->IncreasePlayerCount();

    m_ChampioningFaction = 0;

    for (uint8 i = 0; i < MAX_POWERS; ++i)
        m_powerFraction[i] = 0;

    isDebugAreaTriggers = false;

    m_WeeklyQuestChanged = false;

    m_MonthlyQuestChanged = false;

    m_SeasonalQuestChanged = false;

    SetPendingBind(0, 0);

    _activeCheats = CHEAT_NONE;

    m_creationTime = 0s;

    m_achievementMgr = new AchievementMgr(this);
    m_reputationMgr = new ReputationMgr(this);

    m_NeedToSaveGlyphs = false;
    m_MountBlockId = 0;
    m_realDodge = 0.0f;
    m_realParry = 0.0f;
    m_pendingSpectatorForBG = 0;
    m_pendingSpectatorInviteInstanceId = 0;

    m_charmUpdateTimer = 0;

    for( int i = 0; i < NUM_CAI_SPELLS; ++i )
        m_charmAISpells[i] = 0;

    m_applyResilience = true;

    m_isInstantFlightOn = true;

    _wasOutdoor = true;

    GetObjectVisibilityContainer().InitForPlayer();

    sScriptMgr->OnConstructPlayer(this);

    _expectingChangeTransport = false;
    _pendingFlightChangeCounter = 0;
    _mapChangeOrderCounter = 0;
}

Player::~Player()
{
    sScriptMgr->OnDestructPlayer(this);

    // it must be unloaded already in PlayerLogout and accessed only for loggined player
    //m_social = nullptr;

    // Note: buy back item already deleted from DB when player was saved
    for (uint8 i = 0; i < PLAYER_SLOTS_COUNT; ++i)
        delete m_items[i];

    for (PlayerSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
        delete itr->second;

    for (PlayerTalentMap::const_iterator itr = m_talents.begin(); itr != m_talents.end(); ++itr)
        delete itr->second;

    //all mailed items should be deleted, also all mail should be deallocated
    for (PlayerMails::iterator itr = m_mail.begin(); itr != m_mail.end(); ++itr)
    {
        delete *itr;
    }

    for (ItemMap::iterator iter = mMitems.begin(); iter != mMitems.end(); ++iter)
        delete iter->second;                                //if item is duplicated... then server may crash ... but that item should be deallocated

    delete PlayerTalkClass;

    for (std::size_t x = 0; x < ItemSetEff.size(); x++)
        delete ItemSetEff[x];

    delete m_declinedname;
    delete m_runes;
    delete m_achievementMgr;
    delete m_reputationMgr;

    sWorldSessionMgr->DecreasePlayerCount();

    if (!m_isInSharedVisionOf.empty())
    {
        do
        {
            Unit* u = *(m_isInSharedVisionOf.begin());
            u->RemovePlayerFromVision(this);
        } while (!m_isInSharedVisionOf.empty());
    }
}

void Player::CleanupsBeforeDelete(bool finalCleanup)
{
    TradeCancel(false);
    DuelComplete(DUEL_INTERRUPTED);

    Unit::CleanupsBeforeDelete(finalCleanup);
}

bool Player::Create(ObjectGuid::LowType guidlow, CharacterCreateInfo* createInfo)
{
    // FIXME: outfitId not used in player creating
    /// @todo: need more checks against packet modifications
    // should check that skin, face, hair* are valid via DBC per race/class
    // also do it in Player::BuildEnumData, Player::LoadFromDB

    Object::_Create(guidlow, 0, HighGuid::Player);

    m_name = createInfo->Name;

    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(createInfo->Race, createInfo->Class);
    if (!info)
    {
        LOG_ERROR("entities.player", "Player::Create: Possible hacking-attempt: Account {} tried creating a character named '{}' with an invalid race/class pair ({}/{}) - refusing to do so.",
                       GetSession()->GetAccountId(), m_name, createInfo->Race, createInfo->Class);
        return false;
    }

    for (uint8 i = 0; i < PLAYER_SLOTS_COUNT; i++)
        m_items[i] = nullptr;

    Relocate(info->positionX, info->positionY, info->positionZ, info->orientation);

    ChrClassesEntry const* cEntry = sChrClassesStore.LookupEntry(createInfo->Class);
    if (!cEntry)
    {
        LOG_ERROR("entities.player", "Player::Create: Possible hacking-attempt: Account {} tried creating a character named '{}' with an invalid character class ({}) - refusing to do so (wrong DBC-files?)",
                       GetSession()->GetAccountId(), m_name, createInfo->Class);
        return false;
    }

    SetMap(sMapMgr->CreateMap(info->mapId, this));

    uint8 powertype = cEntry->powerType;

    SetObjectScale(1.0f);

    m_realRace = createInfo->Race; // set real race flag
    m_race = createInfo->Race; // set real race flag

    SetFactionForRace(createInfo->Race);

    if (!IsValidGender(createInfo->Gender))
    {
        LOG_ERROR("entities.player", "Player::Create: Possible hacking-attempt: Account {} tried creating a character named '{}' with an invalid gender ({}) - refusing to do so",
                       GetSession()->GetAccountId(), m_name, createInfo->Gender);
        return false;
    }

    uint32 RaceClassGender = (createInfo->Race) | (createInfo->Class << 8) | (createInfo->Gender << 16);

    SetUInt32Value(UNIT_FIELD_BYTES_0, (RaceClassGender | (powertype << 24)));
    InitDisplayIds();
    if (sWorld->getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_PVP || sWorld->getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_RPPVP)
    {
        SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_PVP);
        SetUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED);
    }
    SetUnitFlag2(UNIT_FLAG2_REGENERATE_POWER);
    SetFloatValue(UNIT_MOD_CAST_SPEED, 1.0f);               // fix cast time showed in spell tooltip on client
    SetFloatValue(UNIT_FIELD_HOVERHEIGHT, 1.0f);            // default for players in 3.0.3

    // -1 is default value
    SetInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX, uint32(-1));

    SetUInt32Value(PLAYER_BYTES, (createInfo->Skin | (createInfo->Face << 8) | (createInfo->HairStyle << 16) | (createInfo->HairColor << 24)));
    SetUInt32Value(PLAYER_BYTES_2, (createInfo->FacialHair |
                                    (0x00 << 8) |
                                    (0x00 << 16) |
                                    (((GetSession()->IsARecruiter() || GetSession()->GetRecruiterId() != 0) ? REST_STATE_RAF_LINKED : REST_STATE_NOT_RAF_LINKED) << 24)));
    SetByteValue(PLAYER_BYTES_3, 0, createInfo->Gender);
    SetByteValue(PLAYER_BYTES_3, 3, 0);                     // BattlefieldArenaFaction (0 or 1)

    SetUInt32Value(PLAYER_GUILDID, 0);
    SetUInt32Value(PLAYER_GUILDRANK, 0);
    SetUInt32Value(PLAYER_GUILD_TIMESTAMP, 0);

    for (int i = 0; i < KNOWN_TITLES_SIZE; ++i)
        SetUInt64Value(PLAYER__FIELD_KNOWN_TITLES + i, 0);  // 0=disabled
    SetUInt32Value(PLAYER_CHOSEN_TITLE, 0);

    SetUInt32Value(PLAYER_FIELD_KILLS, 0);
    SetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS, 0);
    SetUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION, 0);
    SetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION, 0);

    // set starting level
    uint32 start_level = !IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT)
                         ? sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL)
                         : sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);

    if (GetSession()->HasPermission(rbac::RBAC_PERM_USE_START_GM_LEVEL))
    {
        uint32 gm_level = sWorld->getIntConfig(CONFIG_START_GM_LEVEL);
        if (gm_level > start_level)
            start_level = gm_level;
    }

    SetUInt32Value(UNIT_FIELD_LEVEL, start_level);

    InitRunes();

    SetUInt32Value(PLAYER_FIELD_COINAGE, !IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT)
                                         ? sWorld->getIntConfig(CONFIG_START_PLAYER_MONEY)
                                         : sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_MONEY));
    SetHonorPoints(sWorld->getIntConfig(CONFIG_START_HONOR_POINTS));
    SetArenaPoints(sWorld->getIntConfig(CONFIG_START_ARENA_POINTS));

    // Played time
    m_Last_tick = GameTime::GetGameTime().count();
    m_Played_time[PLAYED_TIME_TOTAL] = 0;
    m_Played_time[PLAYED_TIME_LEVEL] = 0;

    // base stats and related field values
    InitStatsForLevel();
    InitTaxiNodesForLevel();
    InitGlyphsForLevel();
    InitTalentForLevel();
    InitPrimaryProfessions();                               // to max set before any spell added

    // apply original stats mods before spell loading or item equipment that call before equip _RemoveStatsMods()
    if (HasActivePowerType(POWER_MANA))
    {
        UpdateMaxPower(POWER_MANA);                         // Update max Mana (for add bonus from intellect)
        SetPower(POWER_MANA, GetMaxPower(POWER_MANA));
    }

    if (HasActivePowerType(POWER_RUNIC_POWER))
    {
        SetPower(POWER_RUNE, 8);
        SetMaxPower(POWER_RUNE, 8);
        SetPower(POWER_RUNIC_POWER, 0);
        SetMaxPower(POWER_RUNIC_POWER, 1000);
    }

    // original spells
    LearnDefaultSkills();
    LearnCustomSpells();

    // original action bar
    for (PlayerCreateInfoActions::const_iterator action_itr = info->action.begin(); action_itr != info->action.end(); ++action_itr)
        addActionButton(action_itr->button, action_itr->action, action_itr->type);

    // original items
    if (CharStartOutfitEntry const* oEntry = GetCharStartOutfitEntry(createInfo->Race, createInfo->Class, createInfo->Gender))
    {
        for (int j = 0; j < MAX_OUTFIT_ITEMS; ++j)
        {
            if (oEntry->ItemId[j] <= 0)
                continue;

            uint32 itemId = oEntry->ItemId[j];

            // just skip, reported in ObjectMgr::LoadItemTemplates
            ItemTemplate const* iProto = sObjectMgr->GetItemTemplate(itemId);
            if (!iProto)
                continue;

            // BuyCount by default
            uint32 count = iProto->BuyCount;

            // special amount for food/drink
            if (iProto->Class == ITEM_CLASS_CONSUMABLE && iProto->SubClass == ITEM_SUBCLASS_FOOD)
            {
                switch (iProto->Spells[0].SpellCategory)
                {
                    case SPELL_CATEGORY_FOOD:                                // food
                        count = IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT) ? 10 : 4;
                        break;
                    case SPELL_CATEGORY_DRINK:                                // drink
                        count = 2;
                        break;
                }
                if (iProto->GetMaxStackSize() < count)
                    count = iProto->GetMaxStackSize();
            }
            StoreNewItemInBestSlots(itemId, count);
        }
    }

    for (PlayerCreateInfoItems::const_iterator item_id_itr = info->item.begin(); item_id_itr != info->item.end(); ++item_id_itr)
        StoreNewItemInBestSlots(item_id_itr->item_id, item_id_itr->item_amount);

    // Collector's Edition starter gift voucher
    if (GetSession()->HasAccountFlag(ACCOUNT_FLAG_COLLECTOR))
    {
        uint32 voucherId = 0;
        if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT))
            voucherId = 39713; // Ebon Hold Gift Voucher
        else
        {
            switch (createInfo->Race)
            {
                case RACE_HUMAN:
                    voucherId = 14646; break; // Goldshire Gift Voucher
                case RACE_DWARF:
                case RACE_GNOME:
                    voucherId = 14647; break; // Kharanos Gift Voucher
                case RACE_NIGHTELF:
                    voucherId = 14648; break; // Dolanaar Gift Voucher
                case RACE_ORC:
                case RACE_TROLL:
                    voucherId = 14649; break; // Razor Hill Gift Voucher
                case RACE_TAUREN:
                    voucherId = 14650; break; // Bloodhoof Village Gift Voucher
                case RACE_UNDEAD_PLAYER:
                    voucherId = 14651; break; // Brill Gift Voucher
                case RACE_BLOODELF:
                    voucherId = 20938; break; // Falconwing Square Gift Voucher
                case RACE_DRAENEI:
                    voucherId = 22888; break; // Azure Watch Gift Voucher
                default:
                    break;
            }
        }

        // Only guard against mail delivery if the item was actually stored, otherwise
        // a full bag would suppress the mail fallback and lose the voucher permanently.
        if (voucherId && StoreNewItemInBestSlots(voucherId, 1))
        {
            // Prevent characters from receiving duplicate vouchers through the mail system.
            // voucherId doubles as the mail template id (item == templateID in the SQL data).
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_MAIL_SERVER_CHARACTER);
            stmt->SetData(0, guidlow);
            stmt->SetData(1, voucherId);
            CharacterDatabase.Execute(stmt);
        }
    }

    // bags and main-hand weapon must equipped at this moment
    // now second pass for not equipped (offhand weapon/shield if it attempt equipped before main-hand weapon)
    // or ammo not equipped in special bag
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; i++)
    {
        if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            uint16 eDest;
            // equip offhand weapon/shield if it attempt equipped before main-hand weapon
            InventoryResult msg = CanEquipItem(NULL_SLOT, eDest, pItem, false);
            if (msg == EQUIP_ERR_OK)
            {
                RemoveItem(INVENTORY_SLOT_BAG_0, i, true);
                EquipItem(eDest, pItem, true);
            }
            // move other items to more appropriate slots (ammo not equipped in special bag)
            else
            {
                ItemPosCountVec sDest;
                msg = CanStoreItem(NULL_BAG, NULL_SLOT, sDest, pItem, false);
                if (msg == EQUIP_ERR_OK)
                {
                    RemoveItem(INVENTORY_SLOT_BAG_0, i, true);
                    pItem = StoreItem(sDest, pItem, true);
                }

                // if  this is ammo then use it
                msg = CanUseAmmo(pItem->GetEntry());
                if (msg == EQUIP_ERR_OK)
                    SetAmmo(pItem->GetEntry());
            }
        }
    }
    // all item positions resolved

    // ensure player starts with full health
    UpdateAllStats();
    SetFullHealth();

    CheckAllAchievementCriteria();

    return true;
}



void Player::SendMirrorTimer(MirrorTimerType Type, uint32 MaxValue, uint32 CurrentValue, int32 Regen)
{
    if (int(MaxValue) == DISABLED_MIRROR_TIMER)
    {
        if (int(CurrentValue) != DISABLED_MIRROR_TIMER)
            StopMirrorTimer(Type);
        return;
    }
    SendDirectMessage(WorldPackets::Misc::StartMirrorTimer(Type, CurrentValue, MaxValue, Regen, 0, 0).Write());
}

void Player::StopMirrorTimer(MirrorTimerType Type)
{
    m_MirrorTimer[Type] = DISABLED_MIRROR_TIMER;
    SendDirectMessage(WorldPackets::Misc::StopMirrorTimer(Type).Write());
}





int32 Player::getMaxTimer(MirrorTimerType timer)
{
    switch (timer)
    {
        case FATIGUE_TIMER:
            return MINUTE * IN_MILLISECONDS;
        case BREATH_TIMER:
            {
                if (!IsAlive() || HasWaterBreathingAura() || GetSession()->GetSecurity() >= AccountTypes(sWorld->getIntConfig(CONFIG_DISABLE_BREATHING)))
                    return DISABLED_MIRROR_TIMER;
                int32 UnderWaterTime = sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER);
                UnderWaterTime *= GetTotalAuraMultiplier(SPELL_AURA_MOD_WATER_BREATHING);
                return UnderWaterTime;
            }
        case FIRE_TIMER:
            {
                if (!IsAlive())
                    return DISABLED_MIRROR_TIMER;
                return 2020;
            }
        default:
            return 0;
    }
}

void Player::HandleDrowning(uint32 time_diff)
{
    if (!m_MirrorTimerFlags)
        return;

    // In water
    if (m_MirrorTimerFlags & UNDERWATER_INWATER)
    {
        // Breath timer not activated - activate it
        if (m_MirrorTimer[BREATH_TIMER] == DISABLED_MIRROR_TIMER)
        {
            m_MirrorTimer[BREATH_TIMER] = getMaxTimer(BREATH_TIMER);
            SendMirrorTimer(BREATH_TIMER, m_MirrorTimer[BREATH_TIMER], m_MirrorTimer[BREATH_TIMER], -1);
        }
        else                                                              // If activated - do tick
        {
            m_MirrorTimer[BREATH_TIMER] -= time_diff;
            // Timer limit - need deal damage
            if (m_MirrorTimer[BREATH_TIMER] < 0)
            {
                m_MirrorTimer[BREATH_TIMER] += 1 * IN_MILLISECONDS;
                // Calculate and deal damage
                /// @todo: Check this formula
                uint32 damage = GetMaxHealth() / 5 + urand(0, GetLevel() - 1);
                EnvironmentalDamage(DAMAGE_DROWNING, damage);
            }
            else if (!(m_MirrorTimerFlagsLast & UNDERWATER_INWATER))      // Update time in client if need
                SendMirrorTimer(BREATH_TIMER, getMaxTimer(BREATH_TIMER), m_MirrorTimer[BREATH_TIMER], -1);
        }
    }
    else if (m_MirrorTimer[BREATH_TIMER] != DISABLED_MIRROR_TIMER)        // Regen timer
    {
        int32 UnderWaterTime = getMaxTimer(BREATH_TIMER);
        // Need breath regen
        m_MirrorTimer[BREATH_TIMER] += 10 * time_diff;
        if (m_MirrorTimer[BREATH_TIMER] >= UnderWaterTime || !IsAlive())
            StopMirrorTimer(BREATH_TIMER);
        else if (m_MirrorTimerFlagsLast & UNDERWATER_INWATER)
            SendMirrorTimer(BREATH_TIMER, UnderWaterTime, m_MirrorTimer[BREATH_TIMER], 10);
    }

    // In dark water
    if (m_MirrorTimerFlags & UNDERWATER_INDARKWATER)
    {
        // Fatigue timer not activated - activate it
        if (m_MirrorTimer[FATIGUE_TIMER] == DISABLED_MIRROR_TIMER)
        {
            m_MirrorTimer[FATIGUE_TIMER] = getMaxTimer(FATIGUE_TIMER);
            SendMirrorTimer(FATIGUE_TIMER, m_MirrorTimer[FATIGUE_TIMER], m_MirrorTimer[FATIGUE_TIMER], -1);
        }
        else
        {
            m_MirrorTimer[FATIGUE_TIMER] -= time_diff;
            // Timer limit - need deal damage or teleport ghost to graveyard
            if (m_MirrorTimer[FATIGUE_TIMER] < 0)
            {
                m_MirrorTimer[FATIGUE_TIMER] += 1 * IN_MILLISECONDS;
                if (IsAlive())                                            // Calculate and deal damage
                {
                    uint32 damage = GetMaxHealth() / 5 + urand(0, GetLevel() - 1);
                    EnvironmentalDamage(DAMAGE_EXHAUSTED, damage);
                }
                else if (HasPlayerFlag(PLAYER_FLAGS_GHOST))       // Teleport ghost to graveyard
                    RepopAtGraveyard();
            }
            else if (!(m_MirrorTimerFlagsLast & UNDERWATER_INDARKWATER))
                SendMirrorTimer(FATIGUE_TIMER, getMaxTimer(FATIGUE_TIMER), m_MirrorTimer[FATIGUE_TIMER], -1);
        }
    }
    else if (m_MirrorTimer[FATIGUE_TIMER] != DISABLED_MIRROR_TIMER)       // Regen timer
    {
        int32 DarkWaterTime = getMaxTimer(FATIGUE_TIMER);
        m_MirrorTimer[FATIGUE_TIMER] += 10 * time_diff;
        if (m_MirrorTimer[FATIGUE_TIMER] >= DarkWaterTime || !IsAlive())
            StopMirrorTimer(FATIGUE_TIMER);
        else if (m_MirrorTimerFlagsLast & UNDERWATER_INDARKWATER)
            SendMirrorTimer(FATIGUE_TIMER, DarkWaterTime, m_MirrorTimer[FATIGUE_TIMER], 10);
    }

    if (m_MirrorTimerFlags & (UNDERWATER_INLAVA /*| UNDERWATER_INSLIME*/) && !(_lastLiquid && _lastLiquid->SpellId))
    {
        // Breath timer not activated - activate it
        if (m_MirrorTimer[FIRE_TIMER] == DISABLED_MIRROR_TIMER)
            m_MirrorTimer[FIRE_TIMER] = getMaxTimer(FIRE_TIMER);
        else
        {
            m_MirrorTimer[FIRE_TIMER] -= time_diff;
            if (m_MirrorTimer[FIRE_TIMER] < 0)
            {
                m_MirrorTimer[FIRE_TIMER] += 2020;
                // Calculate and deal damage
                /// @todo: Check this formula
                uint32 damage = urand(600, 700);
                if (m_MirrorTimerFlags & UNDERWATER_INLAVA)
                    EnvironmentalDamage(DAMAGE_LAVA, damage);
                // need to skip Slime damage in Undercity,
                // maybe someone can find better way to handle environmental damage
                //else if (m_zoneUpdateId != 1497)
                //    EnvironmentalDamage(DAMAGE_SLIME, damage);
            }
        }
    }
    else
        m_MirrorTimer[FIRE_TIMER] = DISABLED_MIRROR_TIMER;

    // Recheck timers flag
    m_MirrorTimerFlags &= ~UNDERWATER_EXIST_TIMERS;
    for (uint8 i = 0; i < MAX_TIMERS; ++i)
        if (m_MirrorTimer[i] != DISABLED_MIRROR_TIMER)
        {
            m_MirrorTimerFlags |= UNDERWATER_EXIST_TIMERS;
            break;
        }
    m_MirrorTimerFlagsLast = m_MirrorTimerFlags;
}

///The player sobers by 1% every 9 seconds
void Player::HandleSobering()
{
    m_drunkTimer = 0;

    uint8 currentDrunkValue = GetDrunkValue();
    uint8 drunk = currentDrunkValue ? --currentDrunkValue : 0;
    SetDrunkValue(drunk);
}

/*static*/ DrunkenState Player::GetDrunkenstateByValue(uint8 value)
{
    if (value >= 90)
        return DRUNKEN_SMASHED;
    if (value >= 50)
        return DRUNKEN_DRUNK;
    if (value)
        return DRUNKEN_TIPSY;
    return DRUNKEN_SOBER;
}

void Player::SetDrunkValue(uint8 newDrunkValue, uint32 itemId /*= 0*/)
{
    newDrunkValue = std::min<uint8>(newDrunkValue, 100);
    if (newDrunkValue == GetDrunkValue())
        return;

    uint32 oldDrunkenState = Player::GetDrunkenstateByValue(GetDrunkValue());
    uint32 newDrunkenState = Player::GetDrunkenstateByValue(newDrunkValue);

    SetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_INEBRIATION, newDrunkValue);
    UpdateInvisibilityDrunkDetect();

    m_drunkTimer = 0; // reset sobering timer

    if (newDrunkenState == oldDrunkenState)
        return;

    WorldPackets::Misc::CrossedInebriationThreshold data;
    data.Guid = GetGUID();
    data.Threshold = newDrunkenState;
    data.ItemID = itemId;

    SendMessageToSet(data.Write(), true);
}

void Player::UpdateInvisibilityDrunkDetect()
{
    // select drunk percent or total SPELL_AURA_MOD_FAKE_INEBRIATE amount, whichever is higher for visibility updates
    uint8 drunkValue        = GetDrunkValue();
    int32 fakeDrunkValue    = GetFakeDrunkValue();
    int32 maxDrunkValue     = std::max<int32>(drunkValue, fakeDrunkValue);

    if (maxDrunkValue != 0)
    {
        m_invisibilityDetect.AddFlag(INVISIBILITY_DRUNK);
        m_invisibilityDetect.SetValue(INVISIBILITY_DRUNK, maxDrunkValue);
    }
    else
        m_invisibilityDetect.DelFlag(INVISIBILITY_DRUNK);

    UpdateObjectVisibility();
}







bool Player::BuildEnumData(PreparedQueryResult result, WorldPacket* data)
{
    //             0               1                2                3                 4                  5                 6               7
    //    "SELECT characters.guid, characters.name, characters.race, characters.class, characters.gender, characters.skin, characters.face, characters.hairStyle,
    //     8                     9                       10              11               12              13                     14                     15
    //    characters.hairColor, characters.facialStyle, character.level, characters.zone, characters.map, characters.position_x, characters.position_y, characters.position_z,
    //    16                    17                      18                   19                   20                     21                   22               23
    //    guild_member.guildid, characters.playerFlags, characters.at_login, character_pet.entry, character_pet.modelid, character_pet.level, characters.equipmentCache, character_banned.guid,
    //    24                      25
    //    characters.extra_flags, character_declinedname.genitive

    Field* fields = result->Fetch();

    ObjectGuid::LowType guidLow = fields[0].Get<uint32>();
    uint8 plrRace = fields[2].Get<uint8>();
    uint8 plrClass = fields[3].Get<uint8>();
    uint8 gender = fields[4].Get<uint8>();

    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);

    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(plrRace, plrClass);
    if (!info)
    {
        LOG_ERROR("entities.player", "Player {} has incorrect race/class pair. Don't build enum.", guid.ToString());
        return false;
    }
    else if (!IsValidGender(gender))
    {
        LOG_ERROR("entities.player", "Player ({}) has incorrect gender ({}), don't build enum.", guid.ToString(), gender);
        return false;
    }

    *data << guid;
    *data << fields[1].Get<std::string>();                          // name
    *data << uint8(plrRace);                                 // race
    *data << uint8(plrClass);                                // class
    *data << uint8(gender);                                  // gender

    uint8 skin = fields[5].Get<uint8>();
    uint8 face = fields[6].Get<uint8>();
    uint8 hairStyle = fields[7].Get<uint8>();
    uint8 hairColor = fields[8].Get<uint8>();
    uint8 facialStyle = fields[9].Get<uint8>();

    uint32 charFlags = 0;
    uint32 playerFlags = fields[17].Get<uint32>();
    uint16 atLoginFlags = fields[18].Get<uint16>();
    uint32 zone = (atLoginFlags & AT_LOGIN_FIRST) != 0 ? 0 : fields[11].Get<uint16>(); // if first login do not show the zone

    *data << uint8(skin);
    *data << uint8(face);
    *data << uint8(hairStyle);
    *data << uint8(hairColor);
    *data << uint8(facialStyle);

    *data << uint8(fields[10].Get<uint8>());                   // level
    *data << uint32(zone);                                   // zone
    *data << uint32(fields[12].Get<uint16>());                 // map

    *data << fields[13].Get<float>();                          // x
    *data << fields[14].Get<float>();                          // y
    *data << fields[15].Get<float>();                          // z

    *data << uint32(fields[16].Get<uint32>());                 // guild id

    if (playerFlags & PLAYER_FLAGS_RESTING)
        playerFlags |= CHARACTER_FLAG_RESTING;
    if (atLoginFlags & AT_LOGIN_RESURRECT)
        playerFlags &= ~PLAYER_FLAGS_GHOST;
    if (playerFlags & PLAYER_FLAGS_HIDE_HELM)
        charFlags |= CHARACTER_FLAG_HIDE_HELM;
    if (playerFlags & PLAYER_FLAGS_HIDE_CLOAK)
        charFlags |= CHARACTER_FLAG_HIDE_CLOAK;
    if (playerFlags & PLAYER_FLAGS_GHOST)
        charFlags |= CHARACTER_FLAG_GHOST;
    if (atLoginFlags & AT_LOGIN_RENAME)
        charFlags |= CHARACTER_FLAG_RENAME;
    if (fields[23].Get<uint32>())
        charFlags |= CHARACTER_FLAG_LOCKED_BY_BILLING;
    if (sWorld->getBoolConfig(CONFIG_DECLINED_NAMES_USED))
    {
        if (!fields[25].Get<std::string>().empty())
            charFlags |= CHARACTER_FLAG_DECLINED;
    }
    else
        charFlags |= CHARACTER_FLAG_DECLINED;

    *data << uint32(charFlags);                              // character flags

    // character customize flags
    if (atLoginFlags & AT_LOGIN_CUSTOMIZE)
        *data << uint32(CHAR_CUSTOMIZE_FLAG_CUSTOMIZE);
    else if (atLoginFlags & AT_LOGIN_CHANGE_FACTION)
        *data << uint32(CHAR_CUSTOMIZE_FLAG_FACTION);
    else if (atLoginFlags & AT_LOGIN_CHANGE_RACE)
        *data << uint32(CHAR_CUSTOMIZE_FLAG_RACE);
    else
        *data << uint32(CHAR_CUSTOMIZE_FLAG_NONE);

    // First login
    *data << uint8(atLoginFlags & AT_LOGIN_FIRST ? 1 : 0);

    // Pets info
    uint32 petDisplayId = 0;
    uint32 petLevel = 0;
    uint32 petFamily = 0;

    // show pet at selection character in character list only for non-ghost character
    if (result && !(playerFlags & PLAYER_FLAGS_GHOST) && (plrClass == CLASS_WARLOCK || plrClass == CLASS_HUNTER || (plrClass == CLASS_DEATH_KNIGHT && (fields[21].Get<uint32>()&PLAYER_EXTRA_SHOW_DK_PET))))
    {
        uint32 entry = fields[19].Get<uint32>();
        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(entry);
        if (creatureInfo)
        {
            petDisplayId = fields[20].Get<uint32>();
            petLevel = fields[21].Get<uint16>();
            petFamily = creatureInfo->family;
        }
    }

    *data << uint32(petDisplayId);
    *data << uint32(petLevel);
    *data << uint32(petFamily);

    std::vector<std::string_view> equipment = Acore::Tokenize(fields[22].Get<std::string_view>(), ' ', false);
    for (uint8 slot = 0; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        uint32 const visualBase = slot * 2;
        Optional<uint32> itemId;

        if (visualBase < equipment.size())
        {
            itemId = Acore::StringTo<uint32>(equipment[visualBase]);
        }

        ItemTemplate const* proto = nullptr;
        if (itemId)
        {
            proto = sObjectMgr->GetItemTemplate(*itemId);
        }

        if (!proto)
        {
            if (!itemId || *itemId)
            {
                LOG_WARN("entities.player.loading", "Player {} has invalid equipment '{}' in `equipmentcache` at index {}. Skipped.",
                    guid.ToString(), (visualBase < equipment.size()) ? equipment[visualBase] : "<none>", visualBase);
            }

            *data << uint32(0);
            *data << uint8(0);
            *data << uint32(0);

            continue;
        }

        SpellItemEnchantmentEntry const* enchant = nullptr;

        Optional<uint32> enchants = {};
        if ((visualBase + 1) < equipment.size())
        {
            enchants = Acore::StringTo<uint32>(equipment[visualBase + 1]);
        }

        if (!enchants)
        {
            LOG_WARN("entities.player.loading", "Player {} has invalid enchantment info '{}' in `equipmentcache` at index {}. Skipped.",
                guid.ToString(), ((visualBase + 1) < equipment.size()) ? equipment[visualBase + 1] : "<none>", visualBase + 1);

            enchants = 0;
        }

        for (uint8 enchantSlot = PERM_ENCHANTMENT_SLOT; enchantSlot <= TEMP_ENCHANTMENT_SLOT; ++enchantSlot)
        {
            // values stored in 2 uint16
            uint32 enchantId = 0x0000FFFF & ((*enchants) >> enchantSlot * 16);
            if (!enchantId)
            {
                continue;
            }

            enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
            if (enchant)
            {
                break;
            }
        }

        *data << uint32(proto->DisplayInfoID);
        *data << uint8(proto->InventoryType);
        *data << uint32(enchant ? enchant->aura_id : 0);
    }

    return true;
}

bool Player::IsClass(Classes unitClass, ClassContext context) const
{
    Optional<bool> scriptResult = sScriptMgr->OnPlayerIsClass(this, unitClass, context);
    if (scriptResult != std::nullopt)
        return *scriptResult;
    else
        return (getClass() == unitClass);
}

void Player::ToggleAFK()
{
    ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_AFK);

    // afk player not allowed in battleground
    if (!GetSession()->HasPermission(rbac::RBAC_PERM_CAN_AFK_ON_BATTLEGROUND) && isAFK() && InBattleground() && !InArena())
        LeaveBattleground();
}

void Player::ToggleDND()
{
    ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_DND);
}

uint8 Player::GetChatTag() const
{
    uint8 tag = CHAT_TAG_NONE;

    if (isGMChat())
        tag |= CHAT_TAG_GM;
    if (isDND())
        tag |= CHAT_TAG_DND;
    if (isAFK())
        tag |= CHAT_TAG_AFK;
    if (IsCommentator())
        tag |= CHAT_TAG_COM;
    if (IsDeveloper())
        tag |= CHAT_TAG_DEV;

    return tag;
}

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



void Player::AddToWorld()
{
    ///- Do not add/remove the player from the object storage
    ///- It will crash when updating the ObjectAccessor
    ///- The player should only be added when logging in
    Unit::AddToWorld();

    for (uint8 i = PLAYER_SLOT_START; i < PLAYER_SLOT_END; ++i)
        if (m_items[i])
            m_items[i]->AddToWorld();
}

void Player::RemoveFromWorld()
{
    // cleanup
    if (IsInWorld())
    {
        ///- Release charmed creatures, unsummon totems and remove pets/guardians
        StopCastingCharm();
        StopCastingBindSight();
        UnsummonPetTemporaryIfAny();
        ClearComboPoints(); // pussywizard: crashfix
        ClearComboPointHolders(); // pussywizard: crashfix
        if (ObjectGuid lguid = GetLootGUID()) // pussywizard: crashfix
            m_session->DoLootRelease(lguid);
        sOutdoorPvPMgr->HandlePlayerLeaveZone(this, m_zoneUpdateId);
        sBattlefieldMgr->HandlePlayerLeaveZone(this, m_zoneUpdateId);
        sWorldState->HandlePlayerLeaveZone(this, static_cast<AreaTableIDs>(m_zoneUpdateId));
    }

    // Remove items from world before self - player must be found in Item::RemoveFromObjectUpdate
    for (uint8 i = PLAYER_SLOT_START; i < PLAYER_SLOT_END; ++i)
    {
        if (m_items[i])
            m_items[i]->RemoveFromWorld();
    }

    for (ItemMap::iterator iter = mMitems.begin(); iter != mMitems.end(); ++iter)
        iter->second->RemoveFromWorld();

    ///- Do not add/remove the player from the object storage
    ///- It will crash when updating the ObjectAccessor
    ///- The player should only be removed when logging out
    Unit::RemoveFromWorld();

    if (m_uint32Values)
    {
        if (WorldObject* viewpoint = GetViewpoint())
        {
            LOG_FATAL("entities.player", "Player {} has viewpoint {} {} when removed from world", GetName(), viewpoint->GetEntry(), viewpoint->GetTypeId());
            SetViewpoint(viewpoint, false);
        }
    }
}









bool Player::CanInteractWithQuestGiver(Object* questGiver)
{
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
            return GetNPCIfCanInteractWith(questGiver->GetGUID(), UNIT_NPC_FLAG_QUESTGIVER) != nullptr;
        case TYPEID_GAMEOBJECT:
            return GetGameObjectIfCanInteractWith(questGiver->GetGUID(), GAMEOBJECT_TYPE_QUESTGIVER) != nullptr;
        case TYPEID_PLAYER:
            return IsAlive() && questGiver->ToPlayer()->IsAlive();
        case TYPEID_ITEM:
            return IsAlive();
        default:
            break;
    }
    return false;
}

Creature* Player::GetNPCIfCanInteractWith(ObjectGuid const& guid, uint32 npcflagmask)
{
    // unit checks
    if (!guid)
        return nullptr;

    if (!IsInWorld())
        return nullptr;

    if (IsInFlight())
        return nullptr;

    // exist (we need look pets also for some interaction (quest/etc)
    Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*this, guid);
    if (!creature)
        return nullptr;

    // Deathstate checks
    if (!IsAlive() && !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
        return nullptr;

    // alive or spirit healer
    if (!creature->IsAlive() && !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
        return nullptr;

    // appropriate npc type
    if (npcflagmask && !creature->HasNpcFlag(NPCFlags(npcflagmask)))
        return nullptr;

    // not allow interaction under control, but allow with own pets
    if (creature->GetCharmerGUID())
        return nullptr;

    // xinef: perform better check
    if (creature->GetReactionTo(this) <= REP_UNFRIENDLY)
        return nullptr;

    // xinef: not needed, CORRECTLY checked above including forced reputations etc
    // not unfriendly
    //if (FactionTemplateEntry const* factionTemplate = sFactionTemplateStore.LookupEntry(creature->GetFaction()))
    //    if (factionTemplate->faction)
    //        if (FactionEntry const* faction = sFactionStore.LookupEntry(factionTemplate->faction))
    //            if (faction->reputationListID >= 0 && GetReputationMgr().GetRank(faction) <= REP_UNFRIENDLY)
    //                return nullptr;

    // not too far
    if (!creature->IsWithinDistInMap(this, INTERACTION_DISTANCE))
        return nullptr;

    return creature;
}

GameObject* Player::GetGameObjectIfCanInteractWith(ObjectGuid const& guid, GameobjectTypes type) const
{
    if (GameObject* go = GetMap()->GetGameObject(guid))
    {
        if (go->GetGoType() == type)
        {
            // Players cannot interact with gameobjects that use the "Point" icon
            if (go->GetGOInfo()->IconName == "Point")
            {
                return nullptr;
            }

            if (go->IsWithinDistInMap(this))
            {
                return go;
            }

            LOG_DEBUG("maps", "IsGameObjectOfTypeInRange: GameObject '{}' [{}] is too far away from player {} [{}] to be used by him (distance={}, maximal 10 is allowed)",
                go->GetGOInfo()->name, go->GetGUID().ToString(), GetName(), GetGUID().ToString(), go->GetDistance(this));
        }
    }
    return nullptr;
}

bool Player::IsFalling() const
{
    // Xinef: Added !IsInFlight check
    return GetPositionZ() < m_lastFallZ && !IsInFlight();
}

void Player::SetInWater(bool apply)
{
    if (m_isInWater == apply)
        return;

    //define player in water by opcodes
    //move player's guid into HateOfflineList of those mobs
    //which can't swim and move guid back into ThreatList when
    //on surface.
    //TODO: exist also swimming mobs, and function must be symmetric to enter/leave water
    m_isInWater = apply;

    // remove auras that need water/land
    RemoveAurasWithInterruptFlags(apply ? AURA_INTERRUPT_FLAG_NOT_ABOVEWATER : AURA_INTERRUPT_FLAG_NOT_UNDERWATER);

    GetThreatMgr().EvaluateSuppressed();

    if (InstanceScript* instance = GetInstanceScript())
        instance->OnPlayerInWaterStateUpdate(this, apply);
}

bool Player::IsInAreaTriggerRadius(AreaTrigger const* trigger, float delta) const
{
    if (!trigger || GetMapId() != trigger->map)
        return false;

    if (trigger->radius > 0)
    {
        // if we have radius check it
        float dist = GetDistance(trigger->x, trigger->y, trigger->z);
        if (dist > trigger->radius + delta)
            return false;
    }
    else
    {
        Position center(trigger->x, trigger->y, trigger->z, trigger->orientation);
        if (!IsWithinBox(center, trigger->length / 2 + delta, trigger->width / 2 + delta, trigger->height / 2 + delta))
            return false;
    }

    return true;
}

bool Player::CanBeGameMaster() const
{
    return GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
}

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

void Player::SetGMVisible(bool on)
{
    const uint32 VISUAL_AURA = 37800;

    if (on)
    {
        RemoveAurasDueToSpell(VISUAL_AURA);
        m_ExtraFlags &= ~PLAYER_EXTRA_GM_INVISIBLE;
        SetServerSideVisibility(SERVERSIDE_VISIBILITY_GM, SEC_PLAYER);

        CombatStopWithPets();
    }
    else
    {
        AddAura(VISUAL_AURA, this);
        m_ExtraFlags |= PLAYER_EXTRA_GM_INVISIBLE;
        SetServerSideVisibility(SERVERSIDE_VISIBILITY_GM, GetSession()->GetSecurity());
    }
}

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

///- If the player is invited, remove him. If the group if then only 1 person, disband the group.
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

void Player::SendLogXPGain(uint32 GivenXP, Unit* victim, uint32 BonusXP, bool recruitAFriend, float /*group_rate*/)
{
    WorldPacket data(SMSG_LOG_XPGAIN, 22); // guess size?
    data << (victim ? victim->GetGUID() : ObjectGuid::Empty);   // guid
    data << uint32(GivenXP + BonusXP);                          // given experience
    data << uint8(victim ? 0 : 1);                              // 00-kill_xp type, 01-non_kill_xp type

    if (victim)
    {
        data << uint32(GivenXP);                            // experience without bonus

        // should use group_rate here but can't figure out how
        data << float(1);                                   // 1 - none 0 - 100% group bonus output
    }

    data << uint8(recruitAFriend ? 1 : 0);                  // does the GivenXP include a RaF bonus?
    SendDirectMessage(&data);
}

void Player::GiveXP(uint32 xp, Unit* victim, float group_rate, bool isLFGReward)
{
    if (xp < 1)
        return;

    if (!IsAlive() && !GetBattlegroundId() && !isLFGReward)
        return;

    if (HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN) || HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
        return;

    if (victim && victim->IsCreature() && !victim->ToCreature()->hasLootRecipient())
        return;

    uint8 level = GetLevel();
    sScriptMgr->OnPlayerBeforeGetLevelForXPGain(this, level);

    // Favored experience increase START
    uint32 zone = GetZoneId();
    float favored_exp_mult = 0;
    if ((zone == AREA_HELLFIRE_PENINSULA || zone == AREA_HELLFIRE_RAMPARTS || zone == AREA_MAGTHERIDONS_LAIR || zone == AREA_THE_BLOOD_FURNACE || zone == AREA_THE_SHATTERED_HALLS) && HasAnyAuras(32096 /*Thrallmar's Favor*/, 32098 /*Honor Hold's Favor*/))
        favored_exp_mult = 0.05f; // Thrallmar's Favor and Honor Hold's Favor

    xp = uint32(xp * (1 + favored_exp_mult));
    // Favored experience increase END

    // XP to money conversion processed in Player::RewardQuest
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // Trial account level cap (0 disables the cap)
    if (uint32 trialLevelCap = sWorld->getIntConfig(CONFIG_TRIAL_LEVEL_CAP))
        if (GetSession()->IsTrialAccount())
            maxLevel = std::min(maxLevel, trialLevelCap);

    if (level >= maxLevel)
        return;

    if (HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
        xp = std::max(1u, xp / 2);

    uint32 bonus_xp = 0;
    bool recruitAFriend = GetsRecruitAFriendBonus(true);

    // RaF does NOT stack with rested experience
    if (recruitAFriend)
        bonus_xp = 2 * xp; // xp + bonus_xp must add up to 3 * xp for RaF; calculation for quests done client-side
    else
        bonus_xp = victim ? GetXPRestBonus(xp) : 0; // XP resting bonus

    // hooks and multipliers can modify the xp with a zero or negative value
    // check again before sending invalid xp to the client
    if (xp < 1)
        return;

    SendLogXPGain(xp, victim, bonus_xp, recruitAFriend, group_rate);

    uint32 curXP = GetUInt32Value(PLAYER_XP);
    uint32 nextLvlXP = GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    uint32 newXP = curXP + xp + bonus_xp;

    while (newXP >= nextLvlXP && level < maxLevel)
    {
        newXP -= nextLvlXP;

        if (level < maxLevel)
            GiveLevel(level + 1);

        level = GetLevel();
        nextLvlXP = GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    }

    SetUInt32Value(PLAYER_XP, newXP);
}

// Update player to next level
// Current player experience not update (must be update by caller)
void Player::GiveLevel(uint8 level)
{
    uint8 oldLevel = GetLevel();
    if (level == oldLevel)
        return;

    if (!sScriptMgr->OnPlayerCanGiveLevel(this, level))
        return;

    if (Guild* guild = GetGuild())
        guild->UpdateMemberData(this, GUILD_MEMBER_DATA_LEVEL, level);

    PlayerLevelInfo info;
    sObjectMgr->GetPlayerLevelInfo(getRace(true), getClass(), level, &info);

    PlayerClassLevelInfo classInfo;
    sObjectMgr->GetPlayerClassLevelInfo(getClass(), level, &classInfo);

    WorldPackets::Misc::LevelUpInfo packet;
    packet.Level = level;
    packet.HealthDelta = int32(classInfo.basehealth) - int32(GetCreateHealth());

    /// @todo find some better solution
    // for (int i = 0; i < MAX_POWERS; ++i)
    packet.PowerDelta[0] = int32(classInfo.basemana) - int32(GetCreateMana());
    packet.PowerDelta[1] = 0;
    packet.PowerDelta[2] = 0;
    packet.PowerDelta[3] = 0;
    packet.PowerDelta[4] = 0;
    packet.PowerDelta[5] = 0;

    for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
        packet.StatDelta[i] = int32(info.stats[i]) - GetCreateStat(Stats(i));

    SendDirectMessage(packet.Write());

    SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(level));

    //update level, max level of skills
    m_Played_time[PLAYED_TIME_LEVEL] = 0;                   // Level Played Time reset

    _ApplyAllLevelScaleItemMods(false);

    SetLevel(level);

    UpdateSkillsForLevel();

    // save base values (bonuses already included in stored stats
    for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
        SetCreateStat(Stats(i), info.stats[i]);

    SetCreateHealth(classInfo.basehealth);
    SetCreateMana(classInfo.basemana);

    InitTalentForLevel();
    InitTaxiNodesForLevel();
    InitGlyphsForLevel();

    UpdateAllStats();

    if (sWorld->getBoolConfig(CONFIG_ALWAYS_MAXSKILL)) // Max weapon skill when leveling up
        UpdateSkillsToMaxSkillsForLevel();

    _ApplyAllLevelScaleItemMods(true);

    if (!isDead())
    {
        // set current level health and mana/energy to maximum after applying all mods.
        SetFullHealth();
        SetPower(POWER_MANA, GetMaxPower(POWER_MANA));
        SetPower(POWER_ENERGY, GetMaxPower(POWER_ENERGY));
        if (GetPower(POWER_RAGE) > GetMaxPower(POWER_RAGE))
            SetPower(POWER_RAGE, GetMaxPower(POWER_RAGE));
        SetPower(POWER_FOCUS, 0);
        SetPower(POWER_HAPPINESS, 0);
    }

    // update level to hunter/summon pet
    if (Pet* pet = GetPet())
        pet->SynchronizeLevelWithOwner();

    MailLevelReward const* mailReward = sObjectMgr->GetMailLevelReward(level, getRaceMask());
    if (mailReward && sScriptMgr->OnPlayerCanGiveMailRewardAtGiveLevel(this, level))
    {
        //- TODO: Poor design of mail system
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft(mailReward->mailTemplateId).SendMailTo(trans, this, MailSender(MAIL_CREATURE, mailReward->senderEntry));
        CharacterDatabase.CommitTransaction(trans);
    }

    UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_REACH_LEVEL);

    // Refer-A-Friend
    if (GetSession()->GetRecruiterId())
        if (level < sWorld->getIntConfig(CONFIG_MAX_RECRUIT_A_FRIEND_BONUS_PLAYER_LEVEL))
            if (level % 2 == 0)
            {
                ++m_grantableLevels;

                if (!HasByteFlag(PLAYER_FIELD_BYTES, 1, 0x01))
                    SetByteFlag(PLAYER_FIELD_BYTES, 1, 0x01);
            }

    SendQuestGiverStatusMultiple();

    sScriptMgr->OnPlayerLevelChanged(this, oldLevel);
}

bool Player::IsMaxLevel() const
{
    return GetLevel() >= GetUInt32Value(PLAYER_FIELD_MAX_LEVEL);
}













void Player::RemoveMail(uint32 id)
{
    for (PlayerMails::iterator itr = m_mail.begin(); itr != m_mail.end(); ++itr)
    {
        if ((*itr)->messageID == id)
        {
            //do not delete item, because Player::removeMail() is called when returning mail to sender.
            m_mail.erase(itr);
            return;
        }
    }
}

void Player::SendMailResult(uint32 mailId, MailResponseType mailAction, MailResponseResult mailError, uint32 equipError, ObjectGuid::LowType item_guid, uint32 item_count)
{
    WorldPacket data(SMSG_SEND_MAIL_RESULT, (4 + 4 + 4 + (mailError == MAIL_ERR_EQUIP_ERROR ? 4 : (mailAction == MAIL_ITEM_TAKEN ? 4 + 4 : 0))));
    data << (uint32) mailId;
    data << (uint32) mailAction;
    data << (uint32) mailError;
    if (mailError == MAIL_ERR_EQUIP_ERROR)
        data << (uint32) equipError;
    else if (mailAction == MAIL_ITEM_TAKEN)
    {
        data << (uint32) item_guid;                         // item guid low?
        data << (uint32) item_count;                        // item count?
    }
    SendDirectMessage(&data);
}

void Player::SendNewMail()
{
    // deliver undelivered mail
    WorldPacket data(SMSG_RECEIVED_MAIL, 4);
    data << (uint32) 0;
    SendDirectMessage(&data);

    // The client caches its inbox and refuses to re-query it more than once a minute, so a mailbox
    // opened inside that window still shows the old list. Pushing the inbox refreshes it in place.
    // Only when in world: _LoadInventory mails problematic items during login, and that must not
    // push a mail list to a client that has not finished logging in yet.
    if (IsInWorld() && sWorld->getBoolConfig(CONFIG_MAIL_PUSH_INBOX_ON_DELIVERY))
        GetSession()->SendMailList();
}

void Player::AddNewMailDeliverTime(time_t deliver_time)
{
    if (deliver_time <= GameTime::GetGameTime().count())                      // ready now
    {
        ++unReadMails;
        SendNewMail();
    }
    else                                                    // not ready and no have ready mails
    {
        if (!m_nextMailDelivereTime || m_nextMailDelivereTime > deliver_time)
            m_nextMailDelivereTime = deliver_time;
    }
}











void Player::SendLearnPacket(uint32 spellId, bool learn)
{
    if (learn)
    {
        WorldPacket data(SMSG_LEARNED_SPELL, 6);
        data << uint32(spellId);
        data << uint16(0);
        SendDirectMessage(&data);
    }
    else
    {
        WorldPacket data(SMSG_REMOVED_SPELL, 4);
        data << uint32(spellId);
        SendDirectMessage(&data);
    }
}





bool Player::_addSpell(uint32 spellId, uint8 addSpecMask, bool temporary, bool learnFromSkill /*= false*/)
{
    // pussywizard: this can be called to OVERWRITE currently existing spell params! usually to set active = false for lower ranks of a spell

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!SpellMgr::CheckSpellValid(spellInfo, spellId, false))
        return false;

    // pussywizard: already found and temporary, nothing to do
    PlayerSpellMap::iterator itr = m_spells.find(spellId);
    if (itr != m_spells.end() && itr->second->State == PLAYERSPELL_TEMPORARY)
        return false;

    // xinef: send packet so client can properly recognize this new spell
    // xinef: ignore passive spells and spells with learn effect
    // xinef: send spells with no aura effects (ie dual wield)
    if (IsInWorld() && !isBeingLoaded() && temporary && !learnFromSkill && (!spellInfo->HasAttribute(SpellAttr0(SPELL_ATTR0_PASSIVE | SPELL_ATTR0_DO_NOT_DISPLAY)) || !spellInfo->HasAnyAura()) && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
        SendLearnPacket(spellInfo->Id, true);

    // xinef: DO NOT allow to learn spell with effect learn spell!
    // xinef: if spell possess spell learn effects only, learn those spells as temporary (eg. Metamorphosis, Tree of Life)
    if (temporary && spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].IsEffect())
            {
                if (spellInfo->Effects[i].Effect != SPELL_EFFECT_LEARN_SPELL)
                {
                    LOG_INFO("entities.player", "TRYING TO LEARN SPELL WITH EFFECT LEARN: {}, PLAYER: {}", spellId, GetGUID().ToString());
                    return false;
                    //ABORT();
                }
                else if (SpellInfo const* learnSpell = sSpellMgr->GetSpellInfo(spellInfo->Effects[i].TriggerSpell))
                    _addSpell(learnSpell->Id, SPEC_MASK_ALL, true);
            }

        return false;
    }

    if (itr != m_spells.end()) // pussywizard: already know this spell, so update information
    {
        // pussywizard: do nothing if already set as wanted
        if (itr->second->State != PLAYERSPELL_REMOVED && (itr->second->specMask & addSpecMask) == addSpecMask)
            return false;

        // pussywizard: need cast auras, learn linked spells, do professions stuff, etc.
        // pussywizard: but only for spells that are really added (inactive -> active OR added to current spec)
        bool spellIsNew = true;

        // pussywizard: present in m_spells, not removed, already in current spec, already active
        if (itr->second->State != PLAYERSPELL_REMOVED && itr->second->IsInSpec(m_activeSpec))
            spellIsNew = false;

        // pussywizard: update info in m_spells
        if (itr->second->State != PLAYERSPELL_NEW && (itr->second->specMask & addSpecMask) != addSpecMask)
            itr->second->State = PLAYERSPELL_CHANGED;
        itr->second->Active = true;
        itr->second->specMask |= addSpecMask;

        if (!spellIsNew)
            return true;
    }
    else // pussywizard: not found in m_spells
    {
        PlayerSpell* newspell = new PlayerSpell;
        newspell->Active = true;
        newspell->State = temporary ? PLAYERSPELL_TEMPORARY : (isBeingLoaded() ? PLAYERSPELL_UNCHANGED : PLAYERSPELL_NEW);
        newspell->specMask = addSpecMask;

        m_spells[spellId] = newspell;
    }

    // pussywizard: return if spell not in current spec
    // pussywizard: return true to fix active for ranks, this condition is true only at loading, so no problems with learning packets
    if (!((1 << GetActiveSpec()) & addSpecMask))
        return true;

    // xinef: do not add spells with effect learn spell
    if (spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
    {
        LOG_INFO("entities.player", "TRYING TO LEARN SPELL WITH EFFECT LEARN 2: {}, PLAYER: {}", spellId, GetGUID().ToString());
        m_spells.erase(spellInfo->Id); // mem leak, but should never happen
        return false;
        //ABORT();
    }
    // pussywizard: cast passive spells (including all talents without SPELL_EFFECT_LEARN_SPELL) with additional checks
    else if (spellInfo->IsPassive() || (spellInfo->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) && spellInfo->Stances))
    {
        if (IsNeedCastPassiveSpellAtLearn(spellInfo))
            CastSpell(this, spellId, true);
    }
    // pussywizard: cast and return, learnt spells will update profession count, etc.
    else if (spellInfo->HasEffect(SPELL_EFFECT_SKILL_STEP))
    {
        CastSpell(this, spellId, true);
        return false;
    }

    // xinef: unapply aura stats if dont meet requirements
    // xinef: handle only if player is not loaded, loading is handled in loadfromdb
    if (!isBeingLoaded())
        if (Aura* aura = GetAura(spellId))
        {
            if (aura->GetSpellInfo()->CasterAuraState == AURA_STATE_HEALTHLESS_35_PERCENT ||
                    aura->GetSpellInfo()->CasterAuraState == AURA_STATE_HEALTH_ABOVE_75_PERCENT ||
                    aura->GetSpellInfo()->CasterAuraState == AURA_STATE_HEALTHLESS_20_PERCENT )
                if (!HasAuraState((AuraStateType)aura->GetSpellInfo()->CasterAuraState))
                    aura->HandleAllEffects(aura->GetApplicationOfTarget(GetGUID()), AURA_EFFECT_HANDLE_REAL, false);
        }

    // pussywizard: update free primary prof points
    if (uint32 freeProfs = GetFreePrimaryProfessionPoints())
    {
        if (spellInfo->IsPrimaryProfessionFirstRank())
            SetFreePrimaryProfessions(freeProfs - 1);
    }

    uint16 maxskill = GetMaxSkillValueForLevel();
    SpellLearnSkillNode const* spellLearnSkill = sSpellMgr->GetSpellLearnSkill(spellId);
    SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    // xinef: set appropriate skill value
    if (spellLearnSkill)
    {
        uint32 skill_value = GetPureSkillValue(spellLearnSkill->skill);
        uint32 skill_max_value = GetPureMaxSkillValue(spellLearnSkill->skill);
        uint32 new_skill_max_value = spellLearnSkill->maxvalue == 0 ? maxskill : spellLearnSkill->maxvalue;

        if (skill_value < spellLearnSkill->value)
            skill_value = spellLearnSkill->value;
        if (skill_max_value < new_skill_max_value)
            skill_max_value = new_skill_max_value;

        SetSkill(spellLearnSkill->skill, spellLearnSkill->step, skill_value, skill_max_value);
    }
    else
    {
        // not ranked skills
        for (SkillLineAbilityMap::const_iterator _spell_idx = skill_bounds.first; _spell_idx != skill_bounds.second; ++_spell_idx)
        {
            SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(_spell_idx->second->SkillLine);
            if (!pSkill)
                continue;

            /// @todo confirm if rogues start wth lockpicking skill at level 1 but only recieve the spell to use it at level 16
            // Added for runeforging, it is confirmed via sniff that this happens when death knights learn the spell, not on character creation.
            if ((_spell_idx->second->AcquireMethod == SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN && !HasSkill(pSkill->id)) || ((pSkill->id == SKILL_LOCKPICKING || pSkill->id == SKILL_RUNEFORGING) && _spell_idx->second->TrivialSkillLineRankHigh == 0))
                LearnDefaultSkill(pSkill->id, 0);

            if (pSkill->id == SKILL_MOUNTS && !Has310Flyer(false))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED && spellInfo->Effects[i].CalcValue() == 310)
                        SetHas310Flyer(true);
        }
    }

    // xinef: update achievement criteria
    if (!GetSession()->PlayerLoading())
    {
        for (SkillLineAbilityMap::const_iterator _spell_idx = skill_bounds.first; _spell_idx != skill_bounds.second; ++_spell_idx)
        {
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LINE, _spell_idx->second->SkillLine);
            UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILLLINE_SPELLS, _spell_idx->second->SkillLine);
        }
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LEARN_SPELL, spellId);
    }

    return true;
}









bool Player::Has310Flyer(bool checkAllSpells, uint32 excludeSpellId)
{
    if (!checkAllSpells)
        return m_ExtraFlags & PLAYER_EXTRA_HAS_310_FLYER;
    else
    {
        SetHas310Flyer(false);
        SpellInfo const* spellInfo;
        for (PlayerSpellMap::iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
        {
            // pussywizard:
            if (itr->second->State == PLAYERSPELL_REMOVED)
                continue;

            if (itr->first == excludeSpellId)
                continue;

            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(itr->first);
            for (SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
            {
                if (_spell_idx->second->SkillLine != SKILL_MOUNTS)
                    break;  // We can break because mount spells belong only to one skillline (at least 310 flyers do)

                spellInfo = sSpellMgr->AssertSpellInfo(itr->first);
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED &&
                            spellInfo->Effects[i].CalcValue() == 310)
                    {
                        SetHas310Flyer(true);
                        return true;
                    }
            }
        }
    }

    return false;
}



















Mail* Player::GetMail(uint32 id)
{
    for (PlayerMails::iterator itr = m_mail.begin(); itr != m_mail.end(); ++itr)
    {
        if ((*itr)->messageID == id)
        {
            return (*itr);
        }
    }
    return nullptr;
}



void Player::DestroyForPlayer(Player* target, bool onDeath) const
{
    Unit::DestroyForPlayer(target, onDeath);

    for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i) // xinef: previously INVENTORY_SLOT_BAG_END
    {
        if (!m_items[i])
            continue;

        m_items[i]->DestroyForPlayer(target);
    }

    if (target == this)
    {
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (!m_items[i])
                continue;

            m_items[i]->DestroyForPlayer(target);
        }
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
        {
            if (!m_items[i])
                continue;

            m_items[i]->DestroyForPlayer(target);
        }
    }
}







/**
 * Deletes a character from the database
 *
 * The way, how the characters will be deleted is decided based on the config option.
 *
 * @param playerguid       the low-GUID from the player which should be deleted
 * @param accountId        the account id from the player
 * @param updateRealmChars when this flag is set, the amount of characters on that realm will be updated in the realmlist
 * @param deleteFinally    if this flag is set, the config option will be ignored and the character will be permanently removed from the database
 */
void Player::DeleteFromDB(ObjectGuid::LowType lowGuid, uint32 accountId, bool updateRealmChars, bool deleteFinally)
{
    // for not existed account avoid update realm
    if (!accountId)
        updateRealmChars = false;

    ObjectGuid playerGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid);

    uint32 charDelete_method = sWorld->getIntConfig(CONFIG_CHARDELETE_METHOD);
    uint32 charDelete_minLvl = sWorld->getIntConfig(CONFIG_CHARDELETE_MIN_LEVEL);

    // if we want to finally delete the character or the character does not meet the level requirement,
    // we set it to mode CHAR_DELETE_REMOVE
    if (deleteFinally || sCharacterCache->GetCharacterLevelByGuid(playerGuid) < charDelete_minLvl)
        charDelete_method = CHAR_DELETE_REMOVE;

    if (uint32 guildId = sCharacterCache->GetCharacterGuildIdByGuid(playerGuid))
        if (Guild* guild = sGuildMgr->GetGuildById(guildId))
            guild->DeleteMember(playerGuid, false, false, true);

    // remove from arena teams
    LeaveAllArenaTeams(playerGuid);

    // close player ticket if any
    GmTicket* ticket = sTicketMgr->GetTicketByPlayer(playerGuid);
    if (ticket)
        sTicketMgr->CloseTicket(ticket->GetId(), playerGuid);

    // remove from group
    if (ObjectGuid groupId = sCharacterCache->GetCharacterGroupGuidByGuid(playerGuid))
        if (Group* group = sGroupMgr->GetGroupByGUID(groupId.GetCounter()))
            RemoveFromGroup(group, playerGuid);

    // Remove signs from petitions (also remove petitions if owner);
    RemovePetitionsAndSigns(playerGuid, 10);

    CharacterDatabasePreparedStatement* stmt = nullptr;

    switch (charDelete_method)
    {
        // Completely remove from the database
        case CHAR_DELETE_REMOVE:
            {
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_COD_ITEM_MAIL);
                stmt->SetData(0, lowGuid);
                PreparedQueryResult resultMail = CharacterDatabase.Query(stmt);

                if (resultMail)
                {
                    std::unordered_map<uint32, std::vector<Item*>> itemsByMail;

                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_MAILITEMS);
                    stmt->SetData(0, lowGuid);
                    PreparedQueryResult resultItems = CharacterDatabase.Query(stmt);

                    if (resultItems)
                    {
                        do
                        {
                            Field* fields = resultItems->Fetch();
                            uint32 mailId = fields[14].Get<uint32>();
                            if (Item* mailItem = _LoadMailedItem(playerGuid, nullptr, mailId, nullptr, fields))
                            {
                                itemsByMail[mailId].push_back(mailItem);
                            }
                        } while (resultItems->NextRow());
                    }

                    do
                    {
                        Field* mailFields = resultMail->Fetch();

                        uint32 mail_id       = mailFields[0].Get<uint32>();
                        uint8 mailType       = mailFields[1].Get<uint8>();
                        uint16 mailTemplateId = mailFields[2].Get<uint16>();
                        uint32 sender        = mailFields[3].Get<uint32>();
                        std::string subject  = mailFields[4].Get<std::string>();
                        std::string body     = mailFields[5].Get<std::string>();
                        uint32 money         = mailFields[6].Get<uint32>();
                        bool has_items       = mailFields[7].Get<bool>();

                        // We can return mail now
                        // So firstly delete the old one
                        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                        stmt->SetData(0, mail_id);
                        trans->Append(stmt);

                        // Mail is not from player
                        if (mailType != MAIL_NORMAL)
                        {
                            if (has_items)
                            {
                                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
                                stmt->SetData(0, mail_id);
                                trans->Append(stmt);
                            }
                            continue;
                        }

                        MailDraft draft(subject, body);
                        if (mailTemplateId)
                            draft = MailDraft(mailTemplateId, false);    // items are already included

                        auto itemsItr = itemsByMail.find(mail_id);
                        if (itemsItr != itemsByMail.end())
                        {
                            for (Item* item : itemsItr->second)
                            {
                                draft.AddItem(item);
                            }

                            // MailDraft will take care of freeing memory.
                            itemsByMail.erase(itemsItr);
                        }

                        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
                        stmt->SetData(0, mail_id);
                        trans->Append(stmt);

                        uint32 pl_account = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid(HighGuid::Player, lowGuid));

                        draft.AddMoney(money).SendReturnToSender(pl_account, lowGuid, sender, trans);
                    } while (resultMail->NextRow());
                }

                // Unsummon and delete for pets in world is not required: player deleted from CLI or character list with not loaded pet.
                // NOW we can finally clear other DB data related to character
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_PET_IDS);
                stmt->SetData(0, lowGuid);
                PreparedQueryResult resultPets = CharacterDatabase.Query(stmt);

                if (resultPets)
                {
                    do
                    {
                        ObjectGuid::LowType petguidlow = (*resultPets)[0].Get<uint32>();
                        Pet::DeleteFromDB(petguidlow);
                    } while (resultPets->NextRow());
                }

                // Delete char from social list of online chars
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_SOCIAL);
                stmt->SetData(0, lowGuid);
                PreparedQueryResult resultFriends = CharacterDatabase.Query(stmt);

                if (resultFriends)
                {
                    do
                    {
                        if (Player* pFriend = ObjectAccessor::FindPlayerByLowGUID((*resultFriends)[0].Get<uint32>()))
                        {
                            pFriend->GetSocial()->RemoveFromSocialList(playerGuid, SOCIAL_FLAG_ALL);
                            sSocialMgr->SendFriendStatus(pFriend, FRIEND_REMOVED, playerGuid, false);
                        }
                    } while (resultFriends->NextRow());
                }

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHARACTER);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_ACCOUNT_DATA);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_DECLINED_NAME);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_ACTION);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_AURA);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_GIFT);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_HOMEBIND);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INSTANCE);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INVENTORY);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS_REWARDED);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_REPUTATION);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SPELL);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SPELL_COOLDOWN);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                if (sWorld->getBoolConfig(CONFIG_DELETE_CHARACTER_TICKET_TRACE))
                {
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_PLAYER_GM_TICKETS_ON_CHAR_DELETION);
                    stmt->SetData(0, lowGuid);
                    trans->Append(stmt);
                }
                else
                {
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_GM_TICKETS);
                    stmt->SetData(0, lowGuid);
                    trans->Append(stmt);
                }

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE_BY_OWNER);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SOCIAL_BY_FRIEND);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SOCIAL_BY_GUID);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEMS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_PET_BY_OWNER);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_PET_DECLINEDNAME_BY_OWNER);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_ACHIEVEMENTS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_ACHIEVEMENT_PROGRESS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_EQUIPMENTSETS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GUILD_EVENTLOG_BY_PLAYER);
                stmt->SetData(0, lowGuid);
                stmt->SetData(1, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GUILD_BANK_EVENTLOG_BY_PLAYER);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_ENTRY_POINT);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_GLYPHS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_DAILY_CHAR);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_WEEKLY_CHAR);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_MONTHLY_CHAR);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_SEASONAL_CHAR);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_TALENT);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SKILLS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_SETTINGS);
                stmt->SetData(0, lowGuid);
                trans->Append(stmt);

                Corpse::DeleteFromDB(playerGuid, trans);

                sScriptMgr->OnPlayerDeleteFromDB(trans, lowGuid);

                CharacterDatabase.CommitTransaction(trans);
                break;
            }
        // The character gets unlinked from the account, the name gets freed up and appears as deleted ingame
        case CHAR_DELETE_UNLINK:
            {
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_DELETE_INFO);

                stmt->SetData(0, lowGuid);

                CharacterDatabase.Execute(stmt);
                break;
            }
        default:
            LOG_ERROR("entities.player", "Player::DeleteFromDB: Unsupported delete method: {}.", charDelete_method);
            return;
    }

    if (CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(playerGuid))
    {
        std::string name = cache->Name;
        sCharacterCache->DeleteCharacterCacheEntry(playerGuid, name);
    }

    if (updateRealmChars)
    {
        sWorld->UpdateRealmCharCount(accountId);
    }
}

/**
 * Characters which were kept back in the database after being deleted and are now too old (see config option "CharDelete.KeepDays"), will be completely deleted.
 */
void Player::DeleteOldCharacters()
{
    uint32 keepDays = sWorld->getIntConfig(CONFIG_CHARDELETE_KEEP_DAYS);
    if (!keepDays)
        return;

    Player::DeleteOldCharacters(keepDays);
}

/**
 * Characters which were kept back in the database after being deleted and are older than the specified amount of days, will be completely deleted.
 */
void Player::DeleteOldCharacters(uint32 keepDays)
{
    LOG_INFO("server.loading", "Player::DeleteOldChars: Deleting all characters which have been deleted {} days before...", keepDays);
    LOG_INFO("server.loading", " ");

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_OLD_CHARS);
    stmt->SetData(0, uint32(GameTime::GetGameTime().count() - time_t(keepDays * DAY)));
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (result)
    {
        LOG_INFO("server.loading", "Player::DeleteOldChars: Found {} character(s) to delete", result->GetRowCount());
        do
        {
            Field* fields = result->Fetch();
            Player::DeleteFromDB(fields[0].Get<uint32>(), fields[1].Get<uint32>(), true, true);
        } while (result->NextRow());
    }
}

/**
 * Items which were kept back in the database after being deleted and are now too old (see config option "ItemDelete.KeepDays"), will be completely deleted.
 */


/**
 * Items which were kept back in the database after being deleted and are older than the specified amount of days, will be completely deleted.
 */


/* Preconditions:
  - a resurrectable corpse must not be loaded for the player (only bones)
  - the player must be in world
*/
void Player::BuildPlayerRepop()
{
    WorldPacket data(SMSG_PRE_RESURRECT, GetPackGUID().size());
    data << GetPackGUID();
    SendDirectMessage(&data);
    if (getRace(true) == RACE_NIGHTELF)
    {
        CastSpell(this, 20584, true);
    }
    CastSpell(this, 8326, true);

    // there must be SMSG.FORCE_RUN_SPEED_CHANGE, SMSG.FORCE_SWIM_SPEED_CHANGE, SMSG.MOVE_WATER_WALK
    // there must be SMSG.STOP_MIRROR_TIMER

    // the player cannot have a corpse already on current map, only bones which are not returned by GetCorpse
    WorldLocation corpseLocation = GetCorpseLocation();
    if (GetCorpse() && corpseLocation.GetMapId() == GetMapId())
    {
        LOG_ERROR("entities.player", "BuildPlayerRepop: player {} ({}) already has a corpse", GetName(), GetGUID().ToString());
        return;
    }

    // create a corpse and place it at the player's location
    Corpse* corpse = CreateCorpse();
    if (!corpse)
    {
        LOG_ERROR("entities.player", "Error creating corpse for Player {} [{}]", GetName(), GetGUID().ToString());
        return;
    }
    GetMap()->AddToMap(corpse);
    SetHealth(1); // convert player body to ghost
    SetWaterWalking(true);

    if (!IsImmobilizedState())
        SendMoveRoot(false);

    RemoveUnitFlag(UNIT_FLAG_SKINNABLE); // BG - remove insignia related
    int32 corpseReclaimDelay = CalculateCorpseReclaimDelay();
    if (corpseReclaimDelay >= 0)
    {
        SendCorpseReclaimDelay(corpseReclaimDelay);
    }
    corpse->ResetGhostTime(); // to prevent cheating
    StopMirrorTimers(); // disable timers on bars
    SetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_ANIM_TIER, UNIT_BYTE1_FLAG_ALWAYS_STAND); // set and clear other
    sScriptMgr->OnPlayerReleasedGhost(this);
}

void Player::ResurrectPlayer(float restore_percent, bool applySickness)
{
    if (!sScriptMgr->OnPlayerCanResurrect(this))
        return;

    WorldPacket data(SMSG_DEATH_RELEASE_LOC, 4 * 4);        // remove spirit healer position
    data << uint32(-1);
    data << float(0);
    data << float(0);
    data << float(0);
    SendDirectMessage(&data);

    // speed change, land walk

    // remove death flag + set aura
    SetByteValue(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_ANIM_TIER, UNIT_BYTE1_FLAG_GROUND);
    RemoveAurasDueToSpell(20584);                           // speed bonuses
    RemoveAurasDueToSpell(8326);                            // SPELL_AURA_GHOST

    if (GetSession()->IsARecruiter() || (GetSession()->GetRecruiterId() != 0))
        SetDynamicFlag(UNIT_DYNFLAG_REFER_A_FRIEND);

    setDeathState(DeathState::Alive);
    SendMoveRoot(false);
    SetWaterWalking(false);
    m_deathTimer = 0;

    // set health/powers (0- will be set in caller)
    if (restore_percent > 0.0f)
    {
        SetHealth(uint32(GetMaxHealth()*restore_percent));
        SetPower(POWER_MANA, uint32(GetMaxPower(POWER_MANA)*restore_percent));
        SetPower(POWER_RAGE, 0);
        SetPower(POWER_ENERGY, uint32(GetMaxPower(POWER_ENERGY)*restore_percent));
    }

    // trigger update zone for alive state zone updates
    uint32 newzone, newarea;
    GetZoneAndAreaId(newzone, newarea);
    UpdateZone(newzone, newarea, true);
    sOutdoorPvPMgr->HandlePlayerResurrects(this, newzone);

    if (Battleground* bg = GetBattleground())
        bg->HandlePlayerResurrect(this);

    // update visibility
    UpdateObjectVisibility();

    // recast lost by death auras of any items held in the inventory
    CastAllObtainSpells();

    sScriptMgr->OnPlayerResurrect(this, restore_percent, applySickness);

    if (!applySickness)
    {
        return;
    }

    //Characters from level 1-10 are not affected by resurrection sickness.
    //Characters from level 11-19 will suffer from one minute of sickness
    //for each level they are above 10.
    //Characters level 20 and up suffer from ten minutes of sickness.
    int32 startLevel = sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL);

    if (int32(GetLevel()) >= startLevel)
    {
        // set resurrection sickness
        CastSpell(this, 15007, true);

        // not full duration
        if (int32(GetLevel()) < startLevel + 9)
        {
            int32 delta = (int32(GetLevel()) - startLevel + 1) * MINUTE;

            if (Aura* aur = GetAura(15007, GetGUID()))
            {
                aur->SetDuration(delta * IN_MILLISECONDS);
            }
        }
    }
}

void Player::KillPlayer()
{
    if (IsFlying() && !GetTransport())
        GetMotionMaster()->MoveFall();

    SendMoveRoot(true);

    StopMirrorTimers();                                     //disable timers(bars)

    setDeathState(DeathState::Corpse);
    //SetUnitFlag(UNIT_FLAG_NOT_IN_PVP);

    ReplaceAllDynamicFlags(UNIT_DYNFLAG_NONE);
    ApplyModFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_RELEASE_TIMER, !sMapStore.LookupEntry(GetMapId())->Instanceable() && !HasPreventResurectionAura());

    // 6 minutes until repop at graveyard
    m_deathTimer = 6 * MINUTE * IN_MILLISECONDS;

    UpdateCorpseReclaimDelay();                             // dependent at use SetDeathPvP() call before kill

    int32 corpseReclaimDelay = CalculateCorpseReclaimDelay();

    if (corpseReclaimDelay >= 0)
        SendCorpseReclaimDelay(corpseReclaimDelay);

    sScriptMgr->OnPlayerJustDied(this);
    // don't create corpse at this moment, player might be falling

    // update visibility
    //UpdateObjectVisibility(); // pussywizard: not needed
}

void Player::OfflineResurrect(ObjectGuid const& guid, CharacterDatabaseTransaction trans)
{
    Corpse::DeleteFromDB(guid, trans);
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ADD_AT_LOGIN_FLAG);
    stmt->SetData(0, uint16(AT_LOGIN_RESURRECT));
    stmt->SetData(1, guid.GetCounter());
    CharacterDatabase.ExecuteOrAppend(trans, stmt);
}

Corpse* Player::CreateCorpse()
{
    // prevent existence 2 corpse for player
    SpawnCorpseBones();

    uint32 _uf, _pb, _pb2, _cfb1, _cfb2;

    Corpse* corpse = new Corpse((m_ExtraFlags & PLAYER_EXTRA_PVP_DEATH) ? CORPSE_RESURRECTABLE_PVP : CORPSE_RESURRECTABLE_PVE);
    SetPvPDeath(false);

    if (!corpse->Create(GetMap()->GenerateLowGuid<HighGuid::Corpse>(), this))
    {
        delete corpse;
        return nullptr;
    }

    _corpseLocation.WorldRelocate(*this);

    _uf = getRace();
    _pb = GetUInt32Value(PLAYER_BYTES);
    _pb2 = GetUInt32Value(PLAYER_BYTES_2);

    uint8 race       = (uint8)(_uf);
    uint8 skin       = (uint8)(_pb);
    uint8 face       = (uint8)(_pb >> 8);
    uint8 hairstyle  = (uint8)(_pb >> 16);
    uint8 haircolor  = (uint8)(_pb >> 24);
    uint8 facialhair = (uint8)(_pb2);

    _cfb1 = ((0x00) | (race << 8) | (GetByteValue(PLAYER_BYTES_3, 0) << 16) | (skin << 24));
    _cfb2 = ((face) | (hairstyle << 8) | (haircolor << 16) | (facialhair << 24));

    corpse->SetUInt32Value(CORPSE_FIELD_BYTES_1, _cfb1);
    corpse->SetUInt32Value(CORPSE_FIELD_BYTES_2, _cfb2);

    uint32 flags = CORPSE_FLAG_UNK2;
    if (HasPlayerFlag(PLAYER_FLAGS_HIDE_HELM))
        flags |= CORPSE_FLAG_HIDE_HELM;
    if (HasPlayerFlag(PLAYER_FLAGS_HIDE_CLOAK))
        flags |= CORPSE_FLAG_HIDE_CLOAK;

    // Xinef: Player can loop corpses while in BG or in WG
    if (InBattleground() && !InArena())
        flags |= CORPSE_FLAG_LOOTABLE;
    Battlefield* Bf = sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG);
    if (Bf && Bf->IsWarTime())
        flags |= CORPSE_FLAG_LOOTABLE;

    corpse->SetUInt32Value(CORPSE_FIELD_FLAGS, flags);

    corpse->SetUInt32Value(CORPSE_FIELD_DISPLAY_ID, GetNativeDisplayId());

    corpse->SetUInt32Value(CORPSE_FIELD_GUILD, GetGuildId());

    uint32 iDisplayID;
    uint32 iIventoryType;
    uint32 _cfi;
    for (uint8 i = 0; i < EQUIPMENT_SLOT_END; i++)
    {
        if (m_items[i])
        {
            iDisplayID = m_items[i]->GetTemplate()->DisplayInfoID;
            iIventoryType = m_items[i]->GetTemplate()->InventoryType;

            _cfi = iDisplayID | (iIventoryType << 24);
            corpse->SetUInt32Value(CORPSE_FIELD_ITEM + i, _cfi);
        }
    }

    // register for player, but not show
    GetMap()->AddCorpse(corpse);

    UpdatePositionData();

    // we do not need to save corpses for BG/arenas
    if (!GetMap()->IsBattlegroundOrArena())
        corpse->SaveToDB();

    return corpse;
}

void Player::RemoveCorpse()
{
    if (GetCorpse())
    {
        GetCorpse()->RemoveFromWorld();
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    Corpse::DeleteFromDB(GetGUID(), trans);
    CharacterDatabase.CommitTransaction(trans);

    _corpseLocation.WorldRelocate();
}

void Player::SpawnCorpseBones(bool triggerSave /*= true*/)
{
    _corpseLocation.WorldRelocate();
    if (GetMap()->ConvertCorpseToBones(GetGUID()))
        if (triggerSave && !GetSession()->PlayerLogoutWithSave())   // at logout we will already store the player
        {
            // prevent loading as ghost without corpse
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            // pussywizard: update only ghost flag instead of whole character table entry! data integrity is crucial
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_REMOVE_GHOST);
            stmt->SetData(0, GetGUID().GetRawValue());
            trans->Append(stmt);

            _SaveAuras(trans, false);

            CharacterDatabase.CommitTransaction(trans);
        }
}

Corpse* Player::GetCorpse() const
{
    return GetMap()->GetCorpseByPlayer(GetGUID());
}

















void Player::RepopAtGraveyard()
{
    // note: this can be called also when the player is alive
    // for example from WorldSession::HandleMovementOpcodes

    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(GetAreaId());

    if (!sScriptMgr->OnPlayerCanRepopAtGraveyard(this))
        return;

    // Such zones are considered unreachable as a ghost and the player must be automatically revived
    // Xinef: Get Transport Check is not needed
    if ((!IsAlive() && zone && zone->flags & AREA_FLAG_NEED_FLY) /*|| GetTransport()*/ || GetPositionZ() < GetMap()->GetMinHeight(GetPositionX(), GetPositionY()))
    {
        ResurrectPlayer(0.5f);
        SpawnCorpseBones();
    }

    GraveyardStruct const* ClosestGrave = nullptr;

    // Special handle for battleground maps
    if (Battleground* bg = GetBattleground())
        ClosestGrave = bg->GetClosestGraveyard(this);
    else
    {
        if (sBattlefieldMgr->GetBattlefieldToZoneId(GetZoneId()))
            ClosestGrave = sBattlefieldMgr->GetBattlefieldToZoneId(GetZoneId())->GetClosestGraveyard(this);
        else
            ClosestGrave = sGraveyard->GetClosestGraveyard(this, GetTeamId());
    }

    // stop countdown until repop
    m_deathTimer = 0;

    // if no grave found, stay at the current location
    // and don't show spirit healer location
    if (ClosestGrave)
    {
        TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, GetOrientation());
        if (isDead())                                        // not send if alive, because it used in TeleportTo()
        {
            WorldPacket data(SMSG_DEATH_RELEASE_LOC, 4 * 4); // show spirit healer position on minimap
            data << ClosestGrave->Map;
            data << ClosestGrave->x;
            data << ClosestGrave->y;
            data << ClosestGrave->z;
            SendDirectMessage(&data);
        }
    }
    else if (GetPositionZ() < GetMap()->GetMinHeight(GetPositionX(), GetPositionY()))
        TeleportTo(m_homebindMapId, m_homebindX, m_homebindY, m_homebindZ, GetOrientation());

    RemovePlayerFlag(PLAYER_FLAGS_IS_OUT_OF_BOUNDS);
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

void Player::HandleBaseModFlatValue(BaseModGroup modGroup, float amount, bool apply)
{
    if (modGroup >= BASEMOD_END)
    {
        LOG_ERROR("entities.player", "Player::HandleBaseModFlatValue: Invalid BaseModGroup/BaseModType ({}/{}) for player '{}' ({})",
            modGroup, FLAT_MOD, GetName(), GetGUID().ToString());
        return;
    }

    m_auraBaseFlatMod[modGroup] += apply ? amount : -amount;
    UpdateBaseModGroup(modGroup);
}

void Player::ApplyBaseModPctValue(BaseModGroup modGroup, float pct)
{
    if (modGroup >= BASEMOD_END)
    {
        LOG_ERROR("entities.player", "Player::ApplyBaseModPctValue: Invalid BaseModGroup/BaseModType ({}/{}) for player '{}' ({})",
            modGroup, PCT_MOD, GetName(), GetGUID().ToString());
        return;
    }

    m_auraBasePctMod[modGroup] += CalculatePct(1.0f, pct);
    UpdateBaseModGroup(modGroup);
}

void Player::SetBaseModFlatValue(BaseModGroup modGroup, float val)
{
    if (m_auraBaseFlatMod[modGroup] == val)
        return;

    m_auraBaseFlatMod[modGroup] = val;
    UpdateBaseModGroup(modGroup);
}

void Player::SetBaseModPctValue(BaseModGroup modGroup, float val)
{
    if (m_auraBasePctMod[modGroup] == val)
        return;

    m_auraBasePctMod[modGroup] = val;
    UpdateBaseModGroup(modGroup);
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

float Player::GetBaseModValue(BaseModGroup modGroup, BaseModType modType) const
{
    if (modGroup >= BASEMOD_END)
    {
        LOG_ERROR("entities.player", "trial to access non existed BaseModGroup!");
        return 0.0f;
    }

    return (modType == FLAT_MOD ? m_auraBaseFlatMod[modGroup] : m_auraBasePctMod[modGroup]);
}

float Player::GetTotalBaseModValue(BaseModGroup modGroup) const
{
    if (modGroup >= BASEMOD_END)
    {
        LOG_ERROR("entities.player", "wrong BaseModGroup in GetTotalBaseModValue()!");
        return 0.0f;
    }

    if (m_auraBasePctMod[modGroup] <= 0.0f)
        return 0.0f;

    return m_auraBaseFlatMod[modGroup] * m_auraBasePctMod[modGroup];
}

























// This functions sets a skill line value (and adds if doesn't exist yet)
// To "remove" a skill line, set it's values to zero




















void Player::SendActionButtons(uint32 state) const
{
    LOG_DEBUG("entities.player", "Sending Action Buttons for {} spec {}", GetGUID().ToString(), m_activeSpec);

    WorldPacket data(SMSG_ACTION_BUTTONS, 1 + (MAX_ACTION_BUTTONS * 4));
    data << uint8(state);
    /*
        state can be 0, 1, 2
        0 - Looks to be sent when initial action buttons get sent, however on Trinity we use 1 since 0 had some difficulties
        1 - Used in any SMSG_ACTION_BUTTONS packet with button data on Trinity. Only used after spec swaps on retail.
        2 - Clears the action bars client sided. This is sent during spec swap before unlearning and before sending the new buttons
    */
    if (state != 2)
    {
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        {
            ActionButtonList::const_iterator itr = m_actionButtons.find(button);
            if (itr != m_actionButtons.end() && itr->second.uState != ACTIONBUTTON_DELETED)
                data << uint32(itr->second.packedData);
            else
                data << uint32(0);
        }
    }

    SendDirectMessage(&data);
    LOG_DEBUG("entities.player", "Action Buttons for {} spec {} Sent", GetGUID().ToString(), m_activeSpec);
}

bool Player::IsActionButtonDataValid(uint8 button, uint32 action, uint8 type)
{
    if (button >= MAX_ACTION_BUTTONS)
    {
        LOG_ERROR("entities.player", "Action {} not added into button {} for player {}: button must be < {}", action, button, GetName(), MAX_ACTION_BUTTONS);
        return false;
    }

    if (action >= MAX_ACTION_BUTTON_ACTION_VALUE)
    {
        LOG_ERROR("entities.player", "Action {} not added into button {} for player {}: action must be < {}", action, button, GetName(), MAX_ACTION_BUTTON_ACTION_VALUE);
        return false;
    }

    switch (type)
    {
        case ACTION_BUTTON_SPELL:
            if (!sSpellMgr->GetSpellInfo(action))
            {
                LOG_ERROR("entities.player", "Spell action {} not added into button {} for player {}: spell not exist", action, button, GetName());
                return false;
            }

            if (!HasSpell(action))
            {
                LOG_DEBUG("entities.player.loading", "Player::IsActionButtonDataValid Spell action {} not added into button {} for player {}: player don't known this spell", action, button, GetName());
                return false;
            }
            break;
        case ACTION_BUTTON_ITEM:
            if (!sObjectMgr->GetItemTemplate(action))
            {
                LOG_ERROR("entities.player", "Item action {} not added into button {} for player {}: item not exist", action, button, GetName());
                return false;
            }
            break;
        default:
            break;                                          // other cases not checked at this moment
    }

    return true;
}

ActionButton* Player::addActionButton(uint8 button, uint32 action, uint8 type)
{
    if (!IsActionButtonDataValid(button, action, type))
        return nullptr;

    // it create new button (NEW state) if need or return existed
    ActionButton& ab = m_actionButtons[button];

    // set data and update to CHANGED if not NEW
    ab.SetActionAndType(action, ActionButtonType(type));

    LOG_DEBUG("entities.player", "Player {} Added Action {} (type {}) to Button {}", GetGUID().ToString(), action, type, button);
    return &ab;
}

void Player::removeActionButton(uint8 button)
{
    ActionButtonList::iterator buttonItr = m_actionButtons.find(button);
    if (buttonItr == m_actionButtons.end() || buttonItr->second.uState == ACTIONBUTTON_DELETED)
        return;

    if (buttonItr->second.uState == ACTIONBUTTON_NEW)
        m_actionButtons.erase(buttonItr);                   // new and not saved
    else
        buttonItr->second.uState = ACTIONBUTTON_DELETED;    // saved, will deleted at next save

    LOG_DEBUG("entities.player", "Action Button {} Removed from Player {}", button, GetGUID().ToString());
}

ActionButton const* Player::GetActionButton(uint8 button)
{
    ActionButtonList::iterator buttonItr = m_actionButtons.find(button);
    if (buttonItr == m_actionButtons.end() || buttonItr->second.uState == ACTIONBUTTON_DELETED)
        return nullptr;

    return &buttonItr->second;
}

void Player::SaveRecallPosition()
{
    m_recallMap = GetMapId();
    m_recallX = GetPositionX();
    m_recallY = GetPositionY();
    m_recallZ = GetPositionZ();
    m_recallO = GetOrientation();
}

void Player::SendMessageToSet(WorldPacket const* data, bool self) const
{
    SendMessageToSetInRange(data, GetVisibilityRange(), self);
}

void Player::SendMessageToSetInRange(WorldPacket const* data, float dist, bool self) const
{
    if (self)
        SendDirectMessage(data);

    Acore::MessageDistDeliverer notifier(this, data, dist);
    notifier.Visit(GetObjectVisibilityContainer().GetVisiblePlayersMap());
}

void Player::SendMessageToSet(WorldPacket const* data, Player const* skipped_rcvr) const
{
    if (skipped_rcvr != this)
        SendDirectMessage(data);

    Acore::MessageDistDeliverer notifier(this, data, 0.0f, Acore::TeamFilter::All, skipped_rcvr);
    notifier.Visit(GetObjectVisibilityContainer().GetVisiblePlayersMap());
}

void Player::SendDirectMessage(WorldPacket const* data) const
{
    m_session->SendPacket(data);
}

void Player::SendCinematicStart(uint32 CinematicSequenceId) const
{
    WorldPacket data(SMSG_TRIGGER_CINEMATIC, 4);
    data << uint32(CinematicSequenceId);
    SendDirectMessage(&data);
}

void Player::SendMovieStart(uint32 MovieId)
{
    WorldPacket data(SMSG_TRIGGER_MOVIE, 4);
    data << uint32(MovieId);
    SendDirectMessage(&data);
}



TeamId Player::TeamIdForRace(uint8 race)
{
    if (ChrRacesEntry const* rEntry = sChrRacesStore.LookupEntry(race))
    {
        switch (rEntry->TeamID)
        {
            case 1:
                return TEAM_HORDE;
            case 7:
                return TEAM_ALLIANCE;
        }
        LOG_ERROR("entities.player", "Race ({}) has wrong teamid ({}) in DBC: wrong DBC files?", uint32(race), rEntry->TeamID);
    }
    else
        LOG_ERROR("entities.player", "Race ({}) not found in DBC: wrong DBC files?", uint32(race));

    return TEAM_ALLIANCE;
}

void Player::SetFactionForRace(uint8 race)
{
    m_team = TeamIdForRace(race);

    sScriptMgr->OnPlayerUpdateFaction(this);

    if (GetTeamId(true) != GetTeamId())
        return;

    ChrRacesEntry const* rEntry = sChrRacesStore.LookupEntry(race);
    SetFaction(rEntry ? rEntry->FactionID : 0);
}

ReputationRank Player::GetReputationRank(uint32 faction) const
{
    FactionEntry const* factionEntry = sFactionStore.LookupEntry(faction);
    return GetReputationMgr().GetRank(factionEntry);
}

// Calculate total reputation percent player gain with quest/creature level
float Player::CalculateReputationGain(ReputationSource source, uint32 creatureOrQuestLevel, float rep, int32 faction, bool noQuestBonus)
{
    float percent = 100.0f;

    float repMod = noQuestBonus ? 0.0f : float(GetTotalAuraModifier(SPELL_AURA_MOD_REPUTATION_GAIN));

    // faction specific auras only seem to apply to kills
    if (source == REPUTATION_SOURCE_KILL)
        repMod += GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_FACTION_REPUTATION_GAIN, faction);

    percent += rep > 0.f ? repMod : -repMod;

    float rate;
    switch (source)
    {
        case REPUTATION_SOURCE_KILL:
            rate = sWorld->getRate(RATE_REPUTATION_LOWLEVEL_KILL);
            break;
        case REPUTATION_SOURCE_QUEST:
        case REPUTATION_SOURCE_DAILY_QUEST:
        case REPUTATION_SOURCE_WEEKLY_QUEST:
        case REPUTATION_SOURCE_MONTHLY_QUEST:
        case REPUTATION_SOURCE_REPEATABLE_QUEST:
            rate = sWorld->getRate(RATE_REPUTATION_LOWLEVEL_QUEST);
            break;
        case REPUTATION_SOURCE_SPELL:
        default:
            rate = 1.0f;
            break;
    }

    if (rate != 1.0f && creatureOrQuestLevel <= Acore::XP::GetGrayLevel(GetLevel()))
        percent *= rate;

    if (percent <= 0.0f)
        return 0;

    // Multiply result with the faction specific rate
    if (RepRewardRate const* repData = sObjectMgr->GetRepRewardRate(faction))
    {
        float repRate = 0.0f;
        switch (source)
        {
            case REPUTATION_SOURCE_KILL:
                repRate = repData->creatureRate;
                break;
            case REPUTATION_SOURCE_QUEST:
                repRate = repData->questRate;
                break;
            case REPUTATION_SOURCE_DAILY_QUEST:
                repRate = repData->questDailyRate;
                break;
            case REPUTATION_SOURCE_WEEKLY_QUEST:
                repRate = repData->questWeeklyRate;
                break;
            case REPUTATION_SOURCE_MONTHLY_QUEST:
                repRate = repData->questMonthlyRate;
                break;
            case REPUTATION_SOURCE_REPEATABLE_QUEST:
                repRate = repData->questRepeatableRate;
                break;
            case REPUTATION_SOURCE_SPELL:
                repRate = repData->spellRate;
                break;
        }

        // for custom, a rate of 0.0 will totally disable reputation gain for this faction/type
        if (repRate <= 0.0f)
            return 0;

        percent *= repRate;
    }

    if (source != REPUTATION_SOURCE_SPELL && GetsRecruitAFriendBonus(false))
        percent *= 1.0f + sWorld->getRate(RATE_REPUTATION_RECRUIT_A_FRIEND_BONUS);

    return CalculatePct(rep, percent);
}

// Calculates how many reputation points player gains in victim's enemy factions
void Player::RewardReputation(Unit* victim)
{
    if (!victim || victim->IsPlayer())
        return;

    if (victim->ToCreature()->IsReputationRewardDisabled())
        return;

    ReputationOnKillEntry const* Rep = sObjectMgr->GetReputationOnKilEntry(victim->ToCreature()->GetCreatureTemplate()->Entry);
    if (!Rep)
        return;

    uint32 ChampioningFaction = 0;

    if (GetChampioningFaction())
    {
        // support for: Championing - http://www.wowwiki.com/Championing
        Map const* map = GetMap();
        if (map->IsNonRaidDungeon())
            if (LFGDungeonEntry const* dungeon = GetLFGDungeon(map->GetId(), map->GetDifficulty()))
                if (dungeon->TargetLevel == 80)
                    ChampioningFaction = GetChampioningFaction();
    }

    TeamId teamId = GetTeamId(true); // Always check player original reputation when rewarding

    if (Rep->RepFaction1 && (!Rep->TeamDependent || teamId == TEAM_ALLIANCE))
    {
        float donerep1 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->GetLevel(), static_cast<float>(Rep->RepValue1), ChampioningFaction ? ChampioningFaction : Rep->RepFaction1);
        sScriptMgr->OnPlayerGiveReputation(this, Rep->RepFaction1, donerep1, REPUTATION_SOURCE_KILL);

        FactionEntry const* factionEntry1 = sFactionStore.LookupEntry(ChampioningFaction ? ChampioningFaction : Rep->RepFaction1);
        if (factionEntry1)
        {
            GetReputationMgr().ModifyReputation(factionEntry1, donerep1, false, static_cast<ReputationRank>(Rep->ReputationMaxCap1));
        }
    }

    if (Rep->RepFaction2 && (!Rep->TeamDependent || teamId == TEAM_HORDE))
    {
        float donerep2 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->GetLevel(), static_cast<float>(Rep->RepValue2), ChampioningFaction ? ChampioningFaction : Rep->RepFaction2);
        sScriptMgr->OnPlayerGiveReputation(this, Rep->RepFaction2, donerep2, REPUTATION_SOURCE_KILL);

        FactionEntry const* factionEntry2 = sFactionStore.LookupEntry(ChampioningFaction ? ChampioningFaction : Rep->RepFaction2);
        if (factionEntry2)
        {
            GetReputationMgr().ModifyReputation(factionEntry2, donerep2, false, static_cast<ReputationRank>(Rep->ReputationMaxCap2));
        }
    }
}

FactionTemplateEntry const* GetAnyFactionTemplateForFaction(uint32 factionId)
{
    for (uint32 i = 0; i < sFactionTemplateStore.GetNumRows(); ++i)
    {
        if (FactionTemplateEntry const* factionTemplate = sFactionTemplateStore.LookupEntry(i))
        {
            if (factionTemplate->faction == factionId)
                return factionTemplate;
        }
    }
    return nullptr;
}

// Calculate how many reputation points player gain with the quest
void Player::RewardReputation(Quest const* quest)
{
    for (uint8 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
    {
        if (!quest->RewardFactionId[i])
            continue;

        float rep = 0.f;

        if (quest->RewardFactionValueIdOverride[i])
        {
            rep = quest->RewardFactionValueIdOverride[i] / 100.f;
        }
        else
        {
            uint32 row = ((quest->RewardFactionValueId[i] < 0) ? 1 : 0) + 1;
            if (QuestFactionRewEntry const* questFactionRewEntry = sQuestFactionRewardStore.LookupEntry(row))
            {
                uint32 field = std::abs(quest->RewardFactionValueId[i]);
                rep = static_cast<float>(questFactionRewEntry->QuestRewFactionValue[field]);
            }
        }

        if (rep == 0.f)
            continue;

        if (quest->IsDaily())
        {
            rep = CalculateReputationGain(REPUTATION_SOURCE_DAILY_QUEST, GetQuestLevel(quest), rep, quest->RewardFactionId[i], false);
            sScriptMgr->OnPlayerGiveReputation(this, quest->RewardFactionId[i], rep, REPUTATION_SOURCE_DAILY_QUEST);
        }
        else if (quest->IsWeekly())
        {
            rep = CalculateReputationGain(REPUTATION_SOURCE_WEEKLY_QUEST, GetQuestLevel(quest), rep, quest->RewardFactionId[i], false);
            sScriptMgr->OnPlayerGiveReputation(this, quest->RewardFactionId[i], rep, REPUTATION_SOURCE_WEEKLY_QUEST);
        }
        else if (quest->IsMonthly())
        {
            rep = CalculateReputationGain(REPUTATION_SOURCE_MONTHLY_QUEST, GetQuestLevel(quest), rep, quest->RewardFactionId[i], false);
            sScriptMgr->OnPlayerGiveReputation(this, quest->RewardFactionId[i], rep, REPUTATION_SOURCE_MONTHLY_QUEST);
        }
        else if (quest->IsRepeatable())
        {
            rep = CalculateReputationGain(REPUTATION_SOURCE_REPEATABLE_QUEST, GetQuestLevel(quest), rep, quest->RewardFactionId[i], false);
            sScriptMgr->OnPlayerGiveReputation(this, quest->RewardFactionId[i], rep, REPUTATION_SOURCE_REPEATABLE_QUEST);
        }
        else
        {
            rep = CalculateReputationGain(REPUTATION_SOURCE_QUEST, GetQuestLevel(quest), rep, quest->RewardFactionId[i], false);
            sScriptMgr->OnPlayerGiveReputation(this, quest->RewardFactionId[i], rep, REPUTATION_SOURCE_QUEST);
        }

        FactionEntry const* factionEntry = sFactionStore.LookupEntry(quest->RewardFactionId[i]);
        if (!factionEntry)
            continue;

        FactionTemplateEntry const* templateEntry = GetAnyFactionTemplateForFaction(factionEntry->ID);
        if (templateEntry)
        {
            bool hostile = (GetTeamId() == TEAM_ALLIANCE) ? templateEntry->IsHostileToAlliancePlayers()
                                                          : templateEntry->IsHostileToHordePlayers();

            if (hostile)
            {
                LOG_DEBUG("sql.sql", "RewardReputation: {} is hostile with player ({}), skipping!", templateEntry->ID, GetGUID().ToString());
                continue;
            }
        }

        GetReputationMgr().ModifyReputation(factionEntry, rep, quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_NO_REP_SPILLOVER));
    }
}



///Calculate the amount of honor gained based on the victim
///and the size of the group for which the honor is divided
///An exact honor value can also be given (overriding the calcs)
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

uint32 Player::GetZoneIdFromDB(ObjectGuid guid)
{
    ObjectGuid::LowType guidLow = guid.GetCounter();

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_ZONE);
    stmt->SetData(0, guidLow);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (!result)
        return 0;

    Field* fields = result->Fetch();
    uint32 zone = fields[0].Get<uint16>();

    if (!zone)
    {
        // stored zone is zero, use generic and slow zone detection
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_POSITION_XYZ);
        stmt->SetData(0, guidLow);
        PreparedQueryResult posResult = CharacterDatabase.Query(stmt);

        if (!posResult)
        {
            return 0;
        }

        fields = posResult->Fetch();
        uint32 map = fields[0].Get<uint16>();
        float posx = fields[1].Get<float>();
        float posy = fields[2].Get<float>();
        float posz = fields[3].Get<float>();

        if (!sMapStore.LookupEntry(map))
            return 0;

        zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, map, posx, posy, posz);

        if (zone > 0)
        {
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ZONE);

            stmt->SetData(0, uint16(zone));
            stmt->SetData(1, guidLow);

            CharacterDatabase.Execute(stmt);
        }
    }

    return zone;
}

//If players are too far away from the duel flag... they lose the duel
void Player::CheckDuelDistance(time_t currTime)
{
    if (!duel)
    {
        return;
    }

    ObjectGuid duelFlagGUID = GetGuidValue(PLAYER_DUEL_ARBITER);
    GameObject* obj = GetMap()->GetGameObject(duelFlagGUID);
    if (!obj)
        return;

    if (!duel->OutOfBoundsTime)
    {
        if (!IsWithinDistInMap(obj, 50))
        {
            duel->OutOfBoundsTime = currTime + 10;

            WorldPacket data(SMSG_DUEL_OUTOFBOUNDS, 0);
            SendDirectMessage(&data);
        }
    }
    else
    {
        if (IsWithinDistInMap(obj, 40))
        {
            duel->OutOfBoundsTime = 0;

            WorldPacket data(SMSG_DUEL_INBOUNDS, 0);
            SendDirectMessage(&data);
        }
        else if (currTime >= duel->OutOfBoundsTime)
            DuelComplete(DUEL_FLED);
    }
}

bool Player::IsOutdoorPvPActive()
{
    return IsAlive() && !HasInvisibilityAura() && !HasStealthAura() && IsPvP() && !HasUnitMovementFlag(MOVEMENTFLAG_FLYING) && !IsInFlight();
}

void Player::DuelComplete(DuelCompleteType type)
{
    // duel not requested
    if (!duel)
        return;

    // Check if DuelComplete() has been called already up in the stack and in that case don't do anything else here
    if (duel->State == DUEL_STATE_COMPLETED)
        return;

    Player* opponent      = duel->Opponent;
    duel->State           = DUEL_STATE_COMPLETED;
    opponent->duel->State = DUEL_STATE_COMPLETED;

    LOG_DEBUG("entities.unit", "Player::DuelComplete: Player '{}' ({}), Opponent: '{}' ({})", GetName(), GetGUID().ToString(), opponent->GetName(), opponent->GetGUID().ToString());

    WorldPacket data(SMSG_DUEL_COMPLETE, (1));
    data << uint8((type != DUEL_INTERRUPTED) ? 1 : 0);
    SendDirectMessage(&data);
    if (opponent->GetSession())
    {
        opponent->SendDirectMessage(&data);
    }

    if (type != DUEL_INTERRUPTED)
    {
        data.Initialize(SMSG_DUEL_WINNER, (1 + 20)); // we guess size
        data << uint8(type == DUEL_WON ? 0 : 1);     // 0 = just won; 1 = fled
        data << opponent->GetName();
        data << GetName();
        SendMessageToSet(&data, true);
    }

    sScriptMgr->OnPlayerDuelEnd(opponent, this, type);

    switch (type)
    {
    case DUEL_FLED:
        // if initiator and opponent are on the same team
        // or initiator and opponent are not PvP enabled, forcibly stop attacking
        if (GetTeamId() == opponent->GetTeamId())
        {
            AttackStop();
            opponent->AttackStop();
        }
        else
        {
            if (!IsPvP())
            {
                AttackStop();
            }
            if (!opponent->IsPvP())
            {
                opponent->AttackStop();
            }
        }
        break;
    case DUEL_WON:
        UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOSE_DUEL, 1);
        opponent->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_DUEL, 1);

        // Credit for quest Death's Challenge
        if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_QUEST) && opponent->GetQuestStatus(12733) == QUEST_STATUS_INCOMPLETE)
        {
            opponent->CastSpell(opponent, 52994, true);
        }

        // Honor points after duel (the winner) - ImpConfig
        if (uint32 amount = sWorld->getIntConfig(CONFIG_HONOR_AFTER_DUEL))
        {
            opponent->RewardHonor(nullptr, 1, amount);
        }

        break;
    default:
        break;
    }

    // Victory emote spell
    if (type != DUEL_INTERRUPTED)
    {
        opponent->CastSpell(opponent, 52852, true);
    }

    // Remove Duel Flag object
    GameObject* obj = GetMap()->GetGameObject(GetGuidValue(PLAYER_DUEL_ARBITER));
    if (obj)
    {
        duel->Initiator->RemoveGameObject(obj, true);
    }

    /* remove auras */
    AuraApplicationMap& itsAuras = opponent->GetAppliedAuras();
    for (AuraApplicationMap::iterator i = itsAuras.begin(); i != itsAuras.end();)
    {
        Aura const* aura = i->second->GetBase();
        if (!i->second->IsPositive() && aura->GetCasterGUID() == GetGUID() && aura->GetApplyTime() >= duel->StartTime)
        {
            opponent->RemoveAura(i);
        }
        else
        {
            ++i;
        }
    }

    AuraApplicationMap& myAuras = GetAppliedAuras();
    for (AuraApplicationMap::iterator i = myAuras.begin(); i != myAuras.end();)
    {
        Aura const* aura = i->second->GetBase();
        if (!i->second->IsPositive() && aura->GetCasterGUID() == opponent->GetGUID() && aura->GetApplyTime() >= duel->StartTime)
            RemoveAura(i);
        else
            ++i;
    }

    // cleanup combo points
    if (GetComboTarget() == duel->Opponent)
    {
        ClearComboPoints();
    }
    else if (GetComboTargetGUID() == duel->Opponent->GetPetGUID())
    {
        ClearComboPoints();
    }

    if (duel->Opponent->GetComboTarget() == this)
    {
        duel->Opponent->ClearComboPoints();
    }
    else if (duel->Opponent->GetComboTargetGUID() == GetPetGUID())
    {
        duel->Opponent->ClearComboPoints();
    }

    //cleanups
    SetGuidValue(PLAYER_DUEL_ARBITER, ObjectGuid::Empty);
    SetUInt32Value(PLAYER_DUEL_TEAM, 0);
    opponent->SetGuidValue(PLAYER_DUEL_ARBITER, ObjectGuid::Empty);
    opponent->SetUInt32Value(PLAYER_DUEL_TEAM, 0);

    opponent->duel.reset(nullptr);
    duel.reset(nullptr);
}

//---------------------------------------------------------//













// this one rechecks weapon auras and stores them in BaseModGroup container
// needed for things like axe specialization applying only to axe weapons in case of dual-wield
































void Player::SendQuestGiverStatusMultiple()
{
    if (GetObjectVisibilityContainer().GetVisibleWorldObjectsMap()->empty())
        return;

    uint32 count = 0;

    WorldPacket data(SMSG_QUESTGIVER_STATUS_MULTIPLE, 4);
    data << uint32(count); // placeholder

    DoForAllVisibleWorldObjects([this, &data, &count](WorldObject* worldObject)
    {
        uint32 questStatus = DIALOG_STATUS_NONE;

        if (worldObject->IsCreature())
        {
            // need also pet quests case support
            Creature* questgiver = worldObject->ToCreature();
            if (!questgiver || questgiver->IsHostileTo(this))
                return;
            if (!questgiver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return;

            questStatus = GetQuestDialogStatus(questgiver);

            data << questgiver->GetGUID();
            data << uint8(questStatus);
            ++count;
        }
        else if (worldObject->IsGameObject())
        {
            GameObject* questgiver = worldObject->ToGameObject();
            if (!questgiver || questgiver->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER)
                return;

            questStatus = GetQuestDialogStatus(questgiver);

            data << questgiver->GetGUID();
            data << uint8(questStatus);
            ++count;
        }
    });

    data.put<uint32>(0, count); // write real count
    SendDirectMessage(&data);
}

/*  If in a battleground a player dies, and an enemy removes the insignia, the player's bones is lootable
    Called by remove insignia spell effect    */
void Player::RemovedInsignia(Player* looterPlr)
{
    // Xinef: If player is not in battleground and not in wintergrasp
    if (!GetBattlegroundId() && GetZoneId() != AREA_WINTERGRASP)
        return;

    // If not released spirit, do it !
    if (m_deathTimer > 0)
    {
        m_deathTimer = 0;
        BuildPlayerRepop();
        RepopAtGraveyard();
    }

    _corpseLocation.WorldRelocate();

    // We have to convert player corpse to bones, not to be able to resurrect there
    // SpawnCorpseBones isn't handy, 'cos it saves player while he in BG
    Corpse* bones = GetMap()->ConvertCorpseToBones(GetGUID(), true);
    if (!bones)
        return;

    // Now we must make bones lootable, and send player loot
    bones->SetFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE);

    // We store the level of our player in the gold field
    // We retrieve this information at Player::SendLoot()
    bones->loot.gold = GetLevel();
    bones->lootRecipient = looterPlr;
    looterPlr->SendLoot(bones->GetGUID(), LOOT_INSIGNIA);
}











// TODO - InitWorldStates should NOT always send the same states
//        Some should keep the same value between different zoneIds and areaIds on the same map






uint32 Player::GetXPRestBonus(uint32 xp)
{
    uint32 rested_bonus = (uint32)GetRestBonus();           // xp for each rested bonus

    if (rested_bonus > xp)                                   // max rested_bonus == xp or (r+x) = 200% xp
        rested_bonus = xp;

    SetRestBonus(GetRestBonus() - rested_bonus);

    LOG_DEBUG("entities.player", "Player gain {} xp (+ {} Rested Bonus). Rested points={}", xp + rested_bonus, rested_bonus, GetRestBonus());
    return rested_bonus;
}

void Player::SetBindPoint(ObjectGuid guid)
{
    WorldPacket data(SMSG_BINDER_CONFIRM, 8);
    data << guid;
    SendDirectMessage(&data);
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



void Player::Say(std::string_view text, Language language, WorldObject const* /*= nullptr*/)
{
    std::string _text(text);
    if (!sScriptMgr->OnPlayerCanUseChat(this, CHAT_MSG_SAY, language, _text))
        return;

    if (sWorld->getBoolConfig(CONFIG_CHAT_FILTER_SAY) && IsChatFiltered(text))
    {
        ChatHandler(GetSession()).SendSysMessage(LANG_CHATFILTER_SAY);
        return;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SAY, language, this, this, _text);

    SendDirectMessage(&data);

    // Special handling for messages, do not use visibility map for stealthed units
    Acore::MessageDistDeliverer notifier(this, &data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), Acore::TeamFilter::All, nullptr, true);
    Cell::VisitObjects(this, notifier, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
}

void Player::Say(uint32 textId, WorldObject const* target /*= nullptr*/)
{
    Talk(textId, CHAT_MSG_SAY, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY), target);
}

void Player::Yell(std::string_view text, Language language, WorldObject const* /*= nullptr*/)
{
    std::string _text(text);

    if (!sScriptMgr->OnPlayerCanUseChat(this, CHAT_MSG_YELL, language, _text))
        return;

    if (sWorld->getBoolConfig(CONFIG_CHAT_FILTER_YELL) && IsChatFiltered(text))
    {
        ChatHandler(GetSession()).SendSysMessage(LANG_CHATFILTER_YELL);
        return;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_YELL, language, this, this, _text);

    SendDirectMessage(&data);

    // Special handling for messages, do not use visibility map for stealthed units
    Acore::MessageDistDeliverer notifier(this, &data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL), Acore::TeamFilter::All, nullptr, true);
    Cell::VisitObjects(this, notifier, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL));
}

void Player::Yell(uint32 textId, WorldObject const* target /*= nullptr*/)
{
    Talk(textId, CHAT_MSG_YELL, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_YELL), target);
}

void Player::TextEmote(std::string_view text, WorldObject const* /*= nullptr*/, bool /*= false*/)
{
    std::string _text(text);

    if (!sScriptMgr->OnPlayerCanUseChat(this, CHAT_MSG_EMOTE, LANG_UNIVERSAL, _text))
        return;

    if (sWorld->getBoolConfig(CONFIG_CHAT_FILTER_EMOTE) && IsChatFiltered(text))
    {
        ChatHandler(GetSession()).SendSysMessage(LANG_CHATFILTER_EMOTE);
        return;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_EMOTE, LANG_UNIVERSAL, this, this, _text);

    SendDirectMessage(&data);

    // Special handling for messages, do not use visibility map for stealthed units
    if (GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT))
    {
        Acore::MessageDistDeliverer notifier(this, &data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), Acore::TeamFilter::All, nullptr, true);
        Cell::VisitObjects(this, notifier, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));
    }
    else
    {
        // Same faction
        {
            Acore::MessageDistDeliverer notifier(this, &data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), Acore::TeamFilter::OwnTeam, nullptr, true);
            Cell::VisitObjects(this, notifier, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));
        }
        // Opposite faction
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_EMOTE, LANG_UNIVERSAL, this, this, "");

            Acore::MessageDistDeliverer notifier(this, &data, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), Acore::TeamFilter::OtherTeam, nullptr, true);
            Cell::VisitObjects(this, notifier, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));
        }
    }
}

void Player::TextEmote(uint32 textId, WorldObject const* target /*= nullptr*/, bool /*isBossEmote = false*/)
{
    Talk(textId, CHAT_MSG_EMOTE, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), target);
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

bool Player::IsChatFiltered(std::string_view text)
{
    return sObjectMgr->IsChatFiltered(text);
}









void Player::SendRemoveControlBar()
{
    WorldPacket data(SMSG_PET_SPELLS, 8);
    data << uint64(0);
    SendDirectMessage(&data);
}













// Restore spellmods in case of failed cast












// send Proficiency
void Player::SendProficiency(ItemClass itemClass, uint32 itemSubclassMask)
{
    WorldPacket data(SMSG_SET_PROFICIENCY, 1 + 4);
    data << uint8(itemClass) << uint32(itemSubclassMask);
    SendDirectMessage(&data);
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

void Player::SetRestBonus(float restBonusNew)
{
    // Prevent resting on max level
    if (GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        restBonusNew = 0;

    if (restBonusNew < 0)
        restBonusNew = 0;

    // Fetch rest bonus multiplier from cached configuration
    float restBonusMultiplier = sWorld->getRate(RATE_REST_MAX_BONUS);

    // Calculate rest bonus max using the multiplier
    float restBonusMax = (float)GetUInt32Value(PLAYER_NEXT_LEVEL_XP) * restBonusMultiplier / 2;

    if (restBonusNew > restBonusMax)
        _restBonus = restBonusMax;
    else
        _restBonus = restBonusNew;
    // update data for client
    if ((GetsRecruitAFriendBonus(true) && (GetSession()->IsARecruiter() || GetSession()->GetRecruiterId() != 0)))
        SetByteValue(PLAYER_BYTES_2, 3, REST_STATE_RAF_LINKED);
    else
    {
        if (_restBonus > 10)
            SetByteValue(PLAYER_BYTES_2, 3, REST_STATE_RESTED);
        else if (_restBonus <= 1)
            SetByteValue(PLAYER_BYTES_2, 3, REST_STATE_NOT_RAF_LINKED);
    }

    //RestTickUpdate
    SetUInt32Value(PLAYER_REST_STATE_EXPERIENCE, uint32(_restBonus));
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





void Player::InitDataForForm(bool reapplyMods)
{
    ShapeshiftForm form = GetShapeshiftForm();

    SpellShapeshiftFormEntry const* ssEntry = sSpellShapeshiftFormStore.LookupEntry(form);
    if (ssEntry && ssEntry->attackSpeed)
    {
        SetAttackTime(BASE_ATTACK, ssEntry->attackSpeed);
        SetAttackTime(OFF_ATTACK, ssEntry->attackSpeed);
        SetAttackTime(RANGED_ATTACK, BASE_ATTACK_TIME);
    }
    else
        SetRegularAttackTime();

    switch (form)
    {
        case FORM_GHOUL:
        case FORM_CAT:
            {
                if (getPowerType() != POWER_ENERGY)
                    setPowerType(POWER_ENERGY);
                break;
            }
        case FORM_BEAR:
        case FORM_DIREBEAR:
            {
                if (getPowerType() != POWER_RAGE)
                    setPowerType(POWER_RAGE);
                break;
            }
        default:                                            // 0, for example
            {
                ChrClassesEntry const* cEntry = sChrClassesStore.LookupEntry(getClass());
                if (cEntry && cEntry->powerType < MAX_POWERS && uint32(getPowerType()) != cEntry->powerType)
                    setPowerType(Powers(cEntry->powerType));
                break;
            }
    }

    // update auras at form change, ignore this at mods reapply (.reset stats/etc) when form not change.
    if (!reapplyMods)
        UpdateEquipSpellsAtFormChange();

    UpdateAttackPowerAndDamage();
    UpdateAttackPowerAndDamage(true);
}

void Player::InitDisplayIds()
{
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(true), getClass());
    if (!info)
    {
        LOG_ERROR("entities.player", "Player {} has incorrect race/class pair. Can't init display ids.", GetGUID().ToString());
        return;
    }

    uint8 gender = getGender();
    switch (gender)
    {
        case GENDER_FEMALE:
            SetDisplayId(info->displayId_f);
            SetNativeDisplayId(info->displayId_f);
            break;
        case GENDER_MALE:
            SetDisplayId(info->displayId_m);
            SetNativeDisplayId(info->displayId_m);
            break;
        default:
            LOG_ERROR("entities.player", "Invalid gender {} for player", gender);
            return;
    }
}



// Return true is the bought item has a max count to force refresh of window by caller








void Player::AddSpellCooldown(uint32 spellid, uint32 itemid, uint32 end_time, bool needSendToClient, bool forceSendToSpectator)
{
    _AddSpellCooldown(spellid, 0, itemid, end_time, needSendToClient, forceSendToSpectator);
}





//slot to be excluded while counting




//if false -> then toggled off if was on| if true -> toggled on if was off AND meets requirements




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

bool Player::CanReportAfkDueToLimit()
{
    // a player can complain about 15 people per 5 minutes
    if (m_bgData.bgAfkReportedCount++ >= 15)
        return false;

    return true;
}

///This player has been blamed to be inactive in a battleground
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

WorldLocation Player::GetStartPosition() const
{
    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(getRace(true), getClass());
    uint32 mapId = info->mapId;
    if (IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_INIT) && HasSpell(50977))
        return WorldLocation(0, 2352.0f, -5709.0f, 154.5f, 0.0f);
    return WorldLocation(mapId, info->positionX, info->positionY, info->positionZ, 0);
}

bool Player::HaveAtClient(WorldObject const* u) const
{
    // Motion Transports are always present in player's client
    if (GameObject const* gameobject = u->ToGameObject())
    {
        if (gameobject->IsMotionTransport())
            return true;
    }

    return HaveAtClient(u->GetGUID());
}

bool Player::HaveAtClient(ObjectGuid guid) const
{
    if (guid == GetGUID())
        return true;

    return GetObjectVisibilityContainer().GetVisibleWorldObjectsMap()->find(guid) != GetObjectVisibilityContainer().GetVisibleWorldObjectsMap()->end();
}

bool Player::IsNeverVisible() const
{
    if (Unit::IsNeverVisible())
        return true;

    if (GetSession()->PlayerLogout() || GetSession()->PlayerLoading())
        return true;

    return false;
}

bool Player::CanAlwaysSee(WorldObject const* obj) const
{
    // Always can see self
    if (m_mover == obj)
        return true;

    if (ObjectGuid guid = GetGuidValue(PLAYER_FARSIGHT))
        if (obj->GetGUID() == guid)
            return true;

    return false;
}

bool Player::IsAlwaysDetectableFor(WorldObject const* seer) const
{
    if (Unit::IsAlwaysDetectableFor(seer))
        return true;

    if (duel && duel->State != DUEL_STATE_CHALLENGED && duel->Opponent == seer)
    {
        return false;
    }

    if (Player const* seerPlayer = seer->ToPlayer())
    {
        if (IsGroupVisibleFor(seerPlayer))
        {
            return true;
        }
    }

    return false;
}

bool Player::IsVisibleGloballyFor(Player const* u) const
{
    if (!u)
        return false;

    // Always can see self
    if (u == this)
        return true;

    // Visible units, always are visible for all players
    if (IsVisible())
        return true;

    // GMs are visible for higher gms (or players are visible for gms)
    if (!AccountMgr::IsPlayerAccount(u->GetSession()->GetSecurity()))
        return GetSession()->GetSecurity() <= u->GetSession()->GetSecurity();

    if (!sScriptMgr->OnPlayerNotVisibleGloballyFor(const_cast<Player*>(this), u))
        return true;

    // non faction visibility non-breakable for non-GMs
    return false;
}

void Player::InitPrimaryProfessions()
{
    SetFreePrimaryProfessions(sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL));
}

bool Player::ModifyMoney(int32 amount, bool sendError /*= true*/)
{
    if (!amount)
        return true;

    sScriptMgr->OnPlayerMoneyChanged(this, amount);

    if (amount < 0)
        SetMoney (GetMoney() > uint32(-amount) ? GetMoney() + amount : 0);
    else
    {
        // Trial account money cap (0 disables the cap)
        if (uint32 trialMoneyCap = sWorld->getIntConfig(CONFIG_TRIAL_MONEY_CAP))
        {
            if (GetSession()->IsTrialAccount() && GetMoney() + uint32(amount) > trialMoneyCap)
            {
                if (sendError)
                    SendEquipError(EQUIP_ERR_TOO_MUCH_GOLD, nullptr, nullptr);
                return false;
            }
        }

        if (GetMoney() < uint32(MAX_MONEY_AMOUNT - amount))
            SetMoney(GetMoney() + amount);
        else
        {
            if (sendError)
                SendEquipError(EQUIP_ERR_TOO_MUCH_GOLD, nullptr, nullptr);
            return false;
        }
    }

    return true;
}

Unit* Player::GetSelectedUnit() const
{
    if (ObjectGuid selectionGUID = GetGuidValue(UNIT_FIELD_TARGET))
        return ObjectAccessor::GetUnit(*this, selectionGUID);

    return nullptr;
}

Player* Player::GetSelectedPlayer() const
{
    if (ObjectGuid selectionGUID = GetGuidValue(UNIT_FIELD_TARGET))
        return ObjectAccessor::GetPlayer(*this, selectionGUID);

    return nullptr;
}

void Player::SetSelection(ObjectGuid guid)
{
    SetGuidValue(UNIT_FIELD_TARGET, guid);

    if (NeedSendSpectatorData())
        ArenaSpectator::SendCommand_GUID(FindMap(), GetGUID(), "TRG", guid);
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

void Player::SendInitialPacketsBeforeAddToMap()
{
    /// Pass 'this' as argument because we're not stored in ObjectAccessor yet
    GetSocial()->SendSocialList(this, SOCIAL_FLAG_ALL);

    // guild bank list?

    // Homebind
    WorldPacket data(SMSG_BINDPOINTUPDATE, 5 * 4);
    data << m_homebindX << m_homebindY << m_homebindZ;
    data << (uint32) m_homebindMapId;
    data << (uint32) m_homebindAreaId;
    SendDirectMessage(&data);

    // SMSG_SET_PROFICIENCY
    // SMSG_SET_PCT_SPELL_MODIFIER
    // SMSG_SET_FLAT_SPELL_MODIFIER
    // SMSG_UPDATE_AURA_DURATION

    SendTalentsInfoData(false);

    // SMSG_INSTANCE_DIFFICULTY
    data.Initialize(SMSG_INSTANCE_DIFFICULTY, 4 + 4);
    data << uint32(GetMap()->GetDifficulty());
    data << uint32(GetMap()->GetEntry()->IsDynamicDifficultyMap() && GetMap()->IsHeroic()); // Raid dynamic difficulty
    SendDirectMessage(&data);

    SendInitialSpells();
    SendUnlearnSpells();

    SendInitialActionButtons();
    m_reputationMgr->SendInitialReputations();
    m_achievementMgr->SendAllAchievementData();

    SendEquipmentSetList();

    data.Initialize(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
    data.AppendPackedTime(GameTime::GetGameTime().count());
    data << float(0.01666667f);                             // game speed
    data << uint32(0);                                      // added in 3.1.2
    SendDirectMessage(&data);

    GetReputationMgr().SendForceReactions();                // SMSG_SET_FORCED_REACTIONS

    // SMSG_TALENTS_INFO x 2 for pet (unspent points and talents in separate packets...)
    // SMSG_PET_GUIDS
    // SMSG_UPDATE_WORLD_STATE
    // SMSG_POWER_UPDATE

    SetMover(this);

    sScriptMgr->OnPlayerSendInitialPacketsBeforeAddToMap(this, data);
}

void Player::SendInitialPacketsAfterAddToMap()
{
    UpdateVisibilityForPlayer(true);

    GetSession()->ResetTimeSync();
    GetSession()->SendTimeSync();

    CastSpell(this, 836, true);                             // LOGINEFFECT

    // set some aura effects that send packet to player client after add player to map
    // SendMessageToSet not send it to player not it map, only for aura that not changed anything at re-apply
    // same auras state lost at far teleport, send it one more time in this case also
    static const AuraType auratypes[] =
    {
        SPELL_AURA_MOD_FEAR,     SPELL_AURA_TRANSFORM,                 SPELL_AURA_WATER_WALK,
        SPELL_AURA_FEATHER_FALL, SPELL_AURA_HOVER,                     SPELL_AURA_SAFE_FALL,
        SPELL_AURA_FLY,          SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED, SPELL_AURA_NONE
    };
    for (AuraType const* itr = &auratypes[0]; itr && itr[0] != SPELL_AURA_NONE; ++itr)
    {
        Unit::AuraEffectList const& auraList = GetAuraEffectsByType(*itr);
        if (!auraList.empty())
            auraList.front()->HandleEffect(this, AURA_EFFECT_HANDLE_SEND_FOR_CLIENT, true);
    }

    // Explicitly synchronize CAN_FLY state with client on login to prevent
    // players from retaining flight ability after disconnecting during a
    // teleport from a flyable to non-flyable zone (e.g. entering an instance).
    if (!HasIncreaseMountedFlightSpeedAura() && !HasFlyAura())
        SetCanFly(false);

    // Fix mount, update block gets messed somewhere
    {
        if (!isBeingLoaded() && GetMountBlockId() && !HasMountedAura())
        {
            AddAura(GetMountBlockId(), this);
            SetMountBlockId(0);
        }
    }

    // update zone
    uint32 newzone, newarea;
    GetZoneAndAreaId(newzone, newarea);
    UpdateZone(newzone, newarea);                            // also call SendInitWorldStates();

    WorldPacket setCompoundState(SMSG_MULTIPLE_MOVES, 100);
    setCompoundState << uint32(0); // size placeholder

    // manual send package (have code in HandleEffect(this, AURA_EFFECT_HANDLE_SEND_FOR_CLIENT, true); that must not be re-applied.
    if (IsImmobilizedState())
    {
        uint32 const counter = GetSession()->GetOrderCounter();
        setCompoundState << uint8(2 + GetPackGUID().size() + 4);
        setCompoundState << uint16(SMSG_FORCE_MOVE_ROOT);
        setCompoundState << GetPackGUID();
        setCompoundState << uint32(counter);
        GetSession()->IncrementOrderCounter();
    }

    if (HasAuraType(SPELL_AURA_FEATHER_FALL))
    {
        uint32 const counter = GetSession()->GetOrderCounter();
        setCompoundState << uint8(2 + GetPackGUID().size() + 4);
        setCompoundState << uint16(SMSG_MOVE_FEATHER_FALL);
        setCompoundState << GetPackGUID();
        setCompoundState << uint32(counter);
        GetSession()->IncrementOrderCounter();
    }

    if (HasAuraType(SPELL_AURA_WATER_WALK) || HasAura(8326))
    {
        uint32 const counter = GetSession()->GetOrderCounter();
        setCompoundState << uint8(2 + GetPackGUID().size() + 4);
        setCompoundState << uint16(SMSG_MOVE_WATER_WALK);
        setCompoundState << GetPackGUID();
        setCompoundState << uint32(counter);
        GetSession()->IncrementOrderCounter();
    }

    if (HasAuraType(SPELL_AURA_HOVER))
    {
        uint32 const counter = GetSession()->GetOrderCounter();
        setCompoundState << uint8(2 + GetPackGUID().size() + 4);
        setCompoundState << uint16(SMSG_MOVE_SET_HOVER);
        setCompoundState << GetPackGUID();
        setCompoundState << uint32(counter);
        GetSession()->IncrementOrderCounter();
    }

    // TODO: Pending mount protocol

    if (setCompoundState.size() > 4)
    {
        setCompoundState.put<uint32>(0, setCompoundState.size() - 4);
        SendDirectMessage(&setCompoundState);
    }

    SendEnchantmentDurations();                             // must be after add to map
    SendItemDurations();                                    // must be after add to map
    SendQuestGiverStatusMultiple();
    SendTaxiNodeStatusMultiple();

    // raid downscaling - send difficulty to player
    if (GetMap()->IsRaid())
    {
        if (GetMap()->GetDifficulty() != GetRaidDifficulty())
        {
            StoreRaidMapDifficulty();
            SendRaidDifficulty(GetGroup() != nullptr, GetStoredRaidDifficulty());
        }
    }
    else if (GetRaidDifficulty() != GetStoredRaidDifficulty())
        SendRaidDifficulty(GetGroup() != nullptr);

    // Re-send any pending group loot rolls to this player when they enter a dungeon/raid.
    if (Map* map = GetMap())
        if (map->IsDungeon() || map->IsRaid())
            if (Group* group = GetGroup())
                group->SendPendingRollsToPlayer(this, map);

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

void Player::SendTransferAborted(uint32 mapid, TransferAbortReason reason, uint8 arg)
{
    WorldPacket data(SMSG_TRANSFER_ABORTED, 4 + 2);
    data << uint32(mapid);
    data << uint8(reason);                                 // transfer abort reason
    switch (reason)
    {
        case TRANSFER_ABORT_INSUF_EXPAN_LVL:
        case TRANSFER_ABORT_DIFFICULTY:
        case TRANSFER_ABORT_UNIQUE_MESSAGE:
            // these are the ONLY cases that have an extra argument in the packet!!!
            data << uint8(arg);
            break;
        default:
            break;
    }
    SendDirectMessage(&data);
}

void Player::SendInstanceResetWarning(uint32 mapid, Difficulty difficulty, uint32 time, bool onEnterMap)
{
    // pussywizard:
    InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(GetGUID(), mapid, difficulty);
    if (bind && bind->extended)
    {
        if (!onEnterMap) // extended id player shouldn't be warned about lock expiration
            return;
        time += (bind->save->GetExtendedResetTime() - bind->save->GetResetTime()); // add lockout period to the time left
    }

    // type of warning, based on the time remaining until reset
    uint32 type;
    if (time > 3600)
        type = RAID_INSTANCE_WELCOME;
    else if (time > 900)
        type = RAID_INSTANCE_WARNING_HOURS;
    else if (time > 300)
        type = RAID_INSTANCE_WARNING_MIN;
    else
        type = RAID_INSTANCE_WARNING_MIN_SOON;

    WorldPacket data(SMSG_RAID_INSTANCE_MESSAGE, 4 + 4 + 4 + 4);
    data << uint32(type);
    data << uint32(mapid);
    data << uint32(difficulty);                             // difficulty
    data << uint32(time);
    if (type == RAID_INSTANCE_WELCOME)
    {
        data << uint8(bind && bind->perm);                  // is locked
        data << uint8(bind && bind->extended);              // is extended, ignored if prev field is 0
    }
    SendDirectMessage(&data);
}



















void Player::SetDailyQuestStatus(uint32 quest_id)
{
    if (Quest const* qQuest = sObjectMgr->GetQuestTemplate(quest_id))
    {
        if (!qQuest->IsDFQuest())
        {
            for (uint32 quest_daily_idx = 0; quest_daily_idx < PLAYER_MAX_DAILY_QUESTS; ++quest_daily_idx)
            {
                if (!GetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx))
                {
                    SetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx, quest_id);
                    m_lastDailyQuestTime = GameTime::GetGameTime().count();              // last daily quest time
                    m_DailyQuestChanged = true;
                    break;
                }
            }
        }
        else
        {
            m_DFQuests.insert(quest_id);
            m_lastDailyQuestTime = GameTime::GetGameTime().count();
            m_DailyQuestChanged = true;
        }
    }
}

bool Player::IsDailyQuestDone(uint32 quest_id)
{
    if (sObjectMgr->GetQuestTemplate(quest_id))
    {
        for (uint32 quest_daily_idx = 0; quest_daily_idx < PLAYER_MAX_DAILY_QUESTS; ++quest_daily_idx)
        {
            if (GetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx) == quest_id)
            {
                return true;
            }
        }
    }

    return false;
}

void Player::SetWeeklyQuestStatus(uint32 quest_id)
{
    m_weeklyquests.insert(quest_id);
    m_WeeklyQuestChanged = true;
}

void Player::SetSeasonalQuestStatus(uint32 quest_id)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(quest_id);
    if (!quest)
        return;

    m_seasonalquests[quest->GetEventIdForQuest()].insert(quest_id);
    m_SeasonalQuestChanged = true;
}

void Player::SetMonthlyQuestStatus(uint32 quest_id)
{
    m_monthlyquests.insert(quest_id);
    m_MonthlyQuestChanged = true;
}

void Player::ResetDailyQuestStatus()
{
    for (uint32 quest_daily_idx = 0; quest_daily_idx < PLAYER_MAX_DAILY_QUESTS; ++quest_daily_idx)
        SetUInt32Value(PLAYER_FIELD_DAILY_QUESTS_1 + quest_daily_idx, 0);

    m_DFQuests.clear(); // Dungeon Finder Quests.

    // DB data deleted in caller
    m_DailyQuestChanged = false;
    m_lastDailyQuestTime = 0;
}

void Player::ResetWeeklyQuestStatus()
{
    if (m_weeklyquests.empty())
        return;

    m_weeklyquests.clear();
    // DB data deleted in caller
    m_WeeklyQuestChanged = false;
}

void Player::ResetSeasonalQuestStatus(uint16 event_id)
{
    if (m_seasonalquests.empty() || m_seasonalquests[event_id].empty())
        return;

    m_seasonalquests.erase(event_id);
    // DB data deleted in caller
    m_SeasonalQuestChanged = false;
}

void Player::ResetMonthlyQuestStatus()
{
    if (m_monthlyquests.empty())
        return;

    m_monthlyquests.clear();
    // DB data deleted in caller
    m_MonthlyQuestChanged = false;
}

Battleground* Player::GetBattleground(bool create) const
{
    if (GetBattlegroundId() == 0)
        return nullptr;

    Battleground* bg = sBattlegroundMgr->GetBattleground(GetBattlegroundId(), GetBattlegroundTypeId());
    return (create || (bg && bg->FindBgMap()) ? bg : nullptr);
}

bool Player::InBattlefield() const
{
    Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(GetZoneId());
    return bf && bf->IsWarTime();
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

bool Player::GetBGAccessByLevel(BattlegroundTypeId bgTypeId) const
{
    // get a template bg instead of running one
    Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgt)
        return false;

    // limit check leel to dbc compatible level range
    uint32 level = GetLevel();
    if (level > DEFAULT_MAX_LEVEL)
        level = DEFAULT_MAX_LEVEL;

    if (level < bgt->GetMinLevel() || level > bgt->GetMaxLevel())
        return false;

    return true;
}

float Player::GetReputationPriceDiscount(Creature const* creature) const
{
    float discount = GetReputationPriceDiscount(creature->GetFactionTemplateEntry());
    sScriptMgr->OnPlayerGetReputationPriceDiscount(this, creature, discount);
    return discount;
}

float Player::GetReputationPriceDiscount(FactionTemplateEntry const* factionTemplate) const
{
    float discount = 1.0f;

    if (!factionTemplate || !factionTemplate->faction)
    {
        sScriptMgr->OnPlayerGetReputationPriceDiscount(this, factionTemplate, discount);
        return discount;
    }

    ReputationRank rank = GetReputationRank(factionTemplate->faction);

    if (rank <= REP_NEUTRAL)
    {
        sScriptMgr->OnPlayerGetReputationPriceDiscount(this, factionTemplate, discount);
        return discount;
    }

    discount = discount - 0.05f * (rank - REP_NEUTRAL);

    sScriptMgr->OnPlayerGetReputationPriceDiscount(this, factionTemplate, discount);
    return discount;
}



bool Player::HasQuestForGO(int32 GOId) const
{
    for (uint8 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questid = GetQuestSlotQuestId(i);
        if (questid == 0)
            continue;

        QuestStatusMap::const_iterator qs_itr = m_QuestStatus.find(questid);
        if (qs_itr == m_QuestStatus.end())
            continue;

        QuestStatusData const& qs = qs_itr->second;

        if (qs.Status == QUEST_STATUS_INCOMPLETE)
        {
            Quest const* qinfo = sObjectMgr->GetQuestTemplate(questid);
            if (!qinfo)
                continue;

            if (GetGroup() && GetGroup()->isRaidGroup() && !qinfo->IsAllowedInRaid(GetMap()->GetDifficulty()))
                continue;

            for (uint8 j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
            {
                if (qinfo->RequiredNpcOrGo[j] >= 0)       //skip non GO case
                    continue;

                if ((-1)*GOId == qinfo->RequiredNpcOrGo[j] && qs.CreatureOrGOCount[j] < qinfo->RequiredNpcOrGoCount[j])
                    return true;
            }
        }
    }
    return false;
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







OutdoorPvP* Player::GetOutdoorPvP() const
{
    return sOutdoorPvPMgr->GetOutdoorPvPToZoneId(GetZoneId());
}









// Used in triggers for check "Only to targets that grant experience or honor" req
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



void Player::ResurectUsingRequestData()
{
    /// Teleport before resurrecting by player, otherwise the player might get attacked from creatures near his corpse
    TeleportTo(m_resurrectMap, m_resurrectX, m_resurrectY, m_resurrectZ, GetOrientation());

    if (IsBeingTeleported())
    {
        ScheduleDelayedOperation(DELAYED_RESURRECT_PLAYER);
        return;
    }

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

void Player::SetClientControl(Unit* target, bool allowMove, bool packetOnly /*= false*/)
{
    ASSERT(target);
    // refuse to grant control if target has UNIT_STATE_CHARMED
    if (target->HasUnitState(UNIT_STATE_CHARMED) && (GetGUID() != target->GetCharmerGUID()))
    {
        LOG_ERROR("entities.player", "Player '{}' attempt to client control '{}', which is charmed by GUID {}", GetName(), target->GetName(), target->GetCharmerGUID().ToString());
        return;
    }

    // A fleeing/confused target can't be controlled by the client yet, but the mover
    // must still switch so control is restored once the crowd control ends.
    WorldPacket data(SMSG_CLIENT_CONTROL_UPDATE, target->GetPackGUID().size() + 1);
    data << target->GetPackGUID();
    data << uint8((allowMove && !target->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED)) ? 1 : 0);
    SendDirectMessage(&data);

    // We want to set the packet only
    if (packetOnly)
        return;

    if (this != target)
        SetViewpoint(target, allowMove);

    if (allowMove)
        SetMover(target);

    // Xinef: disable moving if target has disable move flag
    if (!target->IsCreature())
        return;

    if (allowMove && target->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
    {
        target->ClearUnitState(UNIT_STATE_ROOT);
        target->SetControlled(true, UNIT_STATE_ROOT);
    }
    else if (!allowMove && target->HasUnitState(UNIT_STATE_ROOT) && !target->HasUnitTypeMask(UNIT_MASK_ACCESSORY))
    {
        if (target->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        {
            // Xinef: restore original orientation, important for shooting vehicles!
            Position pos = target->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && target->GetTransGUID() && target->GetTransGUID().IsMOTransport() ? target->ToCreature()->GetTransportHomePosition() : target->ToCreature()->GetHomePosition();
            target->SetOrientation(pos.GetOrientation());
            target->SetFacingTo(pos.GetOrientation());
            target->DisableSpline();
        }
        else
            target->SetControlled(false, UNIT_STATE_ROOT);
    }
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

uint32 Player::GetCorpseReclaimDelay(bool pvp) const
{
    if (pvp)
    {
        if (!sWorld->getBoolConfig(CONFIG_DEATH_CORPSE_RECLAIM_DELAY_PVP))
            return copseReclaimDelay[0];
    }
    else if (!sWorld->getBoolConfig(CONFIG_DEATH_CORPSE_RECLAIM_DELAY_PVE))
        return 0;

    time_t now = GameTime::GetGameTime().count();
    // 0..2 full period
    // should be std::ceil(x)-1 but not floor(x)
    uint64 count = (now < m_deathExpireTime - 1) ? (m_deathExpireTime - 1 - now) / DEATH_EXPIRE_STEP : 0;
    return copseReclaimDelay[count];
}

int32 Player::CalculateCorpseReclaimDelay(bool load)
{
    Corpse* corpse = GetCorpse();

    if (load && !corpse)
        return -1;

    bool pvp = corpse ? corpse->GetType() == CORPSE_RESURRECTABLE_PVP : m_ExtraFlags & PLAYER_EXTRA_PVP_DEATH;

    uint32 delay;

    if (load)
    {
        if (corpse->GetGhostTime() > m_deathExpireTime)
            return -1;

        uint64 count = 0;

        if ((pvp && sWorld->getBoolConfig(CONFIG_DEATH_CORPSE_RECLAIM_DELAY_PVP)) ||
                (!pvp && sWorld->getBoolConfig(CONFIG_DEATH_CORPSE_RECLAIM_DELAY_PVE)))
        {
            count = (m_deathExpireTime - corpse->GetGhostTime()) / DEATH_EXPIRE_STEP;

            if (count >= MAX_DEATH_COUNT)
                count = MAX_DEATH_COUNT - 1;
        }

        time_t expected_time = corpse->GetGhostTime() + copseReclaimDelay[count];
        time_t now = GameTime::GetGameTime().count();

        if (now >= expected_time)
            return -1;

        delay = expected_time - now;
    }
    else
        delay = GetCorpseReclaimDelay(pvp);

    return delay * IN_MILLISECONDS;
}

void Player::SendCorpseReclaimDelay(uint32 delay)
{
    WorldPacket data(SMSG_CORPSE_RECLAIM_DELAY, 4);
    data << uint32(delay);
    SendDirectMessage(&data);
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

bool Player::IsUsingLfg()
{
    return sLFGMgr->GetState(GetGUID()) != lfg::LFG_STATE_NONE;
}

bool Player::inRandomLfgDungeon()
{
    if (sLFGMgr->selectedRandomLfgDungeon(GetGUID()))
    {
        Map const* map = GetMap();
        return sLFGMgr->inLfgDungeonMap(GetGUID(), map->GetId(), map->GetDifficulty());
    }

    return false;
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





void Player::SetCanTitanGrip(bool value)
{
    m_canTitanGrip = value;
}

bool ItemPosCount::isContainedIn(ItemPosCountVec const& vec) const
{
    for (ItemPosCountVec::const_iterator itr = vec.begin(); itr != vec.end(); ++itr)
        if (itr->pos == pos)
            return true;
    return false;
}



void Player::SetViewpoint(WorldObject* target, bool apply)
{
    if (apply)
    {
        LOG_DEBUG("maps", "Player::CreateViewpoint: Player {} create seer {} (TypeId: {}).", GetName(), target->GetEntry(), target->GetTypeId());

        if (!AddGuidValue(PLAYER_FARSIGHT, target->GetGUID()))
        {
            LOG_DEBUG("entities.player", "Player::CreateViewpoint: Player {} cannot add new viewpoint!", GetName());
            return;
        }

        // farsight dynobj or puppet may be very far away
        UpdateVisibilityOf(target);

        if (target->IsUnit() && !GetVehicle())
            ((Unit*)target)->AddPlayerToVision(this);
        SetSeer(target);
    }
    else
    {
        //must immediately set seer back otherwise may crash
        m_seer = this;

        LOG_DEBUG("maps", "Player::CreateViewpoint: Player {} remove seer", GetName());

        if (!RemoveGuidValue(PLAYER_FARSIGHT, target->GetGUID()))
        {
            LOG_DEBUG("entities.player", "Player::CreateViewpoint: Player {} cannot remove current viewpoint!", GetName());
            return;
        }

        if (target->IsUnit() && !GetVehicle())
            static_cast<Unit*>(target)->RemovePlayerFromVision(this);

        // must immediately set seer back otherwise may crash
        SetSeer(this);

        //WorldPacket data(SMSG_CLEAR_FAR_SIGHT_IMMEDIATE, 0);
        //SendDirectMessage(&data);
    }
}

WorldObject* Player::GetViewpoint() const
{
    if (ObjectGuid guid = GetGuidValue(PLAYER_FARSIGHT))
        return static_cast<WorldObject*>(ObjectAccessor::GetObjectByTypeMask(*this, guid, TYPEMASK_SEER));
    return nullptr;
}

Position const& Player::GetSightPosition() const
{
    if (_cinematicMgr.IsOnCinematic())
        return _cinematicMgr.GetRemoteSightPosition();

    return *m_seer;
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

bool Player::CanCaptureTowerPoint() const
{
    return (!HasStealthAura() &&                            // not stealthed
            !HasInvisibilityAura() &&                      // not invisible
            IsAlive()                                      // live player
           );
}

uint32 Player::GetBarberShopCost(uint8 newhairstyle, uint8 newhaircolor, uint8 newfacialhair, BarberShopStyleEntry const* newSkin)
{
    uint8 level = GetLevel();

    if (level > GT_MAX_LEVEL)
        level = GT_MAX_LEVEL;                               // max level in this dbc

    uint8 hairstyle = GetByteValue(PLAYER_BYTES, 2);
    uint8 haircolor = GetByteValue(PLAYER_BYTES, 3);
    uint8 facialhair = GetByteValue(PLAYER_BYTES_2, 0);
    uint8 skincolor = GetByteValue(PLAYER_BYTES, 0);

    if ((hairstyle == newhairstyle) && (haircolor == newhaircolor) && (facialhair == newfacialhair) && (!newSkin || (newSkin->hair_id == skincolor)))
        return 0;

    GtBarberShopCostBaseEntry const* bsc = sGtBarberShopCostBaseStore.LookupEntry(level - 1);

    if (!bsc)                                                // shouldn't happen
        return 0xFFFFFFFF;

    float cost = 0;

    if (hairstyle != newhairstyle)
        cost += bsc->cost;                                  // full price

    if ((haircolor != newhaircolor) && (hairstyle == newhairstyle))
        cost += bsc->cost * 0.5f;                           // +1/2 of price

    if (facialhair != newfacialhair)
        cost += bsc->cost * 0.75f;                          // +3/4 of price

    if (newSkin && skincolor != newSkin->hair_id)
        cost += bsc->cost * 0.75f;                          // +5/6 of price

    return uint32(cost);
}





bool Player::HasTitle(uint32 bitIndex) const
{
    if (bitIndex > MAX_TITLE_INDEX)
        return false;

    uint32 fieldIndexOffset = bitIndex / 32;
    uint32 flag = 1 << (bitIndex % 32);
    return HasFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag);
}

void Player::SetTitle(CharTitlesEntry const* title, bool lost)
{
    uint32 fieldIndexOffset = title->bit_index / 32;
    uint32 flag = 1 << (title->bit_index % 32);

    if (lost)
    {
        if (!HasFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag))
            return;

        // Clear the current title if it is the one being removed.
        if (title->bit_index == GetUInt32Value(PLAYER_CHOSEN_TITLE))
        {
            SetCurrentTitle(nullptr, true);
        }

        RemoveFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag);
    }
    else
    {
        if (HasFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag))
            return;

        SetFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag);
    }

    WorldPacket data(SMSG_TITLE_EARNED, 4 + 4);
    data << uint32(title->bit_index);
    data << uint32(lost ? 0 : 1);                           // 1 - earned, 0 - lost
    SendDirectMessage(&data);

    UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_OWN_RANK);
}

























bool Player::canFlyInZone(uint32 mapid, uint32 zone, SpellInfo const* bySpell)
{
    if (!sScriptMgr->OnPlayerCanFlyInZone(this, mapid,zone,bySpell))
    {
        return false;
    }

    // continent checked in SpellInfo::CheckLocation at cast and area update
    uint32 v_map = GetVirtualMapForMapAndZone(mapid, zone);
    if (v_map == MAP_NORTHREND && !bySpell->HasAttribute(SPELL_ATTR7_IGNORES_COLD_WEATHER_FLYING_REQUIREMENT))
    {
        if (!HasSpell(54197)) // 54197 = Cold Weather Flying
        {
            return false;
        }
    }

    return true;
}





uint32 Player::GetPhaseMaskForSpawn() const
{
    uint32 phase = IsGameMaster() ? GetPhaseByAuras() : GetPhaseMask();

    if (!phase)
        phase = PHASEMASK_NORMAL;

    // some aura phases include 1 normal map in addition to phase itself
    uint32 n_phase = phase & ~PHASEMASK_NORMAL;
    if (n_phase > 0)
        return n_phase;

    return phase;
}





static constexpr float   FALL_DMG_EQU_SLOPE         = 0.018f;
static constexpr float   FALL_DMG_EQU_INTERCEPT     = -0.2426f;
static constexpr float   MIN_FALL_DMG_DIST          = 13.48f;       // Minimum fall distance that deals damage
// 13.48 can be calculated by resolving damageperc to 0 in the fall damage equation below, and assuming safe_fall reduction = 0

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



void Player::ResetAchievements()
{
    m_achievementMgr->Reset();
}

void Player::SendRespondInspectAchievements(Player* player) const
{
    m_achievementMgr->SendRespondInspectAchievements(player);
}

bool Player::HasAchieved(uint32 achievementId) const
{
    return m_achievementMgr->HasAchieved(achievementId);
}

void Player::StartTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry, uint32 timeLost/* = 0*/)
{
    m_achievementMgr->StartTimedAchievement(type, entry, timeLost);
}

void Player::RemoveTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry)
{
    m_achievementMgr->RemoveTimedAchievement(type, entry);
}



void Player::CompletedAchievement(AchievementEntry const* entry)
{
    m_achievementMgr->CompletedAchievement(entry);
}





void Player::AddKnownCurrency(uint32 itemId)
{
    if (CurrencyTypesEntry const* ctEntry = sCurrencyTypesStore.LookupEntry(itemId))
        SetFlag64(PLAYER_FIELD_KNOWN_CURRENCIES, (1LL << (ctEntry->BitIndex - 1)));
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



bool Player::CanSeeObjectByVisibilityConditions(WorldObject const* object) const
{
    if (IsGameMaster())
        return true;

    ConditionList conds = sConditionMgr->GetConditionsForObjectVisibility(object);
    ConditionSourceInfo info = ConditionSourceInfo(const_cast<Player*>(this), const_cast<WorldObject*>(object));
    return sConditionMgr->IsObjectMeetToConditions(info, conds);
}

/**
 * @brief Checks if any vendor option is available in the gossip menu tree for a given creature.
 *
 * @param menuId The starting gossip menu ID to check.
 * @param creature Pointer to the creature whose gossip menus are being checked.
 * @return true if a vendor option is available in any accessible menu; false otherwise.
 */
bool Player::AnyVendorOptionAvailable(uint32 menuId, Creature const* creature) const
{
    {
        GossipMenuItemsMapBounds menuItemBounds = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
        if (menuItemBounds.first == menuItemBounds.second)
            return true;
    }

    std::set<uint32> visitedMenus;
    std::queue<uint32> menusToCheck;
    menusToCheck.push(menuId);

    while (!menusToCheck.empty())
    {
        uint32 const currentMenuId = menusToCheck.front();
        menusToCheck.pop();

        if (visitedMenus.find(currentMenuId) != visitedMenus.end())
            continue;

        visitedMenus.insert(currentMenuId);

        GossipMenuItemsMapBounds menuItemBounds = sObjectMgr->GetGossipMenuItemsMapBounds(currentMenuId);

        if (menuItemBounds.first == menuItemBounds.second && currentMenuId != 0)
            continue;

        for (auto itr = menuItemBounds.first; itr != menuItemBounds.second; ++itr)
        {
            if (!sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(this), const_cast<Creature*>(creature), itr->second.Conditions))
                continue;

            if (itr->second.OptionType == GOSSIP_OPTION_VENDOR)
                return true;
            else if (itr->second.ActionMenuID)
            {
                GossipMenusMapBounds menuBounds = sObjectMgr->GetGossipMenusMapBounds(itr->second.ActionMenuID);
                bool menuAccessible = false;

                if (menuBounds.first == menuBounds.second)
                    menuAccessible = true;
                else
                {
                    for (auto menuItr = menuBounds.first; menuItr != menuBounds.second; ++menuItr)
                        if (sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(this), const_cast<Creature*>(creature), menuItr->second.Conditions))
                        {
                            menuAccessible = true;
                            break;
                        }
                }

                if (menuAccessible)
                    menusToCheck.push(itr->second.ActionMenuID);
            }
        }
    }

    return false;
}

bool Player::CanSeeVendor(Creature const* creature) const
{
    if (!creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        return true;

    ConditionList conditions = sConditionMgr->GetConditionsForNpcVendorEvent(creature->GetEntry(), 0);
    if (!sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(this), const_cast<Creature*>(creature), conditions))
        return false;

    uint32 const menuId = creature->GetGossipMenuId();
    if (!AnyVendorOptionAvailable(menuId, creature))
        return false;

    return true;
}

bool Player::CanSeeTrainer(Creature const* creature) const
{
    if (!creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
        return true;

    if (auto trainer = sObjectMgr->GetTrainer(creature->GetEntry()))
        if (!trainer || !trainer->IsTrainerValidForPlayer(this))
            return false;

    return true;
}















void Player::_SaveEntryPoint(CharacterDatabaseTransaction trans)
{
    // xinef: dont save joinpos with invalid mapid
    MapEntry const* mEntry = sMapStore.LookupEntry(m_entryPointData.joinPos.GetMapId());
    if (!mEntry)
        return;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PLAYER_ENTRY_POINT);
    stmt->SetData(0, GetGUID().GetRawValue());
    trans->Append(stmt);

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PLAYER_ENTRY_POINT);
    stmt->SetData(0, GetGUID().GetRawValue());
    stmt->SetData (1, m_entryPointData.joinPos.GetPositionX());
    stmt->SetData (2, m_entryPointData.joinPos.GetPositionY());
    stmt->SetData (3, m_entryPointData.joinPos.GetPositionZ());
    stmt->SetData (4, m_entryPointData.joinPos.GetOrientation());
    stmt->SetData(5, m_entryPointData.joinPos.GetMapId());
    stmt->SetData(6, m_entryPointData.taxiPath[0]);
    stmt->SetData(7, m_entryPointData.taxiPath[1]);
    stmt->SetData(8, m_entryPointData.mountSpell);
    trans->Append(stmt);
}



void Player::RemoveAtLoginFlag(AtLoginFlags flags, bool persist /*= false*/)
{
    m_atLoginFlags &= ~flags;

    if (persist)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_REM_AT_LOGIN_FLAG);

        stmt->SetData(0, uint16(flags));
        stmt->SetData(1, GetGUID().GetRawValue());

        CharacterDatabase.Execute(stmt);
    }
}



void Player::ResetMap()
{
    // this may be called during Map::Update
    // after decrement+unlink, ++m_mapRefIter will continue correctly
    // when the first element of the list is being removed
    // nocheck_prev will return the padding element of the RefMgr
    // instead of nullptr in the case of prev
    GetMap()->UpdateIteratorBack(this);
    Unit::ResetMap();
    GetMapRef().unlink();
}

void Player::SetMap(Map* map)
{
    Unit::SetMap(map);
    m_mapRef.link(map, this);
}

void Player::_SaveCharacter(bool create, CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;
    uint8 index = 0;

    auto finiteAlways = [](float f) { return std::isfinite(f) ? f : 0.0f; };

    if (create)
    {
        //! Insert query
        //! TO DO: Filter out more redundant fields that can take their default value at player create
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHARACTER);
        stmt->SetData(index++, GetGUID().GetRawValue());
        stmt->SetData(index++, GetSession()->GetAccountId());
        stmt->SetData(index++, GetName());
        stmt->SetData(index++, getRace(true));
        stmt->SetData(index++, getClass());
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_3, 0));   // save gender from PLAYER_BYTES_3, UNIT_BYTES_0 changes with every transform effect
        stmt->SetData(index++, GetLevel());
        stmt->SetData(index++, GetUInt32Value(PLAYER_XP));
        stmt->SetData(index++, GetMoney());
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 0));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 1));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 2));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 3));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 0));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 2));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 3));
        stmt->SetData(index++, (uint32)GetPlayerFlags());
        stmt->SetData(index++, (uint16)GetMapId());
        stmt->SetData(index++, (uint32)GetInstanceId());
        stmt->SetData(index++, (uint8(GetDungeonDifficulty()) | uint8(GetRaidDifficulty()) << 4));
        stmt->SetData(index++, finiteAlways(GetPositionX()));
        stmt->SetData(index++, finiteAlways(GetPositionY()));
        stmt->SetData(index++, finiteAlways(GetPositionZ()));
        stmt->SetData(index++, finiteAlways(GetOrientation()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetX()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetY()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetZ()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetO()));

        int32 lowGuidOrSpawnId = 0;
        if (Transport* transport = GetTransport())
        {
            if (transport->IsMotionTransport())
                lowGuidOrSpawnId = static_cast<int32>(transport->GetGUID().GetCounter());
            else if (transport->IsStaticTransport())
                lowGuidOrSpawnId = -static_cast<int32>(transport->GetSpawnId());
        }
        stmt->SetData(index++, lowGuidOrSpawnId);

        std::ostringstream ss;
        ss << m_taxi;
        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, m_cinematic);
        stmt->SetData(index++, m_Played_time[PLAYED_TIME_TOTAL]);
        stmt->SetData(index++, m_Played_time[PLAYED_TIME_LEVEL]);
        stmt->SetData(index++, finiteAlways(_restBonus));
        stmt->SetData(index++, uint32(GameTime::GetGameTime().count()));
        stmt->SetData(index++,  (HasPlayerFlag(PLAYER_FLAGS_RESTING) ? 1 : 0));
        //save, far from tavern/city
        //save, but in tavern/city
        stmt->SetData(index++, m_resetTalentsCost);
        stmt->SetData(index++, uint32(m_resetTalentsTime));
        stmt->SetData(index++, (uint16)m_ExtraFlags);
        stmt->SetData(index++, m_petStable ? m_petStable->MaxStabledPets : 0);
        stmt->SetData(index++, (uint16)m_atLoginFlags);
        stmt->SetData(index++, GetZoneId());
        stmt->SetData(index++, uint32(m_deathExpireTime));

        ss.str("");
        ss << m_taxi.SaveTaxiDestinationsToString();

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetArenaPoints());
        stmt->SetData(index++, GetHonorPoints());
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
        stmt->SetData(index++, GetUInt16Value(PLAYER_FIELD_KILLS, 0));
        stmt->SetData(index++, GetUInt16Value(PLAYER_FIELD_KILLS, 1));
        stmt->SetData(index++, GetUInt32Value(PLAYER_CHOSEN_TITLE));
        stmt->SetData(index++, GetUInt64Value(PLAYER_FIELD_KNOWN_CURRENCIES));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX));
        stmt->SetData(index++, GetDrunkValue());
        stmt->SetData(index++, GetHealth());

        for (uint32 i = 0; i < MAX_POWERS; ++i)
            stmt->SetData(index++, GetPower(Powers(i)));

        stmt->SetData(index++, GetSession()->GetLatency());

        stmt->SetData(index++, m_specsCount);
        stmt->SetData(index++, m_activeSpec);

        ss.str("");
        for (uint32 i = 0; i < PLAYER_EXPLORED_ZONES_SIZE; ++i)
            ss << GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + i) << ' ';
        stmt->SetData(index++, ss.str());

        ss.str("");
        // cache equipment...
        for (uint32 i = 0; i < EQUIPMENT_SLOT_END * 2; ++i)
            ss << GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + i) << ' ';

        // ...and bags for enum opcode
        for (uint32 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                ss << item->GetEntry();
            else
                ss << '0';
            ss << " 0 ";
        }

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetUInt32Value(PLAYER_AMMO_ID));

        ss.str("");
        for (uint32 i = 0; i < KNOWN_TITLES_SIZE * 2; ++i)
            ss << GetUInt32Value(PLAYER__FIELD_KNOWN_TITLES + i) << ' ';

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetByteValue(PLAYER_FIELD_BYTES, 2));
        stmt->SetData(index++, m_grantableLevels);
        stmt->SetData(index++, _innTriggerId);
        stmt->SetData(index++, m_extraBonusTalentCount);
    }
    else
    {
        // Update query
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHARACTER);
        stmt->SetData(index++, GetName());
        stmt->SetData(index++, getRace(true));
        stmt->SetData(index++, getClass());
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_3, 0));   // save gender from PLAYER_BYTES_3, UNIT_BYTES_0 changes with every transform effect
        stmt->SetData(index++, GetLevel());
        stmt->SetData(index++, GetUInt32Value(PLAYER_XP));
        stmt->SetData(index++, GetMoney());
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 0));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 1));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 2));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES, 3));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 0));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 2));
        stmt->SetData(index++, GetByteValue(PLAYER_BYTES_2, 3));
        stmt->SetData(index++, GetPlayerFlags());

        if (!IsBeingTeleported())
        {
            Difficulty dd = GetDungeonDifficulty(), rd = GetRaidDifficulty();
            if (Map* m = FindMap())
                if (m->IsDungeon())
                {
                    if (m->IsNonRaidDungeon()) dd = m->GetDifficulty();
                    else rd = m->GetDifficulty();
                }
            stmt->SetData(index++, (uint16)GetMapId());
            stmt->SetData(index++, (uint32)GetInstanceId());
            stmt->SetData(index++, (uint8(dd) | uint8(rd) << 4));
            stmt->SetData(index++, finiteAlways(GetPositionX()));
            stmt->SetData(index++, finiteAlways(GetPositionY()));
            stmt->SetData(index++, finiteAlways(GetPositionZ()));
            stmt->SetData(index++, finiteAlways(GetOrientation()));
        }
        else
        {
            stmt->SetData(index++, (uint16)GetTeleportDest().GetMapId());
            stmt->SetData(index++, (uint32)0);
            stmt->SetData(index++, (uint8(GetDungeonDifficulty()) | uint8(GetRaidDifficulty()) << 4));
            stmt->SetData(index++, finiteAlways(GetTeleportDest().GetPositionX()));
            stmt->SetData(index++, finiteAlways(GetTeleportDest().GetPositionY()));
            stmt->SetData(index++, finiteAlways(GetTeleportDest().GetPositionZ()));
            stmt->SetData(index++, finiteAlways(GetTeleportDest().GetOrientation()));
        }

        stmt->SetData(index++, finiteAlways(GetTransOffsetX()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetY()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetZ()));
        stmt->SetData(index++, finiteAlways(GetTransOffsetO()));

        int32 lowGuidOrSpawnId = 0;
        if (Transport* transport = GetTransport())
        {
            if (transport->IsMotionTransport())
                lowGuidOrSpawnId = static_cast<int32>(transport->GetGUID().GetCounter());
            else if (transport->IsStaticTransport())
                lowGuidOrSpawnId = -static_cast<int32>(transport->GetSpawnId());
        }
        stmt->SetData(index++, lowGuidOrSpawnId);

        std::ostringstream ss;
        ss << m_taxi;
        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, m_cinematic);
        stmt->SetData(index++, m_Played_time[PLAYED_TIME_TOTAL]);
        stmt->SetData(index++, m_Played_time[PLAYED_TIME_LEVEL]);
        stmt->SetData(index++, finiteAlways(_restBonus));
        stmt->SetData(index++, uint32(GameTime::GetGameTime().count()));
        stmt->SetData(index++,  (HasPlayerFlag(PLAYER_FLAGS_RESTING) ? 1 : 0));
        //save, far from tavern/city
        //save, but in tavern/city
        stmt->SetData(index++, m_resetTalentsCost);
        stmt->SetData(index++, uint32(m_resetTalentsTime));
        stmt->SetData(index++, (uint16)m_ExtraFlags);
        stmt->SetData(index++, m_petStable ? m_petStable->MaxStabledPets : 0);
        stmt->SetData(index++, (uint16)m_atLoginFlags);
        stmt->SetData(index++, GetZoneId());
        stmt->SetData(index++, uint32(m_deathExpireTime));

        ss.str("");
        ss << m_taxi.SaveTaxiDestinationsToString();

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetArenaPoints());
        stmt->SetData(index++, GetHonorPoints());
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
        stmt->SetData(index++, GetUInt16Value(PLAYER_FIELD_KILLS, 0));
        stmt->SetData(index++, GetUInt16Value(PLAYER_FIELD_KILLS, 1));
        stmt->SetData(index++, GetUInt32Value(PLAYER_CHOSEN_TITLE));
        stmt->SetData(index++, GetUInt64Value(PLAYER_FIELD_KNOWN_CURRENCIES));
        stmt->SetData(index++, GetUInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX));
        stmt->SetData(index++, GetDrunkValue());
        stmt->SetData(index++, GetHealth());

        for (uint32 i = 0; i < MAX_POWERS; ++i)
            stmt->SetData(index++, GetPower(Powers(i)));

        stmt->SetData(index++, GetSession()->GetLatency());

        stmt->SetData(index++, m_specsCount);
        stmt->SetData(index++, m_activeSpec);

        ss.str("");
        for (uint32 i = 0; i < PLAYER_EXPLORED_ZONES_SIZE; ++i)
            ss << GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + i) << ' ';
        stmt->SetData(index++, ss.str());

        ss.str("");
        // cache equipment...
        for (uint32 i = 0; i < EQUIPMENT_SLOT_END * 2; ++i)
            ss << GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + i) << ' ';

        // ...and bags for enum opcode
        for (uint32 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                ss << item->GetEntry();
            else
                ss << '0';
            ss << " 0 ";
        }

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetUInt32Value(PLAYER_AMMO_ID));

        ss.str("");
        for (uint32 i = 0; i < KNOWN_TITLES_SIZE * 2; ++i)
            ss << GetUInt32Value(PLAYER__FIELD_KNOWN_TITLES + i) << ' ';

        stmt->SetData(index++, ss.str());
        stmt->SetData(index++, GetByteValue(PLAYER_FIELD_BYTES, 2));
        stmt->SetData(index++, m_grantableLevels);
        stmt->SetData(index++, _innTriggerId);
        stmt->SetData(index++, m_extraBonusTalentCount);

        stmt->SetData(index++, IsInWorld() && !GetSession()->PlayerLogout() ? 1 : 0);
        // Index
        stmt->SetData(index++, GetGUID().GetRawValue());
    }

    trans->Append(stmt);
}











void Player::LoadActions(PreparedQueryResult result)
{
    if (result)
        _LoadActions(result);

    SendActionButtons(1);
}





void Player::SetReputation(uint32 factionentry, float value)
{
    GetReputationMgr().SetReputation(sFactionStore.LookupEntry(factionentry), value);
}

uint32 Player::GetReputation(uint32 factionentry) const
{
    return GetReputationMgr().GetReputation(sFactionStore.LookupEntry(factionentry));
}

std::string const& Player::GetGuildName()
{
    return sGuildMgr->GetGuildById(GetGuildId())->GetName();
}

void Player::SendDuelCountdown(uint32 counter)
{
    WorldPacket data(SMSG_DUEL_COUNTDOWN, 4);
    data << uint32(counter);                                // seconds
    SendDirectMessage(&data);
}







void Player::AddRefundReference(ObjectGuid itemGUID)
{
    m_refundableItems.insert(itemGUID);
}

void Player::DeleteRefundReference(ObjectGuid itemGUID)
{
    RefundableItemsSet::iterator itr = m_refundableItems.find(itemGUID);
    if (itr != m_refundableItems.end())
        m_refundableItems.erase(itr);
}

void Player::SendRefundInfo(Item* item)
{
    // This function call unsets ITEM_FLAGS_REFUNDABLE if played time is over 2 hours.
    item->UpdatePlayedTime(this);

    if (!item->IsRefundable())
    {
        LOG_DEBUG("entities.player.items", "Item refund: item not refundable!");
        return;
    }

    if (GetGUID().GetCounter() != item->GetRefundRecipient()) // Formerly refundable item got traded
    {
        LOG_DEBUG("entities.player.items", "Item refund: item was traded!");
        item->SetNotRefundable(this);
        return;
    }

    ItemExtendedCostEntry const* iece = sItemExtendedCostStore.LookupEntry(item->GetPaidExtendedCost());
    if (!iece)
    {
        LOG_DEBUG("entities.player.items", "Item refund: cannot find extendedcost data.");
        return;
    }

    WorldPacket data(SMSG_ITEM_REFUND_INFO_RESPONSE, 8 + 4 + 4 + 4 + 4 * 4 + 4 * 4 + 4 + 4);
    data << item->GetGUID();                            // item guid
    data << uint32(item->GetPaidMoney());               // money cost
    data << uint32(iece->reqhonorpoints);               // honor point cost
    data << uint32(iece->reqarenapoints);               // arena point cost
    for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)                       // item cost data
    {
        data << uint32(iece->reqitem[i]);
        data << uint32(iece->reqitemcount[i]);
    }
    data << uint32(0);
    data << uint32(GetTotalPlayedTime() - item->GetPlayedTime());
    SendDirectMessage(&data);
}



PetStable& Player::GetOrInitPetStable()
{
    if (!m_petStable)
        m_petStable = std::make_unique<PetStable>();

    return *m_petStable;
}



void Player::SetRandomWinner(bool isWinner)
{
    m_IsBGRandomWinner = isWinner;
    if (m_IsBGRandomWinner)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_BATTLEGROUND_RANDOM);
        stmt->SetData(0, GetGUID().GetRawValue());
        CharacterDatabase.Execute(stmt);
    }
}









void Player::_LoadInstanceTimeRestrictions(PreparedQueryResult result)
{
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        _instanceResetTimes.insert(InstanceTimeMap::value_type(fields[0].Get<uint32>(), fields[1].Get<uint64>()));
    } while (result->NextRow());
}

void Player::_LoadBrewOfTheMonth(PreparedQueryResult result)
{
    uint32 lastEventId = 0;
    if (result)
    {
        Field* fields = result->Fetch();
        lastEventId = fields[0].Get<uint32>();
    }

    uint16 month = static_cast<uint16>(Acore::Time::GetMonth());
    uint16 eventId = month;
    if (eventId < 9)
        eventId += 3;
    else
        eventId -= 9;

    // Brew of the Month October (first in list)
    eventId += 34;

    if (lastEventId != eventId && IsEventActive(eventId) && HasAchieved(2796 /* Brew of the Month*/))
    {
        // Send Mail
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailSender sender(MAIL_CREATURE, 27487 /*NPC_BREW_OF_THE_MONTH_CLUB*/);
        MailDraft draft(uint16(212 + month)); // 212 is starting template id
        draft.SendMailTo(trans, MailReceiver(this, GetGUID().GetCounter()), sender);

        // Update Event Id
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_BREW_OF_THE_MONTH);
        stmt->SetData(0, GetGUID().GetRawValue());
        stmt->SetData(1, uint32(eventId));
        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);
    }
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

void Player::_SaveInstanceTimeRestrictions(CharacterDatabaseTransaction trans)
{
    if (_instanceResetTimes.empty())
        return;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_INSTANCE_LOCK_TIMES);
    stmt->SetData(0, GetSession()->GetAccountId());
    trans->Append(stmt);

    for (InstanceTimeMap::const_iterator itr = _instanceResetTimes.begin(); itr != _instanceResetTimes.end(); ++itr)
    {
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ACCOUNT_INSTANCE_LOCK_TIMES);
        stmt->SetData(0, GetSession()->GetAccountId());
        stmt->SetData(1, itr->first);
        stmt->SetData(2, (int64)itr->second);
        trans->Append(stmt);
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











std::unordered_map<int, bgZoneRef> Player::bgZoneIdToFillWorldStates = {};

void Player::SetRestFlag(RestFlag restFlag, uint32 triggerId /*= 0*/)
{
    uint32 oldRestMask = _restFlagMask;
    _restFlagMask |= restFlag;

    if (!oldRestMask && _restFlagMask) // only set flag/time on the first rest state
    {
        _restTime = GameTime::GetGameTime().count();
        SetPlayerFlag(PLAYER_FLAGS_RESTING);
    }

    if (triggerId)
        _innTriggerId = triggerId;
}

void Player::RemoveRestFlag(RestFlag restFlag)
{
    uint32 oldRestMask = _restFlagMask;
    _restFlagMask &= ~restFlag;

    if (oldRestMask && !_restFlagMask) // only remove flag/time on the last rest state remove
    {
        _restTime = 0;
        RemovePlayerFlag(PLAYER_FLAGS_RESTING);
    }
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



float Player::GetQuestRate(bool isDFQuest)
{
    float result = isDFQuest ? sWorld->getRate(RATE_XP_QUEST_DF) : sWorld->getRate(RATE_XP_QUEST);

    sScriptMgr->OnPlayerGetQuestRate(this, result);

    return result;
}

void Player::SetServerSideVisibility(ServerSideVisibilityType type, AccountTypes sec)
{
    sScriptMgr->OnPlayerSetServerSideVisibility(this, type, sec);

    m_serverSideVisibility.SetValue(type, sec);
}

void Player::SetServerSideVisibilityDetect(ServerSideVisibilityType type, AccountTypes sec)
{
    sScriptMgr->OnPlayerSetServerSideVisibilityDetect(this, type, sec);

    m_serverSideVisibilityDetect.SetValue(type, sec);
}

void Player::SetFarSightDistance(float radius)
{
    _farSightDistance = radius;
}

void Player::ResetFarSightDistance()
{
    _farSightDistance.reset();
}

Optional<float> Player::GetFarSightDistance() const
{
    return _farSightDistance;
}

float Player::GetSightRange(WorldObject const* target) const
{
    float sightRange = WorldObject::GetSightRange(target);
    if (_farSightDistance)
        sightRange += *_farSightDistance;

    return sightRange;
}

bool Player::IsWorldObjectOutOfSightRange(WorldObject const* target) const
{
    // Special handling for Infinite visibility override objects -> they are zone wide visible
    if (target->GetVisibilityOverrideType() == VisibilityDistanceType::Infinite)
    {
        // Same zone, always visible
        if (target->GetZoneId() == GetZoneId())
            return false;
    }

    // Check if out of range
    if (!target->IsWithinSightRange(GetSightPosition(), GetSightRange(target)))
        return true;

    return false;
}

std::string Player::GetPlayerName()
{
    std::string name = GetName();
    std::string color = "";

    switch (getClass())
    {
        case CLASS_DEATH_KNIGHT: color = "|cffC41F3B"; break;
        case CLASS_DRUID:        color = "|cffFF7D0A"; break;
        case CLASS_HUNTER:       color = "|cffABD473"; break;
        case CLASS_MAGE:         color = "|cff69CCF0"; break;
        case CLASS_PALADIN:      color = "|cffF58CBA"; break;
        case CLASS_PRIEST:       color = "|cffFFFFFF"; break;
        case CLASS_ROGUE:        color = "|cffFFF569"; break;
        case CLASS_SHAMAN:       color = "|cff0070DE"; break;
        case CLASS_WARLOCK:      color = "|cff9482C9"; break;
        case CLASS_WARRIOR:      color = "|cffC79C6E"; break;
    }

    return "|Hplayer:" + name + "|h" + color + name + "|h|r";
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









std::string Player::GetDebugInfo() const
{
    std::stringstream sstr;
    sstr << Unit::GetDebugInfo();
    return sstr.str();
}

void Player::SendSystemMessage(std::string_view msg, bool escapeCharacters)
{
    ChatHandler(GetSession()).SendSysMessage(msg, escapeCharacters);
}
