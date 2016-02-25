/**
 * @file
 *
 * This file implements the org.alljoyn.Bus.Peer.* interfaces
 */

/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include <qcc/platform.h>

#include <assert.h>

#include <qcc/Debug.h>
#include <qcc/String.h>
#include <qcc/Crypto.h>
#include <qcc/Util.h>
#include <qcc/StringUtil.h>
#include <qcc/StringSink.h>
#include <qcc/StringSource.h>

#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/Message.h>
#include <alljoyn/MessageReceiver.h>

#include "SessionInternal.h"
#include "KeyStore.h"
#include "BusEndpoint.h"
#include "PeerState.h"
#include "AllJoynPeerObj.h"
#include "SASLEngine.h"
#include "AllJoynCrypto.h"
#include "BusInternal.h"

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

/*
 * Version number of the key generation algorithm.
 */
static const uint32_t MIN_KEYGEN_VERSION = 0x00;
static const uint32_t MAX_KEYGEN_VERSION = 0x01;

/*
 * The base authentication version number
 */
static const uint32_t MIN_AUTH_VERSION = 0x0001;
static const uint32_t MAX_AUTH_VERSION = 0x0004;

/**
 * starting version with capability of supporting membership certificates.
 */
static const uint32_t CAPABLE_MEMBERSHIP_CERT_VERSION = 0x0004;

static const uint32_t PREFERRED_AUTH_VERSION = (MAX_AUTH_VERSION << 16) | MIN_KEYGEN_VERSION;

/*
 * the protocol version of the ECDHE_ECDSA with non X.509 certificate
 */
static const uint32_t NON_ECDSA_X509_VERSION = 0x0002;

static bool IsCompatibleVersion(uint32_t version)
{
    uint16_t authV = version >> 16;
    uint8_t keyV = version & 0xFF;

    if ((authV < MIN_AUTH_VERSION) || (authV > MAX_AUTH_VERSION)) {
        return false;
    }
    //The llvm clang compiler will complain about the code `keyV < MIN_KEYGEN_VERSION`
    // will always return false.  Which is true as long as MIN_KEYGEN_VERSION is
    // zero however we don't want to remove this check because the MIN_KEYGEN_VERSION
    // may be changed in the future. If it is changed we want this check to still
    // be in the code.
#if defined __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-compare"
#endif
    if ((keyV < MIN_KEYGEN_VERSION) || (keyV > MAX_KEYGEN_VERSION)) {
        return false;
    }
#if defined __clang__
#pragma clang diagnostic pop
#endif
    return (version & 0xFF00) == 0;
}

static bool IsMembershipCertCapable(uint32_t version)
{
    uint16_t authV = version >> 16;
    return (authV >= CAPABLE_MEMBERSHIP_CERT_VERSION);
}

static uint32_t GetLowerVersion(uint32_t v1, uint32_t v2)
{
    uint16_t authV1 = v1 >> 16;
    uint8_t keyV1 = v1 & 0xFF;
    uint16_t authV2 = v2 >> 16;
    uint8_t keyV2 = v2 & 0xFF;

    if (authV1 < authV2) {
        return v1;
    }
    if (authV1 > authV2) {
        return v2;
    }
    if (keyV1 <= keyV2) {
        return v1;
    }
    return v2;
}

static bool UseKeyExchanger(uint32_t peerAuthVersion, const uint32_t*authMaskList, size_t authCount)
{
    uint16_t authV = peerAuthVersion >> 16;
    if (authV < 2) {
        return false;
    }
    for (size_t cnt = 0; cnt < authCount; cnt++) {
        uint32_t suite = authMaskList[cnt];
        if ((suite & AUTH_KEYX_ECDHE) == AUTH_KEYX_ECDHE) {
            return true;
        }
    }
    return false;
}

