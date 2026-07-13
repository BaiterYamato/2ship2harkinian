#pragma once

#include <optional>
#include <string>
#include <vector>

#include <shiplua/world/PortableItemCatalog.h>
#include <shiplua/world/WorldSession.h>

namespace ShipLuaHost {

class MmWorldAdapter final : public ShipLua::IWorldAdapter {
  public:
    explicit MmWorldAdapter(ShipLua::PortableItemCatalog catalog);

    ShipLua::WorldId Id() const noexcept override;
    ShipLua::Result<ShipLua::PortablePlayerState> CapturePlayerState() override;
    bool CanResolveAsset(const ShipLua::AssetReference& asset) const noexcept override;
    ShipLua::Result<ShipLua::WorldImportPreview> PrepareImport(
        const ShipLua::PortablePlayerState& state,
        const ShipLua::WorldDestination& destination) override;
    ShipLua::Result<void> CommitImport() override;
    void AbortImport() noexcept override;

    struct PendingItem {
        ShipLua::PortableItem source;
        std::string targetId;
        bool translated = false;
    };

    struct PendingImport {
        ShipLua::PortablePlayerState state;
        ShipLua::WorldDestination destination;
        std::vector<PendingItem> items;
    };

  private:
    ShipLua::PortableItemCatalog mCatalog;
    std::optional<PendingImport> mPending;
};

} // namespace ShipLuaHost
