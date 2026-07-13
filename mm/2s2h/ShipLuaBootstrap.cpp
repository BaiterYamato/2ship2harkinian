#include "ShipLuaBootstrap.h"

#include <memory>

#include <spdlog/spdlog.h>

#include <shiplua/host/ModHost.h>

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

} // namespace

void Initialize() {
    if (gModHost != nullptr) {
        SPDLOG_WARN("ShipLua j\xC3\xA1 foi inicializado");
        return;
    }

    gModHost = std::make_unique<ShipLua::ModHost>(CreateLogger());
    SPDLOG_INFO("ShipLua inicializado");
}

ShipLua::ModHost* GetModHost() {
    return gModHost.get();
}

} // namespace ShipLuaHost