static void SetRights(PeerState& peerState, bool mutual, bool challenger)
{
    if (mutual) {
        QCC_DbgHLPrintf(("SetRights mutual"));
        peerState->SetAuthorization(MESSAGE_METHOD_CALL, _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
        peerState->SetAuthorization(MESSAGE_METHOD_RET,  _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
        peerState->SetAuthorization(MESSAGE_ERROR,       _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
        peerState->SetAuthorization(MESSAGE_SIGNAL,      _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
    } else {
        if (challenger) {
            QCC_DbgHLPrintf(("SetRights challenger"));
            /*
             * We are the challenger in the auth conversation. The authentication was one-side so we
             * will accept encrypted calls from the remote peer but will not send them.
             */
            peerState->SetAuthorization(MESSAGE_METHOD_CALL, _PeerState::ALLOW_SECURE_RX);
            peerState->SetAuthorization(MESSAGE_METHOD_RET,  _PeerState::ALLOW_SECURE_TX);
            peerState->SetAuthorization(MESSAGE_ERROR,       _PeerState::ALLOW_SECURE_TX);
            peerState->SetAuthorization(MESSAGE_SIGNAL,      _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
        } else {
            QCC_DbgHLPrintf(("SetRights responder"));
            /*
             * We initiated the authentication and responded to challenges from the remote peer. The
             * authentication was not mutual so we are not going to allow encrypted method calls
             * from the remote peer.
             */
            peerState->SetAuthorization(MESSAGE_METHOD_CALL, _PeerState::ALLOW_SECURE_TX);
            peerState->SetAuthorization(MESSAGE_METHOD_RET,  _PeerState::ALLOW_SECURE_RX);
            peerState->SetAuthorization(MESSAGE_ERROR,       _PeerState::ALLOW_SECURE_RX);
            peerState->SetAuthorization(MESSAGE_SIGNAL,      _PeerState::ALLOW_SECURE_TX | _PeerState::ALLOW_SECURE_RX);
        }
    }
}

AllJoynPeerObj::AllJoynPeerObj(BusAttachment& bus) :
    BusObject(org::alljoyn::Bus::Peer::ObjectPath, false),
    AlarmListener(),
    dispatcher("PeerObjDispatcher", true, 3), supportedAuthSuitesCount(0), supportedAuthSuites(NULL), securityApplicationObj(bus)
{
    /* Add org.alljoyn.Bus.Peer.Authentication interface */
    {
        const InterfaceDescription* ifc = bus.GetInterface(org::alljoyn::Bus::Peer::Authentication::InterfaceName);
        if (ifc) {
            AddInterface(*ifc);
            AddMethodHandler(ifc->GetMember("AuthChallenge"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::AuthChallenge));
            AddMethodHandler(ifc->GetMember("ExchangeGuids"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::ExchangeGuids));
            AddMethodHandler(ifc->GetMember("ExchangeSuites"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::ExchangeSuites));
            AddMethodHandler(ifc->GetMember("KeyExchange"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::KeyExchange));
            AddMethodHandler(ifc->GetMember("KeyAuthentication"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::KeyAuthentication));
            AddMethodHandler(ifc->GetMember("GenSessionKey"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::GenSessionKey));
            AddMethodHandler(ifc->GetMember("ExchangeGroupKeys"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::ExchangeGroupKeys));
            AddMethodHandler(ifc->GetMember("SendManifest"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::HandleSendManifest));
            AddMethodHandler(ifc->GetMember("SendMemberships"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::SendMemberships));
        }
    }
    /* Add org.alljoyn.Bus.Peer.Session interface */
    {
        const InterfaceDescription* ifc = bus.GetInterface(org::alljoyn::Bus::Peer::Session::InterfaceName);
        if (ifc) {
            AddInterface(*ifc);
            AddMethodHandler(ifc->GetMember("AcceptSession"), static_cast<MessageReceiver::MethodHandler>(&AllJoynPeerObj::AcceptSession));
            bus.RegisterSignalHandler(
                this,
                static_cast<MessageReceiver::SignalHandler>(&AllJoynPeerObj::SessionJoined),
                ifc->GetMember("SessionJoined"),
                NULL);
        }
    }
}

QStatus AllJoynPeerObj::Start()
{
    assert(bus);
    bus->RegisterBusListener(*this);
    dispatcher.Start();
    return ER_OK;
}

QStatus AllJoynPeerObj::Stop()
{
    assert(bus);
    dispatcher.Stop();
    bus->UnregisterBusListener(*this);
    return ER_OK;
}

QStatus AllJoynPeerObj::Join()
{
    lock.Lock(MUTEX_CONTEXT);
    std::map<qcc::String, SASLEngine*>::iterator iter = conversations.begin();
    while (iter != conversations.end()) {
        delete iter->second;
        ++iter;
    }
    conversations.clear();
    keyExConversations.clear();
    lock.Unlock(MUTEX_CONTEXT);

    dispatcher.Join();
    return ER_OK;
}

AllJoynPeerObj::~AllJoynPeerObj()
{
    delete [] supportedAuthSuites;
    supportedAuthSuites = NULL;
}

QStatus AllJoynPeerObj::Init(BusAttachment& peerBus)
{
    QStatus status = securityApplicationObj.Init();
    if (ER_OK != status) {
        QCC_LogError(status, ("PermissionMgmtObj Initialization failed"));
        return status;
    }
    status = peerBus.RegisterBusObject(*this);
    return status;
}

void AllJoynPeerObj::ObjectRegistered(void)
{
    /* Must call base class */
    BusObject::ObjectRegistered();

}

QStatus AllJoynPeerObj::RequestAuthentication(Message& msg)
{
    return DispatchRequest(msg, AUTHENTICATE_PEER);
}

/*
 * Get a property
 */
QStatus AllJoynPeerObj::Get(const char* ifcName, const char* propName, MsgArg& val)
{
    QStatus status = ER_BUS_NO_SUCH_PROPERTY;

    if (strcmp(ifcName, org::alljoyn::Bus::Peer::Authentication::InterfaceName) == 0) {
        if (strcmp("Mechanisms", propName) == 0) {
            val.typeId = ALLJOYN_STRING;
            val.v_string.str = peerAuthMechanisms.c_str();
            val.v_string.len = peerAuthMechanisms.size();
            status = ER_OK;
        }
    }
    return status;
}

void AllJoynPeerObj::ExchangeGroupKeys(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);
    assert(bus);

    QStatus status;
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();

    /*
     * We expect to know the peer that is making this method call
     */
    if (peerStateTable->IsKnownPeer(msg->GetSender())) {
        PeerState peerState = peerStateTable->GetPeerState(msg->GetSender());
        uint8_t keyGenVersion = peerState->GetAuthVersion() & 0xFF;
        uint16_t authV = peerState->GetAuthVersion() >> 16;
        uint8_t sendKeyBlob = (authV <= 1) && (keyGenVersion == 0);
        QCC_DbgHLPrintf(("ExchangeGroupKeys using key gen version %d", keyGenVersion));
        /*
         * KeyGen version 0 exchanges key blobs, version 1 just exchanges the key
         */
        KeyBlob key;
        if (sendKeyBlob) {
            StringSource src(msg->GetArg(0)->v_scalarArray.v_byte, msg->GetArg(0)->v_scalarArray.numElements);
            status = key.Load(src);
        } else {
            status = key.Set(msg->GetArg(0)->v_scalarArray.v_byte, msg->GetArg(0)->v_scalarArray.numElements, KeyBlob::AES);
        }
        if (status == ER_OK) {
            /*
             * Tag the group key with the auth mechanism used by ExchangeGroupKeys. Group keys
             * are inherently directional - only initiator encrypts with the group key. We set
             * the role to NO_ROLE otherwise senders can't decrypt their own broadcast messages.
             */
            key.SetTag(msg->GetAuthMechanism(), KeyBlob::NO_ROLE);
            peerState->SetKey(key, PEER_GROUP_KEY);
            /*
             * Return the local group key.
             */
            peerStateTable->GetGroupKey(key);
            StringSink snk;
            MsgArg replyArg;
            if (sendKeyBlob) {
                key.Store(snk);
                replyArg.Set("ay", snk.GetString().size(), snk.GetString().data());
            } else {
                replyArg.Set("ay", key.GetSize(), key.GetData());
            }
            MethodReply(msg, &replyArg, 1);
        }
    } else {
        status = ER_BUS_NO_PEER_GUID;
    }
    if (status != ER_OK) {
        MethodReply(msg, status);
    }
}

void AllJoynPeerObj::ExchangeGuids(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);
    assert(bus);

    qcc::GUID128 remotePeerGuid(msg->GetArg(0)->v_string.str);
    uint32_t authVersion = msg->GetArg(1)->v_uint32;

    qcc::String localGuidStr = bus->GetInternal().GetKeyStore().GetGuid();
    if (!localGuidStr.empty()) {
        PeerState peerState = bus->GetInternal().GetPeerStateTable()->GetPeerState(msg->GetSender());
        /*
         * If don't support the proposed version reply with our preferred version
         */
        if (!IsCompatibleVersion(authVersion)) {
            authVersion = PREFERRED_AUTH_VERSION;
        } else {
            authVersion = GetLowerVersion(authVersion, PREFERRED_AUTH_VERSION);
        }
        QCC_DbgHLPrintf(("ExchangeGuids Local %s", localGuidStr.c_str()));
        QCC_DbgHLPrintf(("ExchangeGuids Remote %s", remotePeerGuid.ToString().c_str()));
        QCC_DbgHLPrintf(("ExchangeGuids AuthVersion %d", authVersion));
        /*
         * If we proposed a different version we simply assume it is acceptable. The remote peer
         * will try a different version or give up if it doesn't like our suggestion.
         */
        peerState->SetGuidAndAuthVersion(remotePeerGuid, authVersion);

        /*
         * Associate the remote peer GUID with the sender peer state.
         */
        MsgArg replyArgs[2];
        replyArgs[0].Set("s", localGuidStr.c_str());
        replyArgs[1].Set("u", authVersion);
        Message replyMsg(*bus);
        MethodReply(msg, replyArgs, ArraySize(replyArgs), &replyMsg);
    } else {
        MethodReply(msg, ER_BUS_NO_PEER_GUID);
    }
}

/*
 * These two lengths are used in RFC 5246
 */
#define VERIFIER_LEN  12
#define NONCE_LEN     28

/*
 * Limit session key lifetime to 2 days.
 */
#define SESSION_KEY_EXPIRATION (60 * 60 * 24 * 2)

QStatus AllJoynPeerObj::KeyGen(PeerState& peerState, String seed, qcc::String& verifier, KeyBlob::Role role)
{
    assert(bus);
    QStatus status;
    KeyStore& keyStore = bus->GetInternal().GetKeyStore();
    KeyBlob peerSecret;
    uint8_t keyGenVersion = peerState->GetAuthVersion() & 0xFF;

    KeyStore::Key key(KeyStore::Key::REMOTE, peerState->GetGuid());
    status = keyStore.GetKey(key, peerSecret, peerState->authorizations);
    if ((status == ER_OK) && peerSecret.HasExpired()) {
        status = ER_BUS_KEY_EXPIRED;
    }
    if (status == ER_OK) {
        String tag = peerSecret.GetTag();
        if (tag == "ALLJOYN_ECDHE_NULL") {
            /* expires the ECDHE_NULL after first use */
            Timespec now;
            GetTimeNow(&now);
            keyStore.SetKeyExpiration(key, now);
        }
    }
    KeyBlob masterSecret;
    if (status == ER_OK) {
        status = KeyExchanger::ParsePeerSecretRecord(peerSecret, masterSecret);
    }
    if (status == ER_OK) {
        size_t keylen = Crypto_AES::AES128_SIZE + VERIFIER_LEN;
        uint8_t* keymatter = new uint8_t[keylen];

        QCC_DbgHLPrintf(("KeyGen using key gen version %d", keyGenVersion));
        if (keyGenVersion == 0) {
            /*
             * Session key is generated using the procedure described in RFC 5246
             */
            status = Crypto_PseudorandomFunction(masterSecret, "session key", seed, keymatter, keylen);
        } else {
            status = ER_CRYPTO_ILLEGAL_PARAMETERS;
        }
        if (status == ER_OK) {
            KeyBlob sessionKey(keymatter, Crypto_AES::AES128_SIZE, KeyBlob::AES);
            /*
             * Tag the session key with auth mechanism tag from the master secret
             */
            sessionKey.SetTag(masterSecret.GetTag(), role);
            sessionKey.SetExpiration(SESSION_KEY_EXPIRATION);
            /*
             * Store session key in the peer state.
             */
            peerState->SetKey(sessionKey, PEER_SESSION_KEY);
            /*
             * Return verifier string
             */
            verifier = BytesToHexString(keymatter + Crypto_AES::AES128_SIZE, VERIFIER_LEN);
        }

        ClearMemory(keymatter, keylen);
        delete [] keymatter;
    }
    /*
     * Store any changes to the key store.
     */
    keyStore.Store();
    return status;
}

void AllJoynPeerObj::GenSessionKey(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);
    assert(bus);

    QStatus status;
    PeerState peerState = bus->GetInternal().GetPeerStateTable()->GetPeerState(msg->GetSender());
    Message replyMsg(*bus);

    /*
     * The hash state may have been previously initialized by ExchangeSuites. If so,
     * ExchangeSuites will also hash the GUIDs.
     */
    peerState->AcquireConversationHashLock();
    if (!peerState->IsConversationHashInitialized()) {
        peerState->InitializeConversationHash();
        this->HashGUIDs(peerState, false);
    }
    peerState->UpdateHash(CONVERSATION_V4, msg);
    peerState->ReleaseConversationHashLock();

    qcc::GUID128 remotePeerGuid(msg->GetArg(0)->v_string.str);
    qcc::GUID128 localPeerGuid(msg->GetArg(1)->v_string.str);
    /*
     * Check that target GUID is our GUID.
     */
    peerState->AcquireConversationHashLock();
    if (bus->GetInternal().GetKeyStore().GetGuid() != localPeerGuid.ToString()) {
        status = ER_BUS_NO_PEER_GUID;
        MethodReply(msg, status, &replyMsg);
    } else {
        qcc::String nonce = RandHexString(NONCE_LEN);
        qcc::String verifier;
        status = KeyGen(peerState, msg->GetArg(2)->v_string.str + nonce, verifier, KeyBlob::RESPONDER);
        if (status == ER_OK) {
            QCC_DbgHLPrintf(("GenSessionKey succeeds for peer %s", msg->GetSender()));
            MsgArg replyArgs[2];
            replyArgs[0].Set("s", nonce.c_str());
            replyArgs[1].Set("s", verifier.c_str());

            MethodReply(msg, replyArgs, ArraySize(replyArgs), &replyMsg);
        } else {
            MethodReply(msg, status, &replyMsg);
        }
    }

    if (status == ER_OK) {
        /* Key has been established successfully. */
        peerState->FreeConversationHash();
    } else {
        peerState->UpdateHash(CONVERSATION_V4, replyMsg);
    }
    peerState->ReleaseConversationHashLock();
}

void AllJoynPeerObj::AuthAdvance(Message& msg)
{
    assert(bus);
    QStatus status = ER_OK;
    ajn::SASLEngine* sasl = NULL;
    ajn::SASLEngine::AuthState authState = SASLEngine::ALLJOYN_AUTH_FAILED;
    qcc::String outStr;
    qcc::String sender = msg->GetSender();
    qcc::String mech;

    /*
     * There can be multiple authentication conversations going on simultaneously between the
     * current peer and other remote peers but only one conversation between each pair.
     *
     * Check for existing conversation and allocate a new SASL engine if we need one
     */
    lock.Lock(MUTEX_CONTEXT);
    sasl = conversations[sender];
    conversations.erase(sender);
    lock.Unlock(MUTEX_CONTEXT);

    if (!sasl) {
        sasl = new SASLEngine(*bus, ajn::AuthMechanism::CHALLENGER, peerAuthMechanisms.c_str(), sender.c_str(), peerAuthListener);
        qcc::String localGuidStr = bus->GetInternal().GetKeyStore().GetGuid();
        if (!localGuidStr.empty()) {
            sasl->SetLocalId(localGuidStr);
        } else {
            status = ER_BUS_NO_PEER_GUID;
        }
    }
    /*
     * Move the authentication conversation forward.
     */
    if (status == ER_OK) {
        status = sasl->Advance(msg->GetArg(0)->v_string.str, outStr, authState);
    }
    /*
     * If auth conversation was sucessful store the master secret in the key store.
     */
    if ((status == ER_OK) && (authState == SASLEngine::ALLJOYN_AUTH_SUCCESS)) {
        PeerState peerState = bus->GetInternal().GetPeerStateTable()->GetPeerState(sender);
        SetRights(peerState, sasl->AuthenticationIsMutual(), true /*challenger*/);
        KeyBlob masterSecret;
        KeyStore& keyStore = bus->GetInternal().GetKeyStore();
        status = sasl->GetMasterSecret(masterSecret);
        mech = sasl->GetMechanism();
        if (status == ER_OK) {
            qcc::GUID128 remotePeerGuid(sasl->GetRemoteId());
            /* Tag the master secret with the auth mechanism used to generate it */
            masterSecret.SetTag(mech, KeyBlob::RESPONDER);
            KeyStore::Key key(KeyStore::Key::REMOTE, remotePeerGuid);
            status = keyStore.AddKey(key, masterSecret, peerState->authorizations);
        }
        /*
         * Report the succesful authentication to allow application to clear UI etc.
         */
        if (status == ER_OK) {
            peerAuthListener.AuthenticationComplete(mech.c_str(), sender.c_str(), true /* success */);
        }
        delete sasl;
        sasl = NULL;
    }

    if (status != ER_OK) {
        /*
         * Report the failed authentication to allow application to clear UI etc.
         */
        peerAuthListener.AuthenticationComplete(mech.c_str(), sender.c_str(), false /* failure */);
        /*
         * Let remote peer know the authentication failed.
         */
        MethodReply(msg, status);
        delete sasl;
        sasl = NULL;
    } else {
        /*
         * If we are not done put the SASL engine back
         */
        if (authState != SASLEngine::ALLJOYN_AUTH_SUCCESS) {
            lock.Lock(MUTEX_CONTEXT);
            conversations[sender] = sasl;
            lock.Unlock(MUTEX_CONTEXT);
        }
        MsgArg replyMsg("s", outStr.c_str());
        MethodReply(msg, &replyMsg, 1);
    }
}

void AllJoynPeerObj::DoKeyExchange(Message& msg)
{
    QStatus status = ER_OK;

    uint32_t authMask = msg->GetArg(0)->v_uint32;
    MsgArg* inVariant;
    status = msg->GetArg(1)->Get("v", &inVariant);

    uint32_t effectiveAuthMask = 0;
    lock.Lock(MUTEX_CONTEXT);
    for (int cnt = 0; cnt < supportedAuthSuitesCount; cnt++) {
        if ((authMask & supportedAuthSuites[cnt]) == authMask) {
            effectiveAuthMask = authMask;
            break;
        }
    }

    if (!effectiveAuthMask) {
        lock.Unlock(MUTEX_CONTEXT);
        status = ER_AUTH_FAIL;
        MethodReply(msg, status);
        return;
    }

    uint32_t authMaskList[1];
    authMaskList[0] = effectiveAuthMask;
    qcc::String sender = msg->GetSender();
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    if (!peerStateTable->IsKnownPeer(sender)) {
        lock.Unlock(MUTEX_CONTEXT);
        status = ER_AUTH_FAIL;
        MethodReply(msg, status);
        return;
    }
    PeerState peerState = peerStateTable->GetPeerState(sender);
    shared_ptr<KeyExchanger> keyExchanger = GetKeyExchangerInstance(peerState, false, authMaskList, 1);
    if (!keyExchanger) {
        lock.Unlock(MUTEX_CONTEXT);
        Message replyMsg(*bus);
        status = ER_AUTH_FAIL;
        peerState->AcquireConversationHashLock();
        MethodReply(msg, status, &replyMsg);
        peerState->UpdateHash(CONVERSATION_V4, replyMsg);
        peerState->ReleaseConversationHashLock();
        return;
    }
    if ((peerState->GetAuthVersion() >> 16) < CONVERSATION_V4) {
        /* any peer with auth version smaller than 4 need to start the hash at
         * the KeyExchange call */
        peerState->AcquireConversationHashLock();
        peerState->InitializeConversationHash();
        peerState->ReleaseConversationHashLock();
    }

    /* storing the key exchanger for the given sender  */
    keyExConversations[sender] = keyExchanger;

    lock.Unlock(MUTEX_CONTEXT);
    keyExchanger->RespondToKeyExchange(msg, inVariant, authMask, effectiveAuthMask);
} /* DoKeyExchange */

QStatus AllJoynPeerObj::RecordMasterSecret(const qcc::String& sender, shared_ptr<KeyExchanger> keyExchanger, PeerState peerState)
{
    qcc::String guidStr;
    bus->GetPeerGUID(sender.c_str(), guidStr);
    qcc::GUID128 remotePeerGuid(guidStr);
    return keyExchanger->StoreMasterSecret(remotePeerGuid, peerState->authorizations);
}

void AllJoynPeerObj::DoKeyAuthentication(Message& msg)
{
    assert(bus);
    QStatus status = ER_OK;
    qcc::String sender = msg->GetSender();
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    PeerState peerState;
    if (peerStateTable->IsKnownPeer(sender)) {
        peerState = peerStateTable->GetPeerState(sender);
    } else {
        return;
    }

    /*
     * There can be multiple authentication conversations going on simultaneously between the
     * current peer and other remote peers but only one conversation between each pair.
     *
     * Check for existing conversation and allocate a new SASL engine if we need one
     */
    lock.Lock(MUTEX_CONTEXT);
    shared_ptr<KeyExchanger> keyExchanger = keyExConversations[sender];
    keyExConversations.erase(sender);
    lock.Unlock(MUTEX_CONTEXT);

    if (!keyExchanger) {
        status = ER_AUTH_FAIL;
    }
    if (status == ER_OK) {
        bool authorized = false;
        MsgArg* variant;
        status = msg->GetArg(0)->Get("v", &variant);
        if (status == ER_OK) {
            status = keyExchanger->ValidateRemoteVerifierVariant(sender.c_str(), variant, (uint8_t*) &authorized);

            /* Hash the received message after ValidateRemoteVerifierVariant so the verifier is correctly computed. */
            peerState->AcquireConversationHashLock();
            peerState->UpdateHash(CONVERSATION_V4, msg);
            peerState->ReleaseConversationHashLock();

            if ((status == ER_OK) && authorized) {
                SetRights(peerState, true, true /*challenger*/);
                status = RecordMasterSecret(sender, keyExchanger, peerState);
                /*
                 * Report the succesful authentication to allow application to clear UI etc.
                 */
                if (status == ER_OK) {
                    peerAuthListener.AuthenticationComplete(keyExchanger->GetSuiteName(), sender.c_str(), true /* success */);
                    /* compute the local verifier to send back */
                    keyExchanger->ReplyWithVerifier(msg);
                    return;
                }
            }
        }
    }

    /* assume failure */
    status = ER_AUTH_FAIL;
    /*
     * Report the failed authentication to allow application to clear UI etc.
     */
    const char* suiteName;
    if (keyExchanger) {
        suiteName = keyExchanger->GetSuiteName();
    } else {
        suiteName = "Unknown";
    }
    peerAuthListener.AuthenticationComplete(suiteName, sender.c_str(), false /* failure */);

    /*
     * Let remote peer know the authentication failed.
     */
    Message replyMsg(*bus);
    peerState->AcquireConversationHashLock();
    MethodReply(msg, status, &replyMsg);
    peerState->UpdateHash(CONVERSATION_V4, replyMsg);
    peerState->ReleaseConversationHashLock();
}

void AllJoynPeerObj::AuthChallenge(const ajn::InterfaceDescription::Member* member, ajn::Message& msg)
{
    QCC_UNUSED(member);
    /*
     * Cannot authenticate if we don't have any authentication mechanisms
     */
    if (peerAuthMechanisms.empty()) {
        MethodReply(msg, ER_BUS_NO_AUTHENTICATION_MECHANISM);
        return;
    }
    /*
     * Authentication may involve user interaction or be computationally expensive so cannot be
     * allowed to block the read thread.
     */
    QStatus status = DispatchRequest(msg, AUTH_CHALLENGE);
    if (status != ER_OK) {
        MethodReply(msg, status);
    }
}

void AllJoynPeerObj::HashGUIDs(PeerState& peerState, bool localFirst)
{
    /* Hash the authentication version and both GUIDs */
    qcc::GUID128 remotePeerGuid = peerState->GetGuid();
    qcc::String guidStr;
    guidStr = bus->GetInternal().GetKeyStore().GetGuid();
    qcc::GUID128 localPeerGuid(guidStr);
    uint32_t authVersionLE = htole32(peerState->GetAuthVersion());

    peerState->AcquireConversationHashLock();
    assert(peerState->IsConversationHashInitialized());
    peerState->UpdateHash(CONVERSATION_V4, (uint8_t*)&authVersionLE, sizeof(uint32_t));
    if (localFirst) {
        peerState->UpdateHash(CONVERSATION_V4, localPeerGuid.GetBytes(), qcc::GUID128::SIZE);
        peerState->UpdateHash(CONVERSATION_V4, remotePeerGuid.GetBytes(), qcc::GUID128::SIZE);
    } else {
        peerState->UpdateHash(CONVERSATION_V4, remotePeerGuid.GetBytes(), qcc::GUID128::SIZE);
        peerState->UpdateHash(CONVERSATION_V4, localPeerGuid.GetBytes(), qcc::GUID128::SIZE);
    }
    peerState->ReleaseConversationHashLock();
}

void AllJoynPeerObj::ExchangeSuites(const ajn::InterfaceDescription::Member* member, ajn::Message& msg)
{
    Message replyMsg(*bus);

    QCC_UNUSED(member);

    uint32_t*remoteSuites;
    size_t remoteSuitesLen;

    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    PeerState peerState = peerStateTable->GetPeerState(msg->GetSender());

    /*
     * The hash state may have been previously initialized by GenSessionKey.
     * If so, GenSessionKey will also hash the GUIDs.
     */
    peerState->AcquireConversationHashLock();
    if (!peerState->IsConversationHashInitialized()) {
        peerState->InitializeConversationHash();
        this->HashGUIDs(peerState, false);
    }
    peerState->ReleaseConversationHashLock();

    QStatus status = msg->GetArg(0)->Get("au", &remoteSuitesLen, &remoteSuites);
    if (status != ER_OK) {
        peerState->AcquireConversationHashLock();
        MethodReply(msg, status, &replyMsg);
        peerState->UpdateHash(CONVERSATION_V4, replyMsg);
        peerState->ReleaseConversationHashLock();
        return;
    }
    peerState->AcquireConversationHashLock();
    peerState->UpdateHash(CONVERSATION_V4, msg);
    peerState->ReleaseConversationHashLock();
    size_t effectiveAuthSuitesCount = 0;
    if (supportedAuthSuitesCount == 0) {
        effectiveAuthSuitesCount = 1;
    } else {
        effectiveAuthSuitesCount = supportedAuthSuitesCount;
    }
    uint32_t* effectiveAuthSuites = new uint32_t[supportedAuthSuitesCount];

    if (supportedAuthSuitesCount == 0) {
        effectiveAuthSuites[0] = 0;
    } else {
        for (size_t cnt = 0; cnt < effectiveAuthSuitesCount; cnt++) {
            effectiveAuthSuites[cnt] = 0;
        }
        int netCnt = 0;
        /* the order of precedence is from the server perspective */
        for (size_t cnt = 0; cnt < supportedAuthSuitesCount; cnt++) {
            for (size_t idx = 0; idx < remoteSuitesLen; idx++) {
                if (supportedAuthSuites[cnt] == remoteSuites[idx]) {
                    bool addIt = true;
                    if (supportedAuthSuites[cnt] == AUTH_SUITE_ECDHE_ECDSA) {
                        /* Does the peer auth version >= 3?  If not, the peer
                           can't handle ECDSA with X.509 certificate */
                        if ((peerState->GetAuthVersion() >> 16) <= NON_ECDSA_X509_VERSION) {
                            addIt = false;
                        }
                    }
                    /* add it */
                    if (addIt) {
                        effectiveAuthSuites[netCnt++] = supportedAuthSuites[cnt];
                    }
                    break;
                }
            }
        }
        effectiveAuthSuitesCount = netCnt;
    }

    MsgArg replyArg;
    replyArg.Set("au", effectiveAuthSuitesCount, effectiveAuthSuites);
    peerState->AcquireConversationHashLock();
    MethodReply(msg, &replyArg, 1, &replyMsg);
    peerState->UpdateHash(CONVERSATION_V4, replyMsg);
    peerState->ReleaseConversationHashLock();
    delete [] effectiveAuthSuites;
}

void AllJoynPeerObj::KeyExchange(const ajn::InterfaceDescription::Member* member, ajn::Message& msg)
{
    QCC_UNUSED(member);
    /*
     * Cannot authenticate if we don't have any authentication mechanisms
     */
    if (peerAuthMechanisms.empty()) {
        MethodReply(msg, ER_BUS_NO_AUTHENTICATION_MECHANISM);
        return;
    }
    /*
     * Authentication may involve user interaction or be computationally expensive so cannot be
     * allowed to block the read thread.
     */
    QStatus status = DispatchRequest(msg, KEY_EXCHANGE);
    if (status != ER_OK) {
        MethodReply(msg, status);
    }
}

void AllJoynPeerObj::KeyAuthentication(const ajn::InterfaceDescription::Member* member, ajn::Message& msg)
{
    QCC_UNUSED(member);
    /*
     * Cannot authenticate if we don't have any authentication mechanisms
     */
    if (peerAuthMechanisms.empty()) {
        MethodReply(msg, ER_BUS_NO_AUTHENTICATION_MECHANISM);
        return;
    }
    /*
     * Authentication may involve user interaction or be computationally expensive so cannot be
     * allowed to block the read thread.
     */
    QStatus status = DispatchRequest(msg, KEY_AUTHENTICATION);
    if (status != ER_OK) {
        MethodReply(msg, status);
    }
}

void AllJoynPeerObj::ForceAuthentication(const qcc::String& busName)
{
    assert(bus);
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    if (peerStateTable->IsKnownPeer(busName)) {
        lock.Lock(MUTEX_CONTEXT);
        PeerState peerState = peerStateTable->GetPeerState(busName);
        peerState->ClearKeys();
        bus->ClearKeys(peerState->GetGuid().ToString());
        lock.Unlock(MUTEX_CONTEXT);
    }
}

/*
 * A long timeout to allow for possible PIN entry
 */
#define AUTH_TIMEOUT      120000
#define DEFAULT_TIMEOUT   10000

QStatus AllJoynPeerObj::AuthenticatePeer(AllJoynMessageType msgType, const qcc::String& busName, bool wait)
{
    assert(bus);
    QStatus status;
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    PeerState peerState = peerStateTable->GetPeerState(busName);
    qcc::String mech;
    const InterfaceDescription* ifc = bus->GetInterface(org::alljoyn::Bus::Peer::Authentication::InterfaceName);
    if (ifc == NULL) {
        return ER_BUS_NO_SUCH_INTERFACE;
    }
    /*
     * Cannot authenticate if we don't have an authentication mechanism
     */
    if (peerAuthMechanisms.empty()) {
        return ER_BUS_NO_AUTHENTICATION_MECHANISM;
    }
    /*
     * Return if the peer is already secured.
     */
    if (peerState->IsSecure()) {
        return ER_OK;
    }
    /*
     * Check if this peer is already being authenticated. This check won't catch authentications
     * that use different names for the same peer, but we catch those below when we using the
     * unique name. Worst case we end up making a redundant ExchangeGuids method call.
     */
    if (msgType == MESSAGE_METHOD_CALL) {
        lock.Lock(MUTEX_CONTEXT);
        if (peerState->GetAuthEvent()) {
            if (wait) {
                Event::Wait(*peerState->GetAuthEvent(), lock);
                return peerState->IsSecure() ? ER_OK : ER_AUTH_FAIL;
            } else {
                lock.Unlock(MUTEX_CONTEXT);
                return ER_WOULDBLOCK;
            }
        }
        lock.Unlock(MUTEX_CONTEXT);
    }

    ProxyBusObject remotePeerObj(*bus, busName.c_str(), org::alljoyn::Bus::Peer::ObjectPath, 0);
    remotePeerObj.AddInterface(*ifc);

    /*
     * Exchange GUIDs with the peer, this will get us the GUID of the remote peer and also the
     * unique bus name from which we can determine if we have already have a session key, a
     * master secret or if we have to start an authentication conversation.
     */
    qcc::String localGuidStr = bus->GetInternal().GetKeyStore().GetGuid();
    MsgArg args[2];
    args[0].Set("s", localGuidStr.c_str());
    args[1].Set("u", PREFERRED_AUTH_VERSION);
    Message callMsg(*bus);
    Message replyMsg(*bus);
    const InterfaceDescription::Member* exchangeGuidsMember = ifc->GetMember("ExchangeGuids");
    assert(exchangeGuidsMember);
    status = remotePeerObj.MethodCall(*exchangeGuidsMember, args, ArraySize(args), replyMsg, DEFAULT_TIMEOUT, 0, &callMsg);
    if (status != ER_OK) {
        /*
         * ER_BUS_REPLY_IS_ERROR_MESSAGE has a specific meaning in the public API and should not be
         * propogated to the caller from this context.
         */
        if (status == ER_BUS_REPLY_IS_ERROR_MESSAGE) {
            if (replyMsg->GetErrorName() != NULL && strcmp(replyMsg->GetErrorName(), "org.freedesktop.DBus.Error.ServiceUnknown") == 0) {
                status = ER_BUS_NO_SUCH_OBJECT;
            } else {
                status = ER_AUTH_FAIL;
            }
        }
        QCC_LogError(status, ("ExchangeGuids failed"));
        return status;
    }
    const qcc::String sender = replyMsg->GetSender();
    /*
     * Extract the remote guid from the message
     */
    qcc::GUID128 remotePeerGuid(replyMsg->GetArg(0)->v_string.str);
    KeyStore::Key remotePeerKey(KeyStore::Key::REMOTE, remotePeerGuid);
    uint32_t authVersion = replyMsg->GetArg(1)->v_uint32;
    qcc::String remoteGuidStr = remotePeerGuid.ToString();
    /*
     * Check that we can support the version the remote peer proposed.
     */
    if (!IsCompatibleVersion(authVersion)) {
        status = ER_BUS_PEER_AUTH_VERSION_MISMATCH;
        QCC_LogError(status, ("ExchangeGuids incompatible authentication version %u", authVersion));
        return status;
    } else {
        authVersion = GetLowerVersion(authVersion, PREFERRED_AUTH_VERSION);
    }
    QCC_DbgHLPrintf(("ExchangeGuids Local %s", localGuidStr.c_str()));
    QCC_DbgHLPrintf(("ExchangeGuids Remote %s", remoteGuidStr.c_str()));
    QCC_DbgHLPrintf(("ExchangeGuids AuthVersion %d", authVersion));
    /*
     * Now we have the unique bus name in the reply try again to find out if we have a session key
     * for this peer.
     */
    peerState = peerStateTable->GetPeerState(sender, busName);
    peerState->SetGuidAndAuthVersion(remotePeerGuid, authVersion);
    /*
     * We can now return if the peer is authenticated.
     */
    if (peerState->IsSecure()) {
        return ER_OK;
    }
    /*
     * Check again if the peer is being authenticated on another thread. We need to do this because
     * the check above may have used a well-known-namme and now we know the unique name.
     */
    lock.Lock(MUTEX_CONTEXT);
    if (peerState->GetAuthEvent()) {
        if (wait) {
            Event::Wait(*peerState->GetAuthEvent(), lock);
            return peerState->IsSecure() ? ER_OK : ER_AUTH_FAIL;
        } else {
            lock.Unlock(MUTEX_CONTEXT);
            return ER_WOULDBLOCK;
        }
    }
    /*
     * The bus allows a peer to send signals and make method calls to itself. If we are securing the
     * local peer we obviously don't need to authenticate but we must initialize a peer state object
     * with a session key and group key.
     */
    if (bus->GetUniqueName() == sender) {
        assert(remoteGuidStr == localGuidStr);
        QCC_DbgHLPrintf(("Securing local peer to itself"));
        KeyBlob key;
        /* Use the local peer's GROUP key */
        peerStateTable->GetGroupKey(key);
        key.SetTag("SELF", KeyBlob::NO_ROLE);
        peerState->SetKey(key, PEER_GROUP_KEY);
        /* Allocate a random session key - no role because we are both INITIATOR and RESPONDER */
        key.Rand(Crypto_AES::AES128_SIZE, KeyBlob::AES);
        key.SetTag("SELF", KeyBlob::NO_ROLE);
        peerState->SetKey(key, PEER_SESSION_KEY);
        /* Record in the peer state that this peer is the local peer */
        peerState->isLocalPeer = true;
        /* Set rights on the local peer - treat as mutual authentication */
        SetRights(peerState, true, false);
        /* We are still holding the lock */
        lock.Unlock(MUTEX_CONTEXT);
        return ER_OK;
    }
    /*
     * Only method calls or error message trigger authentications so if the
     * remote peer is not authenticated or in the process or being
     * authenticated we return an error status which will cause a security
     * violation notification back to the application.
     */
    if ((msgType != MESSAGE_METHOD_CALL) && (msgType != MESSAGE_ERROR)) {
        /* We are still holding the lock */
        lock.Unlock(MUTEX_CONTEXT);
        return ER_BUS_DESTINATION_NOT_AUTHENTICATED;
    }
    /*
     * Other threads authenticating the same peer will block on this event until the authentication completes.
     */
    qcc::Event authEvent;
    peerState->SetAuthEvent(&authEvent);
    lock.Unlock(MUTEX_CONTEXT);

    KeyStore& keyStore = bus->GetInternal().GetKeyStore();
    bool authTried = false;
    bool firstPass = true;
    bool useKeyExchanger = UseKeyExchanger(authVersion, supportedAuthSuites, supportedAuthSuitesCount);
    peerState->AcquireConversationHashLock();
    peerState->InitializeConversationHash();
    this->HashGUIDs(peerState, true);
    peerState->ReleaseConversationHashLock();
    do {
        /*
         * Try to load the master secret for the remote peer. It is possible that the master secret
         * has expired or been deleted either locally or remotely so if we fail to establish a
         * session key on the first pass we start an authentication conversation to establish a new
         * master secret.
         */

        if (!keyStore.HasKey(remotePeerKey)) {
            /*
             * If the key store is shared try reloading in case another application has already
             * authenticated this peer.
             */
            if (keyStore.IsShared()) {
                keyStore.Reload();
                if (!keyStore.HasKey(remotePeerKey)) {
                    status = ER_AUTH_FAIL;
                }
            } else {
                status = ER_AUTH_FAIL;
            }
        }
        if (status == ER_OK) {
            /*
             * Generate a random string - this is the local half of the seed string.
             */
            qcc::String nonce = RandHexString(NONCE_LEN);
            /*
             * Send GenSessionKey message to remote peer.
             */
            MsgArg msgArgs[3];
            msgArgs[0].Set("s", localGuidStr.c_str());
            msgArgs[1].Set("s", remoteGuidStr.c_str());
            msgArgs[2].Set("s", nonce.c_str());

            const InterfaceDescription::Member* genSessionKeyMember = ifc->GetMember("GenSessionKey");
            assert(genSessionKeyMember);
            peerState->AcquireConversationHashLock();
            status = remotePeerObj.MethodCall(*genSessionKeyMember, msgArgs, ArraySize(msgArgs), replyMsg, DEFAULT_TIMEOUT, 0, &callMsg);
            peerState->UpdateHash(CONVERSATION_V4, callMsg);
            peerState->UpdateHash(CONVERSATION_V4, replyMsg);
            peerState->ReleaseConversationHashLock();
            if (status == ER_OK) {
                qcc::String verifier;
                /*
                 * The response completes the seed string so we can generate the session key.
                 */
                status = KeyGen(peerState, nonce + replyMsg->GetArg(0)->v_string.str, verifier, KeyBlob::INITIATOR);
                QCC_DbgHLPrintf(("Initiator KeyGen after receiving response from sender %s", busName.c_str()));
                if ((status == ER_OK) && (verifier != replyMsg->GetArg(1)->v_string.str)) {
                    status = ER_AUTH_FAIL;
                }
            }
        }
        if ((status == ER_OK) || !firstPass) {
            break;
        }
        if (useKeyExchanger) {
            uint32_t*remoteAuthSuites = NULL;
            size_t remoteAuthSuitesCount = 0;
            status = AskForAuthSuites(authVersion, remotePeerObj, ifc, &remoteAuthSuites, &remoteAuthSuitesCount, peerState);
            if (status == ER_OK) {
                status = AuthenticatePeerUsingKeyExchange(remoteAuthSuites, remoteAuthSuitesCount, busName, peerState, localGuidStr, remotePeerObj, ifc, mech);
                if (remoteAuthSuites) {
                    delete [] remoteAuthSuites;
                }
            }
        } else {
            status = AuthenticatePeerUsingSASL(busName, peerState, localGuidStr, remotePeerObj, ifc, remotePeerKey, mech);
        }
        authTried = true;
        firstPass = false;
    } while (status == ER_OK);
    /*
     * At this point, the authentication conversation is over and we no longer need
     * to keep the conversation hash.
     */
    peerState->AcquireConversationHashLock();
    peerState->FreeConversationHash();
    peerState->ReleaseConversationHashLock();
    /*
     * Exchange group keys with the remote peer. This method call is encrypted using the session key
     * that we just established.
     */
    if (status == ER_OK) {
        uint8_t keyGenVersion = authVersion & 0xFF;
        uint16_t authV = authVersion >> 16;
        uint8_t sendKeyBlob = (authV <= 1) && (keyGenVersion == 0);
        Message keyExchangeReplyMsg(*bus);
        KeyBlob key;
        peerStateTable->GetGroupKey(key);
        StringSink snk;
        MsgArg arg;
        /*
         * KeyGen version 0 exchanges key blobs, version 1 just exchanges the key
         */
        QCC_DbgHLPrintf(("ExchangeGroupKeys using key gen version %d", keyGenVersion));
        if (sendKeyBlob) {
            key.Store(snk);
            arg.Set("ay", snk.GetString().size(), snk.GetString().data());
        } else {
            arg.Set("ay", key.GetSize(), key.GetData());
        }
        const InterfaceDescription::Member* exchangeGroupKeysMember = ifc->GetMember("ExchangeGroupKeys");
        assert(exchangeGroupKeysMember);
        status = remotePeerObj.MethodCall(*exchangeGroupKeysMember, &arg, 1, keyExchangeReplyMsg, DEFAULT_TIMEOUT, ALLJOYN_FLAG_ENCRYPTED);
        if (status == ER_OK) {
            if (sendKeyBlob) {
                StringSource src(keyExchangeReplyMsg->GetArg(0)->v_scalarArray.v_byte, keyExchangeReplyMsg->GetArg(0)->v_scalarArray.numElements);
                status = key.Load(src);
            } else {
                status = key.Set(keyExchangeReplyMsg->GetArg(0)->v_scalarArray.v_byte, keyExchangeReplyMsg->GetArg(0)->v_scalarArray.numElements, KeyBlob::AES);
            }
            if (status == ER_OK) {
                /*
                 * Tag the group key with the auth mechanism used by ExchangeGroupKeys. Group keys
                 * are inherently directional - only initiator encrypts with the group key. We set
                 * the role to NO_ROLE otherwise senders can't decrypt their own broadcast messages.
                 */
                key.SetTag(keyExchangeReplyMsg->GetAuthMechanism(), KeyBlob::NO_ROLE);
                peerState->SetKey(key, PEER_GROUP_KEY);
            }
            if (status == ER_OK) {
                /* exchange membership guilds */
                if (useKeyExchanger && IsMembershipCertCapable(peerState->GetAuthVersion())) {
                    bool sendManifest = false;
                    if (mech == "ALLJOYN_ECDHE_ECDSA") {
                        sendManifest = true;
                    } else if (mech.empty()) {
                        /* key exchange step was skipped.
                           Send manifest if the local peer already cached the
                           remote peer's public key */
                        ECCPublicKey pubKey;
                        QStatus aStatus = securityApplicationObj.GetConnectedPeerPublicKey(peerState->GetGuid(), &pubKey);
                        sendManifest = (ER_OK == aStatus);
                    }
                    if (sendManifest) {
                        SendManifest(remotePeerObj, ifc, peerState);
                        SendMembershipData(remotePeerObj, ifc, remotePeerGuid);
                    }
                }
            }
        }
    }
    /*
     * If an authentication was tried report the authentication completion to allow application to clear UI etc.
     */
    if (authTried) {
        peerAuthListener.AuthenticationComplete(mech.c_str(), sender.c_str(), status == ER_OK);
    }
    /*
     * ER_BUS_REPLY_IS_ERROR_MESSAGE has a specific meaning in the public API an should not be
     * propogated to the caller from this context.
     */
    if (status == ER_BUS_REPLY_IS_ERROR_MESSAGE) {
        status = ER_AUTH_FAIL;
    }
    /*
     * Release any other threads waiting on the result of this authentication.
     */
    lock.Lock(MUTEX_CONTEXT);
    peerState->SetAuthEvent(NULL);
    while (authEvent.GetNumBlockedThreads() > 0) {
        authEvent.SetEvent();
        qcc::Sleep(10);
    }
    lock.Unlock(MUTEX_CONTEXT);
    return status;
}

QStatus AllJoynPeerObj::AuthenticatePeerUsingSASL(const qcc::String& busName, PeerState peerState, qcc::String& localGuidStr, ProxyBusObject& remotePeerObj, const InterfaceDescription* ifc, KeyStore::Key& remotePeerKey, qcc::String& mech)
{
    QStatus status;
    ajn::SASLEngine::AuthState authState;

    /*
     * Initiaize the SASL engine as responder (i.e. client) this terminology seems backwards but
     * is the terminology used by the DBus specification.
     */
    SASLEngine sasl(*bus, ajn::AuthMechanism::RESPONDER, peerAuthMechanisms, busName.c_str(), peerAuthListener);
    sasl.SetLocalId(localGuidStr);
    /*
     * This will let us know if we need to make an AuthenticationComplete callback below.
     */
    qcc::String inStr;
    qcc::String outStr;
    status = sasl.Advance(inStr, outStr, authState);
    while (status == ER_OK) {
        Message replyMsg(*bus);
        MsgArg arg("s", outStr.c_str());
        const InterfaceDescription::Member* authChallengeMember = ifc->GetMember("AuthChallenge");
        assert(authChallengeMember);
        status = remotePeerObj.MethodCall(*authChallengeMember, &arg, 1, replyMsg, AUTH_TIMEOUT);
        if (status == ER_OK) {
            /*
             * This will let us know if we need to make an AuthenticationComplete callback below.
             */
            if (authState == SASLEngine::ALLJOYN_AUTH_SUCCESS) {
                SetRights(peerState, sasl.AuthenticationIsMutual(), false /*responder*/);
                break;
            }
            inStr = qcc::String(replyMsg->GetArg(0)->v_string.str);
            status = sasl.Advance(inStr, outStr, authState);
            if (authState == SASLEngine::ALLJOYN_AUTH_SUCCESS) {
                KeyBlob masterSecret;
                mech = sasl.GetMechanism();
                status = sasl.GetMasterSecret(masterSecret);
                if (status == ER_OK) {
                    SetRights(peerState, sasl.AuthenticationIsMutual(), false /*responder*/);
                    /* Tag the master secret with the auth mechanism used to generate it */
                    masterSecret.SetTag(mech, KeyBlob::INITIATOR);
                    status = bus->GetInternal().GetKeyStore().AddKey(remotePeerKey, masterSecret, peerState->authorizations);
                }
            }
        } else {
            status = ER_AUTH_FAIL;
        }
    }
    return status;
}

QStatus AllJoynPeerObj::AskForAuthSuites(uint32_t peerAuthVersion, ProxyBusObject& remotePeerObj, const InterfaceDescription* ifc, uint32_t** remoteAuthSuites, size_t* remoteAuthCount, PeerState peerState)
{
    if (supportedAuthSuitesCount == 0) {
        return ER_AUTH_FAIL;
    }
    MsgArg arg;
    bool excludeECDHE_ECDSA = false;
    if ((peerAuthVersion >> 16) <= NON_ECDSA_X509_VERSION) {
        for (size_t cnt = 0; cnt < supportedAuthSuitesCount; cnt++) {
            if (supportedAuthSuites[cnt] == AUTH_SUITE_ECDHE_ECDSA) {
                excludeECDHE_ECDSA = true;
                break;
            }
        }
    }
    uint32_t* authSuites = supportedAuthSuites;
    size_t authSuitesCount = supportedAuthSuitesCount;
    if (excludeECDHE_ECDSA) {
        authSuites = new uint32_t[supportedAuthSuitesCount];
        size_t netCnt = 0;
        for (size_t cnt = 0; cnt < supportedAuthSuitesCount; cnt++) {
            if (supportedAuthSuites[cnt] != AUTH_SUITE_ECDHE_ECDSA) {
                authSuites[netCnt++] = supportedAuthSuites[cnt];
            }
        }
        authSuitesCount = netCnt;
    }

    arg.Set("au", authSuitesCount, authSuites);
    Message callMsg(*bus);
    Message replyMsg(*bus);
    const InterfaceDescription::Member* exchangeSuites = ifc->GetMember("ExchangeSuites");
    assert(exchangeSuites);

    QStatus status = remotePeerObj.MethodCall(*exchangeSuites, &arg, 1, replyMsg, DEFAULT_TIMEOUT, 0, &callMsg);
    if (excludeECDHE_ECDSA) {
        delete [] authSuites;
    }
    if (status != ER_OK) {
        return status;
    }
    peerState->AcquireConversationHashLock();
    peerState->UpdateHash(CONVERSATION_V4, callMsg);
    peerState->UpdateHash(CONVERSATION_V4, replyMsg);
    peerState->ReleaseConversationHashLock();
    uint32_t* remoteSuites;
    size_t remoteSuitesLen;

    status = replyMsg->GetArg(0)->Get("au", &remoteSuitesLen, &remoteSuites);
    if (status != ER_OK) {
        return status;
    }
    *remoteAuthCount = remoteSuitesLen;
    uint32_t* effectiveAuthSuites = new uint32_t[remoteSuitesLen];
    for (size_t cnt = 0; cnt < remoteSuitesLen; cnt++) {
        effectiveAuthSuites[cnt] = remoteSuites[cnt];
    }
    *remoteAuthSuites = effectiveAuthSuites;
    return ER_OK;
}

QStatus AllJoynPeerObj::AuthenticatePeerUsingKeyExchange(const uint32_t* requestingAuthList, size_t requestingAuthCount, const qcc::String& busName, PeerState peerState, qcc::String& localGuidStr, ProxyBusObject& remotePeerObj, const InterfaceDescription* ifc, qcc::String& mech)
{
    QStatus status;

    QCC_DbgHLPrintf(("AuthenticatePeerUsingKeyExchange"));
    shared_ptr<KeyExchanger> keyExchanger = GetKeyExchangerInstance(peerState, true, requestingAuthList, requestingAuthCount);  /* initiator */
    if (!keyExchanger) {
        return ER_AUTH_FAIL;
    }
    uint32_t remoteAuthMask = 0;
    uint32_t currentSuite = keyExchanger->GetSuite();
    mech = keyExchanger->GetSuiteName();
    KeyExchangerCB kxCB(remotePeerObj, ifc, AUTH_TIMEOUT);
    status = keyExchanger->ExecKeyExchange(currentSuite, kxCB, &remoteAuthMask);

    if ((status == ER_OK) && (remoteAuthMask == currentSuite)) {
        uint8_t authorized = false;
        status = keyExchanger->KeyAuthentication(kxCB, busName.c_str(), &authorized);
        if (authorized) {
            SetRights(peerState, true, false /*responder*/);
            status = RecordMasterSecret(busName, keyExchanger, peerState);
        } else {
            status = ER_AUTH_FAIL;
        }
    } else if (status == ER_OK) {
        status = ER_AUTH_FAIL; /* remote auth mask is 0 */
    }

    if (status == ER_OK) {
        return status;
    }
    if (!remoteAuthMask) {
        return ER_AUTH_FAIL; /* done */
    }
    if (requestingAuthCount == 1) {
        return ER_AUTH_FAIL; /* done.  There is no more to try. */
    }
    size_t smallerCount = requestingAuthCount - 1;
    uint32_t* smallerSuites = new uint32_t[smallerCount];
    size_t idx = 0;
    for (size_t cnt = 0; cnt < requestingAuthCount; cnt++) {
        if ((requestingAuthList[cnt] & currentSuite) != currentSuite) {
            assert(idx < smallerCount);
            if (idx >= smallerCount) {
                delete [] smallerSuites;
                return ER_AUTH_FAIL;
            }
            smallerSuites[idx++] = requestingAuthList[cnt];
        }
    }
    if ((peerState->GetAuthVersion() >> 16) < CONVERSATION_V4) {
        /* any peer with auth version smaller than 4 need to start the hash at
         * the KeyExchange call */
        peerState->AcquireConversationHashLock();
        peerState->InitializeConversationHash();
        peerState->ReleaseConversationHashLock();
    }
    status = AuthenticatePeerUsingKeyExchange(smallerSuites, smallerCount, busName, peerState, localGuidStr, remotePeerObj, ifc, mech);
    delete [] smallerSuites;
    return status;
}

QStatus AllJoynPeerObj::AuthenticatePeerAsync(const qcc::String& busName)
{
    assert(bus);
    Message invalidMsg(*bus);
    return DispatchRequest(invalidMsg, SECURE_CONNECTION, busName);
}

QStatus AllJoynPeerObj::DispatchRequest(Message& msg, RequestType reqType, const qcc::String data)
{
    QStatus status;
    QCC_DbgHLPrintf(("DispatchRequest %s", msg->Description().c_str()));
    lock.Lock(MUTEX_CONTEXT);
    if (dispatcher.IsRunning()) {
        Request* req = new Request(msg, reqType, data);
        qcc::AlarmListener* alljoynPeerListener = this;
        status = dispatcher.AddAlarm(Alarm(alljoynPeerListener, req));
        if (status != ER_OK) {
            delete req;
        }
    } else {
        status = ER_BUS_STOPPING;
    }
    lock.Unlock(MUTEX_CONTEXT);
    return status;
}

void AllJoynPeerObj::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_UNUSED(reason);

    QStatus status;

    assert(bus);
    QCC_DbgHLPrintf(("AllJoynPeerObj::AlarmTriggered"));
    Request* req = static_cast<Request*>(alarm->GetContext());

    switch (req->reqType) {
    case AUTHENTICATE_PEER:
        /*
         * Push the message onto a queue of messages to be encrypted and forwarded in order when
         * the authentication completes.
         */
        lock.Lock(MUTEX_CONTEXT);
        msgsPendingAuth.push_back(req->msg);
        lock.Unlock(MUTEX_CONTEXT);
        /*
         * Pause timeouts so reply handlers don't expire while waiting for authentication to complete
         */
        if (req->msg->GetType() == MESSAGE_METHOD_CALL) {
            bus->GetInternal().GetLocalEndpoint()->PauseReplyHandlerTimeout(req->msg);
        }
        status = AuthenticatePeer(req->msg->GetType(), req->msg->GetDestination(), false);
        if (status != ER_WOULDBLOCK) {
            PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
            /*
             * Check each message that is queued waiting for an authentication to complete
             * to see if this is the authentication the message was waiting for.
             */
            lock.Lock(MUTEX_CONTEXT);
            std::deque<Message>::iterator iter = msgsPendingAuth.begin();
            while (iter != msgsPendingAuth.end()) {
                Message msg = *iter;
                if (peerStateTable->IsAlias(msg->GetDestination(), req->msg->GetDestination())) {
                    LocalEndpoint lep =  bus->GetInternal().GetLocalEndpoint();
                    if (status != ER_OK) {
                        /*
                         * If the failed message was a method call push an error response.
                         */
                        if (msg->GetType() == MESSAGE_METHOD_CALL) {
                            Message reply(*bus);
                            reply->ErrorMsg(status, msg->GetCallSerial());
                            bus->GetInternal().GetLocalEndpoint()->PushMessage(reply);
                        }
                    } else {
                        if (msg->GetType() == MESSAGE_METHOD_CALL) {
                            bus->GetInternal().GetLocalEndpoint()->ResumeReplyHandlerTimeout(msg);
                        }
                        BusEndpoint busEndpoint = BusEndpoint::cast(bus->GetInternal().GetLocalEndpoint());
                        status = bus->GetInternal().GetRouter().PushMessage(msg, busEndpoint);
                        if (status == ER_PERMISSION_DENIED) {
                            if (req->msg->GetType() == MESSAGE_METHOD_CALL) {
                                Message reply(*bus);
                                reply->ErrorMsg(status, req->msg->GetCallSerial());
                                bus->GetInternal().GetLocalEndpoint()->PushMessage(reply);
                            }
                        }
                    }
                    iter = msgsPendingAuth.erase(iter);
                } else {
                    iter++;
                }
            }
            lock.Unlock(MUTEX_CONTEXT);
            /*
             * Report a single error for the message the triggered the authentication
             */
            if (status != ER_OK) {
                peerAuthListener.SecurityViolation(status, req->msg);
            }
        }
        break;

    case AUTH_CHALLENGE:
        AuthAdvance(req->msg);
        break;

    case KEY_EXCHANGE:
        DoKeyExchange(req->msg);
        break;

    case KEY_AUTHENTICATION:
        DoKeyAuthentication(req->msg);
        break;

    case SECURE_CONNECTION:
        status = AuthenticatePeer(MESSAGE_METHOD_CALL, req->data, true);
        if (status != ER_OK) {
            peerAuthListener.SecurityViolation(status, req->msg);
        }
        break;

    }

    delete req;
    QCC_DbgHLPrintf(("AllJoynPeerObj::AlarmTriggered - exiting"));
    return;
}

void AllJoynPeerObj::HandleSecurityViolation(Message& msg, QStatus status)
{
    assert(bus);
    QCC_DbgTrace(("HandleSecurityViolation %s %s", QCC_StatusText(status), msg->Description().c_str()));

    if (status == ER_PERMISSION_DENIED) {
        if (!bus->GetInternal().GetRouter().IsDaemon()) {
            /* The message was not delivered because of permission denied.
               So notify the sender */
            if (msg->GetType() == MESSAGE_METHOD_CALL) {
                Message reply(*bus);
                reply->ErrorMsg(status, msg->GetCallSerial());
                bus->GetInternal().GetLocalEndpoint()->PushMessage(reply);
            }
        }
        return;
    }

    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();

    if (status == ER_BUS_MESSAGE_DECRYPTION_FAILED) {
        PeerState peerState = peerStateTable->GetPeerState(msg->GetSender());
        /*
         * If we believe the peer is secure we have a clear security violation
         */
        if (peerState->IsSecure()) {
            /*
             * The keys we have for this peer are no good
             */
            peerState->ClearKeys();
        } else if (msg->IsBroadcastSignal()) {
            /*
             * Encrypted broadcast signals are silently ignored
             */
            QCC_DbgHLPrintf(("Discarding encrypted broadcast signal"));
            status = ER_OK;
        }
    }
    /*
     * Report the security violation
     */
    if (status != ER_OK) {
        QCC_DbgTrace(("Reporting security violation %s for %s", QCC_StatusText(status), msg->Description().c_str()));
        peerAuthListener.SecurityViolation(status, msg);
    }
}


void AllJoynPeerObj::NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner)
{
    QCC_UNUSED(previousOwner);
    assert(bus);

    /*
     * We are only interested in names that no longer have an owner.
     */
    if (newOwner == NULL) {
        QCC_DbgHLPrintf(("Peer %s is gone", busName));
        /*
         * Clean up peer state.
         */
        bus->GetInternal().GetPeerStateTable()->DelPeerState(busName);
        /*
         * We are no longer in an authentication conversation with this peer.
         */
        lock.Lock(MUTEX_CONTEXT);
        delete conversations[busName];
        conversations.erase(busName);
        keyExConversations.erase(busName);
        lock.Unlock(MUTEX_CONTEXT);
    }
}

