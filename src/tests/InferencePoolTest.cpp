#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>

#include <gtest/gtest.h>
#include <synthrt/Core/ContribImportBinding.h>
#include <wolf/Api/Inferences/G2p/1/G2pApiL1.h>

#include <dsspinference/Environment.h>
#include <dssptests/TestLanguagePackage.h>

namespace dssp::tests {
    using namespace std::chrono_literals;
    namespace G2P = wolf::Api::G2p::L1;
    using G2pResult = inference::ExecutionResult<std::unique_ptr<G2P::Result>>;

    namespace {
        struct ExecutionControl {
            std::mutex mutex;
            std::condition_variable wake;
            std::vector<std::string> starts;
            unsigned active = 0;
            unsigned maximumActive = 0;
            std::atomic<unsigned> created = 0;
            std::atomic<unsigned> stopped = 0;
            bool block = false;
            bool released = false;
        };

        // A native contract test double lets cancellation races be controlled
        // without sleeps, external models, or edits to any upstream library.
        class ControlledExecutive final : public G2P::Executive {
        public:
            ControlledExecutive(srt::InferenceSpec &spec, std::shared_ptr<ExecutionControl> control) : G2P::Executive(spec), m_control(std::move(control)) { ++m_control->created; }

            srt::Expected<void> initialize(const G2P::InitArgs &) override { return {}; }

            srt::Expected<std::unique_ptr<G2P::Result>> start(const G2P::StartInput &input) override {
                m_state = srt::ITask::Running;
                if (input.lyrics.front() == "throw") {
                    throw std::runtime_error("simulated native failure");
                }

                {
                    std::unique_lock lock(m_control->mutex);
                    ++m_control->active;
                    m_control->maximumActive = std::max(m_control->maximumActive, m_control->active);
                    m_control->starts.push_back(input.lyrics.front());
                    m_control->wake.notify_all();
                    if (m_control->block) {
                        m_control->wake.wait_for(lock, 5s, [&] { return m_stopped || m_control->released; });
                    }
                    --m_control->active;
                }

                auto result = std::make_unique<G2P::Result>();
                for (const auto &lyric : input.lyrics) {
                    G2P::WordOutput word;
                    word.pronunciation = lyric;
                    result->words.push_back(std::move(word));
                }

                m_state = m_stopped ? srt::ITask::Canceled : srt::ITask::Succeeded;
                return result;
            }

            srt::Expected<void> startAsync(std::shared_ptr<const G2P::StartInput> input, AsyncCallback complete) override {
                complete(start(*input));
                return {};
            }

            srt::ITask::State state() const noexcept override { return m_state; }

            srt::Expected<void> quit() override {
                {
                    const std::lock_guard lock(m_control->mutex);
                    m_stopped = true;
                }

                ++m_control->stopped;
                m_control->wake.notify_all();
                return {};
            }

            srt::Expected<void> wait() override { return {}; }

        private:
            std::shared_ptr<ExecutionControl> m_control;
            std::atomic<srt::ITask::State> m_state = srt::ITask::Idle;
            std::atomic<bool> m_stopped = false;
        };

        G2pResult awaitResult(std::future<G2pResult> result) {
            const auto ready = result.wait_for(5s);
            EXPECT_EQ(ready, std::future_status::ready);
            if (ready != std::future_status::ready) {
                return fail<inference::ExecutionErrors>(inference::ExecutionError("Test inference timed out"));
            }
            return result.get();
        }
    }

