// DRAM read-bandwidth measurement, in its own translation unit so it can be
// forced to -O2 (see CMakeLists.txt).
//
// WHY A SEPARATE FILE: the app's native build is CMAKE_BUILD_TYPE=Debug, i.e.
// `-O0 -g`. An unoptimized read loop is not vectorized or unrolled, so it
// measures loop overhead rather than the memory controller -- the first version
// of this probe reported 8.2 GB/s while llama.cpp's decode was demonstrably
// sustaining ~14.3 GB/s, which is the contradiction that exposed the bug. Only
// this loop needs the optimizer, and giving the whole NAPI bridge -O2 would
// change how the rest of the app debugs, so the scope is one file.
//
// clang has no working per-function `optimize("O2")` attribute (it parses and
// ignores it), so the optimization level has to come from the build system.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// Fill the buffer so every page is faulted in. A fresh anonymous mapping is
// copy-on-write zero pages; faulting them during the timed loop would measure
// the page-fault path instead of DRAM.
extern "C" void mirobody_membw_touch(std::uint64_t* buf, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) buf[i] = i;
}

// Sequential read of the whole buffer split across `threads`, returned as GB/s.
// One thread cannot saturate a modern memory controller, so the caller is
// expected to report both a 1-thread and an n-thread figure.
extern "C" double mirobody_membw_read_gbs(const std::uint64_t* buf, std::size_t n, int threads) {
    if (threads < 1) threads = 1;
    std::atomic<std::uint64_t> sink{0};

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([buf, n, threads, t, &sink] {
            const std::size_t lo = n * static_cast<std::size_t>(t) / threads;
            const std::size_t hi = n * static_cast<std::size_t>(t + 1) / threads;
            // Four independent accumulators: a single chain would serialize on
            // the add latency and cap throughput below what the loads can do.
            std::uint64_t a = 0, b = 0, c = 0, d = 0;
            std::size_t i = lo;
            for (; i + 4 <= hi; i += 4) {
                a += buf[i]; b += buf[i + 1]; c += buf[i + 2]; d += buf[i + 3];
            }
            for (; i < hi; ++i) a += buf[i];
            sink.fetch_add(a + b + c + d, std::memory_order_relaxed);  // defeat DCE
        });
    }
    for (auto& th : pool) th.join();
    const double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    const double bytes = static_cast<double>(n) * sizeof(std::uint64_t);
    return sec > 0 ? bytes / sec / 1e9 : 0.0;
}