void AllJoynPeerObj::AcceptSession(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);

    QStatus status;
    size_t numArgs;
    const MsgArg* args;

    assert(bus);
    msg->GetArgs(numArgs, args);
    SessionPort sessionPort = args[0].v_uint16;
    SessionId sessionId = args[1].v_uint32;
    String joiner = args[2].v_string.str;
    SessionOpts opts;
    status = GetSessionOpts(args[3], opts);

    if (status == ER_OK) {
        MsgArg replyArg;

        /* Call bus listeners */
        bool isAccepted = bus->GetInternal().CallAcceptListeners(sessionPort, joiner.c_str(), opts);

        /* Reply to AcceptSession */
        replyArg.Set("b", isAccepted);
        status = MethodReply(msg, &replyArg, 1);

        if ((status == ER_OK) && isAccepted) {
            BusEndpoint sender = bus->GetInternal().GetRouter().FindEndpoint(msg->GetRcvEndpointName());
            if (sender->GetEndpointType() == ENDPOINT_TYPE_REMOTE) {
                RemoteEndpoint rep = RemoteEndpoint::cast(sender);
                const uint32_t VER_250 = 33882112;
                uint32_t protoVersion = rep->GetRemoteProtocolVersion();
                /**
                 * remote daemon is older than version 2.5.0; it will *NOT* send the SessionJoined signal
                 *
                 * Unfortunately, the original form of this code checked the AllJoyn version number rather than the
                 * protocol version number. Since the AllJoyn version number is only valid at release time,
                 * the check was later updated to also filter on protocol version numbers. Therefore protocol
                 * version number works fine except when protocol version is 3 in which case the AllJoyn version
                 * number must be used.
                 */
                if ((protoVersion < 3) || ((protoVersion == 3) && (rep->GetRemoteAllJoynVersion() < VER_250))) {
                    bus->GetInternal().CallJoinedListeners(sessionPort, sessionId, joiner.c_str());
                }
            }
        }
    } else {
        MethodReply(msg, status);
    }
}



