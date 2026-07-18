# MM-CI-001 — Diagnóstico de CI do fork 2ship2harkinian + rascunho de workflows

Data: 2026-07-16 (verificado nesta data). Escopo: apenas leitura no GitHub; nenhum push/merge/close.

## 1. O que existe no fork BaiterYamato/2ship2harkinian

As branches `develop` e `lua/main` apontam para o mesmo commit (`b3cc366`,
"Implement skeleton key (#1776)", 2026-07-11) e contêm `.github/workflows/`:

| Arquivo | Gatilho | Conteúdo |
|---|---|---|
| `main.yml` (`generate-builds`) | `push`, `pull_request` | jobs: `generate-2ship-otr` (ubuntu-22.04), `build-macos` (macos-14), `build-linux` (ubuntu-22.04, gcc-12), `build-windows` (windows-latest, MSVC+Ninja+sccache); ccache, cache de `deps/` e `vcpkg/`, cpack + upload de artefatos |
| `clang-format.yml` | `pull_request` | job `clang-format`: ubuntu-latest, clang-format-14, `./run-clang-format.sh` + `git diff --exit-code` |
| `pr-artifacts.yml` | `workflow_run` de `generate-builds` | publica seção de artefatos no PR via nightly.link |
| `apt-deps.txt` | — | deps apt do build Linux |

Os workflows vieram do upstream HarbourMasters no fork e são anteriores aos
PRs (commit de 2026-07-11; PR #1 aberto em 2026-07-13).

## 2. Por que os PRs #1–#9 não têm checks

- API: `GET /repos/BaiterYamato/2ship2harkinian/actions/runs` → `total_count: 0`
  (nenhum workflow jamais rodou no fork).
- API: check runs do PR #1 → `total_count: 0`.
- Os gatilhos `pull_request` estão corretos e a branch base do PR #1 (`lua/main`)
  já continha os workflows quando o PR foi aberto.

Conclusão: o GitHub Actions está desabilitado no fork ( forks novos vêm com
Actions desligado até o dono clicar em "enable" na aba Actions, ou workflows
foram desabilitados individualmente). Não é problema de YAML: nenhum arquivo
novo resolve; é configuração do repositório (aba Actions / Settings → Actions).

Detalhe da pilha: PR #1 tem base `lua/main`; PRs #2–#9 têm bases `agent/**`
empilhadas; PR #10 é rascunho com base `agent/MM-WORLD-002-assets`. Quando o
Actions for habilitado, o gatilho `pull_request` usa o workflow da branch base
de cada PR — todas as bases `agent/MM-*` derivam de `lua/main` e já carregam
`.github/workflows/`, então os checks aparecem para a pilha inteira.

## 3. Referência: Shipwright-HyliaFoundry (fork OoT, Actions HABILITADO)

Workflows em `lua/main`: `generate-builds.yml`, `clang-format.yml`,
`pr-artifacts.yml`, `test-builds-on-distros.yml`, `apt-deps.txt`,
`macports-deps.txt`. Checks reais observados no PR #5 (run 29226741551/29226741605):

- `generate-soh-otr` — success
- `build-macos` — success
- `build-linux` — failure
- `build-windows` — failure
- `clang-format` — failure

Ou seja: no fork OoT o Actions roda (CI vermelho por falhas reais de
build/formatação), confirmando que a diferença do fork MM é só o Actions
desligado.

## 4. Rascunho criado nesta branch (`agent/MM-CI-001-host-ci`)

`.github/workflows/mm-pr-ci.yml` — validação de PR adaptada ao 2ship2harkinian:

- `clang-format` — idêntico ao do upstream (clang-format-14 + run-clang-format.sh);
- `build-linux` — ubuntu-22.04, gcc-12, apt-deps.txt, SDL 2.30.3 / tinyxml2 10.0.0 /
  libzip 1.10.1 compilados em `deps/` com cache, Ninja Release, alvo `2ship`,
  SEM geração de OTR, SEM cpack e SEM artefatos (feedback rápido);
- `build-windows` — windows-latest, Ninja + MSVC (`ilammy/msvc-dev-cmd`), sccache,
  cache de `vcpkg/`, alvo `2ship`, sem empacotamento;
- `shiplua-core-tests` — extra vs. upstream: se `extern/ship-lua` existir no
  merge do PR (pilha MM-001+), configura o CMake standalone do núcleo e roda
  `ctest -C Release --output-on-failure`; na `lua/main` pura o job passa sem rodar.

Submódulos: `actions/checkout` com `submodules: true` cobre `libultraship`,
`OTRExporter`, `ZAPDTR` e `extern/ship-lua` (todos HTTPS públicos).

## 5. O que falta (ações do dono do fork, fora do alcance deste agente)

1. Habilitar o GitHub Actions em BaiterYamato/2ship2harkinian (aba Actions →
   "I understand my workflows, go ahead and enable them").
2. Decidir se `mm-pr-ci.yml` entra como workflow adicional ou se basta re-rodar
   os workflows herdados (re-run em cada PR atualiza os checks).
3. Após habilitar, esperar falhas reais nos PRs da pilha (o fork OoT mostra
   build-linux/build-windows/clang-format vermelhos — a pilha MM provavelmente
   terá problemas semelhantes para corrigir).
