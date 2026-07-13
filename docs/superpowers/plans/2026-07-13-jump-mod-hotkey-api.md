# Jump Mod + `ship.hotkeys` API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a Jump mod (default hotkey K, rebindable in-game) backed by a new cross-game `ship.hotkeys.register` API in the ship-lua core and a concrete `HotkeyRegistry` implementation injected by the MM host.

**Architecture:** Three layers. (1) Core (`extern/ship-lua`) gains an abstract `HotkeyRegistry` SPI + a `ship.hotkeys.register` Lua binding that stores the Lua callback as a registry ref and calls into the injected registry. (2) The MM host (`mm/2s2h`) injects a `MmHotkeyRegistry` that owns the CVars, dispatches keydowns from `Graph_StartFrame`, drives a new ImGui "ShipLua Hotkeys" settings panel, and adds a `ship.mm.jump(height)` binding. (3) The `jump` example mod wires them together.

**Tech Stack:** C++20, Lua 5.4, libultraship (ImGui/CVar/KbScancode), Python codegen, CMake/CTest.

## Global Constraints

Copied verbatim from the spec + AGENTS.md:

- **Core never includes game headers.** `extern/ship-lua/src` and `extern/ship-lua/include` may not include anything from `mm/` or libultraship's input/window headers. Key-name → scancode conversion lives ONLY in the MM host.
- **Cross-game API.** `ship.hotkeys.register` must work unchanged on any host that injects a `HotkeyRegistry`. If none is injected, it is a graceful no-op (warn + return false); the mod still loads.
- **`api_version` bumps `0.1.0` → `0.2.0`** (additive SemVer minor) in `schema/api.yml`; `runtime_version` default in `LuaApiHostContext` also bumps to `0.2.0`.
- **Keep `input.hotkey` in the schema** (do not remove) for backward compatibility.
- **Remove the experimental `DogHotkey`** block in `BenPort.cpp` and migrate `examples/dog-spawner` to `ship.hotkeys`.
- **Git (per AGENTS.md):** never commit to `main`/`develop`/`lua/main` without explicit user authorization. Use branch `agent/HOTKEY-001-hotkey-api` in the `ship-lua` submodule; the MM host work continues on the current `agent/MM-005-mod-directory` branch. The user authorized commits as part of "execute automatically."
- **CVar naming:** `gShipLua.Hotkey.<modId>.<id>.Scancode` and `.Enabled` (modId dots preserved verbatim, e.g. `gShipLua.Hotkey.community.jump.Scancode`).
- **Default key "K"** = scancode value for K, declared by the mod as the string `"K"`; the host converts.
- **Jump is grounded-only:** gate on `BGCHECKFLAG_GROUND` (bit 0 of `actor.bgCheckFlags`, macro defined `mm/include/z64actor.h:101`). Use the macro, not a magic `& 1`.
- **Python interpreter** is `python` (3.14) on this machine; the codegen scripts are at `extern/ship-lua/tools/`.
- **Commits are authorized** (user said "execute automatically, I just want the final result"). Do NOT push without explicit ask.

## Repos and branches

- **Submodule `extern/ship-lua`** (remote `BaiterYamato/ship-lua`): create branch `agent/HOTKEY-001-hotkey-api` off the current detached HEAD (`694ed54`). Core SPI + binding + schema/codegen + examples + tests + version bump.
- **MM host** (`D:\Desenvolvimento\ship-lua-worktrees\MM-003`, branch `agent/MM-005-mod-directory`): bump submodule pointer to the new branch's HEAD, then add `MmHotkeyRegistry`, `LuaJump`, BenPort change, UI panel.

---

## File Structure

### Submodule `extern/ship-lua` (branch `agent/HOTKEY-001-hotkey-api`)

| File | Responsibility |
|------|----------------|
| **Create** `include/shiplua/input/HotkeyRegistry.h` | Abstract SPI + `HotkeyBinding` struct + `NullHotkeyRegistry`. Pure header, no game deps. |
| **Create** `src/input/HotkeyRegistry.cpp` | `NullHotkeyRegistry` impl (logs warn on Register). |
| **Modify** `include/shiplua/api/LuaApiBinding.h` | Add `hotkeys` to `LuaApiHostContext`; add `mHotkeys` member + `HotkeysRegister` decl. |
| **Modify** `src/api/LuaApiBinding.cpp` | Store `mHotkeys` in ctor; build `ship.hotkeys` table in `BuildModule` when present; implement `HotkeysRegister` (Lua ref + std::function → `Register`). |
| **Modify** `schema/api.yml` | Bump `api_version` → `0.2.0`; add `hotkey_options` type + `ship.hotkeys.register` function. |
| **Regenerate** `generated/include/shiplua/generated/ApiBindings.h` | Via `python tools/generate_cpp_api.py`. |
| **Regenerate** `generated/lua/shiplua.lua` + `generated/docs/api-reference.md` | Via `python tools/generate_api_docs.py`. |
| **Modify** `tests/unit/LuaApiBindingTests.cpp` | Add tests: hotkey register success, no-op when registry null, callback fires. |
| **Modify** `tests/unit/GeneratedApiBindingsTests.cpp` | Bump expected counts (functions 12→13, version 0.2.0). |
| **Create** `examples/jump/main.lua`, `manifest.toml`, `README.md` | The jump mod. |
| **Modify** `examples/dog-spawner/main.lua`, `manifest.toml` | Migrate to `ship.hotkeys.register`. |
| **Modify** `tests/unit/LuaApiBindingTests.cpp` | Bump `apiRange` to `>=0.2 <0.3` on manifests where needed; existing `>=0.1 <0.2` manifests still load because 0.2 satisfies `>=0.1`? **No** — `<0.2` excludes 0.2. Update test manifests to `>=0.1 <0.3`. |

### MM host `mm/2s2h` (branch `agent/MM-005-mod-directory`)

| File | Responsibility |
|------|----------------|
| **Create** `mm/2s2h/MmHotkeyRegistry.h` / `.cpp` | Concrete `HotkeyRegistry` impl: owns bindings, creates CVars, dispatches scancodes, name↔scancode conversion. |
| **Modify** `mm/2s2h/ShipLuaBootstrap.h` | Replace `DispatchHotkey` decl with `MmHotkeyRegistry* Hotkeys()` accessor; add `LuaJump` isn't here (it's a static in .cpp). |
| **Modify** `mm/2s2h/ShipLuaBootstrap.cpp` | Add `LuaJump`; register `ship.mm.jump` in `InstallMmApi`; inject `MmHotkeyRegistry` into host context; remove `DispatchHotkey`. |
| **Modify** `mm/2s2h/BenPort.cpp` | Replace `DogHotkey` block with `gHotkeyRegistry->DispatchScancode(dwScancode)`. |
| **Modify** `mm/CMakeLists.txt` | Add `mm/2s2h/MmHotkeyRegistry.cpp` to sources (if not globbed). |
| **Create** `mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp` | ImGui panel: per-binding Enabled checkbox + rebind button with live key capture popup. Registered via `RegisterMenuInitFunc`. |
| **Modify** `mm/2s2h/BenGui/BenMenu.cpp` | Add `AddSidebarEntry("Settings", "ShipLua Hotkeys", 1)` in `AddSettings()`. |

---

