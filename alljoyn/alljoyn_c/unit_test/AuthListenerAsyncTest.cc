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

#include <gtest/gtest.h>

#include "ajTestCommon.h"

#include <alljoyn_c/BusAttachment.h>
#include <alljoyn_c/DBusStdDefines.h>
#include <alljoyn_c/AuthListener.h>
#include <alljoyn_c/Status.h>

#include <alljoyn_c/AjAPI.h>
#include <qcc/Thread.h>

static const char* INTERFACE_NAME = "org.alljoyn.test.c.authlistener.async";
static const char* OBJECT_NAME = "org.alljoyn.test.c.authlistener.async";
static const char* OBJECT_PATH = "/org/alljoyn/test";

static QCC_BOOL name_owner_changed_flag = QCC_FALSE;

static QCC_BOOL requestcredentials_service_flag = QCC_FALSE;
static QCC_BOOL authenticationcomplete_service_flag = QCC_FALSE;
static QCC_BOOL verifycredentials_service_flag = QCC_FALSE;
static QCC_BOOL securityviolation_service_flag = QCC_FALSE;

static QCC_BOOL requestcredentials_client_flag = QCC_FALSE;
static QCC_BOOL authenticationcomplete_client_flag = QCC_FALSE;
static QCC_BOOL verifycredentials_client_flag = QCC_FALSE;
static QCC_BOOL securityviolation_client_flag = QCC_TRUE;

/* NameOwnerChanged callback */
static void AJ_CALL name_owner_changed(const void* context, const char* busName, const char* previousOwner, const char* newOwner)
{
    QCC_UNUSED(context);
    QCC_UNUSED(previousOwner);
    QCC_UNUSED(newOwner);

    if (strcmp(busName, OBJECT_NAME) == 0) {
        name_owner_changed_flag = QCC_TRUE;
    }
}

/* Exposed methods */
static void AJ_CALL ping_method(alljoyn_busobject bus, const alljoyn_interfacedescription_member* member, alljoyn_message msg)
{
    QCC_UNUSED(member);

    alljoyn_msgarg outArg = alljoyn_msgarg_create();
    alljoyn_msgarg inArg = alljoyn_message_getarg(msg, 0);
    const char* str;
    alljoyn_msgarg_get(inArg, "s", &str);
    alljoyn_msgarg_set(outArg, "s", str);
    QStatus status = alljoyn_busobject_methodreply_args(bus, msg, outArg, 1);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    alljoyn_msgarg_destroy(outArg);
}

class AuthListenerAsyncTest : public testing::Test {
  public:
    virtual void SetUp() {
        /* initilize test fixture values to known start value */
        status = ER_FAIL;
        servicebus = NULL;
        clientbus = NULL;

        testObj = NULL;
        buslistener = NULL;

        /* set up the service bus */
        servicebus = alljoyn_busattachment_create("AuthListenerAsyncTestService", false);
        status = alljoyn_busattachment_start(servicebus);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        status = alljoyn_busattachment_connect(servicebus, ajn::getConnectArg().c_str());
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        alljoyn_interfacedescription service_intf = NULL;
        status = alljoyn_busattachment_createinterface_secure(servicebus, INTERFACE_NAME, &service_intf, AJ_IFC_SECURITY_REQUIRED);
        ASSERT_TRUE(service_intf != NULL);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        status = alljoyn_interfacedescription_addmember(service_intf, ALLJOYN_MESSAGE_METHOD_CALL, "ping", "s", "s", "in,out", 0);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        alljoyn_interfacedescription_activate(service_intf);

        clientbus = alljoyn_busattachment_create("AuthListenerAsyncTestClient", false);
        status = alljoyn_busattachment_start(clientbus);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        status = alljoyn_busattachment_connect(clientbus, ajn::getConnectArg().c_str());
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    }

    virtual void TearDown() {
        alljoyn_busattachment_stop(servicebus);
        alljoyn_busattachment_join(servicebus);
        alljoyn_busattachment_stop(clientbus);
        alljoyn_busattachment_join(clientbus);
        alljoyn_busattachment_destroy(servicebus);
        alljoyn_busattachment_destroy(clientbus);
        alljoyn_buslistener_destroy(buslistener);
        alljoyn_busobject_destroy(testObj);
    }

