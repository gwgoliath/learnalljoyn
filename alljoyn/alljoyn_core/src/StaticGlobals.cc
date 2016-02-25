/**
 * @file
 * Static global creation and destruction
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
#include <qcc/StaticGlobals.h>
#include <alljoyn/Init.h>
#include <alljoyn/PasswordManager.h>
#include "AutoPingerInternal.h"
#include "BusInternal.h"
#include "NamedPipeClientTransport.h"

namespace ajn {

class StaticGlobals {
  public:
    static void Init()
    {
        NamedPipeClientTransport::Init();
        AutoPingerInternal::Init();
        PasswordManager::Init();
        BusAttachment::Internal::Init();
    }

    static void Shutdown()
    {
        BusAttachment::Internal::Shutdown();
        PasswordManager::Shutdown();
        AutoPingerInternal::Shutdown();
        NamedPipeClientTransport::Shutdown();
    }
};

}

extern "C" {

static uint32_t allJoynInitCount = 0;
static qcc::Mutex allJoynInitLock;

QStatus AJ_CALL AllJoynInit(void)
{
    QStatus status = ER_OK;

    allJoynInitLock.Lock();

    if (allJoynInitCount == 0) {
        status = qcc::Init();
        if (status == ER_OK) {
            ajn::StaticGlobals::Init();
            allJoynInitCount = 1;
        }
    } else if (allJoynInitCount < 0xFFFFFFFF) {
        allJoynInitCount++;
    }

    allJoynInitLock.Unlock();

    return status;
}

QStatus AJ_CALL AllJoynShutdown(void)
{
    allJoynInitLock.Lock();

    if (allJoynInitCount > 0) {
        allJoynInitCount--;

        if (allJoynInitCount == 0) {
            ajn::StaticGlobals::Shutdown();
            qcc::Shutdown();
        }
    }

    allJoynInitLock.Unlock();

    return ER_OK;
}

}
