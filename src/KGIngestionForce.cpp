// KgIngestionForce.cpp (ersetzt/ergänzt)
#include "KgIngestionForce.h"
#include "Acks.h"
#include <pybind11/embed.h>
#include <iomanip>
#include <sstream>
#include "KGIngestionParams.h"
#include <iostream>

using json = nlohmann::json;

std::string KgIngestionForce::getStr(const UAValueMap& m, int idx) {
    auto it = m.find(idx);
    if (it == m.end()) return {};
    if (it->second.index() == 6) return std::get<std::string>(it->second);
    return {};
}

std::string KgIngestionForce::now_ts() {
    using clock = std::chrono::system_clock;
    auto t  = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

// snapshotJson hat in deinem Recorder die Struktur:
//   { rows:[...],
//     strings:[ {k:{ns,t,id}, v:"..."}, ... ],
//     bools:[...], int16s:[...], floats:[...] }
std::string KgIngestionForce::findStringInSnapshot(const json& snap, const char* nodeId) {
    if (!snap.contains("strings") || !snap["strings"].is_array()) return {};
    for (const auto& e : snap["strings"]) {
        if (!e.is_object()) continue;
        const auto& k = e.contains("k") ? e["k"] : json::object();
        if (k.contains("id") && k["id"].is_string() && k["id"].get<std::string>() == nodeId) {
            if (e.contains("v") && e["v"].is_string()) return e["v"].get<std::string>();
        }
    }
    return {};
}

KgIngestionForce::Parameters KgIngestionForce::buildParams(const Plan& p) {
    Parameters out;
    const auto& op = p.ops.front();

    out.corr       = p.correlationId.empty() ? getStr(op.inputs, 0) : p.correlationId;
    out.process    = getStr(op.inputs, 1);
    out.summary    = getStr(op.inputs, 2);
    out.resourceId = p.resourceId;

    const std::string snapJson = getStr(op.inputs, 3);
    out.snapshotWrapped = wrapSnapshot(snapJson);

    // Zeit + IndividualName
    out.ts             = now_ts();
    out.individualName = out.corr + "_" + out.ts;

    // lastSkill/lastProcess aus Snapshot (JSON parsing)
    json snap;
    try { snap = json::parse(snapJson); } catch (...) { snap = json::object(); }
    out.lastSkill   = findStringInSnapshot(snap, "OPCUA.lastExecutedSkill");
    out.lastProcess = findStringInSnapshot(snap, "OPCUA.lastExecutedProcess");

    // optionale Felder: sys/mon/failureModes als JSON-String in inputs[4..6]
    auto parseVec = [&](int idx) -> std::vector<std::string> {
        std::vector<std::string> v;
        const std::string s = getStr(op.inputs, idx);
        if (s.empty()) return v;
        try {
            json arr = json::parse(s);
            if (arr.is_array()) {
                for (auto& it : arr) if (it.is_string()) v.push_back(it.get<std::string>());
            }
        } catch (...) {}
        return v;
    };
    out.sysReacts    = parseVec(4);
    out.monReacts    = parseVec(5);
    out.failureModes = parseVec(6);

    return out;
}

int KgIngestionForce::execute(const Plan& p) {
    using Clock = std::chrono::steady_clock;
    if (p.ops.empty()) return 0;
    const auto& op = p.ops.front();

    // 1) Parameters aus attach (bevorzugt)
    std::shared_ptr<KgIngestionParams> prm;
    if (op.attach.has_value()) {
        if (auto spp = std::any_cast<std::shared_ptr<KgIngestionParams>>(&op.attach)) {
            prm = *spp;
        }
    }

    // 2) Fallback: aus inputs rekonstruieren (alt-kompatibel)  :contentReference[oaicite:4]{index=4}
    KgIngestionParams tmp;
    if (!prm) {
        tmp.corr      = p.correlationId.empty() ? getStr(op.inputs, 0) : p.correlationId;
        tmp.process   = getStr(op.inputs, 1);
        tmp.summary   = getStr(op.inputs, 2);
        tmp.snapshotWrapped = getStr(op.inputs, 3); // evtl. bereits "==InventorySnapshot==...=="
        tmp.resourceId = p.resourceId;
        prm = std::make_shared<KgIngestionParams>(std::move(tmp));
    }

     // 3) Ack: PLANNED (neu: ingestion-spezifisch)
    bus_.post({ EventType::evIngestionPlanned, Clock::now(),
        std::any{ IngestionPlannedAck{
            prm->corr,
            prm->individualName,
            prm->lastProcess.empty() ? prm->process : prm->lastProcess,
            "OccurredFailure ingestion (prepared in C++)"
        } }
    });

    // 4) Statt Python: Parameter hübsch ausgeben
    std::cout << "[KGIngestion] params=\n" << prm->toJson().dump(2) << "\n";

    // „Erfolg“ simulieren
    const bool ok = true;

    // 5) DONE (neu: ingestion-spezifisch)
    bus_.post({ EventType::evIngestionDone, Clock::now(),
        std::any{ IngestionDoneAck{ prm->corr, ok ? 1 : 0, ok ? "printed params" : "error" } } });

    return ok ? 1 : 0;
}
