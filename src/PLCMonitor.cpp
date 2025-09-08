#include "PLCMonitor.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <unordered_set>
#include <future>

// ==== Helpers (datei-lokal) =================================================
namespace {

bool loadFile(const std::string& path, UA_ByteString& out) {
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

// ---- helpers: UA_String/NodeId → std::string
inline std::string uaToStdString(const UA_String &s) {
    if (!s.data || s.length == 0) return {};
    return std::string(reinterpret_cast<const char*>(s.data), s.length);
}

inline std::string nodeIdToString(const UA_NodeId &id) {
    UA_String s = UA_STRING_NULL;
    UA_NodeId_print(&id, &s);
    std::string out = uaToStdString(s);
    UA_String_clear(&s);
    return out;
}

inline std::string toStdString(const UA_String &s) {
    return std::string((const char*)s.data, s.length);
}

// Variant → string (einige gängige Typen)
std::string variantToString(const UA_Variant *v, std::string &outTypeName) {
    if (!v || !v->type) { outTypeName = "null"; return "<null>"; }

    if (UA_Variant_isScalar(v)) {
        if (v->type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
            outTypeName = "Boolean";
            return (*(UA_Boolean*)v->data) ? "true" : "false";
        } else if (v->type == &UA_TYPES[UA_TYPES_INT16]) {
            outTypeName = "Int16";
            return std::to_string(*(UA_Int16*)v->data);
        } else if (v->type == &UA_TYPES[UA_TYPES_INT32]) {
            outTypeName = "Int32";
            return std::to_string(*(UA_Int32*)v->data);
        } else if (v->type == &UA_TYPES[UA_TYPES_UINT32]) {
            outTypeName = "UInt32";
            return std::to_string(*(UA_UInt32*)v->data);
        } else if (v->type == &UA_TYPES[UA_TYPES_DOUBLE]) {
            outTypeName = "Double";
            std::ostringstream os; os << *(UA_Double*)v->data; return os.str();
        } else if (v->type == &UA_TYPES[UA_TYPES_FLOAT]) {
            outTypeName = "Float";
            std::ostringstream os; os << *(UA_Float*)v->data; return os.str();
        } else if (v->type == &UA_TYPES[UA_TYPES_STRING]) {
            outTypeName = "String";
            UA_String *s = (UA_String*)v->data;
            return toStdString(*s);
        } else {
            outTypeName = "Scalar";
            return "<scalar>";
        }
    } else {
        outTypeName = "Array";
        return "<array>";
    }
}

// Browsed **eine** Ebene (FORWARD, HierarchicalReferences) unterhalb von 'start'.
UA_StatusCode browseOneDeep(UA_Client *client,
                            const UA_NodeId &start,
                            std::vector<UA_ReferenceDescription> &out) {
    out.clear();

    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = start;
    bd.resultMask = UA_BROWSERESULTMASK_ALL;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    bd.includeSubtypes = true;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;

    UA_BrowseRequest brq;
    UA_BrowseRequest_init(&brq);
    brq.nodesToBrowse = &bd;
    brq.nodesToBrowseSize = 1;
    brq.requestedMaxReferencesPerNode = 0; // Server entscheidet

    UA_BrowseResponse brs = UA_Client_Service_browse(client, brq);

    if (brs.responseHeader.serviceResult == UA_STATUSCODE_GOOD && brs.resultsSize > 0) {
        auto &res = brs.results[0];
        out.reserve(res.referencesSize);
        for (size_t i = 0; i < res.referencesSize; ++i)
            out.push_back(res.references[i]);
    }

    UA_StatusCode rc = brs.responseHeader.serviceResult;
    UA_BrowseResponse_clear(&brs);
    return rc;
}

} // namespace (helpers)

// ==== PLCMonitor – Basics ====================================================
PLCMonitor::PLCMonitor(Options o) : opt_(std::move(o)) {}
PLCMonitor::~PLCMonitor() { disconnect(); }

bool PLCMonitor::loadFileToByteString(const std::string& path, UA_ByteString &out) {
    return loadFile(path, out);
}

bool PLCMonitor::connect() {
    disconnect();

    client_ = UA_Client_new();
    if(!client_) return false;

    UA_ClientConfig* cfg = UA_Client_getConfig(client_);
    UA_ClientConfig_setDefault(cfg);

    cfg->outStandingPublishRequests = 5;
    cfg->securityMode = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
    cfg->securityPolicyUri = UA_STRING_ALLOC(
        const_cast<char*>("http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"));

    if(!opt_.applicationUri.empty())
        cfg->clientDescription.applicationUri = UA_STRING_ALLOC(
            const_cast<char*>(opt_.applicationUri.c_str()));

    UA_ByteString cert = UA_BYTESTRING_NULL;
    UA_ByteString key  = UA_BYTESTRING_NULL;
    if(!loadFile(opt_.certDerPath, cert) || !loadFile(opt_.keyDerPath, key)) {
        std::fprintf(stderr, "Failed to load cert/key\n");
        return false;
    }

    UA_StatusCode st = UA_ClientConfig_setDefaultEncryption(
        cfg, cert, key, /*trustList*/nullptr, 0, /*revocation*/nullptr, 0);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "Encryption setup failed: 0x%08x\n", st);
        UA_Client_delete(client_); client_ = nullptr;
        return false;
    }

    st = UA_Client_connectUsername(client_,
                                   opt_.endpoint.c_str(),
                                   opt_.username.c_str(),
                                   opt_.password.c_str());
    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "Connect failed: 0x%08x\n", st);
        UA_Client_delete(client_); client_ = nullptr;
        return false;
    }

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
        if(subId_) {
            UA_Client_Subscriptions_deleteSingle(client_, subId_);
            subId_ = 0; monIdInt16_ = 0; monIdBool_ = 0;
        }
        UA_Client_disconnect(client_);
        UA_Client_delete(client_);
        client_ = nullptr;
    }
}

