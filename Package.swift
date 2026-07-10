// swift-tools-version:5.9
////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 the Barq authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////
//
// SwiftPM manifest for barq-core. Modified fork of the Realm Core Package.swift.
// Builds barq-core (database + object-store + sync client) from source for
// Apple platforms so that barq-swift can depend on the `BarqCore` product.
// On Apple, sync TLS uses the system Security framework, so OpenSSL is not
// required here; the CMake build (used for the prebuilt xcframework and for
// Linux/Windows/Android) acquires OpenSSL/zlib from Barq hosting via
// tools/cmake/AcquireBarqDependency.cmake.

import PackageDescription
import Foundation

let versionStr = "20.1.6"
let versionPieces = versionStr.split(separator: "-")
let versionComponents = versionPieces[0].split(separator: ".")
let versionExtra = versionPieces.count > 1 ? versionPieces[1] : ""

let platforms: [SupportedPlatform] = [
    .macOS(.v10_13),
    .iOS(.v12),
    .tvOS(.v12),
    .watchOS(.v4)
]

var cxxSettings: [CXXSetting] = [
    .headerSearchPath("."),
    .define("BARQ_DEBUG", .when(configuration: .debug)),
    .define("BARQ_NO_CONFIG"),
    .define("BARQ_INSTALL_LIBEXECDIR", to: ""),
    .define("BARQ_ENABLE_ASSERTIONS", to: "1"),
    .define("BARQ_ENABLE_ENCRYPTION", to: "1"),
    .define("BARQ_ENABLE_GEOSPATIAL", to: "1"),
    // Enable the sync client (matches the CMake build's BARQ_ENABLE_SYNC=1);
    // the object-store subscription API is guarded behind this.
    .define("BARQ_ENABLE_SYNC", to: "1"),

    .define("BARQ_VERSION_MAJOR", to: String(versionComponents[0])),
    .define("BARQ_VERSION_MINOR", to: String(versionComponents[1])),
    .define("BARQ_VERSION_PATCH", to: String(versionComponents[2])),
    .define("BARQ_VERSION_EXTRA", to: "\"\(versionExtra)\""),
    .define("BARQ_VERSION_STRING", to: "\"\(versionStr)\""),
]

