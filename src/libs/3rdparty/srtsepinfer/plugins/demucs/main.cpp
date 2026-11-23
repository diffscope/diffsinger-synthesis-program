#include "SpectralTransform.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <span>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriverPlugin.h>
#include <srtsepinfer/Api/Separation/1/SeparationApiL1.h>
#include <srtsepinfer/Separation/Provider/SeparationInterpreter.h>
#include <stdcorelib/plugin/plugin.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

namespace Sep = srtsepinfer::Api::Separation::L1;
namespace Onnx = ds::Api::Onnx;
using srtsepinfer::cancelledError;
using srtsepinfer::SeparationError;

namespace {
    constexpr char Variant[] = "demucs";
    const std::vector<std::string> Sources4{"drums", "bass", "other", "vocals"};
    const std::vector<std::string> Sources6{"drums", "bass", "other", "vocals", "guitar", "piano"};

    class Configuration final : public srt::ContribConfiguration {
    public:
        Configuration() : ContribConfiguration(srt::ContribContractTriplet{Sep::Contract, Variant}) {}

        std::filesystem::path model;
        int segmentSamples = 0;
        int fftSize = 0;
        int hopLength = 0;
        double overlap = 0.25;
        std::size_t modelTrackCount = 0;
        std::vector<std::size_t> trackMapping; // Export order -> model source axis.
    };

    class Executive final : public Sep::SeparationExecutive {
    public:
        Executive(srt::InferenceSpec &spec, std::unique_ptr<ds::InferenceSession> session, const Sep::SeparationSchema &schema, const Configuration &configuration)
            : SeparationExecutive(spec), m_session(std::move(session)), m_schema(schema), m_configuration(configuration), m_transform(configuration.fftSize, configuration.hopLength) {}

        ~Executive() override {
            (void)stop();
            (void)waitForFinished();
            (void)m_session->close();
        }

    protected:
        srt::Expected<void> quit() override {
            (void)SeparationExecutive::quit();
            (void)m_session->stop();
            return {};
        }

        srt::Expected<void> wait() override {
            (void)SeparationExecutive::wait();
            return m_session->waitForFinished();
        }

        srt::Expected<std::unique_ptr<Sep::SeparationResult>>
        run(const Sep::SeparationStartInput &input, const srt::TaskStopToken &token) override {
            const auto &audio = input.audio;
            if (audio.sampleRate != m_schema.sampleRate || audio.channelCount != m_schema.channelCount || audio.samples.empty() || audio.samples.size() % audio.channelCount || !std::isfinite(audio.startTime) || audio.startTime < 0) {
                return srt::Error(srt::Error::InvalidArgument, "Demucs requires nonempty, interleaved stereo audio at 44100 Hz with a finite nonnegative startTime");
            }
            if (std::ranges::any_of(audio.samples, [](float value) { return !std::isfinite(value); }))
                return srt::Error(srt::Error::InvalidArgument, "Audio contains nonfinite samples");

            const auto count = audio.samples.size() / audio.channelCount;
            const auto segment = static_cast<std::size_t>(m_configuration.segmentSamples);
            const auto stride = static_cast<std::size_t>((1 - m_configuration.overlap) * segment);
            const auto frames = (segment + m_configuration.hopLength - 1) / m_configuration.hopLength;
            const auto bins = m_configuration.fftSize / 2;
            const auto sources = m_schema.tracks.size();
            const auto modelSources = m_configuration.modelTrackCount;

            bool identityMapping = sources == modelSources;
            for (std::size_t i = 0; i < sources; ++i)
                identityMapping = identityMapping && m_configuration.trackMapping[i] == i;

            const auto stopped = [&token] { return token.stopRequested(); };
            const auto progress = [&](double value) {
                if (input.progress && !input.progress(value))
                    (void)stop();
                return !token.stopRequested();
            };
            if (!progress(0))
                return cancelledError();

            auto result = std::make_unique<Sep::SeparationResult>();
            for (const auto &track : m_schema.tracks)
                result->tracks.push_back({track.id, {audio.sampleRate, audio.channelCount, std::vector<float>(audio.samples.size(), 0), audio.startTime}});

            // Official separation normalizes using the mono reference, once for
            // the entire recording. The neural graph also performs its own two
            // branch normalizations, which serve a different purpose.
            double mean = 0;
            for (std::size_t i = 0; i < count; ++i)
                mean += (static_cast<double>(audio.samples[2 * i]) + audio.samples[2 * i + 1]) / 2;
            mean /= count;

            double variance = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = (static_cast<double>(audio.samples[2 * i]) + audio.samples[2 * i + 1]) / 2 - mean;
                variance += value * value;
            }
            double deviation = count > 1 ? std::sqrt(variance / (count - 1)) : 0;
            if (deviation < 1e-8)
                deviation = 1; // Keep silence/constant signals finite.

