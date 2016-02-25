/**
 * @file
 * Router is responsible for taking inbound messages and routing them
 * to an appropriate set of endpoints.
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
#include <qcc/Logger.h>
#include <qcc/String.h>
#include <qcc/Util.h>
#include <qcc/atomic.h>

#include <alljoyn/AllJoynStd.h>
#include <alljoyn/Status.h>

#include "BusEndpoint.h"
#include "ConfigDB.h"
#include "DaemonRouter.h"
#include "EndpointHelper.h"
#include "AllJoynObj.h"
#include "SessionlessObj.h"
#ifdef ENABLE_POLICYDB
#include "PolicyDB.h"
#endif

#define QCC_MODULE "ROUTER"

using namespace std;
using namespace qcc;

namespace ajn {

#define SESSION_SELF_JOIN 0x02

DaemonRouter::DaemonRouter() : ruleTable(), nameTable(), busController(NULL), alljoynObj(NULL), sessionlessObj(NULL)
{
#ifdef ENABLE_POLICYDB
    AddBusNameListener(ConfigDB::GetConfigDB());
#endif
}

DaemonRouter::~DaemonRouter()
{
}

static inline QStatus SendThroughEndpoint(Message& msg, BusEndpoint& ep, SessionId sessionId)
{
    QCC_DbgTrace(("SendThroughEndpoint(): Routing \"%s\" (%d) through \"%s\"", msg->Description().c_str(), msg->GetCallSerial(), ep->GetUniqueName().c_str()));
    QStatus status;
    if ((sessionId != 0) && (ep->GetEndpointType() == ENDPOINT_TYPE_VIRTUAL)) {
        status = VirtualEndpoint::cast(ep)->PushMessage(msg, sessionId);
    } else {
        status = ep->PushMessage(msg);
    }
    // if the bus is stopping or the endpoint is closing we don't expect to be able to send
    if ((status != ER_OK) && (status != ER_BUS_ENDPOINT_CLOSING) && (status != ER_BUS_STOPPING)) {
        QCC_DbgPrintf(("SendThroughEndpoint(dest=%s, ep=%s, id=%u) failed: %s", msg->GetDestination(), ep->GetUniqueName().c_str(), sessionId, QCC_StatusText(status)));
    }
    return status;
}

#ifdef ENABLE_OLD_PUSHMESSAGE_COMPATIBILITY

/*
 * The following function alters the decision to include/exclude a potential
 * destination endpoint in the list of destination endpoints that is built up in
 * PushMessage() so that PushMessage() will retain the same behavior as the
 * previous implementation.  The behaviors this function tries to model are
 * considered to be either bugs or oddities.
 *
 * This was broken out into a separate function for 3 reasons:
 *
 *   1. It helps to highlight certain odd or inconsistent behavior in the
 *      original implementation of PushMessage().
 *
 *   2. It allows the new implementation of PushMessage() to be cleaner.
 *
 *   3. It makes the odd/inconsistent behavior easier to remove once the rest of
 *      the system has been updated to work with the sanitized behavior.
 */