void AllJoynPeerObj::SessionJoined(const InterfaceDescription::Member* member, const char* srcPath, Message& msg)
{
    QCC_UNUSED(member);
    QCC_UNUSED(srcPath);
    assert(bus);

    // dispatch to the dispatcher thread
    size_t numArgs;
    const MsgArg* args;

    msg->GetArgs(numArgs, args);
    assert(numArgs == 3);
    const SessionPort sessionPort = args[0].v_uint16;
    const SessionId sessionId = args[1].v_uint32;
    const char* joiner = args[2].v_string.str;
    bus->GetInternal().CallJoinedListeners(sessionPort, sessionId, joiner);
}

std::shared_ptr<KeyExchanger> AllJoynPeerObj::GetKeyExchangerInstance(PeerState peerState, bool initiator, const uint32_t* requestingAuthList, size_t requestingAuthCount)
{
    for (size_t cnt = 0; cnt < requestingAuthCount; cnt++) {
        uint32_t suite = requestingAuthList[cnt];
        if ((suite & AUTH_SUITE_ECDHE_ECDSA) == AUTH_SUITE_ECDHE_ECDSA) {
            return make_shared<KeyExchangerECDHE_ECDSA>(initiator, this, *bus, peerAuthListener, peerState, (PermissionMgmtObj::TrustAnchorList*) &securityApplicationObj.GetTrustAnchors());
        }
        if ((suite & AUTH_SUITE_ECDHE_PSK) == AUTH_SUITE_ECDHE_PSK) {
            return make_shared<KeyExchangerECDHE_PSK>(initiator, this, *bus, peerAuthListener, peerState);
        }
        if ((suite & AUTH_SUITE_ECDHE_NULL) == AUTH_SUITE_ECDHE_NULL) {
            return make_shared<KeyExchangerECDHE_NULL>(initiator, this, *bus, peerAuthListener, peerState);
        }
    }
    return shared_ptr<KeyExchanger>();
}

