/**
 * @file
 * Implementation of class for launching a bundled router
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
#include "BundledRouter.h"
#include "RouterGlobals.h"

static ajn::BundledRouter* bundledRouter = NULL;

extern "C" {

static uint32_t allJoynRouterInitCount = 0;
static qcc::Mutex allJoynRouterInitLock;

QStatus AJ_CALL AllJoynRouterInit(void)
{
    allJoynRouterInitLock.Lock();

    if (allJoynRouterInitCount == 0) {
        ajn::RouterGlobals::Init();
        bundledRouter = new ajn::BundledRouter();
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
            delete bundledRouter;
            bundledRouter = NULL;
            ajn::RouterGlobals::Shutdown();
        }
    }

    allJoynRouterInitLock.Unlock();

    return ER_OK;
}


}
