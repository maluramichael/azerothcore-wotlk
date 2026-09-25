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

bool Player::StoreNewItemInBestSlots(uint32 titem_id, uint32 titem_amount)
{
    LOG_DEBUG("entities.player.items", "STORAGE: Creating initial item, itemId = {}, count = {}", titem_id, titem_amount);

    // attempt equip by one
    while (titem_amount > 0)
    {
        uint16 eDest;
        InventoryResult msg = CanEquipNewItem(NULL_SLOT, eDest, titem_id, false);
        if (msg != EQUIP_ERR_OK)
            break;

        EquipNewItem(eDest, titem_id, true);
        AutoUnequipOffhandIfNeed();
        --titem_amount;
    }

    if (titem_amount == 0)
        return true;                                        // equipped

    // attempt store
    ItemPosCountVec sDest;
    // store in main bag to simplify second pass (special bags can be not equipped yet at this moment)
    InventoryResult msg = CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, sDest, titem_id, titem_amount);
    if (msg == EQUIP_ERR_OK)
    {
        StoreNewItem(sDest, titem_id, true);
        return true;                                        // stored
    }

    // item can't be added
    LOG_ERROR("entities.player", "STORAGE: Can't equip or store initial item {} for race {} class {}, error msg = {}", titem_id, getRace(true), getClass(), msg);
    return false;
}

void Player::DeleteOldRecoveryItems()
{
    uint32 keepDays = sWorld->getIntConfig(CONFIG_ITEMDELETE_KEEP_DAYS);
    if (!keepDays)
        return;

    Player::DeleteOldRecoveryItems(keepDays);
}

void Player::DeleteOldRecoveryItems(uint32 keepDays)
{
    LOG_INFO("server.loading", "Player::DeleteOldRecoveryItems: Deleting all items which have been deleted {} days before...", keepDays);
    LOG_INFO("server.loading", " ");

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_RECOVERY_ITEM_OLD_ITEMS);
    stmt->SetData(0, uint32(GameTime::GetGameTime().count() - time_t(keepDays * DAY)));
    PreparedQueryResult result = CharacterDatabase.Query(stmt);

    if (result)
    {
        LOG_INFO("server.loading", "Player::DeleteOldRecoveryItems: Found {} item(s) to delete", result->GetRowCount());
        do
        {
            Field* fields = result->Fetch();

            uint32 guid = fields[0].Get<uint32>();
            uint32 itemEntry = fields[1].Get<uint32>();

            CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_RECOVERY_ITEM_BY_GUID);
            deleteStmt->SetData(0, guid);
            CharacterDatabase.Execute(deleteStmt);

            LOG_INFO("server.loading", "Deleted item from recovery_item table where guid {} and item id {}", guid, itemEntry);
        } while (result->NextRow());
    }
}

void Player::SendDurabilityLoss()
{
    SendDirectMessage(WorldPackets::Misc::DurabilityDamageDeath().Write());
}

void Player::DurabilityLossAll(double percent, bool inventory)
{
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
        if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            DurabilityLoss(pItem, percent);

    if (inventory)
    {
        // bags not have durability
        // for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++)

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; i++)
            if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                DurabilityLoss(pItem, percent);

        // keys not have durability
        //for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; i++)

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++)
            if (Bag* pBag = GetBagByPos(i))
                for (uint32 j = 0; j < pBag->GetBagSize(); j++)
                    if (Item* pItem = GetItemByPos(i, j))
                        DurabilityLoss(pItem, percent);
    }
}

void Player::DurabilityLoss(Item* item, double percent)
{
    if (!item || percent == 0.0)
        return;

    uint32 pMaxDurability = item ->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);

    if (!pMaxDurability)
        return;

    uint32 pDurabilityLoss = uint32(pMaxDurability * percent);

    if (pDurabilityLoss < 1)
        pDurabilityLoss = 1;

    DurabilityPointsLoss(item, pDurabilityLoss);
}

void Player::DurabilityPointsLossAll(int32 points, bool inventory)
{
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
        if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            DurabilityPointsLoss(pItem, points);

    if (inventory)
    {
        // bags not have durability
        // for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++)

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; i++)
            if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                DurabilityPointsLoss(pItem, points);

        // keys not have durability
        //for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; i++)

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++)
            if (Bag* pBag = (Bag*)GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                for (uint32 j = 0; j < pBag->GetBagSize(); j++)
                    if (Item* pItem = GetItemByPos(i, j))
                        DurabilityPointsLoss(pItem, points);
    }
}

void Player::DurabilityPointsLoss(Item* item, int32 points)
{
    if (HasPreventDurabilityLossAura())
        return;

    int32 pMaxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    int32 pOldDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
    int32 pNewDurability = pOldDurability - points;

    if (pNewDurability < 0)
        pNewDurability = 0;
    else if (pNewDurability > pMaxDurability)
        pNewDurability = pMaxDurability;

    if (pOldDurability != pNewDurability)
    {
        // modify item stats _before_ Durability set to 0 to pass _ApplyItemMods internal check
        if (pNewDurability == 0 && pOldDurability > 0 && item->IsEquipped())
            _ApplyItemMods(item, item->GetSlot(), false);

        item->SetUInt32Value(ITEM_FIELD_DURABILITY, pNewDurability);

        // modify item stats _after_ restore durability to pass _ApplyItemMods internal check
        if (pNewDurability > 0 && pOldDurability == 0 && item->IsEquipped())
            _ApplyItemMods(item, item->GetSlot(), true);

        item->SetState(ITEM_CHANGED, this);
    }
}

void Player::DurabilityPointLossForEquipSlot(EquipmentSlots slot)
{
    if (Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        DurabilityPointsLoss(pItem, 1);
}

uint32 Player::DurabilityRepairAll(bool cost, float discountMod, bool guildBank)
{
    uint32 TotalCost = 0;
    // equipped, backpack, bags itself
    for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; i++)
        TotalCost += DurabilityRepair(((INVENTORY_SLOT_BAG_0 << 8) | i), cost, discountMod, guildBank);

    // bank, buyback and keys not repaired

    // items in inventory bags
    for (uint8 j = INVENTORY_SLOT_BAG_START; j < INVENTORY_SLOT_BAG_END; j++)
        for (uint8 i = 0; i < MAX_BAG_SIZE; i++)
            TotalCost += DurabilityRepair(((j << 8) | i), cost, discountMod, guildBank);
    return TotalCost;
}

uint32 Player::DurabilityRepair(uint16 pos, bool cost, float discountMod, bool guildBank)
{
    Item* item = GetItemByPos(pos);

    uint32 TotalCost = 0;
    if (!item)
        return TotalCost;

    uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    if (!maxDurability)
        return TotalCost;

    uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);

    if (cost)
    {
        uint32 LostDurability = maxDurability - curDurability;
        if (LostDurability > 0)
        {
            ItemTemplate const* ditemProto = item->GetTemplate();

            DurabilityCostsEntry const* dcost = sDurabilityCostsStore.LookupEntry(ditemProto->ItemLevel);
            if (!dcost)
            {
                LOG_ERROR("entities.player", "RepairDurability: Wrong item lvl {}", ditemProto->ItemLevel);
                return TotalCost;
            }

            uint32 dQualitymodEntryId = (ditemProto->Quality + 1) * 2;
            DurabilityQualityEntry const* dQualitymodEntry = sDurabilityQualityStore.LookupEntry(dQualitymodEntryId);
            if (!dQualitymodEntry)
            {
                LOG_ERROR("entities.player", "RepairDurability: Wrong dQualityModEntry {}", dQualitymodEntryId);
                return TotalCost;
            }

            uint32 dmultiplier = dcost->multiplier[ItemSubClassToDurabilityMultiplierId(ditemProto->Class, ditemProto->SubClass)];
            uint32 costs = uint32(LostDurability * dmultiplier * double(dQualitymodEntry->quality_mod));

            costs = uint32(costs * discountMod * sWorld->getRate(RATE_REPAIRCOST));

            if (costs == 0)                                   //fix for ITEM_QUALITY_ARTIFACT
                costs = 1;

            if (guildBank)
            {
                if (GetGuildId() == 0)
                {
                    // LOG_DEBUG("entities.player", "You are not member of a guild");
                    return TotalCost;
                }

                Guild* guild = sGuildMgr->GetGuildById(GetGuildId());
                if (!guild)
                    return TotalCost;

                if (!guild->HandleMemberWithdrawMoney(GetSession(), costs, true))
                    return TotalCost;

                TotalCost = costs;
            }
            else if (!HasEnoughMoney(costs))
            {
                // LOG_DEBUG("entities.player", "You do not have enough money");
                return TotalCost;
            }
            else
                ModifyMoney(-int32(costs));
        }
    }

    item->SetUInt32Value(ITEM_FIELD_DURABILITY, maxDurability);
    item->SetState(ITEM_CHANGED, this);

    // reapply mods for total broken and repaired item if equipped
    if (IsEquipmentPos(pos) && !curDurability)
        _ApplyItemMods(item, pos & 255, true);
    return TotalCost;
}

void Player::_ApplyItemMods(Item* item, uint8 slot, bool apply)
{
    if (slot >= INVENTORY_SLOT_BAG_END || !item)
        return;

    ItemTemplate const* proto = item->GetTemplate();

    if (!proto)
        return;

    // not apply/remove mods for broken item
    if (item->IsBroken())
        return;

    LOG_DEBUG("entities.player", "applying mods for item {} ", item->GetGUID().ToString());

    if (item->HasSocket())                              //only (un)equipping of items with sockets can influence metagems, so no need to waste time with normal items
        CorrectMetaGemEnchants(slot, apply);

    _ApplyItemBonuses(proto, slot, apply);

    if (slot == EQUIPMENT_SLOT_RANGED)
        _ApplyAmmoBonuses();

    ApplyItemEquipSpell(item, apply);

    ApplyItemDependentAuras(item, apply);

    WeaponAttackType const attackType = Player::GetAttackBySlot(slot);
    if (attackType != MAX_ATTACK)
        UpdateWeaponDependentAuras(attackType);

    ApplyEnchantment(item, apply);

    LOG_DEBUG("entities.player.items", "_ApplyItemMods complete.");
}

