#pragma once

// Puppet cross-world do lado MM: desenha NPCs esqueléticos do OOT usando
// assets montados em runtime do oot.o2r da instalação vizinha (../OOT).
// Primeiro consumidor: Rauru (En_Rl / object_rl). Ver plan-sdk §26.

namespace ShipLuaHost {

// Converte o ator host recém-spawnado num puppet do Rauru (esqueleto +
// animação de espera do OOT). Main-thread only.
bool ShipLuaOotPuppet_AttachRauru(void* actor, void* play);

// Libera o estado quando o ator morre ou a cena é destruída.
void ShipLuaOotPuppet_HandleActorDestroy(void* actor);
void ShipLuaOotPuppet_Reset();

} // namespace ShipLuaHost
