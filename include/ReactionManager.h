#pragma once
#include "ReactiveObserver.h"
#include <memory>
#include <string>

class PLCMonitor; // Forward

class ReactionManager : public ReactiveObserver,
                    public std::enable_shared_from_this<ReactionManager> {
public:
    explicit ReactionManager(PLCMonitor& mon) : mon_(mon) {}

    // Einheitlicher Callback-Name
    void onMethod(const Event& ev) override;

private:
    PLCMonitor& mon_;
};
