# Repository Guidelines

## Project Architecture and Layout

DSSP (DiffSinger Synthesis Program) is a C++23 command-line service for singing voice synthesis and audio analysis through the DiffScope Synthesis Platform HTTP API. Keep application startup and service composition in `src/app`. Reusable behavior belongs in the corresponding application library.

- `src/app/`: executable entry point, logging, architecture and inference composition, and CLI commands under `cli/`.
- `src/libs/application/error/`: diagnostic error types, causal chains, and typed error sets.
- `src/libs/application/codecutil/`: shared encoding and conversion utilities.
- `src/libs/application/resource/`: worker pools, task execution, coroutine adapters, execution gates, and cache primitives.
- `src/libs/application/runtime/`: server configuration, execution-provider and device discovery, and inference-cache configuration.
- `src/libs/application/localization/`: ICU resource catalogs and request-specific locale selection.
- `src/libs/application/media/`: MIME detection, resource loading, and WAV handling.
- `src/libs/application/inference/`: the shared SynthRT environment, package ownership, inference pools, resource identity, language routing, package media paths, and native-call adaptation.
- `src/libs/application/extraction/`: audio-analysis services, including note and pitch extraction, slicing, tempo analysis, and source separation.
- `src/libs/application/arch/common/`: architecture-neutral synthesis contracts, types, and catalogs. `arch/diffsinger/` implements the DiffSinger backend.
- `src/libs/application/http/`: reusable HTTP payload, response, media-query, access-log, and request-lifetime infrastructure. `api/v1/` owns version-specific DTOs, codecs, controllers, and domain-to-HTTP mappings.
- `src/libs/3rdparty/`: repository-managed SynthRT extensions `srtsepinfer` and `srtbeatinfer`, including their shared APIs, DSP code, and inference-interpreter plugins. These directories are not Git submodules. Preserve their separate API, namespace, build, and license boundaries.
- `src/tests/`: Google Test suites, fixtures, and test translation resources. `docs/open-api/1.0.json` describes the HTTP API surface represented by this service.
- `src/conf.cmake` and `cmake/`: build layout, plugin deployment, dependency deployment, and runtime checks.
- `scripts/vcpkg-manifest/`: dependency manifest. `scripts/vcpkg-overrides/ports/` holds the project-specific pinned ports and `scripts/vcpkg-overrides/triplets/` the project triplets. `scripts/vcpkg-sources.json` names the vcpkg commit and the overlays, and `vcpkg-workspace.cmake` from https://github.com/stdware/dev-scripts prepares the dependency tree under `deps/`. There is no submodule.
- `scripts/ci/` and `.github/`: packaging scripts and CI workflows. Inspect the current scripts before relying on their parameters or behavior.

Never commit generated files from `build/`, `cmake-build-*`, `out/`, synchronized include trees, compiled ICU resources, or local dependency installations. Keep local model packages, audio, and other development data out of the diff.

## Module and Dependency Boundaries

Extend the closest existing library before creating a new one. Follow the neighboring target's CMake structure, use a `dssp_<module>` target and a complete flatcase `dssp<module>` include prefix, and add the module to the owning `CMakeLists.txt`. Declare dependencies on the target that uses them. Use `PUBLIC` for dependencies required by published headers and `PRIVATE` for implementation-only dependencies.

Keep architecture-neutral contracts in `dssp_arch`. They must not depend on HTTP controllers, version-specific DTOs, or a concrete synthesis backend. Keep DiffSinger package interpretation and synthesis implementation in `dssp_arch_diffsinger`. The application composition root registers compiled architectures through their factories and constructs the catalog. Adding a backend requires updating its CMake target, application linkage, factory registration, and required plugin search paths together.

Construct shared inference and media services at the application composition root and pass them through `arch::ArchitectureServices` or the existing service constructors. Architecture factories must not create a separate SynthRT unit/driver or global registration mechanism. Keep the inference host usable without linking a concrete architecture backend.

HTTP controllers decode and validate requests, select a service or architecture, map domain results, and send responses. Put synthesis and analysis behavior in the backend or service. Perform version-specific DTO conversions and HTTP error/status mappings in `api/v1`, rather than adding HTTP types to synthesis contracts.

## Existing Infrastructure

Do not build parallel task, inference, error, localization, media, or configuration frameworks.