    void SetUpAuthService() {
        /* register bus listener */
        alljoyn_buslistener_callbacks buslistenerCbs = {
            NULL,
            NULL,
            NULL,
            NULL,
            &name_owner_changed,
            NULL,
            NULL,
            NULL
        };
        buslistener = alljoyn_buslistener_create(&buslistenerCbs, NULL);
        alljoyn_busattachment_registerbuslistener(servicebus, buslistener);

        /* Set up bus object */
        alljoyn_busobject_callbacks busObjCbs = {
            NULL,
            NULL,
            NULL,
            NULL
        };
        testObj = alljoyn_busobject_create(OBJECT_PATH, QCC_FALSE, &busObjCbs, NULL);
        const alljoyn_interfacedescription exampleIntf = alljoyn_busattachment_getinterface(servicebus, INTERFACE_NAME);
        ASSERT_TRUE(exampleIntf);

        status = alljoyn_busobject_addinterface(testObj, exampleIntf);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        /* register method handlers */
        alljoyn_interfacedescription_member ping_member;
        QCC_BOOL foundMember = alljoyn_interfacedescription_getmember(exampleIntf, "ping", &ping_member);
        EXPECT_TRUE(foundMember);

        /* add methodhandlers */
        alljoyn_busobject_methodentry methodEntries[] = {
            { &ping_member, ping_method },
        };
        status = alljoyn_busobject_addmethodhandlers(testObj, methodEntries, sizeof(methodEntries) / sizeof(methodEntries[0]));
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        status = alljoyn_busattachment_registerbusobject(servicebus, testObj);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        name_owner_changed_flag = QCC_FALSE;

        /* request name */
        uint32_t flags = DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE;
        status = alljoyn_busattachment_requestname(servicebus, OBJECT_NAME, flags);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        for (size_t i = 0; i < 200; ++i) {
            if (name_owner_changed_flag) {
                break;
            }
            qcc::Sleep(5);
        }
        EXPECT_TRUE(name_owner_changed_flag);
    }

    void SetupAuthClient() {
        alljoyn_proxybusobject proxyObj = alljoyn_proxybusobject_create(clientbus, OBJECT_NAME, OBJECT_PATH, 0);
        EXPECT_TRUE(proxyObj);
        status = alljoyn_proxybusobject_introspectremoteobject(proxyObj);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        alljoyn_message reply = alljoyn_message_create(clientbus);
        alljoyn_msgarg input = alljoyn_msgarg_create_and_set("s", "AllJoyn");
        status = alljoyn_proxybusobject_methodcall(proxyObj, INTERFACE_NAME, "ping", input, 1, reply, ALLJOYN_MESSAGE_DEFAULT_TIMEOUT, 0);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
        const char* str;
        alljoyn_msgarg_get(alljoyn_message_getarg(reply, 0), "s", &str);
        EXPECT_STREQ("AllJoyn", str);

        alljoyn_message_destroy(reply);
        alljoyn_msgarg_destroy(input);
        alljoyn_proxybusobject_destroy(proxyObj);

    }

    void SetupAuthClientAuthFail() {
        alljoyn_proxybusobject proxyObj = alljoyn_proxybusobject_create(clientbus, OBJECT_NAME, OBJECT_PATH, 0);
        EXPECT_TRUE(proxyObj);
        status = alljoyn_proxybusobject_introspectremoteobject(proxyObj);
        EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

        alljoyn_message reply = alljoyn_message_create(clientbus);
        alljoyn_msgarg input = alljoyn_msgarg_create_and_set("s", "AllJoyn");
        status = alljoyn_proxybusobject_methodcall(proxyObj, INTERFACE_NAME, "ping", input, 1, reply, 200, 0);
        EXPECT_EQ(ER_BUS_REPLY_IS_ERROR_MESSAGE, status) << "  Actual Status: " << QCC_StatusText(status);

        alljoyn_message_destroy(reply);
        alljoyn_msgarg_destroy(input);
        alljoyn_proxybusobject_destroy(proxyObj);
    }