void Player::_ApplyItemBonuses(ItemTemplate const* proto, uint8 slot, bool apply, bool only_level_scale /*= false*/)
{
    if (slot >= INVENTORY_SLOT_BAG_END || !proto)
        return;

    ScalingStatDistributionEntry const* ssd = proto->ScalingStatDistribution ? sScalingStatDistributionStore.LookupEntry(proto->ScalingStatDistribution) : nullptr;
    if (only_level_scale && !ssd)
        return;

    // req. check at equip, but allow use for extended range if range limit max level, set proper level
    uint32 ssd_level = GetLevel();
    uint32 CustomScalingStatValue = 0;

    sScriptMgr->OnPlayerCustomScalingStatValueBefore(this, proto, slot, apply, CustomScalingStatValue);

    uint32 ScalingStatValue = proto->ScalingStatValue > 0 ? proto->ScalingStatValue : CustomScalingStatValue;

    if (ssd && ssd_level > ssd->MaxLevel)
        ssd_level = ssd->MaxLevel;

    ScalingStatValuesEntry const* ssv = proto->ScalingStatValue ? sScalingStatValuesStore.LookupEntry(ssd_level) : nullptr;
    if (only_level_scale && !ssv)
        return;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        uint32 statType = 0;
        int32  val = 0;
        // If set ScalingStatDistribution need get stats and values from it
        if (ssv)
        {
            if (ssd)
            {
                if (ssd->StatMod[i] < 0)
                    continue;

                statType = ssd->StatMod[i];
                val = (ssv->getssdMultiplier(ScalingStatValue) * ssd->Modifier[i]) / 10000;
            }
            else
            {
                if (i >= proto->StatsCount)
                    continue;

                // OnCustomScalingStatValue(Player* player, ItemTemplate const* proto, uint32& statType, int32& val, uint8 itemProtoStatNumber, uint32 ScalingStatValue, ScalingStatValuesEntry const* ssv)
                sScriptMgr->OnPlayerCustomScalingStatValue(this, proto, statType, val, i, ScalingStatValue, ssv);
            }
        }
        else
        {
            if (i >= proto->StatsCount)
                continue;

            statType = proto->ItemStat[i].ItemStatType;
            val = proto->ItemStat[i].ItemStatValue;

            sScriptMgr->OnPlayerApplyItemModsBefore(this, slot, apply, i, statType, val);
        }

        if (val == 0)
            continue;

        switch (statType)
        {
            case ITEM_MOD_MANA:
                HandleStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, float(val), apply);
                break;
            case ITEM_MOD_HEALTH:                           // modify HP
                HandleStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(val), apply);
                break;
            case ITEM_MOD_AGILITY:                          // modify agility
                HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, BASE_VALUE, float(val), apply);
                UpdateStatBuffMod(STAT_AGILITY);
                break;
            case ITEM_MOD_STRENGTH:                         //modify strength
                HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH, BASE_VALUE, float(val), apply);
                UpdateStatBuffMod(STAT_STRENGTH);
                break;
            case ITEM_MOD_INTELLECT:                        //modify intellect
                HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, BASE_VALUE, float(val), apply);
                UpdateStatBuffMod(STAT_INTELLECT);
                break;
            case ITEM_MOD_SPIRIT:                           //modify spirit
                HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, BASE_VALUE, float(val), apply);
                UpdateStatBuffMod(STAT_SPIRIT);
                break;
            case ITEM_MOD_STAMINA:                          //modify stamina
                HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA, BASE_VALUE, float(val), apply);
                UpdateStatBuffMod(STAT_STAMINA);
                break;
            case ITEM_MOD_DEFENSE_SKILL_RATING:
                ApplyRatingMod(CR_DEFENSE_SKILL, int32(val), apply);
                break;
            case ITEM_MOD_DODGE_RATING:
                ApplyRatingMod(CR_DODGE, int32(val), apply);
                break;
            case ITEM_MOD_PARRY_RATING:
                ApplyRatingMod(CR_PARRY, int32(val), apply);
                break;
            case ITEM_MOD_BLOCK_RATING:
                ApplyRatingMod(CR_BLOCK, int32(val), apply);
                break;
            case ITEM_MOD_HIT_MELEE_RATING:
                ApplyRatingMod(CR_HIT_MELEE, int32(val), apply);
                break;
            case ITEM_MOD_HIT_RANGED_RATING:
                ApplyRatingMod(CR_HIT_RANGED, int32(val), apply);
                break;
            case ITEM_MOD_HIT_SPELL_RATING:
                ApplyRatingMod(CR_HIT_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_MELEE_RATING:
                ApplyRatingMod(CR_CRIT_MELEE, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_RANGED_RATING:
                ApplyRatingMod(CR_CRIT_RANGED, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_SPELL_RATING:
                ApplyRatingMod(CR_CRIT_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_HIT_TAKEN_MELEE_RATING:
                ApplyRatingMod(CR_HIT_TAKEN_MELEE, int32(val), apply);
                break;
            case ITEM_MOD_HIT_TAKEN_RANGED_RATING:
                ApplyRatingMod(CR_HIT_TAKEN_RANGED, int32(val), apply);
                break;
            case ITEM_MOD_HIT_TAKEN_SPELL_RATING:
                ApplyRatingMod(CR_HIT_TAKEN_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_TAKEN_MELEE_RATING:
                ApplyRatingMod(CR_CRIT_TAKEN_MELEE, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_TAKEN_RANGED_RATING:
                ApplyRatingMod(CR_CRIT_TAKEN_RANGED, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_TAKEN_SPELL_RATING:
                ApplyRatingMod(CR_CRIT_TAKEN_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_HASTE_MELEE_RATING:
                ApplyRatingMod(CR_HASTE_MELEE, int32(val), apply);
                break;
            case ITEM_MOD_HASTE_RANGED_RATING:
                ApplyRatingMod(CR_HASTE_RANGED, int32(val), apply);
                break;
            case ITEM_MOD_HASTE_SPELL_RATING:
                ApplyRatingMod(CR_HASTE_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_HIT_RATING:
                ApplyRatingMod(CR_HIT_MELEE, int32(val), apply);
                ApplyRatingMod(CR_HIT_RANGED, int32(val), apply);
                ApplyRatingMod(CR_HIT_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_RATING:
                ApplyRatingMod(CR_CRIT_MELEE, int32(val), apply);
                ApplyRatingMod(CR_CRIT_RANGED, int32(val), apply);
                ApplyRatingMod(CR_CRIT_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_HIT_TAKEN_RATING:
                ApplyRatingMod(CR_HIT_TAKEN_MELEE, int32(val), apply);
                ApplyRatingMod(CR_HIT_TAKEN_RANGED, int32(val), apply);
                ApplyRatingMod(CR_HIT_TAKEN_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_CRIT_TAKEN_RATING:
            case ITEM_MOD_RESILIENCE_RATING:
                ApplyRatingMod(CR_CRIT_TAKEN_MELEE, int32(val), apply);
                ApplyRatingMod(CR_CRIT_TAKEN_RANGED, int32(val), apply);
                ApplyRatingMod(CR_CRIT_TAKEN_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_HASTE_RATING:
                ApplyRatingMod(CR_HASTE_MELEE, int32(val), apply);
                ApplyRatingMod(CR_HASTE_RANGED, int32(val), apply);
                ApplyRatingMod(CR_HASTE_SPELL, int32(val), apply);
                break;
            case ITEM_MOD_EXPERTISE_RATING:
                ApplyRatingMod(CR_EXPERTISE, int32(val), apply);
                break;
            case ITEM_MOD_ATTACK_POWER:
                HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, float(val), apply);
                HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, float(val), apply);
                break;
            case ITEM_MOD_RANGED_ATTACK_POWER:
                HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, float(val), apply);
                break;
            //            case ITEM_MOD_FERAL_ATTACK_POWER:
            //                ApplyFeralAPBonus(int32(val), apply);
            //                break;
            case ITEM_MOD_MANA_REGENERATION:
                ApplyManaRegenBonus(int32(val), apply);
                break;
            case ITEM_MOD_ARMOR_PENETRATION_RATING:
                ApplyRatingMod(CR_ARMOR_PENETRATION, int32(val), apply);
                break;
            case ITEM_MOD_SPELL_POWER:
                ApplySpellPowerBonus(int32(val), apply);
                break;
            case ITEM_MOD_HEALTH_REGEN:
                ApplyHealthRegenBonus(int32(val), apply);
                break;
            case ITEM_MOD_SPELL_PENETRATION:
                ApplySpellPenetrationBonus(val, apply);
                break;
            case ITEM_MOD_BLOCK_VALUE:
                HandleBaseModFlatValue(SHIELD_BLOCK_VALUE, float(val), apply);
                break;
            /// @deprecated item mods
            case ITEM_MOD_SPELL_HEALING_DONE:
                ApplySpellHealingBonus(int32(val), apply);
                break;
            case ITEM_MOD_SPELL_DAMAGE_DONE:
                ApplySpellDamageBonus(int32(val), apply);
                break;
        }
    }

    // Apply Spell Power from ScalingStatValue if set
    if (ssv)
        if (int32 spellbonus = ssv->getSpellBonus(ScalingStatValue))
            ApplySpellPowerBonus(spellbonus, apply);

    // If set ScalingStatValue armor get it or use item armor
    uint32 armor = proto->Armor;
    if (ssv)
    {
        if (uint32 ssvarmor = ssv->getArmorMod(ScalingStatValue))
            if (proto->ScalingStatValue > 0 || ssvarmor < proto->Armor) //Check to avoid higher values than stat itself (heirloom OR items with correct armor value)
                armor = ssvarmor;
    }
    else if (armor && proto->ArmorDamageModifier)
        armor -= uint32(proto->ArmorDamageModifier);

    if (armor)
    {
        UnitModifierFlatType modType = TOTAL_VALUE;
        if (proto->Class == ITEM_CLASS_ARMOR)
        {
            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_ARMOR_CLOTH:
                case ITEM_SUBCLASS_ARMOR_LEATHER:
                case ITEM_SUBCLASS_ARMOR_MAIL:
                case ITEM_SUBCLASS_ARMOR_PLATE:
                case ITEM_SUBCLASS_ARMOR_SHIELD:
                    modType = BASE_VALUE;
                    break;
            }
        }
        HandleStatFlatModifier(UNIT_MOD_ARMOR, modType, float(armor), apply);
    }

    // Add armor bonus from ArmorDamageModifier if > 0
    if (proto->ArmorDamageModifier > 0 && sScriptMgr->OnPlayerCanArmorDamageModifier(this))
        HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, float(proto->ArmorDamageModifier), apply);

    if (proto->Block)
        HandleBaseModFlatValue(SHIELD_BLOCK_VALUE, float(proto->Block), apply);

    if (proto->HolyRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_HOLY, BASE_VALUE, float(proto->HolyRes), apply);

    if (proto->FireRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_FIRE, BASE_VALUE, float(proto->FireRes), apply);

    if (proto->NatureRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_NATURE, BASE_VALUE, float(proto->NatureRes), apply);

    if (proto->FrostRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_FROST, BASE_VALUE, float(proto->FrostRes), apply);

    if (proto->ShadowRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_SHADOW, BASE_VALUE, float(proto->ShadowRes), apply);

    if (proto->ArcaneRes)
        HandleStatFlatModifier(UNIT_MOD_RESISTANCE_ARCANE, BASE_VALUE, float(proto->ArcaneRes), apply);

    WeaponAttackType attType = Player::GetAttackBySlot(slot);
    if (attType != MAX_ATTACK)
    {
        _ApplyWeaponDamage(slot, proto, ssv, apply);
    }

    // Druids get feral AP bonus from weapon dps (also use DPS from ScalingStatValue)
    if (IsClass(CLASS_DRUID, CLASS_CONTEXT_STATS))
    {
        int32 dpsMod = 0;
        int32 feral_bonus = 0;
        if (ssv)
        {
            dpsMod = ssv->getDPSMod(ScalingStatValue);
            feral_bonus += ssv->getFeralBonus(ScalingStatValue);
        }

        feral_bonus += proto->getFeralBonus(dpsMod);
        sScriptMgr->OnPlayerGetFeralApBonus(this, feral_bonus, dpsMod, proto, ssv);
        if (feral_bonus)
            ApplyFeralAPBonus(feral_bonus, apply);
    }
}

void Player::_RemoveAllItemMods()
{
    LOG_DEBUG("entities.player.items", "_RemoveAllItemMods start.");

    for (uint8 i = 0; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (m_items[i])
        {
            ItemTemplate const* proto = m_items[i]->GetTemplate();
            if (!proto)
                continue;

            // item set bonuses not dependent from item broken state
            if (proto->ItemSet)
                RemoveItemsSetItem(this, proto);

            if (m_items[i]->IsBroken() || !CanUseAttackType(GetAttackBySlot(i)))
                continue;

            ApplyItemEquipSpell(m_items[i], false);
            ApplyEnchantment(m_items[i], false);
        }
    }

    for (uint8 i = 0; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (m_items[i])
        {
            if (m_items[i]->IsBroken() || !CanUseAttackType(GetAttackBySlot(i)))
                continue;
            ItemTemplate const* proto = m_items[i]->GetTemplate();
            if (!proto)
                continue;

            ApplyItemDependentAuras(m_items[i], false);
            _ApplyItemBonuses(proto, i, false);

            if (i == EQUIPMENT_SLOT_RANGED)
                _ApplyAmmoBonuses();
        }
    }

    LOG_DEBUG("entities.player.items", "_RemoveAllItemMods complete.");
}

void Player::_ApplyAllItemMods()
{
    LOG_DEBUG("entities.player.items", "_ApplyAllItemMods start.");

    for (uint8 i = 0; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (m_items[i])
        {
            if (m_items[i]->IsBroken() || !CanUseAttackType(GetAttackBySlot(i)))
                continue;

            ItemTemplate const* proto = m_items[i]->GetTemplate();
            if (!proto)
                continue;

            ApplyItemDependentAuras(m_items[i], true);
            _ApplyItemBonuses(proto, i, true);

            WeaponAttackType const attackType = Player::GetAttackBySlot(i);
            if (attackType != MAX_ATTACK)
                UpdateWeaponDependentAuras(attackType);

            if (i == EQUIPMENT_SLOT_RANGED)
                _ApplyAmmoBonuses();
        }
    }

    for (uint8 i = 0; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (m_items[i])
        {
            ItemTemplate const* proto = m_items[i]->GetTemplate();
            if (!proto)
                continue;

            // item set bonuses not dependent from item broken state
            if (proto->ItemSet)
                AddItemsSetItem(this, m_items[i]);

            if (m_items[i]->IsBroken() || !CanUseAttackType(GetAttackBySlot(i)))
                continue;

            ApplyItemEquipSpell(m_items[i], true);
            ApplyEnchantment(m_items[i], true);
        }
    }

    LOG_DEBUG("entities.player.items", "_ApplyAllItemMods complete.");
}

void Player::_ApplyAllLevelScaleItemMods(bool apply)
{
    for (uint8 i = 0; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (m_items[i])
        {
            if (m_items[i]->IsBroken() || !CanUseAttackType(GetAttackBySlot(i)))
                continue;

            ItemTemplate const* proto = m_items[i]->GetTemplate();
            if (!proto)
                continue;

            _ApplyItemMods(m_items[i], i, apply);
        }
    }
}

void Player::_ApplyAmmoBonuses()
{
    // check ammo
    uint32 ammo_id = GetUInt32Value(PLAYER_AMMO_ID);
    if (!ammo_id)
        return;

    float currentAmmoDPS;

    ItemTemplate const* ammo_proto = sObjectMgr->GetItemTemplate(ammo_id);
    if (!ammo_proto || ammo_proto->Class != ITEM_CLASS_PROJECTILE || !CheckAmmoCompatibility(ammo_proto))
        currentAmmoDPS = 0.0f;
    else
        currentAmmoDPS = (ammo_proto->Damage[0].DamageMin + ammo_proto->Damage[0].DamageMax) / 2;

    sScriptMgr->OnPlayerApplyAmmoBonuses(this, ammo_proto, currentAmmoDPS);

    if (currentAmmoDPS == GetAmmoDPS())
        return;

    m_ammoDPS = currentAmmoDPS;

    if (CanModifyStats())
        UpdateDamagePhysical(RANGED_ATTACK);
}

bool Player::CheckAmmoCompatibility(ItemTemplate const* ammo_proto) const
{
    if (!ammo_proto)
        return false;

    // check ranged weapon
    Item* weapon = GetWeaponForAttack(RANGED_ATTACK);
    if (!weapon  || weapon->IsBroken())
        return false;

    ItemTemplate const* weapon_proto = weapon->GetTemplate();
    if (!weapon_proto || weapon_proto->Class != ITEM_CLASS_WEAPON)
        return false;

    // check ammo ws. weapon compatibility
    switch (weapon_proto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            if (ammo_proto->SubClass != ITEM_SUBCLASS_ARROW)
                return false;
            break;
        case ITEM_SUBCLASS_WEAPON_GUN:
            if (ammo_proto->SubClass != ITEM_SUBCLASS_BULLET)
                return false;
            break;
        default:
            return false;
    }

    return true;
}

void Player::SendLootRelease(ObjectGuid guid)
{
    WorldPacket data(SMSG_LOOT_RELEASE_RESPONSE, (8 + 1));
    data << guid << uint8(1);
    SendDirectMessage(&data);
}

void Player::SendLoot(ObjectGuid guid, LootType loot_type)
{
    if (ObjectGuid lguid = GetLootGUID())
        m_session->DoLootRelease(lguid);

    Loot* loot = 0;
    PermissionTypes permission = ALL_PERMISSION;

    LOG_DEBUG("loot", "Player::SendLoot");

    // remove FD and invisibility at all loots
    constexpr std::array<AuraType, 2> toRemove = {SPELL_AURA_MOD_INVISIBILITY, SPELL_AURA_FEIGN_DEATH};
    for (auto const& aura : toRemove)
    {
        RemoveAurasByType(aura);
    }
    // remove stealth only if looting a corpse
    if (loot_type == LOOT_CORPSE && !guid.IsItem())
    {
        RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
    }

    if (guid.IsGameObject())
    {
        LOG_DEBUG("loot", "guid.IsGameObject");
        GameObject* go = GetMap()->GetGameObject(guid);

        // not check distance for GO in case owned GO (fishing bobber case, for example)
        // And permit out of range GO with no owner in case fishing hole
        if (!go || (loot_type != LOOT_FISHINGHOLE && ((loot_type != LOOT_FISHING && loot_type != LOOT_FISHING_JUNK) || go->GetOwnerGUID() != GetGUID()) && !go->IsWithinDistInMap(this)) || (loot_type == LOOT_CORPSE && go->GetRespawnTime() && go->isSpawnedByDefault()))
        {
            if (go)
                go->ForceValuesUpdateAtIndex(GAMEOBJECT_BYTES_1);

            SendLootRelease(guid);
            return;
        }

        loot = &go->loot;

        // Xinef: loot was generated and respawntime has passed since then, allow to recreate loot
        // Xinef: to avoid bugs, this rule covers spawned gameobjects only
        if (go->isSpawnedByDefault() && go->getLootState() == GO_ACTIVATED && !go->loot.isLooted() && go->GetLootGenerationTime() + go->GetRespawnDelay() < GameTime::GetGameTime().count())
            go->SetLootState(GO_READY);

        if (go->getLootState() == GO_READY)
        {
            uint32 lootid = go->GetGOInfo()->GetLootId();

            //TODO: fix this big hack
            if ((go->GetEntry() == BG_AV_OBJECTID_MINE_N || go->GetEntry() == BG_AV_OBJECTID_MINE_S))
                if (Battleground* bg = GetBattleground())
                    if (bg->GetBgTypeID(true) == BATTLEGROUND_AV)
                        if (!bg->ToBattlegroundAV()->PlayerCanDoMineQuest(go->GetEntry(), GetTeamId()))
                        {
                            go->ForceValuesUpdateAtIndex(GAMEOBJECT_BYTES_1);
                            SendLootRelease(guid);
                            return;
                        }

            if (lootid)
            {
                loot->clear();

                Group* group = GetGroup();
                bool groupRules = (group && go->GetGOInfo()->type == GAMEOBJECT_TYPE_CHEST && go->GetGOInfo()->chest.groupLootRules);

                // check current RR player and get next if necessary
                if (groupRules)
                    group->UpdateLooterGuid(go, true);

                loot->FillLoot(lootid, LootTemplates_Gameobject, this, !groupRules, false, go->GetLootMode(), go);
                go->SetLootGenerationTime();

                // get next RR player (for next loot)
                if (groupRules && !go->loot.empty())
                    group->UpdateLooterGuid(go);

                if (groupRules)
                {
                    GuidUnorderedSet const& allowedLooters = go->GetAllowedLooters();
                    if (!allowedLooters.empty())
                        for (ObjectGuid const& allowedGuid : allowedLooters)
                            if (Player* allowedPlayer = ObjectAccessor::FindPlayer(allowedGuid))
                                loot->FillNotNormalLootFor(allowedPlayer);
                }
            }
            if (GameObjectTemplateAddon const* addon = go->GetTemplateAddon())
                loot->generateMoneyLoot(addon->mingold, addon->maxgold);

            if (loot_type == LOOT_FISHING)
                go->GetFishLoot(loot, this);
            else if (loot_type == LOOT_FISHING_JUNK)
                go->GetFishLoot(loot, this, true);

            if (go->GetGOInfo()->type == GAMEOBJECT_TYPE_CHEST && go->GetGOInfo()->chest.groupLootRules)
            {
                if (Group* group = GetGroup())
                {
                    switch (group->GetLootMethod())
                    {
                        case GROUP_LOOT:
                            // GroupLoot: rolls items over threshold. Items with quality < threshold, round robin
                            group->GroupLoot(loot, go);
                            break;
                        case NEED_BEFORE_GREED:
                            group->NeedBeforeGreed(loot, go);
                            break;
                        case MASTER_LOOT:
                            group->MasterLoot(loot, go);
                            break;
                        default:
                            break;
                    }
                }
            }

            go->SetLootState(GO_ACTIVATED, this);

            // Trigger chest traps once per loot generation, including when the generated loot is empty.
            if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
                if (uint32 trapEntry = go->GetGOInfo()->chest.linkedTrapId)
                    go->TriggeringLinkedGameObject(trapEntry, this);
        }

        if (go->getLootState() == GO_ACTIVATED)
        {
            if (Group* group = GetGroup())
            {
                switch (group->GetLootMethod())
                {
                    case MASTER_LOOT:
                        permission = group->GetMasterLooterGuid() == GetGUID() ? MASTER_PERMISSION : RESTRICTED_PERMISSION;
                        break;
                    case FREE_FOR_ALL:
                        permission = ALL_PERMISSION;
                        break;
                    case ROUND_ROBIN:
                        permission = ROUND_ROBIN_PERMISSION;
                        break;
                    default:
                        permission = GROUP_PERMISSION;
                        break;
                }
            }
            else
                permission = ALL_PERMISSION;
        }
    }
    else if (guid.IsItem())
    {
        Item* item = GetItemByGuid(guid);

        if (!item)
        {
            SendLootRelease(guid);
            return;
        }

        permission = OWNER_PERMISSION;

        loot = &item->loot;

        // Xinef: Store container id
        loot->containerGUID = item->GetGUID();

        if (!item->m_lootGenerated && !sLootItemStorage->LoadStoredLoot(item, this))
        {
            item->m_lootGenerated = true;
            loot->clear();

            switch (loot_type)
            {
                case LOOT_DISENCHANTING:
                    loot->FillLoot(item->GetTemplate()->DisenchantID, LootTemplates_Disenchant, this, true);
                    break;
                case LOOT_PROSPECTING:
                    loot->FillLoot(item->GetEntry(), LootTemplates_Prospecting, this, true);
                    break;
                case LOOT_MILLING:
                    loot->FillLoot(item->GetEntry(), LootTemplates_Milling, this, true);
                    break;
                default:
                    loot->generateMoneyLoot(item->GetTemplate()->MinMoneyLoot, item->GetTemplate()->MaxMoneyLoot);
                    loot->FillLoot(item->GetEntry(), LootTemplates_Item, this, true, loot->gold != 0);

                    // Xinef: Add to storage
                    if (loot->gold > 0 || loot->unlootedCount > 0)
                        sLootItemStorage->AddNewStoredLoot(loot, this);

                    break;
            }
        }
    }
    else if (guid.IsCorpse())                          // remove insignia
    {
        Corpse* bones = ObjectAccessor::GetCorpse(*this, guid);

        if (!bones || !(loot_type == LOOT_CORPSE || loot_type == LOOT_INSIGNIA) || bones->GetType() != CORPSE_BONES || !bones->HasFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE))
        {
            SendLootRelease(guid);
            return;
        }

        loot = &bones->loot;

        if (loot->loot_type == LOOT_NONE)
        {
            uint32 pLevel = bones->loot.gold;
            bones->loot.clear();

            loot->FillLoot(GetTeamId(), LootTemplates_Player, this, true);

            // It may need a better formula
            // Now it works like this: lvl10: ~6copper, lvl70: ~9silver
            bones->loot.gold = uint32(urand(50, 150) * 0.016f * pow(float(pLevel) / 5.76f, 2.5f) * sWorld->getRate(RATE_DROP_MONEY));
        }

        if (bones->lootRecipient != this)
            permission = NONE_PERMISSION;
        else
            permission = OWNER_PERMISSION;
    }
    else
    {
        Creature* creature = GetMap()->GetCreature(guid);

        // must be in range and creature must be alive for pickpocket and must be dead for another loot
        if (!creature || creature->IsAlive() != (loot_type == LOOT_PICKPOCKETING) || !creature->IsWithinDistInMap(this, INTERACTION_DISTANCE))
        {
            SendLootRelease(guid);
            return;
        }

        if (loot_type == LOOT_PICKPOCKETING && IsFriendlyTo(creature))
        {
            SendLootRelease(guid);
            return;
        }

        loot = &creature->loot;

        if (loot_type == LOOT_PICKPOCKETING)
        {
            if (!loot || loot->loot_type != LOOT_PICKPOCKETING)
            {
                if (creature->CanGeneratePickPocketLoot())
                {
                    creature->SetPickPocketLootTime();
                    loot->clear();

                    if (uint32 lootid = creature->GetCreatureTemplate()->pickpocketLootId)
                        loot->FillLoot(lootid, LootTemplates_Pickpocketing, this, true);

                    // Generate extra money for pick pocket loot
                    const uint32 a = urand(0, creature->GetLevel() / 2);
                    const uint32 b = urand(0, GetLevel() / 2);
                    loot->gold = uint32(10 * (a + b) * sWorld->getRate(RATE_DROP_MONEY));
                    permission = OWNER_PERMISSION;
                }
                else
                {
                    permission = NONE_PERMISSION;
                    SendLootError(guid, LOOT_ERROR_ALREADY_PICKPOCKETED);
                    return;
                }
            }
        }
        else
        {
            // Xinef: Exploit fix
            if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
            {
                SendLootError(guid, LOOT_ERROR_DIDNT_KILL);
                return;
            }

            // the player whose group may loot the corpse
            Player* recipient = creature->GetLootRecipient();
            Group* recipientGroup = creature->GetLootRecipientGroup();
            if (!recipient && !recipientGroup)
                return;

            if (loot->loot_type == LOOT_NONE)
            {
                // for creature, loot is filled when creature is killed.
                if (recipientGroup)
                {
                    switch (recipientGroup->GetLootMethod())
                    {
                        case GROUP_LOOT:
                            // GroupLoot: rolls items over threshold. Items with quality < threshold, round robin
                            recipientGroup->GroupLoot(loot, creature);
                            break;
                        case NEED_BEFORE_GREED:
                            recipientGroup->NeedBeforeGreed(loot, creature);
                            break;
                        case MASTER_LOOT:
                            recipientGroup->MasterLoot(loot, creature);
                            break;
                        default:
                            break;
                    }
                }
            }

            // if loot is already skinning loot then don't do anything else
            if (loot->loot_type == LOOT_SKINNING)
            {
                loot_type = LOOT_SKINNING;
                permission = creature->GetLootRecipientGUID() == GetGUID() ? OWNER_PERMISSION : NONE_PERMISSION;
            }
            else if (loot_type == LOOT_SKINNING)
            {
                loot->clear();
                loot->FillLoot(creature->GetCreatureTemplate()->SkinLootId, LootTemplates_Skinning, this, true);
                permission = OWNER_PERMISSION;

                //Inform instance if creature is skinned.
                if (InstanceScript* mapInstance = creature->GetInstanceScript())
                {
                    mapInstance->CreatureLooted(creature, LOOT_SKINNING);
                }

                // Xinef: Set new loot recipient
                creature->SetLootRecipient(this, false);
            }
            // set group rights only for loot_type != LOOT_SKINNING
            else
            {
                if (recipientGroup)
                {
                    if (GetGroup() == recipientGroup)
                    {
                        switch (recipientGroup->GetLootMethod())
                        {
                            case MASTER_LOOT:
                                permission = recipientGroup->GetMasterLooterGuid() == GetGUID() ? MASTER_PERMISSION : RESTRICTED_PERMISSION;
                                break;
                            case FREE_FOR_ALL:
                                permission = ALL_PERMISSION;
                                break;
                            case ROUND_ROBIN:
                                permission = ROUND_ROBIN_PERMISSION;
                                break;
                            default:
                                permission = GROUP_PERMISSION;
                                break;
                        }
                    }
                    else
                        permission = NONE_PERMISSION;
                }
                else if (recipient == this)
                    permission = OWNER_PERMISSION;
                else
                    permission = NONE_PERMISSION;
            }
        }
    }

    // LOOT_INSIGNIA and LOOT_FISHINGHOLE unsupported by client
    switch (loot_type)
    {
        case LOOT_INSIGNIA:
            loot_type = LOOT_SKINNING;
            break;
        case LOOT_FISHINGHOLE:
            loot_type = LOOT_FISHING;
            break;
        case LOOT_FISHING_JUNK:
            loot_type = LOOT_FISHING;
            break;
        default:
            break;
    }

    // need know merged fishing/corpse loot type for achievements
    loot->loot_type = loot_type;

    if (!sScriptMgr->OnAllowedToLootContainerCheck(this, guid))
    {
        SendLootError(guid, LOOT_ERROR_DIDNT_KILL);
        return;
    }

    if (permission != NONE_PERMISSION)
    {
        SetLootGUID(guid);

        sScriptMgr->OnPlayerBeforeSendLoot(this, guid, loot);

        WorldPacket data(SMSG_LOOT_RESPONSE, (9 + 50));         // we guess size
        data << guid;
        data << uint8(loot_type);
        data << LootView(*loot, this, permission);

        SendDirectMessage(&data);

        // add 'this' player as one of the players that are looting 'loot'
        loot->AddLooter(GetGUID());

        if (loot_type == LOOT_CORPSE && !guid.IsItem())
            SetUnitFlag(UNIT_FLAG_LOOTING);
    }
    else
        SendLootError(guid, LOOT_ERROR_DIDNT_KILL);
}

void Player::SendLootError(ObjectGuid guid, LootError error)
{
    WorldPacket data(SMSG_LOOT_RESPONSE, 10);
    data << guid;
    data << uint8(LOOT_NONE);
    data << uint8(error);
    SendDirectMessage(&data);
}

void Player::SendNotifyLootMoneyRemoved()
{
    WorldPacket data(SMSG_LOOT_CLEAR_MONEY, 0);
    SendDirectMessage(&data);
}

void Player::SendNotifyLootItemRemoved(uint8 lootSlot)
{
    WorldPacket data(SMSG_LOOT_REMOVED, 1);
    data << uint8(lootSlot);
    SendDirectMessage(&data);
}

inline bool Player::_StoreOrEquipNewItem(uint32 vendorslot, uint32 item, uint8 count, uint8 bag, uint8 slot, int32 price, ItemTemplate const* pProto, Creature* pVendor, VendorItem const* crItem, bool bStore)
{
    ItemPosCountVec vDest;
    uint16 uiDest = 0;
    InventoryResult msg = bStore ?
                          CanStoreNewItem(bag, slot, vDest, item, pProto->BuyCount * count) :
                          CanEquipNewItem(slot, uiDest, item, false);
    if (msg != EQUIP_ERR_OK)
    {
        SendEquipError(msg, nullptr, nullptr, item);
        return false;
    }

    ModifyMoney(-price);

    if (crItem->ExtendedCost)                            // case for new honor system
    {
        ItemExtendedCostEntry const* iece = sItemExtendedCostStore.LookupEntry(crItem->ExtendedCost);
        if (iece->reqhonorpoints)
            ModifyHonorPoints(- int32(iece->reqhonorpoints * count));

        if (iece->reqarenapoints)
            ModifyArenaPoints(- int32(iece->reqarenapoints * count));

        for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
        {
            if (iece->reqitem[i])
                DestroyItemCount(iece->reqitem[i], (iece->reqitemcount[i] * count), true);
        }
    }

    sScriptMgr->OnPlayerBeforeStoreOrEquipNewItem(this, vendorslot, item, count, bag, slot, pProto, pVendor, crItem, bStore);

    Item* it = bStore ? StoreNewItem(vDest, item, true) : EquipNewItem(uiDest, item, true);
    if (it)
    {
        uint32 new_count = pVendor->UpdateVendorItemCurrentCount(crItem, pProto->BuyCount * count);

        WorldPacket data(SMSG_BUY_ITEM, (8 + 4 + 4 + 4));
        data << pVendor->GetGUID();
        data << uint32(vendorslot + 1);                   // numbered from 1 at client
        data << int32(crItem->maxcount > 0 ? new_count : 0xFFFFFFFF);
        data << uint32(count);
        SendDirectMessage(&data);
        SendNewItem(it, pProto->BuyCount * count, true, false, false);

        if (!bStore)
            AutoUnequipOffhandIfNeed();

        if (pProto->HasFlag(ITEM_FLAG_ITEM_PURCHASE_RECORD) && crItem->ExtendedCost && pProto->GetMaxStackSize() == 1)
        {
            it->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
            it->SetRefundRecipient(GetGUID().GetCounter());
            it->SetPaidMoney(price);
            it->SetPaidExtendedCost(crItem->ExtendedCost);
            it->SaveRefundDataToDB();
            AddRefundReference(it->GetGUID());
        }
    }

    sScriptMgr->OnPlayerAfterStoreOrEquipNewItem(this, vendorslot, it, count, bag, slot, pProto, pVendor, crItem, bStore);

    return true;
}

bool Player::BuyItemFromVendorSlot(ObjectGuid vendorguid, uint32 vendorslot, uint32 item, uint8 count, uint8 bag, uint8 slot)
{
    sScriptMgr->OnPlayerBeforeBuyItemFromVendor(this, vendorguid, vendorslot, item, count, bag, slot);

    // this check can be used from the hook to implement a custom vendor process
    if (item == 0)
        return true;

    // cheating attempt
    if (count < 1) count = 1;

    // cheating attempt
    if (slot > MAX_BAG_SIZE && slot != NULL_SLOT)
        return false;

    if (!IsAlive())
        return false;

    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(item);
    if (!pProto)
    {
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, nullptr, item, 0);
        return false;
    }

    if (!(pProto->AllowableClass & getClassMask()) && pProto->Bonding == BIND_WHEN_PICKED_UP && !IsGameMaster())
    {
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, nullptr, item, 0);
        return false;
    }

    if (!IsGameMaster() && ((pProto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && GetTeamId(true) == TEAM_ALLIANCE) || (pProto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && GetTeamId(true) == TEAM_HORDE)))
    {
        return false;
    }

    Creature* creature = GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: BuyItemFromVendor - Unit ({}) not found or you can't interact with him.", vendorguid.ToString());
        SendBuyError(BUY_ERR_DISTANCE_TOO_FAR, nullptr, item, 0);
        return false;
    }

    ConditionList conditions = sConditionMgr->GetConditionsForNpcVendorEvent(creature->GetEntry(), item);
    if (!sConditionMgr->IsObjectMeetToConditions(this, creature, conditions))
    {
        //LOG_DEBUG("condition", "BuyItemFromVendor: conditions not met for creature entry {} item {}", creature->GetEntry(), item);
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, creature, item, 0);
        return false;
    }

    VendorItemData const* vItems = GetSession()->GetCurrentVendor() ? sObjectMgr->GetNpcVendorItemList(GetSession()->GetCurrentVendor()) : creature->GetVendorItems();
    if (!vItems || vItems->Empty())
    {
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, creature, item, 0);
        return false;
    }

    if (vendorslot >= vItems->GetItemCount())
    {
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, creature, item, 0);
        return false;
    }

    VendorItem const* crItem = vItems->GetItem(vendorslot);
    // store diff item (cheating)
    if (!crItem || crItem->item != item)
    {
        SendBuyError(BUY_ERR_CANT_FIND_ITEM, creature, item, 0);
        return false;
    }

    // check current item amount if it limited
    if (crItem->maxcount != 0)
    {
        if (creature->GetVendorItemCurrentCount(crItem) < pProto->BuyCount * count)
        {
            SendBuyError(BUY_ERR_ITEM_ALREADY_SOLD, creature, item, 0);
            return false;
        }
    }

    if (pProto->RequiredReputationFaction && (uint32(GetReputationRank(pProto->RequiredReputationFaction)) < pProto->RequiredReputationRank))
    {
        SendBuyError(BUY_ERR_REPUTATION_REQUIRE, creature, item, 0);
        return false;
    }

    if (crItem->ExtendedCost)
    {
        ItemExtendedCostEntry const* iece = sItemExtendedCostStore.LookupEntry(crItem->ExtendedCost);
        if (!iece)
        {
            LOG_ERROR("entities.player", "Item {} have wrong ExtendedCost field value {}", pProto->ItemId, crItem->ExtendedCost);
            return false;
        }

        // honor points price
        if (GetHonorPoints() < (iece->reqhonorpoints * count))
        {
            SendEquipError(EQUIP_ERR_NOT_ENOUGH_HONOR_POINTS, nullptr, nullptr);
            return false;
        }

        // arena points price
        if (GetArenaPoints() < (iece->reqarenapoints * count))
        {
            SendEquipError(EQUIP_ERR_NOT_ENOUGH_ARENA_POINTS, nullptr, nullptr);
            return false;
        }

        // item base price
        for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
        {
            if (iece->reqitem[i] && !HasItemCount(iece->reqitem[i], (iece->reqitemcount[i] * count)))
            {
                SendEquipError(EQUIP_ERR_VENDOR_MISSING_TURNINS, nullptr, nullptr);
                return false;
            }
        }

        // check for personal arena rating requirement
        if (GetMaxPersonalArenaRatingRequirement(iece->reqarenaslot) < iece->reqpersonalarenarating)
        {
            // probably not the proper equip err
            SendEquipError(EQUIP_ERR_CANT_EQUIP_RANK, nullptr, nullptr);
            return false;
        }
    }

    uint32 price = 0;
    if (crItem->IsGoldRequired(pProto) && pProto->BuyPrice > 0) //Assume price cannot be negative (do not know why it is int32)
    {
        uint32 maxCount = MAX_MONEY_AMOUNT / pProto->BuyPrice;
        if ((uint32)count > maxCount)
        {
            LOG_ERROR("entities.player", "Player {} tried to buy {} item id {}, causing overflow", GetName(), (uint32)count, pProto->ItemId);
            count = (uint8)maxCount;
        }
        price = pProto->BuyPrice * count; //it should not exceed MAX_MONEY_AMOUNT

        // reputation discount
        price = uint32(std::floor(price * GetReputationPriceDiscount(creature)));

        if (!HasEnoughMoney(price))
        {
            SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, item, 0);
            return false;
        }
    }

    if ((bag == NULL_BAG && slot == NULL_SLOT) || IsInventoryPos(bag, slot))
    {
        if (!_StoreOrEquipNewItem(vendorslot, item, count, bag, slot, price, pProto, creature, crItem, true))
            return false;
    }
    else if (IsEquipmentPos(bag, slot))
    {
        if (pProto->BuyCount * count != 1)
        {
            SendEquipError(EQUIP_ERR_ITEM_CANT_BE_EQUIPPED, nullptr, nullptr);
            return false;
        }
        if (!_StoreOrEquipNewItem(vendorslot, item, count, bag, slot, price, pProto, creature, crItem, false))
            return false;
    }
    else
    {
        SendEquipError(EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT, nullptr, nullptr);
        return false;
    }

    return crItem->maxcount != 0;
}

