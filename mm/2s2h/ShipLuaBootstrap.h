#pragma once

#include <string>

namespace ShipLua {
class ModHost;
}

namespace ShipLuaHost {

void Initialize();
void Shutdown();
ShipLua::ModHost* GetModHost();

// Dispatches an "input.hotkey" event to loaded mods. Called from the port's
// keyboard handler when the configured hotkey is pressed.
void DispatchHotkey(const std::string& action);

} // namespace ShipLuaHost
