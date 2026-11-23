#include "BeatFeatures.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriverPlugin.h>
#include <srtbeatinfer/Beat/Provider/BeatInterpreter.h>
#include <stdcorelib/plugin/plugin.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

namespace Beat = srtbeatinfer::Api::Beat::L1;
namespace Onnx = ds::Api::Onnx;
using srtbeatinfer::BeatError;
using srtbeatinfer::cancelledError;

namespace {
    constexpr char Variant[] = "beat-this";

    class Configuration final : public srt::ContribConfiguration {
    public:
        Configuration() : ContribConfiguration(srt::ContribContractTriplet{Beat::Contract, Variant}) {}

        std::filesystem::path model;
        int chunkFrames = 1500;
        int borderFrames = 6;
    };

    class Executive final : public Beat::BeatExecutive {
    public:
        Executive(srt::InferenceSpec &spec, std::unique_ptr<ds::InferenceSession> session, const Configuration &config)
            : BeatExecutive(spec), m_session(std::move(session)), m_config(config) {}

        ~Executive() override {
            (void)stop();
            (void)waitForFinished();
            (void)m_session->close();
        }

    protected:
        srt::Expected<void> quit() override {
            (void)BeatExecutive::quit();
            (void)m_session->stop();
            return {};
        }

        srt::Expected<void> wait() override {
            (void)BeatExecutive::wait();
            return m_session->waitForFinished();
        }

        srt::Expected<std::unique_ptr<Beat::BeatResult>>
        run(const Beat::BeatStartInput &input, const srt::TaskStopToken &token) override {
            const auto &audio = input.audio;
            if (audio.sampleRate != 22050 || audio.channelCount != 1 || audio.samples.empty() || !std::isfinite(audio.startTime) || audio.startTime < 0 || std::ranges::any_of(audio.samples, [](float value) { return !std::isfinite(value); }))
                return srt::Error(srt::Error::InvalidArgument, "Beat This requires nonempty finite mono PCM at 22050 Hz and a nonnegative finite startTime");

            const auto duration = static_cast<double>(audio.samples.size()) / audio.sampleRate;
            if (!std::isfinite(audio.startTime + duration))
                return srt::Error(srt::Error::InvalidArgument, "Audio time range overflows");

            const auto progress = [&](double value) {
                if (input.progress && !input.progress(value))
                    (void)stop();
                return !token.stopRequested();
            };
            auto features = m_features.compute(audio.samples, [&token] { return token.stopRequested(); });
            if (!features)
                return features.takeError();
            if (!progress(0))
                return cancelledError();

            const auto frameCount = features->size() / 128;
            if (frameCount > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max() - m_config.chunkFrames))
                return srt::Error(srt::Error::InvalidArgument, "Audio has too many feature frames");

            std::vector<float> beat(frameCount, -1000), downbeat(frameCount, -1000);
            std::vector<bool> assigned(frameCount, false);
            const auto starts = srtbeatinfer::beatthis::chunkStarts(frameCount, m_config.chunkFrames, m_config.borderFrames);

            for (std::size_t chunkIndex = 0; chunkIndex < starts.size(); ++chunkIndex) {
                if (token.stopRequested())
                    return cancelledError();

                const auto start = starts[chunkIndex];
                const auto end = std::min(start + m_config.chunkFrames, static_cast<std::ptrdiff_t>(frameCount));
                const auto left = std::max<std::ptrdiff_t>(0, -start);
                const auto right = std::max<std::ptrdiff_t>(0, std::min<std::ptrdiff_t>(m_config.borderFrames, start + m_config.chunkFrames - static_cast<std::ptrdiff_t>(frameCount)));
                const auto length = end - std::max<std::ptrdiff_t>(start, 0) + left + right;
                std::vector<float> chunk(length * 128, 0);
                std::copy(features->begin() + std::max<std::ptrdiff_t>(start, 0) * 128, features->begin() + end * 128, chunk.begin() + left * 128);

                auto tensor = ds::Tensor::createFromView<float>({1, length, 128}, {chunk.data(), chunk.size()});
                if (!tensor)
                    return tensor.takeError();

                Onnx::SessionStartInput modelInput;
                modelInput.inputs = {{"spectrogram", tensor.take()}};
                modelInput.outputs = {"beat", "downbeat"};
                if (token.stopRequested())
                    return cancelledError();

                auto ran = m_session->start(modelInput);
                if (token.stopRequested())
                    return cancelledError();
                if (!ran)
                    return ran.takeError().withContext("Beat This neural core failed");

                auto native = ran.take();
                auto *output = native ? native->as<Onnx::SessionResult>() : nullptr;
                if (!output)
                    return srt::Error(BeatError::ModelFailed, "ONNX driver returned no typed result");

                for (const auto *name : {"beat", "downbeat"}) {
                    const auto found = output->outputs.find(name);
                    if (found == output->outputs.end() || !found->second || found->second->dataType() != ds::ITensor::Float || found->second->shape() != std::vector<int64_t>{1, length})
                        return srt::Error(BeatError::ModelMismatch, std::string("Unexpected Beat This output shape or type: ") + name);
                    if (std::ranges::any_of(found->second->view<float>(), [](float value) { return !std::isfinite(value); }))
                        return srt::Error(BeatError::ModelMismatch, "Nonfinite Beat This logits");
                }

                const auto beats = output->outputs.at("beat")->view<float>(), downbeats = output->outputs.at("downbeat")->view<float>();
                for (std::ptrdiff_t i = m_config.borderFrames; i < length - m_config.borderFrames; ++i) {
                    const auto destination = start + i;
                    if (destination < 0 || destination >= static_cast<std::ptrdiff_t>(frameCount) || assigned[destination])
                        continue;
                    beat[destination] = beats[i];
                    downbeat[destination] = downbeats[i];
                    assigned[destination] = true; // Official keep_first overlap policy.
                }

                if (!progress(static_cast<double>(chunkIndex + 1) / starts.size()))
                    return cancelledError();
            }

