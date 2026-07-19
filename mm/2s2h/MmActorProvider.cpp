#include "MmActorProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <utility>

#include <shiplua/manifest/SemVersion.h>

namespace ShipLuaHost {
namespace {

constexpr const char* kProviderName = "2ship-native";
constexpr const char* kProviderVersion = "0.1.0";
constexpr const char* kCapabilityVersion = "0.1.0";

ShipLua::CapabilityProvider MakeOffer(const ShipLua::SemVersion& providerVersion,
                                      const ShipLua::SemVersion& capabilityVersion, const char* permission,
                                      const char* description, std::size_t perModLimit) {
    ShipLua::CapabilityProvider offer;
    offer.name = kProviderName;
    offer.providerVersion = providerVersion;
    offer.capabilityVersion = capabilityVersion;
    offer.games = { "mm" };
    offer.stability = ShipLua::CapabilityStability::Experimental;
    offer.permissions = { permission };
    offer.limits.perMod = perModLimit;
    offer.description = description;
    return offer;
}

} // namespace

MmActorProvider::MmActorProvider(std::vector<MmActorDefinition> allowlist, MmActorProviderHooks hooks,
                                 ShipLua::Logger logger, std::int16_t forbiddenActorId, ShipLua::HandleLimits limits)
    : mHooks(std::move(hooks)), mLogger(std::move(logger)), mHandles(2, limits),
      mGameThread(std::this_thread::get_id()), mForbiddenActorId(forbiddenActorId) {
    for (MmActorDefinition& definition : allowlist) {
        if (!IsSafeKey(definition.key) || definition.actorId < 0 || definition.objectId < 0 ||
            definition.actorId == forbiddenActorId) {
            mLogger.warn("host", "ignored unsafe MM actor allowlist entry '" + definition.key + "'");
            continue;
        }
        auto inserted = mAllowlist.emplace(definition.key, std::move(definition));
        if (!inserted.second) {
            mLogger.warn("host", "ignored duplicate MM actor allowlist entry '" + inserted.first->first + "'");
        }
    }
}

ShipLua::Result<void> MmActorProvider::RegisterCapabilities(ShipLua::CapabilityRegistry& registry) const {
    const auto providerVersion = ShipLua::SemVersion::Parse(kProviderVersion);
    const auto capabilityVersion = ShipLua::SemVersion::Parse(kCapabilityVersion);
    if (!providerVersion.isOk() || !capabilityVersion.isOk()) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::HostFailure,
                                          "invalid built-in MM actor provider version");
    }

    struct OfferSpec {
        const char* id;
        const char* permission;
        const char* description;
    };
    constexpr OfferSpec offers[] = {
        { "actor.spawn", "world.entities.create", "Spawn an allowlisted existing MM actor." },
        { "actor.destroy", "world.entities.destroy", "Destroy an actor owned by the calling mod." },
        { "actor.exists", "world.entities.read", "Validate an owned MM actor handle." },
    };

    std::vector<std::string> registered;
    for (const OfferSpec& spec : offers) {
        auto result =
            registry.Register(spec.id, MakeOffer(*providerVersion.value, *capabilityVersion.value, spec.permission,
                                                 spec.description, mHandles.Limits().maxPerMod));
        if (!result.isOk()) {
            for (const std::string& id : registered) {
                (void)registry.Unregister(id, kProviderName);
            }
            return result;
        }
        registered.emplace_back(spec.id);
    }
    return ShipLua::Result<void>::ok();
}

