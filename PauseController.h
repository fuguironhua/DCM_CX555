#pragma once
#include "framework.h"

class CThreadControl final {
public:
    bool WaitIfPaused() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_paused.load() || m_stop.load(); });
        return !m_stop.load();
    }
    bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, timeout, [this] { return m_stop.load() || m_paused.load(); });
        return !m_stop.load();
    }
    void Pause() { m_paused.store(true); }
    void Resume() { m_paused.store(false); m_cv.notify_all(); }
    void Stop() { m_stop.store(true); m_paused.store(false); m_cv.notify_all(); }
    bool IsPaused() const { return m_paused.load(); }
    bool IsStopping() const { return m_stop.load(); }
private:
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_paused{ false };
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
};
