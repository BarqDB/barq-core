# Barq Core

[![License](https://img.shields.io/github/license/BarqDB/barq-core)](./LICENSE)
![Status](https://img.shields.io/badge/status-alpha-f7c948)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599c)

Barq is a local-first database core with sync support.

This code base keeps the storage engine, object store, query system, and sync protocol foundation, then moves the project toward Barq-owned backend pieces.

## At A Glance

- Native storage engine and object store
- Query parser and execution layer
- Token-based sync client and server foundation
- C API for SDK bindings

## What Is Here

- `src/barq`: storage engine, query engine, object store, sync client, C API, and tools.
- `src/barq/sync/noinst/server`: Barq sync server library and optional server executable.
- `test`: storage, sync, object-store, and C API tests.
- `src/external`: vendored third-party code.

## Build

This project uses CMake and C++17.

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug
```

To build the sync server executable:

```sh
cmake -S . -B cmake-build-debug -DBARQ_BUILD_SERVER=ON
cmake --build cmake-build-debug --target BarqSyncServer
```

The debug server binary is written here:

```sh
cmake-build-debug/src/barq/sync/noinst/server/barq-server-dbg
```

## Run The Sync Server

The server stores synced files under `--root-dir` and verifies client access tokens with the public key passed by `--jwt-public-key`.

```sh
cmake-build-debug/src/barq/sync/noinst/server/barq-server-dbg \
  --root-dir ./barq-sync-data \
  --jwt-public-key ./public.pem \
  --host 127.0.0.1 \
  --port 9090
```

For TLS:

```sh
cmake-build-debug/src/barq/sync/noinst/server/barq-server-dbg \
  --root-dir ./barq-sync-data \
  --jwt-public-key ./public.pem \
  --tls-cert ./cert.pem \
  --tls-key ./key.pem
```

For local testing only, unsigned tokens can be allowed:

```sh
cmake-build-debug/src/barq/sync/noinst/server/barq-server-dbg \
  --root-dir ./barq-sync-data \
  --allow-unsigned-tokens
```

Production clients should use signed JWT access tokens. Token signing belongs in the application auth service, not in this core library.

## Point A Client At The Server

The client route is configurable.

```c
barq_sync_manager_set_route(manager, "wss://your-server/barq-sync", true);
```

User token callbacks should fetch signed JWTs from your auth service. Partition values are plain strings:

```c
barq_sync_config_t* config = barq_sync_config_new(user, "partition");
```

## Run Tests

Build and run the sync tests:

```sh
cmake --build cmake-build-debug --target barq-sync-tests
cmake-build-debug/test/barq-sync-tests.app/Contents/MacOS/barq-sync-tests
```

Build and run the object-store tests:

```sh
cmake --build cmake-build-debug --target barq-object-store-tests
cmake-build-debug/test/object-store/barq-object-store-tests.app/Contents/MacOS/barq-object-store-tests
```

## Roadmap

- Stabilize the sync server surface
- Keep the C API small and SDK-friendly
- Improve release notes for native artifacts
- Add more end-to-end sync coverage

## License And Attribution

Barq is a modified fork of [Realm Core](https://github.com/realm/realm-core).
The source files have been changed from their original form as part of the Barq
project; see the Git history for details. The original Realm Inc. copyright
headers are retained.

The code is licensed under the Apache License 2.0. See `LICENSE` and `NOTICE`.

Barq is an independent project and is not affiliated with, sponsored by, or
endorsed by Realm, MongoDB, Inc., or MongoDB Atlas. "Realm", "MongoDB", and
"MongoDB Atlas" are trademarks of MongoDB, Inc.

Third-party notices are kept in `THIRD-PARTY-NOTICES`.
