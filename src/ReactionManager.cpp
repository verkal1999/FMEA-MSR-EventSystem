#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <sstream>

#include <pybind11/embed.h>
#include "PythonWorker.h"

#include "ReactionManager.h"
#include "EventBus.h"
#include "Event.h"
#include "Acks.h"
#include "Correlation.h"
#include "CommandForce.h"
#include "CommandForceFactory.h"
#include "PLCMonitor.h"
#include "Plan.h"
#include <nlohmann/json.hpp>

namespace py = pybind11;
using json = nlohmann::json;

// ---------- Null-Stream für stummgeschaltete Logs ----------------------------
struct RM_NullBuf : std::streambuf { int overflow(int c) override { return c; } };
static RM_NullBuf   rm_nullbuf;
static std::ostream rm_nullout(&rm_nullbuf);

// ---------- Kleine Utils für Logs --------------------------------------------
static std::string truncStr(const std::string& s, size_t n=200) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...(" + std::to_string(s.size()) + "B)";
}
static std::string keyStr(const ReactionManager::NodeKey& k) {
    std::ostringstream oss;
    oss << "ns=" << k.ns << ";" << k.type << "=" << k.id;
    return oss.str();
}

// ---------- Freie Parser/JSON-Helfer (still & ohne RM-Logs) ------------------
// "ns=<n>;<t>=<id>" -> (ns, idType, idStr). Unterstützt Kurzform "OPCUA.*"
static bool parseNsAndId(const std::string& full,
                         UA_UInt16& nsOut,
                         std::string& idStrOut,
                         char& idTypeOut)
{
    nsOut = 4; idStrOut.clear(); idTypeOut = '?';

    if (full.rfind("OPCUA.", 0) == 0) { idTypeOut='s'; idStrOut=full; return true; }
    if (full.rfind("ns=", 0) != 0) return false;

    const size_t semi = full.find(';');
    if (semi == std::string::npos) return false;

    try { nsOut = static_cast<UA_UInt16>(std::stoi(full.substr(3, semi - 3))); }
    catch (...) { nsOut = 4; }

    if (semi + 2 >= full.size()) return false;
    idTypeOut = full[semi + 1];
    if (full[semi + 2] != '=') return false;

    idStrOut = full.substr(semi + 3);
    return true;
}
static json parseMaybeDoubleEncoded(const json& j, const char* /*where*/) {
    if (j.is_string()) { try { return json::parse(j.get<std::string>()); } catch (...) {} }
    return j;
}
static bool coerceRowObject(const json& in, json& out) {
    if (in.is_object()) { out = in; return true; }
    if (in.is_string()) {
        try {
            json j2 = json::parse(in.get<std::string>());
            if (j2.is_object()) {
                if (j2.contains("rows") && j2["rows"].is_array() && !j2["rows"].empty() && j2["rows"][0].is_object()) { out=j2["rows"][0]; return true; }
                out=j2; return true;
            }
            if (j2.is_array() && !j2.empty() && j2[0].is_object()) { out=j2[0]; return true; }
        } catch (...) {}
    }
    return false;
}

// ---------- ReactionManager ---------------------------------------------------
ReactionManager::ReactionManager(PLCMonitor& mon, EventBus& bus)
    : mon_(mon), bus_(bus)
{}

const char* ReactionManager::toCStr(LogLevel lvl) const {
    switch (lvl) {
        case LogLevel::Error:   return "ERR";
        case LogLevel::Warn:    return "WRN";
        case LogLevel::Info:    return "INF";
        case LogLevel::Debug:   return "DBG";
        case LogLevel::Trace:   return "TRC";
        case LogLevel::Verbose: return "VRB";
    }
    return "?";
}
std::ostream& ReactionManager::log(LogLevel lvl) const {
    if (isEnabled(lvl)) {
        std::cout << "[RM][" << toCStr(lvl) << "] ";
        return std::cout;
    }
    return rm_nullout;
}

