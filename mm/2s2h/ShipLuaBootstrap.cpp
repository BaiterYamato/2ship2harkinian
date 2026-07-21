#include "ShipLuaBootstrap.h"
#include "MmActorProvider.h"
#include "MmHotkeyRegistry.h"
#include "MmWorldAdapter.h"
#include "ShipLuaOotPuppet.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <ship/resource/File.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/resource/archive/O2rArchive.h>
#include <shiplua/generated/ApiBindings.h>
#include <shiplua/host/ModHost.h>
#include <shiplua/runtime/LuaRuntime.h>
#include <shiplua/storage/AtomicFile.h>
#include <shiplua/world/WorldHandoff.h>

#include "GameInteractor/GameInteractor.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "build.h"

// Privileged adapter code — allowed to touch MM internals to implement the
// host-specific ship.mm.* bindings. (The shared core never includes game headers.)
#include "align_asset_macro.h"
#include "variables.h"
#include "z64.h"

extern "C" {
// Tabelas de display list da mão esquerda do player (z_player_lib.c) e o
// helper de existência de recurso declarado em BenPort.h (guard colidente).
extern Gfx* gPlayerLeftHandOneHandSwordDLs[];
extern Gfx* D_801C018C[];
uint8_t ResourceMgr_FileExists(const char* resName);
}

