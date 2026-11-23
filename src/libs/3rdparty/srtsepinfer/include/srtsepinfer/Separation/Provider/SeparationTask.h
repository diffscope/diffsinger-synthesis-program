// Adapted from otter (Apache-2.0); see LICENSE.otter and NOTICE.
#ifndef SRTSEPINFER_SEPARATIONTASK_H
#define SRTSEPINFER_SEPARATIONTASK_H

#include <functional>
#include <memory>
#include <utility>

#include <srtsepinfer/Separation/SeparationError.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

namespace srtsepinfer {

    /// Task of one separator, built on \c srt::ITask.
    ///
    /// \c srt::ITask owns the execution claim, the worker thread, the stop request and the wait.
    /// This class adds only the conversions between the untyped payloads and the payloads of a
    /// contract, and it reports \c SeparationError::Cancelled as \c Canceled.
    ///
    /// The body is a callable that receives the stop token of its execution. The executive holds
    /// this task as its last member and calls stop() and waitForFinished() before it destroys any
    /// object that the body reads.
    template <class Input, class Result>
    class SeparationTask : public srt::ITask {
    public:
        using Body = std::function<srt::Expected<std::unique_ptr<Result>>(
            const Input &input, const srt::TaskStopToken &token)>;
        using TypedCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        explicit SeparationTask(Body body) : srt::ITask(Input::Contract), m_body(std::move(body)) {
        }

        /// Runs the body synchronously on \a input.
        ///
        /// \return The result of the body, or an \c InvalidArgument error for a payload of
        /// another contract or during another execution.
        srt::Expected<std::unique_ptr<Result>> run(const Input &input) {
            auto result = start(static_cast<const srt::TaskStartInput &>(input));
            if (!result) {
                return result.takeError();
            }
            return toResult(result.take());
        }

        /// Runs the body on a worker thread and delivers the typed outcome to \a callback.
        srt::Expected<void> runAsync(std::shared_ptr<const Input> input, TypedCallback callback) {
            if (!callback) {
                return srt::Error(srt::Error::InvalidArgument,
                    "an asynchronous callback must not be empty");
            }
            auto started = startAsync(
                std::static_pointer_cast<const srt::TaskStartInput>(std::move(input)),
                [callback = std::move(callback)](
                    srt::Expected<std::unique_ptr<srt::TaskResult>> result) mutable {
                    if (!result) {
                        callback(result.takeError());
                        return;
                    }
                    callback(toResult(result.take()));
                });
            if (!started) {
                return started.takeError();
            }
            return {};
        }

    protected:
        srt::Expected<std::unique_ptr<srt::TaskResult>>
        execute(const srt::TaskStartInput &input, const srt::TaskStopToken &token) override {
            auto result = m_body(static_cast<const Input &>(input), token);
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::TaskResult>(result.take().release());
        }

        State classifyOutcome(const srt::Expected<std::unique_ptr<srt::TaskResult>> &outcome,
            bool) noexcept override {
            if (outcome) {
                return Succeeded;
            }
            return outcome.error().code() == SeparationError::Cancelled ? Canceled : Failed;
        }

        srt::Expected<void> startWorker(std::function<void()> work) override {
            auto started = ITask::startWorker(std::move(work));
            if (!started) {
                return srt::Error(SeparationError::NoWorker, started.error().message());
            }
            return {};
        }

    private:
        // Converts an untyped result to the result type of the contract after checking that it
        // belongs to the contract.
        static srt::Expected<std::unique_ptr<Result>>
        toResult(std::unique_ptr<srt::TaskResult> result) {
            if (!result) {
                return srt::Error(SeparationError::Internal, "the separation produced no result");
            }
            if (result->contract() != Result::Contract) {
                return srt::Error(SeparationError::ContractViolation,
                    "the separation produced a " + result->contract().toString() +
                        " result");
            }
            return std::unique_ptr<Result>(static_cast<Result *>(result.release()));
        }

        Body m_body;
    };

}

#endif // SRTSEPINFER_SEPARATIONTASK_H
