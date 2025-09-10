
#include <iostream>
#include <thread>
#include <pybind11/embed.h>
#include "PythonWorker.h"
#include "ReactionManager.h"
#include "EventBus.h"
#include "Event.h"
#include "Acks.h"
#include "Correlation.h"
#include <chrono>
#include "Plan.h"
#include <nlohmann/json.hpp>
#include "CommandForce.h"       // deine umbenannte Klasse (statt UAWriteTaskForce)
#include "CommandForceFactory.h"   // falls du die Factory nutzt
#include "PLCMonitor.h"
#include <unordered_map>

namespace py = pybind11;

ReactionManager::ReactionManager(PLCMonitor& mon, EventBus& bus)
    : mon_(mon), bus_(bus)    
{}

// ---------------------------------------------------------
// Debug-Helfer: NodeId "ns=<n>;<t>=<id>" -> (ns, typeChar, idStr)
// Unterstützt Kurzform "OPCUA.*" (typisch s=) und volle Formen i|s|g|b
// ---------------------------------------------------------
static bool parseNsAndId(const std::string& full,
                         UA_UInt16& nsOut,
                         std::string& idStrOut,
                         char& idTypeOut)
{
    nsOut = 4;
    idStrOut.clear();
    idTypeOut = '?';

    std::cout << "[parseNsAndId] in: '" << full << "'\n";

    // Kurzform ohne "ns=" -> behandle als s= im Default-NS
    if (full.rfind("OPCUA.", 0) == 0) {
        idTypeOut = 's';
        idStrOut  = full;
        std::cout << "[parseNsAndId] short-form -> ns=" << nsOut
                  << " type='" << idTypeOut << "' id='" << idStrOut << "'\n";
        return true;
    }

    // Erwartet "ns=<num>;<t>=<id>"
    if (full.rfind("ns=", 0) != 0) {
        std::cout << "[parseNsAndId][FAIL] no 'ns=' prefix\n";
        return false;
    }

    const size_t semi = full.find(';');
    if (semi == std::string::npos) {
        std::cout << "[parseNsAndId][FAIL] missing ';'\n";
        return false;
    }

    try {
        nsOut = static_cast<UA_UInt16>(std::stoi(full.substr(3, semi - 3)));
    } catch (...) {
        std::cout << "[parseNsAndId][WARN] stoi ns failed, keep default ns=4\n";
        nsOut = 4;
    }

    if (semi + 2 >= full.size()) {
        std::cout << "[parseNsAndId][FAIL] missing id part\n";
        return false;
    }

    // form: "<...>;<t>=<id>"
    idTypeOut = full[semi + 1];
    if (full[semi + 2] != '=') {
        std::cout << "[parseNsAndId][FAIL] malformed after ';' (no '=')\n";
        return false;
    }

    idStrOut = full.substr(semi + 3);

    std::cout << "[parseNsAndId] out: ns=" << nsOut
              << " type='" << idTypeOut
              << "' id='" << idStrOut << "'\n";
    return true;
}

// ---------------------------------------------------------
// Debug-Helfer: Inventar-Match (kanonisch ns + idType + idStr)
// Druckt JEDE Prüfung, damit du die Ursache siehst
// ---------------------------------------------------------
static bool inInventory(const std::vector<PLCMonitor::InventoryRow>& rows,
                        UA_UInt16 nsWant,
                        const std::string& idStrWant,
                        char idTypeWant)
{
    std::cout << "[inInventory] WANT ns=" << nsWant
              << " type='" << idTypeWant
              << "' id='" << idStrWant << "'\n";

    bool matched = false;
    size_t idx = 0;
    for (const auto& ir : rows) {
        ++idx;
        if (ir.nodeClass != "Variable") continue;

        UA_UInt16 nsInv = 0; std::string idInv; char typeInv = '?';
        if (!parseNsAndId(ir.nodeId, nsInv, idInv, typeInv)) {
            std::cout << "  [inInventory][" << idx << "] skip (parse FAIL) invNodeId='"
                      << ir.nodeId << "'\n";
            continue;
        }

        const bool eq = (nsInv == nsWant) && (typeInv == idTypeWant) && (idInv == idStrWant);
        std::cout << "  [inInventory][" << idx << "] inv ns=" << nsInv
                  << " type='" << typeInv << "' id='" << idInv
                  << "'  ~?  want ns=" << nsWant
                  << " type='" << idTypeWant << "' id='" << idStrWant
                  << "'  -> " << (eq ? "MATCH" : "nope") << "\n";

        if (eq) { matched = true; break; }
    }

    if (!matched) std::cout << "[inInventory] NO MATCH in inventory\n";
    return matched;
}

