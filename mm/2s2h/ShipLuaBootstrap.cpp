#include "ShipLuaBootstrap.h"
#include "MmHotkeyRegistry.h"
#include "MmWorldAdapter.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <shiplua/generated/ApiBindings.h>
#include <shiplua/host/ModHost.h>
#include <shiplua/runtime/LuaRuntime.h>
#include <shiplua/storage/AtomicFile.h>
#include <shiplua/world/WorldHandoff.h>

#include "GameInteractor/GameInteractor.h"

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
std::shared_ptr<MmWorldAdapter> gWorldAdapter;
HOOK_ID gSaveLoadHook = 0;

constexpr int kSwitchWorldExitCode = 73;

struct BridgeConfig {
    std::filesystem::path sessionDirectory;
    std::filesystem::path handoffPath;
    std::array<std::byte, 16> sessionId{};
    std::array<std::byte, 32> authenticationKey{};
    std::uint64_t sequence = 0;
};

int HexDigit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

template <std::size_t Size>
bool ParseHex(const char* text, std::array<std::byte, Size>& output) {
    if (text == nullptr || std::char_traits<char>::length(text) != Size * 2) {
        return false;
    }
    for (std::size_t index = 0; index < Size; ++index) {
        const int high = HexDigit(text[index * 2]);
        const int low = HexDigit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

std::optional<BridgeConfig> GetBridgeConfig() {
    const char* sessionDirectory = std::getenv("LINKSPAN_SESSION_DIR");
    const char* handoffPath = std::getenv("LINKSPAN_HANDOFF_PATH");
    const char* sequence = std::getenv("LINKSPAN_SEQUENCE");
    if (sessionDirectory == nullptr || handoffPath == nullptr || sequence == nullptr) {
        return std::nullopt;
    }
    BridgeConfig config;
    config.sessionDirectory = sessionDirectory;
    config.handoffPath = handoffPath;
    if (!ParseHex(std::getenv("LINKSPAN_SESSION_ID"), config.sessionId) ||
        !ParseHex(std::getenv("LINKSPAN_AUTH_KEY"), config.authenticationKey)) {
        return std::nullopt;
    }
    char* end = nullptr;
    config.sequence = std::strtoull(sequence, &end, 10);
    if (end == sequence || *end != '\0' || config.sequence == 0) {
        return std::nullopt;
    }
    return config;
}

bool BothGamesAvailable() {
    const char* available = std::getenv("LINKSPAN_AVAILABLE_GAMES");
    if (available == nullptr) {
        return false;
    }
    const std::string games(available);
    return games.find("oot") != std::string::npos && games.find("mm") != std::string::npos;
}

ShipLua::Result<void> RequestWorldTravel(const ShipLua::WorldDestination& destination) {
    const auto config = GetBridgeConfig();
    if (!config.has_value() || !BothGamesAvailable() || destination.world != ShipLua::WorldId::Oot ||
        gWorldAdapter == nullptr) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::Unsupported,
                                          "ponte Link-Span para OoT indisponível");
    }
    const auto player = gWorldAdapter->CapturePlayerState();
    if (!player.isOk()) {
        return ShipLua::Result<void>::err(player.code, player.message);
    }
    ShipLua::WorldHandoff handoff;
    handoff.sessionId = config->sessionId;
    handoff.sequence = config->sequence;
    handoff.source = ShipLua::WorldId::Mm;
    handoff.destination = destination;
    handoff.player = *player.value;
    const auto written = ShipLua::WorldHandoffCodec::WriteFile(
        config->handoffPath, handoff, config->authenticationKey);
    if (!written.isOk()) {
        return written;
    }
    const auto requested = ShipLua::AtomicFile::Write(config->sessionDirectory / "next-world", "oot\n");
    if (!requested.isOk()) {
        std::error_code ignored;
        std::filesystem::remove(config->handoffPath, ignored);
        return requested;
    }
    SPDLOG_INFO("Link-Span exportou o estado MM e solicitou troca para OoT ({})", destination.id);
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
    std::exit(kSwitchWorldExitCode);
}

