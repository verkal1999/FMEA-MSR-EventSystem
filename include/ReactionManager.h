#pragma once
#include "ReactiveObserver.h"

class PLCMonitor;
class EventBus;

class ReactionManager : public ReactiveObserver {
public:
    ReactionManager(PLCMonitor& mon, EventBus& bus) : mon_(mon), bus_(bus) {}
    void onMethod(const Event& ev) override;

private:
    PLCMonitor& mon_;
    EventBus&   bus_;
};
