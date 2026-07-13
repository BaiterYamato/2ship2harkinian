# Mod de Pulo (Jump) + API `ship.hotkeys` — Design

**Data:** 2026-07-13
**Branch MM:** `agent/MM-005-mod-directory`
**Branch submódulo (ship-lua):** `feature/hotkey-api` (a criar no fork `BaiterYamato/ship-lua`)
**Status:** Aprovado (brainstorm), aguardando revisão do spec

## Objetivo

Um mod que, ao apertar a tecla configurável (default **K**), faz o Link pular
no 2 Ship Hypes (Majora's Mask). A tecla deve ser **rebindável dentro do jogo**,
no menu de Settings. A infraestrutura de hotkeys deve morar no **submódulo
ship-lua** (cross-game, API pública) — não amarrada ao host MM — para que mods
futuros reutilizem a mesma API em qualquer host.

## Requisitos

1. **Comportamento do pulo**: impulso vertical aplicado **somente quando o Link
   estiver no chão** (`actor.bgCheckFlags & 1`). Sem multi-jump no ar.
2. **Altura ajustável**: `ship.mm.jump(height)` aceita um parâmetro opcional
   para mods ajustarem a intensidade do pulo.
3. **Hotkey default K, rebindável em jogo**: painel "ShipLua Hotkeys" nas
   Settings, com captura de tecla ao vivo (clica no binding → pressiona a tecla).
4. **API `ship.hotkeys` no core**: o mod declara seu próprio hotkey via Lua;
   o host injeta a implementação. Coerente com a filosofia cross-game do
   ship-lua.

## Arquitetura

Três camadas, respeitando a regra fundamental: **o core (`extern/ship-lua`)
nunca inclui headers de jogo**. A ponte core↔host é uma interface abstrata
(SPI) injetada no `LuaApiHostContext`.

```
┌─────────────────────────────────────────────────────────┐
│ MOD LUA (jump/main.lua)                                  │
│   ship.hotkeys.register("jump", {default="K",label=…},  │
│     function() ship.mm.jump() end)                       │
└───────────────┬─────────────────────────────────────────┘
                │ Lua C function (upvalue → LuaApiBinding)
                ▼
┌─────────────────────────────────────────────────────────┐
│ CORE (extern/ship-lua) — cross-game, SEM headers de jogo │
│  • HotkeyRegistry = interface ABSTRATA (header puro)     │
│  • LuaApiBinding expõe ship.hotkeys.register             │
│  • LuaApiHostContext.carrega HotkeyRegistry* (nullable)  │
│  • Guarda callback Lua (registry ref) + std::function    │
└───────────────┬─────────────────────────────────────────┘
                │ chamadas virtuais puras (Register/Fire)
                ▼
┌─────────────────────────────────────────────────────────┐
│ HOST MM (mm/2s2h) — específico do jogo                   │
│  • MmHotkeyRegistry : public ShipLua::HotkeyRegistry     │
│      - Register(): cria CVar gShipLua.Hotkey.<modId>.<id>│
│      - Fire(id): invoca o callback guardado pelo core    │
│  • Graph_StartFrame: itera registry, compara scancode    │
│  • Painel ImGui "ShipLua Hotkeys" (rebind ao vivo)       │
│  • ship.mm.jump(height): novo binding (muta velocity.y)  │
└─────────────────────────────────────────────────────────┘
```

### Por que SPI injetável (e não `ship.hotkeys` direto no host)

- O core depende apenas de uma interface abstrata (header puro: `<string>`,
  `<functional>`, `<memory>`). Nunca referencia `KbScancode`, `CVarGetInteger`
  ou structs do jogo.
- Qualquer host futuro (OOT, Switch) implementa `HotkeyRegistry` do seu jeito e
  a API Lua é idêntica — mods são portáteis.
- Se o host **não** injetar o registry (host legacy / sem suporte),
  `ship.hotkeys.register` vira **no-op gracoso com warn** — o mod carrega e
  roda, só sem o hotkey. Mods nunca quebram por falta de suporte.

## Fluxo de dados — apertar K até o Link pular

1. Usuário aperta **K** → `Fast3dWindow::KeyDown` →
   `Window::SetLastScancode(LUS_KB_K)`.
2. `Graph_StartFrame` lê `GetLastScancode()`, reseta para `-1`.
3. `gHotkeyRegistry->DispatchScancode(scancode)` itera os bindings registrados;
   para cada um lê `CVarGetInteger("gShipLua.Hotkey.<modId>.<id>.Scancode",
   default)` e `.Enabled`.
4. Match + habilitado → `Fire(modId, id)` → o core faz **protected call** no
   callback Lua guardado.
5. Callback Lua chama `ship.mm.jump()` → `LuaJump` (C++) →
   `player->actor.velocity.y = impulso` (somente se `bgCheckFlags & 1`).
6. Física do jogo aplica gravidade no frame seguinte → Link pula.

## Componentes

### 1. Core (`extern/ship-lua`) — novos e modificações

#### Novo: `include/shiplua/input/HotkeyRegistry.h`

Header puro (sem dependências de jogo). Define:

- `struct HotkeyBinding { std::string id; std::string modId; std::string defaultKey; std::string label; }`
  - `id` — identificador do hotkey (único por mod; ex.: `"jump"`).
  - `modId` — preenchido pelo core a partir do `LuaRuntime::ModId()`.
  - `defaultKey` — nome físico da tecla como string (ex.: `"K"`, `"F1"`). O host
    converte para o scancode nativo. **O core nunca referencia enums de teclado
    do jogo** — fica portátil.
  - `label` — rótulo legível para a UI de Settings.
- `class HotkeyRegistry` (interface abstrata, todas virtuais puras):
  - `virtual bool Register(const HotkeyBinding& binding, std::function<void()> onFire) = 0;`
    Idempotente para o mesmo `(modId, id)` — re-registro atualiza o callback.
  - `virtual void Fire(const std::string& modId, const std::string& id) = 0;`
    Dispara o callback. No-op se ausente.
  - `virtual std::vector<HotkeyBinding> Registered() const = 0;`
    Enumera bindings para a UI iterar.
- `class NullHotkeyRegistry : public HotkeyRegistry` — implementação padrão
  no-op. `Register` loga warn e retorna `false`; `Fire` é no-op;
  `Registered()` retorna `{}`. Hosts sem suporte a hotkeys injetam esta (ou
  nullptr) — mods nunca quebram.

#### Modificação: `include/shiplua/api/LuaApiBinding.h`

- `LuaApiHostContext` ganha `std::shared_ptr<HotkeyRegistry> hotkeys;`
  (nullable; padrão `nullptr` = host sem suporte).
- `LuaApiBinding` guarda `std::shared_ptr<HotkeyRegistry> mHotkeys;` (copiado
  do contexto no construtor).
- Novo método privado: `static int HotkeysRegister(lua_State* state) noexcept;`.

#### Modificação: `src/api/LuaApiBinding.cpp`

- `BuildModule` adiciona o submódulo `ship.hotkeys` **somente se
  `mHotkeys != nullptr`**. Se for null, `ship.hotkeys` não existe — e o mod
  deve guardá-lo com nil-check (como já faz com `ship.mm`).
- `HotkeysRegister` implementa `ship.hotkeys.register(id, options, callback)`:
  - `id` (string, obrigatório), `options` (table, opcional: `{default=...,
    label=...}`), `callback` (function, obrigatório).
  - Valida argumentos; empacota o callback Lua num `std::function<void()>` que
    faz `lua_rawgeti` no registry reference + `lua_pcall` via `LuaRuntime`,
    reaproveitando a infraestrutura de protected-call existente
    (`InvokeCallback` / `LuaCallback`).
  - Chama `mHotkeys->Register(binding, onFire)`. Retorna `boolean` (sucesso).
  - Erros: `unsupported` (host sem `HotkeyRegistry`), `invalid_argument`
    (id vazio / callback ausente).

#### Modificação: `schema/api.yml`

Adiciona:

```json
{"name": "ship.hotkeys.register",
 "arguments": [
   {"name": "id", "type": "string"},
   {"name": "options", "type": "hotkey_options", "required": false},
   {"name": "callback", "type": "callback"}
 ],
 "returns": "boolean", "availability": "common", "capability": null,
 "errors": ["invalid_argument", "unsupported"]}
```

Novo tipo `hotkey_options` (object): `{default: string, label: string}` (ambos
opcionais).

#### Regeneração: `generated/`

Codegen regenera `generated/include/shiplua/generated/ApiBindings.h` e
`generated/lua/shiplua.lua` (type defs) a partir do schema atualizado.

#### Versão

`api_version`: `0.1.0` → **`0.2.0`** (aditivo, SemVer minor). `runtime_version`
também bumped. Exemplos atualizam `api = ">=0.2 <0.3"`.

### 2. Host MM (`mm/2s2h`) — novos e modificações

#### Novo: `mm/2s2h/MmHotkeyRegistry.{h,cpp}`

Implementação concreta do SPI:

```cpp
class MmHotkeyRegistry : public ShipLua::HotkeyRegistry {
  public:
    bool Register(const ShipLua::HotkeyBinding& b, std::function<void()> onFire) override;
    void Fire(const std::string& modId, const std::string& id) override;
    std::vector<ShipLua::HotkeyBinding> Registered() const override;

    // Chamado por Graph_StartFrame a cada keydown.
    void DispatchScancode(int32_t scancode);

    struct RegisteredBinding { ShipLua::HotkeyBinding meta; std::function<void()> onFire; };
    const std::vector<RegisteredBinding>& Bindings() const;

    // Conversão nome↔scancode (só no host — único lugar que inclui KeyboardScancodes.h).
    static int32_t ParseKeyName(const std::string& name);  // "K" → LUS_KB_K
    static std::string KeyName(int32_t scancode);          // via Window::GetKeyName
  private:
    std::vector<RegisteredBinding> mBindings;
};
```

- `Register`: cria os CVars `gShipLua.Hotkey.<modId>.<id>.Scancode`
  (default = `ParseKeyName(defaultKey)`) e `.Enabled` (default 1), guarda
  `onFire`. Idempotente: re-registro do mesmo `(modId, id)` atualiza callback
  (não duplica).
- `DispatchScancode`: itera `mBindings`; compara
  `scancode == CVarGetInteger(cvarScancode, default) &&
   CVarGetInteger(cvarEnabled, 1)` → chama `onFire()`.
- `ParseKeyName`/`KeyName` moram **somente no host** (único lugar que pode
  incluir `<ship/.../KeyboardScancodes.h>`). Tabela nome→scancode cobre pelo
  menos letras A–Z, dígitos 0–9, F1–F12, Space, Enter, Tab, Shift, Ctrl, Alt,
  setas.

#### Modificação: `mm/2s2h/ShipLuaBootstrap.cpp`

- Nova função `int LuaJump(lua_State* L)` (próximo a `LuaSpawnDog`):
  ```cpp
  // ship.mm.jump([height]) → bool.
  // Aplica impulso vertical se o player estiver no chão (bgCheckFlags & 1).
  // height opcional (default = kJumpImpulseDefault). Retorna false se airborne
  // ou fora de gameplay.
  int LuaJump(lua_State* L) {
      PlayState* play = gPlayState;
      if (play == nullptr) { lua_pushboolean(L, 0); return 1; }
      Player* player = GET_PLAYER(play);
      if (player == nullptr) { lua_pushboolean(L, 0); return 1; }
      if ((player->actor.bgCheckFlags & 1) == 0) { lua_pushboolean(L, 0); return 1; }
      const float height = luaL_optnumber(L, 1, kJumpImpulseDefault);
      player->actor.velocity.y = height;
      lua_pushboolean(L, 1);
      return 1;
  }
  ```
  Registrado em `InstallMmApi` como `ship.mm.jump` (novo `lua_pushcfunction`/
  `lua_setfield`).
- `CreateHostContext` injeta `context.hotkeys = gHotkeyRegistry` (instância
  global de `MmHotkeyRegistry`, criada em `Initialize()`).
- **Remove** `DispatchHotkey`/`ShipLuaHost::DispatchHotkey` (declarado em
  `ShipLuaBootstrap.h`, definido em `.cpp`) e o campo público correspondente —
  substituído por `gHotkeyRegistry->DispatchScancode`.

#### Modificação: `mm/2s2h/BenPort.cpp`

Em `Graph_StartFrame`, substitui o bloco `DogHotkey` (~L1158-1162) por:

```cpp
// ShipLua: despacha keydown para todos os hotkeys registrados por mods.
if (dwScancode > 0 && gHotkeyRegistry != nullptr) {
    gHotkeyRegistry->DispatchScancode(dwScancode);
}
```

#### Novo: `mm/2s2h/BenGui/ShipLuaHotkeysPanel.cpp`

Registra o painel "ShipLua Hotkeys" em Settings via `RegisterMenuInitFunc`:

- Cria sidebar `"ShipLua Hotkeys"` em Settings
  (`AddSidebarEntry("Settings", "ShipLua Hotkeys", 1)`).
- Para cada binding do `gHotkeyRegistry->Bindings()`:
  - `WIDGET_CVAR_CHECKBOX` para `.Enabled` (CVar
    `gShipLua.Hotkey.<modId>.<id>.Enabled`).
  - `WIDGET_CUSTOM` com um botão de rebind que mostra o nome atual da tecla
    (`MmHotkeyRegistry::KeyName(CVarGetInteger(scancodeCvar, default))`) e, ao
    clicar, abre um popup ImGui "Pressione uma tecla..." que lê
    `Window::GetLastScancode()` a cada frame, escreve o CVar e consome o
    scancode. Padrão de captura inspirado no InputEditorWindow do libultraship.
- Os widgets são **dinâmicos**: como os bindings são registrados pelos mods em
  tempo de carga, o painel é redesenhado a partir de `Bindings()` (que reflete
  o estado atual do `gHotkeyRegistry`). Se um mod for carregado depois do
  `InitElement` do menu, o painel aparece na próxima abertura das Settings.

### 3. Mod Lua (`extern/ship-lua/examples/jump/`)

#### `examples/jump/main.lua`

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
        if ship.mm.jump() then
            ship.log.debug("pulou")
        end
    end)
    ship.log.info("Jump pronto — aperte K (rebindável nas Settings → ShipLua Hotkeys)")
