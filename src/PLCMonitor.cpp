#include "PLCMonitor.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>

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
    cfg->outStandingPublishRequests = 4;                                  // :contentReference[oaicite:2]{index=2}
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
