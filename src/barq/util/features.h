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

#ifndef BARQ_UTIL_FEATURES_H
#define BARQ_UTIL_FEATURES_H

#ifdef _MSC_VER
#pragma warning(disable : 4800) // Visual Studio int->bool performance warnings
#endif

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#ifndef BARQ_NO_CONFIG
#include <barq/util/config.h>
#endif

/* The maximum number of elements in a B+-tree node. Applies to inner nodes and
 * to leaves. The minimum allowable value is 2.
 */
#ifndef BARQ_MAX_BPNODE_SIZE
#define BARQ_MAX_BPNODE_SIZE 1000
#endif


#define BARQ_QUOTE_2(x) #x
#define BARQ_QUOTE(x) BARQ_QUOTE_2(x)

/* See these links for information about feature check macroes in GCC,
 * Clang, and MSVC:
 *
 * http://gcc.gnu.org/projects/cxx0x.html
 * http://clang.llvm.org/cxx_status.html
 * http://clang.llvm.org/docs/LanguageExtensions.html#checks-for-standard-language-features
 * http://msdn.microsoft.com/en-us/library/vstudio/hh567368.aspx
 * http://sourceforge.net/p/predef/wiki/Compilers
 */


/* Compiler is GCC and version is greater than or equal to the specified version */
#define BARQ_HAVE_AT_LEAST_GCC(maj, min) \
    (__GNUC__ > (maj) || __GNUC__ == (maj) && __GNUC_MINOR__ >= (min))

#if defined(__clang__)
#define BARQ_HAVE_CLANG_FEATURE(feature) __has_feature(feature)
#define BARQ_HAVE_CLANG_WARNING(warning) __has_warning(warning)
#else
#define BARQ_HAVE_CLANG_FEATURE(feature) 0
#define BARQ_HAVE_CLANG_WARNING(warning) 0
#endif

#ifdef __has_cpp_attribute
#define BARQ_HAS_CPP_ATTRIBUTE(attr) __has_cpp_attribute(attr)
#else
#define BARQ_HAS_CPP_ATTRIBUTE(attr) 0
#endif

#if BARQ_HAS_CPP_ATTRIBUTE(clang::fallthrough)
#define BARQ_FALLTHROUGH [[clang::fallthrough]]
#elif BARQ_HAS_CPP_ATTRIBUTE(gnu::fallthrough)
#define BARQ_FALLTHROUGH [[gnu::fallthrough]]
#elif BARQ_HAS_CPP_ATTRIBUTE(fallthrough)
#define BARQ_FALLTHROUGH [[fallthrough]]
#else
#define BARQ_FALLTHROUGH
#endif

// This should be renamed to BARQ_UNREACHABLE as soon as BARQ_UNREACHABLE is renamed to
// BARQ_ASSERT_NOT_REACHED which will better reflect its nature
#if defined(__GNUC__) || defined(__clang__)
#define BARQ_COMPILER_HINT_UNREACHABLE __builtin_unreachable
#else
#define BARQ_COMPILER_HINT_UNREACHABLE abort
#endif

#if defined(__GNUC__) // clang or GCC
#define BARQ_PRAGMA(v) _Pragma(BARQ_QUOTE_2(v))
#elif defined(_MSC_VER) // VS
#define BARQ_PRAGMA(v) __pragma(v)
#else
#define BARQ_PRAGMA(v)
#endif

#if defined(__clang__)
#define BARQ_DIAG(v) BARQ_PRAGMA(clang diagnostic v)
#elif defined(__GNUC__)
#define BARQ_DIAG(v) BARQ_PRAGMA(GCC diagnostic v)
#else
#define BARQ_DIAG(v)
#endif

#define BARQ_DIAG_PUSH() BARQ_DIAG(push)
#define BARQ_DIAG_POP() BARQ_DIAG(pop)

#ifdef _MSC_VER
#define BARQ_VS_WARNING_DISABLE #pragma warning (default: 4297)
#endif