- Use `resource::TaskExecutor` for ordinary background work, `WorkerPool` and `ExecutionGate` for their existing scheduling contracts, and `resource::awaitCallback` for callback-to-coroutine adaptation. The adapter does not select a worker thread.
- Use `inference::Environment` for SynthRT ownership and package discovery, `InferencePool` for executive creation, caching, and scheduled execution, and the existing resource-identity and parallelism policies. Do not bypass resource serialization or introduce independent caches for the same executives.
- Use `arch::Architecture`, `ArchitectureCatalog`, and the existing factories for synthesis dispatch. Use `extraction::Service` and its catalogs for analysis dispatch.
- Use `Error`, `ErrorType`, `ErrorSet`, and `std::expected` for application failures. Use the existing HTTP request codecs, response helpers, and domain-error mappings at the API boundary.
- Use `media::Resources`, the existing URI/path helpers, and WAV utilities for media handling. Keep native filesystem paths and protocol strings distinct and convert at their boundaries.
- Use `runtime::resolveServerConfig` for option/environment/default resolution, `DeviceInfo` for device selection, and the existing deployment helpers for runtime libraries, plugins, and data.
- Use `stdc::LogCategory` and the application logger for diagnostics. Preserve request correlation through the access-log and request-context APIs.

## Data Compatibility and Versioning

Unless explicitly requested, do not account for compatibility with existing persisted data or interchange data formats. When changing such data or formats, do not update their header metadata or version numbers.

This rule does not determine HTTP API compatibility. Confirm the required compatibility behavior with the user before implementing a breaking HTTP API change. Keep the OpenAPI document, DTOs, codecs, controllers, mappings, and affected examples consistent with the requested behavior. Do not treat the data-compatibility rule as permission to violate the package or runtime contracts of installed dependencies.

## Feature Replacement Hygiene

When asked to modify a feature, treat the requested result as the only current behavior. Unless the user explicitly requests otherwise, do not leave any remnants of the old behavior in code, comments, or documentation. Remove obsolete implementations, branches, APIs, settings, names, examples, compatibility paths, migration notes, comparisons, historical context, and explanations of why the feature was changed.

## Header Publication and Include Paths

Application libraries publish flattened headers through `dssp_sync_include`, backed by the qmsetup build helpers. Include the published path rather than `src/...` or a relative path across directories. Use the prefix declared by the target, not a prefix inferred from its source directory or target spelling.

| Source example | Consumer include |
| --- | --- |
| `src/libs/application/arch/common/Architecture.h` | `<dssparch/Architecture.h>` |
| `src/libs/application/arch/diffsinger/Synthesis.h` | `<dssparchdiffsinger/Synthesis.h>` |
| `src/libs/application/api/v1/dto/Requests.h` | `<dsspapiv1/Requests.h>` |
| `src/libs/application/api/v1/codec/RequestCodec.h` | `<dsspapiv1/RequestCodec.h>` |
| `src/libs/application/resource/TaskExecutor.h` | `<dsspresource/TaskExecutor.h>` |
| `src/app/ArchComposition.h` | `<dsspapp/ArchComposition.h>` |
| `src/app/cli/Serve.h` | `<dsspcli/Serve.h>` |
| `src/tests/TestFiles.h` | `<dssptests/TestFiles.h>` |

Header basenames must be unique within the target's publication class because directory nesting is flattened. A published path does not make an implementation detail a supported cross-module API. Test headers are build-only. The SynthRT extensions under `src/libs/3rdparty` publish their own include trees, such as `<srtbeatinfer/Api/Beat/1/BeatApiL1.h>`. Preserve those trees rather than applying DSSP's flattened layout to them.

## Include Format and Ordering

In every header, use angle brackets for all includes, including first-party headers. In a `.cpp` or `.mm`, the matching header and, when present, its private `_p.h` partner are the only quoted header includes. Write them as plain relative filenames, public before private. In a `Class_p.h`, the corresponding public header is the first include, alone in its own group. All other headers, including same-directory siblings, use their published angle-bracket paths.

Platform-specific implementations sharing a class header treat that shared header as their matching header. Generated headers without a published include path may remain quoted. Preserve required preprocessor wrappers around dependency headers, such as Windows macro isolation.

Separate include groups with one blank line and follow `.clang-format`'s case-sensitive ordering:

1. Matching headers in implementation files, or the corresponding public header in a `_p.h`.
2. Platform and C library headers, followed by C++ standard library headers.
3. Boost headers.
4. Other third-party library headers, such as Drogon, ICU, stdcorelib, and SynthRT.
5. DSSP headers under the configured `dssp*` prefixes.

```cpp
#include "RequestCodec.h"

#include <expected>
#include <string>

#include <drogon/HttpRequest.h>

#include <dsspapiv1/Requests.h>
#include <dssphttp/JsonPayload.h>
```

