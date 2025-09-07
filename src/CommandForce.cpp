#include "CommandForce.h"
#include "PLCMonitor.h"

#include <string>    // std::stoi
#include <iostream>
#include <thread>
#include <chrono>

CommandForce::CommandForce(PLCMonitor& mon, IOrderQueue* oq)
    : mon_(mon), oq_(oq) {}

int CommandForce::execute(const Plan& p) {
    bool ok = true;

    for (const auto& op : p.ops) {
        switch (op.type) {
        case OpType::WriteBool: {
            const bool value = (op.arg == "true" || op.arg == "1");
            mon_.post([&m = mon_, op, value]{
                const bool wr = m.writeBool(op.nodeId, op.ns, value);
                std::cout << "[CommandForce] WriteBool " << op.nodeId
                          << " ns=" << op.ns << " value=" << (value ? "true" : "false")
                          << " -> " << (wr ? "OK" : "FAIL") << "\n";
            });
            break;
        }

        case OpType::WriteInt32: {
            // TODO: Implementiere writeInt32 in PLCMonitor, dann hier aktivieren
            int value = 0;
            try { value = std::stoi(op.arg); } catch (...) { value = 0; ok = false; }
            std::cout << "[CommandForce] WriteInt32 TODO node=" << op.nodeId
                      << " ns=" << op.ns << " value=" << value << " (not implemented)\n";
            // mon_.post([&m = mon_, op, value]{ m.writeInt32(op.nodeId, op.ns, value); });
            break;
        }

        case OpType::CallMethod: {
            // TODO: op.arg als JSON der Method-Argumente parsen und callMethod aufrufen
            std::cout << "[CommandForce] CallMethod TODO node=" << op.nodeId
                      << " ns=" << op.ns << " args='" << op.arg << "' (not implemented)\n";
            // mon_.post([&m = mon_, op]{ m.callMethod(...); });
            break;
        }

        case OpType::WaitMs: {
            const int ms = (op.timeoutMs > 0) ? op.timeoutMs : 0; // makro-sicher
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            break;
        }

        case OpType::ReadCheck: {
            std::cout << "[CommandForce] ReadCheck TODO node=" << op.nodeId
                      << " ns=" << op.ns << " expect='" << op.arg
                      << "' timeoutMs=" << op.timeoutMs << " (not implemented)\n";
            // Optional: synchron lesen & prüfen -> ok = ok && result;
            break;
        }

        case OpType::BlockResource: {
            if (oq_) ok = oq_->blockResource(op.nodeId) && ok;
            else std::cout << "[CommandForce] BlockResource(" << op.nodeId << ") (noop)\n";
            break;
        }

        case OpType::RerouteOrders: {
            if (oq_) ok = oq_->reroute(op.nodeId, op.arg) && ok;
            else std::cout << "[CommandForce] RerouteOrders(" << op.nodeId
                           << ", criteria=" << op.arg << ") (noop)\n";
            break;
        }

        case OpType::UnblockResource: {
            if (oq_) ok = oq_->unblockResource(op.nodeId) && ok;
            else std::cout << "[CommandForce] UnblockResource(" << op.nodeId << ") (noop)\n";
            break;
        }
        }
    }

    return ok ? 1 : 0;
}
