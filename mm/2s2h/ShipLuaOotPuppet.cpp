#include "ShipLuaOotPuppet.h"

#include <map>
#include <memory>

#include <spdlog/spdlog.h>

extern "C" {
#include "align_asset_macro.h"
#include "functions.h"
#include "gfx_setupdl.h"
#include "macros.h"
#include "variables.h"
#include "z64.h"

extern PlayState* gPlayState;
// Declarado em BenPort.h, cujo include-guard colide com outro header.
uint8_t ResourceMgr_FileExists(const char* resName);
}

namespace ShipLuaHost {
namespace {

// Assets do OOT lidos em runtime do oot.o2r vizinho, resolvidos pelo alias de
// hash do OotCrossWorldArchive (object_rl não colide com nada do MM).
static const ALIGN_ASSET(2) char sRauruSkel[] = "__OTR__objects/object_rl/object_rl_Skel_007B38";
static const ALIGN_ASSET(2) char sRauruWaitAnim[] = "__OTR__objects/object_rl/object_rl_Anim_000A3C";

constexpr float kRauruScale = 0.01f;

struct PuppetState {
    SkelAnime skelAnime{};
    bool ready = false;
};

std::map<Actor*, std::unique_ptr<PuppetState>> sPuppets;

PuppetState* FindPuppet(Actor* actor) {
    const auto it = sPuppets.find(actor);
    return it == sPuppets.end() ? nullptr : it->second.get();
}

} // namespace
} // namespace ShipLuaHost

// Callbacks com ABI C (ponteiros de função do engine).
extern "C" {

static void ShipLuaOotPuppetUpdate(Actor* actor, PlayState* play) {
    ShipLuaHost::PuppetState* state = ShipLuaHost::FindPuppet(actor);
    if (state == nullptr || !state->ready) {
        return;
    }
    SkelAnime_Update(&state->skelAnime);
    actor->focus.pos = actor->world.pos;
    actor->focus.pos.y += 60.0f;
}

static void ShipLuaOotPuppetDraw(Actor* actor, PlayState* play) {
    ShipLuaHost::PuppetState* state = ShipLuaHost::FindPuppet(actor);
    if (state == nullptr || !state->ready) {
        return;
    }
    Gfx_SetupDL25_Opa(play->state.gfxCtx);
    SkelAnime_DrawFlexOpa(play, state->skelAnime.skeleton, state->skelAnime.jointTable, state->skelAnime.dListCount,
                          nullptr, nullptr, actor);
}

} // extern "C"

namespace ShipLuaHost {

bool ShipLuaOotPuppet_AttachRauru(void* actorPtr, void* playPtr) {
    Actor* actor = static_cast<Actor*>(actorPtr);
    PlayState* play = static_cast<PlayState*>(playPtr);
    if (actor == nullptr || play == nullptr) {
        return false;
    }

    // SkelAnime_InitFlex desreferencia o recurso: sem o oot.o2r vizinho
    // montado, o ponteiro não resolve e o engine quebra. Confira ANTES.
    if (!ResourceMgr_FileExists(sRauruSkel) || !ResourceMgr_FileExists(sRauruWaitAnim)) {
        SPDLOG_WARN("ShipLua: assets do Rauru indisponíveis — o oot.o2r do OoT não está montado "
                    "(coloque as duas instalações lado a lado ou use SHIPLUA_OOT_ROOT)");
        return false;
    }

    auto state = std::make_unique<PuppetState>();
    SkelAnime_InitFlex(play, &state->skelAnime, (FlexSkeletonHeader*)sRauruSkel, (AnimationHeader*)sRauruWaitAnim,
                       nullptr, nullptr, 0);
    if (state->skelAnime.skeleton == nullptr) {
        SPDLOG_WARN("ShipLua não conseguiu carregar o esqueleto do Rauru (oot.o2r montado?)");
        return false;
    }
    state->ready = true;

    Actor_SetScale(actor, kRauruScale);
    actor->update = ShipLuaOotPuppetUpdate;
    actor->draw = ShipLuaOotPuppetDraw;

    sPuppets[actor] = std::move(state);
    SPDLOG_INFO("ShipLua puppet do Rauru (OOT) anexado ao ator {}", static_cast<void*>(actor));
    return true;
}

void ShipLuaOotPuppet_HandleActorDestroy(void* actor) {
    sPuppets.erase(static_cast<Actor*>(actor));
}

void ShipLuaOotPuppet_Reset() {
    sPuppets.clear();
}

} // namespace ShipLuaHost
