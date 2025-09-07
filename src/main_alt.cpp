#include "PLCMonitor.h"
#include "TaskForce.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
//#include "PythonRuntime.h"
#include "PythonWorker.h"

static volatile std::sig_atomic_t g_run = 1;
static void onSig(int) { g_run = 0; }

int main() {
    std::signal(SIGINT,  onSig);
    std::signal(SIGTERM, onSig);
    PythonRuntime::ensureStarted();                    // einmalig pro Prozess
    { py::gil_scoped_acquire _g; }                     // kurzes Priming (hilft bei Multithread-Embedding) :contentReference[oaicite:1]{index=1}
    PythonWorker::instance().start();   // <-- NEU
    PLCMonitor::Options opt;
    opt.endpoint       = "opc.tcp://DESKTOP-LNJR8E0:4840"; // anpassen falls nötig
    opt.username       = "Admin";
    opt.password       = "123456";
    opt.certDerPath    = R"(..\..\certificates\client_cert.der)";
    opt.keyDerPath     = R"(..\..\certificates\client_key.der)";
    opt.applicationUri = "urn:DESKTOP-LNJR8E0:Test:opcua-client";
    opt.nsIndex        = 4;

    PLCMonitor mon(opt);
    if(!mon.connect()){
        std::cerr << "Connect failed\n";
        return 1;
    }
    std::cout << "Connected and session ACTIVATED.\n";

    std::unique_ptr<TaskForce> task;
    std::atomic<bool> taskDone{false};
    std::atomic<bool> taskOk{false};
    std::atomic<UA_Int32> taskY{0};

    // --- NEW: Rising-Edge + Busy-Flag, damit kein Doppelstart ---
    std::atomic<bool> lastD2{false};
    std::atomic<bool> taskBusy{false};

    bool okD2 = mon.subscribeBool("OPCUA.TriggerD2", /*ns*/4, /*sampling*/50.0, /*queue*/8,
        [&](UA_Boolean b, const UA_DataValue& dv){
            std::cout << "[TriggerD2] = " << (b ? "TRUE" : "FALSE");
            bool prev = lastD2.exchange(b, std::memory_order_acq_rel);
            if(dv.hasSourceTimestamp) {
                UA_DateTime ts = dv.sourceTimestamp;
                std::cout << " (ts=" << ts/10000 << " ms)";
            }
            std::cout << std::endl;

            // Nur bei steigender Flanke starten (FALSE -> TRUE)
            if(b && !prev) {
                bool expected = false;
                if(taskBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    task = std::make_unique<TaskForce>(
                        opt, /*ns*/4,
                        "MAIN.fbJob",
                        "MAIN.fbJob.M_Methode1",
                        "OPCUA.DiagnoseFinished",
                        /*timeoutMs*/60000,
                        // Callback: Ergebnis an main melden
                        [&](bool ok, UA_Int32 y){
                            taskOk  = ok;
                            taskY   = y;
                            taskDone= true;      // Signal setzen
                            taskBusy.store(false, std::memory_order_release); // --- NEW ---
                        }
                    );
                } else {
                    std::cout << "[Task] busy, trigger ignored\n";
                }
            }
        });
    if(!okD2) std::cerr << "Subscribe TriggerD2 failed\n";

    while(g_run) {
        mon.runIterate(20);                     // Session & Subscriptions am Leben halten. :contentReference[oaicite:4]{index=4}
        // Ergebnis abholen und aufräumen
        if(taskDone.exchange(false)) {
            std::cout << "[Task] finished, ok=" << (taskOk ? "TRUE":"FALSE")
                    << ", y=" << taskY.load() << "\n";
            task.reset(); // TaskForce-Objekt zerstören -> Thread gejoint, Session geschlossen
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // --- Phase 2: Drain-Phase (sauberer Shutdown) – unverändert beibehalten! ---
    auto t0 = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
        mon.runIterate(20);                     // ausstehende PublishResponses noch verarbeiten. :contentReference[oaicite:5]{index=5}
    }

    mon.unsubscribe();
    PythonWorker::instance().stop();
    mon.disconnect();
    return 0;
}