#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shiplua/input/HotkeyRegistry.h"

namespace ShipLuaHost {

// Concrete HotkeyRegistry for the MM host. Owns the gShipLua.Hotkey.<modId>.<id>
// CVars, dispatches keydowns from Graph_StartFrame, and exposes the binding list
// for the settings UI.
class MmHotkeyRegistry : public ShipLua::HotkeyRegistry {
  public:
    struct Binding {
        ShipLua::HotkeyBinding meta;
        std::function<void()> onFire;
        std::string scancodeCvar; // gShipLua.Hotkey.<modId>.<id>.Scancode
        std::string enabledCvar;  // gShipLua.Hotkey.<modId>.<id>.Enabled
        int32_t defaultScancode = 0;
    };

    bool Register(const ShipLua::HotkeyBinding& binding, std::function<void()> onFire) override;
    void UnregisterMod(const std::string& modId) override;
    void Fire(const std::string& modId, const std::string& id) override;
    std::vector<ShipLua::HotkeyBinding> Registered() const override;

    // Called from Graph_StartFrame each keydown.
    void DispatchScancode(int32_t scancode);

    const std::vector<Binding>& Bindings() const {
        return mBindings;
    }

    // Physical key name <-> scancode. Unknown names are rejected instead of
    // silently changing a mod's requested default.
    static int32_t ParseKeyName(const std::string& name);
    static std::string KeyName(int32_t scancode);

  private:
    std::vector<Binding> mBindings;
};

} // namespace ShipLuaHost