## Task 1: Core SPI — `HotkeyRegistry` abstract interface + `NullHotkeyRegistry`

**Branch:** `agent/HOTKEY-001-hotkey-api` in the submodule.

**Files:**
- Create: `extern/ship-lua/include/shiplua/input/HotkeyRegistry.h`
- Create: `extern/ship-lua/src/input/HotkeyRegistry.cpp`

**Interfaces:**
- Produces: `ShipLua::HotkeyBinding` (struct), `ShipLua::HotkeyRegistry` (abstract), `ShipLua::NullHotkeyRegistry`. Consumed by Task 3 (LuaApiBinding) and Task 8 (MmHotkeyRegistry).

- [ ] **Step 1: Create the header**

`extern/ship-lua/include/shiplua/input/HotkeyRegistry.h`:
```cpp
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "shiplua/runtime/Logger.h"

namespace ShipLua {

// Metadata for a hotkey registered by a mod. The host converts `defaultKey`
// (a physical key name like "K", "F1") to its native scancode — the core never
// references game keyboard enums, so it stays cross-game.
struct HotkeyBinding {
    std::string id;          // unique within a mod, e.g. "jump"
    std::string modId;       // filled in by the core from LuaRuntime::ModId()
    std::string defaultKey;  // physical key name, e.g. "K"
    std::string label;       // human-readable label for the settings UI
};

// Injectable SPI: the core calls, the host implements. Nullable — a host that
// does not support hotkeys leaves the shared_ptr empty and ship.hotkeys.register
// becomes a graceful no-op (warn + false), so mods never fail to load.
class HotkeyRegistry {
  public:
    virtual ~HotkeyRegistry() = default;

    // Registers (or re-registers, updating the callback) a binding. Returns
    // false if the host rejected it (e.g. invalid key name). Idempotent for a
    // repeated (modId, id) pair.
    virtual bool Register(const HotkeyBinding& binding, std::function<void()> onFire) = 0;

    // Fires the registered callback. No-op if absent.
    virtual void Fire(const std::string& modId, const std::string& id) = 0;

    // Enumerates registered bindings for the settings UI. May be empty.
    virtual std::vector<HotkeyBinding> Registered() const = 0;
};

// Default no-op implementation. Hosts that do not support hotkeys inject this
// (or nullptr) so mods still load.
class NullHotkeyRegistry : public HotkeyRegistry {
  public:
    explicit NullHotkeyRegistry(Logger logger = {});
    bool Register(const HotkeyBinding& binding, std::function<void()> onFire) override;
    void Fire(const std::string& modId, const std::string& id) override;
    std::vector<HotkeyBinding> Registered() const override;

  private:
    Logger mLogger;
};

} // namespace ShipLua
```

- [ ] **Step 2: Create the implementation**

`extern/ship-lua/src/input/HotkeyRegistry.cpp`:
```cpp
#include "shiplua/input/HotkeyRegistry.h"

namespace ShipLua {

NullHotkeyRegistry::NullHotkeyRegistry(Logger logger) : mLogger(std::move(logger)) {}

bool NullHotkeyRegistry::Register(const HotkeyBinding& binding, std::function<void()> onFire) {
    (void)onFire;
    if (mLogger) {
        mLogger(LogLevel::Warn, binding.modId,
                "host sem suporte a hotkeys: registro de '" + binding.id + "' ignorado");
    }
    return false;
}

void NullHotkeyRegistry::Fire(const std::string& modId, const std::string& id) {
    (void)modId;
    (void)id;
}

std::vector<HotkeyBinding> NullHotkeyRegistry::Registered() const {
    return {};
}

} // namespace ShipLua
```

- [ ] **Step 3: Verify it compiles in isolation**

The CMake glob picks up `src/input/HotkeyRegistry.cpp` automatically (no CMake edit needed). Build the `shiplua` target to confirm:
```bash
cd extern/ship-lua && cmake -B build-cmake -S . && cmake --build build-cmake --target shiplua
```
Expected: builds cleanly (the file is standalone; nothing references it yet).

- [ ] **Step 4: Commit**

```bash
cd extern/ship-lua
git checkout -b agent/HOTKEY-001-hotkey-api
git add include/shiplua/input/HotkeyRegistry.h src/input/HotkeyRegistry.cpp
git commit -m "feat(input): HotkeyRegistry SPI + NullHotkeyRegistry

Cross-game abstract interface for mod-registered hotkeys. The core calls
Register/Fire/Registered; the host injects the concrete implementation.
defaultKey is a physical key-name string (e.g. \"K\") so the core never
references game keyboard enums. NullHotkeyRegistry is the graceful no-op
for hosts without hotkey support."
```

---

## Task 2: Bump API/runtime version + extend schema + regenerate bindings

**Files:**
- Modify: `extern/ship-lua/schema/api.yml`
- Regenerate: `extern/ship-lua/generated/include/shiplua/generated/ApiBindings.h`
- Regenerate: `extern/ship-lua/generated/lua/shiplua.lua`
- Regenerate: `extern/ship-lua/generated/docs/api-reference.md`

**Interfaces:**
- Produces: `FunctionId::ShipHotkeysRegister`, the `kShipHotkeysRegisterArguments` array, and `hotkey_options` in the generated header. Consumed by Task 3 (the hand-written binding reads nothing from generated; the generated data is descriptive metadata used by GeneratedApiBindingsTests).

- [ ] **Step 1: Bump `api_version` and add the type + function to `schema/api.yml`**

In `extern/ship-lua/schema/api.yml`:
- Change line 3 `"api_version": "0.1.0",` → `"api_version": "0.2.0",`
- Add a new type entry to the `"types"` array (after `actor_snapshot`):
```json
    {"name": "hotkey_options", "kind": "object", "fields": [
      {"name": "default", "type": "string", "required": false},
      {"name": "label", "type": "string", "required": false}
    ], "description": "Opções de registro de hotkey (tecla default e rótulo)."}
```
- Add a new function entry to the `"functions"` array (after `ship.events.off`):
```json
    {"name": "ship.hotkeys.register", "arguments": [
      {"name": "id", "type": "string"},
      {"name": "options", "type": "hotkey_options", "required": false},
      {"name": "callback", "type": "callback"}
    ], "returns": "boolean", "availability": "common", "capability": null, "errors": ["invalid_argument", "unsupported"]},
```

- [ ] **Step 2: Regenerate the C++ bindings header**

```bash
cd extern/ship-lua
python tools/generate_cpp_api.py
```
Expected output: `Binding C++ gerado: <path>/ApiBindings.h`. Verify the new `ShipHotkeysRegister` enum value and `kShipHotkeysRegisterArguments` array are present:
```bash
grep -n "ShipHotkeysRegister\|kShipHotkeysRegisterArguments\|kApiVersion" generated/include/shiplua/generated/ApiBindings.h
```
Expected: shows `kApiVersion = "0.2.0"`, the new enum value, and the 3-argument array.

- [ ] **Step 3: Regenerate Lua type defs + docs**

```bash
cd extern/ship-lua
python tools/generate_api_docs.py
```
Expected: regenerates `generated/lua/shiplua.lua` and `generated/docs/api-reference.md`.

- [ ] **Step 4: Verify codegen `--check` passes (the CI gate)**

