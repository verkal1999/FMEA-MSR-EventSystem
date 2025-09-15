#pragma once
#include <any>
#include <chrono>
#include <string>

enum class EventType {
    evD2, evD1, evD3,
    evSRPlanned, evSRDone, evProcessFail,
    evMonActPlanned, evMonActDone,
    evKGResult, evKGTimeout,
    evIngestionPlanned, evIngestionDone,
    evMonActFinished, evSysReactFinished,
    evUnknownFM, evGotFM
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

struct PLCSnapshotPayload {
    std::string correlationId;   // vom Erzeuger vergeben (z. B. "evD2-...")
    std::string snapshotJson;    // z.B. {"rows":[...],"vals":{...},"processName":"..."}
};