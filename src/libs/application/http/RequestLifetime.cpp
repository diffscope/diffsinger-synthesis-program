#include "RequestLifetime.h"

#include <vector>

#include <stdcorelib/support/logging.h>

namespace dssp::http {
    namespace {
        stdc::LogCategory log("dssp.http.RequestLifetime");
    }

    struct RequestContext::State {
        std::stop_source source;
        ResponseContext response;
        std::weak_ptr<RequestLifetimeRegistry> registry;
        std::uint64_t registrationId = 0;

        ~State() {
            if (const auto owner = registry.lock()) {
                owner->release(registrationId);
            }
        }
    };

    std::uint64_t RequestContext::id() const noexcept {
        return logContext().id();
    }

    const RequestLog &RequestContext::logContext() const noexcept {
        return responseContext().log;
    }

    std::stop_token RequestContext::cancellation() const noexcept {
        return m_state ? m_state->source.get_token() : std::stop_token{};
    }

    bool RequestContext::stopRequested() const noexcept {
        return cancellation().stop_requested();
    }

    const ResponseContext &RequestContext::responseContext() const noexcept {
        static const ResponseContext empty;
        return m_state ? m_state->response : empty;
    }

    ResponseContext RequestLifetimeRegistry::responseContext(const drogon::HttpRequest &request) const {
        return {requestLog(request), localization::Context(m_translations, request.getHeader("accept-language"))};
    }

    RequestContext RequestLifetimeRegistry::track(const drogon::HttpRequest &request) {
        RequestContext context;
        context.m_state = std::make_shared<RequestContext::State>();
        context.m_state->response = responseContext(request);
        context.m_state->registry = weak_from_this();

        bool stop = false;
        {
            std::lock_guard lock(m_mutex);
            context.m_state->registrationId = ++m_nextId;
            const auto connection = request.getConnectionPtr();
            const auto live = connection.lock();
            const std::weak_ptr<trantor::TcpConnection> empty;
            const bool hasConnection = connection.owner_before(empty) || empty.owner_before(connection);
            stop = m_closing || (live ? !live->connected() : hasConnection);
            if (!stop) {
                m_requests.emplace(m_nextId, Entry{connection, context.m_state});
            }
        }

        if (stop) {
            context.m_state->source.request_stop();
        }

        return context;
    }

    void RequestLifetimeRegistry::release(std::uint64_t registrationId) {
        std::lock_guard lock(m_mutex);
        m_requests.erase(registrationId);
    }

    void RequestLifetimeRegistry::connectionChanged(const trantor::TcpConnectionPtr &connection) {
        if (connection->connected()) {
            return;
        }

        std::vector<std::shared_ptr<RequestContext::State>> states;
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_requests.begin(); it != m_requests.end();) {
                if (it->second.connection.lock() == connection) {
                    if (auto state = it->second.state.lock()) {
                        states.push_back(std::move(state));
                    }
                    it = m_requests.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Stop callbacks may synchronously release their registration. Never call
        // request_stop or destroy the last request state while holding this mutex.
        for (auto &state : states) {
            log.stdcTrace("%1 cancelling backend work after client disconnect", state->response.log.description());
            state->source.request_stop();
        }
    }

    void RequestLifetimeRegistry::cancelAll() {
        std::vector<std::shared_ptr<RequestContext::State>> states;
        {
            std::lock_guard lock(m_mutex);
            m_closing = true;
            for (const auto &[id, entry] : m_requests) {
                if (auto state = entry.state.lock()) {
                    states.push_back(std::move(state));
                }
            }
            m_requests.clear();
        }

        for (auto &state : states) {
            log.stdcTrace("%1 cancelling backend work during server shutdown", state->response.log.description());
            state->source.request_stop();
        }
    }
}