// Versucht, JSON zu "normalisieren": falls j ein String mit JSON-Inhalt ist → nochmal parse.
// Gibt bei Erfolg das geparste Objekt zurück, ansonsten das Original.
static nlohmann::json parseMaybeDoubleEncoded(const nlohmann::json& j, const char* where) {
    using json = nlohmann::json;
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        std::cout << "[KG][" << where << "] value is STRING, attempting second parse (len=" << s.size() << ")\n";
        try {
            json jj = json::parse(s);
            std::cout << "[KG][" << where << "] second parse OK; type=" << (jj.is_object() ? "object" : (jj.is_array() ? "array" : jj.type_name())) << "\n";
            return jj;
        } catch (const std::exception& e) {
            std::cout << "[KG][" << where << "] second parse FAILED: " << e.what() << "\n";
        }
    }
    return j;
}

// Sorgt dafür, dass eine einzelne Row ein Objekt ist:
// - wenn 'in' Objekt: ok
// - wenn 'in' String: parse → Objekt?
// - corner case: parse ergibt { "rows":[{...}] } → nimm das erste Objekt
static bool coerceRowObject(const nlohmann::json& in, nlohmann::json& out) {
    using json = nlohmann::json;
    if (in.is_object()) { out = in; return true; }
    if (in.is_string()) {
        const std::string s = in.get<std::string>();
        std::cout << "[KG][coerceRow] row is STRING, trying parse; len=" << s.size() << "\n";
        try {
            json j2 = json::parse(s);
            if (j2.is_object()) {
                if (j2.contains("rows") && j2["rows"].is_array() && !j2["rows"].empty() && j2["rows"][0].is_object()) {
                    out = j2["rows"][0];
                    std::cout << "[KG][coerceRow] parsed STRING → object via rows[0]\n";
                    return true;
                }
                out = j2;
                std::cout << "[KG][coerceRow] parsed STRING → object directly\n";
                return true;
            }
            if (j2.is_array() && !j2.empty() && j2[0].is_object()) {
                out = j2[0];
                std::cout << "[KG][coerceRow] parsed STRING → array; using [0]\n";
                return true;
            }
            std::cout << "[KG][coerceRow] parsed STRING but not object/array-of-object\n";
        } catch (const std::exception& e) {
            std::cout << "[KG][coerceRow] parse fail: " << e.what() << "\n";
        }
    }
    return false;
}