bool DaemonRouter::AddCompatibilityOverride(bool add,
                                            BusEndpoint& src,
                                            BusEndpoint& dest,
                                            const SessionId sessionId,
                                            const bool isBroadcast,
                                            const bool isSessioncast,
                                            const bool isSessionless,
                                            const bool isGlobalBroadcast,
                                            const SessionId detachId)
{
    const bool srcIsB2b = (src->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS);
    const bool destIsB2b = (dest->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS);

    if (isBroadcast) {
        if (isGlobalBroadcast && destIsB2b && detachId != 0) {
            RemoteEndpoint b2bDest = RemoteEndpoint::cast(dest);
            /*
             * DetachSession Hack Part 2: Ensure that the destination endpoint
             * gets the DetachSession message if and only if it is in this session.
             */
            add = (b2bDest->GetSessionId() == detachId);
        }
    } else if (isSessioncast) {
        if (!add) {
            /*
             * The original implementation of PushMessage() ignored the
             * AllowRemoteMessages flag on destination endpoints for messages
             * delivered via a session.
             *
             * ASACORE-1609 - If an endpoint does not want remote messages, that
             *                should be honored regardless of whether it is in a
             *                session or not.
             */
            const bool srcInSession = src->IsInSession(sessionId);
            const bool destInSession = dest->IsInSession(sessionId);
            const bool destIsVirtual = (dest->GetEndpointType() == ENDPOINT_TYPE_VIRTUAL);
            m_Lock.Lock(MUTEX_CONTEXT);
            const bool selfJoin = selfJoinEps.find(pair<String, SessionId>(src->GetUniqueName(), sessionId)) != selfJoinEps.end();
            m_Lock.Unlock(MUTEX_CONTEXT);
            add = add || (srcInSession && destInSession && !destIsVirtual && ((src != dest) || selfJoin));
        }
    }

    /*
     * In the original implementation, sessionless messages would be delivered
     * to the endpoint(s) directly unless it was sent by a B2B endpoint.  This
     * seems like inconsistent behavior.
     */
    add = add && !(srcIsB2b && isSessionless);

    QCC_DbgPrintf(("    compatibility override: add = %d", add));
    return add;
}

/*
 * The following function alters the status code returned from PushMessage() to
 * better mimic the behavior of the original implementation of PushMessage() for
 * cases where the return code from the original implementation is inconsistent
 * with what is really happening.
 */
QStatus DaemonRouter::StatusCompatibilityOverride(QStatus status,
                                                  BusEndpoint& src,
                                                  const bool isSessioncast,
                                                  const bool isSessionless,
                                                  const bool policyRejected)
{
    const bool srcIsB2b = (src->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS);

    if (isSessioncast && srcIsB2b && isSessionless && !policyRejected) {
        /*
         * While a sessionless message sent over a session is not normal for the
         * system, the original implementation would always return ER_OK for
         * sessionless messages with no direct destination that was sent over a
         * session from a B2B endpoint provided they weren't rejected by policy
         * rules.  (Since this is not a normal condition to begin with, perhaps
         * this override function can be removed.)
         */
        return ER_OK;
    }

    return status;
}

#endif

bool DaemonRouter::IsSessionDeliverable(SessionId sessionId, BusEndpoint& src, BusEndpoint& dest)
{
    bool add = true;
    const bool srcInSession = src->IsInSession(sessionId);
    const bool destInSession = dest->IsInSession(sessionId);
    const bool destIsVirtual = (dest->GetEndpointType() == ENDPOINT_TYPE_VIRTUAL);
    m_Lock.Lock(MUTEX_CONTEXT);
    const bool selfJoin = selfJoinEps.find(pair<String, SessionId>(src->GetUniqueName(), sessionId)) != selfJoinEps.end();
    m_Lock.Unlock(MUTEX_CONTEXT);
    /*
     * Ideally, the client library should handle the self join case locally, but
     * we need to handle it here in case clients connect to us that don't handle
     * self join in the client library.
     */
    add = add && srcInSession && destInSession && !destIsVirtual && ((src != dest) || selfJoin);

    return add;
}

