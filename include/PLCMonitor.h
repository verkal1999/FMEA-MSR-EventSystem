#pragma once
#include <string>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/util.h>
#include <open62541/client.h>

class PLCMonitor {
public:
    struct Options {
        std::string endpoint;        // z.B. "opc.tcp://DESKTOP-XYZ:4840"
        std::string username;        // OPC UA Username
        std::string password;        // OPC UA Password
        std::string certDerPath;     // Pfad zu client_cert.der
        std::string keyDerPath;      // Pfad zu client_key.der (PKCS#8, unverschlüsselt)
        std::string applicationUri;  // MUSS zur SAN-URI im Client-Zertifikat passen
        UA_UInt16    nsIndex   = 4;  // Namespace-Index
        std::string  nodeIdStr = "OPCUA.Z1"; // Standard-Knoten
    };

    explicit PLCMonitor(Options o);
    ~PLCMonitor();

    // Aufbau einer gesicherten Verbindung (Basic256Sha256 + Sign&Encrypt + Username/Passwort)
    bool connect();
    void disconnect();

    // Interne Client-Statemachine vorantreiben (single-thread)
    UA_StatusCode runIterate(int timeoutMs = 0);

    // Warten bis Session aktiviert ist (siehe UA_SessionState)
    bool waitUntilActivated(int timeoutMs = 3000);

    // Polling-Read eines Int16 an beliebiger String-NodeId im gegebenen Namespace
    bool readInt16At(const std::string& nodeIdStr, UA_UInt16 nsIndex, UA_Int16 &out) const;

    // Zugriff auf den Roh-Client (falls nötig)
    UA_Client* raw() const { return client_; }

private:
    static bool loadFileToByteString(const std::string& path, UA_ByteString &out);

    Options    opt_;
    UA_Client* client_ {nullptr};

    PLCMonitor(const PLCMonitor&) = delete;
    PLCMonitor& operator=(const PLCMonitor&) = delete;
};
