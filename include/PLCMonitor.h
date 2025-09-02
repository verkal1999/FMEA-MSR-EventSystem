#pragma once
#include <string>
#include <vector>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/util.h>
#include <open62541/client.h>
#include <functional>

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

    struct SnapshotItem {
    std::string nodeIdStr;
    UA_UInt16   ns;
    UA_DataValue dv; // deep-copied; caller must clear
    };

    bool readSnapshot(const std::vector<std::pair<UA_UInt16,std::string>>& nodes,
                    std::vector<SnapshotItem>& out,
                    UA_TimestampsToReturn ttr = UA_TIMESTAMPSTORETURN_SOURCE,
                    double maxAgeMs = 0.0);

    // Callback: liefert den neuen INT16-Wert und die komplette UA_DataValue (z. B. für Timestamps)
    using Int16ChangeCallback = std::function<void(UA_Int16, const UA_DataValue&)>;
    //zusätzlicher Bool-Callback
    using BoolChangeCallback  = std::function<void(UA_Boolean, const UA_DataValue&)>;

     // Subscription auf INT16-Node
    // samplingMs: schneller als der SPS-Inkrement-Takt wählen
    // queueSize : >1, um schnelle Änderungen nicht zu verlieren
    bool subscribeInt16(const std::string& nodeIdStr,
                        UA_UInt16 nsIndex,
                        double samplingMs,
                        UA_UInt32 queueSize,
                        Int16ChangeCallback cb);
    bool subscribeBool(const std::string& nodeIdStr, UA_UInt16 nsIndex,
                       double samplingMs, UA_UInt32 queueSize, BoolChangeCallback cb);

    // Optional: abmelden (hier simpel – wird auch durch disconnect() erledigt)
    void unsubscribe();

    // ...

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

    //Schreibzugriff auf bool1
    bool writeBool(const std::string& nodeIdStr, UA_UInt16 nsIndex, bool v);

private:
    static bool loadFileToByteString(const std::string& path, UA_ByteString &out);

    Options    opt_;
    UA_Client* client_ {nullptr};

    PLCMonitor(const PLCMonitor&) = delete;
    PLCMonitor& operator=(const PLCMonitor&) = delete;

     // ...
    UA_UInt32 subId_ {0};
    UA_UInt32 monIdInt16_ {0};
    UA_UInt32 monIdBool_  {0};
    Int16ChangeCallback onInt16Change_;
    BoolChangeCallback  onBoolChange_;

    static void dataChangeHandler(UA_Client*, UA_UInt32, void*, UA_UInt32, void*, UA_DataValue*);
};
