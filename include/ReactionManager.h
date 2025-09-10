#pragma once
#include "ReactiveObserver.h"
#include <mutex>
#include <unordered_set>

class PLCMonitor;
class EventBus;

class ReactionManager : public ReactiveObserver {
public:
    ReactionManager(PLCMonitor& mon, EventBus& bus);
    void onMethod(const Event& ev) override;

private:
    PLCMonitor& mon_;
    EventBus&   bus_;
    std::mutex corrMx_;
    std::unordered_set<std::string> corrDone_; // enthält Korrelationen, die bereits final sind

    bool markDone_(const std::string& id) {
        std::lock_guard<std::mutex> lk(corrMx_);
        return corrDone_.insert(id).second; // true, falls neu eingetragen
    }
    bool isDone_(const std::string& id) {
        std::lock_guard<std::mutex> lk(corrMx_);
        return corrDone_.count(id) != 0;
    }
};
