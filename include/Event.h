#pragma once
#include <any>
#include <chrono>

enum class EventType {
    evD2, // wird ausgelöst bei TriggerD2 = TRUE
    // ... weitere Events hier ergänzen
};

struct Event {
    EventType type;
    std::chrono::steady_clock::time_point ts{};
    std::any payload; // optional: z. B. Messwert, NodeId, Kontext
};