UA_StatusCode PLCMonitor::runIterate(int timeoutMs) {
    if(!client_) return UA_STATUSCODE_BADSERVERNOTCONNECTED;
    return UA_Client_run_iterate(client_, timeoutMs);
}

bool PLCMonitor::waitUntilActivated(int timeoutMs) {
    if(!client_) return false;

    auto t0 = std::chrono::steady_clock::now();
    for(;;) {
        UA_SecureChannelState scState;
        UA_SessionState      ssState;
        UA_StatusCode status;
        (void)UA_Client_run_iterate(client_, 50);

        UA_Client_getState(client_, &scState, &ssState, &status);
        if(scState == UA_SECURECHANNELSTATE_OPEN &&
           ssState == UA_SESSIONSTATE_ACTIVATED)
            return true;

        if(std::chrono::steady_clock::now() - t0 >
           std::chrono::milliseconds(timeoutMs))
            return false;
    }
}

// ==== Reads & Writes =========================================================
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

bool PLCMonitor::writeBool(const std::string& nodeIdStr, UA_UInt16 ns, bool value) {
    if(!client_) return false;

    UA_NodeId nid = UA_NODEID_STRING_ALLOC(ns, const_cast<char*>(nodeIdStr.c_str()));

    UA_Variant v; UA_Variant_init(&v);
    UA_Boolean b = value;
    UA_Variant_setScalarCopy(&v, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);

    UA_StatusCode rc = UA_Client_writeValueAttribute(client_, nid, &v);
    std::cout << "WriteBool " << nodeIdStr << " = " << (value ? "true" : "false")
              << " -> " << UA_StatusCode_name(rc) << "\n";
    UA_Variant_clear(&v);
    UA_NodeId_clear(&nid);
    return rc == UA_STATUSCODE_GOOD;
}

