#include "ShipLuaBootstrap.h"
#include "MmHotkeyRegistry.h"

#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <shiplua/generated/ApiBindings.h>
#include <shiplua/host/ModHost.h>
#include <shiplua/runtime/LuaRuntime.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "build.h"

// Privileged adapter code — allowed to touch MM internals to implement the
// host-specific ship.mm.* bindings. (The shared core never includes game headers.)
#include "variables.h"
#include "z64.h"

namespace ShipLuaHost {
namespace {

std::unique_ptr<ShipLua::ModHost> gModHost;
std::shared_ptr<MmHotkeyRegistry> gHotkeys;

ShipLua::Logger CreateLogger() {
    return ShipLua::Logger([](ShipLua::LogLevel level, const std::string& modId, const std::string& message) {
        switch (level) {
            case ShipLua::LogLevel::Debug:
                SPDLOG_DEBUG("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Info:
                SPDLOG_INFO("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Warn:
                SPDLOG_WARN("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Error:
                SPDLOG_ERROR("ShipLua [{}]: {}", modId, message);
                break;
        }
    });
}

std::string GetHostVersion() {
    return std::to_string(gBuildVersionMajor) + "." + std::to_string(gBuildVersionMinor) + "." +
           std::to_string(gBuildVersionPatch);
}

ShipLua::LuaApiHostContext CreateHostContext() {
    ShipLua::LuaApiHostContext context;
    context.gameId = "mm";
    context.hostVersion = GetHostVersion();
    context.capabilities = { "mm.player.jump" };
    context.hotkeys = gHotkeys;
    return context;
}

// ship.mm.player.jump(): applies a host-controlled vertical impulse only when
// the player is alive and standing on the ground. No internal pointer or force
// value crosses the Lua boundary.
int LuaPlayerJump(lua_State* L) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }

    Player* player = GET_PLAYER(play);
    if (player == nullptr || (player->stateFlags1 & PLAYER_STATE1_DEAD) != 0 ||
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    player->actor.velocity.y = 6.34375f;
    lua_pushboolean(L, 1);
    return 1;
}

// ship.mm.spawn_dog(): spawns the Clock Town dog (En_Dg) at the player's
// position, returning true on success. object_dog is loaded in Clock Town, so
// the dog appears reliably there.
int LuaSpawnDog(lua_State* L) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        SPDLOG_WARN("ShipLua spawn_dog: gPlayState nulo (fora de gameplay)");
        lua_pushboolean(L, 0);
        return 1;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        SPDLOG_WARN("ShipLua spawn_dog: player nulo");
        lua_pushboolean(L, 0);
        return 1;
    }

    // Diagnostics: scene, player form, and whether object_dog is loaded in the scene.
    s32 dogSlot = Object_GetSlot(&play->objectCtx, OBJECT_DOG);
    SPDLOG_INFO("ShipLua spawn_dog: sceneId={} form={} object_dog_slot={} (OBJECT_SLOT_NONE={})",
                (int)play->sceneId, (int)player->transformation, (int)dogSlot, (int)OBJECT_SLOT_NONE);

    // 0x03E0 = ENDG_PARAMS(path=0, index=ENDG_INDEX_SOUTH_CLOCK_TOWN=31).
    // A South Clock Town dog. It MUST have a valid path: EnDg_IdleMove ->
    // EnDg_MoveAlongPath does Actor_Kill when this->path == NULL, which is why a
    // PATH_INDEX_NONE dog self-destructs the moment it idles (e.g. as human Link).
    // Clock Town scenes have dog paths, so path index 0 keeps it alive.
    Actor* dog =
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_DG, player->actor.world.pos.x, player->actor.world.pos.y,
                    player->actor.world.pos.z, 0, player->actor.shape.rot.y, 0, (s16)0x03E0);
    SPDLOG_INFO("ShipLua spawn_dog: Actor_Spawn -> {} em pos=({:.0f},{:.0f},{:.0f})",
                dog != nullptr ? "ator criado" : "NULL", player->actor.world.pos.x, player->actor.world.pos.y,
                player->actor.world.pos.z);
    lua_pushboolean(L, dog != nullptr);
    return 1;
}

// Installs the MM-specific ship.mm.* table onto a mod runtime. Uses
// require("ship") so it works regardless of how the core registers the module.
void InstallMmApi(lua_State* L) {
    if (L == nullptr) {
        return;
    }
    lua_getglobal(L, "require");
    lua_pushstring(L, "ship");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int shipTable = lua_gettop(L);
    lua_getfield(L, shipTable, "mm");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    const int mmTable = lua_gettop(L);
    lua_pushcfunction(L, LuaSpawnDog);
    lua_setfield(L, -2, "spawn_dog");

    lua_getfield(L, mmTable, "player");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, LuaPlayerJump);
    lua_setfield(L, -2, "jump");
    lua_setfield(L, mmTable, "player");

