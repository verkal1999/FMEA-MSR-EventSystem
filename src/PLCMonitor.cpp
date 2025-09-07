#include "PLCMonitor.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <open62541/client.h>
#include <open62541/client_config_default.h>

// NEU hinzufügen:
#include <open62541/client_subscriptions.h>
#include <open62541/client_highlevel.h>

PLCMonitor::PLCMonitor(Options o) : opt_(std::move(o)) {}
PLCMonitor::~PLCMonitor() { disconnect(); }

static bool loadFile(const std::string& path, UA_ByteString& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f) return false;
    const std::streamsize len = f.tellg();
    if(len <= 0) return false;
    f.seekg(0, std::ios::beg);

    out.length = static_cast<size_t>(len);
    out.data   = (UA_Byte*)UA_malloc(out.length);
    if(!out.data) { out.length = 0; return false; }

    if(!f.read(reinterpret_cast<char*>(out.data), len)) {
        UA_ByteString_clear(&out);
        return false;
    }
    return true;
}

bool PLCMonitor::connect() {
    disconnect();

    client_ = UA_Client_new();
    if(!client_) return false;

    UA_ClientConfig* cfg = UA_Client_getConfig(client_);
    UA_ClientConfig_setDefault(cfg); // Basisdefaults setzen (timeouts etc.). :contentReference[oaicite:1]{index=1}

    // Optional: Publish-Queue etc.
    cfg->outStandingPublishRequests = 5;                                  // :contentReference[oaicite:2]{index=2}
    cfg->securityMode = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;            // Sign&Encrypt
    cfg->securityPolicyUri = UA_STRING_ALLOC(
        const_cast<char*>("http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"));

    // ApplicationUri muss zur SAN-URI des Client-Zertifikats passen (sonst Warnungen/Rejects)
    if(!opt_.applicationUri.empty())
        cfg->clientDescription.applicationUri = UA_STRING_ALLOC(
            const_cast<char*>(opt_.applicationUri.c_str()));

    // Zertifikat + Key laden
    UA_ByteString cert = UA_BYTESTRING_NULL;
    UA_ByteString key  = UA_BYTESTRING_NULL;
    if(!loadFile(opt_.certDerPath, cert) || !loadFile(opt_.keyDerPath, key)) {
    fprintf(stderr, "Failed to load cert/key\n");
    return false;
    }

    // Verschlüsselung konfigurieren (keine Trust/CRL-Listen hier – Demo/Test)
    UA_StatusCode st = UA_ClientConfig_setDefaultEncryption(
        cfg, cert, key, /*trustList*/nullptr, 0, /*revocation*/nullptr, 0);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "Encryption setup failed: 0x%08x\n", st);
        UA_Client_delete(client_); client_ = nullptr;
        return false;
    }
    // (Die API wird in open62541-Beispielen genauso verwendet.) :contentReference[oaicite:3]{index=3}

    // Username/Passwort verbinden
    st = UA_Client_connectUsername(client_,
                                   opt_.endpoint.c_str(),
                                   opt_.username.c_str(),
                                   opt_.password.c_str());                // 
    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "Connect failed: 0x%08x\n", st);
        UA_Client_delete(client_); client_ = nullptr;
        return false;
    }

    // Warten bis Session ACTIVATED (vgl. UA_SessionState) :contentReference[oaicite:5]{index=5}
    if(!waitUntilActivated(3000)) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT,
                       "Session not ACTIVATED within timeout");
        disconnect();
        return false;
    }
    return true;
}

void PLCMonitor::disconnect() {
    if(client_) {
        // --- NEW: Subscription vor Disconnect löschen (sauberer) ---
        if(subId_) {
            UA_Client_Subscriptions_deleteSingle(client_, subId_);   // open62541-API
            subId_ = 0; monIdInt16_ = 0; monIdBool_ = 0;
        }
        UA_Client_disconnect(client_);
        UA_Client_delete(client_);
        client_ = nullptr;
    }
}

UA_StatusCode PLCMonitor::runIterate(int timeoutMs) {
    if(!client_) return UA_STATUSCODE_BADSERVERNOTCONNECTED;
    return UA_Client_run_iterate(client_, timeoutMs);                     // :contentReference[oaicite:6]{index=6}
}

