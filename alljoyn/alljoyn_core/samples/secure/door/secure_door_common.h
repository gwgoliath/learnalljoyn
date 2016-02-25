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

#ifndef ALLJOYN_SAMPLE_SECURE_DOOR_COMMON_H_
#define ALLJOYN_SAMPLE_SECURE_DOOR_COMMON_H_

#include <string>
#include <cstdio>

#include <qcc/GUID.h>
#include <qcc/Mutex.h>
#include <qcc/Condition.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/AuthListener.h>
#include <alljoyn/AboutObj.h>
#include <alljoyn/PermissionConfigurationListener.h>

/* Door sample common definitions. */
#define DOOR_INTERFACE "sample.securitymgr.door.Door"
#define DOOR_OPEN "Open"
#define DOOR_CLOSE "Close"
#define DOOR_GET_STATE "GetState"
#define DOOR_STATE "State"
#define DOOR_STATE_CHANGED "StateChanged"
#define DOOR_SIGNAL_MATCH_RULE  "type='signal',interface='" DOOR_INTERFACE "',member='" DOOR_STATE_CHANGED "'"

#define DOOR_OBJECT_PATH "/sample/security/Door"

#define KEYX_ECDHE_NULL "ALLJOYN_ECDHE_NULL"
#define KEYX_ECDHE_PSK "ALLJOYN_ECDHE_PSK"
#define KEYX_ECDHE_DSA "ALLJOYN_ECDHE_ECDSA"

#define DOOR_APPLICATION_PORT 12345

using namespace ajn;
using namespace std;
using namespace qcc;

namespace sample {
namespace secure {
namespace door {

/* DoorComon PermissionConfigurationListener*/
class DoorCommonPCL : public PermissionConfigurationListener {

  public:
    DoorCommonPCL(BusAttachment& _ba) : ba(_ba) { }

    void PolicyChanged();

    bool WaitForClaimedState();

  private:
    BusAttachment& ba;
    Mutex lock;
    Condition sem;
};

/* Session port listener. */
class SPListener :
    public SessionPortListener {
    bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
    {
        QCC_UNUSED(opts);
        QCC_UNUSED(joiner);
        QCC_UNUSED(sessionPort);

        return true; // Always accept sessions regardless.
    }
};

/* Door bus object that emulates a door on the bus; it opens and closes, etc. */
class Door :
    public BusObject {
  public:
    Door(BusAttachment* ba);

    ~Door() { }

    void SendDoorEvent();

    bool autoSignal; // Automatically send signals on door state changes when set.

  protected:
    /* Get the property matching propName on the interface matching ifcName*/
    QStatus Get(const char* ifcName,
                const char* propName,
                MsgArg& val);

    void ReplyWithBoolean(bool answer,
                          Message& msg);

  private:
    bool open; // open = true, close = false

    const InterfaceDescription::Member* stateSignal;

    void Open(const InterfaceDescription::Member* member,
              Message& msg);

    void Close(const InterfaceDescription::Member* member,
               Message& msg);

    void GetState(const InterfaceDescription::Member* member,
                  Message& msg);

};

/* DoorCommon encapsulates the common setup needed for both door consumer and provider */
class DoorCommon {
  public:
    DoorCommon(string _appName) :
        appName(_appName),
        ba(new BusAttachment(_appName.c_str(), true)),
        aboutData("en"), aboutObj(new AboutObj(*ba))
    {
    }

    ~DoorCommon();

    QStatus Init(bool provider, PermissionConfigurationListener* pcl = nullptr);

    QStatus Fini();

    const InterfaceDescription::Member* GetDoorSignal()
    {
        return ba->GetInterface(DOOR_INTERFACE)->GetMember(DOOR_STATE_CHANGED);
    }

    BusAttachment* GetBusAttachment()
    {
        return ba;
    }

    QStatus AnnounceAbout();

    void UpdateManifest(const PermissionPolicy::Acl& manifest);

  private:
    QStatus CreateInterface();

    QStatus AdvertiseApplication();

    QStatus SetAboutData();

    QStatus HostSession();

    string appName;
    BusAttachment* ba;
    AboutData aboutData;
    AboutObj* aboutObj;
    SPListener spl;
};
}
}
}

#endif /* ALLJOYN_SAMPLE_SECURE_DOOR_COMMON_H_ */