bool Player::EnchantmentFitsRequirements(uint32 enchantmentcondition, int8 slot)
{
    if (!enchantmentcondition)
        return true;

    SpellItemEnchantmentConditionEntry const* Condition = sSpellItemEnchantmentConditionStore.LookupEntry(enchantmentcondition);

    if (!Condition)
        return true;

    uint8 curcount[4] = {0, 0, 0, 0};

    //counting current equipped gem colors
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (i == slot)
            continue;
        Item* pItem2 = GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (pItem2 && !pItem2->IsBroken() && pItem2->HasSocket())
        {
            for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot <= PRISMATIC_ENCHANTMENT_SLOT; ++enchant_slot)
            {
                if (enchant_slot == BONUS_ENCHANTMENT_SLOT)
                    continue;

                uint32 enchant_id = pItem2->GetEnchantmentId(EnchantmentSlot(enchant_slot));
                if (!enchant_id)
                    continue;

                SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                if (!enchantEntry)
                    continue;

                uint32 gemid = enchantEntry->GemID;
                if (!gemid)
                    continue;

                ItemTemplate const* gemProto = sObjectMgr->GetItemTemplate(gemid);
                if (!gemProto)
                    continue;

                GemPropertiesEntry const* gemProperty = sGemPropertiesStore.LookupEntry(gemProto->GemProperties);
                if (!gemProperty)
                    continue;

                uint8 GemColor = gemProperty->color;

                for (uint8 b = 0, tmpcolormask = 1; b < 4; b++, tmpcolormask <<= 1)
                {
                    if (tmpcolormask & GemColor)
                        ++curcount[b];
                }
            }
        }
    }

    bool activate = true;

    for (uint8 i = 0; i < 5; i++)
    {
        if (!Condition->Color[i])
            continue;

        uint32 _cur_gem = curcount[Condition->Color[i] - 1];

        // if have <CompareColor> use them as count, else use <value> from Condition
        uint32 _cmp_gem = Condition->CompareColor[i] ? curcount[Condition->CompareColor[i] - 1] : Condition->Value[i];

        switch (Condition->Comparator[i])
        {
            case 2:                                         // requires less <color> than (<value> || <comparecolor>) gems
                activate &= (_cur_gem < _cmp_gem);
                break;
            case 3:                                         // requires more <color> than (<value> || <comparecolor>) gems
                activate &= (_cur_gem > _cmp_gem);
                break;
            case 5:                                         // requires at least <color> than (<value> || <comparecolor>) gems
                activate &= (_cur_gem >= _cmp_gem);
                break;
        }
    }

    LOG_DEBUG("entities.player.items", "Checking Condition {}, there are {} Meta Gems, {} Red Gems, {} Yellow Gems and {} Blue Gems, Activate:{}", enchantmentcondition, curcount[0], curcount[1], curcount[2], curcount[3], activate ? "yes" : "no");

    return activate;
}

