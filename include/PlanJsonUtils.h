#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "Plan.h"
#include "common_types.h"

// --- Kleine Utils, auch praktisch für Logs ---
nlohmann::json uaValueToJson(const UAValue& v);
nlohmann::json uaMapToJson(const UAValueMap& m);

// --- Parser-Helpers (öffentlich, damit überall nutzbar) ---
std::string    fixParamsRawIfNeeded(std::string s);
UAValue        parseUAValueFromTypeTag(const std::string& t, const nlohmann::json& v);
void           assignTyped(UAValueMap& target, int idx, const std::string& t, const nlohmann::json& v);

// --- Hauptfunktion: JSON-Payload -> Plan (CallMethod-Only)
// * akzeptiert top-level array, { rows: [...] } sowie { sysReactions/monReactions: [{ rows: [...] }] }
// * optionaler DiagnoseFinished-Puls via appendPulse
Plan buildCallMethodPlanFromPayload(const std::string& corr,
                                    const std::string& payload,
                                    bool appendPulse = true,
                                    const std::string& resourceId = "Station");