#if BARQ_HAVE_CLANG_WARNING("-Wtautological-compare") || BARQ_HAVE_AT_LEAST_GCC(6, 0)
#define BARQ_DIAG_IGNORE_TAUTOLOGICAL_COMPARE() BARQ_DIAG(ignored "-Wtautological-compare")
#else
#define BARQ_DIAG_IGNORE_TAUTOLOGICAL_COMPARE()
#endif

#ifdef _MSC_VER
#  define BARQ_DIAG_IGNORE_UNSIGNED_MINUS() BARQ_PRAGMA(warning(disable:4146))
#else
#define BARQ_DIAG_IGNORE_UNSIGNED_MINUS()
#endif

/* The way to specify that a function never returns. */
#if BARQ_HAVE_AT_LEAST_GCC(4, 8) || BARQ_HAVE_CLANG_FEATURE(cxx_attributes)
#define BARQ_NORETURN [[noreturn]]
#elif __GNUC__
#define BARQ_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define BARQ_NORETURN __declspec(noreturn)
#else
#define BARQ_NORETURN
#endif


/* The way to specify that a variable or type is intended to possibly
 * not be used. Use it to suppress a warning from the compiler. */
#if __GNUC__
#define BARQ_UNUSED __attribute__((unused))
#else
#define BARQ_UNUSED
#endif

/* The way to specify that a function is deprecated
 * not be used. Use it to suppress a warning from the compiler. */
#if __GNUC__
#define BARQ_DEPRECATED(x) [[deprecated(x)]]
#else
#define BARQ_DEPRECATED(x) __declspec(deprecated(x))
#endif


#if __GNUC__ || defined __INTEL_COMPILER
#define BARQ_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define BARQ_LIKELY(expr) __builtin_expect(!!(expr), 1)
#else
#define BARQ_UNLIKELY(expr) (expr)
#define BARQ_LIKELY(expr) (expr)
#endif


#if defined(__GNUC__) || defined(__HP_aCC)
#define BARQ_FORCEINLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define BARQ_FORCEINLINE __forceinline
#else
#define BARQ_FORCEINLINE inline
#endif


#if BARQ_HAS_CPP_ATTRIBUTE(gnu::cold)
#define BARQ_COLD [[gnu::cold]]
#else
#define BARQ_COLD
#endif


#if BARQ_HAS_CPP_ATTRIBUTE(gnu::noinline)
#define BARQ_NOINLINE [[gnu::noinline]]
#elif defined(__GNUC__) || defined(__HP_aCC)
#define BARQ_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define BARQ_NOINLINE __declspec(noinline)
#else
#define BARQ_NOINLINE
#endif


#if BARQ_HAS_CPP_ATTRIBUTE(nodiscard)
#define BARQ_NODISCARD [[nodiscard]]
#else
#if defined(__GNUC__) || defined(__HP_aCC)
#define BARQ_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define BARQ_NODISCARD _Check_return_
#else
#define BARQ_NODISCARD
#endif
#endif

/* Thread specific data (only for POD types) */
#if defined __clang__
#define BARQ_THREAD_LOCAL __thread
#else
#define BARQ_THREAD_LOCAL thread_local
#endif


#if defined ANDROID || defined __ANDROID_API__
#define BARQ_ANDROID 1
#define BARQ_LINUX 0
#elif defined(__linux__)
#define BARQ_ANDROID 0
#define BARQ_LINUX 1
#else
#define BARQ_ANDROID 0
#define BARQ_LINUX 0
#endif

#if defined _WIN32
#include <winapifamily.h>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM)
#define BARQ_WINDOWS 1
#define BARQ_UWP 0
#elif WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
#define BARQ_WINDOWS 0
#define BARQ_UWP 1
#endif
#else
#define BARQ_WINDOWS 0
#define BARQ_UWP 0
#endif

