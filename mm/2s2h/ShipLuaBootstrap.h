#pragma once

#include <string>

namespace ShipLua {
class ModHost;
}

namespace ShipLuaHost {

class MmHotkeyRegistry;
class MmWorldAdapter;

void Initialize();
void Shutdown();
ShipLua::ModHost* GetModHost();

// Owned hotkey registry (created in Initialize, destroyed in Shutdown).
// May be nullptr before Initialize or after Shutdown.
MmHotkeyRegistry* Hotkeys();
MmWorldAdapter* WorldAdapter();

} // namespace ShipLuaHost