void Player::CorrectMetaGemEnchants(uint8 exceptslot, bool apply)
{
    //cycle all equipped items
    for (uint32 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        //enchants for the slot being socketed are handled by Player::ApplyItemMods
        if (slot == exceptslot)
            continue;

        Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

        if (!pItem || !pItem->HasSocket())
            continue;

        for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot < SOCK_ENCHANTMENT_SLOT + 3; ++enchant_slot)
        {
            uint32 enchant_id = pItem->GetEnchantmentId(EnchantmentSlot(enchant_slot));
            if (!enchant_id)
                continue;

            SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
            if (!enchantEntry)
                continue;

            uint32 condition = enchantEntry->EnchantmentCondition;
            if (condition)
            {
                //was enchant active with/without item?
                bool wasactive = EnchantmentFitsRequirements(condition, apply ? exceptslot : -1);
                //should it now be?
                if (wasactive ^ EnchantmentFitsRequirements(condition, apply ? -1 : exceptslot))
                {
                    // ignore item gem conditions
                    //if state changed, (dis)apply enchant
                    ApplyEnchantment(pItem, EnchantmentSlot(enchant_slot), !wasactive, true, true);
                }
            }
        }
    }
}

void Player::ToggleMetaGemsActive(uint8 exceptslot, bool apply)
{
    //cycle all equipped items
    for (int slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        //enchants for the slot being socketed are handled by WorldSession::HandleSocketOpcode(WorldPacket& recvData)
        if (slot == exceptslot)
            continue;

        Item* pItem = GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

        if (!pItem || !pItem->GetTemplate()->Socket[0].Color)   //if item has no sockets or no item is equipped go to next item
            continue;

        //cycle all (gem)enchants
        for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot < SOCK_ENCHANTMENT_SLOT + 3; ++enchant_slot)
        {
            uint32 enchant_id = pItem->GetEnchantmentId(EnchantmentSlot(enchant_slot));
            if (!enchant_id)                                 //if no enchant go to next enchant(slot)
                continue;

            SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
            if (!enchantEntry)
                continue;

            //only metagems to be (de)activated, so only enchants with condition
            uint32 condition = enchantEntry->EnchantmentCondition;
            if (condition)
                ApplyEnchantment(pItem, EnchantmentSlot(enchant_slot), apply);
        }
    }
}

