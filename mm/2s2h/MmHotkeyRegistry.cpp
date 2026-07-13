#include "MmHotkeyRegistry.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h>

namespace ShipLuaHost {

namespace {

struct KeyNameEntry {
    const char* name;
    Ship::KbScancode scancode;
};

// Physical key names a mod may declare as `default`. Covers common keys; the
// user can rebind to anything in the UI.
const KeyNameEntry kKeyNames[] = {
    { "A", Ship::KbScancode::LUS_KB_A },
    { "B", Ship::KbScancode::LUS_KB_B },
    { "C", Ship::KbScancode::LUS_KB_C },
    { "D", Ship::KbScancode::LUS_KB_D },
    { "E", Ship::KbScancode::LUS_KB_E },
    { "F", Ship::KbScancode::LUS_KB_F },
    { "G", Ship::KbScancode::LUS_KB_G },
    { "H", Ship::KbScancode::LUS_KB_H },
    { "I", Ship::KbScancode::LUS_KB_I },
    { "J", Ship::KbScancode::LUS_KB_J },
    { "K", Ship::KbScancode::LUS_KB_K },
    { "L", Ship::KbScancode::LUS_KB_L },
    { "M", Ship::KbScancode::LUS_KB_M },
    { "N", Ship::KbScancode::LUS_KB_N },
    { "O", Ship::KbScancode::LUS_KB_O },
    { "P", Ship::KbScancode::LUS_KB_P },
    { "Q", Ship::KbScancode::LUS_KB_Q },
    { "R", Ship::KbScancode::LUS_KB_R },
    { "S", Ship::KbScancode::LUS_KB_S },
    { "T", Ship::KbScancode::LUS_KB_T },
    { "U", Ship::KbScancode::LUS_KB_U },
    { "V", Ship::KbScancode::LUS_KB_V },
    { "W", Ship::KbScancode::LUS_KB_W },
    { "X", Ship::KbScancode::LUS_KB_X },
    { "Y", Ship::KbScancode::LUS_KB_Y },
    { "Z", Ship::KbScancode::LUS_KB_Z },
    { "0", Ship::KbScancode::LUS_KB_0 },
    { "1", Ship::KbScancode::LUS_KB_1 },
    { "2", Ship::KbScancode::LUS_KB_2 },
    { "3", Ship::KbScancode::LUS_KB_3 },
    { "4", Ship::KbScancode::LUS_KB_4 },
    { "5", Ship::KbScancode::LUS_KB_5 },
    { "6", Ship::KbScancode::LUS_KB_6 },
    { "7", Ship::KbScancode::LUS_KB_7 },
    { "8", Ship::KbScancode::LUS_KB_8 },
    { "9", Ship::KbScancode::LUS_KB_9 },
    { "F1", Ship::KbScancode::LUS_KB_F1 },
    { "F2", Ship::KbScancode::LUS_KB_F2 },
    { "F3", Ship::KbScancode::LUS_KB_F3 },
    { "F4", Ship::KbScancode::LUS_KB_F4 },
    { "F5", Ship::KbScancode::LUS_KB_F5 },
    { "F6", Ship::KbScancode::LUS_KB_F6 },
    { "F7", Ship::KbScancode::LUS_KB_F7 },
    { "F8", Ship::KbScancode::LUS_KB_F8 },
    { "F9", Ship::KbScancode::LUS_KB_F9 },
    { "F10", Ship::KbScancode::LUS_KB_F10 },
    { "F11", Ship::KbScancode::LUS_KB_F11 },
    { "F12", Ship::KbScancode::LUS_KB_F12 },
    { "Space", Ship::KbScancode::LUS_KB_SPACE },
    { "Enter", Ship::KbScancode::LUS_KB_ENTER },
    { "Tab", Ship::KbScancode::LUS_KB_TAB },
    { "Shift", Ship::KbScancode::LUS_KB_SHIFT },
    { "Control", Ship::KbScancode::LUS_KB_CONTROL },
    { "Alt", Ship::KbScancode::LUS_KB_ALT },
    { "Up", Ship::KbScancode::LUS_KB_ARROWKEY_UP },
    { "Down", Ship::KbScancode::LUS_KB_ARROWKEY_DOWN },
    { "Left", Ship::KbScancode::LUS_KB_ARROWKEY_LEFT },
    { "Right", Ship::KbScancode::LUS_KB_ARROWKEY_RIGHT },
};

} // namespace

int32_t MmHotkeyRegistry::ParseKeyName(const std::string& name) {
    for (const auto& entry : kKeyNames) {
        if (name == entry.name) {
            return static_cast<int32_t>(entry.scancode);
        }
    }
    return -1;
}

std::string MmHotkeyRegistry::KeyName(int32_t scancode) {
    for (const auto& entry : kKeyNames) {
        if (static_cast<int32_t>(entry.scancode) == scancode) {
            return entry.name;
        }
    }
    return "Sem tecla";
}

bool MmHotkeyRegistry::Register(const ShipLua::HotkeyBinding& binding, std::function<void()> onFire) {
    const int32_t defaultScancode = binding.defaultKey.empty() ? -1 : ParseKeyName(binding.defaultKey);
    if (!binding.defaultKey.empty() && defaultScancode < 0) {
        SPDLOG_WARN("ShipLua [{}]: hotkey '{}': tecla padrão desconhecida '{}'", binding.modId, binding.id,
                    binding.defaultKey);
        return false;
    }
    for (auto& existing : mBindings) {
        if (existing.meta.modId == binding.modId && existing.meta.id == binding.id) {
            existing.meta = binding;
            existing.onFire = std::move(onFire);
            existing.defaultScancode = defaultScancode;
            return true;
        }
    }
    Binding b;
    b.meta = binding;
    b.onFire = std::move(onFire);
    b.scancodeCvar = "gShipLua.Hotkey." + binding.modId + "." + binding.id + ".Scancode";
    b.enabledCvar = "gShipLua.Hotkey." + binding.modId + "." + binding.id + ".Enabled";
    b.defaultScancode = defaultScancode;
    mBindings.push_back(std::move(b));
    SPDLOG_INFO("ShipLua hotkey registrado: {}.{} (default={}/{})", binding.modId, binding.id, binding.defaultKey,
                mBindings.back().defaultScancode);
    return true;
}

void MmHotkeyRegistry::UnregisterMod(const std::string& modId) {
    std::erase_if(mBindings, [&](const Binding& binding) { return binding.meta.modId == modId; });
}

void MmHotkeyRegistry::Fire(const std::string& modId, const std::string& id) {
    for (auto& b : mBindings) {
        if (b.meta.modId == modId && b.meta.id == id && b.onFire) {
            const std::function<void()> callback = b.onFire;
            callback();
            return;
        }
    }
}

std::vector<ShipLua::HotkeyBinding> MmHotkeyRegistry::Registered() const {
    std::vector<ShipLua::HotkeyBinding> out;
    out.reserve(mBindings.size());
    for (const auto& b : mBindings) {
        out.push_back(b.meta);
    }
    return out;
}

void MmHotkeyRegistry::DispatchScancode(int32_t scancode) {
    if (scancode <= 0) {
        return;
    }
    std::vector<std::function<void()>> callbacks;
    for (const auto& b : mBindings) {
        if (!b.onFire) {
            continue;
        }
        if (CVarGetInteger(b.enabledCvar.c_str(), 1) == 0) {
            continue;
        }
        const int32_t configured = CVarGetInteger(b.scancodeCvar.c_str(), b.defaultScancode);
        if (configured >= 0 && scancode == configured) {
            callbacks.push_back(b.onFire);
        }
    }
    for (const auto& callback : callbacks) {
        callback();
    }
}

} // namespace ShipLuaHost
