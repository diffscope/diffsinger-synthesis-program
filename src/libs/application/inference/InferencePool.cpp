#include "InferencePool.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include <stdcorelib/support/logging.h>
#include <synthrt/Core/ContribImportBinding.h>

#include <dsspinference/ResourceIdentity.h>
#include <dsspinference/ParallelismPolicy.h>
#include <dsspresource/LruCache.h>
#include <dsspresource/SerialQueue.h>
#include <dsspresource/WorkerPool.h>

namespace dssp::inference {
    namespace {
        stdc::LogCategory log("dssp.inference.InferencePool");

        struct LoadedInference {
            std::string label;
            std::unique_ptr<TaskExecutive> executive;

            ~LoadedInference() {
                if (!executive) {
                    return;
                }
                log.stdcDebug("Releasing inference resource %1", label);
                // start() is synchronous, and the lease outlives its stop callback.
                // The wait also covers native implementations with worker cleanup.
                auto waited = executive->waitForFinished();
                if (!waited) {
                    log.stdcWarning("Cannot finish releasing inference %1: %2", label, waited.error().formatChain());
                }
            }
        };
    }

    class InferenceResource {
    public:
        ExecutiveDefinition<TaskExecutive> definition;

        std::shared_ptr<resource::SerialQueue> queue;
        const void *owner = nullptr;
        mutable std::mutex dispatchMutex;
        std::optional<ExecutionError> dispatchError;
    };

    class InferencePool::Impl {
    public:
        Impl(std::string group, resource::CacheOptions options, std::unique_ptr<resource::WorkerPool> executor) : group(std::move(group)), options(options), cache(options), executor(std::move(executor)) {}
        std::string group;
        resource::CacheOptions options;
        resource::LruCache<std::string, std::unique_ptr<LoadedInference>> cache;
        std::unique_ptr<resource::WorkerPool> executor;
        std::shared_ptr<resource::ExecutionGate> serialExecution;
        std::mutex mutex;
        std::map<std::string, InferenceHandle> resources;
        bool stopped = false;
        std::once_flag shutdownOnce;
        std::mutex cleanupMutex;
        std::condition_variable_any cleanupWake;
        std::jthread cleanup;
    };