ShipLua::Result<ShipLua::Handle> MmActorProvider::Spawn(const std::string& ownerModId,
                                                        const ShipLua::ActorSpawnRequest& request) {
    const auto thread = ValidateThread("spawn");
    if (!thread.isOk()) {
        return ShipLua::Result<ShipLua::Handle>::err(thread.code, thread.message);
    }
    if (ownerModId.empty()) {
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::InvalidArgument,
                                                     "actor owner mod id cannot be empty");
    }
    if (!std::isfinite(request.x) || !std::isfinite(request.y) || !std::isfinite(request.z) ||
        !std::isfinite(request.rotationX) || !std::isfinite(request.rotationY) || !std::isfinite(request.rotationZ)) {
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::InvalidArgument,
                                                     "actor transform must be finite");
    }
    // Nomes curados vêm da allowlist; qualquer outro ator é acessível pelo id
    // genérico "mm.id.<actorId>[.<params>]" (decimal ou 0x-hex). O engine já
    // recusa spawns sem o objeto carregado (Actor_Spawn retorna NULL), então o
    // gate de allowlist não é necessário para segurança — só o player fica
    // proibido (mForbiddenActorId), porque um segundo Player corrompe o jogo.
    MmActorDefinition resolved;
    const auto definition = mAllowlist.find(request.actor);
    if (definition != mAllowlist.end()) {
        resolved = definition->second;
    } else {
        auto parseNumber = [](const std::string& text, long& out) {
            if (text.empty()) {
                return false;
            }
            char* end = nullptr;
            out = std::strtol(text.c_str(), &end, 0);
            return end != nullptr && *end == '\0';
        };
        constexpr const char* kGenericPrefix = "mm.id.";
        if (request.actor.rfind(kGenericPrefix, 0) != 0) {
            return ShipLua::Result<ShipLua::Handle>::err(
                ShipLua::ErrorCode::Unsupported, "MM actor '" + request.actor +
                                                     "' desconhecido — use um nome do catálogo ou mm.id.<actorId>");
        }
        const std::string payload = request.actor.substr(std::char_traits<char>::length(kGenericPrefix));
        const std::size_t dot = payload.find('.');
        long actorId = 0;
        long params = 0;
        if (!parseNumber(payload.substr(0, dot), actorId) ||
            (dot != std::string::npos && !parseNumber(payload.substr(dot + 1), params))) {
            return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::InvalidArgument,
                                                         "formato inválido: use mm.id.<actorId>[.<params>]");
        }
        if (actorId <= 0 || actorId > 0x7FFF || actorId == mForbiddenActorId) {
            return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::InvalidArgument,
                                                         "actorId fora da faixa permitida");
        }
        resolved.key = request.actor;
        resolved.actorId = static_cast<std::int16_t>(actorId);
        resolved.objectId = -1; // engine valida o objeto no Actor_Spawn
        resolved.params = static_cast<std::int16_t>(params);
    }
    if (!mHooks.objectReady || !mHooks.spawn || !mHooks.kill) {
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::InvalidState,
                                                     "MM actor provider hooks are incomplete");
    }

    if (resolved.objectId >= 0) {
        bool objectReady = false;
        try {
            objectReady = mHooks.objectReady(resolved.objectId);
        } catch (const std::exception& error) {
            return ShipLua::Result<ShipLua::Handle>::err(
                ShipLua::ErrorCode::HostFailure, "MM object dependency check failed: " + std::string(error.what()));
        } catch (...) {
            return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::HostFailure,
                                                         "MM object dependency check failed");
        }
        if (!objectReady) {
            return ShipLua::Result<ShipLua::Handle>::err(
                ShipLua::ErrorCode::InvalidState, "required MM object is not loaded for actor '" + request.actor + "'");
        }
    }

    auto handle = mHandles.Create(ShipLua::HandleKind::Actor, ownerModId);
    if (!handle.isOk()) {
        return handle;
    }

    void* nativeActor = nullptr;
    try {
        nativeActor = mHooks.spawn(resolved, request);
    } catch (const std::exception& error) {
        (void)mHandles.Destroy(*handle.value, ownerModId);
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::HostFailure,
                                                     "MM actor spawn failed: " + std::string(error.what()));
    } catch (...) {
        (void)mHandles.Destroy(*handle.value, ownerModId);
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::HostFailure, "MM actor spawn failed");
    }
    if (nativeActor == nullptr) {
        (void)mHandles.Destroy(*handle.value, ownerModId);
        return ShipLua::Result<ShipLua::Handle>::err(ShipLua::ErrorCode::HostFailure, "MM rejected the actor spawn");
    }

    ActorRecord record{ *handle.value, ownerModId, request.actor, nativeActor };
    mRecords.emplace(record.handle.slot, std::move(record));
    mLogger.info(ownerModId,
                 "spawned MM actor '" + request.actor + "' in handle slot " + std::to_string(handle.value->slot));
    return handle;
}

ShipLua::Result<void> MmActorProvider::Destroy(const std::string& ownerModId, const ShipLua::Handle& handle) {
    const auto thread = ValidateThread("destroy");
    if (!thread.isOk()) {
        return thread;
    }
    const auto valid = mHandles.Validate(handle, ShipLua::HandleKind::Actor, ownerModId);
    if (!valid.isOk()) {
        return valid;
    }
    const auto record = mRecords.find(handle.slot);
    if (record == mRecords.end() || !SameHandle(record->second, handle)) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::HostFailure, "MM actor handle lost its native record");
    }
    try {
        mHooks.kill(record->second.nativeActor);
    } catch (const std::exception& error) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::HostFailure,
                                          "MM actor destroy failed: " + std::string(error.what()));
    } catch (...) { return ShipLua::Result<void>::err(ShipLua::ErrorCode::HostFailure, "MM actor destroy failed"); }
    const std::string actorKey = record->second.actorKey;
    mRecords.erase(record);
    const auto destroyed = mHandles.Destroy(handle, ownerModId);
    if (!destroyed.isOk()) {
        return destroyed;
    }
    mLogger.info(ownerModId, "destroyed MM actor '" + actorKey + "'");
    return ShipLua::Result<void>::ok();
}

