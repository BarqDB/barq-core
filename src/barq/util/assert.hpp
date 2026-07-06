/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef BARQ_UTIL_ASSERT_HPP
#define BARQ_UTIL_ASSERT_HPP

#include <barq/util/features.h>
#include <barq/util/terminate.hpp>

#if BARQ_ENABLE_ASSERTIONS || defined(BARQ_DEBUG)
#define BARQ_ASSERTIONS_ENABLED 1
#else
#define BARQ_ASSERTIONS_ENABLED 0
#endif

#define BARQ_ASSERT_RELEASE(condition)                                                                              \
    (BARQ_LIKELY(condition) ? static_cast<void>(0)                                                                  \
                             : barq::util::terminate("Assertion failed: " #condition, __FILE__, __LINE__))

#if BARQ_ASSERTIONS_ENABLED
#define BARQ_ASSERT(condition) BARQ_ASSERT_RELEASE(condition)
#else
#define BARQ_ASSERT(condition) static_cast<void>(sizeof bool(condition))
#endif

#ifdef BARQ_DEBUG
#define BARQ_ASSERT_DEBUG(condition) BARQ_ASSERT_RELEASE(condition)
#else
#define BARQ_ASSERT_DEBUG(condition) static_cast<void>(sizeof bool(condition))
#endif

#define BARQ_STRINGIFY(X) #X

#define BARQ_ASSERT_RELEASE_EX(condition, ...)                                                                      \
    (BARQ_LIKELY(condition) ? static_cast<void>(0)                                                                  \
                             : barq::util::terminate_with_info("Assertion failed: " #condition, __LINE__, __FILE__, \
                                                                BARQ_STRINGIFY((__VA_ARGS__)), __VA_ARGS__))

#ifdef BARQ_DEBUG
#define BARQ_ASSERT_DEBUG_EX BARQ_ASSERT_RELEASE_EX
#else
#define BARQ_ASSERT_DEBUG_EX(condition, ...) static_cast<void>(sizeof bool(condition))
#endif

// Becase the assert is used in noexcept methods, it's a bad idea to allocate
// buffer space for the message so therefore we must pass it to terminate which
// will 'cerr' it for us without needing any buffer
#if BARQ_ENABLE_ASSERTIONS || defined(BARQ_DEBUG)

#define BARQ_ASSERT_EX BARQ_ASSERT_RELEASE_EX

#define BARQ_ASSERT_3(left, cmp, right)                                                                             \
    (BARQ_LIKELY((left)cmp(right)) ? static_cast<void>(0)                                                           \
                                    : barq::util::terminate("Assertion failed: "                                    \
                                                             "" #left " " #cmp " " #right,                           \
                                                             __FILE__, __LINE__, left, right))

#define BARQ_ASSERT_7(left1, cmp1, right1, logical, left2, cmp2, right2)                                            \
    (BARQ_LIKELY(((left1)cmp1(right1))logical((left2)cmp2(right2)))                                                 \
         ? static_cast<void>(0)                                                                                      \
         : barq::util::terminate("Assertion failed: "                                                               \
                                  "" #left1 " " #cmp1 " " #right1 " " #logical " "                                   \
                                  "" #left2 " " #cmp2 " " #right2,                                                   \
                                  __FILE__, __LINE__, left1, right1, left2, right2))

#define BARQ_ASSERT_11(left1, cmp1, right1, logical1, left2, cmp2, right2, logical2, left3, cmp3, right3)           \
    (BARQ_LIKELY(((left1)cmp1(right1))logical1((left2)cmp2(right2)) logical2((left3)cmp3(right3)))                  \
         ? static_cast<void>(0)                                                                                      \
         : barq::util::terminate("Assertion failed: "                                                               \
                                  "" #left1 " " #cmp1 " " #right1 " " #logical1 " "                                  \
                                  "" #left2 " " #cmp2 " " #right2 " " #logical2 " "                                  \
                                  "" #left3 " " #cmp3 " " #right3,                                                   \
                                  __FILE__, __LINE__, left1, right1, left2, right2, left3, right3))
#else
#define BARQ_ASSERT_EX(condition, ...) static_cast<void>(sizeof bool(condition))
#define BARQ_ASSERT_3(left, cmp, right) static_cast<void>(sizeof bool((left)cmp(right)))
#define BARQ_ASSERT_7(left1, cmp1, right1, logical, left2, cmp2, right2)                                            \
    static_cast<void>(sizeof bool(((left1)cmp1(right1))logical((left2)cmp2(right2))))
#define BARQ_ASSERT_11(left1, cmp1, right1, logical1, left2, cmp2, right2, logical2, left3, cmp3, right3)           \
    static_cast<void>(sizeof bool(((left1)cmp1(right1))logical1((left2)cmp2(right2)) logical2((left3)cmp3(right3))))
#endif

#define BARQ_UNREACHABLE() barq::util::terminate("Unreachable code", __FILE__, __LINE__)
#ifdef BARQ_COVER
#define BARQ_COVER_NEVER(x) false
#define BARQ_COVER_ALWAYS(x) true
#else
#define BARQ_COVER_NEVER(x) (x)
#define BARQ_COVER_ALWAYS(x) (x)
#endif

#endif // BARQ_UTIL_ASSERT_HPP
