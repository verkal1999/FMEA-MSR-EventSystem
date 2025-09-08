
#include <iostream>
#include <pybind11/embed.h>
//#include "PythonWorker.h"
#include "ReactionManager.h"
#include "EventBus.h"
#include "Event.h"
#include "Acks.h"
#include "Correlation.h"
#include <chrono>
#include "Plan.h"
#include "CommandForce.h"       // deine umbenannte Klasse (statt UAWriteTaskForce)
#include "CommandForceFactory.h"   // falls du die Factory nutzt

namespace py = pybind11;

void ReactionManager::onMethod(const Event& ev) {
  if (ev.type != EventType::evD2) return;

  std::string srows;
  try {
    py::gil_scoped_acquire gil;

    py::module_ sys = py::module_::import("sys");
    sys.attr("path").attr("insert")(0, py::str(KG_SRC_DIR)); // <-- absolut

    auto kg = py::module_::import("KG_Interface");
    auto kgi = kg.attr("KGInterface")(
        KG_TTL_PATH,                                                // <-- absolut
        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/",
        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/class_",
        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/op_",
        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/dp_"
    );
    srows = py::str(kgi.attr("getFailureModeParameters")("TestSkill1"));
  } catch (const py::error_already_set& e) {
    std::cerr << "[ReactionManager] Python-Fehler: " << e.what() << "\n";
    srows = R"({"rows":[]})";
  }

   std::cout << "[ReactionManager] KG-Ergebnis: " << srows << "\n";

    // 2) Plan aus KG bauen (Minimalbeispiel)
    Plan plan;
    plan.correlationId = makeCorrelationId("evD2"); // <<— NEU
    plan.resourceId    = "Station";       // optional, befülle aus KG
    plan.ops.push_back(Operation{
           OpType::PulseBool,   // type
          "OPCUA.DiagnoseFinished",  // nodeId
            4,                   // nsF
            "true",              // arg
            100                    // timeoutMs
        });

    // 3) Ack: ReactionPlanned posten
    ReactionPlannedAck planned{
        plan.correlationId,
        plan.resourceId,
        "Setze DiagnoseFinished=true (ns=1) und führe nachgelagerte Schritte aus"
    };
    bus_.post(Event{ EventType::evReactionPlanned, std::chrono::steady_clock::now(), planned });

    // 4) Plan ausführen (Variante A – CommandForce nutzt PLCMonitor::post(...))
    auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
    int rc = cf->execute(plan);

    // 5) Ack: ReactionDone posten
    ReactionDoneAck done{
        plan.correlationId,
        rc,
        rc ? "Ausführung OK" : "Ausführung fehlgeschlagen"
    };
    bus_.post(Event{ EventType::evReactionDone, std::chrono::steady_clock::now(), done });
}