Include what the file uses and prefer forward declarations in headers when a complete type is unnecessary.

## C++ Style and Naming

`.clang-format` is authoritative: four spaces, no tabs, attached braces, right-aligned pointers/references, indented namespace contents, LF endings, and no column limit. Automatic formatting is subject to the explicit-request restriction in Configure, Build, and Run.

- Use `PascalCase.h/.cpp` matching the primary type. Preserve established lowercase utility or module-global filenames such as `codecutil.h`.
- Use `PascalCase` types and `lowerCamelCase` functions, parameters, and locals. Use `m_lowerCamelCase` for direct private/protected data members. Members inside a private implementation class have no `m_` prefix. Public DTO/aggregate fields retain their domain or protocol naming.
- Apply these naming rules to new code and storage declarations being substantively changed. Do not rename unrelated existing members or perform repository-wide style cleanup as part of a feature change.
- Match established namespace and acronym spelling, such as `dssp::arch_diffsinger`, `dssp::api_v1`, `srt`, `ICU`, and `ONNX`. Preserve external API names required by dependency contracts.
- Use `#ifndef` guards such as `DSSP_RESOURCE_TASKEXECUTOR_H`. Do not introduce `#pragma once`.
- Use the existing target export macro for public symbols crossing a shared-library boundary, including `SRTSEPINFER_EXPORT` and `SRTBEATINFER_EXPORT` in the corresponding extension. Do not introduce export macros merely for the statically linked application libraries.
- Make single-argument constructors `explicit`, mark overrides, initialize pointers, and express ownership through RAII. Use `std::unique_ptr` for exclusive ownership and `std::shared_ptr` when asynchronous work or shared infrastructure requires shared lifetime.
- Prefer C++23 facilities, including ranges and `<=>`, when they make code clearer. Do not use exceptions for normal control flow. Catch at APIs that can throw, translate the failure there, and preserve useful diagnostics.

## Prose Style

These rules apply to comments, documentation, README files, help text, and diagnostic messages.

- **Use formal technical writing.** Comments and documentation are normative text, not narration. Each sentence states a fact, a constraint, or a reason, without preamble, impressions, or rhetoric.
- **Do not personify.** Code, files, formats, programs, and tests are not subjects with a will. Do not write says, tells, knows, asks, wants, means, cares, decides, promises, or is told. Use returns, indicates, records, specifies, reports, detects, requires, or rejects instead. Real actors such as the user, the author, or the caller may be subjects.
- **Use terms, not descriptive paraphrases.** Write invalid byte sequence, not bytes that do not decode. Write unpaired surrogate, reverse mapping, and unrepresentable character, not half of one, the way back, and what cannot be spelled. If no common term exists, define one at its first use and keep that name throughout.
- **Headings, group names, and list labels are nouns or noun phrases.** Write Motivation, Behavior, and Rationale, not Why, What it does, and How it works. If a noun works, do not use a what-clause as a subject or object. Write the requested encoding, not what the user asked for.
- **Use if for conditions and where only for places.** Write empty if absent, not empty where there is none. Do not use one to refer back to a noun (such a one, the one it wants). Repeat the noun.
- **Avoid colloquial phrases** such as whatever else, for good, as it stands, on its own, on the way out, at a glance, there and back, is given up on, the rest of why, and and all. Use precise written expressions instead.
- **Write complete sentences.** Do not write fragments, comma splices (two independent clauses joined only by a comma), or rhetorical questions. Do not start a causal statement with So, And so, Which is why, That is why, or Hence. State the cause in the same sentence with because or therefore. Do not address the reader in the second person.
- **Do not use dashes, and do not join clauses with semicolons.** Start a new sentence wherever a sentence break belongs. Use American spelling.
- **Function documentation starts with a verb** (Returns, Decodes, Reads, Rejects). State the return value for each case. Boolean queries read Returns whether ….
- **Do not write a paragraph for what a few words can say.** Comments explain constraints, ownership, lifetimes, and the reason the implementation must take its current form. Do not restate what the signature already shows.
- **Do not write archaeological comments.** Keep the reason the code must be this way now. Remove how it used to be and how it was fixed.
- The same rules apply to Chinese text. Use written language, not colloquial words such as 「别」「搞」「就行」「得（表必须）」「啥」「拿来」「反正」「其实」「说白了」「这玩意儿」. Headings use 「动机」「设计理由」「实现方式」, not 「为什么」「怎么做」. Normative imperatives such as 「不要」「必须」 are not colloquial and remain in use.

## Class Design and Ownership

