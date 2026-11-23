#ifndef SRTBEATINFER_BEATAPIL1_H
#define SRTBEATINFER_BEATAPIL1_H

#include <functional>
#include <memory>
#include <vector>

#include <srtbeatinfer/Beat/Provider/BeatTask.h>
#include <synthrt/SVS/InferenceContrib.h>

namespace srtbeatinfer::Api::Beat::L1 {
    /// Identifies the beat contract.
    inline const srt::ContribContract Contract{"org.openvpi.srtbeatinfer.inference.Beat", 1};

    /// Finite, interleaved float32 PCM. Hosts decode and resample before calling.
    struct AudioSegment {
        int sampleRate = 0;
        int channelCount = 0;
        std::vector<float> samples;
        double startTime = 0;
    };

    class SRTBEATINFER_EXPORT BeatSchema : public srt::ContribExports {
    public:
        BeatSchema() : ContribExports(Contract) {}
        ~BeatSchema() override;

        int sampleRate = 0;
        int channelCount = 0;
        double maxSegmentDuration = 0; // Zero means the interpreter handles arbitrary lengths.
        bool downbeats = false; // Can identify the first beat of a measure.
    };

    SRTBEATINFER_EXPORT srt::Expected<std::unique_ptr<BeatSchema>>
    readBeatSchema(const srt::ContribSpec &spec);

    class BeatImportOptions : public srt::ContribImportOptions {
    public:
        BeatImportOptions() : ContribImportOptions(Contract) {}
    };

    class BeatRuntimeOptions : public srt::InferenceRuntimeOptions {
    public:
        BeatRuntimeOptions() : InferenceRuntimeOptions(Contract) {}
    };

    class BeatStartInput : public srt::TaskStartInput {
    public:
        inline static const auto &Contract = L1::Contract;

        BeatStartInput() : TaskStartInput(Contract) {}

        AudioSegment audio;
        // Reports completed inference blocks in [0,1] after feature computation.
        // Returning false requests cancellation.
        std::function<bool(double)> progress;
    };

    struct Beat {
        double time = 0; // Absolute seconds, including AudioSegment::startTime.
        bool downbeat = false;
    };

    class BeatResult : public srt::TaskResult {
    public:
        inline static const auto &Contract = L1::Contract;

        BeatResult() : TaskResult(Contract) {}

        std::vector<Beat> beats; // Strictly increasing times within the input; may be empty.
    };

    class SRTBEATINFER_EXPORT BeatExecutive : public srt::InferenceExecutive {
    public:
        using AsyncCallback = std::function<void(srt::Expected<std::unique_ptr<BeatResult>>)>;
        ~BeatExecutive() override;

        srt::Expected<std::unique_ptr<BeatResult>> start(const BeatStartInput &input) {
            return m_task.run(input);
        }
        srt::Expected<void> startAsync(std::shared_ptr<const BeatStartInput> input,
            AsyncCallback callback) {
            return m_task.runAsync(std::move(input), std::move(callback));
        }

        srt::ITask::State state() const noexcept override {
            return m_task.state();
        }

    protected:
        explicit BeatExecutive(srt::InferenceSpec &spec);
        virtual srt::Expected<std::unique_ptr<BeatResult>>
        run(const BeatStartInput &input, const srt::TaskStopToken &token) = 0;

        srt::Expected<void> quit() override {
            return m_task.stop();
        }
        srt::Expected<void> wait() override {
            return m_task.waitForFinished();
        }

    private:
        // Providers stop and wait before destroying members accessed by run().
        BeatTask<BeatStartInput, BeatResult> m_task;
    };

    SRTBEATINFER_EXPORT srt::Expected<std::unique_ptr<BeatExecutive>>
    createBeatTracker(srt::InferenceSpec &spec);
}

#endif