// ==== Subscriptions ==========================================================
bool PLCMonitor::subscribeInt16(const std::string& nodeIdStr, UA_UInt16 nsIndex,
                                double samplingMs, UA_UInt32 queueSize, Int16ChangeCallback cb) {
    if(!client_) return false;
    onInt16Change_ = std::move(cb);

    if(subId_ == 0) {
        UA_CreateSubscriptionRequest sReq = UA_CreateSubscriptionRequest_default();
        sReq.requestedPublishingInterval = 20.0;
        sReq.requestedMaxKeepAliveCount  = 20;
        sReq.requestedLifetimeCount      = 60;

        UA_CreateSubscriptionResponse sResp =
            UA_Client_Subscriptions_create(client_, sReq, /*subCtx*/this, nullptr, nullptr);
        if(sResp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) return false;
        subId_ = sResp.subscriptionId;
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
    return true;
}

bool PLCMonitor::subscribeBool(const std::string& nodeIdStr, UA_UInt16 nsIndex,
                               double samplingMs, UA_UInt32 queueSize, BoolChangeCallback cb) {
    if(!client_) return false;
    onBoolChange_ = std::move(cb);

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

    if(monRes.statusCode != UA_STATUSCODE_GOOD)
        return false;

    monIdBool_ = monRes.monitoredItemId;
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
    onBoolChange_  = nullptr;
}

void PLCMonitor::dataChangeHandler(UA_Client*,
                                   UA_UInt32 /*subId*/, void* subCtx,
                                   UA_UInt32 monId, void* monCtx,
                                   UA_DataValue* value) {
    PLCMonitor* self = static_cast<PLCMonitor*>(monCtx ? monCtx : subCtx);
    if(!self || !value || !value->hasValue) return;

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

// ==== ReadSnapshot (mehrere Knoten) =========================================
bool PLCMonitor::readSnapshot(const std::vector<std::pair<UA_UInt16,std::string>>& nodes,
                              std::vector<SnapshotItem>& out,
                              UA_TimestampsToReturn ttr, double maxAgeMs) {
    if(!client_ || nodes.empty()) return false;

    const size_t N = nodes.size();
    UA_ReadValueId* rvids =
        (UA_ReadValueId*)UA_Array_new(N, &UA_TYPES[UA_TYPES_READVALUEID]);
    if(!rvids) return false;

    for(size_t i=0; i<N; ++i) {
        UA_ReadValueId_init(&rvids[i]);
        rvids[i].nodeId = UA_NODEID_STRING_ALLOC(
            nodes[i].first, const_cast<char*>(nodes[i].second.c_str()));
        rvids[i].attributeId = UA_ATTRIBUTEID_VALUE;
    }

    UA_ReadRequest req; UA_ReadRequest_init(&req);
    req.nodesToRead       = rvids;
    req.nodesToReadSize   = (UA_UInt32)N;
    req.timestampsToReturn= ttr;
    req.maxAge            = (UA_Double)(maxAgeMs);

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
            if(UA_DataValue_copy(&resp.results[i], &it.dv) != UA_STATUSCODE_GOOD) {
                ok = false;
                break;
            }
            out.push_back(std::move(it));
        }
    }

    UA_ReadResponse_clear(&resp);
    for(size_t i=0; i<N; ++i)
        UA_NodeId_clear(&rvids[i].nodeId);
    UA_Array_delete(rvids, N, &UA_TYPES[UA_TYPES_READVALUEID]);

    if(!ok) {
        for(auto& it : out) UA_DataValue_clear(&it.dv);
        out.clear();
    }
    return ok;
}

// ==== Task-Queue =============================================================
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

// ==== Testserver-Komfort =====================================================
PLCMonitor::Options
PLCMonitor::TestServerDefaults(const std::string& clientCertDer,
                               const std::string& clientKeyDer,
                               const std::string& endpoint) {
    Options o;
    o.endpoint       = endpoint;
    o.username       = "user";
    o.password       = "pass";
    o.certDerPath    = clientCertDer;
    o.keyDerPath     = clientKeyDer;
    o.applicationUri = "urn:example:open62541:TestClient";
    o.nsIndex        = 2;
    return o;
}

bool PLCMonitor::connectToSecureTestServer(const std::string& clientCertDer,
                                           const std::string& clientKeyDer,
                                           const std::string& endpoint) {
    opt_ = TestServerDefaults(clientCertDer, clientKeyDer, endpoint);
    return connect();
}

