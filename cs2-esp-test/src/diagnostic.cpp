// diagnostic.cpp - Phase 1: read-only memory diagnostic for CS2.
// - Finds the cs2.exe process (read-only, no injection, no writes).
// - Enumerates modules and reports base addresses.
// - Warns if the running game build differs from the dumped build (14196).
// - Reads each critical global offset (module-relative) and reports the
//   absolute address + value; FAILED reads are reported as-is (never zeroed
//   or guessed).
// - Resolves the entity list by probing known CS2 layouts and validates every
//   entity (team, health, life state, dormant, absolute origin).
// - Reports the local player and the view matrix.
//
// Build (PowerShell, x64):
//   .\build.ps1 -Target diagnostic
// Run (cs2.exe must be running, e.g. in an offline/practice match):
//   .\cs2-esp-diagnostic.exe
//
// All offsets are sourced from cs2-dumper JSON (build 14196) via the generated
// headers offsets.hpp (module-relative dw*) and schema.hpp (class-field m_*).

#include "mem.h"
#include "offsets.hpp"
#include "schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int EXPECTED_BUILD = 14196;

struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };

std::optional<Vec3> readVec3(const MemReader& mem, uintptr_t addr) {
    std::optional<float> x = mem.read<float>(addr);
    std::optional<float> y = mem.read<float>(addr + 4);
    std::optional<float> z = mem.read<float>(addr + 8);
    if (!x || !y || !z) return std::nullopt;
    return Vec3{*x, *y, *z};
}

bool saneCoord(float v) {
    return std::isfinite(v) && std::fabs(v) < 100000.f;
}

bool saneOrigin(const Vec3& v) {
    return saneCoord(v.x) && saneCoord(v.y) && saneCoord(v.z);
}

std::string teamName(int t) {
    switch (t) {
        case 0: return "unassigned";
        case 1: return "spectator";
        case 2: return "CT";
        case 3: return "TERRORIST";
        default: return std::string("team(") + std::to_string(t) + ")";
    }
}

std::string lifeStateName(int s) {
    switch (s) {
        case 0: return "ALIVE";
        case 1: return "DYING";
        case 2: return "DEAD";
        case 3: return "RESPAWNABLE";
        case 4: return "DEAD_ATTACKER";
        default: return std::string("state(") + std::to_string(s) + ")";
    }
}

std::string hex64(uintptr_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setw(16)
       << std::setfill('0') << v << std::dec;
    return os.str();
}

bool gBuildMismatchWarningShown = false;
int gFailures = 0;

template <typename T>
void reportGlobal(const MemReader& mem, const char* modName, uintptr_t modBase,
                  std::ptrdiff_t off, const char* label) {
    uintptr_t abs = modBase + static_cast<uintptr_t>(off);
    std::optional<T> val = mem.read<T>(abs);
    std::cout << "  [" << (val ? "OK " : "FAIL") << "] " << std::left
              << std::setw(26) << label << " @ " << modName << "+"
              << off << " = " << hex64(abs);
    if (val) std::cout << "  value=" << *val;
    else {
        std::cout << "  value=<READ FAILED>";
        ++gFailures;
    }
    std::cout << "\n";
}

void reportPtrGlobal(const MemReader& mem, const char* modName,
                     uintptr_t modBase, std::ptrdiff_t off,
                     const char* label) {
    uintptr_t abs = modBase + static_cast<uintptr_t>(off);
    std::optional<uintptr_t> ptr = mem.readPtr(abs);
    std::cout << "  [" << (ptr ? "OK " : "FAIL") << "] " << std::left
              << std::setw(26) << label << " @ " << modName << "+"
              << off << " = " << hex64(abs);
    if (ptr) std::cout << "  -> points to " << hex64(*ptr);
    else {
        std::cout << "  -> <READ FAILED>";
        ++gFailures;
    }
    std::cout << "\n";
}

struct Entity {
    int index = -1;
    uintptr_t instance = 0;
    uintptr_t entity = 0;       // C_BaseEntity (from CEntityInstance::m_pEntity)
    uintptr_t sceneNode = 0;    // CGameSceneNode
    int team = -1;
    int health = -1;
    int maxHealth = -1;
    int lifeState = -1;
    bool dormant = false;
    Vec3 origin{};
    float distToLocal = -1.f;
    bool valid = false;
};

} // namespace

