#pragma once
#include <any>
#include <chrono>
#include <string>

enum class EventType {
    evD2, evD1, evD3,
    evReactionPlanned, evReactionDone,
    evKGResult, evKGTimeout
};

struct Event {
    EventType type{};
    std::chrono::steady_clock::time_point ts{ std::chrono::steady_clock::now() };
    std::any payload; // typisierte Payloads (siehe Acks.h)
};

struct KGResultPayload {
    std::string correlationId;
    std::string rowsJson;
    bool ok;
};

struct KGTimeoutPayload {
    std::string correlationId;
};