    TEST(InferenceWorkers, RepeatedShutdownWakesIdleWorkers) {
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            SCOPED_TRACE(iteration);
            const auto created = inference::InferencePool::create("test-shutdown", {1, 0ms, 60s});
            ASSERT_TRUE(created) << (created ? "" : created.error().formatChain());
            (*created)->shutdown();
        }
    }

    class InferencePoolTest : public ::testing::Test {
    protected:
        void SetUp() override {
            writeLanguagePackage(m_files, "one");
            writeLanguagePackage(m_files, "two", {.version = "2"});
            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            options.pluginPaths = {{"inference", plugins / "wolf" / "inferenceinterpreters"}, {"linguist", plugins / "wolf" / "linguistproviders"}};
            options.cacheGroups = {{"diffsinger-pronunciation", {1, 0ms, 60s}}, {"diffsinger-acoustic", {2, 0ms, 60s}}};
            const auto created = inference::Environment::create(options);
            ASSERT_TRUE(created) << (created ? "" : created.error().formatChain());
            m_host = *created;
            ASSERT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().error.formatChain());
            ASSERT_EQ(m_host->packages().size(), 2U);
            const auto group = m_host->inferences("diffsinger-pronunciation");
            ASSERT_TRUE(group);
            m_pool = *group;
            m_spec = dynamic_cast<srt::InferenceSpec *>(m_host->packages().front().contribution("inference", "g2p"));
            ASSERT_NE(m_spec, nullptr);
        }

        void TearDown() override {
            if (m_host) {
                m_host->shutdown();
            }
        }

        inference::InferenceHandle controlled(std::string key, const std::shared_ptr<ExecutionControl> &control, inference::InferencePool *group = nullptr, bool parallelSafe = false) {
            auto definition = inference::InferenceDefinition{key, key, {m_spec->package()}, [spec = m_spec, control]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                                                                 return std::make_unique<ControlledExecutive>(*spec, control);
                                                             }};
            definition.parallelSafe = parallelSafe;
            const auto handle = (group ? group : m_pool)->bind(std::move(definition));
            EXPECT_TRUE(handle);
            return handle ? *handle : inference::InferenceHandle{};
        }

        std::future<G2pResult> submit(inference::InferenceHandle handle, std::string lyric, std::stop_token stop = {}, inference::InferencePool *group = nullptr) {
            auto input = std::make_shared<G2P::StartInput>();
            input->lyrics.push_back(std::move(lyric));
            auto promise = std::make_shared<std::promise<G2pResult>>();
            auto result = promise->get_future();
            (group ? group : m_pool)->run<G2P::Executive>(std::move(handle), stop, [input](auto &executive) { return executive.start(*input); }, [promise](auto value) { promise->set_value(std::move(value)); });
            return result;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_host;
        inference::InferencePool *m_pool = nullptr;
        srt::InferenceSpec *m_spec = nullptr;
    };

    TEST_F(InferencePoolTest, QueueIsSharedByIdentityAndBothQueuedAndRunningCancellationAreIsolated) {
        auto control = std::make_shared<ExecutionControl>();
        control->block = true;
        const auto handle = controlled("shared", control);
        EXPECT_EQ(controlled("shared", control), handle);
        std::stop_source firstStop, secondStop;
        auto first = submit(handle, "first", firstStop.get_token());
        bool started = false;
        {
            std::unique_lock lock(control->mutex);
            started = control->wake.wait_for(lock, 5s, [&] { return !control->starts.empty(); });
        }

        EXPECT_TRUE(started);
        auto second = submit(handle, "second", secondStop.get_token());
        auto third = submit(handle, "third");
        secondStop.request_stop();
        const auto queued = awaitResult(std::move(second));
        ASSERT_FALSE(queued);
        EXPECT_NE(queued.error().get_if<inference::CancelledError>(), nullptr);
        firstStop.request_stop();
        {
            const std::lock_guard lock(control->mutex);
            control->released = true;
        }

        control->wake.notify_all();
        const auto running = awaitResult(std::move(first));
        ASSERT_FALSE(running);
        EXPECT_NE(running.error().get_if<inference::CancelledError>(), nullptr);
        const auto successor = awaitResult(std::move(third));
        ASSERT_TRUE(successor);
        EXPECT_EQ(control->starts, (std::vector<std::string>{"first", "third"}));
        EXPECT_EQ(control->maximumActive, 1U);
        EXPECT_EQ(control->stopped.load(), 1U);
        EXPECT_EQ(control->created.load(), 2U);
    }

    TEST_F(InferencePoolTest, NativeExceptionsBecomeExpectedErrorsAndFaultedInstancesAreRecreated) {
        const auto control = std::make_shared<ExecutionControl>();
        const auto handle = controlled("exception", control);
        const auto failed = awaitResult(submit(handle, "throw"));
        ASSERT_FALSE(failed);
        const auto *error = failed.error().get_if<inference::ExecutionError>();
        ASSERT_NE(error, nullptr);
        ASSERT_NE(error->cause(), nullptr);
        EXPECT_NE(error->formatChain().find("simulated native failure"), std::string::npos);
        EXPECT_EQ(m_pool->residentCount(), 0U);
        const auto next = awaitResult(submit(handle, "next"));
        ASSERT_TRUE(next);
        EXPECT_EQ(control->created.load(), 2U);
    }

    TEST_F(InferencePoolTest, CacheGroupsApplyIndependentCapacitiesAndRejectForeignHandles) {
        const auto synthesis = m_host->inferences("diffsinger-acoustic");
        ASSERT_TRUE(synthesis);
        const auto control = std::make_shared<ExecutionControl>();
        const auto one = controlled("one", control);
        const auto two = controlled("two", control);
        ASSERT_TRUE(awaitResult(submit(one, "one")));
        ASSERT_TRUE(awaitResult(submit(two, "two")));
        EXPECT_EQ(m_pool->residentCount(), 1U);
        const auto synthOne = controlled("one", control, *synthesis);
        const auto synthTwo = controlled("two", control, *synthesis);
        ASSERT_TRUE(awaitResult(submit(synthOne, "one", {}, *synthesis)));
        ASSERT_TRUE(awaitResult(submit(synthTwo, "two", {}, *synthesis)));
        EXPECT_EQ((*synthesis)->residentCount(), 2U);
        EXPECT_EQ(m_pool->residentCount(), 1U);
        EXPECT_EQ(control->created.load(), 4U);
        ASSERT_TRUE(awaitResult(submit(one, "one")));
        EXPECT_EQ(control->created.load(), 5U);
        EXPECT_EQ(controlled("one", control), one);
        const auto wrongGroup = awaitResult(submit(one, "wrong", {}, *synthesis));
        ASSERT_FALSE(wrongGroup);
        EXPECT_NE(wrongGroup.error().get_if<inference::ExecutionError>(), nullptr);
    }

    TEST_F(InferencePoolTest, RealImportBindingsReuseTheSameExecutiveAndKeepVersionsAndRuntimeBindingsDistinct) {
        const auto imported = m_host->packages().front().contribution("linguist", "eng")->findImport("linguist/g2p");
        ASSERT_TRUE(imported);
        auto options = std::make_shared<G2P::RuntimeOptions>();
        options->binding = {"eng", "test"};
        const srt::JsonValue effective(srt::JsonObject{{"language", "eng"}, {"scheme", "test"}});
        const auto one = m_pool->bindImport<G2P::Executive, G2P::InitArgs>(*imported, options, effective);
        ASSERT_TRUE(one);
        const auto repeated = m_pool->bindImport<G2P::Executive, G2P::InitArgs>(*imported, options, effective);
        ASSERT_TRUE(repeated);
        EXPECT_EQ(*one, *repeated);
        auto changedOptions = std::make_shared<G2P::RuntimeOptions>();
        changedOptions->binding = {"eng", "other"};
        const auto changed = m_pool->bindImport<G2P::Executive, G2P::InitArgs>(*imported, changedOptions, srt::JsonObject{{"language", "eng"}, {"scheme", "other"}});
        ASSERT_TRUE(changed);
        EXPECT_NE(*changed, *one);
        const auto otherImport = m_host->packages().back().contribution("linguist", "eng")->findImport("linguist/g2p");
        ASSERT_TRUE(otherImport);
        const auto otherVersion = m_pool->bindImport<G2P::Executive, G2P::InitArgs>(*otherImport, options, effective);
        ASSERT_TRUE(otherVersion);
        EXPECT_NE(*otherVersion, *one);
        EXPECT_EQ(m_pool->residentCount(), 0U);
        const auto result = awaitResult(submit(*one, "read"));
        ASSERT_TRUE(result) << (result ? "" : result.error().asError().formatChain());
        ASSERT_EQ((*result)->words.size(), 1U);
        EXPECT_EQ((*result)->words.front().pronunciation, "b a b i");
        EXPECT_EQ(m_pool->residentCount(), 1U);
    }

    TEST_F(InferencePoolTest, ShutdownCancelsAcceptedWorkAndRejectsSubsequentBindingsAndTasks) {
        const auto control = std::make_shared<ExecutionControl>();
        control->block = true;
        const auto handle = controlled("shutdown", control);
        auto first = submit(handle, "running");
        {
            std::unique_lock lock(control->mutex);
            EXPECT_TRUE(control->wake.wait_for(lock, 5s, [&] { return !control->starts.empty(); }));
        }

        auto queued = submit(handle, "queued");
        m_host->shutdown();
        EXPECT_FALSE(awaitResult(std::move(first)));
        EXPECT_FALSE(awaitResult(std::move(queued)));
        EXPECT_FALSE(awaitResult(submit(handle, "after-shutdown")));
        EXPECT_FALSE(m_host->inferences("diffsinger-pronunciation"));
        EXPECT_EQ(m_pool->residentCount(), 0U);
    }

    TEST_F(InferencePoolTest, SharedAdmissionCoversGroupsWithoutBlockingWorkersAndAllowsWhitelistedTasks) {
        const auto createdGate = resource::ExecutionGate::create("test-shared-admission");
        ASSERT_TRUE(createdGate);
        const auto gate = *createdGate;
        auto firstGroup = inference::InferencePool::create("test-neural-one", {2, 0ms, 60s}, gate);
        auto secondGroup = inference::InferencePool::create("test-neural-two", {2, 0ms, 60s}, gate);
        ASSERT_TRUE(firstGroup);
        ASSERT_TRUE(secondGroup);
        const auto heldPromise = std::make_shared<std::promise<resource::PermitResult>>();
        auto admitted = heldPromise->get_future();
        drogon::async_run([gate, heldPromise]() -> drogon::Task<> { heldPromise->set_value(co_await gate->acquire()); });
        ASSERT_EQ(admitted.wait_for(1s), std::future_status::ready);
        std::optional<resource::PermitResult> held(admitted.get());
        ASSERT_TRUE(*held);

        const auto control = std::make_shared<ExecutionControl>();
        std::vector<std::future<G2pResult>> pending;
        std::vector<std::future<void>> loaded;
        std::stop_source stop;
        for (unsigned i = 0; i < 6; ++i) {
            auto *group = i < 5 ? firstGroup->get() : secondGroup->get();
            const auto created = std::make_shared<std::promise<void>>();
            loaded.push_back(created->get_future());
            const auto key = "gated-" + std::to_string(i);
            inference::InferenceDefinition definition{key, key, {m_spec->package()}, [spec = m_spec, control, created]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                                                          auto executive = std::make_unique<ControlledExecutive>(*spec, control);
                                                          created->set_value();
                                                          return executive;
                                                      }};
            const auto resource = group->bind(std::move(definition));
            ASSERT_TRUE(resource);
            pending.push_back(submit(*resource, key, i == 0 ? stop.get_token() : std::stop_token{}, group));
        }

        // There are only four workers in the first group. All five resources
        // must nevertheless initialize while their executions await admission.
        for (auto &initialized : loaded)
            EXPECT_EQ(initialized.wait_for(1s), std::future_status::ready);
        for (auto &result : pending)
            EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);
        stop.request_stop();
        EXPECT_FALSE(awaitResult(std::move(pending.front())));
        const auto safe = controlled("whitelisted", control, firstGroup->get(), true);
        EXPECT_TRUE(awaitResult(submit(safe, "whitelisted", {}, firstGroup->get())));
        held.reset();
        for (std::size_t i = 1; i < pending.size(); ++i)
            EXPECT_TRUE(awaitResult(std::move(pending[i])));
        EXPECT_EQ(control->maximumActive, 1U);
        EXPECT_EQ(control->starts.size(), 6U);
        firstGroup->get()->shutdown();
        secondGroup->get()->shutdown();
        gate->shutdown();
    }
}