namespace ShipLuaHost {
namespace {

std::unique_ptr<ShipLua::ModHost> gModHost;
std::shared_ptr<MmActorProvider> gActorProvider;
std::shared_ptr<ShipLua::CapabilityRegistry> gCapabilityRegistry;
std::shared_ptr<MmHotkeyRegistry> gHotkeys;
std::shared_ptr<MmWorldAdapter> gWorldAdapter;
HOOK_ID gSaveLoadHook = 0;
HOOK_ID gImportTickHook = 0;
std::optional<ShipLua::WorldHandoff> gPendingHandoff;
HOOK_ID gActorDestroyHook = 0;
HOOK_ID gPlayDestroyHook = 0;

constexpr int kSwitchWorldExitCode = 73;

struct BridgeConfig {
    std::filesystem::path sessionDirectory;
    std::filesystem::path handoffPath;
    std::array<std::byte, 16> sessionId{};
    std::array<std::byte, 32> authenticationKey{};
    std::uint64_t sequence = 0;
};

int HexDigit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

template <std::size_t Size>
bool ParseHex(const char* text, std::array<std::byte, Size>& output) {
    if (text == nullptr || std::char_traits<char>::length(text) != Size * 2) {
        return false;
    }
    for (std::size_t index = 0; index < Size; ++index) {
        const int high = HexDigit(text[index * 2]);
        const int low = HexDigit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

std::optional<BridgeConfig> GetBridgeConfig() {
    const char* sessionDirectory = std::getenv("LINKSPAN_SESSION_DIR");
    const char* handoffPath = std::getenv("LINKSPAN_HANDOFF_PATH");
    const char* sequence = std::getenv("LINKSPAN_SEQUENCE");
    if (sessionDirectory == nullptr || handoffPath == nullptr || sequence == nullptr) {
        return std::nullopt;
    }
    BridgeConfig config;
    config.sessionDirectory = sessionDirectory;
    config.handoffPath = handoffPath;
    if (!ParseHex(std::getenv("LINKSPAN_SESSION_ID"), config.sessionId) ||
        !ParseHex(std::getenv("LINKSPAN_AUTH_KEY"), config.authenticationKey)) {
        return std::nullopt;
    }
    char* end = nullptr;
    config.sequence = std::strtoull(sequence, &end, 10);
    if (end == sequence || *end != '\0' || config.sequence == 0) {
        return std::nullopt;
    }
    return config;
}

bool BothGamesAvailable() {
    const char* available = std::getenv("LINKSPAN_AVAILABLE_GAMES");
    if (available == nullptr) {
        return false;
    }
    const std::string games(available);
    return games.find("oot") != std::string::npos && games.find("mm") != std::string::npos;
}

[[noreturn]] void ExitForWorldSwitch() {
#ifdef _WIN32
    // RequestWorldTravel runs inside a Lua callback. std::exit executes CRT
    // teardown while that callback stack is still active and can trigger the
    // Windows fail-fast code 0xC0000409 before the launcher receives code 73.
    ::ExitProcess(static_cast<UINT>(kSwitchWorldExitCode));
#else
    std::_Exit(kSwitchWorldExitCode);
#endif
}

ShipLua::Result<void> RequestWorldTravel(const ShipLua::WorldDestination& destination) {
    const auto config = GetBridgeConfig();
    if (!config.has_value() || !BothGamesAvailable() || destination.world != ShipLua::WorldId::Oot ||
        gWorldAdapter == nullptr) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::Unsupported,
                                          "ponte Link-Span para OoT indisponível");
    }
    const auto player = gWorldAdapter->CapturePlayerState();
    if (!player.isOk()) {
        return ShipLua::Result<void>::err(player.code, player.message);
    }
    ShipLua::WorldHandoff handoff;
    handoff.sessionId = config->sessionId;
    handoff.sequence = config->sequence;
    handoff.source = ShipLua::WorldId::Mm;
    handoff.destination = destination;
    handoff.player = *player.value;
    const auto written = ShipLua::WorldHandoffCodec::WriteFile(
        config->handoffPath, handoff, config->authenticationKey);
    if (!written.isOk()) {
        return written;
    }
    const auto requested = ShipLua::AtomicFile::Write(config->sessionDirectory / "next-world", "oot\n");
    if (!requested.isOk()) {
        std::error_code ignored;
        std::filesystem::remove(config->handoffPath, ignored);
        return requested;
    }
    SPDLOG_INFO("Link-Span exportou o estado MM e solicitou troca para OoT ({})", destination.id);
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
    ExitForWorldSwitch();
}

void TryConsumeWorldHandoff() {
    const auto config = GetBridgeConfig();
    if (!config.has_value() || gWorldAdapter == nullptr ||
        !std::filesystem::is_regular_file(config->handoffPath)) {
        return;
    }
    const auto handoff = ShipLua::WorldHandoffCodec::ReadFile(
        config->handoffPath, config->authenticationKey);
    if (!handoff.isOk()) {
        SPDLOG_ERROR("Link-Span rejeitou o handoff MM: {}", handoff.message);
        return;
    }
    if (handoff.value->sessionId != config->sessionId || handoff.value->sequence + 1 != config->sequence ||
        handoff.value->destination.world != ShipLua::WorldId::Mm) {
        SPDLOG_ERROR("Link-Span rejeitou um handoff destinado a outra sessão ou jogo");
        return;
    }
    // O handoff NÃO é aplicado aqui: OnSaveLoad também dispara na abertura do
    // jogo (z_opening.c), quando não há arquivo carregado — escrever save e
    // forçar entrance nesse ponto corrompe o estado e derruba o jogo. Guarda
    // o handoff (e o arquivo em disco) até um frame realmente jogável.
    gPendingHandoff = *handoff.value;
    SPDLOG_INFO("Link-Span recebeu um handoff para MM ({}) — aguardando um save carregado",
                handoff.value->destination.id);
}

// Só importa quando existe um jogo de verdade rodando: PlayState ativo, save
// com vida (arquivo carregado, não a intro) e nenhuma transição em curso.
bool WorldImportIsSafe() {
    if (gPlayState == nullptr) {
        return false;
    }
    if (gSaveContext.save.saveInfo.playerData.healthCapacity <= 0) {
        return false;
    }
    return gPlayState->transitionTrigger == TRANS_TRIGGER_OFF;
}

void TickWorldImport() {
    if (!gPendingHandoff.has_value() || gWorldAdapter == nullptr || !WorldImportIsSafe()) {
        return;
    }
    const auto config = GetBridgeConfig();
    if (!config.has_value()) {
        gPendingHandoff.reset();
        return;
    }

    const ShipLua::WorldHandoff handoff = *gPendingHandoff;
    const auto prepared = gWorldAdapter->PrepareImport(handoff.player, handoff.destination);
    if (!prepared.isOk()) {
        gPendingHandoff.reset();
        SPDLOG_ERROR("Link-Span não preparou a importação MM: {}", prepared.message);
        return;
    }
    const auto committed = gWorldAdapter->CommitImport();
    if (!committed.isOk()) {
        gWorldAdapter->AbortImport();
        gPendingHandoff.reset();
        SPDLOG_ERROR("Link-Span não confirmou a importação MM: {}", committed.message);
        return;
    }
    // Só agora o handoff pode sumir do disco: a viagem foi mesmo aplicada.
    gPendingHandoff.reset();
    std::error_code error;
    std::filesystem::remove(config->handoffPath, error);
    SPDLOG_INFO("Link-Span importou o estado compartilhado em MM ({})", handoff.destination.id);
}

#ifdef _WIN32
std::filesystem::path RuntimeRoot() {
    if (const wchar_t* configured = _wgetenv(L"LINKSPAN_ROOT"); configured != nullptr && *configured != L'\0') {
        return configured;
    }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return std::filesystem::current_path();
    }
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::wstring QuotePowerShellLiteral(std::wstring value) {
    std::size_t position = 0;
    while ((position = value.find(L'\'', position)) != std::wstring::npos) {
        value.insert(position, 1, L'\'');
        position += 2;
    }
    return L"'" + value + L"'";
}
#endif

ShipLua::Logger CreateLogger() {
    return ShipLua::Logger([](ShipLua::LogLevel level, const std::string& modId, const std::string& message) {
        switch (level) {
            case ShipLua::LogLevel::Debug:
                SPDLOG_DEBUG("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Info:
                SPDLOG_INFO("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Warn:
                SPDLOG_WARN("ShipLua [{}]: {}", modId, message);
                break;
            case ShipLua::LogLevel::Error:
                SPDLOG_ERROR("ShipLua [{}]: {}", modId, message);
                break;
        }
    });
}

std::string GetHostVersion() {
    return std::to_string(gBuildVersionMajor) + "." + std::to_string(gBuildVersionMinor) + "." +
           std::to_string(gBuildVersionPatch);
}

std::int16_t DegreesToBinang(double degrees) {
    const double normalized = std::remainder(degrees, 360.0);
    return static_cast<std::int16_t>(std::lround(normalized * (65536.0 / 360.0)));
}

// Namespace dos assets do OOT dentro do MM: todo o oot.o2r vizinho fica
// endereçável como "oot/<caminho original>".
constexpr const char* kOotNamespace = "oot/";

// Espelho do MmCrossWorldArchive do host OOT: lê DIRETO do oot.o2r da
// instalação vizinha, expõe tudo sob "oot/" e cria alias no hash original
// apenas para dados de render (objects/, textures/) que não colidem —
// sistemas enumeradores (áudio etc.) nunca veem as entradas do OOT.
class OotCrossWorldArchive final : public Ship::Archive {
  public:
    OotCrossWorldArchive(const std::string& path, Ship::ArchiveManager* manager)
        : Ship::Archive(path), mInner(std::make_shared<Ship::O2rArchive>(path)), mManager(manager) {
    }

    bool Open() override {
        if (!mInner->Open()) {
            SPDLOG_WARN("ShipLua n\xC3\xA3o abriu o archive interno '{}'", GetPath());
            return false;
        }
        std::size_t aliased = 0;
        std::size_t blocked = 0;
        const auto files = mInner->ListFiles();
        for (const auto& [hash, filePath] : *files) {
            IndexFile(kOotNamespace + filePath);
            const bool renderData = filePath.rfind("objects/", 0) == 0 || filePath.rfind("textures/", 0) == 0;
            if (renderData && mManager != nullptr && !mManager->HasFile(filePath)) {
                IndexFile(filePath);
                ++aliased;
            } else {
                ++blocked;
            }
        }
        mOwnIndex = ListFiles();
        SPDLOG_INFO("ShipLua exp\xC3\xB4s {} assets do OOT sob 'oot/' ({} com alias direto, {} sem alias)",
                    files->size(), aliased, blocked);
        return !files->empty();
    }

    bool Close() override {
        mOwnIndex.reset();
        return mInner->Close();
    }

    std::shared_ptr<Ship::File> LoadFile(const std::string& filePath) override {
        if (!HasFile(filePath)) {
            return nullptr;
        }
        std::shared_ptr<Ship::File> file;
        if (filePath.rfind(kOotNamespace, 0) == 0) {
            file = mInner->LoadFile(filePath.substr(std::char_traits<char>::length(kOotNamespace)));
        } else {
            file = mInner->LoadFile(filePath);
        }
        SanitizeOotDisplayList(file);
        return file;
    }

    std::shared_ptr<Ship::File> LoadFile(uint64_t hash) override {
        if (mOwnIndex == nullptr) {
            return nullptr;
        }
        const auto it = mOwnIndex->find(hash);
        if (it == mOwnIndex->end()) {
            return nullptr;
        }
        return LoadFile(it->second);
    }

    bool WriteFile(const std::string&, const std::vector<uint8_t>&) override {
        return false;
    }

    // Tradução do dialeto SoH → 2ship: neutraliza saltos/shader ops que não
    // existem aqui e remapeia os opcodes de interpolação divergentes
    // (SoH 0x45/0x46 → MM 0x44/0x45). Ops hash consomem 16 bytes.
    static void SanitizeOotDisplayList(const std::shared_ptr<Ship::File>& file) {
        if (file == nullptr || file->Buffer == nullptr || file->Buffer->size() < 0x48) {
            return;
        }
        std::vector<char>& bytes = *file->Buffer;
        if (std::memcmp(bytes.data() + 4, "TLDO", 4) != 0) {
            return;
        }
        for (std::size_t i = 0x40; i + 8 <= bytes.size();) {
            const uint8_t opcode = static_cast<uint8_t>(bytes[i + 3]);
            std::size_t advance = 8;
            switch (opcode) {
                case 0x20:
                case 0x24:
                case 0x25:
                case 0x27:
                case 0x29:
                case 0x31:
                case 0x32:
                case 0x33:
                case 0x35:
                case 0x36:
                case 0x42:
                    advance = 16;
                    break;
                case 0x3D: // G_DL_INDEX — convenção que não cruza jogos
                case 0x43: // PUSH_SHADER do SoH (0x43 aqui é LOAD_SHADER)
                case 0x44: // POP_SHADER do SoH (0x44 aqui é SETTILESIZE_INTERP)
                    std::memset(bytes.data() + i, 0, 8);
                    break;
                case 0x45: // SETTILESIZE_INTERP: SoH 0x45 → MM 0x44
                    bytes[i + 3] = 0x44;
                    break;
                case 0x46: // SETTARGETINTERPINDEX: SoH 0x46 → MM 0x45
                    bytes[i + 3] = 0x45;
                    break;
                default:
                    break;
            }
            i += advance;
            if (opcode == 0xDF) {
                break;
            }
        }
    }

  private:
    std::shared_ptr<Ship::O2rArchive> mInner;
    Ship::ArchiveManager* mManager = nullptr;
    std::shared_ptr<std::unordered_map<uint64_t, std::string>> mOwnIndex;
};

// Monta em runtime o oot.o2r da instalação vizinha (../OOT por convenção,
// override via SHIPLUA_OOT_ROOT).
void MountCrossWorldArchives() {
    const std::shared_ptr<Ship::Context> shipContext = Ship::Context::GetInstance();
    if (shipContext == nullptr || shipContext->GetResourceManager() == nullptr) {
        return;
    }
    const auto archiveManager = shipContext->GetResourceManager()->GetArchiveManager();
    if (archiveManager == nullptr) {
        return;
    }

    // Sonda os layouts conhecidos: override explícito, pacote do launcher
    // (raiz/hosts/oot), host irmão ao lado do exe e, por último, o CWD.
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    // O override é exclusivo: quem aponta SHIPLUA_OOT_ROOT espera aquele
    // caminho, não uma busca automática em volta dele.
    if (const char* configured = std::getenv("SHIPLUA_OOT_ROOT"); configured != nullptr && *configured != '\0') {
        candidates.emplace_back(configured);
    } else {
    // O layout do launcher mantém os dois archives na raiz do pacote (é onde
    // o próprio mm.o2r é encontrado), então a pasta do app vem primeiro.
    const std::filesystem::path cwd = std::filesystem::absolute(Ship::Context::GetAppDirectoryPath(""), ec);
    candidates.push_back(cwd);
    candidates.push_back(cwd / "hosts" / "oot");
    candidates.push_back(cwd.parent_path());
    candidates.push_back(cwd.parent_path() / "oot");
    candidates.push_back(cwd.parent_path() / "OOT");
    candidates.push_back(cwd.parent_path().parent_path() / "hosts" / "oot");
#ifdef _WIN32
    const std::filesystem::path runtimeRoot = RuntimeRoot();
    candidates.push_back(runtimeRoot);
    candidates.push_back(runtimeRoot / "hosts" / "oot");
    candidates.push_back(runtimeRoot.parent_path());
    candidates.push_back(runtimeRoot.parent_path() / "oot");
    candidates.push_back(runtimeRoot.parent_path().parent_path());
    candidates.push_back(runtimeRoot.parent_path().parent_path() / "hosts" / "oot");
#endif
    }

    std::filesystem::path ootArchive;
    for (const std::filesystem::path& candidate : candidates) {
        const std::filesystem::path probe = candidate / "oot.o2r";
        if (std::filesystem::is_regular_file(probe, ec)) {
            ootArchive = probe;
            break;
        }
    }
    if (ootArchive.empty()) {
        std::string tried;
        for (const auto& candidate : candidates) {
            tried += (tried.empty() ? "" : "; ") + candidate.string();
        }
        SPDLOG_INFO("ShipLua: oot.o2r n\xC3\xA3o encontrado (procurado em: {}) — assets do OOT indispon\xC3\xADveis "
                    "no MM",
                    tried);
        return;
    }

    auto archive = std::make_shared<OotCrossWorldArchive>(ootArchive.string(), archiveManager.get());
    archive->Load();
    if (!archive->IsLoaded()) {
        SPDLOG_WARN("ShipLua n\xC3\xA3o conseguiu montar '{}'", ootArchive.string());
        return;
    }
    if (archiveManager->AddArchive(archive) != nullptr) {
        SPDLOG_INFO("ShipLua montou o oot.o2r do OOT em modo cross-world: {}", ootArchive.string());
    } else {
        SPDLOG_WARN("ShipLua n\xC3\xA3o conseguiu registrar '{}'", ootArchive.string());
    }
}

std::shared_ptr<MmActorProvider> CreateActorProvider() {
    MmActorProviderHooks hooks;
    hooks.objectReady = [](std::int16_t objectId) {
        if (gPlayState == nullptr) {
            return false;
        }
        const s32 objectSlot = Object_GetSlot(&gPlayState->objectCtx, objectId);
        return objectSlot > OBJECT_SLOT_NONE && Object_IsLoaded(&gPlayState->objectCtx, objectSlot);
    };
    hooks.spawn = [](const MmActorDefinition& definition, const ShipLua::ActorSpawnRequest& request) -> void* {
        if (gPlayState == nullptr) {
            return nullptr;
        }
        float x = static_cast<float>(request.x);
        float y = static_cast<float>(request.y);
        float z = static_cast<float>(request.z);
        std::int16_t rotationY = DegreesToBinang(request.rotationY);
        // Posição (0,0,0) significa "na frente do player" (plan-sdk §8.4).
        if (request.x == 0 && request.y == 0 && request.z == 0) {
            if (Player* player = GET_PLAYER(gPlayState); player != nullptr) {
                const float forward = 60.0f;
                x = player->actor.world.pos.x + Math_SinS(player->actor.shape.rot.y) * forward;
                y = player->actor.world.pos.y;
                z = player->actor.world.pos.z + Math_CosS(player->actor.shape.rot.y) * forward;
                rotationY = static_cast<std::int16_t>(player->actor.shape.rot.y + 0x8000);
            }
        }
        Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, definition.actorId, x, y, z,
                                     DegreesToBinang(request.rotationX), rotationY,
                                     DegreesToBinang(request.rotationZ), definition.params);
        if (spawned != nullptr && definition.key == "mm.oot_rauru") {
            if (!ShipLuaOotPuppet_AttachRauru(spawned, gPlayState)) {
                Actor_Kill(spawned);
                return nullptr;
            }
        }
        return spawned;
    };
    hooks.kill = [](void* actor) {
        if (actor != nullptr) {
            Actor_Kill(static_cast<Actor*>(actor));
        }
    };
    std::vector<MmActorDefinition> allowlist{
        { "mm.en_dg", ACTOR_EN_DG, OBJECT_DOG, static_cast<std::int16_t>(0x03E0) },
        // Rauru do OOT: host En_Item00 (gameplay_keep) com update/draw
        // trocados por ShipLuaOotPuppet_AttachRauru após o spawn.
        { "mm.oot_rauru", ACTOR_EN_ITEM00, GAMEPLAY_KEEP, 0 },
    };
    return std::make_shared<MmActorProvider>(std::move(allowlist), std::move(hooks), CreateLogger(), ACTOR_PLAYER);
}

ShipLua::Result<void> RegisterHostCapability(const std::string& id, const std::string& description) {
    if (gCapabilityRegistry == nullptr) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::InvalidState, "capability registry is unavailable");
    }
    const auto providerVersion = ShipLua::SemVersion::Parse(GetHostVersion());
    const auto capabilityVersion = ShipLua::SemVersion::Parse(std::string(ShipLua::Generated::kApiVersion));
    if (!providerVersion.isOk() || !capabilityVersion.isOk()) {
        return ShipLua::Result<void>::err(ShipLua::ErrorCode::HostFailure, "invalid 2Ship or ShipLua version");
    }
    ShipLua::CapabilityProvider offer;
    offer.name = "2ship-native";
    offer.providerVersion = *providerVersion.value;
    offer.capabilityVersion = *capabilityVersion.value;
    offer.games = { "mm" };
    offer.stability = ShipLua::CapabilityStability::Experimental;
    offer.description = description;
    return gCapabilityRegistry->Register(id, std::move(offer));
}

ShipLua::Result<ShipLua::LuaApiHostContext> CreateHostContext() {
    ShipLua::LuaApiHostContext context;
    context.gameId = "mm";
    context.hostVersion = GetHostVersion();
    context.capabilities = { "mm.player.jump", "mm.spawn_dog", "mm.player.sword_skin", "player.speed" };
    context.hotkeys = gHotkeys;
    context.capabilityRegistry = gCapabilityRegistry;
    context.actors = gActorProvider;
    auto registered = RegisterHostCapability("mm.player.jump", "Apply a validated jump impulse to MM Link.");
    if (!registered.isOk()) {
        return ShipLua::Result<ShipLua::LuaApiHostContext>::err(registered.code, registered.message);
    }
    registered = RegisterHostCapability("mm.spawn_dog", "Spawn the legacy MM dog demo actor.");
    if (!registered.isOk()) {
        return ShipLua::Result<ShipLua::LuaApiHostContext>::err(registered.code, registered.message);
    }
    registered = RegisterHostCapability("mm.player.sword_skin",
                                        "Swap the held Kokiri sword display list between MM and OoT visuals.");
    if (!registered.isOk()) {
        return ShipLua::Result<ShipLua::LuaApiHostContext>::err(registered.code, registered.message);
    }
    registered = RegisterHostCapability("player.speed", "Scale the player's movement speed by a validated factor.");
    if (!registered.isOk()) {
        return ShipLua::Result<ShipLua::LuaApiHostContext>::err(registered.code, registered.message);
    }
    if (const char* available = std::getenv("LINKSPAN_AVAILABLE_GAMES"); available != nullptr) {
        const std::string games(available);
        if (games.find("oot") != std::string::npos) {
            context.availableGames.push_back("oot");
        }
        if (games.find("mm") != std::string::npos) {
            context.availableGames.push_back("mm");
        }
    }
    if (BothGamesAvailable() && GetBridgeConfig().has_value()) {
        context.capabilities.push_back("world.travel");
        context.worldTravel = RequestWorldTravel;
        registered =
            RegisterHostCapability("world.travel", "Travel to a logical destination in the other Link-Span host.");
        if (!registered.isOk()) {
            return ShipLua::Result<ShipLua::LuaApiHostContext>::err(registered.code, registered.message);
        }
    }
    return ShipLua::Result<ShipLua::LuaApiHostContext>::ok(std::move(context));
}

// ship.mm.player.jump(): applies a host-controlled vertical impulse only when
// the player is alive and standing on the ground. No internal pointer or force
// value crosses the Lua boundary.
int LuaPlayerJump(lua_State* L) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }

    Player* player = GET_PLAYER(play);
    if (player == nullptr || (player->stateFlags1 & PLAYER_STATE1_DEAD) != 0 ||
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    player->actor.velocity.y = 6.34375f;
    lua_pushboolean(L, 1);
    return 1;
}

