// InventorySnapshot.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "PLCMonitor.h"  // für PLCMonitor::InventoryRow

struct NodeKey {
    uint16_t    ns   = 4;
    char        type = 's';     // 's'|'i'|'g'|'b'
    std::string id;             // e.g. "OPCUA.bool1"
    bool operator==(const NodeKey& o) const {
        return ns == o.ns && type == o.type && id == o.id;
    }
};
struct NodeKeyHash {
    size_t operator()(const NodeKey& k) const noexcept {
        return std::hash<uint16_t>{}(k.ns)
             ^ (std::hash<char>{}(k.type) << 1)
             ^ (std::hash<std::string>{}(k.id) << 2);
    }
};

// Ex RM: typisierter Snapshot
struct InventorySnapshot {
    std::vector<PLCMonitor::InventoryRow> rows;
    std::unordered_map<NodeKey, bool,        NodeKeyHash> bools;
    std::unordered_map<NodeKey, std::string, NodeKeyHash> strings;
    std::unordered_map<NodeKey, int16_t,     NodeKeyHash> int16s;
    std::unordered_map<NodeKey, double,      NodeKeyHash> floats;
};

// Payload-Typ für evD2 (Correlation + Snapshot)
struct D2Snapshot {
    std::string        correlationId;
    InventorySnapshot  inv;
};