ShipLua::Result<bool> MmActorProvider::Exists(const std::string& ownerModId, const ShipLua::Handle& handle) const {
    const auto thread = ValidateThread("exists");
    if (!thread.isOk()) {
        return ShipLua::Result<bool>::err(thread.code, thread.message);
    }
    const auto valid = mHandles.Validate(handle, ShipLua::HandleKind::Actor, ownerModId);
    if (!valid.isOk()) {
        return ShipLua::Result<bool>::err(valid.code, valid.message);
    }
    const auto record = mRecords.find(handle.slot);
    if (record == mRecords.end() || !SameHandle(record->second, handle)) {
        return ShipLua::Result<bool>::err(ShipLua::ErrorCode::InvalidHandle,
                                          "MM actor handle has no live native record");
    }
    return ShipLua::Result<bool>::ok(true);
}

ShipLua::Result<bool> MmActorProvider::OnNativeActorDestroyed(void* actor) {
    const auto thread = ValidateThread("native actor destroy");
    if (!thread.isOk()) {
        return ShipLua::Result<bool>::err(thread.code, thread.message);
    }
    if (actor == nullptr) {
        return ShipLua::Result<bool>::ok(false);
    }
    const auto record = std::find_if(mRecords.begin(), mRecords.end(),
                                     [actor](const auto& entry) { return entry.second.nativeActor == actor; });
    if (record == mRecords.end()) {
        return ShipLua::Result<bool>::ok(false);
    }
    const ActorRecord value = record->second;
    const auto destroyed = mHandles.Destroy(value.handle, value.ownerModId);
    mRecords.erase(record);
    if (!destroyed.isOk()) {
        return ShipLua::Result<bool>::err(destroyed.code, destroyed.message);
    }
    mLogger.debug(value.ownerModId, "invalidated externally destroyed MM actor '" + value.actorKey + "'");
    return ShipLua::Result<bool>::ok(true);
}

ShipLua::Result<std::size_t> MmActorProvider::ReleaseMod(const std::string& modId) {
    const auto thread = ValidateThread("mod release");
    if (!thread.isOk()) {
        return ShipLua::Result<std::size_t>::err(thread.code, thread.message);
    }
    if (modId.empty()) {
        return ShipLua::Result<std::size_t>::err(ShipLua::ErrorCode::InvalidArgument, "mod id cannot be empty");
    }
    std::size_t killed = 0;
    for (auto record = mRecords.begin(); record != mRecords.end();) {
        if (record->second.ownerModId != modId) {
            ++record;
            continue;
        }
        try {
            mHooks.kill(record->second.nativeActor);
        } catch (...) { mLogger.error(modId, "native kill failed during MM actor cleanup"); }
        record = mRecords.erase(record);
        ++killed;
    }
    const std::size_t released = mHandles.ReleaseMod(modId);
    if (released != killed) {
        mLogger.warn(modId, "MM actor cleanup repaired a handle/native record mismatch");
    }
    if (released > 0) {
        mLogger.info(modId, "released " + std::to_string(released) + " owned MM actor(s)");
    }
    return ShipLua::Result<std::size_t>::ok(released);
}

ShipLua::Result<std::size_t> MmActorProvider::OnSceneChange() {
    const auto thread = ValidateThread("scene change");
    if (!thread.isOk()) {
        return ShipLua::Result<std::size_t>::err(thread.code, thread.message);
    }
    for (const auto& [slot, record] : mRecords) {
        (void)slot;
        try {
            mHooks.kill(record.nativeActor);
        } catch (...) { mLogger.error(record.ownerModId, "native kill failed during MM scene cleanup"); }
    }
    mRecords.clear();
    const std::size_t invalidated = mHandles.InvalidateScene();
    if (invalidated > 0) {
        mLogger.info("host", "invalidated " + std::to_string(invalidated) + " MM actor handle(s) on scene change");
    }
    return ShipLua::Result<std::size_t>::ok(invalidated);
}

ShipLua::Result<std::size_t> MmActorProvider::Shutdown() {
    return OnSceneChange();
}

std::size_t MmActorProvider::CountForMod(const std::string& modId) const {
    return static_cast<std::size_t>(std::count_if(
        mRecords.begin(), mRecords.end(), [&modId](const auto& entry) { return entry.second.ownerModId == modId; }));
}

ShipLua::Result<void> MmActorProvider::ValidateThread(const char* operation) const {
    if (std::this_thread::get_id() != mGameThread) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::InvalidState,
                                          std::string("MM actor ") + operation + " must run on the game thread");
    }
    return ShipLua::Result<void>::ok();
}

bool MmActorProvider::IsSafeKey(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '.' ||
               value == '-';
    });
}

bool MmActorProvider::SameHandle(const ActorRecord& record, const ShipLua::Handle& handle) const {
    return record.handle == handle;
}

} // namespace ShipLuaHost
