#pragma once
#include "ReactiveObserver.h"
#include "EventBus.h"
#include "PLCMonitor.h"
#include "Plan.h"
#include <memory>
#include <string>

class FailureRecorder : public ReactiveObserver,
                        public std::enable_shared_from_this<FailureRecorder> {
public:
    FailureRecorder(PLCMonitor& mon, EventBus& bus)
        : mon_(mon), bus_(bus) {}

    void onEvent(const Event& ev) override;

    // in main() mehrfach aufrufen: ein Abo je EventType
    void subscribeAll();

private:
    PLCMonitor& mon_;
    EventBus&   bus_;

    // sammelt Screenshot-Pfade: bevorzugt via KG-Cache, sonst Dateisystem-Fallback
    std::string getScreenshotsJson(const std::string& correlationId);

    // baut „Plan“ für KG-Ingestion (reiner String-Transport via inputs[0..3])
    static Plan makeKgIngestionPlan(const std::string& corr,
                                    const std::string& process,
                                    const std::string& summary,
                                    const std::string& screenshotsJson);
};