            std::vector<float> weights(count, 0), triangle(segment);
            for (std::size_t i = 0; i < segment; ++i)
                triangle[i] = static_cast<float>(std::min(i + 1, segment - i)) / (segment / 2);

            const auto chunks = (count - 1) / stride + 1;
            std::size_t chunkIndex = 0;
            for (std::size_t offset = 0; offset < count; offset += stride) {
                if (token.stopRequested())
                    return cancelledError();

                const auto chunkLength = std::min(segment, count - offset);
                const auto padding = (segment - chunkLength) / 2;
                // TensorChunk.padded: retain available context on both sides,
                // center the final short chunk, and zero-pad only outside audio.
                const auto begin = static_cast<std::ptrdiff_t>(offset) - static_cast<std::ptrdiff_t>(padding);
                std::vector<float> waveform(2 * segment, 0);
                for (std::size_t i = 0; i < segment; ++i) {
                    const auto source = begin + static_cast<std::ptrdiff_t>(i);
                    if (source < 0 || source >= static_cast<std::ptrdiff_t>(count))
                        continue;
                    for (int channel = 0; channel < 2; ++channel)
                        waveform[channel * segment + i] = static_cast<float>((audio.samples[2 * source + channel] - mean) / deviation);
                }

                if (std::ranges::any_of(waveform, [](float value) { return !std::isfinite(value); }))
                    return srt::Error(srt::Error::InvalidArgument, "Normalized audio cannot be represented as float32");

                auto spectrum = m_transform.forward(waveform, 2, stopped);
                if (!spectrum)
                    return spectrum.takeError();

                auto waveTensor = ds::Tensor::createFromView<float>({1, 2, static_cast<int64_t>(segment)}, {waveform.data(), waveform.size()});
                auto specTensor = ds::Tensor::createFromView<float>({1, 4, bins, static_cast<int64_t>(frames)}, {spectrum->data(), spectrum->size()});
                if (!waveTensor)
                    return waveTensor.takeError();
                if (!specTensor)
                    return specTensor.takeError();
                Onnx::SessionStartInput modelInput;
                modelInput.inputs = {{"waveform", waveTensor.take()}, {"spectrogram", specTensor.take()}};
                modelInput.outputs = {"spectrum", "waveform_sources"};
                if (token.stopRequested())
                    return cancelledError();
                auto ran = m_session->start(modelInput);
                if (token.stopRequested())
                    return cancelledError();
                if (!ran)
                    return ran.takeError().withContext("Demucs neural core failed");
                auto native = ran.take();
                auto *output = native ? native->as<Onnx::SessionResult>() : nullptr;
                if (!output)
                    return srt::Error(SeparationError::ModelFailed, "ONNX driver returned no typed result");

                const std::vector<int64_t> spectrumShape{1, static_cast<int64_t>(modelSources), 4, bins, static_cast<int64_t>(frames)};
                const std::vector<int64_t> timeShape{1, static_cast<int64_t>(modelSources), 2, static_cast<int64_t>(segment)};
                for (const auto &[name, shape] : {std::pair{"spectrum", spectrumShape}, std::pair{"waveform_sources", timeShape}}) {
                    const auto found = output->outputs.find(name);
                    if (found == output->outputs.end() || !found->second || found->second->dataType() != ds::ITensor::Float || found->second->shape() != shape)
                        return srt::Error(SeparationError::ModelMismatch, std::string("Unexpected Demucs output shape or type: ") + name);
                    const auto values = found->second->view<float>();
                    if (std::ranges::any_of(values, [](float value) { return !std::isfinite(value); }))
                        return srt::Error(SeparationError::ModelMismatch, std::string("Nonfinite Demucs output: ") + name);
                }

                const auto frequency = output->outputs.at("spectrum")->view<float>();
                const auto sourceSpectrumSize = 4 * static_cast<std::size_t>(bins) * frames;
                std::span<const float> exportedSpectrum(frequency.data(), frequency.size());
                std::vector<float> selectedSpectrum;
                if (sources == 1) {
                    exportedSpectrum = exportedSpectrum.subspan(m_configuration.trackMapping.front() * sourceSpectrumSize, sourceSpectrumSize);
                } else if (!identityMapping) {
                    selectedSpectrum.resize(sources * sourceSpectrumSize);
                    for (std::size_t i = 0; i < sources; ++i)
                        std::copy_n(frequency.data() + m_configuration.trackMapping[i] * sourceSpectrumSize, sourceSpectrumSize, selectedSpectrum.data() + i * sourceSpectrumSize);
                    exportedSpectrum = selectedSpectrum;
                }

                auto reconstructed = m_transform.inverse(exportedSpectrum, static_cast<int>(sources * 2), segment, stopped);
                if (!reconstructed)
                    return reconstructed.takeError();

                const auto time = output->outputs.at("waveform_sources")->view<float>();
                for (std::size_t i = 0; i < chunkLength; ++i) {
                    weights[offset + i] += triangle[i];
                    for (std::size_t source = 0; source < sources; ++source) {
                        for (int channel = 0; channel < 2; ++channel) {
                            const auto index = (source * 2 + channel) * segment + padding + i;
                            const auto modelIndex = (m_configuration.trackMapping[source] * 2 + channel) * segment + padding + i;
                            const auto value = ((*reconstructed)[index] + time[modelIndex]) * deviation + mean;
                            result->tracks[source].audio.samples[2 * (offset + i) + channel] += static_cast<float>(value * triangle[i]);
                        }
                    }
                }

                if (!progress(static_cast<double>(++chunkIndex) / chunks))
                    return cancelledError();
            }