QStatus DaemonRouter::PushMessage(Message& msg, BusEndpoint& src)
{
    QCC_DbgTrace(("DaemonRouter::PushMessage(): Routing %s\"%s\" (%d) from \"%s\"",
                  msg->IsSessionless() ? "sessionless " : "",
                  msg->Description().c_str(), msg->GetCallSerial(), src->GetUniqueName().c_str()));

    assert(src->GetEndpointType() != ENDPOINT_TYPE_VIRTUAL);
    /*
     * Since asserts are compiled out in release code, we return an error here.
     */
    if (src->GetEndpointType() == ENDPOINT_TYPE_VIRTUAL) {
        return ER_BUS_NO_ROUTE;
    }

    /*
     * Make a local reference to localEndpoint since it could be altered under
     * us by another thread.
     */
    m_Lock.Lock();
    LocalEndpoint lep = localEndpoint;
    m_Lock.Unlock();

    if (!lep->IsValid()) {
        // ASACORE-1620 - look into removing this block.
        QCC_DbgTrace(("localEndpoint not valid"));
        return ER_BUS_ENDPOINT_CLOSING;
    }

    if (src == lep) {
        // ASACORE-1620 - look into moving call to UpdateSerialNumber to a better location.
        QCC_DbgTrace(("sender is localEndpoint - updating serial number"));
        lep->UpdateSerialNumber(msg);
    }

    SessionId sessionId = msg->GetSessionId();

    /*
     * DetachSession Hack Part 1: The following hack needs some explanation.
     * The DetatchSession message is a signal sent from AllJoynObj from one
     * routing node to AllJoynObj of other routing nodes.  This means that the
     * message is sent from the "LocalEndpoint" of one routing node to the
     * "LocalEndpoint" of another routing.  LocalEndpoints are never members of
     * any session so the DetachSession message cannot be sent over the session
     * being detached from.  However, in order to prevent a race condition, that
     * message must be sent over the connection associated with the session that
     * is being detached from to ensure that all queued messages on that session
     * are delivered.  Normally, non-session messages are sent over any one of
     * the existing connections.  To ensure that it gets delivered to other
     * routing nodes that support endpoints that are members of the session,
     * local copy of sessionId is set to the session that is being detached from
     * so that the rest of this function will deliver it to all the members of
     * the session as if it were a sesssioncast message.
     *
     * This hack is spread out over three parts:
     *
     *    1. This part gets the sessionId being detached which is also used to
     *       indicate to the other parts of this hack that they are to take
     *       effect.
     *
     *    2. Ensures that the destination endpoint will receive the
     *       DetachSession message.
     *
     *    3. Overrides the connection used to deliver the DetachSession message
     *       so that it goes of the session being detached.
     *
     * ASACORE-1621: Change AllJoynObj to send DetachSession as a direct message
     *               to each routing node hosting an endpoint in the session
     *               over the session being detached from.  That would remove
     *               the need for this hack and be cleaner overall.
     *               Unfortunately, for backward compatibility, there would need
     *               to be code that will look for DetachSession coming in as a
     *               broadcast message from older routing nodes.  This could be
     *               handled entirely within AllJoynObj.
     */
    SessionId detachId = 0;
    if ((strcmp("DetachSession", msg->GetMemberName()) == 0) &&
        (strcmp(org::alljoyn::Daemon::InterfaceName, msg->GetInterface()) == 0)) {
        /* Clone the message since this message is unmarshalled by the
         * LocalEndpoint too and the process of unmarshalling is not
         * thread-safe.
         */
        Message clone = Message(msg, true);
        QStatus lStatus = clone->UnmarshalArgs("us");
        if (lStatus == ER_OK) {
            detachId = clone->GetArg(0)->v_uint32;
        } else {
            QCC_LogError(lStatus, ("Failed to unmarshal args for DetachSession message"));
        }
    }

    /*
     * The basic strategy taken here to determine which endpoints are to receive
     * the message is to first get a list of all the known endpoints, then check
     * to see if each endpoint in turn is supposed to receive the message or
     * not.  In the case of messages with an explicit destination, only that
     * destination will be considered.  The goal is to provide a code path that
     * is (nearly) identical for all message types.  By reducing the code paths,
     * there are fewer special cases which yields a structure that is
     * significantly easier to maintain.  For example, if we were to add D-Bus's
     * eavesdrop capability to message delivery, then the changes necessary to
     * iterate over the entire list of endpoints for all messages and not just
     * broadcast/sessioncast messages would be minimal.
     *
     * The first step is to collect some information about the message and
     * sender in a form that is more efficient to test and easier to read.
     */

    const String destination =        msg->GetDestination();
    const bool isUnicast =            !destination.empty();
    const bool isNulSession =         (sessionId == 0);
    const bool isBroadcast =          (!isUnicast && isNulSession);
    const bool isSessioncast =        (!isUnicast && !isNulSession);
    const bool replyIsExpected =      ((msg->GetType() == MESSAGE_METHOD_CALL) &&
                                       ((msg->GetFlags() & ALLJOYN_FLAG_NO_REPLY_EXPECTED) == 0));
    const bool msgIsSessionless =     msg->IsSessionless();
    const bool msgIsGlobalBroadcast = msg->IsGlobalBroadcast();

    const bool srcIsB2b =             (src->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS);
    const bool srcIsOurEp =           (!srcIsB2b);  // EP is directly connected to this router
    const bool srcAllowsRemote =      src->AllowRemoteMessages();

    vector<BusEndpoint> allEps;
    deque<BusEndpoint> destEps;

    bool blocked = false;
    bool blockedReply = false;
    bool policyRejected = false;

#ifdef ENABLE_POLICYDB
    PolicyDB policyDB = ConfigDB::GetConfigDB()->GetPolicyDB();
    NormalizedMsgHdr nmh(msg, policyDB, src);
#endif

    if (isUnicast) {
        /*
         * Only put the one endpoint that corresponds to the destination into
         * allEps for processing.  NOTE: If the destination is a Bus-to-bus
         * endpoint we must fallback to iterating over those endpoints.
         */
        BusEndpoint ep = nameTable.FindEndpoint(destination);
        if (ep->IsValid()) {
            allEps.push_back(ep);
        }
    } else {
        /*
         * Here we get a list of all the known non-Bus-to-bus endpoints in the
         * system.
         */
        nameTable.GetAllBusEndpoints(allEps);
    }

    if (!isUnicast || allEps.empty()) {
        /*
         * Here we get a list of all the known Bus-to-bus endpoints in the
         * system.  Oddly, Bus2Bus endpoints are not in the Name Table but
         * instead are kept in a set<> contained in the DaemonRouter class.
         * (AllJoynObj also keeps a list of Bus2Bus endpoints as well.)
         *
         * ASACORE-1622: There should be one central structure that contains
         *               *ALL* known endpoints.  Once fixed, this block can be
         *               removed.
         *
         * First, reserve room for the Bus-to-bus endpoints.  This could cause a
         * realloc(), but its better to do that once rather than n times as
         * entries get added.  (This won't be an issue once ASACORE-1622 is
         * resolved.)
         */
        allEps.reserve(allEps.size() + m_b2bEndpoints.size());
        m_Lock.Lock();
        for (set<RemoteEndpoint>::iterator it = m_b2bEndpoints.begin(); it != m_b2bEndpoints.end(); ++it) {
            RemoteEndpoint rep = *it;
            BusEndpoint ep = BusEndpoint::cast(rep);
            allEps.push_back(ep);
        }
        m_Lock.Unlock();
    }

    /*
     * Here is where we iterate over all the known endpoints to determine which
     * ones will receive the message.
     */
    for (vector<BusEndpoint>::const_iterator it = allEps.begin(); it != allEps.end(); ++it) {
        BusEndpoint dest = *it;
        const bool destIsDirect =     (isUnicast && nameTable.IsAlias(dest->GetUniqueName(), destination));
        // Is dest directly connected to this router?
        const bool destIsOurEp =      ((dest->GetEndpointType() == ENDPOINT_TYPE_LOCAL) ||
                                       (dest->GetEndpointType() == ENDPOINT_TYPE_NULL) ||
                                       (dest->GetEndpointType() == ENDPOINT_TYPE_REMOTE));
        const bool destIsB2b =        (dest->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS);
        const bool destAllowsRemote = dest->AllowRemoteMessages();

        bool add = true;

        QCC_DbgPrintf(("Checking endpoint: %s (%s connected)",
                       dest->GetUniqueName().c_str(), destIsOurEp ? "directly" : "indirectly"));

#ifdef ENABLE_POLICYDB
        /*
         * Do the policy rules allow for the message to be delivered?  (The
         * check for sending is kept separate from the check for receiving to
         * allow for easier changes should they be necessary in the future.)
         */
        add = add && policyDB->OKToSend(nmh, dest);
        add = add && policyDB->OKToReceive(nmh, dest);
        if (!add) {
            QCC_DbgPrintf(("    policy rejected"));
            policyRejected = true;
            continue;
        }
#endif

        /*
         * Is the message blocked because the receiver does not want to receive
         * messages from off device?
         */
        add = add && (destAllowsRemote || (srcIsOurEp && destIsOurEp));
        if (!add) {
            QCC_DbgPrintf(("    blocked - remote messages not allowed"));
            blocked = blocked || destIsDirect;
#ifdef ENABLE_OLD_PUSHMESSAGE_COMPATIBILITY
            goto addOverride;  // remove this when compatibility override is removed
#endif
            continue;
        }

        /*
         * Optimization: Will the sender block the reply to this message because
         * the reply will be coming from an off device endpoint?
         */
        add = add && (!replyIsExpected || srcAllowsRemote || (srcIsOurEp && destIsOurEp));
        if (!add) {
            if (destIsDirect) {
                QCC_DbgPrintf(("    blocked - remote reply message not allowed"));
            }
            blockedReply = blockedReply || destIsDirect;
#ifdef ENABLE_OLD_PUSHMESSAGE_COMPATIBILITY
            goto addOverride;  // remove this when compatibility override is removed
#endif
            continue;
        }

        /*
         * Does the destination endpoint match the destination name specified in
         * the message?
         */
        add = add && (!isUnicast || destIsDirect);
        if (isUnicast) {
            QCC_DbgPrintf(("    unicast dest->GetUniqueName() => %s   destination = %s   add = %d",
                           dest->GetUniqueName().c_str(), destination.c_str(), add));
        }

        /*
         * Is the message a global broadcast message or is it a message for a
         * local (on device) endpoint that has a match rule for the message?
         *
         * ASACORE-1623: This conditional for broadcast messages is too complex.
         *               Can we deprecate the GlobalBroadcast flag?
         */
        add = add && (!isBroadcast || ((msgIsGlobalBroadcast && destIsB2b && (src != dest)) ||
                                       ruleTable.OkToSend(msg, dest)));
        if (isBroadcast) {
            QCC_DbgPrintf(("    broadcast src = %s   dest = %s   global bcast = %d   dest epType = %d   ruleTable.OkToSend() => %d   add = %d",
                           src->GetUniqueName().c_str(), dest->GetUniqueName().c_str(),
                           msgIsGlobalBroadcast, dest->GetEndpointType(), ruleTable.OkToSend(msg, dest), add));
        }

        add = add && (!isSessioncast || IsSessionDeliverable(sessionId, src, dest));
        if (isSessioncast) {
            QCC_DbgPrintf(("    sessioncast id = %u   src = %s   dest = %s   add = %d",
                           sessionId, src->GetUniqueName().c_str(), dest->GetUniqueName().c_str(), add));
        }

#ifdef ENABLE_OLD_PUSHMESSAGE_COMPATIBILITY
    addOverride:    // WS violation courtesy of uncrustify.
        add = AddCompatibilityOverride(add,
                                       src,
                                       dest,
                                       sessionId,
                                       isBroadcast,
                                       isSessioncast,
                                       msgIsSessionless,
                                       msgIsGlobalBroadcast,
                                       detachId);
#endif

        if (add) {
            destEps.push_back(dest);
            QCC_DbgPrintf(("    dest %s added: %u", dest->GetUniqueName().c_str(), destEps.size()));
        }
    }

    /*
     * At this point destEps now contains all the endpoints that will receive
     * this message normally.  That is, the message is either directed to a
     * specific endpoint, the message is a normal broadcast message, or the
     * message will be sent to destinations that are members of the session the
     * message was sent over.
     */

    QStatus status = ER_NONE;

    /*
     * ASACORE-1626: Shouldn't sessionless message delivery be unified with
     *               normal message delivery?
     *
     * ASACORE-1626: The conditional for sending sessionless messages is too
     *               complex.  Additionally, it is not clear that some messages
     *               may go to both the SessionlessObj *and* to normal endpoints
     *               directly.  A cleaner solution, would be for SessionlessObj
     *               to get the message via localEndpoint and decide how to
     *               handle the sessionless message on its own.
     */
    if (msgIsSessionless && !policyRejected && (isBroadcast || srcIsB2b)) {
        if (srcIsB2b) {
            QCC_DbgPrintf(("sessionless msg delivered via sessionlessObj"));
            /*
             * The Sessionless Object is responsible for routing of sessionless
             * signals.  Specifically, sessionless signals that are received
             * solely to "catch-up" a newly connected local client are routed
             * directly to that client by the Sessionless Object.
             *
             * Sessionless messages conceptually don't have a session ID, but
             * they do get sent over a temporary session and the lifetime of
             * this session is used by SessionlessObj to determine when it is
             * done fetching all the updated sessionless messages.  Therefore,
             * get the sessionId from the endpoint if possible.
             */
            RemoteEndpoint rep = RemoteEndpoint::cast(src);
            sessionlessObj->RouteSessionlessMessage(rep->GetSessionId(), msg);
            status = ER_OK;
        } else if (isBroadcast) {
            status = sessionlessObj->PushMessage(msg);
        }
    }

    if (!destEps.empty()) {
        status = (status == ER_NONE) ? ER_OK : status;

        /*
         * DetachSession Hack Part 3 - Force the DetachSession message to go
         * over the session being detached.
         */
        sessionId = (detachId != 0) ? detachId : sessionId;
        for (deque<BusEndpoint>::iterator it = destEps.begin(); it != destEps.end(); ++it) {
            BusEndpoint& ep = *it;
            QStatus tStatus = SendThroughEndpoint(msg, ep, sessionId);
            QCC_DbgPrintf(("msg delivered via SendThroughEndpoint() to %s: %s",
                           ep->GetUniqueName().c_str(), QCC_StatusText(tStatus)));
            status = (status == ER_OK) ? tStatus : status;
        }
    }

    if (status == ER_NONE) {
        /*
         * The message was not delivered to anyone, so figure out what to do for
         * this error condition.
         */
        status = policyRejected ? ER_BUS_POLICY_VIOLATION : ER_BUS_NO_ROUTE;

#ifdef ENABLE_OLD_PUSHMESSAGE_COMPATIBILITY
        status = StatusCompatibilityOverride(status, src, isSessioncast, msgIsSessionless, policyRejected);
#endif
    }

    assert(status != ER_NONE);

    // ASACORE-1632: Why are autogenerated error replies not sent when the sender is a B2B endpoint?
    if ((status != ER_OK) && replyIsExpected && !srcIsB2b) {
        // Method call with reply expected so send an error.
        BusEndpoint busEndpoint = BusEndpoint::cast(lep);
        String blockedDesc = "Remote method call blocked -- ";

        if (policyRejected) {
            blockedDesc += "policy rule denies message delivery.";
        } else if (blocked) {
            blockedDesc += "endpoint does not accept off device messages.";
        } else if (blockedReply) {
            blockedDesc += "reply from off device endpoint would be blocked.";
        } else {
            blockedDesc += "destination does not exist.";
        }
        blockedDesc += "  Destination = ";
        blockedDesc += destination;

        QCC_DbgPrintf(("Sending ERROR auto reply: %s", blockedDesc.c_str()));
        msg->ErrorMsg(msg, "org.alljoyn.Bus.Blocked", blockedDesc.c_str());
        PushMessage(msg, busEndpoint);
    }

    return status;
}