```bash
cd extern/ship-lua
python tools/generate_cpp_api.py --check
python tools/generate_api_docs.py --check
```
Expected: both print "sincronizado" / equivalent and exit 0.

- [ ] **Step 5: Commit**

```bash
cd extern/ship-lua
git add schema/api.yml generated/
git commit -m "feat(api): ship.hotkeys.register in schema (api 0.2.0)

Additive SemVer minor bump. New hotkey_options type and
ship.hotkeys.register(id, options, callback) -> boolean. Regenerated
C++ bindings, Lua type defs, and API docs."
```

---

## Task 3: Core binding — expose `ship.hotkeys.register` to Lua

**Files:**
- Modify: `extern/ship-lua/include/shiplua/api/LuaApiBinding.h`
- Modify: `extern/ship-lua/src/api/LuaApiBinding.cpp`

**Interfaces:**
- Consumes: `ShipLua::HotkeyRegistry` (Task 1), `LuaRuntime::ModId()`.
- Produces: the `ship.hotkeys.register` Lua function (only present when the host context carries a non-null `hotkeys` registry).

- [ ] **Step 1: Extend `LuaApiHostContext` and `LuaApiBinding` declarations**

In `extern/ship-lua/include/shiplua/api/LuaApiBinding.h`:
- Add include after the existing includes:
```cpp
#include "shiplua/input/HotkeyRegistry.h"
```
- Add a field to `LuaApiHostContext` (after `capabilities`):
```cpp
    std::shared_ptr<HotkeyRegistry> hotkeys;  // nullable; null = host without hotkey support
```
- Add a private member to `class LuaApiBinding` (next to `mHostContext`):
```cpp
    std::shared_ptr<HotkeyRegistry> mHotkeys;
```
- Add a private method declaration (next to `EventsOff`):
```cpp
    static int HotkeysRegister(lua_State* state) noexcept;
```

- [ ] **Step 2: Store the registry in the constructor**

In `extern/ship-lua/src/api/LuaApiBinding.cpp`, find the `LuaApiBinding` constructor (the one that initializes `mHostContext`). Add `mHotkeys` initialization. The constructor body sets members from params; add:
```cpp
    mHotkeys(hostContext.hotkeys),
```
to the member initializer list (alongside `mHostContext(hostContext)` etc.). Verify by reading the constructor signature in the .cpp first.

- [ ] **Step 3: Build `ship.hotkeys` in `BuildModule` (conditional)**

In `src/api/LuaApiBinding.cpp`, inside `BuildModule` (after the `ship.log` block, before `lua_pushvalue(state, ship)`):
```cpp
    if (mHotkeys != nullptr) {
        lua_newtable(state);
        SetFunction(state, -1, "register", &LuaApiBinding::HotkeysRegister, this);
        lua_setfield(state, ship, "hotkeys");
    }
```

- [ ] **Step 4: Implement `HotkeysRegister`**

Add to `src/api/LuaApiBinding.cpp` (near `EventsOn`):
```cpp
int LuaApiBinding::HotkeysRegister(lua_State* state) noexcept {
    LuaApiBinding* binding = FromUpvalue(state);
    if (binding == nullptr) {
        return Fail(state, "contexto da API ship indisponível");
    }
    if (binding->mHotkeys == nullptr) {
        return Fail(state, ErrorMessage(ErrorCode::Unsupported));
    }
    if (lua_type(state, 1) != LUA_TSTRING) {
        return Fail(state, "ship.hotkeys.register exige um id textual");
    }
    std::size_t idLen = 0;
    const char* idRaw = lua_tolstring(state, 1, &idLen);
    if (idRaw == nullptr || idLen == 0) {
        return Fail(state, "ship.hotkeys.register exige um id não vazio");
    }
    const std::string id(idRaw, idLen);

    // Optional options table as arg 2 (default/label), callback at arg 3.
    // Or: callback directly as arg 2.
    int callbackIndex = 0;
    std::string defaultKey;
    std::string label;
    if (lua_istable(state, 2) && lua_isfunction(state, 3)) {
        callbackIndex = 3;
        lua_getfield(state, 2, "default");
        if (lua_type(state, -1) == LUA_TSTRING) {
            size_t n = 0;
            const char* s = lua_tolstring(state, -1, &n);
            if (s != nullptr) defaultKey.assign(s, n);
        }
        lua_pop(state, 1);
        lua_getfield(state, 2, "label");
        if (lua_type(state, -1) == LUA_TSTRING) {
            size_t n = 0;
            const char* s = lua_tolstring(state, -1, &n);
            if (s != nullptr) label.assign(s, n);
        }
        lua_pop(state, 1);
    } else if (lua_isfunction(state, 2)) {
        callbackIndex = 2;
    } else {
        return Fail(state, "ship.hotkeys.register exige callback ou opções e callback");
    }

    // Store the Lua callback as a registry ref (mirrors RegisterEvent).
    lua_pushvalue(state, callbackIndex);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    if (reference == LUA_NOREF) {
        return Fail(state, "não foi possível reter o callback de hotkey");
    }

    // Wrap the Lua callback in a std::function that pcall's it safely.
    // Capture shared_ptrs to keep binding + ref alive for the registry's lifetime.
    auto runtime = &binding->mRuntime;
    const std::string modId = binding->mRuntime.ModId();
    auto logger = binding->mLogger;
    std::shared_ptr<HotkeyRegistry> registry = binding->mHotkeys;
    lua_State* capturedState = state;  // owned by the runtime, which outlives the binding

    std::function<void()> onFire = [capturedState, reference, runtime, logger, modId]() {
        lua_State* s = runtime->State();
        if (s == nullptr || reference == LUA_NOREF) {
            return;
        }
        lua_pushcfunction(s, TracebackHandler);
        const int handlerIndex = lua_gettop(s);
        lua_rawgeti(s, LUA_REGISTRYINDEX, reference);
        const int status = lua_pcall(s, 0, 0, handlerIndex);
        if (status != LUA_OK) {
            size_t n = 0;
            const char* msg = lua_tolstring(s, -1, &n);
            std::string failure = msg != nullptr ? std::string(msg, n) : "callback de hotkey falhou";
            lua_pop(s, 2);
            if (logger) {
                logger(LogLevel::Error, modId, failure);
            }
        } else {
            lua_pop(s, 1);
        }
    };

    HotkeyBinding hb;
    hb.id = id;
    hb.modId = modId;
    hb.defaultKey = defaultKey;
    hb.label = label;

    bool registered = false;
    try {
        registered = registry->Register(hb, onFire);
    } catch (...) {
        registered = false;
    }
    lua_pushboolean(state, registered ? 1 : 0);
    return 1;
}
```

**Note:** `TracebackHandler` is an existing static in `LuaApiBinding.cpp` (used by `InvokeCallback`). Verify the exact name by grep before relying on it; if named differently, use that name.

- [ ] **Step 5: Update the `Uninstall` path**