bool ReactionManager::parseNodeId(const std::string& full, NodeKey& out) const {
    UA_UInt16 ns = 4; std::string id; char type='?';
    const bool ok = parseNsAndId(full, ns, id, type);
    log(ok ? LogLevel::Debug : LogLevel::Warn)
        << "parseNodeId in='" << full << "' -> "
        << (ok ? "OK " : "FAIL ")
        << "ns=" << ns << " type='" << type << "' id='" << id << "'\n";
    if (!ok) return false;
    out.ns = ns; out.type = type; out.id = id;
    return true;
}

ReactionManager::InventorySnapshot
ReactionManager::buildInventorySnapshot(const std::string& root) {
    log(LogLevel::Info) << "buildInventorySnapshot ENTER root='" << root << "'\n";

    InventorySnapshot s;
    const bool dumped = mon_.dumpPlcInventory(s.rows, root.c_str());
    log(LogLevel::Info) << "dumpPlcInventory=" << (dumped?"OK":"FAIL")
                        << " rows=" << s.rows.size() << "\n";

    size_t boolVars = 0, boolReadOk = 0;
    for (const auto& ir : s.rows) {
        if (ir.nodeClass != "Variable") continue;
        const bool isBool = (ir.dtypeOrSig.find("Boolean") != std::string::npos);
        if (!isBool) continue;

        ++boolVars;
        NodeKey k;
        if (!parseNodeId(ir.nodeId, k)) {
            log(LogLevel::Warn) << "[inv] parse FAIL nodeId=" << ir.nodeId << "\n";
            continue;
        }
        if (k.type != 's') {
            log(LogLevel::Trace) << "[inv] skip non-'s' idType for " << keyStr(k) << "\n";
            continue;
        }

        bool v=false;
        const bool ok = mon_.readBoolAt(k.id, k.ns, v);
        log(ok ? LogLevel::Debug : LogLevel::Warn)
            << "[inv] read " << keyStr(k) << " -> "
            << (ok ? (v?"true":"false") : "<read failed>") << "\n";

        if (ok) { s.bools.emplace(k, v); ++boolReadOk; }
    }

    log(LogLevel::Info) << "buildInventorySnapshot BOOL vars=" << boolVars
                        << " cached=" << boolReadOk << "\n";
    log(LogLevel::Info) << "buildInventorySnapshot EXIT\n";
    return s;
}

void ReactionManager::logBoolInventory(const InventorySnapshot& inv) const {
    log(LogLevel::Info) << "logBoolInventory ENTER count=" << inv.bools.size() << "\n";
    std::cout << "[Inventory] Aktuelle BOOL-Werte:\n";
    for (const auto& it : inv.bools) {
        const auto& k = it.first;
        std::cout << "  " << keyStr(k) << " = " << (it.second ? "true" : "false") << "\n";
    }
    log(LogLevel::Info) << "logBoolInventory EXIT\n\n";
}

