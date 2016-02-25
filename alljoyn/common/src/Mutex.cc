/**
 * @file
 *
 * Define a class that abstracts mutexes.
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
#include <assert.h>
#include <qcc/platform.h>
#include <qcc/Mutex.h>
#include <qcc/Debug.h>

/** @internal */
#define QCC_MODULE "MUTEX"

using namespace qcc;

Mutex::Mutex() : isInitialized(false)
{
    Init();
}

Mutex::Mutex(const Mutex& other) : isInitialized(false)
{
    QCC_UNUSED(other);
    Init();
}

Mutex::~Mutex()
{
    Destroy();
}

Mutex& Mutex::operator=(const Mutex& other)
{
    QCC_UNUSED(other);
    Destroy();
    Init();
    return *this;
}

QStatus Mutex::Lock(const char* file, uint32_t line)
{
#ifdef NDEBUG
    QCC_UNUSED(file);
    QCC_UNUSED(line);
    return Lock();
#else
    assert(isInitialized);
    QStatus status;
    if (TryLock()) {
        status = ER_OK;
    } else {
        status = Lock();
    }
    if (status == ER_OK) {
        QCC_DbgPrintf(("Lock Acquired %s:%d", file, line));
        this->file = file;
        this->line = line;
    } else {
        QCC_LogError(status, ("Mutex::Lock %s:%d failed", file, line));
    }
    return status;
#endif
}

QStatus Mutex::Unlock(const char* file, uint32_t line)
{
#ifdef NDEBUG
    QCC_UNUSED(file);
    QCC_UNUSED(line);
    return Unlock();
#else
    assert(isInitialized);
    QCC_DbgPrintf(("Lock Released: %s:%d (acquired at %s:%u)", file, line, this->file, this->line));
    this->file = NULL;
    this->line = static_cast<uint32_t>(-1);
    return Unlock();
#endif
}