QStatus AllJoynPeerObj::HandleMethodReply(Message& msg, QStatus status)
{
    return MethodReply(msg, status);
}

QStatus AllJoynPeerObj::HandleMethodReply(Message& msg, Message& replyMsg, QStatus status)
{
    return MethodReply(msg, status, &replyMsg);
}

QStatus AllJoynPeerObj::HandleMethodReply(Message& msg, const MsgArg* args, size_t numArgs)
{
    return MethodReply(msg, args, numArgs);
}

QStatus AllJoynPeerObj::HandleMethodReply(Message& msg, Message& replyMsg, const MsgArg* args, size_t numArgs)
{
    return MethodReply(msg, args, numArgs, &replyMsg);
}

QStatus KeyExchangerCB::SendKeyExchange(MsgArg* args, size_t numArgs, Message* sentMsg, Message* replyMsg)
{
    const InterfaceDescription::Member* keyExchange = ifc->GetMember("KeyExchange");
    assert(keyExchange);
    return remoteObj.MethodCall(*keyExchange, args, numArgs, *replyMsg, timeout, 0, sentMsg);
}

QStatus KeyExchangerCB::SendKeyAuthentication(MsgArg* msg, Message* sentMsg, Message* replyMsg)
{
    const InterfaceDescription::Member* keyAuth = ifc->GetMember("KeyAuthentication");
    assert(keyAuth);
    return remoteObj.MethodCall(*keyAuth, msg, 1, *replyMsg, timeout, 0, sentMsg);
}

