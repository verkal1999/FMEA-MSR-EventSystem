#include "InventorySnapshotUtils.h"
#include <iostream>
#include "NodeIdUtils.h"

bool buildInventorySnapshotNow(PLCMonitor& mon, const std::string& root, InventorySnapshot& s) {
    s = InventorySnapshot{};
    mon.dumpPlcInventory(s.rows, root.c_str());                                        // :contentReference[oaicite:3]{index=3}
    for (const auto& ir : s.rows) {
        if (ir.nodeClass != "Variable") continue;
        uint16_t ns=4; std::string id; char type='?';
        if (!parseNsAndId(ir.nodeId, ns, id, type) || type!='s') continue;

        const bool isBool   = (ir.dtypeOrSig.find("Boolean") != std::string::npos);
        const bool isString = (ir.dtypeOrSig.find("String")  != std::string::npos) ||
                              (ir.dtypeOrSig.find("STRING")  != std::string::npos);
        const bool isI16    = (ir.dtypeOrSig.find("Int16")   != std::string::npos);
        const bool isF64    = (ir.dtypeOrSig.find("Double")  != std::string::npos);
        const bool isF32    = (ir.dtypeOrSig.find("Float")   != std::string::npos);

        NodeKey k; k.ns = ns; k.type='s'; k.id = id;

        if (isBool)   { bool v=false;        if (mon.readBoolAt  (id, ns, v)) s.bools.emplace(k, v); }       // :contentReference[oaicite:4]{index=4}
        if (isString) { std::string sv;      if (mon.readStringAt(id, ns, sv)) s.strings.emplace(k, sv); }    // :contentReference[oaicite:5]{index=5}
        if (isI16)    { UA_Int16 v=0;        if (mon.readInt16At (id, ns, v)) s.int16s.emplace(k, (int16_t)v);} // :contentReference[oaicite:6]{index=6}
        if (isF64)    { UA_Double v=0;       if (mon.readDoubleAt(id, ns, v)) s.floats.emplace(k, (double)v);} // :contentReference[oaicite:7]{index=7}
        if (isF32)    { UA_Float  v=0;       if (mon.readFloatAt (id, ns, v)) s.floats.emplace(k, (double)v);} // :contentReference[oaicite:8]{index=8}
    }
    return true;
}
