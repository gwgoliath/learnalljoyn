/**
 * @file
 *
 * This file implements a PermissionConfigurationListener subclass for use by the C API
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

#include <alljoyn/PermissionConfigurationListener.h>
#include <alljoyn_c/PermissionConfigurationListener.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <qcc/Debug.h>

#define QCC_MODULE "ALLJOYN_C"

using namespace qcc;
using namespace std;

namespace ajn {
/**
 * Abstract base class implemented by AllJoyn users and called by AllJoyn to inform
 * users of bus related events.
 */
class PermissionConfigurationListenerCallbackC : public PermissionConfigurationListener {
  public:
    PermissionConfigurationListenerCallbackC(const alljoyn_permissionconfigurationlistener_callbacks* callbacks_in, const void* context_in)
    {
        QCC_DbgTrace(("%s", __FUNCTION__));
        memcpy(&callbacks, callbacks_in, sizeof(alljoyn_permissionconfigurationlistener_callbacks));
        context = context_in;
    }

    virtual QStatus FactoryReset()
    {
        QCC_DbgTrace(("%s", __FUNCTION__));
        if (callbacks.factory_reset != NULL) {
            return callbacks.factory_reset(context);
        }
        return ER_OK;
    }

    virtual void PolicyChanged()
    {
        QCC_DbgTrace(("%s", __FUNCTION__));
        if (callbacks.policy_changed != NULL) {
            return callbacks.policy_changed(context);
        }
    }

  private:
    alljoyn_permissionconfigurationlistener_callbacks callbacks;
    const void* context;
};

}

struct _alljoyn_permissionconfigurationlistener_handle {
    /* Empty by design, this is just to allow the type restrictions to save coders from themselves */
};

alljoyn_permissionconfigurationlistener AJ_CALL alljoyn_permissionconfigurationlistener_create(const alljoyn_permissionconfigurationlistener_callbacks* callbacks, const void* context)
{
    QCC_DbgTrace(("%s", __FUNCTION__));
    return (alljoyn_permissionconfigurationlistener) new ajn::PermissionConfigurationListenerCallbackC(callbacks, context);
}

void AJ_CALL alljoyn_permissionconfigurationlistener_destroy(alljoyn_permissionconfigurationlistener listener)
{
    QCC_DbgTrace(("%s", __FUNCTION__));
    assert(listener != NULL && "listener parameter must not be NULL");
    delete (ajn::PermissionConfigurationListenerCallbackC*)listener;
}