void DaemonRouter::GetBusNames(vector<qcc::String>& names) const
{
    nameTable.GetBusNames(names);
}

BusEndpoint DaemonRouter::FindEndpoint(const qcc::String& busName)
{
    BusEndpoint ep = nameTable.FindEndpoint(busName);
    if (!ep->IsValid()) {
        m_Lock.Lock(MUTEX_CONTEXT);
        for (set<RemoteEndpoint>::const_iterator it = m_b2bEndpoints.begin(); it != m_b2bEndpoints.end(); ++it) {
            if ((*it)->GetUniqueName() == busName) {
                RemoteEndpoint rep = *it;
                ep = BusEndpoint::cast(rep);
                break;
            }
        }
        m_Lock.Unlock(MUTEX_CONTEXT);
    }
    return ep;
}

QStatus DaemonRouter::AddRule(BusEndpoint& endpoint, Rule& rule)
{
    QStatus status = ruleTable.AddRule(endpoint, rule);

    /* Allow sessionlessObj to examine this rule */
    if (status == ER_OK) {
        sessionlessObj->AddRule(endpoint->GetUniqueName(), rule);
    }

    return status;
}

QStatus DaemonRouter::RemoveRule(BusEndpoint& endpoint, Rule& rule)
{
    QStatus status = ruleTable.RemoveRule(endpoint, rule);
    if (ER_OK == status) {
        /* Allow sessionlessObj to examine rule being removed */
        sessionlessObj->RemoveRule(endpoint->GetUniqueName(), rule);
    }
    return status;
}