// ship.player.set_speed_multiplier(factor): primitiva comum aos dois jogos.
// Dirige o LinkSpeedModifier que o host já implementa, em modo "sempre ativo"
// (Mode 1), sem exigir que o jogador segure o botão. 1.0 restaura o estado.
bool gSpeedForced = false;
float gSpeedPreviousValue = 1.0f;
int gSpeedPreviousMode = 0;

int LuaSetSpeedMultiplier(lua_State* L) {
    const double requested = luaL_checknumber(L, 1);
    if (!std::isfinite(requested) || requested < 0.1 || requested > 5.0) {
        SPDLOG_WARN("ShipLua set_speed_multiplier: fator fora da faixa 0.1–5.0");
        lua_pushboolean(L, 0);
        return 1;
    }

    const bool restore = std::fabs(requested - 1.0) < 0.0001;
    if (!gSpeedForced && !restore) {
        gSpeedPreviousValue = CVarGetFloat("gCheats.SpeedModifier.Value", 1.0f);
        gSpeedPreviousMode = CVarGetInteger("gCheats.SpeedModifier.Mode", 0);
        gSpeedForced = true;
    }

    if (restore) {
        if (gSpeedForced) {
            CVarSetFloat("gCheats.SpeedModifier.Value", gSpeedPreviousValue);
            CVarSetInteger("gCheats.SpeedModifier.Mode", gSpeedPreviousMode);
            gSpeedForced = false;
        }
        SPDLOG_INFO("ShipLua set_speed_multiplier: velocidade restaurada");
    } else {
        CVarSetFloat("gCheats.SpeedModifier.Value", static_cast<float>(requested));
        CVarSetInteger("gCheats.SpeedModifier.Mode", 1); // 1 = sempre ativo
        SPDLOG_INFO("ShipLua set_speed_multiplier: velocidade x{:.2f}", requested);
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ship.mm.player.set_sword_skin("oot" | "mm"): troca a display list da espada
// Kokiri empunhada pelo Link humano. A variante "oot" aponta para o punho com
// a Kokiri Sword do child Link do OoT, lido do oot.o2r vizinho — os assets de
// object_link_child não colidem entre os jogos, então as texturas resolvem
// pelo alias de hash. Nenhum ponteiro nativo cruza a fronteira Lua.
static const ALIGN_ASSET(2) char kOotKokiriSwordHandDL[] =
    "__OTR__oot/objects/object_link_child/gLinkChildLeftFistAndKokiriSwordNearDL";

bool gSwordSkinIsOot = false;
Gfx* gVanillaOneHandSwordDLs[2] = { nullptr, nullptr };
Gfx* gVanillaKokiriEquipDLs[2] = { nullptr, nullptr };

// Índices da forma humana na tabela por-forma (as duas últimas entradas).
constexpr std::size_t kHumanLeftHandIndex = 2 * PLAYER_FORM_HUMAN;

int LuaSetSwordSkin(lua_State* L) {
    const char* requested = luaL_optstring(L, 1, "mm");
    const bool wantsOot = std::strcmp(requested, "oot") == 0;

    if (gVanillaOneHandSwordDLs[0] == nullptr) {
        gVanillaOneHandSwordDLs[0] = gPlayerLeftHandOneHandSwordDLs[kHumanLeftHandIndex];
        gVanillaOneHandSwordDLs[1] = gPlayerLeftHandOneHandSwordDLs[kHumanLeftHandIndex + 1];
        gVanillaKokiriEquipDLs[0] = D_801C018C[0];
        gVanillaKokiriEquipDLs[1] = D_801C018C[1];
    }

    if (wantsOot && !ResourceMgr_FileExists(kOotKokiriSwordHandDL)) {
        SPDLOG_WARN("ShipLua set_sword_skin: assets do OoT indisponíveis — o oot.o2r não está montado");
        lua_pushboolean(L, 0);
        return 1;
    }

    Gfx* const target = wantsOot ? (Gfx*)kOotKokiriSwordHandDL : gVanillaOneHandSwordDLs[0];
    gPlayerLeftHandOneHandSwordDLs[kHumanLeftHandIndex] = target;
    gPlayerLeftHandOneHandSwordDLs[kHumanLeftHandIndex + 1] = wantsOot ? target : gVanillaOneHandSwordDLs[1];
    D_801C018C[0] = wantsOot ? target : gVanillaKokiriEquipDLs[0];
    D_801C018C[1] = wantsOot ? target : gVanillaKokiriEquipDLs[1];
    gSwordSkinIsOot = wantsOot;

    SPDLOG_INFO("ShipLua set_sword_skin: espada Kokiri agora usa o visual {}", wantsOot ? "do OoT" : "do MM");
    lua_pushboolean(L, 1);
    return 1;
}

// ship.mm.spawn_dog(): spawns the Clock Town dog (En_Dg) at the player's
// position, returning true on success. object_dog is loaded in Clock Town, so
// the dog appears reliably there.
int LuaSpawnDog(lua_State* L) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        SPDLOG_WARN("ShipLua spawn_dog: gPlayState nulo (fora de gameplay)");
        lua_pushboolean(L, 0);
        return 1;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        SPDLOG_WARN("ShipLua spawn_dog: player nulo");
        lua_pushboolean(L, 0);
        return 1;
    }

    // Diagnostics: scene, player form, and whether object_dog is loaded in the scene.
    s32 dogSlot = Object_GetSlot(&play->objectCtx, OBJECT_DOG);
    SPDLOG_INFO("ShipLua spawn_dog: sceneId={} form={} object_dog_slot={} (OBJECT_SLOT_NONE={})",
                (int)play->sceneId, (int)player->transformation, (int)dogSlot, (int)OBJECT_SLOT_NONE);

    // 0x03E0 = ENDG_PARAMS(path=0, index=ENDG_INDEX_SOUTH_CLOCK_TOWN=31).
    // A South Clock Town dog. It MUST have a valid path: EnDg_IdleMove ->
    // EnDg_MoveAlongPath does Actor_Kill when this->path == NULL, which is why a
    // PATH_INDEX_NONE dog self-destructs the moment it idles (e.g. as human Link).
    // Clock Town scenes have dog paths, so path index 0 keeps it alive.
    Actor* dog =
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_DG, player->actor.world.pos.x, player->actor.world.pos.y,
                    player->actor.world.pos.z, 0, player->actor.shape.rot.y, 0, (s16)0x03E0);
    SPDLOG_INFO("ShipLua spawn_dog: Actor_Spawn -> {} em pos=({:.0f},{:.0f},{:.0f})",
                dog != nullptr ? "ator criado" : "NULL", player->actor.world.pos.x, player->actor.world.pos.y,
                player->actor.world.pos.z);
    lua_pushboolean(L, dog != nullptr);
    return 1;
}