class SortableAuthSuite {
  public:
    SortableAuthSuite(uint8_t weight, uint32_t suite) : weight(weight), suite(suite)
    {
    }

    bool operator <(const SortableAuthSuite& other) const
    {
        /* sort with highest weight first */
        return weight > other.weight;
    }

    uint8_t weight;
    uint32_t suite;

};

/**
 * Setup for peer-to-peer authentication. The authentication mechanisms listed can only be used
 * if they are already registered with bus. The authentication mechanisms names are separated by
 * space characters.
 *
 * @param authMechanisms   The names of the authentication mechanisms to set
 * @param listener         Required for authentication mechanisms that require interation with the user
 *                         or application. Can be NULL if not required.
 * @param bus               Bus attachment
 */
void AllJoynPeerObj::SetupPeerAuthentication(const qcc::String& authMechanisms, AuthListener* listener, BusAttachment& bus)
{
    QCC_UNUSED(bus);
    /* clean up first */
    delete [] supportedAuthSuites;
    supportedAuthSuites = NULL;

    peerAuthMechanisms = authMechanisms;
    peerAuthListener.Set(listener);

    delete [] supportedAuthSuites;
    supportedAuthSuites = NULL;
    supportedAuthSuitesCount = 0;

    /* setup the peer auth mask */
    size_t pos;
    qcc::String remainder = authMechanisms;
    qcc::String mech;

    /* parse and load each auth mechanism into a vector with assigned weight */
    bool done = false;
    remainder = authMechanisms;
    std::vector<SortableAuthSuite> suiteList;
    while (!done) {
        pos = remainder.find_first_of(" ");
        if (pos == qcc::String::npos) {
            mech = remainder;
            done = true;
        } else {
            mech = remainder.substr(0, pos);
        }
        remainder = remainder.substr(pos + 1);
        if (mech == "ANONYMOUS") {
            suiteList.push_back(SortableAuthSuite(1, AUTH_SUITE_ANONYMOUS));
        } else if (mech == "EXTERNAL") {
            suiteList.push_back(SortableAuthSuite(2, AUTH_SUITE_EXTERNAL));
        } else if (mech == "ALLJOYN_ECDHE_NULL") {
            suiteList.push_back(SortableAuthSuite(3, AUTH_SUITE_ECDHE_NULL));
        } else if (mech == "ALLJOYN_SRP_KEYX") {
            suiteList.push_back(SortableAuthSuite(4, AUTH_SUITE_SRP_KEYX));
        } else if (mech == "ALLJOYN_SRP_LOGON") {
            suiteList.push_back(SortableAuthSuite(5, AUTH_SUITE_SRP_LOGON));
        } else if (mech == "ALLJOYN_ECDHE_PSK") {
            suiteList.push_back(SortableAuthSuite(6, AUTH_SUITE_ECDHE_PSK));
        } else if (mech == "GSSAPI") {
            suiteList.push_back(SortableAuthSuite(7, AUTH_SUITE_GSSAPI));
        } else if (mech == "ALLJOYN_ECDHE_ECDSA") {
            suiteList.push_back(SortableAuthSuite(8, AUTH_SUITE_ECDHE_ECDSA));
        }
    }
    if (suiteList.size() == 0) {
        return;
    }

    /* need to have the vector sorted in highest weight first */
    std::sort(suiteList.begin(), suiteList.end());

    supportedAuthSuitesCount = suiteList.size();
    supportedAuthSuites = new uint32_t[supportedAuthSuitesCount];
    size_t cnt = 0;
    for (std::vector<SortableAuthSuite>::iterator it = suiteList.begin(); it != suiteList.end(); it++) {
        supportedAuthSuites[cnt++] = (*it).suite;
    }
    suiteList.clear();
    /* reload the object to reflect possible keystore changes */
    securityApplicationObj.Load();
    peerAuthListener.SetPermissionMgmtObj(&securityApplicationObj);
}

