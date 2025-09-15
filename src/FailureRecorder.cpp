#include "FailureRecorder.h"
#include "Acks.h"
#include "CommandForceFactory.h"
#include "CommandForce.h"          // vollständige ICommandForce-Definition
#include <iomanip>
#include <sstream>

using nlohmann::json;

// ---------- subscriptions ----------
void FailureRecorder::subscribeAll() {
    auto self = shared_from_this();
    bus_.subscribe(EventType::evD2,               self, 3);
    bus_.subscribe(EventType::evMonActFinished,   self, 3); // Executed MonActions
    bus_.subscribe(EventType::evSysReactFinished, self, 3); // Executed SysReacts
    bus_.subscribe(EventType::evProcessFail,      self, 3); // Trigger Ingestion
    bus_.subscribe(EventType::evSRDone,           self, 3); // Trigger Ingestion
    bus_.subscribe(EventType::evKGTimeout,        self, 3); // Trigger Ingestion
    bus_.subscribe(EventType::evIngestionDone,    self, 3); // Cleanup
}

bool FailureRecorder::tryMarkIngestion(const std::string& corr) {
    std::lock_guard<std::mutex> lk(mx_);
    auto [it, inserted] = ingestionStarted_.insert(corr);
    return inserted; // true = first time, false = already triggered
}

// ---------- small helpers ----------
std::string FailureRecorder::now_ts() {
    using clock = std::chrono::system_clock;
    auto t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss; oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}
std::string FailureRecorder::wrapSnapshot(const std::string& js) {
    return "==InventorySnapshot==" + js + "==InventorySnapshot==";
}
std::string FailureRecorder::findStringInSnap(const json& snap, const char* nodeId) {
    if (!snap.contains("vars") || !snap["vars"].is_array()) return {};
    for (const auto& e : snap["vars"]) {
        if (e.is_object()
            && e.contains("id") && e["id"].is_string()
            && e["id"].get<std::string>() == nodeId
            && e.contains("t") && e.contains("v")
            && e["t"].is_string() && e["t"].get<std::string>() == "string"
            && e["v"].is_string())
            return e["v"].get<std::string>();
    }
    return {};
}
FailureRecorder::json FailureRecorder::snapshotToJson_flat(const InventorySnapshot& inv) {
    auto add = [](json& arr, const std::string& id, const char* t, const json& v) {
        arr.push_back(json{{"id",id},{"t",t},{"v",v}});
    };
    json out;
    out["rows"] = json::array();
    for (const auto& r : inv.rows)
        out["rows"].push_back({{"id",r.nodeId},{"t",r.dtypeOrSig},{"nodeClass",r.nodeClass}});
    out["vars"] = json::array();
    for (const auto& [k,v] : inv.bools)   add(out["vars"], k.id, "bool",   v);
    for (const auto& [k,v] : inv.strings) add(out["vars"], k.id, "string", v);
    for (const auto& [k,v] : inv.int16s)  add(out["vars"], k.id, "int16",  v);
    for (const auto& [k,v] : inv.floats)  add(out["vars"], k.id, "float",  v);
    return out;
}

// (legacy helper; falls anderswo gebraucht)
FailureRecorder::json FailureRecorder::snapshotToJson(const InventorySnapshot& inv) {
    auto keyToJ = [](const NodeKey& k){
        return json{{"ns",k.ns},{"t",std::string(1,k.type)},{"id",k.id}};
    };
    json j;
    j["rows"] = json::array();
    for (const auto& r : inv.rows) {
        j["rows"].push_back({
            {"nodeClass", r.nodeClass},
            {"id",        r.nodeId},
            {"t",         r.dtypeOrSig}
        });
    }
    j["bools"]   = json::array();
    for (const auto& [k,v] : inv.bools)   j["bools"].push_back(  {{"k",keyToJ(k)},{"v",v}} );
    j["strings"] = json::array();
    for (const auto& [k,v] : inv.strings) j["strings"].push_back({{"k",keyToJ(k)},{"v",v}} );
    j["int16s"]  = json::array();
    for (const auto& [k,v] : inv.int16s)  j["int16s"].push_back( {{"k",keyToJ(k)},{"v",v}} );
    j["floats"]  = json::array();
    for (const auto& [k,v] : inv.floats)  j["floats"].push_back( {{"k",keyToJ(k)},{"v",v}} );
    return j;
}