// Installs the MM-specific ship.mm.* table onto a mod runtime. Uses
// require("ship") so it works regardless of how the core registers the module.
void InstallMmApi(lua_State* L) {
    if (L == nullptr) {
        return;
    }
    lua_getglobal(L, "require");
    lua_pushstring(L, "ship");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int shipTable = lua_gettop(L);
    lua_getfield(L, shipTable, "mm");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    const int mmTable = lua_gettop(L);
    lua_pushcfunction(L, LuaSpawnDog);
    lua_setfield(L, -2, "spawn_dog");

    lua_getfield(L, mmTable, "player");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, LuaPlayerJump);
    lua_setfield(L, -2, "jump");
    lua_pushcfunction(L, LuaSetSwordSkin);
    lua_setfield(L, -2, "set_sword_skin");
    lua_setfield(L, mmTable, "player");

    lua_setfield(L, shipTable, "mm");

    // Primitiva comum aos dois jogos: ship.player.set_speed_multiplier.
    lua_getfield(L, shipTable, "player");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, LuaSetSpeedMultiplier);
    lua_setfield(L, -2, "set_speed_multiplier");
    lua_setfield(L, shipTable, "player");
    lua_pop(L, 1);
}

void LoadModsAndDispatchReady(const ShipLua::LuaApiHostContext& context) {
    const std::shared_ptr<Ship::Context> shipContext = Ship::Context::GetInstance();
    if (shipContext == nullptr) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o encontrou o contexto do aplicativo");
        return;
    }

    const std::string appName = shipContext->GetShortName();
    const std::filesystem::path modsRoot = Ship::Context::GetPathRelativeToAppDirectory("mods", appName);
    const std::filesystem::path cacheRoot = modsRoot / ".shiplua-cache";
    std::error_code error;
    std::filesystem::create_directories(modsRoot, error);
    if (error) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu criar a pasta de mods '{}': {}", modsRoot.string(), error.message());
        return;
    }

    auto loaded = gModHost->LoadModsFromRoot(modsRoot, cacheRoot);
    if (!loaded.isOk()) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu carregar a pasta de mods '{}': {}", modsRoot.string(),
                     loaded.message);
        return;
    }
    for (const auto& [modId, reason] : loaded.value->rejected) {
        SPDLOG_WARN("ShipLua rejeitou o mod '{}': {}", modId, reason);
    }
    SPDLOG_INFO("ShipLua carregou {} mod(s) de '{}'", loaded.value->loadedIds.size(), modsRoot.string());

    // Install MM-specific bindings (ship.mm.*) on every loaded runtime before
    // game.ready so mods can use them from the first event onwards.
    for (const std::string& modId : loaded.value->loadedIds) {
        ShipLua::LuaRuntime* runtime = gModHost->GetRuntime(modId);
        if (runtime != nullptr) {
            InstallMmApi(runtime->State());
        }
    }

    ShipLua::EventPayload payload{
        { "game_id", context.gameId },
        { "host_version", context.hostVersion },
        { "runtime_version", context.runtimeVersion },
        { "api_version", std::string(ShipLua::Generated::kApiVersion) },
    };
    auto ready = gModHost->DispatchEvent("game.ready", payload);
    if (!ready.isOk()) {
        SPDLOG_ERROR("ShipLua n\xC3\xA3o conseguiu publicar game.ready: {}", ready.message);
        return;
    }
    for (const ShipLua::CallbackFailure& failure : ready.value->failures) {
        SPDLOG_ERROR("ShipLua [{}] falhou em game.ready: {}", failure.modId, failure.message);
    }
}

} // namespace