void Player::RemoveItemDurations(Item* item)
{
    for (ItemDurationList::iterator itr = m_itemDuration.begin(); itr != m_itemDuration.end(); ++itr)
    {
        if (*itr == item)
        {
            m_itemDuration.erase(itr);
            break;
        }
    }
}

void Player::AddItemDurations(Item* item)
{
    if (item->GetUInt32Value(ITEM_FIELD_DURATION))
    {
        m_itemDuration.push_back(item);
        item->SendTimeUpdate(this);
    }
}

void Player::AutoUnequipOffhandIfNeed(bool force /*= false*/)
{
    Item* offItem = GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!offItem)
    {
        UpdateTitansGrip();
        return;
    }

    // unequip offhand weapon if player doesn't have dual wield anymore
    if (!CanDualWield() && (offItem->GetTemplate()->InventoryType == INVTYPE_WEAPONOFFHAND || offItem->GetTemplate()->InventoryType == INVTYPE_WEAPON))
        force = true;

    // unequip offhand weapon if player main hand weapon is a polearm or staff or fishing pole
    if (Item* mhWeapon = GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        if (ItemTemplate const* mhWeaponProto = mhWeapon->GetTemplate())
            if (mhWeaponProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
                mhWeaponProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF ||
                mhWeaponProto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                force = true;

    // need unequip offhand for 2h-weapon without TitanGrip (in any from hands)
    if (!force && (CanTitanGrip() || (offItem->GetTemplate()->InventoryType != INVTYPE_2HWEAPON && !IsTwoHandUsed())))
    {
        UpdateTitansGrip();
        return;
    }

    ItemPosCountVec off_dest;
    uint8 off_msg = CanStoreItem(NULL_BAG, NULL_SLOT, off_dest, offItem, false);
    if (off_msg == EQUIP_ERR_OK)
    {
        RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
        StoreItem(off_dest, offItem, true);
    }
    else
    {
        MoveItemFromInventory(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        offItem->DeleteFromInventoryDB(trans);                   // deletes item from character's inventory
        offItem->SaveToDB(trans);                                // recursive and not have transaction guard into self, item not in inventory and can be save standalone

        std::string subject = GetSession()->GetAcoreString(LANG_NOT_EQUIPPED_ITEM);
        MailDraft(subject, "There were problems with equipping one or several items").AddItem(offItem).SendMailTo(trans, this, MailSender(this, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);

        CharacterDatabase.CommitTransaction(trans);
    }
    UpdateTitansGrip();
}

bool Player::IsAtLootRewardDistance(WorldObject const* pRewardSource) const
{
    if (!IsAtGroupRewardDistance(pRewardSource))
    {
        return false;
    }

    if (HasPendingBind())
    {
        return false;
    }

    return pRewardSource->HasAllowedLooter(GetGUID());
}

void Player::AutoStoreLoot(uint8 bag, uint8 slot, uint32 loot_id, LootStore const& store, bool broadcast)
{
    Loot loot;
    loot.FillLoot (loot_id, store, this, true);

    uint32 max_slot = loot.GetMaxSlotInLootFor(this);
    for (uint32 i = 0; i < max_slot; ++i)
    {
        LootItem* lootItem = loot.LootItemInSlot(i, this);

        ItemPosCountVec dest;
        InventoryResult msg = CanStoreNewItem(bag, slot, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK && slot != NULL_SLOT)
            msg = CanStoreNewItem(bag, NULL_SLOT, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK && bag != NULL_BAG)
            msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem->itemid, lootItem->count);
        if (msg != EQUIP_ERR_OK)
        {
            SendEquipError(msg, nullptr, nullptr, lootItem->itemid);
            continue;
        }

        Item* pItem = StoreNewItem(dest, lootItem->itemid, true, lootItem->randomPropertyId);
        SendNewItem(pItem, lootItem->count, false, false, broadcast);
    }
}

LootItem* Player::StoreLootItem(uint8 lootSlot, Loot* loot, InventoryResult& msg)
{
    QuestItem* qitem = nullptr;
    QuestItem* ffaitem = nullptr;
    QuestItem* conditem = nullptr;

    msg = EQUIP_ERR_OK;

    LootItem* item = loot->LootItemInSlot(lootSlot, this, &qitem, &ffaitem, &conditem);
    if (!item || item->is_looted)
    {
        if (!sScriptMgr->OnPlayerCanSendErrorAlreadyLooted(this))
        {
            SendEquipError(EQUIP_ERR_ALREADY_LOOTED, nullptr, nullptr);
        }
        return nullptr;
    }

    // Xinef: exploit protection, dont allow to loot normal items if player is not master loot and not below loot threshold
    // Xinef: only quest, ffa and conditioned items
    if (!item->is_underthreshold && loot->roundRobinPlayer && !GetLootGUID().IsItem() && GetGroup() && GetGroup()->GetLootMethod() == MASTER_LOOT && GetGUID() != GetGroup()->GetMasterLooterGuid())
        if (!qitem && !ffaitem && !conditem)
        {
            SendLootRelease(GetLootGUID());
            return nullptr;
        }

    if (!item->AllowedForPlayer(this, loot->sourceWorldObjectGUID))
    {
        SendLootRelease(GetLootGUID());
        return nullptr;
    }

    // questitems use the blocked field for other purposes
    if (!qitem && item->is_blocked)
    {
        SendLootRelease(GetLootGUID());
        return nullptr;
    }

    // xinef: dont allow protected item to be looted by someone else
    if (item->rollWinnerGUID && item->rollWinnerGUID != GetGUID())
    {
        SendLootRelease(GetLootGUID());
        return nullptr;
    }

    ItemPosCountVec dest;
    msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid, item->count);
    if (msg == EQUIP_ERR_OK)
    {
        AllowedLooterSet looters = item->GetAllowedLooters();
        Item* newitem = StoreNewItem(dest, item->itemid, true, item->randomPropertyId, looters);

        if (qitem)
        {
            qitem->is_looted = true;
            //freeforall is 1 if everyone's supposed to get the quest item.
            if (item->freeforall || loot->GetPlayerQuestItems().size() == 1)
                SendNotifyLootItemRemoved(lootSlot);
            else
                loot->NotifyQuestItemRemoved(qitem->index);
        }
        else
        {
            if (ffaitem)
            {
                //freeforall case, notify only one player of the removal
                ffaitem->is_looted = true;
                SendNotifyLootItemRemoved(lootSlot);
            }
            else
            {
                //not freeforall, notify everyone
                if (conditem)
                    conditem->is_looted = true;
                loot->NotifyItemRemoved(lootSlot);
            }
        }

        //if only one person is supposed to loot the item, then set it to looted
        if (!item->freeforall)
            item->is_looted = true;

        --loot->unlootedCount;

        SendNewItem(newitem, uint32(item->count), false, false, true);
        UpdateLootAchievements(item, loot);

        // LootItem is being removed (looted) from the container, delete it from the DB.
        if (loot->containerGUID)
            sLootItemStorage->RemoveStoredLootItem(loot->containerGUID, item->itemid, item->count, loot, item->itemIndex);

        sScriptMgr->OnPlayerLootItem(this, newitem, item->count, this->GetLootGUID());
    }
    else
    {
        SendEquipError(msg, nullptr, nullptr, item->itemid);
    }

    return item;
}

InventoryResult Player::CanEquipUniqueItem(Item* pItem, uint8 eslot, uint32 limit_count) const
{
    ItemTemplate const* pProto = pItem->GetTemplate();

    // proto based limitations
    if (InventoryResult res = CanEquipUniqueItem(pProto, eslot, limit_count))
        return res;

    // check unique-equipped on gems
    for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot <= PRISMATIC_ENCHANTMENT_SLOT; ++enchant_slot)
    {
        if (enchant_slot == BONUS_ENCHANTMENT_SLOT)
            continue;

        uint32 enchant_id = pItem->GetEnchantmentId(EnchantmentSlot(enchant_slot));
        if (!enchant_id)
            continue;
        SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
        if (!enchantEntry)
            continue;

        ItemTemplate const* pGem = sObjectMgr->GetItemTemplate(enchantEntry->GemID);
        if (!pGem)
            continue;

        // include for check equip another gems with same limit category for not equipped item (and then not counted)
        uint32 gem_limit_count = !pItem->IsEquipped() && pGem->ItemLimitCategory
                                 ? pItem->GetGemCountWithLimitCategory(pGem->ItemLimitCategory) : 1;

        if (InventoryResult res = CanEquipUniqueItem(pGem, eslot, gem_limit_count))
            return res;
    }

    return EQUIP_ERR_OK;
}

