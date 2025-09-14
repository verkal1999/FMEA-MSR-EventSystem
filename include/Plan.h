#pragma once
#include <string>
#include <vector>
#include <map>               // neu
#include "common_types.h"    // UAValue, UAValueMap (neu)

/// Primitive, aus denen ein Reaktionsplan besteht.
enum class OpType {
    WriteBool,
    PulseBool,
    WriteInt32,
    // Aufrufe (semantisch getrennt):
    CallMethod,             // generischer Method-Call (falls genutzt)
    CallMonitoringActions,  // <— NEU: MonActionForce-Domain
    CallSystemReaction,     // <— NEU: SystemReactionForce-Domain
    WaitMs,
    ReadCheck,
    BlockResource,
    RerouteOrders,
    UnblockResource,
    KGIngestion             // KG-Ingestion (nicht-PLC)
};

/// Eine einzelne Operation (Schritt) in einem Plan.
struct Operation {
    OpType type;

    // Allgemein (für Write*/ReadCheck):
    std::string   nodeId;                 // z. B. "DiagnoseFinished"
    unsigned short ns   = 1;              // Namespace Index

    // ---- CallMethod-spezifisch ----
    std::string   callObjNodeId;          // Object-NodeId (StringId)
    std::string   callMethNodeId;         // Method-NodeId (StringId)
    unsigned short callNsObj  = 0;        // 0 = "nicht gesetzt" -> 'ns' nutzen
    unsigned short callNsMeth = 0;        // 0 = "nicht gesetzt" -> 'ns' nutzen

    // Typisierte Argumente (nur für CallMethod genutzt):
    UAValueMap    inputs;                 // index -> UAValue (bool,int16,int32,float,double,string)
    UAValueMap    expOuts;                // erwartete Outputs (index -> UAValue)

    // Legacy/sonstige:
    // WriteBool:  "true"/"false"
    // WriteInt32: "42"
    // CallMethod: wird künftig ignoriert (nur noch inputs/expOuts)
    std::string   arg;

    // Zeitfeld:
    // WaitMs:      Wartezeit
    // PulseBool:   Pulsbreite
    // CallMethod:  Call-Timeout (Client->config->timeout)
    int           timeoutMs = 0;
};

/// Gesamter Reaktionsplan …
struct Plan {
    std::string              correlationId;
    std::string              resourceId;
    std::vector<Operation>   ops;
    bool                     abortRequired  = false;
    bool                     degradeAllowed = false;
};
