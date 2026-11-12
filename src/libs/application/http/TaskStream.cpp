#include "TaskStream.h"

#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <json/writer.h>
#include <stdcorelib/support/logging.h>
#include <trantor/net/EventLoop.h>

namespace dssp::http {
    namespace {
        stdc::LogCategory log("dssp.http.TaskStream");

        std::string eventLine(std::string_view name, const Json::Value *data = nullptr) {
            Json::Value event;
            event["event"] = std::string(name);
            if (data) {
                event["data"] = *data;
            }
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            writer["emitUTF8"] = true;
            return Json::writeString(writer, event) + '\n';
        }
    }

    struct TaskStream::State : std::enable_shared_from_this<State> {
        RequestContext context;
        std::string route;
        trantor::EventLoop *loop = nullptr;
        drogon::ResponseStreamPtr stream;
        std::vector<std::string> pending;
        std::optional<resource::TaskUpdate> current;
        trantor::TimerId heartbeat = trantor::InvalidTimerId;
        std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
        bool terminal = false;
        bool completedEvent = false;
        std::mutex dispatchMutex;
        bool deliveryStopped = false;

        // Cancellation closes dispatch before the server can destroy its event loops.
        void stopDelivery() {
            const std::lock_guard lock(dispatchMutex);
            deliveryStopped = true;
        }

        void close() {
            stopDelivery();
            terminal = true;
            pending.clear();
            if (heartbeat != trantor::InvalidTimerId) {
                loop->invalidateTimer(heartbeat);
                heartbeat = trantor::InvalidTimerId;
            }
            if (stream) {
                stream->close();
                stream.reset();
            }
        }

        bool send(std::string line) {
            if (context.stopRequested()) {
                close();
                return false;
            }
            if (!stream) {
                pending.push_back(std::move(line));
                return true;
            }
            if (!stream->send(line)) {
                close();
                return false;
            }
            return true;
        }

        void update(resource::TaskUpdate update) {
            if (terminal || context.stopRequested()) {
                return;
            }
            if (update.state == resource::TaskUpdate::State::Queue) {
                if (current) {
                    return;
                }
            } else {
                if (!std::isfinite(update.value) || update.value < 0 || update.value > 1) {
                    return;
                }
                if (current && current->state == resource::TaskUpdate::State::Progress && update.value <= current->value) {
                    return;
                }
            }
            current = update;
            repeat();
        }

        void repeat() {
            if (terminal || !current) {
                return;
            }
            if (current->state == resource::TaskUpdate::State::Queue) {
                send(eventLine("queue"));
            } else {
                Json::Value data;
                data["value"] = current->value;
                send(eventLine("progress", &data));
            }
        }

        void finish(std::string line) {
            if (terminal) {
                return;
            }
            if (!send(std::move(line))) {
                return;
            }
            terminal = true;
            completedEvent = true;
            stopDelivery();
            if (stream) {
                context.logContext().finishStream(route);
                close();
            } else if (heartbeat != trantor::InvalidTimerId) {
                loop->invalidateTimer(heartbeat);
                heartbeat = trantor::InvalidTimerId;
            }
        }

        template <class Function>
        void dispatch(Function function) {
            const std::lock_guard lock(dispatchMutex);
            if (deliveryStopped) {
                return;
            }
            loop->queueInLoop([self = shared_from_this(), function = std::move(function)] {
                try {
                    function(*self);
                } catch (const std::exception &error) {
                    log.stdcCritical("%1 stream delivery failed: %2", self->context.logContext().description(), quoteLogValue(error.what(), 8192));
                    self->close();
                }
            });
        }

        void cancel() {
            const std::lock_guard lock(dispatchMutex);
            if (deliveryStopped) {
                return;
            }
            deliveryStopped = true;
            loop->queueInLoop([self = shared_from_this()] { self->close(); });
        }
    };

    TaskStream::TaskStream(std::shared_ptr<State> state) : m_state(std::move(state)) {}

    std::shared_ptr<TaskStream> TaskStream::open(const drogon::HttpRequest &request, const Callback &callback, RequestContext context, std::chrono::seconds interval) {
        auto state = std::make_shared<State>();
        state->context = std::move(context);
        state->route = request.matchedPathPattern() == request.path() ? std::string{} : std::string(request.matchedPathPattern());
        const auto connection = request.getConnectionPtr().lock();
        state->loop = connection ? connection->getLoop() : drogon::app().getLoop();
        const auto weak = std::weak_ptr<State>(state);
        state->cancellation = std::make_unique<std::stop_callback<std::function<void()>>>(state->context.cancellation(), [weak] {
            if (const auto state = weak.lock()) {
                state->cancel();
            }
        });
        auto response = drogon::HttpResponse::newAsyncStreamResponse([state, interval, weak](drogon::ResponseStreamPtr stream) {
            // Drogon invokes this callback on the connection event loop.
            state->stream = std::move(stream);
            if (state->context.stopRequested()) {
                state->close();
                return;
            }
            auto pending = std::move(state->pending);
            for (auto &line : pending) {
                if (!state->send(std::move(line))) {
                    return;
                }
            }
            if (state->terminal) {
                if (state->completedEvent) {
                    state->context.logContext().finishStream(state->route);
                }
                state->close();
                return;
            }
            state->heartbeat = state->loop->runEvery(interval, [weak] {
                if (const auto state = weak.lock()) {
                    try {
                        state->repeat();
                    } catch (const std::exception &error) {
                        log.stdcCritical("%1 stream heartbeat failed: %2", state->context.logContext().description(), quoteLogValue(error.what(), 8192));
                        state->close();
                    }
                }
            });
        }, true);
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("application/x-ndjson; charset=utf-8");
        response->addHeader("vary", "Accept, Accept-Language");
        response->addHeader("cache-control", "no-store");
        response->addHeader("x-accel-buffering", "no");
        state->context.logContext().beginStream();
        callback(response);
        return std::shared_ptr<TaskStream>(new TaskStream(std::move(state)));
    }

    resource::TaskProgress TaskStream::progress() const {
        return resource::TaskProgress([state = m_state](resource::TaskUpdate update) {
            state->dispatch([update](State &state) { state.update(update); });
        });
    }

    void TaskStream::finish(const drogon::HttpResponsePtr &response) const {
        if (m_state->context.stopRequested()) {
            return;
        }
        std::string line;
        try {
            const auto data = response->getJsonObject();
            if (data) {
                line = eventLine(response->statusCode() == drogon::k200OK ? "result" : "error", data.get());
            }
        } catch (const std::exception &error) {
            m_state->context.logContext().problem("serialization", "Response serialization failed");
            log.stdcCritical("%1 stream serialization failed: %2", m_state->context.logContext().description(), quoteLogValue(error.what(), 8192));
        }
        if (line.empty()) {
            line = "{\"event\":\"error\",\"data\":" + serializationProblemJson(m_state->context.responseContext()) + "}\n";
        }
        m_state->dispatch([line = std::move(line)](State &state) { state.finish(line); });
    }
}