InventoryResult Player::CanEquipUniqueItem(ItemTemplate const* itemProto, uint8 except_slot, uint32 limit_count) const
{
    // check unique-equipped on item
    if (itemProto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
    {
        // there is an equip limit on this item
        if (HasItemOrGemWithIdEquipped(itemProto->ItemId, 1, except_slot))
            return EQUIP_ERR_ITEM_UNIQUE_EQUIPABLE;
    }

    // check unique-equipped limit
    if (itemProto->ItemLimitCategory)
    {
        ItemLimitCategoryEntry const* limitEntry = sItemLimitCategoryStore.LookupEntry(itemProto->ItemLimitCategory);
        if (!limitEntry)
            return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED;

        // NOTE: limitEntry->mode not checked because if item have have-limit then it applied and to equip case

        if (limit_count > limitEntry->maxCount)
            return EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_EQUIPPED_EXCEEDED;

        // there is an equip limit on this item
        if (HasItemOrGemWithLimitCategoryEquipped(itemProto->ItemLimitCategory, limitEntry->maxCount - limit_count + 1, except_slot))
            return EQUIP_ERR_ITEM_MAX_COUNT_EQUIPPED_SOCKETED;
    }

    return EQUIP_ERR_OK;
}

void Player::BuildEnchantmentsInfoData(WorldPacket* data)
{
    uint32 slotUsedMask = 0;
    std::size_t slotUsedMaskPos = data->wpos();
    *data << uint32(slotUsedMask);                          // slotUsedMask < 0x80000

    for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = GetItemByPos(INVENTORY_SLOT_BAG_0, i);

        if (!item)
            continue;

        slotUsedMask |= (1 << i);

        *data << uint32(item->GetEntry());                  // item entry

        uint16 enchantmentMask = 0;
        std::size_t enchantmentMaskPos = data->wpos();
        *data << uint16(enchantmentMask);                   // enchantmentMask < 0x1000

        for (uint32 j = 0; j < MAX_ENCHANTMENT_SLOT; ++j)
        {
            uint32 enchId = item->GetEnchantmentId(EnchantmentSlot(j));

            if (!enchId)
                continue;

            enchantmentMask |= (1 << j);

            *data << uint16(enchId);                        // enchantmentId?
        }

        data->put<uint16>(enchantmentMaskPos, enchantmentMask);

        *data << int16(item->GetItemRandomPropertyId());                    // item random property id
        *data << item->GetGuidValue(ITEM_FIELD_CREATOR).WriteAsPacked();    // item creator
        *data << uint32(item->GetItemSuffixFactor());                       // item suffix factor
    }

    data->put<uint32>(slotUsedMaskPos, slotUsedMask);
}

