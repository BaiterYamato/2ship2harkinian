#include "2s2h/MmActorProvider.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct FakeActor {
    int actorId = -1;
    bool killed = false;
};

struct Fixture {
    std::set<std::int16_t> readyObjects{ 10 };
    std::vector<std::unique_ptr<FakeActor>> actors;
    ShipLuaHost::MmActorProvider provider;

    explicit Fixture(ShipLua::HandleLimits limits = { 16, 256 })
        : provider(
              {
                  { "mm.en_dg", 1, 10, static_cast<std::int16_t>(0x03E0) },
                  { "mm.en_elf", 2, 20, 0 },
                  { "player", 0, 30, 0 },
              },
              ShipLuaHost::MmActorProviderHooks{
                  [this](std::int16_t objectId) { return readyObjects.contains(objectId); },
                  [this](const ShipLuaHost::MmActorDefinition& definition, const ShipLua::ActorSpawnRequest&) -> void* {
                      auto actor = std::make_unique<FakeActor>();
                      actor->actorId = definition.actorId;
                      FakeActor* native = actor.get();
                      actors.emplace_back(std::move(actor));
                      return native;
                  },
                  [](void* actor) { static_cast<FakeActor*>(actor)->killed = true; },
              },
              ShipLua::Logger([](ShipLua::LogLevel, const std::string&, const std::string&) {}), 0, limits) {
    }
};

ShipLua::ActorSpawnRequest DogRequest() {
    ShipLua::ActorSpawnRequest request;
    request.actor = "mm.en_dg";
    request.x = 1.0;
    request.y = 2.0;
    request.z = 3.0;
    return request;
}

void TestCapabilitiesAndAllowlist() {
    Fixture fixture;
    Check(fixture.provider.AllowedActorCount() == 2, "ACTOR_PLAYER must be removed from allowlist");

    ShipLua::CapabilityRegistry registry;
    Check(fixture.provider.RegisterCapabilities(registry).isOk(), "capabilities should register");
    const auto spawn = registry.Info("actor.spawn", "mm");
    Check(spawn.has_value(), "actor.spawn descriptor should exist for MM");
    Check(spawn->provider == "2ship-native", "provider name should be stable");
    Check(spawn->limits.perMod == 16, "provider descriptor should publish the per-mod limit");
    Check(!registry.Has("actor.spawn", "oot"), "MM provider must not advertise OoT support");

    ShipLua::ActorSpawnRequest player = DogRequest();
    player.actor = "player";
    const auto blocked = fixture.provider.Spawn("test.mod", player);
    Check(blocked.code == ShipLua::ErrorCode::Unsupported, "ACTOR_PLAYER must stay blocked");
}

void TestSpawnDestroyAndOwnership() {
    Fixture fixture;
    const auto spawned = fixture.provider.Spawn("test.mod", DogRequest());
    Check(spawned.isOk(), "allowlisted actor should spawn");
    Check(spawned.value->kind == ShipLua::HandleKind::Actor, "spawn should mint actor handle");
    Check(fixture.provider.Exists("test.mod", *spawned.value).isOk(), "owner should resolve handle");

    const auto foreign = fixture.provider.Exists("other.mod", *spawned.value);
    Check(foreign.code == ShipLua::ErrorCode::PermissionDenied, "another mod must not resolve the handle");

    Check(fixture.provider.Destroy("test.mod", *spawned.value).isOk(), "owner should destroy actor");
    Check(fixture.actors.front()->killed, "native actor should be killed");
    const auto stale = fixture.provider.Exists("test.mod", *spawned.value);
    Check(stale.code == ShipLua::ErrorCode::InvalidHandle, "destroyed handle must be stale");
}

void TestObjectDependencyAndInvalidHandle() {
    Fixture fixture;
    ShipLua::ActorSpawnRequest fairy = DogRequest();
    fairy.actor = "mm.en_elf";
    const auto missing = fixture.provider.Spawn("test.mod", fairy);
    Check(missing.code == ShipLua::ErrorCode::InvalidState, "actor must require its object dependency");

    fixture.readyObjects.insert(20);
    Check(fixture.provider.Spawn("test.mod", fairy).isOk(), "actor should spawn after its object is ready");

    ShipLua::Handle invalid;
    invalid.kind = ShipLua::HandleKind::Actor;
    invalid.slot = 999;
    invalid.generation = 1;
    invalid.sceneGeneration = 1;
    const auto result = fixture.provider.Destroy("test.mod", invalid);
    Check(result.code == ShipLua::ErrorCode::InvalidHandle, "invalid handle must fail without touching native memory");
}

void TestNativeDestroyAndLifecycleCleanup() {
    Fixture fixture;
    const auto first = fixture.provider.Spawn("first.mod", DogRequest());
    const auto second = fixture.provider.Spawn("second.mod", DogRequest());
    Check(first.isOk() && second.isOk(), "two mods should spawn independently");

    Check(fixture.provider.OnNativeActorDestroyed(fixture.actors.front().get()).value.value_or(false),
          "native destroy hook should invalidate a tracked actor");
    Check(fixture.provider.Exists("first.mod", *first.value).code == ShipLua::ErrorCode::InvalidHandle,
          "native destroy should stale the handle");

    const auto released = fixture.provider.ReleaseMod("second.mod");
    Check(released.isOk() && *released.value == 1, "mod unload should release owned actors");
    Check(fixture.actors.back()->killed, "mod unload should kill its native actor");

    const auto third = fixture.provider.Spawn("third.mod", DogRequest());
    Check(third.isOk(), "provider should remain usable after mod cleanup");
    const std::uint32_t oldScene = third.value->sceneGeneration;
    const auto changed = fixture.provider.OnSceneChange();
    Check(changed.isOk() && *changed.value == 1, "scene change should invalidate remaining actors");
    Check(fixture.provider.Handles().SceneGeneration() != oldScene, "scene change must advance handle generation");
}

void TestLimitsAndGameThread() {
    Fixture fixture({ 1, 8 });
    ShipLua::CapabilityRegistry registry;
    Check(fixture.provider.RegisterCapabilities(registry).isOk(), "custom limits should register");
    const auto descriptor = registry.Info("actor.spawn", "mm");
    Check(descriptor.has_value() && descriptor->limits.perMod == 1,
          "capability descriptor must match the configured per-mod limit");
    Check(fixture.provider.Spawn("test.mod", DogRequest()).isOk(), "first actor should fit limit");
    const auto limited = fixture.provider.Spawn("test.mod", DogRequest());
    Check(limited.code == ShipLua::ErrorCode::ResourceLimit, "per-mod actor limit must be enforced");

    ShipLua::Result<ShipLua::Handle> threaded;
    std::thread worker([&]() { threaded = fixture.provider.Spawn("worker.mod", DogRequest()); });
    worker.join();
    Check(threaded.code == ShipLua::ErrorCode::InvalidState, "actor spawn from a worker thread must be rejected");
}

} // namespace

int main() {
    static_assert(std::is_base_of_v<ShipLua::ActorProvider, ShipLuaHost::MmActorProvider>);
    TestCapabilitiesAndAllowlist();
    TestSpawnDestroyAndOwnership();
    TestObjectDependencyAndInvalidHandle();
    TestNativeDestroyAndLifecycleCleanup();
    TestLimitsAndGameThread();
    std::cout << "MmActorProviderTests: OK\n";
    return 0;
}
