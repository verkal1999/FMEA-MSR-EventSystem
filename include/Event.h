#pragma once
#include <any>
#include <chrono>
#include <string>

enum class EventType : unsigned {
    evD2 = 1,
    evReactionPlanned,   // Plan erstellt (vor Ausführung)
    evReactionDone,      // Plan ausgeführt (Ergebnis/Fazit)
    // optional: evFailureDetected, evActionProgress, ...
};

struct Event {
    EventType type{};
    std::chrono::steady_clock::time_point ts{ std::chrono::steady_clock::now() };
    std::any payload; // typisierte Payloads (siehe Acks.h)
};
