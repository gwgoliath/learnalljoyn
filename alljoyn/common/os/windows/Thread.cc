/**
 * @file
 *
 * Define a class that abstracts Windows process/threads.
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

#include <algorithm>
#include <assert.h>
#include <process.h>
#include <map>

#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/Debug.h>
#include <qcc/Thread.h>
#include <qcc/Mutex.h>

#include <Status.h>

using namespace std;

/** @internal */
#define QCC_MODULE "THREAD"

namespace qcc {

#ifndef NDEBUG
static volatile int32_t started = 0;
static volatile int32_t running = 0;
static volatile int32_t stopped = 0;
#endif

/** Maximum number of milliseconds to wait between calls to select to check for thread death */
static const uint32_t MAX_SELECT_WAIT_MS = 10000;

/** Thread list */
Mutex* Thread::threadListLock = NULL;
map<ThreadId, Thread*>* Thread::threadList = NULL;

static DWORD cleanExternalThreadKey;
static bool initialized = false;

void Thread::CleanExternalThread(void* t)
{
    if (!t) {
        return;
    }

    Thread* thread = reinterpret_cast<Thread*>(t);
    threadListLock->Lock();
    map<ThreadId, Thread*>::iterator it = threadList->find(thread->threadId);
    if (it != threadList->end()) {
        if (it->second->isExternal) {
            delete it->second;
            threadList->erase(it);
        }
    }
    threadListLock->Unlock();
}

QStatus Thread::Init()
{
    if (!initialized) {
        Thread::threadListLock = new Mutex();
        Thread::threadList = new map<ThreadId, Thread*>();
        cleanExternalThreadKey = FlsAlloc(Thread::CleanExternalThread);
        if (cleanExternalThreadKey == FLS_OUT_OF_INDEXES) {
            QCC_LogError(ER_OS_ERROR, ("Creating TLS key: %d", GetLastError()));
            delete threadList;
            delete threadListLock;
            return ER_OS_ERROR;
        }
        initialized = true;
    }
    return ER_OK;
}

QStatus Thread::Shutdown()
{
    if (initialized) {
        // Note that FlsFree will call the callback function for all
        // fibers with a valid key in the Fls slot.
        FlsFree(cleanExternalThreadKey);
        delete Thread::threadList;
        delete Thread::threadListLock;
        initialized = false;
    }
    return ER_OK;
}

QStatus Sleep(uint32_t ms) {
    ::sleep(ms);
    return ER_OK;
}

Thread* Thread::GetThread()
{
    Thread* ret = NULL;
    ThreadId id = GetCurrentThreadId();

    /* Find thread on threadList */
    threadListLock->Lock();
    map<ThreadId, Thread*>::const_iterator iter = threadList->find(id);
    if (iter != threadList->end()) {
        ret = iter->second;
    }
    threadListLock->Unlock();
    /*
     * If the current thread isn't on the list, then create an external (wrapper) thread
     */
    if (NULL == ret) {
        char name[32];
        snprintf(name, sizeof(name), "external%d", id);
        ret = new Thread(name, NULL, true);
    }

    return ret;
}

const char* Thread::GetThreadName()
{
    Thread* thread = NULL;
    ThreadId id = GetCurrentThreadId();

    /* Find thread on threadList */
    threadListLock->Lock();
    map<ThreadId, Thread*>::const_iterator iter = threadList->find(id);
    if (iter != threadList->end()) {
        thread = iter->second;
    }
    threadListLock->Unlock();
    /*
     * If the current thread isn't on the list, then don't create an external (wrapper) thread
     */
    if (thread == NULL) {
        return "external";
    }

    return thread->GetName();
}

void Thread::CleanExternalThreads()
{
    threadListLock->Lock();
    map<ThreadId, Thread*>::iterator it = threadList->begin();
    while (it != threadList->end()) {
        if (it->second->isExternal) {
            delete it->second;
            threadList->erase(it++);
        } else {
            ++it;
        }
    }
    threadListLock->Unlock();
}

Thread::Thread(qcc::String name, Thread::ThreadFunction func, bool isExternal) :
    state(isExternal ? RUNNING : DEAD),
    isStopping(false),
    function(isExternal ? NULL : func),
    handle(isExternal ? GetCurrentThread() : 0),
    exitValue(NULL),
    threadArg(NULL),
    threadListener(NULL),
    isExternal(isExternal),
    platformContext(NULL),
    alertCode(0),
    auxListeners(),
    auxListenersLock(),
    threadId(isExternal ? GetCurrentThreadId() : 0)
{
    /* qcc::String is not thread safe.  Don't use it here. */
    funcName[0] = '\0';
    strncpy(funcName, name.c_str(), sizeof(funcName));
    funcName[sizeof(funcName) - 1] = '\0';

    /*
     * External threads are already running so just add them to the thread list.
     */
    if (isExternal) {
        threadListLock->Lock();
        (*threadList)[threadId] = this;
        if (FlsGetValue(cleanExternalThreadKey) == NULL) {
            BOOL ret = FlsSetValue(cleanExternalThreadKey, this);
            if (ret == 0) {
                QCC_LogError(ER_OS_ERROR, ("Setting TLS key: %s", GetLastError()));
            }
            assert(ret != 0);
        }
        threadListLock->Unlock();
    }
    QCC_DbgHLPrintf(("Thread::Thread() [%s,%x]", funcName, this));
}

Thread::~Thread(void)
{
    if (!isExternal) {
        if (IsRunning()) {
            Stop();
            Join();
        } else if (handle) {
            CloseHandle(handle);
            handle = 0;
            QCC_DEBUG_ONLY(IncrementAndFetch(&stopped));
        }
    }
    QCC_DbgHLPrintf(("Thread::~Thread() [%s,%x] started:%d running:%d stopped:%d", GetName(), this, started, running, stopped));
}


ThreadInternalReturn STDCALL Thread::RunInternal(void* arg)
{
    Thread* thread(reinterpret_cast<Thread*>(arg));

    assert(thread != NULL);
    assert(thread->state == STARTED);
    assert(!thread->isExternal);

    QCC_DEBUG_ONLY(IncrementAndFetch(&started));

    /* Add this Thread to list of running threads */
    threadListLock->Lock();
    (*threadList)[thread->threadId] = thread;
    thread->state = RUNNING;
    threadListLock->Unlock();

    /* Start the thread if it hasn't been stopped */
    if (!thread->isStopping) {
        QCC_DbgPrintf(("Starting thread: %s", thread->funcName));
        QCC_DEBUG_ONLY(IncrementAndFetch(&running));
        thread->exitValue = thread->Run(thread->threadArg);
        QCC_DEBUG_ONLY(DecrementAndFetch(&running));
        QCC_DbgPrintf(("Thread function exited: %s --> %p", thread->funcName, thread->exitValue));
    }

    unsigned retVal = PtrToUlong(thread->exitValue);
    uint32_t threadId = thread->threadId;

    thread->state = STOPPING;
    thread->stopEvent.ResetEvent();

    /*
     * The following block must be in its own scope because microsoft STL's ITERATOR_DEBUG_LEVEL==2
     * falsely concludes that the iterator defined below (without its own scope) is still in scope
     * when auxListener's destructor runs from within ~Thread. Go Microsoft.
     */
    {
        /* Call aux listeners before main listener since main listner may delete the thread */
        thread->auxListenersLock.Lock();

        ThreadListeners::iterator it = thread->auxListeners.begin();
        while (it != thread->auxListeners.end()) {
            ThreadListener* listener = *it;
            listener->ThreadExit(thread);
            it = thread->auxListeners.upper_bound(listener);
        }
        thread->auxListenersLock.Unlock();
    }

    /*
     * Call thread exit callback if specified. Note that ThreadExit may dellocate the thread so the
     * members of thread may not be accessed after this call
     */
    if (thread->threadListener) {
        thread->threadListener->ThreadExit(thread);
    }

    /* This also means no QCC_DbgPrintf as they try to get context on the current thread */

    /* Remove this Thread from list of running threads */
    threadListLock->Lock();
    threadList->erase(threadId);
    threadListLock->Unlock();

    _endthreadex(retVal);
    return retVal;
}

/* Inherit stack reserve and initial commit size from the host EXE's image file header */
static const uint32_t stacksize = 0;

QStatus Thread::Start(void* arg, ThreadListener* listener)
{
    QStatus status = ER_OK;

    /* Check that thread can be started */
    if (isExternal) {
        status = ER_EXTERNAL_THREAD;
    } else if (isStopping) {
        status = ER_THREAD_STOPPING;
    } else if (IsRunning()) {
        status = ER_THREAD_RUNNING;
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("Thread::Start() [%s]", funcName));
    } else {
        QCC_DbgTrace(("Thread::Start() [%s]", funcName));
        /*  Reset the stop event so the thread doesn't start out alerted. */
        stopEvent.ResetEvent();
        /* Create OS thread */
        this->threadArg = arg;
        this->threadListener = listener;

        state = STARTED;
        handle = reinterpret_cast<HANDLE>(_beginthreadex(NULL, stacksize, RunInternal, this, CREATE_SUSPENDED, &threadId));
        if (handle != 0) {
            if (ResumeThread(handle) == (DWORD)-1) {
                QCC_LogError(ER_OS_ERROR, ("Resuming thread: %d", GetLastError()));
                CloseHandle(handle);
                handle = 0;
            }
        }
        if (handle == 0) {
            state = DEAD;
            isStopping = false;
            status = ER_OS_ERROR;
            QCC_LogError(status, ("Creating thread"));
        }
    }
    return status;
}


