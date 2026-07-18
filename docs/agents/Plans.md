# Plans Ledger

This file is the append-only execution ledger for ShipLua integration work in
the 2Ship2Harkinian host.

## Rules

1. Append a plan before implementation starts.
2. Never rewrite or delete previous entries.
3. Record progress as a new `[UPDATE]` entry.

## [PLN-20260718-0001] Generic MM actor provider and release integration

- createdUtc: 2026-07-18T20:23:37Z
- status: in_progress
- scope: engine
- summary: Implement the ShipLua 0.4 generic actor provider in the MM host and prove parity with the existing OoT provider before packaging the first functional dual-host release.
- milestones:
  1. pin the reviewed ShipLua actor contract in `extern/ship-lua`
  2. implement an allowlisted MM actor provider with safe handles and ownership
  3. inject the provider into the MM ShipLua bootstrap and add ROM-free tests
  4. compile the MM host on Windows and run the integrated test suite
  5. open the host PR and hand off cross-host runtime smoke and packaging
- tags: shiplua, mm, actor, provider, release-0.2
- refs:
  - C:/Users/leolo/Downloads/plan(sdk).md
  - coordination/claims/MM-MODSDK-001.md
  - mm/2s2h/ShipLuaBootstrap.cpp
  - mm/2s2h/MmActorProvider.cpp
  - mm/tests/MmActorProviderTests.cpp

## [PLN-20260718-0001][UPDATE] 2026-07-18T20:50:00Z

- status: in_progress
- note: Provider MM, bootstrap, CMake e teste ROM-free implementados; `2ship.exe` Release gerado e 57/57 testes aplicáveis passaram. Aguardando checkpoint Git, PR e smoke real com assets do usuário.
- refs:
  - mm/2s2h/MmActorProvider.h
  - mm/2s2h/MmActorProvider.cpp
  - mm/2s2h/ShipLuaBootstrap.cpp
  - mm/tests/MmActorProviderTests.cpp
  - docs/SHIPLUA_MM_ACTOR_PROVIDER.md
  - coordination/handoffs/MM-MODSDK-001.md