QStatus DaemonRouter::RegisterEndpoint(BusEndpoint& endpoint)
{
    QCC_DbgTrace(("DaemonRouter::RegisterEndpoint(%s, %d)", endpoint->GetUniqueName().c_str(), endpoint->GetEndpointType()));
    QStatus status = ER_OK;

    /* Keep track of local endpoint */
    if (endpoint->GetEndpointType() == ENDPOINT_TYPE_LOCAL) {
        m_Lock.Lock();
        localEndpoint = LocalEndpoint::cast(endpoint);
        m_Lock.Unlock();
    }

    if (endpoint->GetEndpointType() == ENDPOINT_TYPE_BUS2BUS) {
        /* AllJoynObj is in charge of managing bus-to-bus endpoints and their names */
        RemoteEndpoint busToBusEndpoint = RemoteEndpoint::cast(endpoint);
        status = alljoynObj->AddBusToBusEndpoint(busToBusEndpoint);

        /* Add to list of bus-to-bus endpoints */
        m_Lock.Lock(MUTEX_CONTEXT);
        m_b2bEndpoints.insert(busToBusEndpoint);
        m_Lock.Unlock(MUTEX_CONTEXT);
    } else {
        /* Bus-to-client endpoints appear directly on the bus */
        nameTable.AddUniqueName(endpoint);
    }

    /* Notify local endpoint that it is connected */
    if (endpoint->GetEndpointType() == ENDPOINT_TYPE_LOCAL) {
        /*
         * Use casted endpoint in the unlikely event that UnregisterEndpoint is
         * called in another thread.
         */
        LocalEndpoint::cast(endpoint)->OnBusConnected();
    }

    return status;
}