QStatus AllJoynPeerObj::SendManifest(ProxyBusObject& remotePeerObj, const InterfaceDescription* ifc, PeerState& peerState)
{
    PermissionPolicy::Rule* manifest = NULL;
    size_t count = 0;
    QStatus status = securityApplicationObj.RetrieveManifest(NULL, &count);
    if (ER_OK != status) {
        if (ER_MANIFEST_NOT_FOUND == status) {
            return ER_OK;  /* nothing to send */
        }
        return status;
    }
    if (count > 0) {
        manifest = new PermissionPolicy::Rule[count];
    }
    status = securityApplicationObj.RetrieveManifest(manifest, &count);
    if (ER_OK != status) {
        delete [] manifest;
        if (ER_MANIFEST_NOT_FOUND == status) {
            return ER_OK;  /* nothing to send */
        }
        return status;
    }

    MsgArg rulesArg;
    PermissionPolicy::GenerateRules(manifest, count, rulesArg);
    Message replyMsg(*bus);
    const InterfaceDescription::Member* sendManifest = ifc->GetMember("SendManifest");
    status = remotePeerObj.MethodCall(*sendManifest, &rulesArg, 1, replyMsg, DEFAULT_TIMEOUT);

    delete [] manifest;
    if (status != ER_OK) {
        return status;
    }
    /* process the reply */
    return securityApplicationObj.ParseSendManifest(replyMsg, peerState);
}