bool PLCMonitor::waitUntilActivated(int timeoutMs) {
    if(!client_) return false;

    auto t0 = std::chrono::steady_clock::now();
    for(;;) {
        UA_SecureChannelState scState;
        UA_SessionState      ssState;
        UA_StatusCode status;
        UA_StatusCode rcIter = UA_Client_run_iterate(client_, 50);
        (void)rcIter;

        UA_Client_getState(client_, &scState, &ssState, &status);         // 
        if(scState == UA_SECURECHANNELSTATE_OPEN &&
           ssState == UA_SESSIONSTATE_ACTIVATED)                          // :contentReference[oaicite:8]{index=8}
            return true;

        if(std::chrono::steady_clock::now() - t0 >
           std::chrono::milliseconds(timeoutMs))
            return false;
    }
}

bool PLCMonitor::readInt16At(const std::string& nodeIdStr,
                             UA_UInt16 nsIndex,
                             UA_Int16 &out) const {
    if(!client_) return false;

    UA_NodeId nid = UA_NODEID_STRING_ALLOC(nsIndex,
                          const_cast<char*>(nodeIdStr.c_str()));
    UA_Variant val; UA_Variant_init(&val);

    UA_StatusCode st = UA_Client_readValueAttribute(client_, nid, &val);
    UA_NodeId_clear(&nid);

    const bool ok = (st == UA_STATUSCODE_GOOD) &&
                    UA_Variant_isScalar(&val) &&
                    val.type == &UA_TYPES[UA_TYPES_INT16] &&
                    val.data != nullptr;
    if(ok) out = *static_cast<UA_Int16*>(val.data);
    UA_Variant_clear(&val);
    return ok;

}
// PLCMonitor.cpp  (Ergänzungen)

bool PLCMonitor::subscribeInt16(const std::string& nodeIdStr, UA_UInt16 nsIndex,
                                double samplingMs, UA_UInt32 queueSize, Int16ChangeCallback cb) {
    if(!client_) return false;
    onInt16Change_ = std::move(cb);

    // Subscription nur einmal anlegen
    if(subId_ == 0) {
        UA_CreateSubscriptionRequest sReq = UA_CreateSubscriptionRequest_default();
        sReq.requestedPublishingInterval = 20.0;
        sReq.requestedMaxKeepAliveCount  = 20;
        sReq.requestedLifetimeCount      = 60;

        UA_CreateSubscriptionResponse sResp =
            UA_Client_Subscriptions_create(client_, sReq, /*subCtx*/this, nullptr, nullptr);
        if(sResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) return false;
        subId_ = sResp.subscriptionId;
        std::cout << "revised publish = " << sResp.revisedPublishingInterval << " ms\n";
    }

    UA_MonitoredItemCreateRequest monReq =
        UA_MonitoredItemCreateRequest_default(
            UA_NODEID_STRING_ALLOC(nsIndex, const_cast<char*>(nodeIdStr.c_str())));
    monReq.requestedParameters.samplingInterval = samplingMs;
    monReq.requestedParameters.queueSize        = queueSize;
    monReq.requestedParameters.discardOldest    = UA_TRUE;

    UA_MonitoredItemCreateResult monRes =
        UA_Client_MonitoredItems_createDataChange(
            client_, subId_, UA_TIMESTAMPSTORETURN_SOURCE, monReq,
            this, &PLCMonitor::dataChangeHandler, nullptr);

    UA_NodeId_clear(&monReq.itemToMonitor.nodeId);

    if(monRes.statusCode != UA_STATUSCODE_GOOD) return false;
    monIdInt16_ = monRes.monitoredItemId;
    std::cout << "revised sampling (int16) = "
              << monRes.revisedSamplingInterval
              << " ms, revised queue = " << monRes.revisedQueueSize << "\n";
    return true;
}

