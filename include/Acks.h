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
