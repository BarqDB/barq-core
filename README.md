# Barq Core

[![License](https://img.shields.io/github/license/BarqDB/barq-core)](./LICENSE)
![Status](https://img.shields.io/badge/status-alpha-f7c948)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599c)

The native core of [BarqDB](https://github.com/BarqDB) — a local-first database with real-time sync. It holds the storage engine, object store, query system, C API, and sync foundation shared by every Barq SDK.

## Use an SDK

Barq Core is a building block, not an end-user product — it has no publicly stable API. To build an app, use one of the SDKs, which embed this core:

| SDK | Platform |
| --- | --- |
| [barq-js](https://github.com/BarqDB/barq-js) | JavaScript, React Native |
| [barq-kotlin](https://github.com/BarqDB/barq-kotlin) | Kotlin Multiplatform — Android, iOS, JVM |
| [barq-swift](https://github.com/BarqDB/barq-swift) | Swift, Objective-C |
| [barq-native](https://github.com/BarqDB/barq-native) | C++ |

## What's inside

| Path | Contents |
| --- | --- |
| `src/barq` | storage engine, query engine, object store, sync client, C API, tools |
| `src/barq/sync/noinst/server` | sync server library and optional server executable |
| `src/external` | vendored third-party code |
| `test` | storage, sync, object-store, and C API tests |

## Build

Barq Core builds with CMake and C++17.

```sh
cmake -S . -B build
cmake --build build
```

## Sync server

Build the optional server executable:

```sh
cmake -S . -B build -DBARQ_BUILD_SERVER=ON
cmake --build build --target BarqSyncServer
```

Run it with a data directory and the public key it uses to verify client JWTs (the binary is under `build/src/barq/sync/noinst/server/`; debug builds append `-dbg`):

```sh
barq-server \
  --root-dir ./barq-sync-data \
  --jwt-public-key ./public.pem \
  --host 127.0.0.1 --port 9090
```

Add `--tls-cert` and `--tls-key` for TLS. `--allow-unsigned-tokens` skips signature checks for local testing only — production clients should send signed JWTs, and token signing belongs in your auth service, not in this library.

Point a client at the server through its configurable route:

```c
barq_sync_manager_set_route(manager, "wss://your-server/barq-sync", true);
barq_sync_config_t* config = barq_sync_config_new(user, "partition");
```

## Test

```sh
cmake --build build --target SyncTests
cmake --build build --target ObjectStoreTests
```

The test binaries (`barq-sync-tests`, `barq-object-store-tests`) are written under `build/test/`.

## License and attribution

Barq Core is a modified fork of [Realm Core](https://github.com/realm/realm-core), licensed under the [Apache License 2.0](./LICENSE). The original Realm Inc. copyright headers are retained in every file they came from; see the Git history for what changed. Third-party notices are listed in [`THIRD-PARTY-NOTICES`](./THIRD-PARTY-NOTICES).

Barq is an independent project. It is not affiliated with, sponsored by, or endorsed by Realm, MongoDB, Inc., or MongoDB Atlas. "Realm", "MongoDB", and "MongoDB Atlas" are trademarks of MongoDB, Inc.