// Some documentation of the defines provided by Apple:
// http://developer.apple.com/library/mac/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html#//apple_ref/doc/uid/TP40002850-SW13
#if defined __APPLE__ && defined __MACH__
#define BARQ_PLATFORM_APPLE 1
/* Apple OSX and iOS (Darwin). */
#include <Availability.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE == 1 && TARGET_OS_IOS == 1
/* Device (iPhone or iPad) or simulator. */
#define BARQ_IOS 1
#define BARQ_APPLE_DEVICE !TARGET_OS_SIMULATOR
#define BARQ_MACCATALYST TARGET_OS_MACCATALYST
#else
#define BARQ_IOS 0
#define BARQ_MACCATALYST 0
#endif
#if TARGET_OS_WATCH == 1
/* Device (Apple Watch) or simulator. */
#define BARQ_WATCHOS 1
#define BARQ_APPLE_DEVICE !TARGET_OS_SIMULATOR
#else
#define BARQ_WATCHOS 0
#endif
#if TARGET_OS_TV
/* Device (Apple TV) or simulator. */
#define BARQ_TVOS 1
#define BARQ_APPLE_DEVICE !TARGET_OS_SIMULATOR
#else
#define BARQ_TVOS 0
#endif
#else
#define BARQ_PLATFORM_APPLE 0
#define BARQ_MACCATALYST 0
#define BARQ_IOS 0
#define BARQ_WATCHOS 0
#define BARQ_TVOS 0
#endif
#ifndef BARQ_APPLE_DEVICE
#define BARQ_APPLE_DEVICE 0
#endif

#if BARQ_ANDROID || BARQ_IOS || BARQ_WATCHOS || BARQ_TVOS || BARQ_UWP
#define BARQ_MOBILE 1
#else
#define BARQ_MOBILE 0
#endif


#if defined(BARQ_DEBUG) && !defined(BARQ_COOKIE_CHECK)
#define BARQ_COOKIE_CHECK
#endif

// We're in i686 mode
#if defined(__i386) || defined(__i386__) || defined(__i686__) || defined(_M_I86) || defined(_M_IX86)
#define BARQ_ARCHITECTURE_X86_32 1
#else
#define BARQ_ARCHITECTURE_X86_32 0
#endif

// We're in amd64 mode
#if defined(__amd64) || defined(__amd64__) || defined(__x86_64) || defined(__x86_64__) || defined(_M_X64) || \
    defined(_M_AMD64)
#define BARQ_ARCHITECTURE_X86_64 1
#else
#define BARQ_ARCHITECTURE_X86_64 0
#endif

#if defined(__arm__) || defined(_M_ARM)
#define BARQ_ARCHITECTURE_ARM32 1
#else
#define BARQ_ARCHITECTURE_ARM32 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#define BARQ_ARCHITECTURE_ARM64 1
#else
#define BARQ_ARCHITECTURE_ARM64 0
#endif

// Address Sanitizer
#if defined(__has_feature) // Clang
#  if __has_feature(address_sanitizer)
#    define BARQ_SANITIZE_ADDRESS 1
#  else
#    define BARQ_SANITIZE_ADDRESS 0
#  endif
#elif defined(__SANITIZE_ADDRESS__) && __SANITIZE_ADDRESS__ // GCC
#  define BARQ_SANITIZE_ADDRESS 1
#else
#  define BARQ_SANITIZE_ADDRESS 0
#endif

// Thread Sanitizer
#if defined(__has_feature) // Clang
#  if __has_feature(thread_sanitizer)
#    define BARQ_SANITIZE_THREAD 1
#  else
#    define BARQ_SANITIZE_THREAD 0
#  endif
#elif defined(__SANITIZE_THREAD__) && __SANITIZE_THREAD__ // GCC
#  define BARQ_SANITIZE_THREAD 1
#else
#  define BARQ_SANITIZE_THREAD 0
#endif

#endif /* BARQ_UTIL_FEATURES_H */