    void ResetAuthFlags() {
        requestcredentials_service_flag = QCC_FALSE;
        authenticationcomplete_service_flag = QCC_FALSE;
        verifycredentials_service_flag = QCC_FALSE;
        securityviolation_service_flag = QCC_FALSE;

        requestcredentials_client_flag = QCC_FALSE;
        authenticationcomplete_client_flag = QCC_FALSE;
        verifycredentials_client_flag = QCC_FALSE;
        securityviolation_client_flag = QCC_FALSE;
    }
    QStatus status;
    alljoyn_busattachment servicebus;
    alljoyn_busattachment clientbus;

    alljoyn_busobject testObj;
    alljoyn_buslistener buslistener;
};

/* AuthListener callback functions*/
static QStatus AJ_CALL authlistener_requestcredentialsasync_service_srp_keyx(const void* context, alljoyn_authlistener listener,
                                                                             const char* authMechanism, const char* peerName,
                                                                             uint16_t authCount, const char* userName,
                                                                             uint16_t credMask, void* authContext) {
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);
    QCC_UNUSED(userName);

    alljoyn_credentials creds = alljoyn_credentials_create();
    EXPECT_STREQ("context test string", (const char*)context);
    EXPECT_STREQ("ALLJOYN_SRP_KEYX", authMechanism);
    if (credMask & ALLJOYN_CRED_PASSWORD) {
        alljoyn_credentials_setpassword(creds, "ABCDEFGH");
    }
    QStatus status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_TRUE, creds);
    alljoyn_credentials_destroy(creds);
    requestcredentials_service_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL alljoyn_authlistener_authenticationcomplete_service_srp_keyx(const void* context, const char* authMechanism,
                                                                                 const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);

    EXPECT_STREQ("context test string", (const char*)context);
    EXPECT_TRUE(success);
    authenticationcomplete_service_flag = QCC_TRUE;
}


static QStatus AJ_CALL authlistener_requestcredentialsasync_client_srp_keyx(const void* context, alljoyn_authlistener listener,
                                                                            const char* authMechanism, const char* peerName,
                                                                            uint16_t authCount, const char* userName,
                                                                            uint16_t credMask, void* authContext) {
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);
    QCC_UNUSED(userName);

    alljoyn_credentials creds = alljoyn_credentials_create();
    EXPECT_STREQ("context test string", (const char*)context);
    EXPECT_STREQ("ALLJOYN_SRP_KEYX", authMechanism);
    if (credMask & ALLJOYN_CRED_PASSWORD) {
        alljoyn_credentials_setpassword(creds, "ABCDEFGH");
    }
    QStatus status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_TRUE, creds);
    alljoyn_credentials_destroy(creds);
    requestcredentials_client_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL alljoyn_authlistener_authenticationcomplete_client_srp_keyx(const void* context, const char* authMechanism,
                                                                                const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);

    EXPECT_STREQ("context test string", (const char*)context);
    EXPECT_TRUE(success);
    authenticationcomplete_client_flag = QCC_TRUE;
}

TEST_F(AuthListenerAsyncTest, srp_keyx) {
    ResetAuthFlags();

    alljoyn_busattachment_clearkeystore(clientbus);

    /* set up the service */
    alljoyn_authlistenerasync_callbacks authlistener_cb_service = {
        authlistener_requestcredentialsasync_service_srp_keyx, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_service_srp_keyx //authenticationcomplete
    };

    alljoyn_authlistener serviceauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_service, (void*)"context test string");

    status = alljoyn_busattachment_enablepeersecurity(servicebus, "ALLJOYN_SRP_KEYX", serviceauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(servicebus);

    SetUpAuthService();

    /* set up the client */
    alljoyn_authlistenerasync_callbacks authlistener_cb_client = {
        authlistener_requestcredentialsasync_client_srp_keyx, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_client_srp_keyx //authenticationcomplete
    };

    alljoyn_authlistener clientauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_client, (void*)"context test string");

    status = alljoyn_busattachment_enablepeersecurity(clientbus, "ALLJOYN_SRP_KEYX", clientauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(clientbus);

    SetupAuthClient();

    EXPECT_TRUE(requestcredentials_service_flag);
    EXPECT_TRUE(authenticationcomplete_service_flag);

    EXPECT_TRUE(requestcredentials_client_flag);
    EXPECT_TRUE(authenticationcomplete_client_flag);

    alljoyn_authlistenerasync_destroy(serviceauthlistener);
    alljoyn_authlistenerasync_destroy(clientauthlistener);
}