Use ordinary C++ ownership and lifecycle contracts. For classes that need implementation hiding, follow the existing `Impl` pattern with `std::unique_ptr<Impl>` and an out-of-line destructor. Keep small data/value types and implementation-local classes direct unless there is a concrete reason to hide their representation. Do not impose PImpl on every application class.

Retain owning package handles, runtime options, and service owners for as long as dependent work can access them. Do not retain borrowed package specifications beyond their SynthRT environment's lifetime. Avoid global mutable services and singletons when the composition root can pass an explicit dependency.

## Error Handling and HTTP Contracts

Use `std::exception` as the common exception interface and the project's `Error` hierarchy as the common application error representation. Catch standard exceptions and derived exceptions by `const std::exception &`. Do not introduce a parallel application error-handling mechanism.

Represent expected application failures with `std::expected<T, E>` and the module's declared `Error`-based error type or error set. `Error` contains diagnostic data and is not a thrown exception. Concrete types used with `ErrorType` and `ErrorSet` must satisfy their existing compile-time requirements, including `final` and the required nonthrowing move operations. Use `fail` and `widen_errors` to preserve typed failures across compatible error sets. Retain causal chains, source locations, and native diagnostics.

Handle third-party failures at the application-owned call boundary. Check the dependency's error return values or status codes and catch `const std::exception &` when the called API can throw. Translate these failures into the appropriate `Error`-based result before returning to application code. If a dependency can throw exceptions outside the `std::exception` hierarchy, contain and translate them at that same boundary. Do not propagate dependency-specific error representations or exceptions through application service interfaces. Use `inference::nativeCall` only around an actual native-library operation. Copy native error information before a plugin or runtime can be unloaded.

Keep transport validation in request codecs and HTTP helpers. Keep domain validation in the owning service or backend. Preserve protocol-defined identifiers, JSON field names, media types, status mappings, and numeric representations. Use the shared problem-response helpers rather than introducing another error envelope or returning a success-shaped response for a failure.

## Asynchronous Execution and Shutdown

`arch::Architecture` implementations must support concurrent const calls. Each operation owns its inputs and cancellation token. Mutable resources require explicit synchronization. Completion may run inline or later on another thread and must occur exactly once, including on cancellation. Scheduling must not block an HTTP event loop.

Track requests through `http::RequestLifetimeRegistry`. Retain the request context, selected implementation, and required runtime/package owners in asynchronous state. A request context does not retain the HTTP body or connection. Decode or copy required request data before scheduling. Do not capture a controller, stack reference, or raw request into work that can outlive it.

Propagate cancellation through `std::stop_token` and the existing task/inference APIs. Use the existing asynchronous reply guards to prevent duplicate or post-cancellation responses. Backend completion and HTTP response delivery are distinct: cancellation still requires backend completion and resource release, even if no HTTP response is sent.

Never invoke completion or resume a coroutine while holding an internal resource lock. Account for inline completion that can destroy an awaiter before its scheduling function returns. A cancellation request does not prove that native work has finished. Retain execution permits and runtime resources until protected work ends.

Shutdown must cancel request work, stop the affected services, and drain their tasks/callbacks before destroying their environment or unloading native plugins. Follow the owning executor's shutdown contract. Do not join or shut down an executor from its own callback.

## Localization, Logging, and Configuration

Use ICU resource sources under the owning module's `translations/` directory and register them with `dssp_add_translations`. The host `genrb` tool produces `.res` files for deployment. Commit source `.txt` resources, not compiled output. Keep resource keys and source-language text consistent with their use. Unless explicitly requested, do not supply translations for other locales.

Use `localization::Catalog` and `LocaleMatcher` for display text selected by `Accept-Language`. Catalogs use immutable snapshots. Do not mutate a process-global locale per request. Preserve fallback behavior and keep machine-readable IDs and protocol representations independent of the display locale. Format display numbers for the selected locale when appropriate, while retaining protocol-defined numeric formats.

Use sentence case for general user-facing text. Describe actions and results from the user's perspective without unnecessary implementation details.

Keep normal diagnostics on stderr through the application logger. Preserve both normal console output and structured JSON logging contracts. Do not insert free-form output into machine-readable command results or structured log streams. Include useful operation and request context without exposing request bodies or sensitive configuration.

Resolve configuration through the existing CLI, `DSSP_*` environment variables, and defaults in that order. Keep configuration validation and derived deployment paths in the runtime layer. Keep machine-specific paths in environment variables or CMake cache entries rather than source files. Do not silently substitute another execution provider when an explicitly selected provider fails.

## Configure, Build, and Run