void Initialize() {
    if (gModHost != nullptr) {
        SPDLOG_WARN("ShipLua j\xC3\xA1 foi inicializado");
        return;
    }

    gHotkeys = std::make_shared<MmHotkeyRegistry>();
    gCapabilityRegistry = std::make_shared<ShipLua::CapabilityRegistry>();
    gActorProvider = CreateActorProvider();
    const auto actorCapabilities = gActorProvider->RegisterCapabilities(*gCapabilityRegistry);
    if (!actorCapabilities.isOk()) {
        SPDLOG_ERROR("ShipLua failed to register the MM actor provider: {}", actorCapabilities.message);
        gActorProvider.reset();
        gCapabilityRegistry.reset();
        gHotkeys.reset();
        return;
    }
    auto catalog = ShipLua::PortableItemCatalog::CreateDefault();
    if (!catalog.isOk()) {
        SPDLOG_ERROR("ShipLua não conseguiu criar o catálogo portátil MM: {}", catalog.message);
        gActorProvider.reset();
        gCapabilityRegistry.reset();
        gHotkeys.reset();
        return;
    }
    gWorldAdapter = std::make_shared<MmWorldAdapter>(std::move(*catalog.value));
    gSaveLoadHook = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSaveLoad>(
        [](s16) { TryConsumeWorldHandoff(); });
    gImportTickHook =
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateUpdate>([]() { TickWorldImport(); });
    gActorDestroyHook = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorDestroy>([](Actor* actor) {
        ShipLuaOotPuppet_HandleActorDestroy(actor);
        if (gActorProvider == nullptr) {
            return;
        }
        const auto destroyed = gActorProvider->OnNativeActorDestroyed(actor);
        if (!destroyed.isOk()) {
            SPDLOG_ERROR("ShipLua failed to invalidate a destroyed MM actor: {}", destroyed.message);
        }
    });
    gPlayDestroyHook = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>([]() {
        ShipLuaOotPuppet_Reset();
        if (gActorProvider == nullptr) {
            return;
        }
        const auto cleaned = gActorProvider->OnSceneChange();
        if (!cleaned.isOk()) {
            SPDLOG_ERROR("ShipLua failed to clean MM actors during scene teardown: {}", cleaned.message);
        }
    });
    auto contextResult = CreateHostContext();
    if (!contextResult.isOk()) {
        SPDLOG_ERROR("ShipLua failed to create the MM host context: {}", contextResult.message);
        Shutdown();
        return;
    }
    ShipLua::LuaApiHostContext context = std::move(*contextResult.value);
    SPDLOG_INFO("ShipLua inicializando para {} {} (commit {})", context.gameId, context.hostVersion, gGitCommitHash);
    gModHost = std::make_unique<ShipLua::ModHost>(context, CreateLogger());
    MountCrossWorldArchives();
    LoadModsAndDispatchReady(context);
    SPDLOG_INFO("ShipLua inicializado");
}