/* AuthListener callback functions*/
static QStatus AJ_CALL authlistener_requestcredentialsasync_service_srp_logon(const void* context, alljoyn_authlistener listener,
                                                                              const char* authMechanism, const char* peerName,
                                                                              uint16_t authCount, const char* userName,
                                                                              uint16_t credMask, void* authContext) {
    QCC_UNUSED(context);
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);

    alljoyn_credentials creds = alljoyn_credentials_create();
    QStatus status = ER_FAIL;
    EXPECT_STREQ("ALLJOYN_SRP_LOGON", authMechanism);
    if (!userName) {
        status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_FALSE, creds);
    } else if (credMask & ALLJOYN_CRED_PASSWORD) {
        EXPECT_STREQ("Mr. Cuddles", userName);
        if (strcmp(userName, "Mr. Cuddles") == 0) {
            alljoyn_credentials_setpassword(creds, "123456");
            status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_TRUE, creds);
        } else {
            status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_FALSE, creds);
        }
    } else {
        status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_FALSE, creds);
    }
    alljoyn_credentials_destroy(creds);
    requestcredentials_service_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL alljoyn_authlistener_authenticationcomplete_service_srp_logon(const void* context, const char* authMechanism,
                                                                                  const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(context);
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);

    EXPECT_TRUE(success);
    authenticationcomplete_service_flag = QCC_TRUE;
}


static QStatus AJ_CALL authlistener_requestcredentialsasync_client_srp_logon(const void* context, alljoyn_authlistener listener,
                                                                             const char* authMechanism, const char* peerName,
                                                                             uint16_t authCount, const char* userName,
                                                                             uint16_t credMask, void* authContext) {
    QCC_UNUSED(context);
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);
    QCC_UNUSED(userName);

    alljoyn_credentials creds = alljoyn_credentials_create();
    EXPECT_STREQ("ALLJOYN_SRP_LOGON", authMechanism);
    if (credMask & ALLJOYN_CRED_USER_NAME) {
        alljoyn_credentials_setusername(creds, "Mr. Cuddles");
    }

    if (credMask & ALLJOYN_CRED_PASSWORD) {
        alljoyn_credentials_setpassword(creds, "123456");
    }
    QStatus status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_TRUE, creds);
    alljoyn_credentials_destroy(creds);
    requestcredentials_client_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL alljoyn_authlistener_authenticationcomplete_client_srp_logon(const void* context, const char* authMechanism,
                                                                                 const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(context);
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);

    EXPECT_TRUE(success);
    authenticationcomplete_client_flag = QCC_TRUE;
}

TEST_F(AuthListenerAsyncTest, srp_logon) {
    ResetAuthFlags();

    alljoyn_busattachment_clearkeystore(clientbus);

    /* set up the service */
    alljoyn_authlistenerasync_callbacks authlistener_cb_service = {
        authlistener_requestcredentialsasync_service_srp_logon, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_service_srp_logon //authenticationcomplete
    };

    alljoyn_authlistener serviceauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_service, NULL);

    status = alljoyn_busattachment_enablepeersecurity(servicebus, "ALLJOYN_SRP_LOGON", serviceauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(servicebus);

    SetUpAuthService();

    /* set up the client */
    alljoyn_authlistenerasync_callbacks authlistener_cb_client = {
        authlistener_requestcredentialsasync_client_srp_logon, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_client_srp_logon //authenticationcomplete
    };

    alljoyn_authlistener clientauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_client, NULL);

    status = alljoyn_busattachment_enablepeersecurity(clientbus, "ALLJOYN_SRP_LOGON", clientauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(clientbus);

    SetupAuthClient();

    EXPECT_TRUE(requestcredentials_service_flag);
    EXPECT_TRUE(authenticationcomplete_service_flag);

    EXPECT_TRUE(requestcredentials_client_flag);
    EXPECT_TRUE(authenticationcomplete_client_flag);

    alljoyn_authlistenerasync_destroy(serviceauthlistener);
    alljoyn_authlistenerasync_destroy(clientauthlistener);
}