let bidExcludes: [String] = [
    "bid128_acos.c",
    "bid128_acosh.c",
    "bid128_asin.c",
    "bid128_asinh.c",
    "bid128_atan.c",
    "bid128_atan2.c",
    "bid128_atanh.c",
    "bid128_cbrt.c",
    "bid128_cos.c",
    "bid128_cosh.c",
    "bid128_erf.c",
    "bid128_erfc.c",
    "bid128_exp.c",
    "bid128_exp10.c",
    "bid128_exp2.c",
    "bid128_expm1.c",
    "bid128_fdimd.c",
    "bid128_fmod.c",
    "bid128_frexp.c",
    "bid128_hypot.c",
    "bid128_ldexp.c",
    "bid128_lgamma.c",
    "bid128_llquantexpd.c",
    "bid128_llrintd.c",
    "bid128_llround.c",
    "bid128_log.c",
    "bid128_log10.c",
    "bid128_log1p.c",
    "bid128_log2.c",
    "bid128_logb.c",
    "bid128_logbd.c",
    "bid128_lrintd.c",
    "bid128_lround.c",
    "bid128_minmax.c",
    "bid128_modf.c",
    "bid128_nearbyintd.c",
    "bid128_next.c",
    "bid128_nexttowardd.c",
    "bid128_noncomp.c",
    "bid128_pow.c",
    "bid128_quantexpd.c",
    "bid128_quantumd.c",
    "bid128_rem.c",
    "bid128_round_integral.c",
    "bid128_scalb.c",
    "bid128_scalbl.c",
    "bid128_sin.c",
    "bid128_sinh.c",
    "bid128_sqrt.c",
    "bid128_tan.c",
    "bid128_tanh.c",
    "bid128_tgamma.c",
    "bid128_to_int16.c",
    "bid128_to_int32.c",
    "bid128_to_int8.c",
    "bid128_to_uint16.c",
    "bid128_to_uint32.c",
    "bid128_to_uint64.c",
    "bid128_to_uint8.c",
    "bid32_acos.c",
    "bid32_acosh.c",
    "bid32_add.c",
    "bid32_asin.c",
    "bid32_asinh.c",
    "bid32_atan.c",
    "bid32_atan2.c",
    "bid32_atanh.c",
    "bid32_cbrt.c",
    "bid32_compare.c",
    "bid32_cos.c",
    "bid32_cosh.c",
    "bid32_div.c",
    "bid32_erf.c",
    "bid32_erfc.c",
    "bid32_exp.c",
    "bid32_exp10.c",
    "bid32_exp2.c",
    "bid32_expm1.c",
    "bid32_fdimd.c",
    "bid32_fma.c",
    "bid32_fmod.c",
    "bid32_frexp.c",
    "bid32_hypot.c",
    "bid32_ldexp.c",
    "bid32_lgamma.c",
    "bid32_llquantexpd.c",
    "bid32_llrintd.c",
    "bid32_llround.c",
    "bid32_log.c",
    "bid32_log10.c",
    "bid32_log1p.c",
    "bid32_log2.c",
    "bid32_logb.c",
    "bid32_logbd.c",
    "bid32_lrintd.c",
    "bid32_lround.c",
    "bid32_minmax.c",
    "bid32_modf.c",
    "bid32_mul.c",
    "bid32_nearbyintd.c",
    "bid32_next.c",
    "bid32_nexttowardd.c",
    "bid32_noncomp.c",
    "bid32_pow.c",
    "bid32_quantexpd.c",
    "bid32_quantize.c",
    "bid32_quantumd.c",
    "bid32_rem.c",
    "bid32_round_integral.c",
    "bid32_scalb.c",
    "bid32_scalbl.c",
    "bid32_sin.c",
    "bid32_sinh.c",
    "bid32_sqrt.c",
    "bid32_string.c",
    "bid32_sub.c",
    "bid32_tan.c",
    "bid32_tanh.c",
    "bid32_tgamma.c",
    "bid32_to_bid64.c",
    "bid32_to_int16.c",
    "bid32_to_int32.c",
    "bid32_to_int64.c",
    "bid32_to_int8.c",
    "bid32_to_uint16.c",
    "bid32_to_uint32.c",
    "bid32_to_uint64.c",
    "bid32_to_uint8.c",
    "bid64_acos.c",
    "bid64_acosh.c",
    "bid64_add.c",
    "bid64_asin.c",
    "bid64_asinh.c",
    "bid64_atan.c",
    "bid64_atan2.c",
    "bid64_atanh.c",
    "bid64_cbrt.c",
    "bid64_compare.c",
    "bid64_cos.c",
    "bid64_cosh.c",
    "bid64_div.c",
    "bid64_erf.c",
    "bid64_erfc.c",
    "bid64_exp.c",
    "bid64_exp10.c",
    "bid64_exp2.c",
    "bid64_expm1.c",
    "bid64_fdimd.c",
    "bid64_fma.c",
    "bid64_fmod.c",
    "bid64_frexp.c",
    "bid64_hypot.c",
    "bid64_ldexp.c",
    "bid64_lgamma.c",
    "bid64_llquantexpd.c",
    "bid64_llrintd.c",
    "bid64_llround.c",
    "bid64_log.c",
    "bid64_log10.c",
    "bid64_log1p.c",
    "bid64_log2.c",
    "bid64_logb.c",
    "bid64_logbd.c",
    "bid64_lrintd.c",
    "bid64_lround.c",
    "bid64_minmax.c",
    "bid64_modf.c",
    "bid64_mul.c",
    "bid64_nearbyintd.c",
    "bid64_next.c",
    "bid64_nexttowardd.c",
    "bid64_noncomp.c",
    "bid64_pow.c",
    "bid64_quantexpd.c",
    "bid64_quantize.c",
    "bid64_quantumd.c",
    "bid64_rem.c",
    "bid64_round_integral.c",
    "bid64_scalb.c",
    "bid64_scalbl.c",
    "bid64_sin.c",
    "bid64_sinh.c",
    "bid64_sqrt.c",
    "bid64_string.c",
    "bid64_tan.c",
    "bid64_tanh.c",
    "bid64_tgamma.c",
    "bid64_to_int16.c",
    "bid64_to_int32.c",
    "bid64_to_int64.c",
    "bid64_to_int8.c",
    "bid64_to_uint16.c",
    "bid64_to_uint32.c",
    "bid64_to_uint64.c",
    "bid64_to_uint8.c",
    "bid_dpd.c",
    "bid_feclearexcept.c",
    "bid_fegetexceptflag.c",
    "bid_feraiseexcept.c",
    "bid_fesetexceptflag.c",
    "bid_fetestexcept.c",
    "bid_flag_operations.c",
    "strtod128.c",
    "strtod32.c",
    "strtod64.c",
    "wcstod128.c",
    "wcstod32.c",
    "wcstod64.c",
]