bool PLCMonitor::subscribeBool(const std::string& nodeIdStr, UA_UInt16 nsIndex,
                               double samplingMs, UA_UInt32 queueSize, BoolChangeCallback cb) {
    if(!client_) return false;
    onBoolChange_ = std::move(cb);

    // Falls noch keine Subscription existiert: anlegen (gleich wie bei subscribeInt16)
    if(subId_ == 0) {
        UA_CreateSubscriptionRequest sReq = UA_CreateSubscriptionRequest_default();
        sReq.requestedPublishingInterval = 20.0;
        sReq.requestedMaxKeepAliveCount  = 20;
        sReq.requestedLifetimeCount      = 60;

        UA_CreateSubscriptionResponse sResp =
            UA_Client_Subscriptions_create(client_, sReq, /*subCtx*/this, nullptr, nullptr);
        if(sResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD)
            return false;
        subId_ = sResp.subscriptionId;
        std::cout << "revised publish = " << sResp.revisedPublishingInterval << " ms\n";
    }

    // Monitored Item für BOOL anlegen
    UA_MonitoredItemCreateRequest monReq =
        UA_MonitoredItemCreateRequest_default(
            UA_NODEID_STRING_ALLOC(nsIndex, const_cast<char*>(nodeIdStr.c_str())));
    monReq.requestedParameters.samplingInterval = samplingMs;
    monReq.requestedParameters.queueSize        = queueSize;
    monReq.requestedParameters.discardOldest    = UA_TRUE;

    UA_MonitoredItemCreateResult monRes =
        UA_Client_MonitoredItems_createDataChange(
            client_, subId_, UA_TIMESTAMPSTORETURN_SOURCE, monReq,
            this, &PLCMonitor::dataChangeHandler, nullptr);

    UA_NodeId_clear(&monReq.itemToMonitor.nodeId);

    if(monRes.statusCode != UA_STATUSCODE_GOOD)
        return false;

    monIdBool_ = monRes.monitoredItemId;
    std::cout << "revised sampling (bool) = "
              << monRes.revisedSamplingInterval
              << " ms, revised queue = " << monRes.revisedQueueSize << "\n";
    return true;
}

void PLCMonitor::unsubscribe() {
    if(client_ && subId_) {
        UA_Client_Subscriptions_deleteSingle(client_, subId_);
    }
    subId_ = 0;
    monIdInt16_ = 0;
    monIdBool_  = 0;
    onInt16Change_ = nullptr;
}

// static
void PLCMonitor::dataChangeHandler(UA_Client*,
                                   UA_UInt32 /*subId*/, void* subCtx,
                                   UA_UInt32 monId, void* monCtx,
                                   UA_DataValue* value) {
    PLCMonitor* self = static_cast<PLCMonitor*>(monCtx ? monCtx : subCtx);
    if(!self || !value || !value->hasValue) return;

    // Overflow-Info (optional)
    if(value->hasStatus && (value->status & UA_STATUSCODE_INFOBITS_OVERFLOW)) {
        std::cout << "[MI " << monId << "] queue OVERFLOW\n";
    }

    // INT16
    if(self->onInt16Change_ &&
       UA_Variant_isScalar(&value->value) &&
       value->value.type == &UA_TYPES[UA_TYPES_INT16] &&
       value->value.data) {
        UA_Int16 v = *static_cast<UA_Int16*>(value->value.data);
        self->onInt16Change_(v, *value);
        return;
    }

    // BOOL
    if(self->onBoolChange_ &&
       UA_Variant_isScalar(&value->value) &&
       value->value.type == &UA_TYPES[UA_TYPES_BOOLEAN] &&
       value->value.data) {
        UA_Boolean b = *static_cast<UA_Boolean*>(value->value.data);
        self->onBoolChange_(b, *value);
        return;
    }
}
bool PLCMonitor::writeBool(const std::string& nodeIdStr, UA_UInt16 ns, bool value) {
    if(!client_) return false;

    UA_NodeId nid = UA_NODEID_STRING_ALLOC(ns, const_cast<char*>(nodeIdStr.c_str()));

    UA_Variant v; UA_Variant_init(&v);
    UA_Boolean b = value;

    // Serverseitig wird kopiert; wir geben *unsere* Kopie wieder frei:
    UA_Variant_setScalarCopy(&v, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);

    UA_StatusCode rc = UA_Client_writeValueAttribute(client_, nid, &v);

    UA_Variant_clear(&v);
    UA_NodeId_clear(&nid);
    return rc == UA_STATUSCODE_GOOD;
}