    std::expected<std::unique_ptr<InferencePool>, InitializationError> InferencePool::create(std::string group, resource::CacheOptions options, std::shared_ptr<resource::ExecutionGate> serialExecution) {
        const auto maximumInterval = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max() / 2);
        if (group.empty() || options.idleTimeout.count() < 0 || options.cleanupInterval.count() <= 0 || options.cleanupInterval > maximumInterval) {
            return std::unexpected{InitializationError("Invalid inference cache group options")};
        }
        try {
            auto executor = resource::WorkerPool::create("dssp-inference/" + group, 4);
            if (!executor) {
                return std::unexpected{InitializationError("Cannot create inference workers", executor.error().clone())};
            }
            auto pool = std::unique_ptr<InferencePool>(new InferencePool(std::make_unique<Impl>(std::move(group), options, std::move(*executor))));
            auto *impl = pool->m_impl.get();
            impl->serialExecution = std::move(serialExecution);
            if (options.idleTimeout.count() > 0) {
                impl->cleanup = std::jthread([impl](std::stop_token stop) {
                    std::unique_lock lock(impl->cleanupMutex);
                    while (!stop.stop_requested()) {
                        impl->cleanupWake.wait_for(lock, stop, impl->options.cleanupInterval, [] { return false; });
                        if (!stop.stop_requested()) {
                            lock.unlock();
                            impl->cache.sweep();
                            lock.lock();
                        }
                    }
                });
            }
            log.stdcDebug("Inference cache group %1: capacity=%2 idle_timeout_ms=%3 cleanup_interval_ms=%4", impl->group, options.capacity, options.idleTimeout.count(), options.cleanupInterval.count());
            return pool;
        } catch (const std::exception &error) {
            return std::unexpected{InitializationError("Cannot create inference workers", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"standard library", "create inference workers", "exception"}))};
        }
    }

    InferencePool::InferencePool(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    InferencePool::~InferencePool() { shutdown(); }

    std::expected<InferenceDefinition, ExecutionError> InferencePool::importDefinition(const srt::ContribImport &imported, std::shared_ptr<const srt::InferenceRuntimeOptions> options, const srt::JsonValue &effectiveRuntime) {
        try {
            const auto *binding = imported.binding();
            if (!binding || binding->state() != srt::ContribImportBinding::Active || !options) {
                return std::unexpected{ExecutionError("Inference import is not active")};
            }

            auto &target = binding->target();
            auto *spec = dynamic_cast<srt::InferenceSpec *>(&target);
            if (!spec || options->interfaceId() != target.interfaceId() || options->variant() != target.variant() || options->level() != target.level()) {
                return std::unexpected{ExecutionError("Inference runtime options do not match the resolved import")};
            }
            const auto *importOptions = &binding->options();
            const auto key = fingerprint(srt::JsonValue(srt::JsonObject{{"resource", resourceId(target)}, {"import", imported.manifestOptions()}, {"runtime", effectiveRuntime}}).toJson());
            if (!key) {
                return std::unexpected{ExecutionError("Cannot identify inference resource", key.error().clone())};
            }

            InferenceDefinition definition;
            definition.key = *key;
            definition.label = resourceReference(target);
            definition.packages = {binding->importer().package(), target.package()};
            definition.parallelSafe = permitsParallelExecution(target);
            definition.create = [spec, importOptions, options = std::move(options)] {
                return nativeCall("InferenceSpec::createInference", [&] { return spec->createInference(*importOptions, *options); });
            };

            return definition;
        } catch (const std::exception &error) {
            return std::unexpected{ExecutionError("Cannot inspect inference import", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"synthrt", "inspect inference import", "exception"}))};
        }
    }

    std::expected<InferenceHandle, ExecutionError> InferencePool::bind(InferenceDefinition definition) {
        return bindExecutive(std::move(definition));
    }

    std::expected<InferenceHandle, ExecutionError> InferencePool::bindTask(ExecutiveDefinition<TaskExecutive> definition) {
        if (definition.key.empty() || !definition.create) {
            return std::unexpected{ExecutionError("Inference resource requires an identity and a factory")};
        }

        const std::lock_guard lock(m_impl->mutex);
        if (m_impl->stopped) {
            return std::unexpected{ExecutionError("Inference cache group has been shut down")};
        }
        if (const auto found = m_impl->resources.find(definition.key); found != m_impl->resources.end()) {
            return found->second;
        }

        auto resource = std::make_shared<InferenceResource>();
        resource->definition = std::move(definition);
        resource->owner = m_impl.get();
        resource->queue = std::make_shared<resource::SerialQueue>([impl = m_impl.get(), weakResource = std::weak_ptr<InferenceResource>(resource)](std::function<void()> task) {
            auto dispatched = impl->executor->dispatch(task);
            if (dispatched) {
                return;
            }
            if (const auto resource = weakResource.lock()) {
                {
                    const std::lock_guard lock(resource->dispatchMutex);
                    resource->dispatchError.emplace("Cannot dispatch inference work", dispatched.error().clone());
                }
                resource->queue->close();
            }
            // Closing completes queued requests with the dispatch diagnostic;
            // this only clears the rejected drain's scheduled state.
            task();
        }, m_impl->group + "/" + resource->definition.label + "#" + resource->definition.key.substr(0, 12));
        m_impl->resources.emplace(resource->definition.key, resource);
        return InferenceHandle(std::move(resource));
    }

    void InferencePool::schedule(InferenceHandle handle, std::stop_token stop, std::function<bool(TaskExecutive &, std::stop_token)> run, std::function<void(std::optional<ExecutionError>, bool)> complete, resource::TaskProgress progress) {
        if (!handle || handle->owner != m_impl.get()) {
            complete(ExecutionError("Inference resource belongs to another cache group"), stop.stop_requested());
            return;
        }

        const auto error = std::make_shared<std::optional<ExecutionError>>();
        const auto admissionCancelled = std::make_shared<bool>(false);
        if (!stop.stop_requested()) progress.queue();
        handle->queue->submit({{}, [handle, error, admissionCancelled, complete = std::move(complete)](bool cancelled) mutable {
            std::optional<ExecutionError> dispatchError;
            {
                const std::lock_guard lock(handle->dispatchMutex);
                dispatchError = handle->dispatchError;
            }
            if (dispatchError) complete(std::move(dispatchError), false);
            else complete(std::move(*error), cancelled || *admissionCancelled);
        }, [this, handle, error, admissionCancelled, run = std::move(run)](std::stop_token token, auto finish) {
            drogon::async_run([this, handle, error, admissionCancelled, run, token, finish = std::move(finish)]() mutable -> drogon::Task<> {
                *admissionCancelled = co_await execute(handle, token, error, std::move(run));
                finish();
            });
        }}, stop);
    }

    drogon::Task<bool> InferencePool::execute(InferenceHandle handle, std::stop_token token, std::shared_ptr<std::optional<ExecutionError>> error, std::function<bool(TaskExecutive &, std::stop_token)> run) {
        auto *impl = m_impl.get();
        if (token.stop_requested()) co_return true;
        auto lease = impl->cache.acquire(handle->definition.key);
        if (!lease) {
            auto created = handle->definition.create();
            if (!created) {
                error->emplace(std::move(created.error()));
                co_return false;
            }
            if (!*created) {
                error->emplace("Inference provider returned a null executive");
                co_return false;
            }

            auto loaded = std::make_unique<LoadedInference>();
            loaded->label = handle->definition.label;
            loaded->executive = std::move(*created);
            log.stdcDebug("Loaded inference resource %1 into cache group %2", loaded->label, impl->group);
            lease.emplace(impl->cache.insert(handle->definition.key, std::move(loaded)));
        }

        auto &executive = *lease->value()->executive;
        // Loading/initialization occurs outside admission. Keep the lease
        // pinned while awaiting the shared execution permit.
        std::optional<resource::ExecutionPermit> permit;
        if (impl->serialExecution && !handle->definition.parallelSafe) {
            log.stdcTrace("Awaiting DirectML execution admission for inference %1", handle->definition.label);
            auto admitted = co_await impl->serialExecution->acquire(token);
            if (!admitted) co_return true;
            permit.emplace(std::move(*admitted));
            log.stdcTrace("Acquired DirectML execution admission for inference %1", handle->definition.label);
        }

        bool succeeded = false;
        {
            std::atomic_flag nativeStopRequested = ATOMIC_FLAG_INIT;
            const auto requestStop = [&] {
                if (nativeStopRequested.test_and_set(std::memory_order_acq_rel)) return;
                log.stdcTrace("Requesting stop for inference %1", handle->definition.label);
                auto stopped = executive.stop();
                if (!stopped) {
                    log.stdcWarning("Cannot request stop for inference %1: %2", handle->definition.label, stopped.error().formatChain());
                }
            };

            const std::stop_callback cancellation(token, requestStop);
            const std::stop_callback gateCancellation(permit ? permit->cancellation() : std::stop_token{}, requestStop);
            if (!token.stop_requested() && (!permit || !permit->cancellation().stop_requested())) {
                succeeded = run(executive, token);
                // A provider may finish native cleanup after producing a
                // result. Admission and the lease last until that is done.
                auto waited = executive.waitForFinished();
                if (!waited) {
                    error->emplace(std::move(waited.error()));
                    succeeded = false;
                }
            }
        }

        if (!succeeded || token.stop_requested() || executive.state() == srt::ITask::Canceled || executive.state() == srt::ITask::Failed) {
            // Cancelled/failed native tasks may retain stop or failure state.
            // Recreate on the next task rather than infecting another request.
            impl->cache.erase(handle->definition.key);
        }

        co_return token.stop_requested() || (permit && permit->cancellation().stop_requested());
    }

    std::size_t InferencePool::residentCount() const { return m_impl->cache.size(); }

    void InferencePool::sweep() { m_impl->cache.sweep(); }

    void InferencePool::shutdown() {
        std::call_once(m_impl->shutdownOnce, [&] {
            std::vector<InferenceHandle> resources;
            {
                const std::lock_guard lock(m_impl->mutex);
                m_impl->stopped = true;
                for (const auto &[key, resource] : m_impl->resources) {
                    resources.push_back(resource);
                }
            }

            m_impl->cleanup.request_stop();
            if (m_impl->cleanup.joinable()) {
                m_impl->cleanup.join();
            }

            for (const auto &resource : resources) {
                resource->queue->close();
            }

            for (const auto &resource : resources) {
                resource->queue->wait();
            }

            m_impl->executor->shutdown();
            m_impl->cache.clear();
            // Keep closed definitions valid for outstanding non-running handles.
            log.stdcDebug("Inference cache group %1 stopped", m_impl->group);
        });
    }
}
