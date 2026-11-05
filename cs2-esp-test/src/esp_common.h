#pragma once
// esp_common.h - Shared read-only entity access for the ESP display (Phase 2).
// Reads only; writes nothing to cs2.exe. No injection, no hooks.
#include "mem.h"
#include "offsets.hpp"
#include "schema.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace cs2esp {

struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };

struct Entity {
    int index = -1;
    uintptr_t entity = 0;   // C_BaseEntity instance (from CEntityInstance::m_pEntity)
    uintptr_t sceneNode = 0;// CGameSceneNode instance
    int team = -1;
    int health = -1;
    int maxHealth = -1;
    int lifeState = -1;
    bool dormant = false;
    Vec3 origin{};
};

inline std::optional<Vec3> readVec3(const MemReader& mem, uintptr_t addr) {
    auto x = mem.read<float>(addr);
    auto y = mem.read<float>(addr + 4);
    auto z = mem.read<float>(addr + 8);
    if (!x || !y || !z) return std::nullopt;
    return Vec3{*x, *y, *z};
}

inline bool saneCoord(float v) {
    return std::isfinite(v) && std::fabs(v) < 100000.f;
}
inline bool saneOrigin(const Vec3& v) {
    return saneCoord(v.x) && saneCoord(v.y) && saneCoord(v.z);
}

// Local player pawn via dwLocalPlayerPawn (module-relative global pointer).
inline std::optional<Entity> readLocalPlayer(const MemReader& mem,
                                             uintptr_t clientBase) {
    using namespace cs2::offsets::client_dll;
    using namespace cs2::schema;
    auto pawn = mem.readPtr(clientBase + dwLocalPlayerPawn);
    if (!pawn || *pawn == 0) return std::nullopt;
    auto sceneNode = mem.readPtr(*pawn + C_BaseEntity::m_pGameSceneNode);
    if (!sceneNode || *sceneNode == 0) return std::nullopt;
    auto origin = readVec3(mem, *sceneNode + CGameSceneNode::m_vecAbsOrigin);
    auto team = mem.read<int32_t>(*pawn + C_BaseEntity::m_iTeamNum);
    auto health = mem.read<int32_t>(*pawn + C_BaseEntity::m_iHealth);
    auto ls = mem.read<int32_t>(*pawn + C_BaseEntity::m_lifeState);
    if (!origin || !team || !health || !ls || !saneOrigin(*origin) ||
        *team < 0 || *team > 3 || *health < 0 || *health > 250 || *ls < 0 ||
        *ls > 6)
        return std::nullopt;
    Entity e;
    e.index = -1;
    e.entity = *pawn;
    e.sceneNode = *sceneNode;
    e.origin = *origin;
    e.team = *team;
    e.health = *health;
    e.lifeState = *ls;
    auto mhp = mem.read<int32_t>(*pawn + C_BaseEntity::m_iMaxHealth);
    if (mhp) e.maxHealth = *mhp;
    auto dorm = mem.read<uint8_t>(*sceneNode + CGameSceneNode::m_bDormant);
    e.dormant = dorm ? *dorm != 0 : false;
    return e;
}

// Current view yaw (degrees) from dwViewAngles (x=pitch, y=yaw, z=roll).
inline std::optional<float> readViewYaw(const MemReader& mem,
                                        uintptr_t clientBase) {
    using namespace cs2::offsets::client_dll;
    auto ang = readVec3(mem, clientBase + dwViewAngles);
    if (!ang) return std::nullopt;
    return ang->y;
}

struct ListProbe {
    bool baseIsPtr;  // dwEntityList holds a pointer to the real list base
    size_t stride;   // bytes between entries
};