            for (auto &track : result->tracks) {
                for (std::size_t i = 0; i < count; ++i) {
                    if ((i & 65535) == 0 && token.stopRequested())
                        return cancelledError();
                    for (int channel = 0; channel < 2; ++channel) {
                        auto &value = track.audio.samples[2 * i + channel];
                        value /= weights[i];
                        if (!std::isfinite(value))
                            return srt::Error(SeparationError::ModelMismatch, "Separated audio contains nonfinite samples");
                    }
                }
            }

            return result;
        }

    private:
        std::unique_ptr<ds::InferenceSession> m_session;
        const Sep::SeparationSchema &m_schema;
        const Configuration &m_configuration;
        srtsepinfer::demucs::SpectralTransform m_transform;
    };

    class Interpreter final : public srtsepinfer::SeparationInterpreter {
    public:
        srt::Expected<std::unique_ptr<srt::ContribExports>> createExports(const srt::ContribSpec &spec) const override {
            auto read = Sep::readSeparationSchema(spec);
            if (!read)
                return read.takeError();

            const auto &schema = **read;
            if (schema.sampleRate != 44100 || schema.channelCount != 2 || schema.maxSegmentDuration != 0)
                return srt::Error(srt::Error::InvalidFormat, "Demucs exports require 44100 Hz stereo and unlimited duration");

            return std::unique_ptr<srt::ContribExports>(read.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>> createConfiguration(const srt::ContribSpec &spec) const override {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject())
                return srt::Error(srt::Error::InvalidFormat, "Demucs configuration must be an object");

            const auto object = value.toObject();
            const std::set<std::string> fields{"model", "segmentSamples", "fftSize", "hopLength", "overlap"};
            for (const auto &[field, unused] : object)
                if (!fields.count(field) && field != "modelTracks" && field != "trackMapping")
                    return srt::Error(srt::Error::InvalidFormat, "Unknown Demucs configuration field: " + field);
            for (const auto &field : fields)
                if (!object.count(field))
                    return srt::Error(srt::Error::InvalidFormat, "Missing Demucs configuration field: " + field);

            auto configuration = std::make_unique<Configuration>();
            if (!object.at("model").isString() || object.at("model").toString().empty())
                return srt::Error(srt::Error::InvalidFormat, "Demucs model must be a nonempty relative path");

            const auto path = std::filesystem::u8path(object.at("model").toString());
            if (path.is_absolute() || path.has_root_name())
                return srt::Error(srt::Error::InvalidFormat, "Demucs model path must be relative to its declaration");
            configuration->model = (spec.declarationPath().parent_path() / path).lexically_normal();

            for (const auto &[key, member] : {std::pair{"segmentSamples", &configuration->segmentSamples}, std::pair{"fftSize", &configuration->fftSize}, std::pair{"hopLength", &configuration->hopLength}}) {
                if (!object.at(key).isNumber())
                    return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be an integer");
                const auto number = object.at(key).toDouble();
                if (!std::isfinite(number) || std::floor(number) != number || number <= 0 || number > std::numeric_limits<int>::max())
                    return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be a positive integer");
                *member = static_cast<int>(number);
            }

            if (!object.at("overlap").isNumber())
                return srt::Error(srt::Error::InvalidFormat, "overlap must be a number");
            configuration->overlap = object.at("overlap").toDouble();
            if (configuration->segmentSamples != 343980 || configuration->fftSize != 4096 || configuration->hopLength != 1024 || !std::isfinite(configuration->overlap) || configuration->overlap < 0 || configuration->overlap > 0.5)
                return srt::Error(srt::Error::InvalidFormat, "Unsupported HTDemucs graph geometry or overlap (expected 343980 samples, FFT 4096, hop 1024, overlap in [0,0.5])");

            auto schema = Sep::readSeparationSchema(spec);
            if (!schema)
                return schema.takeError();
            std::vector<std::string> exportedIds;
            for (const auto &track : (*schema)->tracks)
                exportedIds.push_back(track.id);

            const bool hasModelTracks = object.count("modelTracks");
            const bool hasMapping = object.count("trackMapping");
            if (hasModelTracks != hasMapping)
                return srt::Error(srt::Error::InvalidFormat, "modelTracks and trackMapping must be declared together");

            std::vector<std::string> modelTracks;
            if (hasModelTracks) {
                if (!object.at("modelTracks").isArray())
                    return srt::Error(srt::Error::InvalidFormat, "modelTracks must be the ordered model source names");
                for (const auto &track : object.at("modelTracks").toArray()) {
                    if (!track.isString())
                        return srt::Error(srt::Error::InvalidFormat, "Each modelTracks entry must be a string");
                    modelTracks.push_back(track.toString());
                }
            } else {
                // Legacy declarations exported the complete model layout.
                modelTracks = exportedIds;
            }

            if (modelTracks != Sources4 && modelTracks != Sources6)
                return srt::Error(srt::Error::InvalidFormat, "modelTracks must match the official ordered four or six Demucs sources");

            configuration->modelTrackCount = modelTracks.size();
            if (hasMapping) {
                if (!object.at("trackMapping").isObject())
                    return srt::Error(srt::Error::InvalidFormat, "trackMapping must map every exported track ID to a model track name");
                const auto mapping = object.at("trackMapping").toObject();
                if (mapping.size() != exportedIds.size())
                    return srt::Error(srt::Error::InvalidFormat, "trackMapping keys must exactly match exports.tracks IDs");
                for (const auto &id : exportedIds) {
                    const auto entry = mapping.find(id);
                    if (entry == mapping.end() || !entry->second.isString())
                        return srt::Error(srt::Error::InvalidFormat, "Missing or non-string trackMapping entry for " + id);
                    const auto source = std::ranges::find(modelTracks, entry->second.toString());
                    if (source == modelTracks.end())
                        return srt::Error(srt::Error::InvalidFormat, "Unknown model track mapped from " + id);
                    configuration->trackMapping.push_back(static_cast<std::size_t>(source - modelTracks.begin()));
                }
            } else {
                for (std::size_t i = 0; i < modelTracks.size(); ++i)
                    configuration->trackMapping.push_back(i);
            }

            return std::unique_ptr<srt::ContribConfiguration>(configuration.release());
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>> createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &, const srt::InferenceRuntimeOptions &) override {
            const auto *schema = spec.exports() ? spec.exports()->as<Sep::SeparationSchema>() : nullptr;
            const auto *configuration = spec.configuration() ? spec.configuration()->as<Configuration>() : nullptr;
            if (!schema || !configuration)
                return srt::Error(SeparationError::ContractViolation, "Missing typed Demucs declaration");

            auto *service = spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, Onnx::Contract.name());
            auto *driver = service ? service->as<ds::InferenceDriver>() : nullptr;
            if (!driver)
                return srt::Error(srt::Error::FeatureNotSupported, "The ONNX inference driver is not registered on this SynthUnit");

            auto session = driver->createSession();
            if (!session)
                return srt::Error(SeparationError::ModelFailed, "The ONNX driver created no session");

            Onnx::SessionOpenArgs args;
            if (auto opened = session->open(configuration->model, args); !opened)
                return opened.takeError().withContext("Cannot open Demucs ONNX model");

            return std::unique_ptr<srt::InferenceExecutive>(new Executive(spec, std::move(session), *schema, *configuration));
        }
    };

    class Plugin final : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
        create(const srt::ContribContractTriplet &triplet) override {
            if (triplet != srt::ContribContractTriplet{Sep::Contract, Variant})
                return srt::Error(srt::Error::FeatureNotSupported, "This interpreter serves only Separation Level 1 variant demucs");

            return std::unique_ptr<srt::ContribInterpreter>(new Interpreter);
        }
    };
}

STDC_EXPORT_PLUGIN(Plugin)