end)
```

#### `examples/jump/manifest.toml`

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

#### `examples/jump/README.md`

Documenta: comportamento (só no chão, altura ajustável via `ship.mm.jump(h)`),
como rebindar (Settings → ShipLua Hotkeys), e a dependência de host (precisa de
host que injete `HotkeyRegistry`).

### 4. Migração do dog-spawner para `ship.hotkeys`

`examples/dog-spawner/main.lua` migra de `ship.events.on("input.hotkey", ...)`
para `ship.hotkeys.register("spawn_dog", {default="F", label="Spawn Dog"}, ...)`.
Mantém o mesmo `ship.mm.spawn_dog()` — só muda o mecanismo de disparo. O
`manifest.toml` atualiza `api = ">=0.2 <0.3"`.

## Decisões de compatibilidade

| # | Decisão | Justificativa |
|---|---------|---------------|
| 1 | `api_version` `0.1.0` → `0.2.0` (additivo, SemVer minor) | Nova função é adição; mods que usam `ship.hotkeys` declaram `api = ">=0.2 <0.3"` (não rodam em 0.1, onde a API não existe) |
| 2 | Evento `input.hotkey` permanece no schema (não removido) | Backward-compat: hosts/dispatchers legados ainda funcionam; a nova API é o caminho preferencial, não o único |
| 3 | Bloco `DogHotkey` em BenPort.cpp é removido; `dog-spawner` migra para `ship.hotkeys` | Adicionado hoje (commit 05d6850, experimental); manter os dois criaria dois caminhos conflitantes para o mesmo conceito |
| 4 | `ship.hotkeys.register` é no-op gracoso se `HotkeyRegistry` for null | Mod carrega em qualquer host; só loga warn |

## Calibração e edge cases

- **Impulso do pulo**: `kJumpImpulseDefault` precisa de playtest (provável faixa
  5–7 unidades de `velocity.y`, dado `gravity` negativo). Deixar como constante
  nomeada no header para ajuste fácil. Confirmar o bit exato de "on ground" em
  `actor.bgCheckFlags` consultando o decomp (bit 0 é o canônico).
- **Formas (Zora/Deku/Goron)**: o pulo por impulso direto de `velocity.y`
  funciona em todas as formas (não depende de animação de jump do player).
  Pode parecer estranho em algumas formas — aceitável para v1, documentado no
  README.
- **Rebind conflitante**: se dois mods registrarem o mesmo `id` (ex.: `"jump"`),
  o `modId` os diferencia (`gShipLua.Hotkey.<modId>.<id>`). Se dois bindings
  diferentes mapearem a mesma tecla, **ambos disparam** — documentado, sem
  resolução automática em v1.
- **Persistência**: CVars são persistidos pelo `SaveConsoleVariablesNextFrame()`
  ao rebindar (já é o padrão dos widgets de Settings do 2ship).
- **Thread-safety**: os bindings são populados durante a carga dos mods (thread
  principal, em `ShipLuaHost::Initialize`) e lidos por `DispatchScancode` (thread
  principal, em `Graph_StartFrame`). Sem concorrência em v1.

## Repos e branches

- **`BaiterYamato/ship-lua`** (submódulo `extern/ship-lua`) — branch nova
  `feature/hotkey-api`. Aqui: core SPI + binding + schema + codegen + exemplo
  jump + migração dog-spawner.
- **Host MM** (`D:\Desenvolvimento\ship-lua-worktrees\MM-003`) — branch atual
  `agent/MM-005-mod-directory`. Aqui: `MmHotkeyRegistry`, `LuaJump`, troca em
  BenPort, UI panel. Após merge do submódulo, bump do pointer.

## Sequência de build (ordem de implementação)

1. **Submódulo** (`feature/hotkey-api`):
   a. `HotkeyRegistry.h` + `NullHotkeyRegistry` (+ `src/input/HotkeyRegistry.cpp`
      para a impl no-op).
   b. Estender `LuaApiBinding` (contexto + `HotkeysRegister` + `BuildModule`).
   c. Atualizar `schema/api.yml`; regenerar `generated/`.
   d. Exemplo `examples/jump/`; migrar `examples/dog-spawner/`.
   e. Testes unitários (análogos a `LuaApiBindingTests.cpp`).
   f. Bump `api_version`/`runtime_version`; commit; push.
2. **Host MM** (na worktree atual):
   a. Bump do pointer do submódulo para `feature/hotkey-api`.
   b. `MmHotkeyRegistry.{h,cpp}`; injetar em `ShipLuaHost::Initialize`.
   c. `LuaJump`; registrar em `InstallMmApi`.
   d. Trocar bloco BenPort por `DispatchScancode`; remover `DispatchHotkey`.
   e. `ShipLuaHotkeysPanel.cpp` (UI).
   f. Build + smoke test em jogo.
3. **Validação**: carregar o mod jump em `<appdir>/mods/jump/`, apertar K,
   confirmar pulo; abrir Settings → ShipLua Hotkeys, rebindar para outra tecla,
   confirmar novo binding funcionando.