std::vector<ReactionManager::KgExpect>
ReactionManager::normalizeKgResponse(const std::string& srows) {
    log(LogLevel::Info) << "normalizeKgResponse ENTER len=" << srows.size()
                        << " preview=\"" << truncStr(srows) << "\"\n";

    std::vector<KgExpect> out;

    json j;
    try { j = json::parse(srows); }
    catch (const std::exception& e) {
        log(LogLevel::Warn) << "normalizeKgResponse top parse FAIL: " << e.what() << "\n";
        j = json::object();
    }

    j = parseMaybeDoubleEncoded(j, "top");
    log(LogLevel::Debug) << "normalizeKgResponse top type=" << j.type_name() << "\n";

    json rows = json::array();
    if (j.contains("rows") && j["rows"].is_array()) {
        rows = j["rows"];
        log(LogLevel::Debug) << "rows[] found size=" << rows.size() << "\n";
    } else if (j.is_array()) {
        rows = j;
        log(LogLevel::Debug) << "top is array size=" << rows.size() << "\n";
    } else {
        rows = json::array({ j });
        log(LogLevel::Debug) << "single row promoted\n";
    }

    size_t added = 0, seen = 0;
    for (const auto& rowIn : rows) {
        ++seen;
        json row;
        const bool okC = coerceRowObject(rowIn, row);
        log(LogLevel::Trace) << "[row#" << seen << "] coerce=" << (okC?"OK":"FAIL")
                             << " type=" << row.type_name() << "\n";
        if (!okC) continue;

        std::string idFull = row.value("id", "");
        std::string t      = row.value("t", "");
        const bool hasV    = row.contains("v");
        log(LogLevel::Debug) << "[row#" << seen << "] id='" << truncStr(idFull,120)
                             << "' t='" << t << "' v=" << (hasV?row["v"].dump():"<none>") << "\n";

        // id könnte selbst JSON enthalten
        if (!idFull.empty() && idFull.front()=='{') {
            json idj = parseMaybeDoubleEncoded(json(idFull), "row.id");
            log(LogLevel::Trace) << "[row#" << seen << "] id unwrap type=" << idj.type_name() << "\n";
            if (idj.is_object()) {
                if (idj.contains("rows") && idj["rows"].is_array() && !idj["rows"].empty() && idj["rows"][0].is_object())
                    row = idj["rows"][0];
                else
                    row = idj;
                idFull = row.value("id", "");
                t      = row.value("t", t);
            }
        }

        if (idFull.empty() || t.empty() || !row.contains("v")) {
            log(LogLevel::Warn) << "[row#" << seen << "] skip: missing id/t/v\n";
            continue;
        }

        NodeKey key;
        if (!parseNodeId(idFull, key)) {
            log(LogLevel::Warn) << "[row#" << seen << "] skip: parseNodeId FAIL\n";
            continue;
        }

        if (t == "bool" || t == "boolean") {
            if (!row["v"].is_boolean()) {
                log(LogLevel::Warn) << "[row#" << seen << "] skip: v not boolean\n";
                continue;
            }
            const bool exp = row["v"].get<bool>();
            out.push_back(KgExpect{ key, KgValKind::Bool, exp });
            ++added;
            log(LogLevel::Debug) << "[row#" << seen << "] added BOOL expect "
                                 << keyStr(key) << " = " << (exp?"true":"false") << "\n";
        } else {
            log(LogLevel::Trace) << "[row#" << seen << "] skip: unsupported t='" << t << "' (only bool now)\n";
        }
    }

    log(LogLevel::Info) << "normalizeKgResponse EXIT expects=" << added << "\n";
    return out;
}

ReactionManager::ComparisonReport
ReactionManager::compareAgainstCache(const InventorySnapshot& inv,
                                     const std::vector<KgExpect>& ex) {
    log(LogLevel::Info) << "compareAgainstCache ENTER inv.bools=" << inv.bools.size()
                        << " expects=" << ex.size() << "\n";
    ComparisonReport rep;

    size_t idx = 0;
    for (const auto& e : ex) {
        ++idx;
        if (e.kind == KgValKind::Bool) {
            auto it = inv.bools.find(e.key);
            if (it == inv.bools.end()) {
                rep.items.push_back({ e.key, false, "kein Cachewert vorhanden" });
                rep.allOk = false;
                log(LogLevel::Warn) << "[cmp#" << idx << "] " << keyStr(e.key) << " -> NO CACHE\n";
                continue;
            }
            const bool cur = it->second;
            const bool ok  = (cur == e.expectedBool);
            rep.items.push_back({
                e.key, ok,
                std::string("ist=") + (cur ? "true" : "false")
                + ", soll=" + (e.expectedBool ? "true" : "false")
            });
            if (!ok) rep.allOk = false;

            log(LogLevel::Debug) << "[cmp#" << idx << "] " << keyStr(e.key)
                                 << " ist=" << (cur?"true":"false")
                                 << " soll=" << (e.expectedBool?"true":"false")
                                 << " -> " << (ok?"MATCH":"DIFF") << "\n";
        }
    }

    log(LogLevel::Info) << "compareAgainstCache EXIT allOk=" << (rep.allOk?"true":"false")
                        << " items=" << rep.items.size() << "\n";
    return rep;
}