void TryConsumeWorldHandoff() {
    const auto config = GetBridgeConfig();
    if (!config.has_value() || gWorldAdapter == nullptr ||
        !std::filesystem::is_regular_file(config->handoffPath)) {
        return;
    }
    const auto handoff = ShipLua::WorldHandoffCodec::ReadFile(
        config->handoffPath, config->authenticationKey);
    if (!handoff.isOk()) {
        SPDLOG_ERROR("Link-Span rejeitou o handoff MM: {}", handoff.message);
        return;
    }
    if (handoff.value->sessionId != config->sessionId || handoff.value->sequence + 1 != config->sequence ||
        handoff.value->destination.world != ShipLua::WorldId::Mm) {
        SPDLOG_ERROR("Link-Span rejeitou um handoff destinado a outra sessão ou jogo");
        return;
    }
    const auto prepared = gWorldAdapter->PrepareImport(handoff.value->player, handoff.value->destination);
    if (!prepared.isOk()) {
        SPDLOG_ERROR("Link-Span não preparou a importação MM: {}", prepared.message);
        return;
    }
    const auto committed = gWorldAdapter->CommitImport();
    if (!committed.isOk()) {
        gWorldAdapter->AbortImport();
        SPDLOG_ERROR("Link-Span não confirmou a importação MM: {}", committed.message);
        return;
    }
    std::error_code error;
    std::filesystem::remove(config->handoffPath, error);
    SPDLOG_INFO("Link-Span importou o estado compartilhado em MM ({})", handoff.value->destination.id);
}

#ifdef _WIN32
std::filesystem::path RuntimeRoot() {
    if (const wchar_t* configured = _wgetenv(L"LINKSPAN_ROOT"); configured != nullptr && *configured != L'\0') {
        return configured;
    }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return std::filesystem::current_path();
    }
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::wstring QuotePowerShellLiteral(std::wstring value) {
    std::size_t position = 0;
    while ((position = value.find(L'\'', position)) != std::wstring::npos) {
        value.insert(position, 1, L'\'');
        position += 2;
    }
    return L"'" + value + L"'";
}
#endif

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
    context.capabilities = { "mm.player.jump", "mm.spawn_dog" };
    context.hotkeys = gHotkeys;
    if (const char* available = std::getenv("LINKSPAN_AVAILABLE_GAMES"); available != nullptr) {
        const std::string games(available);
        if (games.find("oot") != std::string::npos) {
            context.availableGames.push_back("oot");
        }
        if (games.find("mm") != std::string::npos) {
            context.availableGames.push_back("mm");
        }
    }
    if (BothGamesAvailable() && GetBridgeConfig().has_value()) {
        context.capabilities.push_back("world.travel");
        context.worldTravel = RequestWorldTravel;
    }
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
    auto catalog = ShipLua::PortableItemCatalog::CreateDefault();
    if (!catalog.isOk()) {
        SPDLOG_ERROR("ShipLua não conseguiu criar o catálogo portátil MM: {}", catalog.message);
        gHotkeys.reset();
        return;
    }
    gWorldAdapter = std::make_shared<MmWorldAdapter>(std::move(*catalog.value));
    gSaveLoadHook = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSaveLoad>(
        [](s16) { TryConsumeWorldHandoff(); });
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
    if (gSaveLoadHook != 0) {
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnSaveLoad>(gSaveLoadHook);
        gSaveLoadHook = 0;
    }
    gWorldAdapter.reset();
    gHotkeys.reset();
    SPDLOG_INFO("ShipLua finalizado");
}

ShipLua::ModHost* GetModHost() {
    return gModHost.get();
}

MmHotkeyRegistry* Hotkeys() {
    return gHotkeys.get();
}

MmWorldAdapter* WorldAdapter() {
    return gWorldAdapter.get();
}

void OpenLogWindow() {
#ifdef _WIN32
    const std::filesystem::path log = RuntimeRoot() / "logs" / "2 Ship 2 Harkinian.log";
    std::filesystem::create_directories(log.parent_path());
    std::wstring command =
        L"powershell.exe -NoLogo -NoProfile -NoExit -Command \"$host.UI.RawUI.WindowTitle='Link-Span - log MM'; "
        L"Write-Host 'Aguardando o log de MM...'; while(-not (Test-Path -LiteralPath " +
        QuotePowerShellLiteral(log.wstring()) + L")){Start-Sleep -Milliseconds 250}; Get-Content -LiteralPath " +
        QuotePowerShellLiteral(log.wstring()) + L" -Tail 200 -Wait\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        RuntimeRoot().c_str(), &startup, &process)) {
        SPDLOG_ERROR("ShipLua não conseguiu abrir a janela de log (erro {})", GetLastError());
        return;
    }
    SPDLOG_INFO("ShipLua abriu uma nova janela para acompanhar o log MM");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    SPDLOG_WARN("ShipLua OpenLogWindow só está disponível no Windows");
#endif
}

} // namespace ShipLuaHost