void DaemonRouter::UnregisterEndpoint(const qcc::String& epName, EndpointType epType)
{
    QCC_UNUSED(epType);

    QCC_DbgTrace(("DaemonRouter::UnregisterEndpoint: %s", epName.c_str()));

    /* Attempt to get the endpoint */
    nameTable.Lock();
    BusEndpoint endpoint = FindEndpoint(epName);
    nameTable.Unlock();

    if (ENDPOINT_TYPE_BUS2BUS == endpoint->GetEndpointType()) {
        /* Inform bus controller of bus-to-bus endpoint removal */
        RemoteEndpoint busToBusEndpoint = RemoteEndpoint::cast(endpoint);

        alljoynObj->RemoveBusToBusEndpoint(busToBusEndpoint);

        /* Remove the bus2bus endpoint from the list */
        m_Lock.Lock(MUTEX_CONTEXT);
        set<RemoteEndpoint>::iterator it = m_b2bEndpoints.begin();
        while (it != m_b2bEndpoints.end()) {
            RemoteEndpoint rep = *it;
            if (rep == busToBusEndpoint) {
                m_b2bEndpoints.erase(it);
                break;
            }
            ++it;
        }
        m_Lock.Unlock(MUTEX_CONTEXT);

    } else {
        /* Remove endpoint from names and rules */
        nameTable.RemoveUniqueName(endpoint->GetUniqueName());
        RemoveAllRules(endpoint);
        PermissionMgr::CleanPermissionCache(endpoint);
    }
    /*
     * If the local endpoint is being deregistered this indicates the router is being shut down.
     */
    m_Lock.Lock();
    if (endpoint == localEndpoint) {
        localEndpoint->Invalidate();
        localEndpoint = LocalEndpoint();
    }
    m_Lock.Unlock();
}

}

