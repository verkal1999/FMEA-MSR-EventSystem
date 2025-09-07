#pragma once
#include <string>
#include <vector>

/// Primitive, aus denen ein Reaktionsplan besteht.
/// Du kannst hier später beliebig erweitern (CallMethod-Args als JSON, etc.).
enum class OpType {
    WriteBool,
    WriteInt32,
    CallMethod,
    WaitMs,
    ReadCheck,
    BlockResource,
    RerouteOrders,
    UnblockResource
};

/// Eine einzelne Operation (Schritt) in einem Plan.
struct Operation {
    OpType        type = OpType::WriteBool;
    std::string   nodeId;            // z. B. "DiagnoseFinished" oder "ns=1;s=Band_steuern"
    unsigned short ns   = 1;         // Namespace Index (Standard: 1)
    std::string   arg;               // WriteBool: "true"/"false"; WriteInt32: "42"; CallMethod: ggf. JSON
    int           timeoutMs = 0;     // für WaitMs / ReadCheck
};

/// Gesamter Reaktionsplan für eine Störung/Diagnose.
struct Plan {
    std::string              correlationId;    // für Acks/Tracing (z. B. "evD2-...")
    std::string              resourceId;       // betroffene Ressource (optional, aus KG)
    std::vector<Operation>   ops;              // sequenziell auszuführen
    bool                     abortRequired  = false;  // aus KG abgeleitet
    bool                     degradeAllowed = false;  // aus KG abgeleitet
};
