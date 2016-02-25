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

#import <Foundation/Foundation.h>
#include "qcc/String.h"

/**
 AJNConvertUtil class includes a string convert methods.
 */
__deprecated
@interface AJNConvertUtil : NSObject

/**
 Convert NSString to qcc::String.
 @param nsstring String from type NSString.
 @return qcc::String.
 */
+ (qcc ::String)convertNSStringToQCCString:(NSString *)nsstring __deprecated;

/**
 Convert qcc::String to NSString.
 @param qccstring String from type qcc::String.
 @return NSString.
 */
+ (NSString *)convertQCCStringtoNSString:(qcc ::String)qccstring __deprecated;

/**
 Convert NSString to const char.
 @param nsstring String from type NSString.
 @return const char.
 */
+ (const char *)convertNSStringToConstChar:(NSString *)nsstring __deprecated;

/**
 Convert const char to NSString.
 @param constchar const char.
 @return NSString.
 */
+ (NSString *)convertConstCharToNSString:(const char *)constchar __deprecated;
@end