            if (std::ranges::any_of(assigned, [](bool value) { return !value; }))
                return srt::Error(BeatError::Internal, "Beat chunk aggregation left uncovered frames");

            auto result = std::make_unique<Beat::BeatResult>();
            result->beats = srtbeatinfer::beatthis::postprocess(beat, downbeat, audio.startTime, duration);
            return result;
        }

    private:
        std::unique_ptr<ds::InferenceSession> m_session;
        const Configuration &m_config;
        srtbeatinfer::beatthis::Features m_features;
    };

    class Interpreter final : public srtbeatinfer::BeatInterpreter {
    public:
        srt::Expected<std::unique_ptr<srt::ContribExports>> createExports(const srt::ContribSpec &spec) const override {
            auto read = Beat::readBeatSchema(spec);
            if (!read)
                return read.takeError();

            const auto &schema = **read;
            if (schema.sampleRate != 22050 || schema.channelCount != 1 || schema.maxSegmentDuration != 0 || !schema.downbeats)
                return srt::Error(srt::Error::InvalidFormat, "Beat This requires 22050 Hz mono, unlimited duration and downbeat support");

            return std::unique_ptr<srt::ContribExports>(read.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>> createConfiguration(const srt::ContribSpec &spec) const override {
            if (!spec.manifestConfiguration().isObject())
                return srt::Error(srt::Error::InvalidFormat, "Beat This configuration must be an object");

            const auto object = spec.manifestConfiguration().toObject();
            if (object.size() != 3 || !object.count("model") || !object.count("chunkFrames") || !object.count("borderFrames"))
                return srt::Error(srt::Error::InvalidFormat, "Beat This configuration requires model, chunkFrames and borderFrames");
            if (!object.at("model").isString() || object.at("model").toString().empty() || !object.at("chunkFrames").isNumber() || !object.at("borderFrames").isNumber() || object.at("chunkFrames").toDouble() != 1500 || object.at("borderFrames").toDouble() != 6)
                return srt::Error(srt::Error::InvalidFormat, "Beat This requires a model path, chunkFrames=1500 and borderFrames=6");

            const auto path = std::filesystem::u8path(object.at("model").toString());
            if (path.is_absolute() || path.has_root_name())
                return srt::Error(srt::Error::InvalidFormat, "Model path must be relative to its declaration");

            auto config = std::make_unique<Configuration>();
            config->model = (spec.declarationPath().parent_path() / path).lexically_normal();
            return std::unique_ptr<srt::ContribConfiguration>(config.release());
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>> createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &, const srt::InferenceRuntimeOptions &) override {
            const auto *schema = spec.exports() ? spec.exports()->as<Beat::BeatSchema>() : nullptr;
            const auto *config = spec.configuration() ? spec.configuration()->as<Configuration>() : nullptr;
            if (!schema || !config)
                return srt::Error(BeatError::ContractViolation, "Missing typed Beat This declaration");

            auto *service = spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, Onnx::Contract.name());
            auto *driver = service ? service->as<ds::InferenceDriver>() : nullptr;
            if (!driver)
                return srt::Error(srt::Error::FeatureNotSupported, "ONNX inference driver is not registered");

            auto session = driver->createSession();
            if (!session)
                return srt::Error(BeatError::ModelFailed, "ONNX driver created no session");

            Onnx::SessionOpenArgs args;
            if (auto opened = session->open(config->model, args); !opened)
                return opened.takeError().withContext("Cannot open Beat This ONNX model");

            return std::unique_ptr<srt::InferenceExecutive>(new Executive(spec, std::move(session), *config));
        }
    };

    class Plugin final : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
        create(const srt::ContribContractTriplet &triplet) override {
            if (triplet != srt::ContribContractTriplet{Beat::Contract, Variant})
                return srt::Error(srt::Error::FeatureNotSupported, "This interpreter serves Beat Level 1 variant beat-this");

            return std::unique_ptr<srt::ContribInterpreter>(new Interpreter);
        }
    };
}

STDC_EXPORT_PLUGIN(Plugin)
