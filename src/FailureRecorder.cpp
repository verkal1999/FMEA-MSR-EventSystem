#include "FailureRecorder.h"
#include "Acks.h"
#include "KgIngestionForce.h"          // direkt instanziieren; alternativ Factory-Erweiterung
#include <nlohmann/json.hpp>
#include <iostream>
#include "CommandForceFactory.h"

using nlohmann::json;

void FailureRecorder::subscribeAll() {
    auto self = shared_from_this();
    // hört wirklich auf alles, reagiert aber nur wo nötig
    bus_.subscribe(EventType::evD1,              self, 3);
    bus_.subscribe(EventType::evD2,              self, 3);  // hier kommt unser Snapshot-Payload
    bus_.subscribe(EventType::evD3,              self, 3);
    bus_.subscribe(EventType::evReactionPlanned, self, 3);
    bus_.subscribe(EventType::evReactionDone,    self, 3);
    bus_.subscribe(EventType::evProcessFail,     self, 3);  // hier triggern wir die Force
    bus_.subscribe(EventType::evKGResult,        self, 3);
    bus_.subscribe(EventType::evKGTimeout,       self, 3);  // optional als "Fehler" verbuchen
}

void FailureRecorder::onEvent(const Event& ev) {
    if (ev.type == EventType::evD2) {
        // Neuer Payload: typisierter Snapshot
        if (auto p = std::any_cast<D2Snapshot>(&ev.payload)) {
            // in JSON konvertieren und lokal vormerken
            const std::string corr = p->correlationId;
            const std::string js   = snapshotToJson(p->inv).dump();

            std::lock_guard<std::mutex> lk(mx_);
            snapshotJsonByCorr_[corr] = js;
        }
        return;
    }

    if (ev.type == EventType::evProcessFail) {
        if (auto a = std::any_cast<ProcessFailAck>(&ev.payload)) {
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (auto it = snapshotJsonByCorr_.find(a->correlationId); it != snapshotJsonByCorr_.end())
                    snap = it->second;
            }
            Plan plan = makeKgIngestionPlan(a->correlationId, a->processName, a->summary, snap);

            // → Factory entscheiden lassen (siehe Punkt 3)
            auto cf = CommandForceFactory::createForOp(plan.ops.front(), /*mon*/nullptr, bus_);
            if (cf) (void)cf->execute(plan);
        }
        return;
    }
    if (ev.type == EventType::evKGTimeout) {
        // optional auch Timeouts aufnehmen
        if (auto t = std::any_cast<ProcessFailAck>(&ev.payload)) {
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (auto it = snapshotJsonByCorr_.find(t->correlationId); it != snapshotJsonByCorr_.end())
                    snap = it->second;
            }
            Plan plan = makeKgIngestionPlan(t->correlationId, t->processName, t->summary, snap);

            // → Factory entscheiden lassen (siehe Punkt 3)
            auto cf = CommandForceFactory::createForOp(plan.ops.front(), /*mon*/nullptr, bus_);
            if (cf) (void)cf->execute(plan);
        }
        return;
    }

    if (ev.type == EventType::evReactionDone) {
        // Aufräumen ist optional – wenn gewünscht, hier löschen:
        if (auto d = std::any_cast<ReactionDoneAck>(&ev.payload)) {
            std::lock_guard<std::mutex> lk(mx_);
            snapshotJsonByCorr_.erase(d->correlationId);
        }
        return;
    }
}

// -------- helpers --------

json FailureRecorder::snapshotToJson(const InventorySnapshot& inv) {
    auto keyToJ = [](const NodeKey& k){
        return json{{"ns",k.ns},{"t",std::string(1,k.type)},{"id",k.id}};
    };

    json j;
    // Rows (Metadaten über die Knoten)
    j["rows"] = json::array();
    for (const auto& r : inv.rows) {
        j["rows"].push_back({
            {"nodeClass", r.nodeClass},
            {"id",        r.nodeId},
            {"t",         r.dtypeOrSig}
        });
    }

    // Werte, typgruppiert
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

Plan FailureRecorder::makeKgIngestionPlan(const std::string& corr,
                                          const std::string& process,
                                          const std::string& summary,
                                          const std::string& snapshotJson)
{
    Plan p;
    p.correlationId = corr;
    p.resourceId    = "KG";

    Operation op;
    op.type = OpType::KGIngestion;    // <— wichtig: dein neuer Typ
    op.inputs[0] = corr;
    op.inputs[1] = process;
    op.inputs[2] = summary;
    op.inputs[3] = snapshotJson;      // JSON des InventorySnapshot
    p.ops.push_back(std::move(op));
    return p;
}