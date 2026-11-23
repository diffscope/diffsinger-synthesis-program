#ifndef SRTSEPINFER_SEPARATIONAPIL1_H
#define SRTSEPINFER_SEPARATIONAPIL1_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <srtsepinfer/Separation/Provider/SeparationTask.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/DisplayText.h>

namespace srtsepinfer::Api::Separation::L1 {
    /// Identifies the separation contract.
    inline const srt::ContribContract Contract{"org.openvpi.srtsepinfer.inference.Separation", 1};

    /// Interleaved finite float32 PCM. No decoding or resampling occurs in the library.
    struct AudioSegment {
        int sampleRate = 0;
        int channelCount = 0;
        std::vector<float> samples;
        double startTime = 0;
    };

    struct TrackInfo {
        std::string id; // Stable identifier, also returned in Track::id.
        srt::DisplayText description; // Human-readable meaning of the source.
    };

    class SRTSEPINFER_EXPORT SeparationSchema : public srt::ContribExports {
    public:
        SeparationSchema() : ContribExports(Contract) {}
        ~SeparationSchema() override;

        int sampleRate = 0;
        int channelCount = 0;
        double maxSegmentDuration = 0; // 0: the interpreter partitions arbitrary-length audio.
        std::vector<TrackInfo> tracks; // Public result order; providers map the model's source axis.
    };

    SRTSEPINFER_EXPORT srt::Expected<std::unique_ptr<SeparationSchema>>
    readSeparationSchema(const srt::ContribSpec &spec);

    class SeparationImportOptions : public srt::ContribImportOptions {
    public:
        SeparationImportOptions() : ContribImportOptions(Contract) {}
    };

    class SeparationRuntimeOptions : public srt::InferenceRuntimeOptions {
    public:
        SeparationRuntimeOptions() : InferenceRuntimeOptions(Contract) {}
    };

    class SeparationStartInput : public srt::TaskStartInput {
    public:
        inline static const auto &Contract = L1::Contract;

        SeparationStartInput() : TaskStartInput(Contract) {}

        AudioSegment audio;
        /// Returning false cancels the execution. Progress is monotonic in [0,1].
        std::function<bool(double)> progress;
    };

    struct Track {
        std::string id;
        AudioSegment audio; // Same format, frame count and startTime as the input.
    };

    class SeparationResult : public srt::TaskResult {
    public:
        inline static const auto &Contract = L1::Contract;

        SeparationResult() : TaskResult(Contract) {}

        std::vector<Track> tracks;
    };

    class SRTSEPINFER_EXPORT SeparationExecutive : public srt::InferenceExecutive {
    public:
        using AsyncCallback =
            std::function<void(srt::Expected<std::unique_ptr<SeparationResult>>)>;
        ~SeparationExecutive() override;

        srt::Expected<std::unique_ptr<SeparationResult>> start(const SeparationStartInput &input) {
            return m_task.run(input);
        }
        srt::Expected<void> startAsync(std::shared_ptr<const SeparationStartInput> input,
            AsyncCallback callback) {
            return m_task.runAsync(std::move(input), std::move(callback));
        }

        srt::ITask::State state() const noexcept override {
            return m_task.state();
        }

    protected:
        explicit SeparationExecutive(srt::InferenceSpec &spec);
        virtual srt::Expected<std::unique_ptr<SeparationResult>>
        run(const SeparationStartInput &input, const srt::TaskStopToken &token) = 0;

        srt::Expected<void> quit() override {
            return m_task.stop();
        }
        srt::Expected<void> wait() override {
            return m_task.waitForFinished();
        }

    private:
        // Providers must stop and wait before destroying objects read by run().
        SeparationTask<SeparationStartInput, SeparationResult> m_task;
    };

    SRTSEPINFER_EXPORT srt::Expected<std::unique_ptr<SeparationExecutive>>
    createSeparator(srt::InferenceSpec &spec);
}

#endif