    lua_setfield(L, shipTable, "mm");
    lua_pop(L, 1);
}

void LoadModsAndDispatchReady(const ShipLua::LuaApiHostContext& context) {
    const std::shared_ptr<Ship::Context> shipContext = Ship::Context::GetInstance();
    if (shipContext == nullptr) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o encontrou o contexto do aplicativo");
        return;
    }

    const std::string appName = shipContext->GetShortName();
    const std::filesystem::path modsRoot = Ship::Context::GetPathRelativeToAppDirectory("mods", appName);
    const std::filesystem::path cacheRoot = modsRoot / ".shiplua-cache";
    std::error_code error;
    std::filesystem::create_directories(modsRoot, error);
    if (error) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu criar a pasta de mods '{}': {}", modsRoot.string(), error.message());
        return;
    }

    auto loaded = gModHost->LoadModsFromRoot(modsRoot, cacheRoot);
    if (!loaded.isOk()) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu carregar a pasta de mods '{}': {}", modsRoot.string(),
                     loaded.message);
        return;
    }
    for (const auto& [modId, reason] : loaded.value->rejected) {
        SPDLOG_WARN("ShipLua rejeitou o mod '{}': {}", modId, reason);
    }
    SPDLOG_INFO("ShipLua carregou {} mod(s) de '{}'", loaded.value->loadedIds.size(), modsRoot.string());

    // Install MM-specific bindings (ship.mm.*) on every loaded runtime before
    // game.ready so mods can use them from the first event onwards.
    for (const std::string& modId : loaded.value->loadedIds) {
        ShipLua::LuaRuntime* runtime = gModHost->GetRuntime(modId);
        if (runtime != nullptr) {
            InstallMmApi(runtime->State());
        }
    }

    ShipLua::EventPayload payload{
        { "game_id", context.gameId },
        { "host_version", context.hostVersion },
        { "runtime_version", context.runtimeVersion },
        { "api_version", std::string(ShipLua::Generated::kApiVersion) },
    };
    auto ready = gModHost->DispatchEvent("game.ready", payload);
    if (!ready.isOk()) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu publicar game.ready: {}", ready.message);
        return;
    }
    for (const ShipLua::CallbackFailure& failure : ready.value->failures) {
        SPDLOG_ERROR("ShipLua [{}] falhou em game.ready: {}", failure.modId, failure.message);
    }
}

} // namespace

void Initialize() {
    if (gModHost != nullptr) {
        SPDLOG_WARN("ShipLua j\xC3\xA1 foi inicializado");
        return;
    }

    gHotkeys = std::make_shared<MmHotkeyRegistry>();
    ShipLua::LuaApiHostContext context = CreateHostContext();
    SPDLOG_INFO("ShipLua inicializando para {} {} (commit {})", context.gameId, context.hostVersion, gGitCommitHash);
    gModHost = std::make_unique<ShipLua::ModHost>(context, CreateLogger());
    LoadModsAndDispatchReady(context);
    SPDLOG_INFO("ShipLua inicializado");
}

void Shutdown() {
    if (gModHost == nullptr) {
        return;
    }

    gModHost.reset();
    gHotkeys.reset();
    SPDLOG_INFO("ShipLua finalizado");
}

ShipLua::ModHost* GetModHost() {
    return gModHost.get();
}

MmHotkeyRegistry* Hotkeys() {
    return gHotkeys.get();
}

} // namespace ShipLuaHost