bool PLCMonitor::watchTriggerD2(double samplingMs, UA_UInt32 queueSize) {
    return subscribeBool("TriggerD2", /*ns*/1, samplingMs, queueSize,
        [](bool b, const UA_DataValue& dv){
            std::cout << "[Client] TriggerD2: "
                      << (b ? "TRUE" : "FALSE")
                      << "  (status=0x" << std::hex << dv.status << std::dec << ")\n";
        });
}

// ==== Snapshot: Browse+Read aller Variablen =================================
bool PLCMonitor::TestServer_snapshotAllSync_impl(std::vector<SnapshotEntry>& out,
                                                 int nsFilter,
                                                 unsigned maxDepth,
                                                 unsigned /*maxRefsPerNode*/) {
    out.clear();
    if (!client_) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT, "Snapshot: no client");
        return false;
    }

    struct StackItem { UA_NodeId id; std::string path; unsigned depth; };
    std::vector<StackItem> stack;

    // Start bei /Objects
    StackItem root;
    UA_NodeId_init(&root.id);
    root.id = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    root.path = "/Objects";
    root.depth = 0;
    stack.push_back(root);

    while (!stack.empty()) {
        StackItem cur = stack.back();
        stack.pop_back();

        // eine Ebene browsen
        std::vector<UA_ReferenceDescription> refs;
        UA_StatusCode brc = browseOneDeep(client_, cur.id, refs);
        if (brc != UA_STATUSCODE_GOOD)
            continue;

        for (const auto &rd : refs) {
            // Namespace-Filter (optional)
            if (nsFilter >= 0 && static_cast<int>(rd.nodeId.nodeId.namespaceIndex) != nsFilter)
                continue;

            const UA_NodeId &target = rd.nodeId.nodeId;
            const std::string dn    = uaToStdString(rd.displayName.text);

            SnapshotEntry e;
            e.nodeIdText  = nodeIdToString(target);
            e.displayName = dn;
            e.browsePath  = cur.path + "/" + dn;
            e.nodeClass   = rd.nodeClass;
            e.status      = UA_STATUSCODE_GOOD;
            e.dataType.clear();
            e.value.clear();

            // Wenn Variable → Wert lesen
            if (rd.nodeClass == UA_NODECLASS_VARIABLE) {
                UA_Variant val;
                UA_Variant_init(&val);

                UA_StatusCode rc = UA_Client_readValueAttribute(client_, target, &val);
                e.status = rc;

                if (rc == UA_STATUSCODE_GOOD && UA_Variant_isScalar(&val)) {
                    std::string typeName;
                    e.value    = variantToString(&val, typeName);
                    e.dataType = std::move(typeName);
                }

                UA_Variant_clear(&val);
            }

            out.push_back(std::move(e));

            // Tiefer gehen nur bei OBJECT/VIEW (kein UA_NODECLASS_FOLDER!)
            if (cur.depth < maxDepth &&
               (rd.nodeClass == UA_NODECLASS_OBJECT || rd.nodeClass == UA_NODECLASS_VIEW)) {
                StackItem next;
                UA_NodeId_init(&next.id);
                UA_NodeId_copy(&target, &next.id);
                next.path  = out.back().browsePath; // bereits berechnet
                next.depth = cur.depth + 1;
                stack.push_back(std::move(next));
            }
        }
        // cur.id ist hier Numeric Id → kein clear nötig
    }

    return true;
}

std::vector<PLCMonitor::SnapshotEntry>
PLCMonitor::TestServer_snapshotAllSync(int nsFilter, int maxDepth, std::size_t maxRefsPerNode) {
    std::vector<SnapshotEntry> out;
    TestServer_snapshotAllSync_impl(out,
                                    nsFilter,
                                    static_cast<unsigned>(maxDepth),
                                    static_cast<unsigned>(maxRefsPerNode));
    return out;
}

std::future<std::vector<PLCMonitor::SnapshotEntry>>
PLCMonitor::TestServer_snapshotAllAsync(int nsFilter, unsigned maxDepth, unsigned maxRefsPerNode) {
    return std::async(std::launch::async, [this, nsFilter, maxDepth, maxRefsPerNode]() {
        std::vector<SnapshotEntry> out;
        TestServer_snapshotAllSync_impl(out, nsFilter, maxDepth, maxRefsPerNode);
        return out;
    });
}
