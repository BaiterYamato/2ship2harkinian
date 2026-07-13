#include "ShipLuaBootstrap.h"

#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <shiplua/generated/ApiBindings.h>
#include <shiplua/host/ModHost.h>

#include "build.h"

namespace ShipLuaHost {
namespace {

std::unique_ptr<ShipLua::ModHost> gModHost;

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
    return context;
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
    SPDLOG_INFO("ShipLua finalizado");
}

ShipLua::ModHost* GetModHost() {
    return gModHost.get();
}

} // namespace ShipLuaHost