void Player::SendEquipmentSetList()
{
    uint32 count = 0;
    WorldPacket data(SMSG_EQUIPMENT_SET_LIST, 4);
    std::size_t count_pos = data.wpos();
    data << uint32(count);                                  // count placeholder
    for (EquipmentSets::iterator itr = m_EquipmentSets.begin(); itr != m_EquipmentSets.end(); ++itr)
    {
        if (itr->second.state == EQUIPMENT_SET_DELETED)
            continue;

        data.appendPackGUID(itr->second.Guid);
        data << uint32(itr->first);
        data << itr->second.Name;
        data << itr->second.IconName;
        for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
        {
            // ignored slots stored in IgnoreMask, client wants "1" as raw GUID, so no HighGuid::Item
            if (itr->second.IgnoreMask & (1 << i))
                data.appendPackGUID(uint64(1));
            else // xinef: send proper data (do not append 0 with high guid)
                data.appendPackGUID(itr->second.Items[i] ? itr->second.Items[i].GetRawValue() : uint64(0));
        }

        ++count;                                            // client have limit but it checked at loading and set
    }
    data.put<uint32>(count_pos, count);
    SendDirectMessage(&data);
}

void Player::SetEquipmentSet(uint32 index, EquipmentSet eqset)
{
    if (eqset.Guid != 0)
    {
        bool found = false;

        for (EquipmentSets::iterator itr = m_EquipmentSets.begin(); itr != m_EquipmentSets.end(); ++itr)
        {
            if ((itr->second.Guid == eqset.Guid) && (itr->first == index))
            {
                found = true;
                break;
            }
        }

        if (!found)                                          // something wrong...
        {
            LOG_ERROR("entities.player", "Player {} tried to save equipment set {} (index {}), but that equipment set not found!", GetName(), eqset.Guid, index);
            return;
        }
    }

    EquipmentSet& eqslot = m_EquipmentSets[index];

    EquipmentSetUpdateState old_state = eqslot.state;

    eqslot = eqset;

    if (eqset.Guid == 0)
    {
        eqslot.Guid = sObjectMgr->GenerateEquipmentSetGuid();

        WorldPacket data(SMSG_EQUIPMENT_SET_SAVED, 4 + 1);
        data << uint32(index);
        data.appendPackGUID(eqslot.Guid);
        SendDirectMessage(&data);
    }

    eqslot.state = old_state == EQUIPMENT_SET_NEW ? EQUIPMENT_SET_NEW : EQUIPMENT_SET_CHANGED;
}

