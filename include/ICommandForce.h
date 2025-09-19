#pragma once
#include "Plan.h"

struct ICommandForce {
    virtual ~ICommandForce() = default;
    // synchroner Ablauf; Rückgabe 1 = OK, 0 = Fehler
    virtual int execute(const Plan& p) = 0;
};