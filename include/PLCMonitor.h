#pragma once

#include <string>
#include <vector>
#include <functional>
#include <queue>
#include <mutex>
#include <memory>
#include <type_traits>
#include <future>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/util.h>

class PLCMonitor {
public:
    // ---------- Task-Posting (läuft im Thread, der runIterate() aufruft) ----------
    using UaFn = std::function<void()>;
    void post(UaFn fn);
    template<class F,
             class Decayed = std::decay_t<F>,
             std::enable_if_t<!std::is_same_v<Decayed, UaFn>, int> = 0>
    void post(F&& f) {
        auto fn = std::make_shared<Decayed>(std::forward<F>(f)); // auch move-only
        UaFn job = [fn]() mutable { (*fn)(); };
        std::lock_guard<std::mutex> lk(qmx_);
        q_.push(std::move(job));
    }
    void processPosted(size_t max = 16);

    // ---------- Verbindungs-Optionen ----------
    struct Options {
        std::string endpoint;
        std::string username;
        std::string password;
        std::string certDerPath;
        std::string keyDerPath;
        std::string applicationUri;
        UA_UInt16   nsIndex = 2;
    };

    // ---------- ctor/dtor ----------
    explicit PLCMonitor(Options o);
    ~PLCMonitor();

    // ---------- Verbindung ----------
    bool connect();            // Basic256Sha256 + Sign&Encrypt + Username/PW + Zert/Key (per Options)
    void disconnect();

    // ---------- Client-Loop ----------
    UA_StatusCode runIterate(int timeoutMs = 0);   // vorantreiben (single-thread)
    bool waitUntilActivated(int timeoutMs = 3000); // bis Session aktiv

    // ---------- Highlevel-Beispiele ----------
    bool callMethode1(UA_UInt16 nsIndex,
                      const std::string& objectIdStr,
                      const std::string& methodIdStr,
                      UA_Int32 x, UA_Int32& yOut);

    bool readInt16At(const std::string& nodeIdStr, UA_UInt16 nsIndex, UA_Int16 &out) const;
    bool writeBool(const std::string& nodeIdStr, UA_UInt16 nsIndex, bool v);

    // ---------- Subscriptions ----------
    using Int16ChangeCallback = std::function<void(UA_Int16, const UA_DataValue&)>;
    using BoolChangeCallback  = std::function<void(UA_Boolean, const UA_DataValue&)>;

    bool subscribeInt16(const std::string& nodeIdStr,
                        UA_UInt16 nsIndex,
                        double samplingMs,
                        UA_UInt32 queueSize,
                        Int16ChangeCallback cb);

    bool subscribeBool(const std::string& nodeIdStr,
                       UA_UInt16 nsIndex,
                       double samplingMs,
                       UA_UInt32 queueSize,
                       BoolChangeCallback cb);

    void unsubscribe();

    // ---------- Snapshots (einfaches Read mehrerer Knoten) ----------
    struct SnapshotItem {
        std::string  nodeIdStr;
        UA_UInt16    ns;
        UA_DataValue dv; // deep-copied; caller must clear
    };

    bool readSnapshot(const std::vector<std::pair<UA_UInt16,std::string>>& nodes,
                      std::vector<SnapshotItem>& out,
                      UA_TimestampsToReturn ttr = UA_TIMESTAMPSTORETURN_SOURCE,
                      double maxAgeMs = 0.0);

    // ---------- Snapshots (Testserver – strukturierte Ausgabe + Browse) ----------
    struct SnapshotEntry {
        std::string   nodeIdText;   // NodeId als String (z. B. "ns=1;s=MyVar")
        std::string   browsePath;   // Pfad ab /Objects
        std::string   displayName;  // DisplayName des Targets
        UA_NodeClass  nodeClass;    // Variable, Object, View, ...
        std::string   dataType;     // z. B. "Double", "Int16"
        std::string   value;        // Wert als Text (falls Variable)
        UA_StatusCode status;       // Status der Read-Operation
    };

    std::future<std::vector<SnapshotEntry>>
    TestServer_snapshotAllAsync(int nsFilter = 1, unsigned maxDepth = 5, unsigned maxRefsPerNode = 200);

    std::vector<SnapshotEntry>
    TestServer_snapshotAllSync(int nsFilter = 1, int maxDepth = 5, std::size_t maxRefsPerNode = 200);

    bool TestServer_snapshotAllSync_impl(std::vector<SnapshotEntry>& out,
                                         int nsFilter, unsigned maxDepth, unsigned maxRefsPerNode);

    // ---------- Komfort für deinen Secure-Testserver ----------
    static Options TestServerDefaults(const std::string& clientCertDer,
                                      const std::string& clientKeyDer,
                                      const std::string& endpoint = "opc.tcp://localhost:4840");

    bool connectToSecureTestServer(const std::string& clientCertDer,
                                   const std::string& clientKeyDer,
                                   const std::string& endpoint = "opc.tcp://localhost:4840");

    bool watchTriggerD2(double samplingMs = 0.0, UA_UInt32 queueSize = 10);

    // Low-level Zugriff (falls nötig)
    UA_Client* raw() const { return client_; }

private:
    std::mutex qmx_;
    std::queue<UaFn> q_;

    static bool loadFileToByteString(const std::string& path, UA_ByteString &out);

    Options    opt_;
    UA_Client* client_=nullptr;

    PLCMonitor(const PLCMonitor&) = delete;
    PLCMonitor& operator=(const PLCMonitor&) = delete;

    UA_UInt32            subId_{0};
    UA_UInt32            monIdInt16_{0};
    UA_UInt32            monIdBool_{0};
    Int16ChangeCallback  onInt16Change_;
    BoolChangeCallback   onBoolChange_;

    static void dataChangeHandler(UA_Client*, UA_UInt32, void*, UA_UInt32, void*, UA_DataValue*);
};