let package = Package(
    name: "BarqDatabase",
    platforms: platforms,
    products: [
        .library(
            name: "BarqCore",
            targets: ["BarqCoreResources"]),
    ],
    targets: [
        .target(
            name: "Bid",
            path: "src/external/IntelRDFPMathLib20U2/LIBRARY/src",
            exclude: bidExcludes,
            publicHeadersPath: "."
        ),
        .target(
            name: "s2geometry",
            path: "src/external/s2",
            exclude: [
                "s2cellunion.cc",
                "s2regioncoverer.cc",
                "s2regionintersection.cc",
                "s2regionunion.cc"
            ],
            publicHeadersPath: ".",
            cxxSettings: ([
                .headerSearchPath(".."),
                .headerSearchPath("../.."),
            ] + cxxSettings) as [CXXSetting]),
        .target(
            name: "BarqCore",
            dependencies: ["Bid", "s2geometry"],
            path: "src",
            exclude: ([
                "CMakeLists.txt",
                "external",
                "barq/CMakeLists.txt",
                "barq/exec",
                "barq/object-store/CMakeLists.txt",
                "barq/object-store/c_api",
                "barq/object-store/impl/epoll",
                "barq/object-store/impl/generic",
                "barq/object-store/impl/windows",
                "barq/object-store/impl/emscripten",
                "barq/object-store/util/emscripten",
                "barq/object-store/sync/impl/emscripten",
                "barq/parser",
                // The sync server is backend-only and links OpenSSL unconditionally;
                // a client SDK never builds it (Apple sync uses SecureTransport).
                "barq/sync/noinst/server",
                "barq/sync/tools",
                "barq/tools",
                "barq/util/config.h.in",
                "barq/version_numbers.hpp.in",
                "spm",
                "win32",
            ]) as [String],
            publicHeadersPath: ".",
            cxxSettings: ([
                .headerSearchPath("external"),
            ] + cxxSettings) as [CXXSetting],
            linkerSettings: [
                .linkedFramework("Foundation", .when(platforms: [.macOS, .iOS, .tvOS, .watchOS, .macCatalyst])),
                .linkedFramework("Security", .when(platforms: [.macOS, .iOS, .tvOS, .watchOS, .macCatalyst])),
                // Changeset compression uses zlib (inflate/deflate/adler32) plus
                // Apple's libcompression (BARQ_USE_LIBCOMPRESSION path).
                .linkedLibrary("z"),
                .linkedLibrary("compression", .when(platforms: [.macOS, .iOS, .tvOS, .watchOS, .macCatalyst])),
            ]),
        // Resources must live in a separate target that depends on BarqCore to
        // work around a swift build force-include of Foundation.h. dummy.mm is
        // required because empty targets fail under xcodebuild.
        .target(
            name: "BarqCoreResources",
            dependencies: ["BarqCore"],
            path: "src/spm",
            resources: [
                .copy("PrivacyInfo.xcprivacy")
            ],
            publicHeadersPath: "."),
    ],
    cxxLanguageStandard: .cxx20
)