QStatus Thread::Stop(void)
{
    /* Cannot stop external threads */
    if (isExternal) {
        QCC_LogError(ER_EXTERNAL_THREAD, ("Cannot stop an external thread"));
        return ER_EXTERNAL_THREAD;
    } else if ((state == DEAD) || (state == INITIAL)) {
        QCC_DbgPrintf(("Thread::Stop() thread is dead [%s]", funcName));
        return ER_OK;
    } else {
        QCC_DbgTrace(("Thread::Stop() %x [%s]", handle, funcName));
        isStopping = true;
        return stopEvent.SetEvent();
    }
}

QStatus Thread::Alert()
{
    if (state == DEAD) {
        return ER_DEAD_THREAD;
    }
    QCC_DbgTrace(("Thread::Alert() [%s:%srunning]", funcName, IsRunning() ? " " : " not "));
    return stopEvent.SetEvent();
}

QStatus Thread::Alert(uint32_t threadAlertCode)
{
    this->alertCode = threadAlertCode;
    if (state == DEAD) {
        return ER_DEAD_THREAD;
    }
    QCC_DbgTrace(("Thread::Alert() [%s run: %s]", funcName, IsRunning() ? "true" : "false"));
    return stopEvent.SetEvent();
}

QStatus Thread::Join(void)
{

    assert(!isExternal);

    QStatus status = ER_OK;
    bool self = (threadId == GetCurrentThreadId());

    QCC_DbgTrace(("Thread::Join() [%s run: %s]", funcName, IsRunning() ? "true" : "false"));

    /*
     * Nothing to join if the thread is dead
     */
    if (state == DEAD) {
        QCC_DbgPrintf(("Thread::Join() thread is dead [%s]", funcName));
        isStopping = false;
        return ER_OK;
    }

    QCC_DbgPrintf(("[%s - %x] %s thread %x [%s - %x]",
                   self ? funcName : GetThread()->funcName,
                   self ? threadId : GetThread()->threadId,
                   self ? "Closing" : "Joining",
                   threadId, funcName, threadId));
    /*
     * make local copy of handle so it is not deleted if two threads are in
     * Join at the same time.
     */
    HANDLE goner = handle;
    if (goner) {
        DWORD ret;
        handle = 0;
        if (self) {
            ret = WAIT_OBJECT_0;
        } else {
            ret = WaitForSingleObject(goner, INFINITE);
        }
        if (ret != WAIT_OBJECT_0) {
            status = ER_OS_ERROR;
            QCC_LogError(status, ("Joining thread: %d", ret));
        }
        CloseHandle(goner);
        QCC_DEBUG_ONLY(IncrementAndFetch(&stopped));
    }
    QCC_DbgPrintf(("%s thread %s", self ? "Closed" : "Joined", funcName));
    isStopping = false;
    /* once the state is changed to DEAD, we must not touch any member of this class anymore */
    state = DEAD;
    return status;
}

void Thread::AddAuxListener(ThreadListener* listener)
{
    auxListenersLock.Lock();
    auxListeners.insert(listener);
    auxListenersLock.Unlock();
}

void Thread::RemoveAuxListener(ThreadListener* listener)
{
    auxListenersLock.Lock();
    ThreadListeners::iterator it = auxListeners.find(listener);
    if (it != auxListeners.end()) {
        auxListeners.erase(it);
    }
    auxListenersLock.Unlock();
}

ThreadReturn STDCALL Thread::Run(void* arg)
{
    assert(NULL != function);
    return (*function)(arg);
}

}    /* namespace */
