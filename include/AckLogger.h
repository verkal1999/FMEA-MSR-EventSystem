#pragma once
#include "ReactiveObserver.h"
#include "Acks.h"
#include <iostream>
#include <any>

class AckLogger : public ReactiveObserver {
public:
    void onMethod(const Event& ev) override {
        switch (ev.type) {
        case EventType::evReactionPlanned: {
            if (auto p = std::any_cast<ReactionPlannedAck>(&ev.payload)) {
                std::cout << "[AckLogger] PLANNED corr=" << p->correlationId
                          << " res=" << p->resourceId
                          << " summary=" << p->summary << "\n";
            }
            break;
        }
        case EventType::evReactionDone: {
            if (auto d = std::any_cast<ReactionDoneAck>(&ev.payload)) {
                std::cout << "[AckLogger] DONE    corr=" << d->correlationId
                          << " rc=" << d->rc
                          << " summary=" << d->summary << "\n";
            }
            break;
        }
        default: break;
        }
    }
};
