#pragma once
#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>

#include "ReactiveObserver.h"
#include "EventBus.h"
#include "Plan.h"
#include <nlohmann/json.hpp>

// dein gemeinsamer Typ-Header mit Snapshot + Payload
#include "InventorySnapshot.h"   // enthält: NodeKey, InventorySnapshot, D2Snapshot

class FailureRecorder : public ReactiveObserver,
                        public std::enable_shared_from_this<FailureRecorder> {
public:
    explicit FailureRecorder(EventBus& bus) : bus_(bus) {}

    // bequem in main() aufrufen, um auf "alle" Events zu lauschen
    void subscribeAll();

    // zentrales Callback
    void onEvent(const Event& ev) override;

private:
    EventBus& bus_;

    // Nur lokal im Recorder, kein globaler Store:
    std::mutex mx_;
    std::unordered_map<std::string, std::string> snapshotJsonByCorr_; // corr -> JSON

    static nlohmann::json snapshotToJson(const InventorySnapshot& inv);

    static Plan makeKgIngestionPlan(const std::string& corr,
                                    const std::string& process,
                                    const std::string& summary,
                                    const std::string& snapshotJson);
};