void Shutdown() {
    if (gModHost == nullptr && gActorProvider == nullptr) {
        return;
    }

    if (gActorProvider != nullptr) {
        const auto cleaned = gActorProvider->Shutdown();
        if (!cleaned.isOk()) {
            SPDLOG_ERROR("ShipLua failed to shut down the MM actor provider: {}", cleaned.message);
        }
    }
    gModHost.reset();
    if (gSaveLoadHook != 0) {
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnSaveLoad>(gSaveLoadHook);
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnGameStateUpdate>(gImportTickHook);
        gImportTickHook = 0;
        gPendingHandoff.reset();
        gSaveLoadHook = 0;
    }
    if (gActorDestroyHook != 0) {
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnActorDestroy>(gActorDestroyHook);
        gActorDestroyHook = 0;
    }
    if (gPlayDestroyHook != 0) {
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnPlayDestroy>(gPlayDestroyHook);
        gPlayDestroyHook = 0;
    }
    gActorProvider.reset();
    gCapabilityRegistry.reset();
    gWorldAdapter.reset();
    gHotkeys.reset();
    SPDLOG_INFO("ShipLua finalizado");
}

ShipLua::ModHost* GetModHost() {
    return gModHost.get();
}

MmActorProvider* ActorProvider() {
    return gActorProvider.get();
}

