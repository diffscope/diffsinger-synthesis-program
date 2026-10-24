#ifndef DSSP_INFERENCE_TASKEXECUTIVE_H
#define DSSP_INFERENCE_TASKEXECUTIVE_H

#include <functional>
#include <memory>
#include <utility>

#include <synthrt/Core/ContribExecutive.h>
#include <synthrt/Task/ITask.h>

#include <dsspinference/NativeCall.h>

namespace dssp::inference {
    // The resource adapter owns a native task without imposing an inference
    // category on it. FIFO and LRU remain in the independent resource library.
    class TaskExecutive final {
    public:
        template <class Executive>
        explicit TaskExecutive(std::unique_ptr<Executive> executive)
            : m_executive(std::move(executive)),
              m_state([task = static_cast<Executive *>(m_executive.get())] { return task->state(); }),
              m_stop([task = static_cast<Executive *>(m_executive.get())] { return task->stop(); }),
              m_wait([task = static_cast<Executive *>(m_executive.get())] { return task->waitForFinished(); }) {}

        srt::ContribExecutive &native() const noexcept { return *m_executive; }
        srt::ITask::State state() const noexcept { return m_state(); }
        std::expected<void, ExecutionError> stop() { return nativeCall("Native task::stop", m_stop); }
        std::expected<void, ExecutionError> waitForFinished() { return nativeCall("Native task::waitForFinished", m_wait); }

    private:
        std::unique_ptr<srt::ContribExecutive> m_executive;
        std::function<srt::ITask::State()> m_state;
        std::function<srt::Expected<void>()> m_stop;
        std::function<srt::Expected<void>()> m_wait;
    };
}

#endif // DSSP_INFERENCE_TASKEXECUTIVE_H
