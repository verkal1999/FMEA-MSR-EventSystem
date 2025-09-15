#pragma once
#include <string>
#include <vector>

struct ReactionPlannedAck {
    std::string correlationId;     // für Tracing (gleich bleibt über alle Acks)
    std::string resourceId;        // betroffene Ressource (optional)
    std::string summary;           // kurze Beschreibung des Plans
    // optional: Liste der Operationen etc.
};

struct ReactionDoneAck {
    std::string correlationId;
    int rc = 0;                    // 1=OK, 0=Fehler
    std::string summary;           // was wurde getan / Ergebnis
};

struct ProcessFailAck {
    std::string correlationId;
    std::string processName;                  // 1=OK, 0=Fehler
    std::string summary;           // was wurde getan / Ergebnis
};

struct IngestionPlannedAck {
    std::string correlationId;
    std::string individualName;   // corr_ts
    std::string process;          // lastExecutedProcess (aus Param)
    std::string summary;          // kurze Beschreibung
};

struct IngestionDoneAck {
    std::string correlationId;
    int rc = 0;                   // 1=OK, 0=FAIL
    std::string message;          // z.B. "printed params"
};

struct MonActFinishedAck {
    std::string correlationId;
    std::vector<std::string> skills;       // IRIs der ausgeführten Monitoring-Actions
};
struct SysReactFinishedAck {
    std::string correlationId;
    std::vector<std::string> skills;       // IRIs der ausgeführten System-Reactions
};
struct UnknownFMAck {
    std::string correlationId;
    std::string processName;   // z.B. "UnknownFM" oder letzter Prozess
    std::string summary;       // kurze Erklärung ("KG: no failure modes for <skill>")
};
struct GotFMAck {
    std::string correlationId;
    std::string failureModeName;   // z.B. "UnknownFM" oder letzter Prozess      // kurze Erklärung ("KG: no failure modes for <skill>")
};

