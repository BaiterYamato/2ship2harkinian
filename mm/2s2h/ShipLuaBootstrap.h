#pragma once

namespace ShipLua {
class ModHost;
}

namespace ShipLuaHost {

void Initialize();
void Shutdown();
ShipLua::ModHost* GetModHost();

} // namespace ShipLuaHost
