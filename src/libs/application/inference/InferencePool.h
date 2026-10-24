#ifndef DSSP_INFERENCE_INFERENCEPOOL_H
#define DSSP_INFERENCE_INFERENCEPOOL_H

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/SVS/InferenceContrib.h>

#include <dsspinference/InferenceError.h>
#include <dsspinference/NativeCall.h>
#include <dsspinference/TaskExecutive.h>
#include <dsspresource/CacheOptions.h>
#include <dsspresource/ExecutionGate.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::inference {

    class InferenceResource;
    using InferenceHandle = std::shared_ptr<const InferenceResource>;

    template <class Executive>
    struct ExecutiveDefinition {
        std::string key;
        std::string label;
        // Retain both the target and the importer owning typed import options.
        std::vector<srt::PackageHandle> packages;
        // The factory returns a ready-to-run executive without throwing.
        std::function<std::expected<std::unique_ptr<Executive>, ExecutionError>()> create;
        // False is deliberately the default for custom/unknown resource types.
        bool parallelSafe = false;
    };

    using InferenceDefinition = ExecutiveDefinition<srt::InferenceExecutive>;

    // One independently configured cache group. Resource definitions and FIFO
    // queues survive eviction; only initialized executives occupy cache entries.
    class InferencePool final {
    public:
        static std::expected<std::unique_ptr<InferencePool>, InitializationError> create(std::string group, resource::CacheOptions = {}, std::shared_ptr<resource::ExecutionGate> serialExecution = {});
        ~InferencePool();
        InferencePool(const InferencePool &) = delete;
        InferencePool &operator=(const InferencePool &) = delete;
        std::expected<InferenceHandle, ExecutionError> bind(InferenceDefinition);

        template <class Executive>
        std::expected<InferenceHandle, ExecutionError> bindExecutive(ExecutiveDefinition<Executive> definition) {
            if (!definition.create) {
                return std::unexpected{ExecutionError("Task resource requires a factory")};
            }

            ExecutiveDefinition<TaskExecutive> adapted;
            adapted.key = std::move(definition.key);
            adapted.label = std::move(definition.label);
            adapted.packages = std::move(definition.packages);
            adapted.parallelSafe = definition.parallelSafe;
            adapted.create = [factory = std::move(definition.create)]() -> std::expected<std::unique_ptr<TaskExecutive>, ExecutionError> {
                auto created = factory();
                if (!created) {
                    return std::unexpected{std::move(created.error())};
                }
                if (!*created) {
                    return std::unexpected{ExecutionError("Task provider returned a null executive")};
                }

                return std::make_unique<TaskExecutive>(std::move(*created));
            };

            return bindTask(std::move(adapted));
        }

        template <class Executive, class InitArgs>
        std::expected<InferenceHandle, ExecutionError> bindImport(const srt::ContribImport &imported, std::shared_ptr<const srt::InferenceRuntimeOptions> options, const srt::JsonValue &effectiveRuntime) {
            auto definition = importDefinition(imported, std::move(options), effectiveRuntime);
            if (!definition) {
                return std::unexpected{std::move(definition.error())};
            }

            auto factory = std::move(definition->create);
            definition->create = [factory = std::move(factory)]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, ExecutionError> {
                auto created = factory();
                if (!created) {
                    return std::unexpected{std::move(created.error())};
                }

                auto *typed = dynamic_cast<Executive *>(created->get());
                if (!typed) {
                    return std::unexpected{ExecutionError("Inference provider returned an incompatible executive")};
                }

                auto initialized = nativeCall("InferenceExecutive::initialize", [&] { return typed->initialize(InitArgs{}); });
                if (!initialized) {
                    return std::unexpected{std::move(initialized.error())};
                }

                return std::move(*created);
            };

            return bind(std::move(*definition));
        }

        // Invoke the synchronous native task contract on a pool worker. The
        // function must finish the task before returning its srt::Expected.
        template <class Executive, class Function, class Completion>
        void run(InferenceHandle handle, std::stop_token stop, Function function, Completion complete, resource::TaskProgress progress = {}) {
            using Value = typename std::invoke_result_t<Function, Executive &>::value_type;
            use<Executive>(std::move(handle), stop, [function = std::move(function)](Executive &executive, std::stop_token) mutable -> ExecutionResult<Value> {
                auto native = nativeCall("InferenceExecutive::start", [&] { return std::invoke(function, executive); });
                if (!native) {
                    return fail<ExecutionErrors>(std::move(native.error()));
                }
                if constexpr (std::is_void_v<Value>) {
                    return {};
                } else {
                    return std::move(*native);
                }
            }, std::move(complete), std::move(progress));
        }

        // Run a sequence on one resource lease. Each native call is adapted at
        // its own boundary; cancellation also covers shutdown between spans.
        template <class Executive, class Function, class Completion>
        void use(InferenceHandle handle, std::stop_token stop, Function function, Completion complete, resource::TaskProgress progress = {}) {
            using Value = typename std::invoke_result_t<Function, Executive &, std::stop_token>::value_type;
            const auto result = std::make_shared<std::optional<ExecutionResult<Value>>>();
            schedule(std::move(handle), stop, [result, function = std::move(function), progress](TaskExecutive &executive, std::stop_token token) mutable {
                auto *typed = dynamic_cast<Executive *>(&executive.native());
                if (!typed) {
                    result->emplace(fail<ExecutionErrors>(ExecutionError("Inference resource does not match the requested contract")));
                    return false;
                }

                progress.advance(0);
                result->emplace(std::invoke(function, *typed, token));
                if (executive.state() == srt::ITask::Canceled) {
                    result->emplace(fail<ExecutionErrors>(CancelledError("Inference execution was cancelled")));
                    return false;
                } else if (executive.state() == srt::ITask::Failed && **result) {
                    result->emplace(fail<ExecutionErrors>(ExecutionError("Inference reported a failed task with a successful result")));
                    return false;
                }
                return static_cast<bool>(**result);
            }, [result, complete = std::move(complete)](std::optional<ExecutionError> error, bool cancelled) mutable {
                if (cancelled) {
                    complete(ExecutionResult<Value>{fail<ExecutionErrors>(CancelledError("Inference request was cancelled"))});
                } else if (error) {
                    complete(ExecutionResult<Value>{fail<ExecutionErrors>(std::move(*error))});
                } else if (*result) {
                    complete(std::move(**result));
                } else {
                    complete(ExecutionResult<Value>{fail<ExecutionErrors>(ExecutionError("Inference task completed without a result"))});
                }
            }, progress);
        }

        std::size_t residentCount() const;
        template <class Executive, class Function>
        auto runCoro(InferenceHandle handle, std::stop_token stop, Function function, resource::TaskProgress progress = {}) {
            using Value = typename std::invoke_result_t<Function, Executive &>::value_type;
            return resource::awaitCallback<ExecutionResult<Value>>([this, handle = std::move(handle), stop, function = std::move(function), progress = std::move(progress)](auto complete) mutable {
                run<Executive>(std::move(handle), stop, std::move(function), std::move(complete), std::move(progress));
            });
        }

        template <class Executive, class Function>
        auto useCoro(InferenceHandle handle, std::stop_token stop, Function function, resource::TaskProgress progress = {}) {
            using Value = typename std::invoke_result_t<Function, Executive &, std::stop_token>::value_type;
            return resource::awaitCallback<ExecutionResult<Value>>([this, handle = std::move(handle), stop, function = std::move(function), progress = std::move(progress)](auto complete) mutable {
                use<Executive>(std::move(handle), stop, std::move(function), std::move(complete), std::move(progress));
            });
        }

        void sweep();
        // Cancel queues, request native stop, wait for callbacks, release all
        // executives. Call on the host thread before releasing the SynthUnit.
        void shutdown();

    private:
        class Impl;
        explicit InferencePool(std::unique_ptr<Impl>);
        static std::expected<InferenceDefinition, ExecutionError> importDefinition(const srt::ContribImport &, std::shared_ptr<const srt::InferenceRuntimeOptions>, const srt::JsonValue &);
        std::expected<InferenceHandle, ExecutionError> bindTask(ExecutiveDefinition<TaskExecutive>);
        void schedule(InferenceHandle, std::stop_token, std::function<bool(TaskExecutive &, std::stop_token)>, std::function<void(std::optional<ExecutionError>, bool)>, resource::TaskProgress);
        drogon::Task<bool> execute(InferenceHandle, std::stop_token, std::shared_ptr<std::optional<ExecutionError>>, std::function<bool(TaskExecutive &, std::stop_token)>);
        std::unique_ptr<Impl> m_impl;
    };
}

#endif // DSSP_INFERENCE_INFERENCEPOOL_H