Use a C++23 compiler, CMake 3.19 or later, qmsetup, and dependencies matching the repository's vcpkg manifest and overlay ports. The manifest's `tests` feature supplies Google Test. `DSSP_BUILD_TESTS=ON` is separately required to configure the test targets.

**Do not perform any test, build, run, or automatic formatting operations unless explicitly requested by the user. Do not include any of these operations in the plan unless explicitly requested by the user.** The commands below are reference material and do not authorize execution. The formatting rules and validation expectations in this file do not override this restriction.

When explicitly requested, use an existing suitable build directory or an out-of-source build. Configure dependencies with the matching vcpkg toolchain, installed triplet, and features. For example, after dependency setup:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DDSSP_BUILD_TESTS=ON
cmake --build build --target <target> --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix <install-dir>
```

For multi-configuration generators, specify `--config <configuration>` for build/install and `-C <configuration>` for CTest. On Windows, use a compiler developer shell and the `.exe` executable suffix. The default executable output is `build/out/bin/dssp`. Runtime plugins are deployed under `out/lib/plugins`, application data under `out/share`, and synchronized headers under `build/include`. Installation uses `bin`, `lib/plugins`, and `share`.

The main options are `DSSP_BUILD_TESTS`, `DSSP_INSTALL`, and `DSSP_ENABLE_CUDA`. Enabling CUDA also requires the corresponding dependency features and runtime deployment. GPU and platform-specific providers depend on the installed runtime. Preserve the plugin/runtime deployment checks in `cmake/`.

When a run is explicitly requested, use `dssp dump-devices` or `dssp serve` with the required package directory and provider. Model packages are obtained separately. Use the current CLI definitions for available options and defaults. `scripts/ci/Build.ps1` configures, builds, installs, and deploys dependencies. `scripts/ci/Create-Portable.ps1` packages an existing installation. Both scripts are subject to the same execution restriction.

## Testing and Validation

Tests use Google Test and are registered with CTest through `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`. Add a test to the owning target in `src/tests/CMakeLists.txt`. Preserve process isolation for Drogon's singleton, default-time-zone changes, and other global runtime state. Do not merge the separate HTTP, backend, inference-host, resource, extraction, metadata, logging, beat, or separation executables merely for convenience.

Cover affected behavior and regressions proportionally. Relevant cases include malformed requests, typed-error mappings, package ownership, cancellation, inline or duplicate completion, shutdown, cache eviction, provider selection, locale fallback, and numerical audio results. Use the existing temporary-package fixtures and synthetic data when practical. Do not make deterministic tests depend on a developer's model collection, network access, or a particular GPU. Identify tests that require native plugins or external model packages explicitly.

There is no numeric coverage gate. Report exact checks performed and their results. If builds, tests, runs, or formatting were not explicitly requested, state that they were not performed and do not imply successful validation. Read-only source and documentation review may still be performed. Do not count dependency tests as validation of first-party behavior.

## Commits and Pull Requests

- **Write the commit message as a single line**: an English, sentence-case imperative subject in American spelling, such as `Add inference cache configuration` or `Fix cancellation during synthesis`. Do not add a body or a mandatory Conventional Commit prefix. Design rationale belongs in code comments or the pull request description, not in the commit message.
- **Commit messages carry no AI attribution.** Do not add `Co-Authored-By` or `Generated with` lines.
- **Each commit does one thing.** Split work into small commits by concern instead of one large commit. If a file contains changes for two commits, take the old content with `git show HEAD:<path>`, apply only one set of changes, commit it, and then restore the complete version. Do not stage the whole file for convenience.
- Every commit must build and pass its tests on its own, including the intermediate states created by splitting. This requirement does not authorize execution without an explicit user request. Report any unperformed checks.
- **Do not commit without the author's authorization, and above all do not push without it.** Leave the changes in the working tree until the author approves the commit.

A pull request must explain the user-visible result, affected modules, architecture/API choices, compatibility decisions, and commands/tests run. Link relevant issues. Call out HTTP endpoints and schemas, CLI options, environment variables, localization resources, and dependency/license changes when affected. Keep unrelated formatting, generated output, and local configuration out of the diff.

## Security and Configuration Boundaries

Never commit credentials, tokens, private service URLs, machine paths, user model packages or audio, or generated package-manager state. Validate paths, URIs, request sizes, and external-process arguments at their owning boundaries. Use `std::filesystem`, stdcorelib path conversions, and the existing URI/media helpers rather than ad hoc path or shell-command construction. Preserve deployment portability across Windows, Linux, and macOS.

Preserve each component's declared license and review the license impact before adding or updating a dependency.
