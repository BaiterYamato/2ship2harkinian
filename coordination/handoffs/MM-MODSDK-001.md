# Handoff — MM-MODSDK-001

## Estado

review

- Commit: `1d2eb2c`
- Pull request: https://github.com/BaiterYamato/2ship2harkinian/pull/12

## Resultado

- `MmActorProvider` implementa o contrato comum `ShipLua::ActorProvider`.
- `mm.en_dg` traduz para `ACTOR_EN_DG` apenas dentro do host.
- `OBJECT_DOG` precisa estar carregado e `ACTOR_PLAYER` permanece bloqueado.
- Handles têm ownership, limite, proteção ABA e invalidação por cena.
- O bootstrap registra capabilities, injeta o provider no contexto Lua e
  conecta cleanup a `OnActorDestroy`/`OnPlayDestroy`.
- O submódulo `extern/ship-lua` aponta para `55d0c63` (`MODSDK-005`).

## Validação executada

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target mm_actor_provider_tests -- /m:1
ctest --test-dir build -C Release -R mm_actor_provider_tests --output-on-failure
cmake --build build --config Release --target 2ship -- /m:1
cmake --build build --config Release -- /m:1
ctest --test-dir build -C Release -E '^prism$' --output-on-failure
git diff --check
```

## Resultado da validação

- MSVC 19.44 / Visual Studio 2022 / x64 Release.
- `x64/Release/2ship.exe` gerado com 23.429.120 bytes.
- SHA-256: `A5028B80105A4C746242FCDA5B65D6E38A192075555D807856F69BCB501977D6`.
- 57/57 testes aplicáveis passaram, incluindo o contrato do provider e o
  exemplo `actor_spawn_example_mm`.
- `prism` permanece fora do gate porque o upstream o registra sem produzir o
  executável nessa configuração.

## Riscos e pendências

- O smoke real ainda depende dos assets legítimos do usuário.
- `mm.en_dg` usa path 0/índice de South Clock Town (`0x03E0`); fora de cenas
  com o objeto/path adequado, o provider retorna erro ou o jogo pode remover o
  ator conforme sua lógica vanilla.
- A PR #12 foi publicada como draft, empilhada sobre
  `agent/MM-LINK-003-runtime`, e aguarda os checks/review.
- O release final ainda precisa reunir launcher + hosts OoT/MM sem incluir ROM,
  O2R, saves ou logs.

## Próxima ação recomendada

Concluir os PRs de contrato/providers e executar o mesmo mod `actor-spawn` nos
dois executáveis antes de empacotar `v0.2.0-alpha.1`.
