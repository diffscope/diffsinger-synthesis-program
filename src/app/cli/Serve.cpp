#include "Serve.h"

#include <exception>
#include <optional>
#include <set>
#include <string_view>

#include <drogon/drogon.h>
#include <stdcorelib/path.h>
#include <stdcorelib/support/logging.h>
#include <stdcorelib/support/systempaths.h>
#include <stdcorelib/system.h>

#include <dsspapiv1/Controllers.h>
#include <dsspapp/ArchComposition.h>
#include <dsspapp/InferenceComposition.h>
#include <dsspextraction/Service.h>
#include <dssphttp/AccessLog.h>
#include <dssphttp/Response.h>
#include <dsspruntime/ServerConfig.h>
#include <dsspmedia/Resources.h>

namespace dssp {

    namespace {

        stdc::LogCategory log("dssp.cli.Serve");

        void configureCors(drogon::HttpAppFramework &app) {
            app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr &request, drogon::AdviceCallback &&respond, drogon::AdviceChainCallback &&next) {
                if (request->method() != drogon::Options) {
                    next();
                    return;
                }

                // Before routing, supply the methods normally provided by
                // Drogon's HttpOptionsMiddleware; accept any requested method.
                const auto &method = request->getHeader("access-control-request-method");
                request->attributes()->insert("drogon.corsMethods", method.empty() ? std::string("GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS") : method);

                // No origin/header allowlist; accept null origins and credentials.
                respond(drogon::HttpResponse::newOptionsResponse(request, nullptr, true, true));
            });