void ReactionManager::onMethod(const Event& ev) {
    using Clock = std::chrono::steady_clock;

    if (ev.type == EventType::evD2) {
        const auto corr = makeCorrelationId("evD2");

        // 1) PLC-Inventar erstellen und drucken
        std::vector<PLCMonitor::InventoryRow> rows;
        if (mon_.dumpPlcInventory(rows, "PLC1")) {
            mon_.printInventoryTable(rows); // Tabelle
            // Bonus: alle aktuellen BOOL-Werte ausgeben
            std::cout << "\n[Inventory] Aktuelle BOOL-Werte:\n";
            for (const auto& ir : rows) {
                if (ir.nodeClass != "Variable") continue;

                // robust: akzeptiere alles, was "Boolean" enthält (Trailing-Infos etc.)
                const bool isBool = (ir.dtypeOrSig.find("Boolean") != std::string::npos);
                if (!isBool) continue;

                UA_UInt16 nsB = 4; std::string idB; char typeB='?';
                if (!parseNsAndId(ir.nodeId, nsB, idB, typeB)) {
                    std::cout << "  " << ir.nodeId << " = <parse failed>\n";
                    continue;
                }

                bool val=false;
                if (typeB != 's') {
                    std::cout << "  " << ir.nodeId << " = <unsupported idType '" << typeB
                            << "' for readBoolAt>\n";
                    continue;
                }

                if (mon_.readBoolAt(idB, nsB, val))
                    std::cout << "  " << ir.nodeId << " = " << (val ? "true" : "false") << "\n";
                else
                    std::cout << "  " << ir.nodeId << " = <read failed>\n";
            }
        std::cout << "\n";
        } else {
            std::cout << "[ReactionManager] dumpPlcInventory failed\n";
        }

        // 2) KG-Call im eigenen Thread: JSON auswerten, Ist/Soll vergleichen, Plan DIREKT ausführen
        std::thread([this, corr, rows]() {
            using json = nlohmann::json;

            std::cout << "[KG] corr=" << corr << " before worker.call\n";
            std::string srows; bool ok = true;
            try {
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
                    return py::str(kgi.attr("getFailureModeParameters")(interruptedSkill));
                });
            } catch (const std::exception& e) {
                ok = false;
                srows = R"({"rows":[]})";
                std::cerr << "[KG][ERR] corr=" << corr << " " << e.what() << "\n";
            }
            std::cout << "[KG] corr=" << corr << " after worker.call ok=" << ok
                      << " json_len=" << srows.size() << "\n";

            // 3) JSON parsen (Top-Level normalisieren, falls doppelt kodiert)
            json j;
            try { j = json::parse(srows); } catch (...) { j = json::object({{"rows", json::array()}}); }
            std::cout << "[KG] top-level type=" << j.type_name() << "\n";
            j = parseMaybeDoubleEncoded(j, "top");

            // 3a) Wir wollen ein Array namens 'rows' erhalten
            json rowsJson = json::array();
            if (j.contains("rows") && j["rows"].is_array()) {
                rowsJson = j["rows"];
                std::cout << "[KG] found rows[] directly, size=" << rowsJson.size() << "\n";
            } else if (j.is_array()) {
                rowsJson = j;
                std::cout << "[KG] top-level is array, size=" << rowsJson.size() << "\n";
            } else {
                // ggf. einzelnes Objekt als einzige Row behandeln
                std::cout << "[KG] no rows[], treating top-level as single row (type=" << j.type_name() << ")\n";
                rowsJson = json::array({ j });
            }

            // 4) Vorgaben prüfen (BOOL-Beispiele)
            bool allOk = true;
            int  checked = 0;

            for (const auto& rowIn : rowsJson) {
                nlohmann::json row;
                if (!coerceRowObject(rowIn, row)) {
                    std::cout << "[KG] row is not object (type=" << rowIn.type_name() << "), skip\n";
                    continue;
                }

                // 1) Felder auslesen
                std::string idFull = row.value("id", "");
                std::string t      = row.value("t", "");
                bool hasV          = row.contains("v");

                std::cout << "[KG] row.id='" << idFull << "' t='" << t
                        << "' v=" << (hasV ? row["v"].dump() : "<none>") << "\n";

                // 2) SPEZIALFALL: 'id' ist selbst ein JSON-String mit {"rows":[{...}]}
                //    -> Versuche, aus 'id' die echte Row zu gewinnen
                if (!idFull.empty() && idFull.front() == '{' && (t == "string" || t == "json" || t == "object")) {
                    std::cout << "[KG] salvage: row.id looks like JSON; trying to unwrap\n";
                    // parseMaybeDoubleEncoded nimmt ein json-Value; wir geben den String hinein
                    nlohmann::json idj = parseMaybeDoubleEncoded(nlohmann::json(idFull), "row.id");
                    if (idj.is_object()) {
                        if (idj.contains("rows") && idj["rows"].is_array() && !idj["rows"].empty() && idj["rows"][0].is_object()) {
                            row = idj["rows"][0];
                            std::cout << "[KG] salvage: took rows[0] from row.id JSON\n";
                        } else {
                            row = idj;
                            std::cout << "[KG] salvage: took object directly from row.id JSON\n";
                        }
                        // Felder neu ziehen
                        idFull = row.value("id", "");
                        t      = row.value("t", t);
                        hasV   = row.contains("v");

                        std::cout << "[KG] after salvage: row.id='" << idFull << "' t='" << t
                                << "' v=" << (hasV ? row["v"].dump() : "<none>") << "\n";
                    } else {
                        std::cout << "[KG] salvage: row.id was not parseable to object\n";
                    }
                }

                if (idFull.empty() || t.empty() || !hasV) {
                    std::cout << "[KG] skip: missing id/t/v\n";
                    continue;
                }

                // 3) NodeId parsen
                UA_UInt16 ns = 4; std::string idStr; char idType='?';
                if (!parseNsAndId(idFull, ns, idStr, idType)) {
                    std::cout << "[KG] unsupported NodeId format: " << idFull << "\n";
                    continue;
                }

                // 4) Inventar-Match (kanonisch: ns + idType + idStr)
                if (!inInventory(rows, ns, idStr, idType)) {
                    std::cout << "[Inv] Hinweis: " << idFull
                            << " nicht in InventoryTable gefunden (siehe [inInventory]-Log oben)\n";
                    // nicht fatal
                }

                // 5) Typ-spezifischer Vergleich – hier: BOOL
                if (t == "bool" || t == "boolean") {
                    if (!row["v"].is_boolean()) {
                        std::cout << "[KG] v not boolean for " << idFull << "\n";
                        continue;
                    }
                    const bool expected = row["v"].get<bool>();

                    if (idType != 's') {
                        std::cout << "[Check] idType '" << idType
                                << "' nicht unterstützt für readBoolAt (nur 's') in " << idFull << "\n";
                        continue;
                    }

                    bool current = false;
                    std::cout << "[Check] readBoolAt(id='" << idStr << "', ns=" << ns << ")\n";
                    const bool okRead = mon_.readBoolAt(idStr, ns, current);
                    if (!okRead) {
                        std::cout << "[Check] Lesen fehlgeschlagen in " << idFull << "\n";
                        allOk = false;
                        continue;
                    }

                    ++checked;
                    if (current == expected) {
                        std::cout << "Übereinstimmende Werte in " << idFull
                                << " (" << (current ? "true" : "false") << ")\n";
                    } else {
                        std::cout << "Abweichung in " << idFull
                                << " (ist="  << (current ? "true" : "false")
                                << ", soll=" << (expected ? "true" : "false") << ")\n";
                        allOk = false;
                    }
                } else {
                    // TODO: Int/String analog ergänzen (readInt16At / readAsString)
                }
            }

            // 5) Plan DIREKT ausführen (immer DiagnoseFinished pulsen, wie gewünscht)
            Plan plan;
            plan.correlationId = corr;
            plan.resourceId    = "Station";
            plan.ops.push_back(Operation{
                OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100
            });

            bus_.post(Event{
                EventType::evReactionPlanned, Clock::now(),
                std::any{ ReactionPlannedAck{
                    plan.correlationId, plan.resourceId,
                    std::string("KG Checks: ") + (allOk ? "OK" : "FAIL")
                    + " -> DiagnoseFinished-Puls"
                } }
            });

            auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
            const int rc = cf->execute(plan);

            bus_.post(Event{
                EventType::evReactionDone, Clock::now(),
                std::any{ ReactionDoneAck{ plan.correlationId, rc, rc ? "OK" : "FAIL" } }
            });
        }).detach(); // WICHTIG: Thread lösen, sonst std::terminate!

        return;
    }

    // ggf. weitere Eventtypen hier behandeln …
}

