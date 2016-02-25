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
#include <qcc/Mutex.h>
#include <qcc/StaticGlobals.h>
#include <alljoyn/Init.h>
#include "RouterGlobals.h"

extern "C" {

static uint32_t allJoynRouterInitCount = 0;
static qcc::Mutex allJoynRouterInitLock;

QStatus AJ_CALL AllJoynRouterInit(void)
{
    allJoynRouterInitLock.Lock();

    if (allJoynRouterInitCount == 0) {
        ajn::RouterGlobals::Init();
        allJoynRouterInitCount = 1;
    } else if (allJoynRouterInitCount < 0xFFFFFFFF) {
        allJoynRouterInitCount++;
    }

    allJoynRouterInitLock.Unlock();

    return ER_OK;
}

QStatus AJ_CALL AllJoynRouterShutdown(void)
{
    allJoynRouterInitLock.Lock();

    if (allJoynRouterInitCount > 0) {
        allJoynRouterInitCount--;

        if (allJoynRouterInitCount == 0) {
            ajn::RouterGlobals::Shutdown();
        }
    }

    allJoynRouterInitLock.Unlock();

    return ER_OK;
}

}