void AllJoynPeerObj::HandleSendManifest(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    PeerState peerState = peerStateTable->GetPeerState(msg->GetSender());
    QStatus status = securityApplicationObj.ParseSendManifest(msg, peerState);
    if (ER_OK != status) {
        MethodReply(msg, status);
        return;
    }
    /* send back manifest to calling peer */
    PermissionPolicy::Rule* manifest = NULL;
    size_t count = 0;
    status = securityApplicationObj.RetrieveManifest(NULL, &count);
    if (ER_OK != status && ER_MANIFEST_NOT_FOUND != status) {
        MethodReply(msg, status);
        return;
    }
    if (count > 0) {
        manifest = new PermissionPolicy::Rule[count];
    }
    status = securityApplicationObj.RetrieveManifest(manifest, &count);
    if ((ER_OK != status) && (ER_MANIFEST_NOT_FOUND != status)) {
        delete [] manifest;
        MethodReply(msg, status);
        return;
    }
    MsgArg replyArg;
    if (ER_MANIFEST_NOT_FOUND == status) {
        /* return empty array */
        status = replyArg.Set("a(ssa(syy))", 0, NULL);
    } else {
        status = PermissionPolicy::GenerateRules(manifest, count, replyArg);
    }
    if (ER_OK == status) {
        MethodReply(msg, &replyArg, 1);
    } else {
        MethodReply(msg, status);
    }
    delete [] manifest;
}

static QStatus SetUpSendMembershipInput(std::vector<MsgArg*>& args, uint8_t& pos, uint8_t total, MsgArg* sendMembershipArgs, size_t sendMembershipArgsSize)
{
    QCC_UNUSED(sendMembershipArgsSize);
    assert(sendMembershipArgsSize == 2);
    MsgArg* certChainArgs = NULL;
    QStatus status = ER_OK;
    if (pos < total) {
        if (pos == (total - 1)) {
            sendMembershipArgs[0].Set("y", PermissionMgmtObj::SEND_MEMBERSHIP_LAST);
        } else {
            sendMembershipArgs[0].Set("y", PermissionMgmtObj::SEND_MEMBERSHIP_MORE);
        }
        certChainArgs = new MsgArg[args.size()];
        size_t idx = 0;
        for (std::vector<MsgArg*>::iterator iter = args.begin(); iter != args.end(); iter++) {
            /* make a shallow copy of the message arg */
            uint8_t encoding;
            size_t len;
            uint8_t* buf;
            status = (*iter)->Get("(yay)", &encoding, &len, &buf);
            if (ER_OK != status) {
                goto Exit;
            }
            status = certChainArgs[idx++].Set("(yay)", encoding, len, buf);
            if (ER_OK != status) {
                goto Exit;
            }
        }
        status = sendMembershipArgs[1].Set("a(yay)", args.size(), certChainArgs);
        if (ER_OK != status) {
            goto Exit;
        }
        sendMembershipArgs[1].SetOwnershipFlags(MsgArg::OwnsArgs, true);
        pos++;  /* move the position */
    } else {
        /* still send the zero list so the peer knows */
        sendMembershipArgs[0].Set("y", PermissionMgmtObj::SEND_MEMBERSHIP_NONE);
        sendMembershipArgs[1].Set("a(yay)", 0, NULL);
    }
Exit:
    if (ER_OK != status) {
        delete [] certChainArgs;
    }
    return status;
}

QStatus AllJoynPeerObj::SendMembershipData(ProxyBusObject& remotePeerObj, const InterfaceDescription* ifc, const qcc::GUID128& remotePeerGuid)
{
    std::vector<std::vector<MsgArg*> > args;
    QStatus status = securityApplicationObj.GenerateSendMemberships(args, remotePeerGuid);
    if (ER_OK != status) {
        return status;
    }
    size_t argCount = args.size();

    Message replyMsg(*bus);
    const InterfaceDescription::Member* sendMembershipData = ifc->GetMember("SendMemberships");

    bool gotAllFromPeer = false;
    uint8_t cnt = 0;
    while (true) {
        MsgArg inputs[2];
        if (cnt == argCount) {
            std::vector<MsgArg*> emptyArgs;
            status = SetUpSendMembershipInput(emptyArgs, cnt, argCount, inputs, 2);
        } else {
            status = SetUpSendMembershipInput(args[cnt], cnt, argCount, inputs, 2);
        }
        /* cnt is updated by SetUpSendMembershipInput */
        if (ER_OK != status) {
            goto Exit;
        }
        status = remotePeerObj.MethodCall(*sendMembershipData, inputs, 2, replyMsg, DEFAULT_TIMEOUT);
        if (ER_OK != status) {
            goto Exit;
        }
        /* process the reply */
        status = securityApplicationObj.ParseSendMemberships(replyMsg, gotAllFromPeer);
        if (ER_OK != status) {
            goto Exit;
        }
        if (gotAllFromPeer && (cnt == argCount)) {
            break;
        }
    }

Exit:
    _PeerState::ClearGuildArgs(args);
    return status;
}

void AllJoynPeerObj::SendMemberships(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_UNUSED(member);
    PeerStateTable* peerStateTable = bus->GetInternal().GetPeerStateTable();
    PeerState peerState = peerStateTable->GetPeerState(msg->GetSender());
    MsgArg replyArgs[2];
    std::vector<MsgArg*> emptyArgs;
    bool gotAllFromPeer = false;
    QStatus status = securityApplicationObj.ParseSendMemberships(msg, gotAllFromPeer);
    if (ER_OK != status) {
        goto Exit;
    }
    if (peerState->guildArgs.size() == 0) {
        status = securityApplicationObj.GenerateSendMemberships(peerState->guildArgs, peerState->GetGuid());
        if (ER_OK != status) {
            goto Exit;
        }
        peerState->guildArgsSentCount = 0;
    }

    if (peerState->guildArgsSentCount < peerState->guildArgs.size()) {
        status = SetUpSendMembershipInput(peerState->guildArgs[peerState->guildArgsSentCount], peerState->guildArgsSentCount, peerState->guildArgs.size(), replyArgs, ArraySize(replyArgs));
    } else {
        status = SetUpSendMembershipInput(emptyArgs, peerState->guildArgsSentCount, peerState->guildArgs.size(), replyArgs, ArraySize(replyArgs));
    }

    if (ER_OK != status) {
        goto Exit;
    }
    MethodReply(msg, replyArgs, ArraySize(replyArgs));
    if (peerState->guildArgsSentCount >= peerState->guildArgs.size()) {
        /* release this resource since it no longer used */
        _PeerState::ClearGuildArgs(peerState->guildArgs);
    }
    return;
Exit:
    if (ER_OK != status) {
        _PeerState::ClearGuildArgs(peerState->guildArgs);
        peerState->guildArgsSentCount = 0;
        MethodReply(msg, status);
    }
}


}
