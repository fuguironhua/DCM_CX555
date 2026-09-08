#pragma once

#include "framework.h"
#include <cstdint>

// The dialog owns one schedule for the current task palette. Each window's
// TaskScript takes a snapshot and keeps its own position in that sequence.
class CTaskSchedule final {
public:
    struct Snapshot {
        std::vector<int> taskIds;
        std::uint64_t generation = 0;
        bool running = false;
    };

    void Start(std::vector<int> taskIds) {
        std::scoped_lock lock(m_mutex);
        m_taskIds = std::move(taskIds);
        m_running = true;
        ++m_generation;
    }

    void Stop() {
        std::scoped_lock lock(m_mutex);
        m_running = false;
        ++m_generation;
    }

    bool IsRunning() const {
        std::scoped_lock lock(m_mutex);
        return m_running;
    }

    Snapshot Read() const {
        std::scoped_lock lock(m_mutex);
        return Snapshot{ m_taskIds, m_generation, m_running };
    }

private:
    mutable std::mutex m_mutex;
    std::vector<int> m_taskIds;
    std::uint64_t m_generation = 0;
    bool m_running = false;
};

using TaskSchedulePtr = std::shared_ptr<CTaskSchedule>;