// baut ein fertiges Param-Objekt (holt Snapshot + lastSkill/lastProcess + Reaktions-IRIs)
std::shared_ptr<KgIngestionParams>
FailureRecorder::buildParams(const std::string& corr, const std::string& process, const std::string& summary)
{
    KgIngestionParams prm;
    prm.corr       = corr;
    prm.process    = process;
    prm.summary    = summary;
    prm.resourceId = "KG";
    prm.ts         = now_ts();
    prm.individualName = prm.corr + "_" + prm.ts;

    std::string snap;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (auto it = snapshotJsonByCorr_.find(corr); it != snapshotJsonByCorr_.end())
            snap = it->second;
        // Reaktionslisten, falls schon vorhanden:
        if (auto it = monReactsByCorr_.find(corr); it != monReactsByCorr_.end())
            prm.monReactions = it->second;
        if (auto it = sysReactsByCorr_.find(corr); it != sysReactsByCorr_.end())
            prm.sysReactions = it->second;
    }
    prm.snapshotWrapped = wrapSnapshot(snap);

    try {
        json j = snap.empty() ? json::object() : json::parse(snap);
        prm.lastSkill   = findStringInSnap(j, "OPCUA.lastExecutedSkill");
        prm.lastProcess = findStringInSnap(j, "OPCUA.lastExecutedProcess");
    } catch (...) {}

    return std::make_shared<KgIngestionParams>(std::move(prm));
}

// Force starten (Factory entscheidet anhand OpType)
void FailureRecorder::startIngestionWith(std::shared_ptr<KgIngestionParams> prm) {
    Plan p; p.correlationId = prm->corr; p.resourceId = prm->resourceId;
    Operation op; op.type = OpType::KGIngestion;
    op.attach = prm; // getyptes Cargo bevorzugt
    // Fallback für (ältere) Implementierungen:
    op.inputs[0] = prm->corr;
    op.inputs[1] = prm->process;
    op.inputs[2] = prm->summary;
    op.inputs[3] = prm->snapshotWrapped;
    p.ops.push_back(std::move(op));

    if (auto cf = CommandForceFactory::createForOp(p.ops.front(), /*mon*/nullptr, bus_))
        (void)cf->execute(p);
}

// ---------- zentrales Event-Handling ----------
void FailureRecorder::onEvent(const Event& ev) {
    switch (ev.type) {
    case EventType::evD2: {
        if (auto p = std::any_cast<D2Snapshot>(&ev.payload)) {
            const std::string corr = p->correlationId;
            const std::string js   = snapshotToJson_flat(p->inv).dump();
            std::lock_guard<std::mutex> lk(mx_);
            snapshotJsonByCorr_[corr] = js;
        }
        break;
    }
    case EventType::evMonActFinished: {
        if (auto a = std::any_cast<MonActFinishedAck>(&ev.payload)) {
            std::lock_guard<std::mutex> lk(mx_);
            monReactsByCorr_[a->correlationId] = a->skills; // wenn leer => absichtlich leer
        }
        break;
    }
    case EventType::evSysReactFinished: {
        if (auto a = std::any_cast<SysReactFinishedAck>(&ev.payload)) {
            std::lock_guard<std::mutex> lk(mx_);
            sysReactsByCorr_[a->correlationId] = a->skills; // wenn leer => absichtlich leer
        }
        break;
    }

    // --- Trigger: Ingestion starten ---
    case EventType::evProcessFail: {
        if (auto a = std::any_cast<ProcessFailAck>(&ev.payload)) {
            if (!tryMarkIngestion(a->correlationId)) break; // schon gestartet
            auto prm = buildParams(a->correlationId, a->processName, a->summary);
            startIngestionWith(std::move(prm));
        }
        break;
    }
    case EventType::evSRDone: {
        if (auto d = std::any_cast<ReactionDoneAck>(&ev.payload)) {
            if (!tryMarkIngestion(d->correlationId)) break; // schon gestartet
            auto prm = buildParams(d->correlationId, "SystemReaction",
                                   std::string("SRDone: ") + (d->rc ? "OK" : "FAIL"));
            if (!prm->lastProcess.empty())
                prm->process = prm->lastProcess;
            startIngestionWith(std::move(prm));
        }
        break;
    }
    case EventType::evKGTimeout: {
        if (auto t = std::any_cast<KGTimeoutPayload>(&ev.payload)) {
            if (!tryMarkIngestion(t->correlationId)) break; // schon gestartet
            auto prm = buildParams(t->correlationId, "KG", "Knowledge Graph timeout");
            startIngestionWith(std::move(prm));
        }
        break;
    }

    // --- Cleanup NUR nach Ingestion ---
    case EventType::evIngestionDone: {
        if (auto d = std::any_cast<IngestionDoneAck>(&ev.payload)) {
            std::lock_guard<std::mutex> lk(mx_);
            snapshotJsonByCorr_.erase(d->correlationId);
            monReactsByCorr_.erase(d->correlationId);
            sysReactsByCorr_.erase(d->correlationId);
            ingestionStarted_.erase(d->correlationId);
        }
        break;
    }
    default:
        break;
    }
}