Plan ReactionManager::buildPlanFromComparison(const std::string& corr,
                                              const ComparisonReport& rep) {
    log(LogLevel::Info) << "buildPlanFromComparison ENTER corr=" << corr
                        << " checksOk=" << (rep.allOk?"true":"false")
                        << " items=" << rep.items.size() << "\n";

    Plan plan;
    plan.correlationId = corr;
    plan.resourceId    = "Station";
    plan.ops.push_back(Operation{
        OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100
    });

    log(LogLevel::Info) << "buildPlanFromComparison EXIT ops=" << plan.ops.size() << "\n";
    return plan;
}

void ReactionManager::executePlanAndAck(const Plan& plan, bool checksOk) {
    using Clock = std::chrono::steady_clock;
    log(LogLevel::Info) << "executePlanAndAck ENTER corr=" << plan.correlationId
                        << " ops=" << plan.ops.size()
                        << " checksOk=" << (checksOk?"true":"false") << "\n";

    bus_.post(Event{
        EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{
            plan.correlationId, plan.resourceId,
            std::string("KG Checks: ") + (checksOk ? "OK" : "FAIL") + " -> DiagnoseFinished-Puls"
        } }
    });
    log(LogLevel::Debug) << "posted evReactionPlanned\n";

    auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
    const int rc = cf->execute(plan);
    log(LogLevel::Debug) << "CommandForce rc=" << rc << "\n";

    bus_.post(Event{
        EventType::evReactionDone, Clock::now(),
        std::any{ ReactionDoneAck{ plan.correlationId, rc, rc ? "OK" : "FAIL" } }
    });
    log(LogLevel::Debug) << "posted evReactionDone\n";
    log(LogLevel::Info)  << "executePlanAndAck EXIT\n";
}

void ReactionManager::onMethod(const Event& ev) {
    if (ev.type == EventType::evD2) {
        const auto corr = makeCorrelationId("evD2");
        log(LogLevel::Info) << "onMethod ENTER evD2 corr=" << corr << "\n";

        auto inv = buildInventorySnapshot("PLC1");
        logBoolInventory(inv);

        std::thread([this, corr, inv]() {
            log(LogLevel::Info) << "[thread] corr=" << corr << " START\n";
            std::string srows;
            try {
                log(LogLevel::Debug) << "[thread] calling PythonWorker...\n";
                srows = PythonWorker::instance().call([&]() -> std::string {
                    py::module_ sys = py::module_::import("sys");
                    py::list path = sys.attr("path").cast<py::list>();
                    path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

                    py::module_ kg  = py::module_::import("KG_Interface");
                    py::object cls  = kg.attr("KGInterface");
                    py::object kgi  = cls(
                        R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src\FMEA_KG.ttl)",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/class_",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/op_",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/dp_"
                    );
                    std::string interruptedSkill = "TestSkill1";
                    auto pyres = kgi.attr("getFailureModeParameters")(interruptedSkill);
                    return py::str(pyres);
                });
                log(LogLevel::Info) << "[thread] PythonWorker OK json_len=" << srows.size()
                                    << " preview=\"" << truncStr(srows) << "\"\n";
            } catch (const std::exception& e) {
                log(LogLevel::Error) << "[thread] KG error: " << e.what() << "\n";
                srows = R"({"rows":[]})";
            }

            auto expects = normalizeKgResponse(srows);
            auto rep     = compareAgainstCache(inv, expects);
            auto plan    = buildPlanFromComparison(corr, rep);
            executePlanAndAck(plan, rep.allOk);
            log(LogLevel::Info) << "[thread] corr=" << corr << " END\n";
        }).detach();

        log(LogLevel::Info) << "onMethod EXIT evD2 corr=" << corr << " (thread detached)\n";
        return;
    }
}
