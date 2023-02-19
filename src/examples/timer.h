#pragma once

#include <chrono>
#include <stdint.h>

class Timer 
{
private:
    std::chrono::steady_clock::time_point dt_start;
public:
    Timer() {
        dt_start = std::chrono::high_resolution_clock::now();
    } 

    template <typename T = std::chrono::microseconds>
    uint64_t get_delta() {
        const auto dt_end = std::chrono::high_resolution_clock::now();
        const auto dt_delta = dt_end - dt_start;
        return std::chrono::duration_cast<T>(dt_delta).count();
    }
};