            app.registerPreSendingAdvice([](const drogon::HttpRequestPtr &request, const drogon::HttpResponsePtr &response) {
                std::set<std::string_view> exposed{"content-type", "content-length", "date", "server"};
                for (const auto &[name, value] : response->headers()) {
                    exposed.insert(name);
                }

                response->addCorsHeaders(request, exposed, true);
            });
        }

        int runServe(const stdc::cli::ParseResult &result) {
            runtime::ConfigValues options, environment;
            std::filesystem::path applicationDir;
            std::optional<std::filesystem::path> temporaryDir;

            try {
                environment = stdc::system::environment();
                applicationDir = stdc::system::application_directory();
                temporaryDir = stdc::system::SystemPaths::writableDirectory(stdc::system::SystemPaths::TempDirectory);
                for (const auto name : {
                    "--host", "--port", "--max-request-body-size", "--package-dir", "--execution-provider", "--device-index", "--silence-threshold", "--stream-heartbeat-interval"
                }) {
                    if (const auto value = result.valueForOption<std::string>(name)) {
                        options.emplace(name, *value);
                    }
                }

                for (const auto &group : runtime::InferenceCacheGroups) {
                    for (const auto suffix : {"-cache-capacity", "-idle-timeout", "-cleanup-interval"}) {
                        const auto name = "--" + std::string(group.id) + suffix;
                        if (const auto value = result.valueForOption<std::string>(name)) options.emplace(name, *value);
                    }
                }
            } catch (const std::exception &error) {
                log.stdcCritical("Cannot read startup configuration: %1", error.what());
                return 1;
            }

            if (!temporaryDir || temporaryDir->empty() || !temporaryDir->is_absolute()) {
                log.stdcCritical("Cannot determine the system temporary directory for HTTP request caching");
                return 1;
            }

            auto config = runtime::resolveServerConfig(options, environment, applicationDir);
            if (!config) {
                log.stdcCritical("Configuration error (%1): %2", config.error().option(), config.error().formatChain());
                return 1;
            }

            log.stdcInfo("Starting DSSP %1", DSSP_VERSION);
            if (log.isLevelEnabled(stdc::Logger::Debug)) {
                log.stdcDebug("Resolved listener %1:%2, execution provider %3, device %4, silence threshold %5", config->host, config->port, runtime::DeviceInfo::executionProviderToString(config->executionProvider), config->deviceIndex, config->silenceThreshold);
                log.stdcDebug("Package directory: %1; plugin directory: %2", stdc::path::to_utf8(config->packageDir), stdc::path::to_utf8(config->pluginDir));
                log.stdcDebug("Resource directory: %1; runtime library: %2", stdc::path::to_utf8(config->resourceDir), stdc::path::to_utf8(config->runtimePath));
            }

            auto inference = initializeInference(*config, architecturePluginPaths(config->pluginDir));
            if (!inference) {
                log.stdcCritical("Inference initialization failed: %1", inference.error().formatChain());
                return 1;
            }

            const auto &resourceRoot = config->resourceDir;
            auto mediaResources = media::Resources::create(resourceRoot / "media" / "magic.mgc");
            if (!mediaResources) {
                log.stdcCritical("Media resource initialization failed: %1", mediaResources.error().formatChain());
                return 1;
            }

            const arch::ArchitectureServices services{*inference, *mediaResources, resourceRoot / "localization"};
            auto architectures = composeArchitectures(services);
            if (!architectures) {
                log.stdcCritical("Architecture initialization failed: %1", architectures.error().formatChain());
                return 1;
            }

            auto extractionService = extraction::Service::create(*inference, config->silenceThreshold);
            if (!extractionService) {
                log.stdcCritical("Extraction initialization failed: %1", extractionService.error().formatChain());
                return 1;
            }

            const auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
            const auto accessLog = std::make_shared<http::AccessLog>();
            auto &app = drogon::app();

            auto shutdown = [&app, lifetime, accessLog] {
                log.stdcInfo("Stopping HTTP service");
                accessLog->shutdown();
                lifetime->cancelAll();
                app.quit();
            };

            try {
                app.setClientMaxBodySize(config->maxRequestBodySize);
                log.stdcInfo("HTTP request body limit: %1 bytes", config->maxRequestBodySize);

                // Drogon also uses this path to cache large JSON request bodies.
                app.setUploadPath(stdc::path::to_utf8(*temporaryDir / "dssp" / "uploads"));
                if (log.isLevelEnabled(stdc::Logger::Debug)) {
                    log.stdcDebug("HTTP request cache directory: %1", stdc::path::to_utf8(*temporaryDir / "dssp" / "uploads"));
                }

                accessLog->install(app);
                configureCors(app);

                app.setExceptionHandler([](const std::exception &error, const drogon::HttpRequestPtr &request, http::Callback &&callback) {
                    // Drogon's exception boundary is the last safeguard for a
                    // throwing dependency escaping a request adapter.
                    const http::RequestErrors failure{http::HttpBoundaryError("Unhandled error at the HTTP library boundary", std::make_shared<ExternalError>(error.what(), NativeDiagnostic{"Drogon", "handle request", "exception"}))};
                    callback(http::problemResponse(failure, http::requestLog(*request)));
                });

                api_v1::registerControllers(app, lifetime, *architectures, *extractionService, config->streamHeartbeatInterval);

                app.setConnectionCallback([lifetime, accessLog](const trantor::TcpConnectionPtr &connection) {
                    accessLog->connectionChanged(connection);
                    lifetime->connectionChanged(connection);
                });

                app.setTermSignalHandler(shutdown);
                app.setIntSignalHandler(shutdown);

                app.registerBeginningAdvice([&app] {
                    // Drogon has bound its listeners before beginning advice,
                    // but starts accepting connections after this advice returns.
                    for (const auto &listener : app.getListeners()) {
                        log.stdcInfo("Starting HTTP listener: host=%1 port=%2", listener.toIp(), listener.toPort());
                    }
                });

                app.addListener(config->host, config->port);
                app.run();
            } catch (const std::exception &error) {
                accessLog->shutdown();
                lifetime->cancelAll();
                (*extractionService)->shutdown();
                (*inference)->shutdown();
                log.stdcCritical("Drogon startup/run failed: %1", error.what());
                return 1;
            }

            accessLog->shutdown();
            lifetime->cancelAll();
            (*extractionService)->shutdown();
            (*inference)->shutdown();
            log.stdcInfo("Server stopped");
            return 0;
        }

    }

    stdc::cli::Command serveCommand = [] {
        auto command = stdc::cli::Command{"serve", "Start the server. This server supports RFC 9727 discovery. Use --help on this command to see more options."}
            .addHelpOption()
            .addOption(stdc::cli::Option({"--host"}, "Host to listen on (DSSP_HOST)").arg("host"))
            .addOption(stdc::cli::Option({"--port"}, "Nonzero listening port (DSSP_PORT)").arg("port"))
            .addOption(stdc::cli::Option({"--max-request-body-size"}, "Maximum HTTP request body in bytes, default 1073741824 (1 GiB; DSSP_MAX_REQUEST_BODY_SIZE)").arg("bytes"))
            .addOption(stdc::cli::Option({"--package-dir"}, "Installed package root, default user data/OpenVPI/DiffScope_packages (DSSP_PACKAGE_DIR)").arg("dir"))
            .addOption(stdc::cli::Option({"--execution-provider"}, "cpu, cuda, directml or coreml; default DirectML on Windows, CPU elsewhere (DSSP_EXECUTION_PROVIDER)").arg("provider"))
            .addOption(stdc::cli::Option({"--device-index"}, "Native backend device index; automatically recommended when omitted (DSSP_DEVICE_INDEX)").arg("index"))
            .addOption(stdc::cli::Option({"--silence-threshold"}, "Linear RMS threshold, default 0.02 (DSSP_SILENCE_THRESHOLD)").arg("threshold"))
            .addOption(stdc::cli::Option({"--stream-heartbeat-interval"}, "Stream heartbeat interval in seconds, default 10 (DSSP_STREAM_HEARTBEAT_INTERVAL)").arg("seconds"))
            .setHandler(runServe);
        for (const auto &group : runtime::InferenceCacheGroups) {
            const auto option = "--" + std::string(group.id);
            const auto description = std::string(group.description);
            const auto prefix = std::string(group.environmentPrefix);
            command.addOption(stdc::cli::Option({option + "-cache-capacity"}, description + " resident resource limit, default 16; 0 disables the limit (" + prefix + "_CACHE_CAPACITY)").arg("count"));
            command.addOption(stdc::cli::Option({option + "-idle-timeout"}, description + " idle expiry in seconds, default 600; 0 disables expiry (" + prefix + "_IDLE_TIMEOUT)").arg("seconds"));
            command.addOption(stdc::cli::Option({option + "-cleanup-interval"}, description + " cleanup interval in seconds, default 60 (" + prefix + "_CLEANUP_INTERVAL)").arg("seconds"));
        }
        return command;
    }();

}
