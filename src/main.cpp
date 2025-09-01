#include "PLCMonitor.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <limits>

static volatile std::sig_atomic_t g_run = 1;
static void onSig(int) { g_run = 0; }

int main() {
    std::signal(SIGINT,  onSig);
    std::signal(SIGTERM, onSig);

    PLCMonitor::Options opt;
    opt.endpoint       = "opc.tcp://DESKTOP-LNJR8E0:4840";
    opt.username       = "Admin";
    opt.password       = "123456";
    opt.certDerPath    = "client_cert.der";
    opt.keyDerPath     = "client_key.der";
    opt.applicationUri = "urn:DESKTOP-LNJR8E0:Test:opcua-client"; // muss zur Zertifikat-SAN passen!
    opt.nsIndex        = 4;
    opt.nodeIdStr      = "OPCUA.Z1";

    PLCMonitor mon(opt);
    if(!mon.connect()) {
        std::cerr << "Connect failed\n";
        return 1;
    }
    std::cout << "Connected and session ACTIVATED.\n";

    UA_Int16 last = (std::numeric_limits<UA_Int16>::min)();

    while(g_run) {
        // Die Client-Statemachine regelmäßig iterieren lassen
        mon.runIterate(50);

        UA_Int16 v = 0;
        if(mon.readInt16At(opt.nodeIdStr, opt.nsIndex, v)) {
            if(v != last) {
                std::cout << "[Z1] = " << v << '\n';
                last = v;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    mon.disconnect();
    return 0;
}