bool PLCMonitor::readSnapshot(const std::vector<std::pair<UA_UInt16,std::string>>& nodes,
                              std::vector<SnapshotItem>& out,
                              UA_TimestampsToReturn ttr, double maxAgeMs) {
    if(!client_ || nodes.empty()) return false;

    // 1) ReadValueId-Array allozieren
    const size_t N = nodes.size();
    UA_ReadValueId* rvids =
        (UA_ReadValueId*)UA_Array_new(N, &UA_TYPES[UA_TYPES_READVALUEID]);
    if(!rvids) return false;

    for(size_t i=0; i<N; ++i) {
        UA_ReadValueId_init(&rvids[i]);
        rvids[i].nodeId = UA_NODEID_STRING_ALLOC(
            nodes[i].first, const_cast<char*>(nodes[i].second.c_str()));
        rvids[i].attributeId = UA_ATTRIBUTEID_VALUE;
        // rvids[i].indexRange, dataEncoding bleiben leer
    }

    // 2) ReadRequest vorbereiten
    UA_ReadRequest req; UA_ReadRequest_init(&req);
    req.nodesToRead       = rvids;
    req.nodesToReadSize   = (UA_UInt32)N;
    req.timestampsToReturn= ttr;                // z.B. SOURCE
    req.maxAge            = (UA_Double)(maxAgeMs); // 0 = always fresh

    // 3) Service aufrufen
    UA_ReadResponse resp = UA_Client_Service_read(client_, req);

    bool ok = (resp.responseHeader.serviceResult == UA_STATUSCODE_GOOD &&
               resp.resultsSize == req.nodesToReadSize);

    if(ok) {
        out.clear();
        out.reserve(N);
        for(size_t i=0; i<N; ++i) {
            SnapshotItem it;
            it.nodeIdStr = nodes[i].second;
            it.ns        = nodes[i].first;
            UA_DataValue_init(&it.dv);
            // Deep copy, damit wir Response danach gefahrlos freigeben können
            if(UA_DataValue_copy(&resp.results[i], &it.dv) != UA_STATUSCODE_GOOD) {
                ok = false;
                // teilweises Aufräumen weiter unten
                break;
            }
            out.push_back(std::move(it));
        }
    }

    // 4) Aufräumen
    UA_ReadResponse_clear(&resp);   // gibt intern allokierte Ressourcen frei
    // NodeIds aus rvids freigeben
    for(size_t i=0; i<N; ++i)
        UA_NodeId_clear(&rvids[i].nodeId);
    UA_Array_delete(rvids, N, &UA_TYPES[UA_TYPES_READVALUEID]);

    // Falls copy oben irgendwo scheiterte: bereits kopierte DataValues freigeben
    if(!ok) {
        for(auto& it : out) UA_DataValue_clear(&it.dv);
        out.clear();
    }
    return ok;
}

// Arbeit mit TaskForce
void PLCMonitor::post(UaFn fn) {
    std::lock_guard<std::mutex> lk(qmx_);
    q_.push(std::move(fn));
}
void PLCMonitor::processPosted(size_t max) {
    for(size_t i=0; i<max; ++i) {
        UaFn fn;
        { std::lock_guard<std::mutex> lk(qmx_);
          if(q_.empty()) break;
          fn = std::move(q_.front()); q_.pop(); }
        fn(); // läuft im gleichen Thread, in dem du runIterate() aufrufst
    }
}
//------------------------------------------------------------------------------------------------------
//Für TestServer-Verbindung
// >>> Am Dateiende oder bei den anderen Methoden ergänzen <<<

PLCMonitor::Options
PLCMonitor::TestServerDefaults(const std::string& clientCertDer,
                               const std::string& clientKeyDer,
                               const std::string& endpoint) {
    Options o;
    o.endpoint       = endpoint;           // ua_test_server_secure.cpp: Port 4850, localhost
    o.username       = "user";             // serverseitig aktiv (Username/Password)
    o.password       = "pass";             // siehe Server-Setup
    o.certDerPath    = clientCertDer;      // Client-Zertifikat (DER)
    o.keyDerPath     = clientKeyDer;       // Client-Key (DER)
    o.applicationUri = "urn:example:open62541:TestClient";
    o.nsIndex        = 2;                  // reine Session ist ns-agnostisch; 2 als sinniger Default
    return o;
}

bool PLCMonitor::connectToSecureTestServer(const std::string& clientCertDer,
                                           const std::string& clientKeyDer,
                                           const std::string& endpoint) {
    // Options auf Testserver-Defaults setzen und normale connect()-Logik nutzen
    opt_ = TestServerDefaults(clientCertDer, clientKeyDer, endpoint);
    return connect(); // nutzt bereits Basic256Sha256 + Sign&Encrypt + Username/Pass + Zert/Key
}

bool PLCMonitor::watchTriggerD2(double samplingMs, UA_UInt32 queueSize) {
    // ns=1; s=TriggerD2 kommt aus deinem Testserver
    return subscribeBool("TriggerD2", /*ns*/1, samplingMs, queueSize,
        [](bool b, const UA_DataValue& dv){
            std::cout << "[Client] TriggerD2: "
                      << (b ? "TRUE" : "FALSE")
                      << "  (status=0x" << std::hex << dv.status << std::dec << ")\n";
        });
}