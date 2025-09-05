#include "TaskForce.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>

// Lokaler Loader (identisch vom Prinzip wie in PLCMonitor.cpp)
static bool loadFile(const std::string& path, UA_ByteString& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f) return false;
    const auto len = f.tellg();
    if(len <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.length = (size_t)len;
    out.data   = (UA_Byte*)UA_malloc(out.length);
    if(!out.data) { out.length = 0; return false; }
    return (bool)f.read((char*)out.data, len);
}

TaskForce::TaskForce(const PLCMonitor::Options& opt,
                         UA_UInt16 nsIndex,
                         std::string obj, std::string meth,
                         std::string diag, unsigned timeout,
                         FinishedCb cb)
: opt_(opt), ns_(nsIndex),
  objNode_(std::move(obj)),
  methNode_(std::move(meth)),
  diagNode_(std::move(diag)),
  callTimeoutMs_(timeout),
  onFinished_(std::move(cb)) {
    th_ = std::thread(&TaskForce::worker, this); // sofort starten
}

TaskForce::~TaskForce() {
    run_ = false;
    if(th_.joinable()) th_.join();
    disconnect();
}


bool TaskForce::connect() {
    disconnect();

    c_ = UA_Client_new();
    if(!c_) return false;

    UA_ClientConfig* cfg = UA_Client_getConfig(c_);
    UA_ClientConfig_setDefault(cfg);
    cfg->timeout = 60000;
    // *** exakt wie PLCMonitor::connect() ***
    cfg->outStandingPublishRequests = 5;
    cfg->securityMode = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
    cfg->securityPolicyUri = UA_STRING_ALLOC(
        const_cast<char*>("http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"));

    if(!opt_.applicationUri.empty()) {
        cfg->clientDescription.applicationUri =
            UA_STRING_ALLOC(const_cast<char*>(opt_.applicationUri.c_str()));
    }

    // Zert + Key (DER) laden
    UA_ByteString cert = UA_BYTESTRING_NULL, key = UA_BYTESTRING_NULL;
    if(!loadFile(opt_.certDerPath, cert) || !loadFile(opt_.keyDerPath, key)) {
        std::fprintf(stderr, "[TaskForce] Failed to load cert/key\n");
        UA_Client_delete(c_); c_ = nullptr; return false;
    }

    UA_StatusCode st = UA_ClientConfig_setDefaultEncryption(
        cfg, cert, key, /*trustList*/nullptr, 0, /*revocation*/nullptr, 0);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "[TaskForce] Encryption setup failed: 0x%08x\n", st);
        UA_Client_delete(c_); c_ = nullptr; return false;
    }

    // Username/Passwort
    if(!opt_.username.empty())
        st = UA_Client_connectUsername(c_, opt_.endpoint.c_str(),
                                       opt_.username.c_str(), opt_.password.c_str());
    else
        st = UA_Client_connect(c_, opt_.endpoint.c_str());

    if(st != UA_STATUSCODE_GOOD) {
        std::fprintf(stderr, "[TaskForce] Connect failed: 0x%08x\n", st);
        UA_Client_delete(c_); c_ = nullptr; return false;
    }

    // optional analog zu PLCMonitor, aber nicht benötigt, da synchrone Methode im Hintergrund
    //(void)waitUntilActivated(3000);
    return true;
}

void TaskForce::disconnect() {
    if(c_) { UA_Client_disconnect(c_); UA_Client_delete(c_); c_ = nullptr; }
}

bool TaskForce::waitUntilActivated(int timeoutMs) {
    if(!c_) return false;
    auto t0 = std::chrono::steady_clock::now();
    for(;;) {
        UA_SecureChannelState scState; UA_SessionState ssState; UA_StatusCode st;
        (void)UA_Client_run_iterate(c_, 50);
        UA_Client_getState(c_, &scState, &ssState, &st);
        if(scState == UA_SECURECHANNELSTATE_OPEN && ssState == UA_SESSIONSTATE_ACTIVATED)
            return true;
        if(std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs))
            return false;
    }
}

