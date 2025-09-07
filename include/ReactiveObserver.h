#pragma once
#include "Event.h"

class ReactiveObserver {
public:
    virtual ~ReactiveObserver() = default;
    // Einheitlicher Callback-Name wie gewünscht:
    virtual void onMethod(const Event& ev) = 0;
};