/* AuthListener callback functions*/
static QStatus AJ_CALL authlistener_requestcredentialsasync_service_srp_keyx2(const void* context, alljoyn_authlistener listener,
                                                                              const char* authMechanism, const char* peerName,
                                                                              uint16_t authCount, const char* userName,
                                                                              uint16_t credMask, void* authContext) {
    QCC_UNUSED(context);
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);
    QCC_UNUSED(userName);
    QCC_UNUSED(credMask);

    EXPECT_STREQ("ALLJOYN_SRP_KEYX", authMechanism);
    alljoyn_credentials creds = alljoyn_credentials_create();
    QStatus status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_FALSE, creds);
    alljoyn_credentials_destroy(creds);
    requestcredentials_service_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL alljoyn_authlistener_authenticationcomplete_service_srp_keyx2(const void* context, const char* authMechanism,
                                                                                  const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(context);
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);

    EXPECT_FALSE(success);
    authenticationcomplete_service_flag = QCC_TRUE;
}


static QStatus AJ_CALL authlistener_requestcredentialsasync_client_srp_keyx2(const void* context, alljoyn_authlistener listener,
                                                                             const char* authMechanism, const char* peerName,
                                                                             uint16_t authCount, const char* userName,
                                                                             uint16_t credMask, void* authContext) {
    QCC_UNUSED(context);
    QCC_UNUSED(peerName);
    QCC_UNUSED(authCount);
    QCC_UNUSED(userName);
    QCC_UNUSED(credMask);

    EXPECT_STREQ("ALLJOYN_SRP_KEYX", authMechanism);
    alljoyn_credentials creds = alljoyn_credentials_create();
    QStatus status = alljoyn_authlistener_requestcredentialsresponse(listener, authContext, QCC_FALSE, creds);
    alljoyn_credentials_destroy(creds);
    requestcredentials_client_flag = QCC_TRUE;
    return status;
}

static void AJ_CALL authlistener_securityviolation_client_srp_keyx2(const void* context, QStatus status, const alljoyn_message msg) {
    QCC_UNUSED(context);
    QCC_UNUSED(status);
    QCC_UNUSED(msg);
    securityviolation_client_flag = QCC_TRUE;
}

/*
 * Run the SRP Key Exchange test again except this time fail the authentication
 * we expect to see an authlistener secutity violation.
 */
static void AJ_CALL alljoyn_authlistener_authenticationcomplete_client_srp_keyx2(const void* context, const char* authMechanism,
                                                                                 const char* peerName, QCC_BOOL success) {
    QCC_UNUSED(context);
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(peerName);
    EXPECT_FALSE(success);
    authenticationcomplete_client_flag = QCC_TRUE;
}

TEST_F(AuthListenerAsyncTest, srp_keyx2) {
    ResetAuthFlags();

    alljoyn_busattachment_clearkeystore(clientbus);

    /* set up the service */
    alljoyn_authlistenerasync_callbacks authlistener_cb_service = {
        authlistener_requestcredentialsasync_service_srp_keyx2, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_service_srp_keyx2 //authenticationcomplete
    };

    alljoyn_authlistener serviceauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_service, NULL);

    status = alljoyn_busattachment_enablepeersecurity(servicebus, "ALLJOYN_SRP_KEYX", serviceauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(servicebus);

    SetUpAuthService();

    /* set up the client */
    alljoyn_authlistenerasync_callbacks authlistener_cb_client = {
        authlistener_requestcredentialsasync_client_srp_keyx2, //requestcredentialsasync
        NULL, //verifycredentialsasync
        authlistener_securityviolation_client_srp_keyx2, //securityviolation
        alljoyn_authlistener_authenticationcomplete_client_srp_keyx2 //authenticationcomplete
    };

    alljoyn_authlistener clientauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_client, NULL);

    status = alljoyn_busattachment_enablepeersecurity(clientbus, "ALLJOYN_SRP_KEYX", clientauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(clientbus);

    SetupAuthClientAuthFail();

    //Wait upto 2 seconds for the signal to complete.
    for (int i = 0; i < 200; ++i) {

        if (securityviolation_client_flag) {
            break;
        }
        qcc::Sleep(10);
    }

    EXPECT_TRUE(requestcredentials_service_flag);
    EXPECT_TRUE(authenticationcomplete_service_flag);

    EXPECT_TRUE(authenticationcomplete_client_flag);
    EXPECT_TRUE(securityviolation_client_flag);

    alljoyn_authlistenerasync_destroy(serviceauthlistener);
    alljoyn_authlistenerasync_destroy(clientauthlistener);
}