void Player::_SaveEquipmentSets(CharacterDatabaseTransaction trans)
{
    for (EquipmentSets::iterator itr = m_EquipmentSets.begin(); itr != m_EquipmentSets.end();)
    {
        uint32 index = itr->first;
        EquipmentSet& eqset = itr->second;
        CharacterDatabasePreparedStatement* stmt = nullptr;
        uint8 j = 0;
        switch (eqset.state)
        {
            case EQUIPMENT_SET_UNCHANGED:
                ++itr;
                break;                                      // nothing do
            case EQUIPMENT_SET_CHANGED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_EQUIP_SET);
                stmt->SetData(j++, eqset.Name.c_str());
                stmt->SetData(j++, eqset.IconName.c_str());
                stmt->SetData(j++, eqset.IgnoreMask);
                for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i)
                    stmt->SetData(j++, eqset.Items[i].GetCounter());
                stmt->SetData(j++, GetGUID().GetRawValue());
                stmt->SetData(j++, eqset.Guid);
                stmt->SetData(j, index);
                trans->Append(stmt);
                eqset.state = EQUIPMENT_SET_UNCHANGED;
                ++itr;
                break;
            case EQUIPMENT_SET_NEW:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_EQUIP_SET);
                stmt->SetData(j++, GetGUID().GetRawValue());
                stmt->SetData(j++, eqset.Guid);
                stmt->SetData(j++, index);
                stmt->SetData(j++, eqset.Name.c_str());
                stmt->SetData(j++, eqset.IconName.c_str());
                stmt->SetData(j++, eqset.IgnoreMask);
                for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i)
                    stmt->SetData(j++, eqset.Items[i].GetCounter());
                trans->Append(stmt);
                eqset.state = EQUIPMENT_SET_UNCHANGED;
                ++itr;
                break;
            case EQUIPMENT_SET_DELETED:
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_EQUIP_SET);
                stmt->SetData(0, eqset.Guid);
                trans->Append(stmt);
                m_EquipmentSets.erase(itr++);
                break;
        }
    }
}

void Player::DeleteEquipmentSet(uint64 setGuid)
{
    for (EquipmentSets::iterator itr = m_EquipmentSets.begin(); itr != m_EquipmentSets.end(); ++itr)
    {
        if (itr->second.Guid == setGuid)
        {
            if (itr->second.state == EQUIPMENT_SET_NEW)
                m_EquipmentSets.erase(itr);
            else
                itr->second.state = EQUIPMENT_SET_DELETED;
            break;
        }
    }
}

bool Player::AddItem(uint32 itemId, uint32 count)
{
    uint32 noSpaceForCount = 0;
    ItemPosCountVec dest;
    InventoryResult msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count, &noSpaceForCount);
    if (msg != EQUIP_ERR_OK)
        count -= noSpaceForCount;

    if (count == 0 || dest.empty())
    {
        // -- TODO: Send to mailbox if no space
        ChatHandler(GetSession()).PSendSysMessage("You don't have any space in your bags.");
        return false;
    }

    Item* item = StoreNewItem(dest, itemId, true);
    if (item)
        SendNewItem(item, count, true, false);
    else
        return false;
    return true;
}

void Player::RefundItem(Item* item)
{
    if (!item->IsRefundable())
    {
        LOG_DEBUG("entities.player.items", "Item refund: item not refundable!");
        return;
    }

    if (item->IsRefundExpired())    // item refund has expired
    {
        item->SetNotRefundable(this);
        WorldPacket data(SMSG_ITEM_REFUND_RESULT, 8 + 4);
        data << item->GetGUID();                     // Guid
        data << uint32(10);                          // Error!
        SendDirectMessage(&data);
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

    bool store_error = false;
    for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
    {
        uint32 count = iece->reqitemcount[i];
        uint32 itemid = iece->reqitem[i];

        if (count && itemid)
        {
            ItemPosCountVec dest;
            InventoryResult msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, count);
            if (msg != EQUIP_ERR_OK)
            {
                store_error = true;
                break;
            }
        }
    }

    if (store_error)
    {
        WorldPacket data(SMSG_ITEM_REFUND_RESULT, 8 + 4);
        data << item->GetGUID();                         // Guid
        data << uint32(10);                              // Error!
        SendDirectMessage(&data);
        return;
    }

    WorldPacket data(SMSG_ITEM_REFUND_RESULT, 8 + 4 + 4 + 4 + 4 + 4 * 4 + 4 * 4);
    data << item->GetGUID();                            // item guid
    data << uint32(0);                                  // 0, or error code
    data << uint32(item->GetPaidMoney());               // money cost
    data << uint32(iece->reqhonorpoints);               // honor point cost
    data << uint32(iece->reqarenapoints);               // arena point cost
    for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i) // item cost data
    {
        data << uint32(iece->reqitem[i]);
        data << uint32(iece->reqitemcount[i]);
    }
    SendDirectMessage(&data);

    uint32 moneyRefund = item->GetPaidMoney();  // item-> will be invalidated in DestroyItem

    // Save all relevant data to DB to prevent desynchronisation exploits
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Delete any references to the refund data
    item->SetNotRefundable(this, true, &trans);

    // Destroy item
    DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    // Grant back extendedcost items
    for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
    {
        uint32 count = iece->reqitemcount[i];
        uint32 itemid = iece->reqitem[i];
        if (count && itemid)
        {
            ItemPosCountVec dest;
            InventoryResult msg = CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, count);
            ASSERT(msg == EQUIP_ERR_OK); /// Already checked before
            Item* it = StoreNewItem(dest, itemid, true, true);
            SendNewItem(it, count, true, false, true);
        }
    }

    // Grant back money
    if (moneyRefund)
        ModifyMoney(moneyRefund); // Saved in SaveInventoryAndGoldToDB

    // Grant back Honor points
    if (uint32 honorRefund = iece->reqhonorpoints)
        ModifyHonorPoints(honorRefund, trans);

    // Grant back Arena points
    if (uint32 arenaRefund = iece->reqarenapoints)
        ModifyArenaPoints(arenaRefund, trans);

    SaveInventoryAndGoldToDB(trans);

    CharacterDatabase.CommitTransaction(trans);
}

float Player::GetTotalItemLevel() const
{
    float sum = 0;
    uint8 level = GetLevel();

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        // don't check tabard, ranged, offhand or shirt
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_RANGED || i == EQUIPMENT_SLOT_OFFHAND || i == EQUIPMENT_SLOT_BODY)
            continue;

        if (m_items[i] && m_items[i]->GetTemplate())
            sum += m_items[i]->GetTemplate()->GetItemLevelIncludingQuality(level);
    }

    return sum;
}

float Player::GetAverageItemLevel()
{
    float sum = 0;
    uint32 count = 0;
    uint8 level = GetLevel();

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        // don't check tabard, ranged, offhand or shirt
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_RANGED || i == EQUIPMENT_SLOT_OFFHAND || i == EQUIPMENT_SLOT_BODY)
            continue;

        if (m_items[i] && m_items[i]->GetTemplate())
            sum += m_items[i]->GetTemplate()->GetItemLevelIncludingQuality(level);

        ++count;
    }

    return std::max<float>(0.0f, sum / (float)count);
}

float Player::GetAverageItemLevelForDF()
{
    float sum = 0;
    uint32 count = 0;
    uint8 level = GetLevel();

    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        // don't check tabard, ranged, offhand or shirt
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_RANGED || i == EQUIPMENT_SLOT_OFFHAND || i == EQUIPMENT_SLOT_BODY)
            continue;

        if (m_items[i] && m_items[i]->GetTemplate())
        {
            if (m_items[i]->GetTemplate()->Quality == ITEM_QUALITY_HEIRLOOM)
                sum += level * 2.33f;
            else
                sum += m_items[i]->GetTemplate()->ItemLevel;
        }

        ++count;
    }

    return std::max(0.0f, sum / (float)count);
}

uint32 Player::DoRandomRoll(uint32 minimum, uint32 maximum)
{
    ASSERT(minimum <= maximum);

    uint32 roll = urand(minimum, maximum);

    WorldPackets::Misc::RandomRoll randomRoll;
    randomRoll.Min = minimum;
    randomRoll.Max = maximum;
    randomRoll.Result = roll;
    randomRoll.Roller = GetGUID();
    if (Group* group = GetGroup())
        group->BroadcastPacket(randomRoll.Write(), false);
    else
        SendDirectMessage(randomRoll.Write());

    return roll;
}
