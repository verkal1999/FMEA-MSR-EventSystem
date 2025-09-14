#include "KgIngestionForce.h"
#include "Acks.h"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

int KgIngestionForce::execute(const Plan& p) {
    using Clock = std::chrono::steady_clock;
    if (p.ops.empty()) return 0;
    const auto& op = p.ops.front();

    const std::string corr      = p.correlationId.empty() ? getStr(op.inputs, 0) : p.correlationId;
    const std::string process   = getStr(op.inputs, 1);
    const std::string summary   = getStr(op.inputs, 2);
    const std::string snapJson  = getStr(op.inputs, 3);

    bus_.post({ EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{ corr, "KG", "OccurredFailure ingestion (KG)" } } });

    bool ok = false;
    try {
        /*
        ok = PythonWorker::instance().call([&]() -> bool {
            namespace py = pybind11;
            py::module_ sys  = py::module_::import("sys");
            py::list    path = sys.attr("path").cast<py::list>();
            path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

            py::module_ kg = py::module_::import("KG_Interface");
            py::object kgi = kg.attr("KGInterface")();

            // ingestOccuredFailure(corr, process, summary, snapshotJson)
            py::object res = kgi.attr("ingestOccuredFailure")(
                py::str(corr), py::str(process), py::str(summary), py::str(snapJson)
            );
            return res.is_none() ? true : res.cast<bool>();
        });
        */
       std::cout << "[KGIngestionForce] Ingestion of " << summary << "/n";
    } catch (const std::exception&) {
        ok = false;
    }

    bus_.post({ EventType::evReactionDone, Clock::now(),
        std::any{ ReactionDoneAck{ corr, ok ? 1 : 0, ok ? "OK" : "FAIL" } } });

    return ok ? 1 : 0;
}