/*
 * This test re-uses the authlisteners used for the srp_keyx unit test.  What
 * It is unimportant to know what authlistener is used, just that authentication
 * is being done when alljoyn_proxybusobject_secureconnectionasync is called.
 */
TEST_F(AuthListenerAsyncTest, secureconnectionasync) {
    ResetAuthFlags();

    alljoyn_busattachment_clearkeystore(clientbus);

    /* set up the service */
    alljoyn_authlistenerasync_callbacks authlistener_cb_service = {
        authlistener_requestcredentialsasync_service_srp_keyx, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_service_srp_keyx //authenticationcomplete
    };

    alljoyn_authlistener serviceauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_service, (void*)"context test string");

    status = alljoyn_busattachment_enablepeersecurity(servicebus, "ALLJOYN_SRP_KEYX", serviceauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(servicebus);

    SetUpAuthService();

    /* set up the client */
    alljoyn_authlistenerasync_callbacks authlistener_cb_client = {
        authlistener_requestcredentialsasync_client_srp_keyx, //requestcredentialsasync
        NULL, //verifycredentialsasync
        NULL, //securityviolation
        alljoyn_authlistener_authenticationcomplete_client_srp_keyx //authenticationcomplete
    };

    alljoyn_authlistener clientauthlistener = alljoyn_authlistenerasync_create(&authlistener_cb_client, (void*)"context test string");

    status = alljoyn_busattachment_enablepeersecurity(clientbus, "ALLJOYN_SRP_KEYX", clientauthlistener, NULL, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);
    /* Clear the Keystore between runs */
    alljoyn_busattachment_clearkeystore(clientbus);

    alljoyn_proxybusobject proxyObj = alljoyn_proxybusobject_create(clientbus, OBJECT_NAME, OBJECT_PATH, 0);
    ASSERT_TRUE(proxyObj);

    status = alljoyn_proxybusobject_secureconnectionasync(proxyObj, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

    //Wait upto 2 seconds for authentication to complete.
    for (int i = 0; i < 200; ++i) {

        if (authenticationcomplete_service_flag && authenticationcomplete_client_flag) {
            break;
        }
        qcc::Sleep(10);
    }

    EXPECT_TRUE(requestcredentials_service_flag);
    EXPECT_TRUE(authenticationcomplete_service_flag);

    EXPECT_TRUE(requestcredentials_client_flag);
    EXPECT_TRUE(authenticationcomplete_client_flag);

    ResetAuthFlags();
    /*
     * the peer-to-peer connection should have been authenticated with the last
     * call to alljoyn_proxybusobject_secureconnection this call should return
     * ER_OK with out calling any of the authlistener functions.
     */
    status = alljoyn_proxybusobject_secureconnectionasync(proxyObj, QCC_FALSE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

    //We don't expect these values to change from there default values.
    EXPECT_FALSE(requestcredentials_service_flag);
    EXPECT_FALSE(authenticationcomplete_service_flag);

    EXPECT_FALSE(requestcredentials_client_flag);
    EXPECT_FALSE(authenticationcomplete_client_flag);

    ResetAuthFlags();

    /*
     * Although the peer-to-peer connection has already been authenticated we
     * are forcing re-authentication so we expect the authlistener functions to
     * be called again.
     */
    status = alljoyn_proxybusobject_secureconnection(proxyObj, QCC_TRUE);
    EXPECT_EQ(ER_OK, status) << "  Actual Status: " << QCC_StatusText(status);

    //Wait upto 2 seconds for authentication to complete.
    for (int i = 0; i < 200; ++i) {

        if (authenticationcomplete_service_flag && authenticationcomplete_client_flag) {
            break;
        }
        qcc::Sleep(10);
    }

    EXPECT_TRUE(requestcredentials_service_flag);
    EXPECT_TRUE(authenticationcomplete_service_flag);

    EXPECT_TRUE(requestcredentials_client_flag);
    EXPECT_TRUE(authenticationcomplete_client_flag);

    alljoyn_proxybusobject_destroy(proxyObj);

    alljoyn_authlistenerasync_destroy(serviceauthlistener);
    alljoyn_authlistenerasync_destroy(clientauthlistener);
}
