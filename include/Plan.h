#pragma once
#include <string>
#include <vector>

/// Primitive, aus denen ein Reaktionsplan besteht.
enum class OpType {
    WriteBool,
    PulseBool,
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
    OpType        type = OpType::CallMethod;

    // Allgemein (für Write*/ReadCheck):
    // NodeId als StringId (wie bei UA_NODEID_STRING[_ALLOC]); Namespace separat.
    std::string   nodeId;                 // z. B. "DiagnoseFinished" (ohne "ns=" Präfix)
    unsigned short ns   = 1;              // Namespace Index (Standard: 1)

    // ---- CallMethod-spezifisch ----
    // Falls type == CallMethod:
    // - callObjNodeId / callMethNodeId werden verwendet
    // - callNsObj / callNsMeth erlauben bei Bedarf verschiedene Namespaces
    // Wenn nicht gesetzt, kannst du in der Ausführung auf 'ns' zurückfallen.
    std::string   callObjNodeId;          // Object-NodeId (StringId)
    std::string   callMethNodeId;         // Method-NodeId (StringId)
    unsigned short callNsObj  = 0;        // 0 = "nicht gesetzt" -> beim Ausführen auf 'ns' zurückfallen
    unsigned short callNsMeth = 0;        // 0 = "nicht gesetzt" -> beim Ausführen auf 'ns' zurückfallen

    // Argument-Payload:
    // WriteBool:  "true"/"false"
    // WriteInt32: "42"
    // CallMethod: Key=Value-Paare ("x=7;mode=fast") ODER JSON (wenn du magst)
    std::string   arg;

    // Zeitfeld:
    // WaitMs:      Wartezeit
    // PulseBool:   Pulsbreite
    // CallMethod:  Call-Timeout (Client->config->timeout)
    int           timeoutMs = 0;
};

/// Gesamter Reaktionsplan für eine Störung/Diagnose.
struct Plan {
    std::string              correlationId;    // für Acks/Tracing (z. B. "evD2-...")
    std::string              resourceId;       // optional (aus KG)
    std::vector<Operation>   ops;              // sequenziell auszuführen
    bool                     abortRequired  = false;
    bool                     degradeAllowed = false;
};
