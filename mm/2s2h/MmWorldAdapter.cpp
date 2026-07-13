#include "MmWorldAdapter.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "functions.h"
#include "variables.h"

namespace ShipLuaHost {
namespace {

using ShipLua::AssetReference;
using ShipLua::ErrorCode;
using ShipLua::PortableItem;
using ShipLua::PortablePlayerState;
using ShipLua::Result;
using ShipLua::WorldId;

PortableItem MakeItem(std::string id, std::uint32_t quantity = 1) {
    PortableItem item;
    item.id = std::move(id);
    item.origin = WorldId::Mm;
    item.quantity = quantity;
    return item;
}

void AddSlotItem(PortablePlayerState& state, std::uint8_t slot, std::uint8_t expected,
                 const char* canonicalId, bool hasAmmo) {
    const Inventory& inventory = gSaveContext.save.saveInfo.inventory;
    if (inventory.items[slot] != expected) {
        return;
    }
    PortableItem item = MakeItem(canonicalId);
    if (hasAmmo) {
        item.quantity = static_cast<std::uint32_t>(std::max<std::int32_t>(0, inventory.ammo[slot]));
    }
    state.items.push_back(std::move(item));
}

std::uint8_t CurrentSword() {
    return static_cast<std::uint8_t>(
        (gSaveContext.save.saveInfo.equips.equipment & gEquipMasks[EQUIP_TYPE_SWORD]) >>
        gEquipShifts[EQUIP_TYPE_SWORD]);
}

void AddSword(PortablePlayerState& state, std::uint8_t sword) {
    const char* id = nullptr;
    const char* assetId = nullptr;
    if (sword == EQUIP_VALUE_SWORD_KOKIRI) {
        id = "mm.sword.kokiri";
        assetId = "mm.player.sword.kokiri";
    } else if (sword == EQUIP_VALUE_SWORD_RAZOR) {
        id = "mm.sword.razor";
        assetId = "mm.player.sword.razor";
    } else if (sword == EQUIP_VALUE_SWORD_GILDED) {
        id = "mm.sword.gilded";
        assetId = "mm.player.sword.gilded";
    }
    if (id == nullptr) {
        return;
    }
    PortableItem item = MakeItem(id);
    item.equipped = true;
    item.visualAsset = AssetReference{ WorldId::Mm, assetId };
    state.items.push_back(std::move(item));
}

std::optional<std::uint16_t> ResolveEntrance(const std::string& destinationId) {
    if (destinationId == "mm.clock_town") {
        return ENTRANCE(SOUTH_CLOCK_TOWN, 0);
    }
    if (destinationId == "mm.woodfall") {
        return ENTRANCE(WOODFALL, 0);
    }
    return std::nullopt;
}

bool IsSword(const std::string& id) {
    return id.rfind("mm.sword.", 0) == 0;
}

std::uint8_t SwordRank(const std::string& id) {
    if (id == "mm.sword.gilded") {
        return 3;
    }
    if (id == "mm.sword.razor") {
        return 2;
    }
    return id == "mm.sword.kokiri" ? 1 : 0;
}

void SetSword(std::uint8_t equipValue, std::uint8_t itemId) {
    gSaveContext.save.saveInfo.equips.equipment = static_cast<std::uint16_t>(
        (gSaveContext.save.saveInfo.equips.equipment & gEquipNegMasks[EQUIP_TYPE_SWORD]) |
        (equipValue << gEquipShifts[EQUIP_TYPE_SWORD]));
    gSaveContext.save.saveInfo.equips.buttonItems[PLAYER_FORM_HUMAN][EQUIP_SLOT_B] = itemId;
}

void ApplyItem(const MmWorldAdapter::PendingItem& item) {
    Inventory& inventory = gSaveContext.save.saveInfo.inventory;
    const std::int8_t quantity = static_cast<std::int8_t>(std::min<std::uint32_t>(item.source.quantity, 99));
    if (item.targetId == "shared.bow") {
        if (CUR_UPG_VALUE(UPG_QUIVER) == 0) {
            Inventory_ChangeUpgrade(UPG_QUIVER, 1);
        }
        inventory.items[SLOT_BOW] = ITEM_BOW;
        inventory.ammo[SLOT_BOW] = quantity;
    } else if (item.targetId == "shared.bombs") {
        if (CUR_UPG_VALUE(UPG_BOMB_BAG) == 0) {
            Inventory_ChangeUpgrade(UPG_BOMB_BAG, 1);
        }
        inventory.items[SLOT_BOMB] = ITEM_BOMB;
        inventory.ammo[SLOT_BOMB] = quantity;
    } else if (item.targetId == "shared.bombchu") {
        inventory.items[SLOT_BOMBCHU] = ITEM_BOMBCHU;
        inventory.ammo[SLOT_BOMBCHU] = quantity;
    } else if (item.targetId == "shared.hookshot") {
        inventory.items[SLOT_HOOKSHOT] = ITEM_HOOKSHOT;
    } else if (item.targetId == "shared.ocarina") {
        inventory.items[SLOT_OCARINA] = ITEM_OCARINA_OF_TIME;
    } else if (item.targetId == "mm.sword.kokiri") {
        SetSword(EQUIP_VALUE_SWORD_KOKIRI, ITEM_SWORD_KOKIRI);
    } else if (item.targetId == "mm.sword.razor") {
        SetSword(EQUIP_VALUE_SWORD_RAZOR, ITEM_SWORD_RAZOR);
    } else if (item.targetId == "mm.sword.gilded") {
        SetSword(EQUIP_VALUE_SWORD_GILDED, ITEM_SWORD_GILDED);
    } else if (item.targetId == "mm.mask.deku") {
        inventory.items[SLOT_MASK_DEKU] = ITEM_MASK_DEKU;
    }
}

} // namespace

MmWorldAdapter::MmWorldAdapter(ShipLua::PortableItemCatalog catalog) : mCatalog(std::move(catalog)) {
}

WorldId MmWorldAdapter::Id() const noexcept {
    return WorldId::Mm;
}

Result<PortablePlayerState> MmWorldAdapter::CapturePlayerState() {
    if (gSaveContext.fileNum == 0xFF) {
        return Result<PortablePlayerState>::err(ErrorCode::InvalidState, "nenhum save MM está carregado");
    }

    const SavePlayerData& player = gSaveContext.save.saveInfo.playerData;
    PortablePlayerState state;
    state.health = static_cast<std::uint16_t>(std::max<std::int32_t>(0, player.health));
    state.healthCapacity = static_cast<std::uint16_t>(std::max<std::int32_t>(0, player.healthCapacity));
    state.rupees = static_cast<std::uint32_t>(std::max<std::int32_t>(0, player.rupees));

    AddSlotItem(state, SLOT_BOW, ITEM_BOW, "shared.bow", true);
    AddSlotItem(state, SLOT_BOMB, ITEM_BOMB, "shared.bombs", true);
    AddSlotItem(state, SLOT_BOMBCHU, ITEM_BOMBCHU, "shared.bombchu", true);
    AddSlotItem(state, SLOT_HOOKSHOT, ITEM_HOOKSHOT, "shared.hookshot", false);
    if (gSaveContext.save.saveInfo.inventory.items[SLOT_OCARINA] == ITEM_OCARINA_OF_TIME ||
        gSaveContext.save.saveInfo.inventory.items[SLOT_OCARINA] == ITEM_OCARINA_FAIRY) {
        state.items.push_back(MakeItem("shared.ocarina"));
    }
    AddSword(state, CurrentSword());
    if (gSaveContext.save.saveInfo.inventory.items[SLOT_MASK_DEKU] == ITEM_MASK_DEKU) {
        state.items.push_back(MakeItem("mm.mask.deku"));
    }

    const Result<void> valid = ShipLua::ValidatePortablePlayerState(state);
    if (!valid.isOk()) {
        return Result<PortablePlayerState>::err(valid.code, valid.message);
    }
    return Result<PortablePlayerState>::ok(std::move(state));
}

bool MmWorldAdapter::CanResolveAsset(const AssetReference& asset) const noexcept {
    return asset.owner == WorldId::Mm && asset.id.rfind("mm.", 0) == 0;
}

Result<ShipLua::WorldImportPreview> MmWorldAdapter::PrepareImport(
    const PortablePlayerState& state, const ShipLua::WorldDestination& destination) {
    AbortImport();
    if (destination.world != WorldId::Mm || !ResolveEntrance(destination.id).has_value()) {
        return Result<ShipLua::WorldImportPreview>::err(ErrorCode::Unsupported, "destino MM desconhecido");
    }
    const Result<void> stateValid = ShipLua::ValidatePortablePlayerState(state);
    if (!stateValid.isOk()) {
        return Result<ShipLua::WorldImportPreview>::err(stateValid.code, stateValid.message);
    }

    std::vector<PendingItem> resolved;
    ShipLua::WorldImportPreview preview;
    std::optional<std::size_t> selectedSword;
    std::uint32_t equippedSwordCount = 0;
    for (const PortableItem& item : state.items) {
        const Result<void> itemValid = mCatalog.Validate(item);
        if (!itemValid.isOk()) {
            return Result<ShipLua::WorldImportPreview>::err(itemValid.code, itemValid.message);
        }
        const auto resolution = mCatalog.ResolveForWorld(item.id, WorldId::Mm);
        if (!resolution.isOk()) {
            if (resolution.code == ErrorCode::Unsupported) {
                preview.deferredItemIds.push_back(item.id);
                continue;
            }
            return Result<ShipLua::WorldImportPreview>::err(resolution.code, resolution.message);
        }

        const std::size_t index = resolved.size();
        resolved.push_back({ item, resolution.value->targetId, resolution.value->translated });
        if (IsSword(resolution.value->targetId)) {
            if (item.equipped && ++equippedSwordCount > 1) {
                return Result<ShipLua::WorldImportPreview>::err(
                    ErrorCode::InvalidArgument, "estado portátil possui mais de uma espada equipada");
            }
            if (item.equipped || !selectedSword.has_value() ||
                (!resolved[*selectedSword].source.equipped &&
                 SwordRank(resolution.value->targetId) > SwordRank(resolved[*selectedSword].targetId))) {
                selectedSword = index;
            }
        }
    }

    PendingImport pending{ state, destination, {} };
    for (std::size_t index = 0; index < resolved.size(); ++index) {
        PendingItem& item = resolved[index];
        if (IsSword(item.targetId) && selectedSword != index) {
            preview.deferredItemIds.push_back(item.source.id);
            continue;
        }
        preview.acceptedItemIds.push_back(item.source.id);
        if (item.translated) {
            preview.translatedItemIds.push_back(item.source.id);
        }
        pending.items.push_back(std::move(item));
    }
    mPending = std::move(pending);
    return Result<ShipLua::WorldImportPreview>::ok(std::move(preview));
}

Result<void> MmWorldAdapter::CommitImport() {
    if (!mPending.has_value()) {
        return Result<void>::err(ErrorCode::InvalidState, "importação MM não foi preparada");
    }
    const std::optional<std::uint16_t> entrance = ResolveEntrance(mPending->destination.id);
    if (!entrance.has_value()) {
        return Result<void>::err(ErrorCode::InvalidState, "destino MM pendente ficou inválido");
    }

    SavePlayerData& player = gSaveContext.save.saveInfo.playerData;
    player.healthCapacity = static_cast<std::int16_t>(mPending->state.healthCapacity);
    player.health = static_cast<std::int16_t>(
        std::min(mPending->state.health, mPending->state.healthCapacity));
    player.rupees = static_cast<std::int16_t>(std::min<std::uint32_t>(mPending->state.rupees, 9999));

    Inventory& inventory = gSaveContext.save.saveInfo.inventory;
    for (const std::uint8_t slot : { SLOT_BOW, SLOT_BOMB, SLOT_BOMBCHU, SLOT_HOOKSHOT, SLOT_OCARINA }) {
        inventory.items[slot] = ITEM_NONE;
        inventory.ammo[slot] = 0;
    }
    gSaveContext.save.saveInfo.equips.equipment &= gEquipNegMasks[EQUIP_TYPE_SWORD];
    gSaveContext.save.saveInfo.equips.buttonItems[PLAYER_FORM_HUMAN][EQUIP_SLOT_B] = ITEM_NONE;

    for (const PendingItem& item : mPending->items) {
        ApplyItem(item);
    }

    gSaveContext.save.entrance = *entrance;
    if (gPlayState != nullptr) {
        gPlayState->nextEntrance = *entrance;
        gPlayState->transitionTrigger = TRANS_TRIGGER_START;
        gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    }
    mPending.reset();
    return Result<void>::ok();
}

void MmWorldAdapter::AbortImport() noexcept {
    mPending.reset();
}

} // namespace ShipLuaHost