int main() {
    using namespace cs2::offsets;
    using namespace cs2::schema;

    std::cout << "=== CS2 read-only diagnostic (build " << EXPECTED_BUILD
              << " dump) ===\n\n";

    MemReader mem;
    if (!mem.openByProcessName(L"cs2.exe")) {
        std::cerr << "\nABORTED: could not open cs2.exe.\n";
        return 1;
    }
    std::cout << "Process: cs2.exe (PID " << mem.pid() << "), opened read-only.\n\n";

    // --- Modules ---------------------------------------------------------
    std::cout << "--- Modules ---\n";
    std::vector<ModuleInfo> mods = mem.enumerateModules();
    const std::vector<std::string> wanted = {
        "client.dll", "engine2.dll", "engine.dll", "inputsystem.dll",
        "soundsystem.dll", "vphysics2.dll", "schemasystem.dll",
        "materialsystem2.dll", "rendersystemdx11.dll", "server.dll",
        "panorama.dll", "networksystem.dll", "animationsystem.dll",
        "particles.dll", "pulse_system.dll", "worldrenderer.dll",
        "steamaudio.dll"
    };
    for (const auto& name : wanted) {
        auto base = mem.moduleBase(name);
        if (base) {
            auto it = std::find_if(mods.begin(), mods.end(),
                                   [&](const ModuleInfo& m) {
                                       std::string a = m.name, b = name;
                                       std::transform(a.begin(), a.end(), a.begin(),
                                                      [](unsigned char c){return std::tolower(c);});
                                       std::transform(b.begin(), b.end(), b.begin(),
                                                      [](unsigned char c){return std::tolower(c);});
                                       return a == b;
                                   });
            uint32_t size = (it != mods.end()) ? it->size : 0;
            std::cout << "  " << std::left << std::setw(30) << name
                      << " base=" << hex64(*base)
                      << " size=" << size << "\n";
        } else {
            std::cout << "  " << std::left << std::setw(30) << name
                      << " <NOT LOADED>\n";
        }
    }
    std::cout << "\n";

    // --- Build number check ---------------------------------------------
    auto engine2 = mem.moduleBase("engine2.dll");
    if (engine2) {
        std::optional<uint32_t> build =
            mem.read<uint32_t>(*engine2 + engine2_dll::dwBuildNumber);
        std::cout << "--- Build check ---\n";
        if (build) {
            std::cout << "  engine2.dll+dwBuildNumber = " << *build
                      << " (dump expects " << EXPECTED_BUILD << ")\n";
            if (*build != EXPECTED_BUILD) {
                gBuildMismatchWarningShown = true;
                std::cout << "  !! WARNING: running game build (" << *build
                          << ") differs from the dump build (" << EXPECTED_BUILD
                          << "). Offsets may be stale. Verify before trusting "
                             "any data. !!\n\n";
            } else {
                std::cout << "  Build matches dump. Offsets should apply.\n\n";
            }
        } else {
            ++gFailures;
            std::cout << "  !! WARNING: could not read current build number. !!\n\n";
        }
    } else {
        std::cout << "--- Build check ---\n  engine2.dll not loaded; "
                     "cannot verify build.\n\n";
    }

    // --- Critical globals -------------------------------------------------
    auto client = mem.moduleBase("client.dll");
    if (!client) {
        std::cerr << "ABORTED: client.dll not loaded.\n";
        return 1;
    }
    std::cout << "--- Critical globals (client.dll) ---\n";
    reportPtrGlobal(mem, "client.dll", *client,
                    client_dll::dwEntityList, "dwEntityList (list head)");
    reportPtrGlobal(mem, "client.dll", *client,
                    client_dll::dwLocalPlayerController,
                    "dwLocalPlayerController");
    reportPtrGlobal(mem, "client.dll", *client,
                    client_dll::dwLocalPlayerPawn, "dwLocalPlayerPawn");
    reportPtrGlobal(mem, "client.dll", *client, client_dll::dwViewMatrix,
                    "dwViewMatrix");
    reportPtrGlobal(mem, "client.dll", *client, client_dll::dwGameRules,
                    "dwGameRules");
    reportGlobal<int32_t>(mem, "client.dll", *client,
                          client_dll::dwGlobalVars, "dwGlobalVars");
    std::cout << "\n";

    // --- Entity count ----------------------------------------------------
    std::cout << "--- Entity count ---\n";
    std::optional<int32_t> highestIdx = std::nullopt;
    if (client) {
        // dwGameEntitySystem_highestEntityIndex is relative to the
        // dwGameEntitySystem base (== dwEntityList here).
        uintptr_t countAddr =
            *client + client_dll::dwGameEntitySystem +
            client_dll::dwGameEntitySystem_highestEntityIndex;
        highestIdx = mem.read<int32_t>(countAddr);
        if (highestIdx)
            std::cout << "  highestEntityIndex @ " << hex64(countAddr)
                      << " = " << *highestIdx << "\n";
        else {
            std::cout << "  <READ FAILED> highestEntityIndex @ "
                      << hex64(countAddr) << "\n";
            ++gFailures;
        }
    }
    int scanLimit = 64;
    if (highestIdx && *highestIdx > 0 && *highestIdx <= 4096)
        scanLimit = *highestIdx + 1;
    std::cout << "  scanning entities [0, " << scanLimit << ")\n\n";

    // --- Entity list probing --------------------------------------------
    struct Probe {
        const char* name;
        bool baseIsPtr;   // dwEntityList holds a pointer to the list base
        size_t stride;    // bytes between entries
    };
    const Probe probes[] = {
        {"listBase-ptr  + stride-ptr-array (0x20)", true,  0x20},
        {"direct ptr-array at dwEntityList (0x20)", false, 0x20},
        {"listBase-ptr  + stride-ptr-array (0x10)", true,  0x10},
        {"direct ptr-array at dwEntityList (0x10)", false, 0x10},
    };

    std::vector<Entity> best;
    const Probe* bestProbe = nullptr;

    for (const auto& p : probes) {
        std::vector<Entity> found;
        uintptr_t listBase = 0;
        bool listOk = true;

        uintptr_t baseAddr = *client + client_dll::dwEntityList;
        if (p.baseIsPtr) {
            auto lb = mem.readPtr(baseAddr);
            if (!lb) { listOk = false; }
            else listBase = *lb;
        } else {
            listBase = baseAddr;
        }
        if (!listOk) continue;

        for (int i = 0; i < scanLimit; ++i) {
            uintptr_t entryAddr = listBase + i * p.stride;
            auto entryPtr = mem.readPtr(entryAddr);
            if (!entryPtr || *entryPtr == 0) continue;
            // entryPtr is CEntityInstance*; real entity is at m_pEntity (+16)
            auto entity = mem.readPtr(*entryPtr +
                                      CEntityInstance::m_pEntity);
            if (!entity || *entity == 0) continue;

            Entity e;
            e.index = i;
            e.instance = *entryPtr;
            e.entity = *entity;

            auto sceneNode = mem.readPtr(e.entity +
                                         C_BaseEntity::m_pGameSceneNode);
            if (!sceneNode || *sceneNode == 0) continue;
            e.sceneNode = *sceneNode;

            auto origin = readVec3(mem, e.sceneNode +
                                   CGameSceneNode::m_vecAbsOrigin);
            if (!origin || !saneOrigin(*origin)) continue;
            e.origin = *origin;

            auto team = mem.read<int32_t>(e.entity + C_BaseEntity::m_iTeamNum);
            if (!team || *team < 0 || *team > 3) continue;
            e.team = *team;

            auto health = mem.read<int32_t>(e.entity + C_BaseEntity::m_iHealth);
            if (!health || *health < 0 || *health > 250) continue;
            e.health = *health;

            auto maxHp = mem.read<int32_t>(e.entity + C_BaseEntity::m_iMaxHealth);
            if (maxHp) e.maxHealth = *maxHp;

            auto ls = mem.read<int32_t>(e.entity + C_BaseEntity::m_lifeState);
            if (!ls || *ls < 0 || *ls > 6) continue;
            e.lifeState = *ls;

            auto dorm = mem.read<uint8_t>(e.sceneNode +
                                          CGameSceneNode::m_bDormant);
            e.dormant = dorm ? *dorm != 0 : false;

            e.valid = true;
            found.push_back(e);
        }

        std::cout << "  probe: " << p.name
                  << " -> valid entities: " << found.size() << "\n";
        if (found.size() > best.size()) {
            best = std::move(found);
            bestProbe = &p;
        }
    }

    std::cout << "\n";

    // --- Local player -----------------------------------------------------
    std::cout << "--- Local player ---\n";
    Entity local;
    bool localOk = false;
    if (client) {
        auto pawnPtr = mem.readPtr(*client + client_dll::dwLocalPlayerPawn);
        if (pawnPtr && *pawnPtr != 0) {
            auto sceneNode = mem.readPtr(*pawnPtr +
                                         C_BaseEntity::m_pGameSceneNode);
            if (sceneNode && *sceneNode != 0) {
                auto origin = readVec3(mem, *sceneNode +
                                       CGameSceneNode::m_vecAbsOrigin);
                auto team = mem.read<int32_t>(*pawnPtr +
                                              C_BaseEntity::m_iTeamNum);
                auto health = mem.read<int32_t>(*pawnPtr +
                                                C_BaseEntity::m_iHealth);
                auto ls = mem.read<int32_t>(*pawnPtr +
                                            C_BaseEntity::m_lifeState);
                if (origin && team && health && ls &&
                    saneOrigin(*origin) && *team >= 0 && *team <= 3 &&
                    *health >= 0 && *health <= 250 && *ls >= 0 && *ls <= 6) {
                    local.index = -1;
                    local.entity = *pawnPtr;
                    local.sceneNode = *sceneNode;
                    local.origin = *origin;
                    local.team = *team;
                    local.health = *health;
                    local.lifeState = *ls;
                    auto maxHp = mem.read<int32_t>(*pawnPtr +
                                                   C_BaseEntity::m_iMaxHealth);
                    if (maxHp) local.maxHealth = *maxHp;
                    local.valid = true;
                    localOk = true;
                }
            }
        }
    }
    if (localOk) {
        std::cout << "  OK   local pawn @ " << hex64(local.entity)
                  << " team=" << teamName(local.team)
                  << " health=" << local.health << "/" << local.maxHealth
                  << " state=" << lifeStateName(local.lifeState)
                  << " origin=(" << local.origin.x << ", " << local.origin.y
                  << ", " << local.origin.z << ")\n";
    } else {
        std::cout << "  FAIL local player could not be resolved\n";
        ++gFailures;
    }
    std::cout << "\n";

    // --- Entity listing ---------------------------------------------------
    std::cout << "--- Entities (best probe: "
              << (bestProbe ? bestProbe->name : "none") << ") ---\n";
    for (auto& e : best) {
        float dist = -1.f;
        if (localOk) {
            float dx = e.origin.x - local.origin.x;
            float dy = e.origin.y - local.origin.y;
            float dz = e.origin.z - local.origin.z;
            dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            e.distToLocal = dist;
        }
        std::cout << "  [" << std::setw(3) << e.index << "] "
                  << "team=" << std::left << std::setw(12) << teamName(e.team)
                  << " hp=" << std::setw(3) << e.health
                  << "/" << std::setw(3) << e.maxHealth
                  << " state=" << std::left << std::setw(14)
                  << lifeStateName(e.lifeState)
                  << " dormant=" << (e.dormant ? "yes" : "no ")
                  << " origin=(" << std::fixed << std::setprecision(1)
                  << e.origin.x << ", " << e.origin.y << ", " << e.origin.z
                  << ")"
                  << (dist >= 0 ? " dist=" + std::to_string(
                      static_cast<long long>(std::round(dist))) : "")
                  << "\n";
        if (best.size() > 20 && &e - &best[0] >= 19) {
            std::cout << "  ... (" << (best.size() - 20)
                      << " more entities not shown)\n";
            break;
        }
    }
    std::cout << "\n";

    // --- View matrix (prep for world-to-screen in Phase 2) ---------------
    std::cout << "--- View matrix (client.dll+dwViewMatrix) ---\n";
    if (client) {
        std::vector<float> vm;
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            auto v = mem.read<float>(*client + client_dll::dwViewMatrix + i * 4);
            if (!v) { ok = false; break; }
            vm.push_back(*v);
        }
        if (ok && vm.size() == 16) {
            std::cout << "  OK   view matrix read (" << vm.size()
                      << " floats), row0: " << vm[0] << ", " << vm[1] << ", "
                      << vm[2] << ", " << vm[3] << "\n";
        } else {
            std::cout << "  FAIL view matrix could not be read\n";
            ++gFailures;
        }
    }
    std::cout << "\n";

    // --- Summary ----------------------------------------------------------
    std::cout << "=== Summary ===\n";
    std::cout << "  Critical read failures: " << gFailures << "\n";
    std::cout << "  Valid entities resolved: " << best.size() << "\n";
    if (localOk) std::cout << "  Local player: resolved\n";
    else std::cout << "  Local player: NOT resolved\n";

    bool allOk = (gFailures == 0) && localOk && !best.empty();
    if (gBuildMismatchWarningShown) {
        std::cout << "\n!! BUILD MISMATCH WARNING: game build != 14196. Treat all "
                     "results as UNCONFIRMED until the dump is regenerated. !!\n";
    }
    std::cout << "\nResult: " << (allOk && !gBuildMismatchWarningShown
                                    ? "ALL CHECKS PASSED"
                                    : (allOk ? "PASSED (with build-warning)"
                                              : "CHECK(S) FAILED"))
              << "\n";
    return allOk ? 0 : 1;
}