// Try known CS2 entity-list layouts; return the one that yields valid entities.
inline std::optional<ListProbe> probeEntityList(const MemReader& mem,
                                                uintptr_t clientBase,
                                                int scanLimit) {
    using namespace cs2::offsets::client_dll;
    using namespace cs2::schema;
    const ListProbe probes[] = {
        {true, 0x20}, {false, 0x20}, {true, 0x10}, {false, 0x10},
    };
    uintptr_t baseAddr = clientBase + dwEntityList;
    int bestCount = 0;
    ListProbe best = {false, 0};

    for (const auto& p : probes) {
        uintptr_t listBase = 0;
        if (p.baseIsPtr) {
            auto lb = mem.readPtr(baseAddr);
            if (!lb) continue;
            listBase = *lb;
        } else {
            listBase = baseAddr;
        }
        int valid = 0;
        for (int i = 0; i < scanLimit; ++i) {
            auto entry = mem.readPtr(listBase + i * p.stride);
            if (!entry || *entry == 0) continue;
            auto ent = mem.readPtr(*entry + CEntityInstance::m_pEntity);
            if (!ent || *ent == 0) continue;
            auto sn = mem.readPtr(*ent + C_BaseEntity::m_pGameSceneNode);
            if (!sn || *sn == 0) continue;
            auto origin = readVec3(mem, *sn + CGameSceneNode::m_vecAbsOrigin);
            if (!origin || !saneOrigin(*origin)) continue;
            auto team = mem.read<int32_t>(*ent + C_BaseEntity::m_iTeamNum);
            auto health = mem.read<int32_t>(*ent + C_BaseEntity::m_iHealth);
            auto ls = mem.read<int32_t>(*ent + C_BaseEntity::m_lifeState);
            if (!team || !health || !ls || *team < 0 || *team > 3 ||
                *health < 0 || *health > 250 || *ls < 0 || *ls > 6)
                continue;
            ++valid;
        }
        if (valid > bestCount) { bestCount = valid; best = p; }
    }
    if (bestCount == 0) return std::nullopt;
    return best;
}

inline std::vector<Entity> readEntities(const MemReader& mem,
                                        const ListProbe& probe,
                                        uintptr_t clientBase, int scanLimit) {
    using namespace cs2::offsets::client_dll;
    using namespace cs2::schema;
    std::vector<Entity> out;
    uintptr_t listBase = 0;
    if (probe.baseIsPtr) {
        auto lb = mem.readPtr(clientBase + dwEntityList);
        if (!lb) return out;
        listBase = *lb;
    } else {
        listBase = clientBase + dwEntityList;
    }
    for (int i = 0; i < scanLimit; ++i) {
        auto entry = mem.readPtr(listBase + i * probe.stride);
        if (!entry || *entry == 0) continue;
        auto ent = mem.readPtr(*entry + CEntityInstance::m_pEntity);
        if (!ent || *ent == 0) continue;
        auto sn = mem.readPtr(*ent + C_BaseEntity::m_pGameSceneNode);
        if (!sn || *sn == 0) continue;
        auto origin = readVec3(mem, *sn + CGameSceneNode::m_vecAbsOrigin);
        if (!origin || !saneOrigin(*origin)) continue;
        auto team = mem.read<int32_t>(*ent + C_BaseEntity::m_iTeamNum);
        auto health = mem.read<int32_t>(*ent + C_BaseEntity::m_iHealth);
        auto ls = mem.read<int32_t>(*ent + C_BaseEntity::m_lifeState);
        if (!team || !health || !ls || *team < 0 || *team > 3 ||
            *health < 0 || *health > 250 || *ls < 0 || *ls > 6)
            continue;
        Entity e;
        e.index = i;
        e.entity = *ent;
        e.sceneNode = *sn;
        e.origin = *origin;
        e.team = *team;
        e.health = *health;
        e.lifeState = *ls;
        auto mhp = mem.read<int32_t>(*ent + C_BaseEntity::m_iMaxHealth);
        if (mhp) e.maxHealth = *mhp;
        auto dorm = mem.read<uint8_t>(*sn + CGameSceneNode::m_bDormant);
        e.dormant = dorm ? *dorm != 0 : false;
        out.push_back(e);
    }
    return out;
}

} // namespace cs2esp