The registry ref created in `HotkeysRegister` is **not tracked in `mCallbacks`** (that map is for event subscriptions). Because the registry callback captures the raw `reference` int, we must release it on unload. Add a parallel `std::vector<int> mHotkeyRefs;` member to `LuaApiBinding`, push `reference` into it after a successful `Register`, and in `Uninstall()` iterate it calling `luaL_unref`. Add the member to the header next to `mCallbacks`, and:
```cpp
// after a successful Register, before pushboolean:
binding->mHotkeyRefs.push_back(reference);
```
And in `Uninstall()` (after the `mCallbacks` loop):
```cpp
    if (state != nullptr) {
        for (int ref : mHotkeyRefs) {
            if (ref != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX, ref);
            }
        }
        mHotkeyRefs.clear();
    }
```

- [ ] **Step 6: Build and commit**

```bash
cd extern/ship-lua
cmake --build build-cmake --target shiplua
```
Expected: compiles. Commit:
```bash
git add include/shiplua/api/LuaApiBinding.h src/api/LuaApiBinding.cpp
git commit -m "feat(api): ship.hotkeys.register Lua binding

Exposed only when LuaApiHostContext.hotkeys is non-null. Stores the
Lua callback as a registry ref and wraps it in a std::function that
pcall's safely (errors logged, never propagated to the host). Refs are
released on Unload. No-op graceful when the host injects no registry."
```

---

## Task 4: Core tests for `ship.hotkeys.register`

**Files:**
- Modify: `extern/ship-lua/tests/unit/LuaApiBindingTests.cpp`

**Interfaces:**
- Consumes: a small in-test `FakeHotkeyRegistry : public ShipLua::HotkeyRegistry` to observe Register/Fire.

- [ ] **Step 1: Write a fake registry + failing tests**

Add near the top of `tests/unit/LuaApiBindingTests.cpp` (after the existing helpers):
```cpp
#include "shiplua/input/HotkeyRegistry.h"

struct FakeHotkey : ShipLua::HotkeyBinding {
    std::function<void()> onFire;
};

struct FakeHotkeyRegistry : ShipLua::HotkeyRegistry {
    bool Register(const ShipLua::HotkeyBinding& b, std::function<void()> onFire) override {
        for (auto& existing : registered) {
            if (existing.modId == b.modId && existing.id == b.id) {
                existing.onFire = std::move(onFire);
                return true;
            }
        }
        FakeHotkey f{b, std::move(onFire)};
        registered.push_back(std::move(f));
        return true;
    }
    void Fire(const std::string& modId, const std::string& id) override {
        for (auto& f : registered) {
            if (f.modId == modId && f.id == id && f.onFire) f.onFire();
        }
    }
    std::vector<ShipLua::HotkeyBinding> Registered() const override {
        std::vector<ShipLua::HotkeyBinding> out;
        for (auto& f : registered) out.push_back(f);
        return out;
    }
    std::vector<FakeHotkey> registered;
};
```

Add a test function (before `main`):
```cpp
void TestHotkeysRegisterFiresCallback() {
    std::vector<CapturedLog> logs;
    auto registry = std::make_shared<FakeHotkeyRegistry>();
    ShipLua::LuaApiHostContext context{"mm", "4.2.0", "0.2.0", {}, registry};
    ShipLua::ModHost host(context, CaptureLogger(logs));

    const std::string source = R"lua(
local ship = require("ship")
local fired = 0
ship.hotkeys.register("jump", {default="K", label="Pulo"}, function()
    fired = fired + 1
end)
assert(ship.hotkeys ~= nil)
)lua";
    Check(host.LoadModFromManifestAndSource(MakeManifest("community.jump"), source).isOk(),
          "jump mod should load");
    Check(registry->registered.size() == 1, "one hotkey registered");
    if (!registry->registered.empty()) {
        Check(registry->registered[0].id == "jump", "id stored");
        Check(registry->registered[0].defaultKey == "K", "defaultKey stored");
        Check(registry->registered[0].label == "Pulo", "label stored");
        registry->Fire("community.jump", "jump");
    }
}

void TestHotkeysNullRegistryIsNoOp() {
    std::vector<CapturedLog> logs;
    ShipLua::LuaApiHostContext context{"mm", "4.2.0", "0.2.0"};  // hotkeys = nullptr
    ShipLua::ModHost host(context, CaptureLogger(logs));

    const std::string source = R"lua(
local ship = require("ship")
assert(ship.hotkeys == nil, "ship.hotkeys must be absent when host injects no registry")
)lua";
    Check(host.LoadModFromManifestAndSource(MakeManifest("community.jump"), source).isOk(),
          "mod loads even without hotkey registry");
}
```

Wire both into `main()`:
```cpp
    TestHotkeysRegisterFiresCallback();
    TestHotkeysNullRegistryIsNoOp();
```

**Also update `MakeManifest`** to use `apiRange = ">=0.1 <0.3"` so the bumped runtime still loads them (line 25):
```cpp
    manifest.apiRange = ">=0.1 <0.3";
```
And update the existing `TestHelloWorldOnBothHosts` `LuaApiHostContext` constructions (line 72) to pass `{}` or `{}` for the new args — actually they use positional init `{game, hostVersion, "0.1.0", {"scene.events"}}`. Since `hotkeys` is now the 5th member, brace init still works (it defaults to nullptr). But the `api_version` string in those payloads (line 83) `"0.1.0"` must become `"0.2.0"` to match the bumped `kApiVersion`. Grep for `"0.1.0"` in this test file and update the api_version payloads.

- [ ] **Step 2: Run the tests (expect pass for impl, and confirm the null case compiles)**

```bash
cd extern/ship-lua
cmake --build build-cmake --target lua_api_binding_tests
./build-cmake/tests/lua_api_binding_tests
```
Expected: all tests pass including the two new ones.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/LuaApiBindingTests.cpp
git commit -m "test(api): ship.hotkeys.register fires callback + null-registry no-op"
```

---

## Task 5: Bump `GeneratedApiBindingsTests` expectations

**Files:**
- Modify: `extern/ship-lua/tests/unit/GeneratedApiBindingsTests.cpp`

- [ ] **Step 1: Find and update the expected counts/version**

```bash
cd extern/ship-lua
grep -n "0.1.0\|kFunctions.size\|size() == 12\|12u\|events.*11\|11u" tests/unit/GeneratedApiBindingsTests.cpp
```
Update: `kApiVersion` expectation `0.1.0` → `0.2.0`; function count 12 → 13. Do not change event count (input.hotkey stays; no events added).

- [ ] **Step 2: Run + commit**

```bash
cmake --build build-cmake --target generated_api_bindings_tests
./build-cmake/tests/generated_api_bindings_tests
git add tests/unit/GeneratedApiBindingsTests.cpp
git commit -m "test(generated): expect api 0.2.0 + 13 functions"
```

---

## Task 6: Create the `jump` example mod

**Files:**
- Create: `extern/ship-lua/examples/jump/main.lua`
- Create: `extern/ship-lua/examples/jump/manifest.toml`
- Create: `extern/ship-lua/examples/jump/README.md`

- [ ] **Step 1: Write the mod Lua**

`extern/ship-lua/examples/jump/main.lua`:
```lua
local ship = require("ship")

