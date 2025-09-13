#include "FailureRecorder.h"
#include "Acks.h"
#include "CommandForceFactory.h"
#include "PythonWorker.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>

using json = nlohmann::json;
namespace fs = std::filesystem;

void FailureRecorder::subscribeAll() {
    auto self = shared_from_this();
    // alle dir. Events registrieren – der Observer reagiert nur bei Fehlern
    bus_.subscribe(EventType::evD1, self, 1);
    bus_.subscribe(EventType::evD2, self, 1);
    bus_.subscribe(EventType::evD3, self, 1);
    bus_.subscribe(EventType::evReactionPlanned, self, 1);
    bus_.subscribe(EventType::evReactionDone, self, 1);
    bus_.subscribe(EventType::evProcessFail, self, 1);
    bus_.subscribe(EventType::evKGResult, self, 1);
    bus_.subscribe(EventType::evKGTimeout, self, 1);
}

void FailureRecorder::onEvent(const Event& ev) {
    // nur bei Fehlern erfassen → evProcessFail, evKGTimeout
    std::string corr, process, text;

    if (ev.type == EventType::evProcessFail) {
        if (!ev.payload.has_value()) return;
        const auto& p = std::any_cast<const ProcessFailAck&>(ev.payload);
        corr    = p.correlationId;
        process = p.processName;
        text    = p.summary;
    } else if (ev.type == EventType::evKGTimeout) {
        if (!ev.payload.has_value()) return;
        const auto& p = std::any_cast<const KGTimeoutPayload&>(ev.payload);
        corr    = p.correlationId;
        process = "KG";
        text    = "Knowledge Graph timeout";
    } else {
        return; // andere Events ignorieren
    }

    // Screenshots einsammeln (KG-Cache bevorzugt)
    const std::string shotsJson = getScreenshotsJson(corr);

    // Plan für Ingestion bauen
    Plan plan = makeKgIngestionPlan(corr, process, text, shotsJson);

    // neue CommandForce-Variante nutzen
    auto cf = CommandForceFactory::create(CommandForceFactory::Kind::KGIngest, mon_);
    (void)cf->execute(plan);
}

std::string FailureRecorder::getScreenshotsJson(const std::string& corr) {
    // 1) KG fragt eigene Cache-Registry (falls vorhanden)
    try {
        return PythonWorker::instance().call([&]() -> std::string {
            namespace py = pybind11;
            py::module_ sys  = py::module_::import("sys");
            py::list    path = sys.attr("path").cast<py::list>();
            path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

            py::module_ kg = py::module_::import("KG_Interface");
            py::object kgi = kg.attr("KGInterface")();
            py::object res = kgi.attr("getCachedScreenshots")(py::str(corr));
            return std::string(py::str(res)); // erwartet z.B. '["C:/.../a.png", "..."]'
        });
    } catch (...) {
        // 2) Fallback: Dateisystem-konvention ./screenshots/<corr>/*.png|*.jpg
        json arr = json::array();
        try {
            fs::path root = fs::path("screenshots") / corr;
            if (fs::exists(root) && fs::is_directory(root)) {
                for (auto& de : fs::directory_iterator(root)) {
                    if (!de.is_regular_file()) continue;
                    const auto ext = de.path().extension().string();
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                        arr.push_back(de.path().string());
                    }
                }
            }
        } catch (...) {}
        return arr.dump();
    }
}

Plan FailureRecorder::makeKgIngestionPlan(const std::string& corr,
                                          const std::string& process,
                                          const std::string& summary,
                                          const std::string& screenshotsJson)
{
    Plan p;
    p.correlationId = corr;
    p.resourceId    = "KG";

    Operation op;
    op.type = OpType::CallMethod; // semantisch egal – wird von KgIngestionForce als „Input-Container“ genutzt
    op.inputs[0] = corr;
    op.inputs[1] = process;
    op.inputs[2] = summary;
    op.inputs[3] = screenshotsJson;
    p.ops.push_back(std::move(op));
    return p;
}
