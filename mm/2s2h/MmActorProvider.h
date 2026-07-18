#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <shiplua/actor/ActorProvider.h>
#include <shiplua/capability/CapabilityRegistry.h>
#include <shiplua/handles/HandleRegistry.h>
#include <shiplua/runtime/Logger.h>
#include <shiplua/runtime/Result.h>

namespace ShipLuaHost {

// Stable logical actor id translated by the MM host. Raw engine ids and
// native pointers never cross this provider boundary.
struct MmActorDefinition {
    std::string key;
    std::int16_t actorId = -1;
    std::int16_t objectId = -1;
    std::int16_t params = 0;
};

struct MmActorProviderHooks {
    std::function<bool(std::int16_t objectId)> objectReady;
    std::function<void*(const MmActorDefinition& definition, const ShipLua::ActorSpawnRequest& request)> spawn;
    std::function<void(void* actor)> kill;
};

// Native MM provider for the common actor API. Public operations are
// main-thread-only. Handles carry no Actor* and are scoped to owner + scene.
class MmActorProvider final : public ShipLua::ActorProvider {
  public:
    MmActorProvider(std::vector<MmActorDefinition> allowlist, MmActorProviderHooks hooks, ShipLua::Logger logger = {},
                    std::int16_t forbiddenActorId = 0, ShipLua::HandleLimits limits = { 16, 256 });

    ShipLua::Result<void> RegisterCapabilities(ShipLua::CapabilityRegistry& registry) const;

    ShipLua::Result<ShipLua::Handle> Spawn(const std::string& ownerModId,
                                           const ShipLua::ActorSpawnRequest& request) override;
    ShipLua::Result<void> Destroy(const std::string& ownerModId, const ShipLua::Handle& handle) override;
    ShipLua::Result<bool> Exists(const std::string& ownerModId, const ShipLua::Handle& handle) const override;

    ShipLua::Result<bool> OnNativeActorDestroyed(void* actor);
    ShipLua::Result<std::size_t> ReleaseMod(const std::string& modId) override;
    ShipLua::Result<std::size_t> OnSceneChange();
    ShipLua::Result<std::size_t> Shutdown();

    std::size_t Count() const noexcept {
        return mRecords.size();
    }
    std::size_t CountForMod(const std::string& modId) const;
    std::size_t AllowedActorCount() const noexcept {
        return mAllowlist.size();
    }
    const ShipLua::HandleRegistry& Handles() const noexcept {
        return mHandles;
    }

  private:
    struct ActorRecord {
        ShipLua::Handle handle;
        std::string ownerModId;
        std::string actorKey;
        void* nativeActor = nullptr;
    };

    ShipLua::Result<void> ValidateThread(const char* operation) const;
    static bool IsSafeKey(const std::string& key);
    bool SameHandle(const ActorRecord& record, const ShipLua::Handle& handle) const;

    std::map<std::string, MmActorDefinition> mAllowlist;
    MmActorProviderHooks mHooks;
    ShipLua::Logger mLogger;
    ShipLua::HandleRegistry mHandles;
    std::map<std::uint32_t, ActorRecord> mRecords;
    std::thread::id mGameThread;
};

} // namespace ShipLuaHost