ship.events.on("game.ready", function()
    if ship.hotkeys == nil then
        ship.log.warn("host sem suporte a ship.hotkeys — pulo desativado")
        return
    end
    ship.hotkeys.register("jump", { default = "K", label = "Pulo" }, function()
        if ship.mm == nil or ship.mm.jump == nil then
            ship.log.warn("ship.mm.jump indisponivel neste host")
            return
        end
        ship.mm.jump()
    end)
    ship.log.info("Jump pronto — aperte K (rebindável em Settings → ShipLua Hotkeys)")
end)
```

- [ ] **Step 2: Write the manifest**

`extern/ship-lua/examples/jump/manifest.toml`:
```toml
id = "community.jump"
name = "Jump"
version = "0.1.0"
api = ">=0.2 <0.3"
entrypoint = "main.lua"
description = "Aperte K (rebindável em Settings → ShipLua Hotkeys) para fazer o Link pular."
authors = ["BaiterYamato"]
games = ["mm"]
```

- [ ] **Step 3: Write the README**

`extern/ship-lua/examples/jump/README.md`:
```markdown
# Jump

Aperta **K** (rebindável) para fazer o Link pular no 2 Ship Hypes (Majora's Mask).

## Comportamento

- Pulso vertical aplicado a `actor.velocity.y`.
- Só atua com o Link no chão (`BGCHECKFLAG_GROUND`). Sem multi-jump no ar.
- Funciona em qualquer forma (humano, Deku, Zora, Goron) — usa impulso direto,
  sem animação dedicada, então pode parecer mais simples que um pulo nativo.

## Como usar

1. Copie a pasta `jump/` para `<pasta-do-2ship>/mods/`.
2. Inicie o jogo; entre em uma cena (ex.: Clock Town).
3. Aperte **K** para pular.
4. Para trocar a tecla: **Settings → ShipLua Hotkeys → Pulo** → clique e pressione a tecla desejada.

## Requer

- Host com suporte a `ship.hotkeys` (injeta `HotkeyRegistry`) e `ship.mm.jump`.
- API ShipLua `>=0.2 <0.3`.
```

- [ ] **Step 4: Commit**

```bash
cd extern/ship-lua
git add examples/jump/
git commit -m "feat(examples): jump mod (ship.hotkeys + ship.mm.jump)"
```

---

## Task 7: Migrate `dog-spawner` to `ship.hotkeys`

**Files:**
- Modify: `extern/ship-lua/examples/dog-spawner/main.lua`
- Modify: `extern/ship-lua/examples/dog-spawner/manifest.toml`

- [ ] **Step 1: Rewrite `main.lua`**

`extern/ship-lua/examples/dog-spawner/main.lua`:
```lua
local ship = require("ship")

ship.events.on("game.ready", function()
    if ship.hotkeys == nil then
        ship.log.warn("host sem suporte a ship.hotkeys — dog spawner desativado")
        return
    end
    ship.hotkeys.register("spawn_dog", { default = "F", label = "Spawn Dog" }, function()
        if ship.mm == nil or ship.mm.spawn_dog == nil then
            ship.log.warn("ship.mm.spawn_dog indisponivel neste host")
            return
        end
        if ship.mm.spawn_dog() then
            ship.log.info("Au au! cachorro spawnado")
        else
            ship.log.warn("nao deu pra spawnar aqui — tente estando em jogo na Clock Town")
        end
    end)
    ship.log.info("Dog Spawner pronto — aperte F (rebindável) na Clock Town")
end)
```

- [ ] **Step 2: Bump manifest api range**

`extern/ship-lua/examples/dog-spawner/manifest.toml`: change `api = ">=0.1 <0.2"` → `api = ">=0.2 <0.3"`. (Verify current value by reading the file first.)

- [ ] **Step 3: Commit**

```bash
cd extern/ship-lua
git add examples/dog-spawner/
git commit -m "feat(examples): migrate dog-spawner to ship.hotkeys.register"
```

---

## Task 8: Submodule full test run + push

- [ ] **Step 1: Configure + build all tests**

```bash
cd extern/ship-lua
cmake -B build-cmake -S .
cmake --build build-cmake
```
Expected: builds.

- [ ] **Step 2: Run CTest**

```bash
cd extern/ship-lua
ctest --test-dir build-cmake --output-on-failure
```
Expected: all green, including `api_cpp_codegen_check`, `api_docs_codegen_check`, `lua_api_binding_tests`, `generated_api_bindings_tests`, `hello_world_conformance_tests`.

- [ ] **Step 3: Push the branch**

```bash
cd extern/ship-lua
git push -u origin agent/HOTKEY-001-hotkey-api
```
Expected: pushed. Record the new HEAD SHA.

---

## Task 9: MM host — bump submodule pointer

**Branch:** `agent/MM-005-mod-directory` in the MM repo.

- [ ] **Step 1: Update the submodule to the new branch HEAD**

```bash
cd /d/Desenvolvimento/ship-lua-worktrees/MM-003
git submodule update --remote --checkout extern/ship-lua  # if tracking branch
```
If the submodule isn't tracking the branch, checkout explicitly:
```bash
cd extern/ship-lua
git fetch origin
git checkout agent/HOTKEY-001-hotkey-api
git pull
cd ..
git add extern/ship-lua
```

- [ ] **Step 2: Commit the pointer bump**

```bash
cd /d/Desenvolvimento/ship-lua-worktrees/MM-003
git add extern/ship-lua
git commit -m "chore(mm): bump ship-lua to agent/HOTKEY-001-hotkey-api (ship.hotkeys API)"
```

---

## Task 10: MM host — `MmHotkeyRegistry`

**Files:**
- Create: `mm/2s2h/MmHotkeyRegistry.h`
- Create: `mm/2s2h/MmHotkeyRegistry.cpp`

**Interfaces:**
- Consumes: `ShipLua::HotkeyRegistry` (submodule Task 1), `KbScancode`, CVar bridge, `Window::GetKeyName`.
- Produces: `MmHotkeyRegistry` global `gHotkeyRegistry`; `DispatchScancode(int32_t)`; `Bindings()`; static `ParseKeyName`/`KeyName`.

- [ ] **Step 1: Write the header**

`mm/2s2h/MmHotkeyRegistry.h`:
```cpp
#pragma once

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
        std::string scancodeCvar;  // gShipLua.Hotkey.<modId>.<id>.Scancode
        std::string enabledCvar;   // gShipLua.Hotkey.<modId>.<id>.Enabled
        int32_t defaultScancode = 0;
    };

    bool Register(const ShipLua::HotkeyBinding& binding, std::function<void()> onFire) override;
    void Fire(const std::string& modId, const std::string& id) override;
    std::vector<ShipLua::HotkeyBinding> Registered() const override;

    // Called from Graph_StartFrame each keydown.
    void DispatchScancode(int32_t scancode);

    const std::vector<Binding>& Bindings() const { return mBindings; }

    // Physical key name <-> scancode. ParseKeyName falls back to LUS_KB_K on
    // unknown names (safe non-zero default; the mod can rebind).
    static int32_t ParseKeyName(const std::string& name);
    static std::string KeyName(int32_t scancode);

  private:
    static std::string ScancodeCvar(const std::string& modId, const std::string& id);
    static std::string EnabledCvar(const std::string& modId, const std::string& id);

    std::vector<Binding> mBindings;
};

extern MmHotkeyRegistry* gHotkeyRegistry;

} // namespace ShipLuaHost
```

- [ ] **Step 2: Write the implementation**

`mm/2s2h/MmHotkeyRegistry.cpp`:
```cpp
#include "MmHotkeyRegistry.h"