MmHotkeyRegistry* Hotkeys() {
    return gHotkeys.get();
}

MmWorldAdapter* WorldAdapter() {
    return gWorldAdapter.get();
}

void OpenLogWindow() {
#ifdef _WIN32
    const std::filesystem::path log = RuntimeRoot() / "logs" / "2 Ship 2 Harkinian.log";
    std::filesystem::create_directories(log.parent_path());
    std::wstring command =
        L"powershell.exe -NoLogo -NoProfile -NoExit -Command \"$host.UI.RawUI.WindowTitle='Link-Span - log MM'; "
        L"Write-Host 'Aguardando o log de MM...'; while(-not (Test-Path -LiteralPath " +
        QuotePowerShellLiteral(log.wstring()) + L")){Start-Sleep -Milliseconds 250}; Get-Content -LiteralPath " +
        QuotePowerShellLiteral(log.wstring()) + L" -Tail 200 -Wait\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        RuntimeRoot().c_str(), &startup, &process)) {
        SPDLOG_ERROR("ShipLua não conseguiu abrir a janela de log (erro {})", GetLastError());
        return;
    }
    SPDLOG_INFO("ShipLua abriu uma nova janela para acompanhar o log MM");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    SPDLOG_WARN("ShipLua OpenLogWindow só está disponível no Windows");
#endif
}

} // namespace ShipLuaHost