bool TaskForce::writeBool(const std::string& id, bool v) {
    if(!c_) return false;

    UA_NodeId nid = UA_NODEID_STRING_ALLOC(ns_, const_cast<char*>(id.c_str()));

    UA_Variant val; UA_Variant_init(&val);
    UA_Boolean b = v ? UA_TRUE : UA_FALSE;
    // macht eine tiefe Kopie in den Variant
    UA_StatusCode st = UA_Variant_setScalarCopy(&val, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(st != UA_STATUSCODE_GOOD) { UA_NodeId_clear(&nid); return false; }

    st = UA_Client_writeValueAttribute(c_, nid, &val);

    UA_Variant_clear(&val);
    UA_NodeId_clear(&nid);
    return st == UA_STATUSCODE_GOOD;
}

bool TaskForce::callJobMethod(UA_Int32 x, UA_Int32& yOut) {
    if(!c_) return false;
    UA_NodeId obj  = UA_NODEID_STRING_ALLOC(ns_, const_cast<char*>(objNode_.c_str()));
    UA_NodeId meth = UA_NODEID_STRING_ALLOC(ns_, const_cast<char*>(methNode_.c_str()));

    UA_Variant in[1]; UA_Variant_init(&in[0]);
    UA_Variant_setScalar(&in[0], &x, &UA_TYPES[UA_TYPES_INT32]);

    size_t outSz = 0; UA_Variant* out = nullptr;
    // Blockiert bis Job fertig (Timeout via cfg->timeout / callTimeoutMs_)
    UA_Client_getConfig(c_)->timeout = callTimeoutMs_;
    UA_StatusCode st = UA_Client_call(c_, obj, meth, 1, in, &outSz, &out);

    UA_NodeId_clear(&obj);
    UA_NodeId_clear(&meth);

    bool ok = (st == UA_STATUSCODE_GOOD) &&
              (outSz >= 1) &&
              UA_Variant_isScalar(&out[0]) &&
              out[0].type == &UA_TYPES[UA_TYPES_INT32] &&
              out[0].data != nullptr;
    if(ok) yOut = *static_cast<UA_Int32*>(out[0].data);

    if(out && outSz > 0) {
    UA_Array_delete(out, outSz, &UA_TYPES[UA_TYPES_VARIANT]);
    out = nullptr;
    }
    return ok;
}

//void TaskForce::notifyTrigger() { trig_.store(true, std::memory_order_relaxed); }

bool TaskForce::callJob(UA_Int32 x, UA_Int32& yOut) {
    if(!c_) return false;
    UA_Client_getConfig(c_)->timeout = callTimeoutMs_; // langer Call
    UA_NodeId obj  = UA_NODEID_STRING_ALLOC(ns_, const_cast<char*>(objNode_.c_str()));
    UA_NodeId meth = UA_NODEID_STRING_ALLOC(ns_, const_cast<char*>(methNode_.c_str()));

    UA_Variant in[1]; UA_Variant_init(&in[0]);
    UA_Variant_setScalar(&in[0], &x, &UA_TYPES[UA_TYPES_INT32]);

    size_t outSz = 0; UA_Variant* out = nullptr;
    auto st = UA_Client_call(c_, obj, meth, 1, in, &outSz, &out); // blockiert bis Job fertig
    UA_NodeId_clear(&obj); UA_NodeId_clear(&meth);

    bool ok = (st == UA_STATUSCODE_GOOD) && outSz >= 1 &&
              UA_Variant_isScalar(&out[0]) &&
              out[0].type == &UA_TYPES[UA_TYPES_INT32] && out[0].data;
    if(ok) yOut = *static_cast<UA_Int32*>(out[0].data);
    if(out) UA_Array_delete(out, outSz, &UA_TYPES[UA_TYPES_VARIANT]);
    return ok;
}

void TaskForce::worker() {
    if(!connect()) {
        state_ = State::Failed;
        std::cout << "[TaskForce] Connect failed, State:: Failed" << std::endl;
        if(onFinished_) onFinished_(false, 0);
        return;
    }

    state_ = State::Calling;
    std::cout << "State:: Calling" << std::endl;
    UA_Int32 y = 0;
    const bool ok = callJob(/*x*/0, y);

    // DiagnoseFinished-Puls
    writeBool(diagNode_, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writeBool(diagNode_, false);

    if(onFinished_) onFinished_(ok, ok ? y : 0);

    state_ = ok ? State::Done : State::Failed;
    std::cout << (ok ? "State::Done" : "State::Failed") << '\n';
    disconnect(); // Session schließen
    run_ = false; // Thread endet jetzt
}