#include <algorithm>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h>
#include <spdlog/spdlog.h>

#include "variables.h"

namespace ShipLuaHost {

MmHotkeyRegistry* gHotkeyRegistry = nullptr;

namespace {

struct KeyNameEntry {
    const char* name;
    Ship::KbScancode scancode;
};

// Physical key names a mod may declare as `default`. Covers common keys; the
// user can rebind to anything in the UI.
const KeyNameEntry kKeyNames[] = {
    {"A", Ship::KbScancode::LUS_KB_A}, {"B", Ship::KbScancode::LUS_KB_B},
    {"C", Ship::KbScancode::LUS_KB_C}, {"D", Ship::KbScancode::LUS_KB_D},
    {"E", Ship::KbScancode::LUS_KB_E}, {"F", Ship::KbScancode::LUS_KB_F},
    {"G", Ship::KbScancode::LUS_KB_G}, {"H", Ship::KbScancode::LUS_KB_H},
    {"I", Ship::KbScancode::LUS_KB_I}, {"J", Ship::KbScancode::LUS_KB_J},
    {"K", Ship::KbScancode::LUS_KB_K}, {"L", Ship::KbScancode::LUS_KB_L},
    {"M", Ship::KbScancode::LUS_KB_M}, {"N", Ship::KbScancode::LUS_KB_N},
    {"O", Ship::KbScancode::LUS_KB_O}, {"P", Ship::KbScancode::LUS_KB_P},
    {"Q", Ship::KbScancode::LUS_KB_Q}, {"R", Ship::KbScancode::LUS_KB_R},
    {"S", Ship::KbScancode::LUS_KB_S}, {"T", Ship::KbScancode::LUS_KB_T},
    {"U", Ship::KbScancode::LUS_KB_U}, {"V", Ship::KbScancode::LUS_KB_V},
    {"W", Ship::KbScancode::LUS_KB_W}, {"X", Ship::KbScancode::LUS_KB_X},
    {"Y", Ship::KbScancode::LUS_KB_Y}, {"Z", Ship::KbScancode::LUS_KB_Z},
    {"F1", Ship::KbScancode::LUS_KB_F1}, {"F2", Ship::KbScancode::LUS_KB_F2},
    {"F3", Ship::KbScancode::LUS_KB_F3}, {"F4", Ship::KbScancode::LUS_KB_F4},
    {"F5", Ship::KbScancode::LUS_KB_F5}, {"F6", Ship::KbScancode::LUS_KB_F6},
    {"F7", Ship::KbScancode::LUS_KB_F7}, {"F8", Ship::KbScancode::LUS_KB_F8},
    {"F9", Ship::KbScancode::LUS_KB_F9}, {"F10", Ship::KbScancode::LUS_KB_F10},
    {"F11", Ship::KbScancode::LUS_KB_F11}, {"F12", Ship::KbScancode::LUS_KB_F12},
    {"Space", Ship::KbScancode::LUS_KB_SPACE}, {"Enter", Ship::KbScancode::LUS_KB_ENTER},
    {"Tab", Ship::KbScancode::LUS_KB_TAB}, {"Shift", Ship::KbScancode::LUS_KB_SHIFT},
    {"Control", Ship::KbScancode::LUS_KB_CONTROL}, {"Alt", Ship::KbScancode::LUS_KB_ALT},
    {"Up", Ship::KbScancode::LUS_KB_ARROWKEY_UP},
    {"Down", Ship::KbScancode::LUS_KB_ARROWKEY_DOWN},
    {"Left", Ship::KbScancode::LUS_KB_ARROWKEY_LEFT},
    {"Right", Ship::KbScancode::LUS_KB_ARROWKEY_RIGHT},
};

} // namespace

std::string MmHotkeyRegistry::ScancodeCvar(const std::string& modId, const std::string& id) {
    return "gShipLua.Hotkey." + modId + "." + id + ".Scancode";
}

std::string MmHotkeyRegistry::EnabledCvar(const std::string& modId, const std::string& id) {
    return "gShipLua.Hotkey." + modId + "." + id + ".Enabled";
}

int32_t MmHotkeyRegistry::ParseKeyName(const std::string& name) {
    for (const auto& entry : kKeyNames) {
        if (name == entry.name) {
            return static_cast<int32_t>(entry.scancode);
        }
    }
    SPDLOG_WARN("ShipLua hotkey: nome de tecla desconhecido '{}', usando K como padrão", name);
    return static_cast<int32_t>(Ship::KbScancode::LUS_KB_K);
}

std::string MmHotkeyRegistry::KeyName(int32_t scancode) {
    for (const auto& entry : kKeyNames) {
        if (static_cast<int32_t>(entry.scancode) == scancode) {
            return entry.name;
        }
    }
    return "K";  // fallback label
}

bool MmHotkeyRegistry::Register(const ShipLua::HotkeyBinding& binding, std::function<void()> onFire) {
    for (auto& existing : mBindings) {
        if (existing.meta.modId == binding.modId && existing.meta.id == binding.id) {
            existing.meta = binding;
            existing.onFire = std::move(onFire);
            return true;
        }
    }
    Binding b;
    b.meta = binding;
    b.onFire = std::move(onFire);
    b.scancodeCvar = ScancodeCvar(binding.modId, binding.id);
    b.enabledCvar = EnabledCvar(binding.modId, binding.id);
    b.defaultScancode = ParseKeyName(binding.defaultKey);
    mBindings.push_back(std::move(b));
    SPDLOG_INFO("ShipLua hotkey registrado: {}.{} (default={}/{})", binding.modId, binding.id,
                binding.defaultKey, b.defaultScancode);
    return true;
}

void MmHotkeyRegistry::Fire(const std::string& modId, const std::string& id) {
    for (auto& b : mBindings) {
        if (b.meta.modId == modId && b.meta.id == id && b.onFire) {
            b.onFire();
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
    for (auto& b : mBindings) {
        if (!b.onFire) continue;
        if (CVarGetInteger(b.enabledCvar.c_str(), 1) == 0) continue;
        if (scancode == CVarGetInteger(b.scancodeCvar.c_str(), b.defaultScancode)) {
            b.onFire();
        }
    }
}

} // namespace ShipLuaHost
```

- [ ] **Step 3: Add to CMake sources + commit**

Check how `mm/2s2h/*.cpp` are listed:
```bash
cd /d/Desenvolvimento/ship-lua-worktrees/MM-003
grep -n "ShipLuaBootstrap\|BenPort\|file(GLOB" mm/CMakeLists.txt | head
```
If globbed, no edit. If explicit, add `mm/2s2h/MmHotkeyRegistry.cpp`. Build the `2ship` target to confirm it links:
```bash
cmake --build build-cmake --target 2ship2harkinian 2>&1 | tail -20
```
(Use the actual target name; verify with `cmake --build build-cmake --target help | grep -i ship`.)

Commit:
```bash
git add mm/2s2h/MmHotkeyRegistry.h mm/2s2h/MmHotkeyRegistry.cpp mm/CMakeLists.txt
git commit -m "feat(shiplua): MmHotkeyRegistry concrete HotkeyRegistry impl"
```

---

## Task 11: MM host — `ship.mm.jump` binding + inject registry + remove `DispatchHotkey`

**Files:**
- Modify: `mm/2s2h/ShipLuaBootstrap.h`
- Modify: `mm/2s2h/ShipLuaBootstrap.cpp`

- [ ] **Step 1: Update the header**

In `mm/2s2h/ShipLuaBootstrap.h`:
- Remove the `void DispatchHotkey(const std::string& action);` declaration.
- Add:
```cpp
namespace ShipLuaHost {

class MmHotkeyRegistry;

void Initialize();
void Shutdown();
ShipLua::ModHost* GetModHost();

// Owned hotkey registry (created in Initialize, destroyed in Shutdown).
MmHotkeyRegistry* Hotkeys();

} // namespace ShipLuaHost
```
(Keep the existing forward decl of `ShipLua::ModHost`.)

- [ ] **Step 2: Add `LuaJump` and inject the registry in the .cpp**

In `mm/2s2h/ShipLuaBootstrap.cpp`:
- Add include: `#include "MmHotkeyRegistry.h"`.
- Add a file-scope impulse constant after the includes (calibration anchor):
```cpp
namespace {
constexpr float kJumpImpulseDefault = 6.0f;  // playtest-tunable vertical velocity
}
```
- Add `LuaJump` next to `LuaSpawnDog`:
```cpp
// ship.mm.jump([height]) -> bool.
// Applies an upward impulse if the player is on the ground. height is an
// optional vertical-velocity override; defaults to kJumpImpulseDefault.
int LuaJump(lua_State* L) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) == 0) {
        lua_pushboolean(L, 0);  // airborne: no double-jump
        return 1;
    }
    const float impulse = (float)luaL_optnumber(L, 1, kJumpImpulseDefault);
    player->actor.velocity.y = impulse;
    SPDLOG_INFO("ShipLua jump: impulse {:.1f} em pos=({:.0f},{:.0f},{:.0f})", impulse,
                player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z);
    lua_pushboolean(L, 1);
    return 1;
}
```
- Register it in `InstallMmApi` (next to `spawn_dog`):
```cpp
    lua_newtable(L);
    lua_pushcfunction(L, LuaSpawnDog);
    lua_setfield(L, -2, "spawn_dog");
    lua_pushcfunction(L, LuaJump);
    lua_setfield(L, -2, "jump");
    lua_setfield(L, -2, "mm");
```
- Add a global registry instance and wire `Hotkeys()`:
```cpp
std::unique_ptr<MmHotkeyRegistry> gHotkeyRegistryOwner;

MmHotkeyRegistry* Hotkeys() { return gHotkeyRegistryOwner.get(); }
```
(Define `MmHotkeyRegistry* gHotkeyRegistry = nullptr;` is owned inside the .cpp of `MmHotkeyRegistry.cpp`; here expose it via `Hotkeys()`. Choose ONE canonical global. Simplest: keep `gHotkeyRegistry` raw pointer defined in `MmHotkeyRegistry.cpp` and set it from `ShipLuaHost::Initialize`. Add `extern MmHotkeyRegistry* gHotkeyRegistry;` already declared in `MmHotkeyRegistry.h`.)

- In `Initialize()`, before `LoadModsAndDispatchReady`:
```cpp
    gHotkeyRegistryOwner = std::make_unique<MmHotkeyRegistry>();
    gHotkeyRegistry = gHotkeyRegistryOwner.get();
```
- In `CreateHostContext()`, inject it:
```cpp
    context.hotkeys = std::shared_ptr<ShipLua::HotkeyRegistry>(
        gHotkeyRegistry, [](ShipLua::HotkeyRegistry*) {});  // non-owning alias
```
  (Shared_ptr with no-op deleter because `MmHotkeyRegistry` is owned by the host, not the context. This avoids double-free.)
- In `Shutdown()`:
```cpp
    gHotkeyRegistry = nullptr;
    gHotkeyRegistryOwner.reset();
```
- **Remove** the `DispatchHotkey` function entirely (both the definition and the payload block).

- [ ] **Step 3: Build + commit**

```bash
cmake --build build-cmake --target 2ship2harkinian
```
Expected: builds. Commit:
```bash
git add mm/2s2h/ShipLuaBootstrap.h mm/2s2h/ShipLuaBootstrap.cpp
git commit -m "feat(shiplua): ship.mm.jump + inject MmHotkeyRegistry

LuaJump applies a grounded-only vertical impulse to player->actor.velocity
(BGCHECKFLAG_GROUND gate, height optional). Initialize creates the
MmHotkeyRegistry and injects it into the LuaApiHostContext so mods see
ship.hotkeys. Removes DispatchHotkey (replaced by DispatchScancode)."
```

---

## Task 12: MM host — `Graph_StartFrame` dispatch

**Files:**
- Modify: `mm/2s2h/BenPort.cpp`

- [ ] **Step 1: Replace the dog-hotkey block**

In `mm/2s2h/BenPort.cpp`, find the block (~L1158-1162):
```cpp
    // ShipLua: configurable hotkey (default F) asking loaded mods to spawn a dog.
    // Rebind via CVar gShipLua.DogHotkey.Scancode; disable via .Enabled.
    if (dwScancode > 0 && CVarGetInteger("gShipLua.DogHotkey.Enabled", 1) != 0 &&
        dwScancode == CVarGetInteger("gShipLua.DogHotkey.Scancode", KbScancode::LUS_KB_F)) {
        ShipLuaHost::DispatchHotkey("spawn_dog");
    }
```
Replace with:
```cpp
    // ShipLua: dispatch the keydown to every hotkey registered by loaded mods.
    if (dwScancode > 0 && ShipLuaHost::gHotkeyRegistry != nullptr) {
        ShipLuaHost::gHotkeyRegistry->DispatchScancode(dwScancode);
    }
```
Ensure the include for `MmHotkeyRegistry.h` is present at the top of `BenPort.cpp` (near the existing `ShipLuaBootstrap.h` include at ~L67).

- [ ] **Step 2: Build + commit**

```bash
cmake --build build-cmake --target 2ship2harkinian
git add mm/2s2h/BenPort.cpp
git commit -m "feat(shiplua): dispatch keydowns via MmHotkeyRegistry

Replaces the hardcoded DogHotkey block; any hotkey a mod registered now fires
through the single DispatchScancode path."
```

---

## Task 13: MM host — Settings UI panel

**Files:**
- Create: `mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp`
- Modify: `mm/2s2h/BenGui/BenMenu.cpp` (add sidebar)

- [ ] **Step 1: Add the sidebar in `BenMenu::AddSettings()`**

In `mm/2s2h/BenGui/BenMenu.cpp`, inside `AddSettings()`, after the last existing sidebar block, add (find the `path` local and reuse it):
```cpp
    AddSidebarEntry("Settings", "ShipLua Hotkeys", 1);
    path.sidebarName = "ShipLua Hotkeys";
    path.column = SECTION_COLUMN_1;
    AddWidget(path, "ShipLua Hotkeys", WIDGET_SEPARATOR_TEXT);
```
(Verify `SECTION_COLUMN_1`, `WIDGET_SEPARATOR_TEXT`, and `WidgetPath` names by grepping `MenuTypes.h` / existing usage; use the repo's actual constants.)

- [ ] **Step 2: Write the panel file**

`mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp`:
```cpp
#include "BenGui/BenMenu.h"
#include "BenGui/MenuTypes.h"
#include "MmHotkeyRegistry.h"
#include "UIWidgets.hpp"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h>
#include <spdlog/spdlog.h>

extern std::shared_ptr<BenGui::BenMenu> mBenMenu;

namespace {

// Per-row capture state for the live key-capture popup.
struct CaptureState {
    bool active = false;
    std::string modId;
    std::string id;
};
CaptureState gCapture;

void DrawHotkeyRow(const ShipLuaHost::MmHotkeyRegistry::Binding& b) {
    ImGui::TableNextColumn();
    const bool enabled = CVarGetInteger(b.enabledCvar.c_str(), 1) != 0;
    bool newEnabled = enabled;
    if (ImGui::Checkbox(b.meta.label.empty() ? b.meta.id.c_str() : b.meta.label.c_str(), &newEnabled)) {
        CVarSetInteger(b.enabledCvar.c_str(), newEnabled ? 1 : 0);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::TableNextColumn();
    const int32_t current = CVarGetInteger(b.scancodeCvar.c_str(), b.defaultScancode);
    const std::string label = ShipLuaHost::MmHotkeyRegistry::KeyName(current) + " (click to rebind)";
    if (ImGui::Button(label.c_str())) {
        gCapture = {true, b.meta.modId, b.meta.id};
        ImGui::OpenPopup("ShipLuaHotkeyCapture");
    }

    if (ImGui::BeginPopup("ShipLuaHotkeyCapture")) {
        ImGui::Text("Pressione uma tecla... (Esc cancela)");
        const int32_t sc = Ship::Context::GetInstance()->GetWindow()->GetLastScancode();
        if (sc > 0) {
            if (sc == Ship::KbScancode::LUS_KB_ESCAPE) {
                Ship::Context::GetInstance()->GetWindow()->SetLastScancode(-1);
            } else {
                CVarSetInteger(b.scancodeCvar.c_str(), sc);
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                Ship::Context::GetInstance()->GetWindow()->SetLastScancode(-1);  // consume
                SPDLOG_INFO("ShipLua hotkey {}.{} rebindada para scancode {}", b.meta.modId, b.meta.id, sc);
            }
            ImGui::CloseCurrentPopup();
            gCapture = {};
        }
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
            gCapture = {};
        }
        ImGui::EndPopup();
    }
}

void DrawShipLuaHotkeysPanel() {
    if (ShipLuaHost::gHotkeyRegistry == nullptr) {
        return;
    }
    const auto& bindings = ShipLuaHost::gHotkeyRegistry->Bindings();
    if (bindings.empty()) {
        ImGui::TextDisabled("Nenhum hotkey registrado por mods ainda.");
        return;
    }
    if (ImGui::BeginTable("ShipLuaHotkeys", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ação");
        ImGui::TableSetupColumn("Tecla");
        ImGui::TableHeadersRow();
        for (const auto& b : bindings) {
            DrawHotkeyRow(b);
        }
        ImGui::EndTable();
    }
}

} // namespace

namespace BenGui {

void RegisterShipLuaHotkeyPanel() {
    // The sidebar is created in BenMenu::AddSettings(); here we register a
    // WIDGET_CUSTOM that draws the dynamic binding list into that sidebar.
    WidgetPath path = {"Settings", "ShipLua Hotkeys", SECTION_COLUMN_1};
    auto& w = mBenMenu->AddWidget(path, "##ShipLuaHotkeyList", WIDGET_CUSTOM);
    w.customFunction = [](WidgetInfo& info) {
        (void)info;
        DrawShipLuaHotkeysPanel();
    };
}

} // namespace BenGui

static RegisterMenuInitFunc sRegister(BenGui::RegisterShipLuaHotkeyPanel);
```

**Verify these names exist** before relying on them (grep the repo): `WidgetPath`, `SECTION_COLUMN_1`, `WIDGET_CUSTOM`, `WidgetInfo::customFunction`, `RegisterMenuInitFunc`, `mBenMenu` extern, `AddWidget` return type, `UIWidgets` namespace vs raw ImGui. The 2ship repo uses raw ImGui in places (UIWidgets wraps some); raw ImGui calls are fine in a custom widget. Adjust to actual API as discovered.

- [ ] **Step 3: Add to CMake + build + commit**

Add `mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp` to sources (globbed or explicit). Build:
```bash
cmake --build build-cmake --target 2ship2harkinian
```
Commit:
```bash
git add mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp mm/2s2h/BenGui/BenMenu.cpp mm/CMakeLists.txt
git commit -m "feat(shiplua): Settings → ShipLua Hotkeys panel (live rebind)"
```

---

## Task 14: Build the full 2ship target + smoke-test checklist

- [ ] **Step 1: Full build**

```bash
cd /d/Desenvolvimento/ship-lua-worktrees/MM-003
cmake --build build-cmake --target 2ship2harkinian 2>&1 | tail -30
```
Expected: links cleanly.

- [ ] **Step 2: Smoke-test checklist (manual, since no ROM automation)**

Since this touches gameplay, the final validation is in-game. Record results:
1. Copy `extern/ship-lua/examples/jump/` to the 2ship app `mods/` directory.
2. Launch the game with a MM ROM.
3. Enter a scene (Clock Town). Confirm the console/spdlog shows:
   `ShipLua hotkey registrado: community.jump.jump (default=K/37)`.
4. Press **K** → Link jumps. Press K again mid-air → no double jump.
5. Open Settings → ShipLua Hotkeys → "Pulo" → click the button → press **Space**.
6. Press **Space** → Link jumps (new binding persisted).
7. Toggle the Enabled checkbox off → key no longer jumps.

- [ ] **Step 3: Final commits if any calibration changes**

If `kJumpImpulseDefault = 6.0f` feels wrong, adjust and amend. Commit:
```bash
git add mm/2s2h/ShipLuaBootstrap.cpp
git commit -m "fix(shiplua): tune jump impulse to <value>"
```

---

## Self-Review (completed by planner)

**Spec coverage:** Every spec section maps to a task. Core SPI (Task 1), schema/version (Task 2), Lua binding (Task 3), core tests (Tasks 4-5), jump example (Task 6), dog migration (Task 7), submodule CI (Task 8), pointer bump (Task 9), MmHotkeyRegistry (Task 10), jump+inject (Task 11), dispatch (Task 12), UI (Task 13), validation (Task 14). The "alturas ajustáveis" requirement → `LuaJump` height param (Task 11). Rebindable-in-game → Task 13. Default K → mod string + ParseKeyName (Tasks 6, 10).

**Placeholder scan:** The UI panel (Task 13) has the highest uncertainty because exact 2ship ImGui widget API names must be verified at implementation time — steps instruct to grep first and adjust. This is flagged, not hidden.

**Type consistency:** `HotkeyBinding` fields (id/modId/defaultKey/label) are consistent across header (Task 1), Lua binding (Task 3), tests (Task 4), and `MmHotkeyRegistry::Binding` (Task 10). `DispatchScancode(int32_t)` matches between Task 10 (def) and Task 12 (call). `gHotkeyRegistry` global declared in `MmHotkeyRegistry.h` (Task 10), set in `Initialize` (Task 11), read in BenPort (Task 12) and panel (Task 13).
