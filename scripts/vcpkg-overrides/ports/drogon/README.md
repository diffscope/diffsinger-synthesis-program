# Drogon overlay port

## Source

This port is based on the official vcpkg Drogon 1.9.13#2 port at commit [659c7023698fc73894a0a296cc89a0143da912bb](https://github.com/microsoft/vcpkg/commit/659c7023698fc73894a0a296cc89a0143da912bb). The upstream source archive, dependencies, features, license declarations, and five packaging patches are preserved. The local port revision is 3.

## Connection callbacks

`0006-fix-non-linux-connection-callback.patch` forwards `ListenerManager::connectionCallback_` to each `HttpServer` in the non-Linux listener creation branch. This branch is used on Windows and macOS. The callback requires forwarding before the listeners start because application request cancellation depends on connection state notifications.

The defect is recorded in [Drogon issue #2531](https://github.com/drogonframework/drogon/issues/2531). As of October 17, 2026, the latest published release is [v1.9.14](https://github.com/drogonframework/drogon/releases/tag/v1.9.14), released on October 12, 2026. Its [ListenerManager implementation](https://github.com/drogonframework/drogon/blob/v1.9.14/lib/src/ListenerManager.cc) still omits this forwarding. The latest official vcpkg port remains 1.9.13#2.

## Validation

An independent Windows HTTP server linked against Drogon was used for validation. The server registers a connection callback and retains pending work with a cancellation token. It does not use DSSP sources or models. Dependency builds and probe runs were performed outside this repository. Existing installed dependencies were used without modification.

Unpatched Drogon 1.9.13#2 and the published v1.9.14 release both omit cancellation notifications for ordinary TCP closure and forced TCP reset, including during asynchronous response streaming. With the patch, each disconnected request receives exactly one cancellation notification. Pending requests with live connections remain active.

The six probe cases cover pending-response closure, pending-response reset, streaming-response closure, streaming-response reset, a live pending response, and a live streaming response. Both unpatched versions fail the four disconnect cases and pass the two live-connection controls. The patched port passes all six cases. Runtime validation was performed on Windows with MSVC 19.51 and Trantor 1.5.28. The macOS branch was reviewed in source but was not executed.

The source archive SHA512 matches the port declaration. All six patches apply sequentially to the archive, and the resulting listener implementation matches the externally built implementation after line-ending normalization. The five upstream patch files and usage file are byte-identical to the official port.

No DSSP configuration, build, test, run, or automatic formatting operation was performed. No package installation, vcpkg update, or submodule update was performed.
