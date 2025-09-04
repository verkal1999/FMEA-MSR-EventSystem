#pragma once
#include "PLCMonitor.h"
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <string>
#include <thread>
#include <atomic>

class TaskManager {
public:
    enum class State { Idle, Starting, Calling, Done, Failed };
    using FinishedCb = std::function<void(bool ok, UA_Int32 y)>;
    TaskManager(const PLCMonitor::Options& opt,
                UA_UInt16 nsIndex,
                std::string objectNodeIdStr,     // z.B. "MAIN.fbJob"
                std::string methodNodeIdStr,     // z.B. "MAIN.fbJob.M_Methode1"
                std::string diagnoseFinishedId,  // z.B. "OPCUA.DiagnoseFinished"
                unsigned callTimeoutMs,
                FinishedCb onFinished);
    ~TaskManager();

    // Von deinem StateD2-Callback rufen
    void notifyTrigger();

    State state() const { return state_.load(); }

private:
    void worker();

    bool connect();
    void disconnect();
    bool callJob(UA_Int32 x, UA_Int32& y);
    bool waitUntilActivated(int timeoutMs);
    bool callJobMethod(UA_Int32 x, UA_Int32& yOut);
    bool writeBool(const std::string& nodeIdStr, bool value);

    // eigene Session (zweiter Client) nur für Calls/Schreibungen
    UA_Client* c_ = nullptr;
    // Konfiguration / NodeIds
    //PLCMonitor& mon_;
    PLCMonitor::Options opt_;
    UA_UInt16 ns_;
    //std::string objId_, methId_, diagId_;
    std::string objNode_;
    std::string methNode_;
    std::string diagNode_;
    unsigned callTimeoutMs_;
    FinishedCb onFinished_;

    // Thread/Steuerung
    std::thread th_;
    std::atomic<bool> run_{true};
    //std::atomic<bool> trig_{false};
    std::atomic<State> state_{State::Idle};
};
