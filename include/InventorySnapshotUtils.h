#pragma once
#include "InventorySnapshot.h"
#include "PLCMonitor.h"
#include <string>
#include <iostream>
#include <cstdint>
#include <sstream>

// identisch zur RM-Logik, nur als freie Funktion
bool buildInventorySnapshotNow(PLCMonitor& mon,
                               const std::string& root,
                               InventorySnapshot& out);

inline std::string nodeKeyToStr(const NodeKey& k) {
    std::ostringstream os;
    os << "ns=" << k.ns << ";type=" << k.type << ";id=\"" << k.id << "\"";
    return os.str();
}

inline void dumpInventorySnapshot(const InventorySnapshot& inv, std::ostream& os = std::cout) {
    os << "\n=== InventorySnapshot ===\n"
       << "rows="    << inv.rows.size()
       << " bools="  << inv.bools.size()
       << " strings="<< inv.strings.size()
       << " int16s=" << inv.int16s.size()
       << " floats=" << inv.floats.size() << "\n";

    os << "-- rows (NodeClass | NodeId | DType/Signature)\n";
    for (const auto& r : inv.rows)
        os << "  " << r.nodeClass << " | " << r.nodeId << " | " << r.dtypeOrSig << "\n";

    os << "-- bools\n";
    for (const auto& [k,v] : inv.bools)   os << "  " << nodeKeyToStr(k) << " = " << (v ? "true":"false") << "\n";
    os << "-- strings\n";
    for (const auto& [k,v] : inv.strings) os << "  " << nodeKeyToStr(k) << " = \"" << v << "\"\n";
    os << "-- int16s\n";
    for (const auto& [k,v] : inv.int16s)  os << "  " << nodeKeyToStr(k) << " = " << v << "\n";
    os << "-- floats\n";
    for (const auto& [k,v] : inv.floats)  os << "  " << nodeKeyToStr(k) << " = " << v << "\n";
    os << "=== /InventorySnapshot ===\n";
}