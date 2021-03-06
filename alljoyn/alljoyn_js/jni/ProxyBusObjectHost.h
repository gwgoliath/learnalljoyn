/*
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
 */
#ifndef _PROXYBUSOBJECTHOST_H
#define _PROXYBUSOBJECTHOST_H

#include "BusAttachment.h"
#include "ProxyBusObject.h"
#include "ScriptableObject.h"
#include <qcc/ManagedObj.h>
class _ProxyBusObjectHostImpl;

class _ProxyBusObjectHost : public ScriptableObject {
  public:
    _ProxyBusObjectHost(Plugin& plugin, BusAttachment& busAttachment, const char* serviceName, const char* path, ajn::SessionId sessionId);
    _ProxyBusObjectHost(Plugin& plugin, BusAttachment& busAttachment, ajn::ProxyBusObject* proxyBusObject);
    virtual ~_ProxyBusObjectHost();

  private:
    BusAttachment busAttachment;
    ProxyBusObject proxyBusObject;
    _ProxyBusObjectHostImpl* impl; /* Hide declaration of ProxyChildrenHost to get around recursive include. */

    void Initialize();

    bool getPath(NPVariant* result);
    bool getServiceName(NPVariant* result);
    bool getSessionId(NPVariant* result);
    bool getSecure(NPVariant* result);

    bool getChildren(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool getInterface(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool getInterfaces(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool introspect(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool methodCall(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool parseXML(const NPVariant* args, uint32_t argCount, NPVariant* result);
    bool secureConnection(const NPVariant* args, uint32_t argCount, NPVariant* result);
};

typedef qcc::ManagedObj<_ProxyBusObjectHost> ProxyBusObjectHost;

#endif // _PROXYBUSOBJECTHOST_H
