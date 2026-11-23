# DSSP — DiffSinger Synthesis Program

DiffSinger Synthesis Program is a C++23 service that provides DiffSinger singing voice synthesis and audio analysis through the DiffScope Synthesis Platform API.

## Features

- DiffSinger singing voice synthesis, phoneme conversion, and duration prediction.
- Pitch and note extraction, audio slicing, and source separation.
- Inference device discovery, with CPU, CUDA, DirectML, or CoreML support depending on the runtime environment.

## Build

Requires a C++23 compiler and CMake 3.19 or later. Dependencies are managed with vcpkg. `scripts/vcpkg-sources.json` names the vcpkg commit and the overlays, the manifest is `scripts/vcpkg-manifest/vcpkg.json`, and the project-specific ports are in `scripts/vcpkg-overrides/ports`.

Install the dependencies with [vcpkg-workspace.cmake](https://github.com/stdware/dev-scripts/blob/main/docs/about-scripts/vcpkg-workspace.md), downloaded into `scripts/`. The script checks out vcpkg and the overlays that `scripts/vcpkg-sources.json` specifies into `deps/`, and installs the dependencies there.

```sh
curl -fsSL -o scripts/vcpkg-workspace.cmake https://raw.githubusercontent.com/stdware/dev-scripts/main/scripts/vcpkg-workspace.cmake
cmake -P scripts/vcpkg-workspace.cmake install
```

Then build with the vcpkg toolchain in `deps/`:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=deps/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
cmake --install build --config Release --prefix installed
```

## Usage

Install compatible model and language packages with dspm into the package directory, then run the following commands from the installation's `bin` directory:

```sh
dssp dump-devices
dssp serve --package-dir /path/to/packages --execution-provider cpu
```

The server listens on `127.0.0.1:13711` by default. Use `--host` and `--port` to change the listening address, or run `dssp serve --help` for more options. Model packages must be obtained separately.

DSSP holds a shared dspm store lock on `<package-dir>/.dspm/lock` from package discovery until all dependent resources are released. Multiple DSSP instances and dspm queries can access the same store concurrently. Installation, replacement, removal, and transaction recovery require exclusive access and remain unavailable while a DSSP instance retains the lock. The package directory must permit creation of `.dspm` and its permanent lock file if they do not exist.

Startup fails if the package store is locked exclusively, has pending transactions or commit markers, or contains an unsupported package database schema. Recovery remains owned by dspm. Run `dspm --packages-dir /path/to/packages list` after stopping programs that retain the store lock, resolve any pending cleanup diagnostics, and then restart DSSP. Package discovery excludes `.dspm` directories.

All loaded packages must be registered in the package store with valid content hashes and matching identities and directories. Synthesis environment tags include the SHA-512 content hashes recorded by dspm for all referenced packages, including imported dependencies. DSSP reads these hashes under the shared store lock and retains the index snapshot without scanning model contents. Replacing an indexed package with different contents changes its environment tag after DSSP restarts.

Logs are written to stderr. `--log-mode normal` (the default) uses the console format with colors where supported. Use `--log-mode structured` for one compact JSON object per line without colors:

```sh
dssp serve --log-mode structured --package-dir /path/to/packages --execution-provider cpu
```

Each JSON record contains `timestamp` (integer milliseconds since the Unix epoch), `timezone_offset` (integer seconds of local time minus UTC, including daylight saving time; UTC+8 is `28800`, UTC−5 is `-18000`), `module` (log category), `level` (uppercase severity), and `message`. Newlines and control characters in messages are JSON-escaped. Both modes support `--log-level`, `--verbose`, and `--quiet`.

See [docs/open-api](docs/open-api) for API specs.

Synthesis and extraction requests support streaming through `Accept: application/x-ndjson`. Streams use HTTP 200 and report task failures through terminal error events. Queue and progress heartbeats repeat every 10 seconds by default. Set `--stream-heartbeat-interval <seconds>` or `DSSP_STREAM_HEARTBEAT_INTERVAL` to configure a positive integer interval. See [task progress](docs/progress.md) for work units and event behavior.

## License

GPL-3.0-or-later

See [COPYING](COPYING).
