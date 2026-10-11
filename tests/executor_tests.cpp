// Phase 6 stackful executor tests. Verifies the baton discipline (one
// participant runs at a time), deterministic interleaving, run/yield/resume,
// and clean shutdown of a never-finishing (parked) participant.

#include "jfg/boot/executor.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace jfg::boot;

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// Runs a two-participant cooperative scenario and returns the interleaving log.
// Because only the baton holder runs, the log is fully determined by the
// scheduler's resume order.
std::vector<std::string> run_scenario() {
    std::vector<std::string> log;
    BatonExecutor executor;

    // Participant A: log a0, yield, log a1, yield, finish.
    const int a = executor.spawn([&log](BatonExecutor& ex, int) {
        log.push_back("a0");
        ex.yield_to_scheduler();
        log.push_back("a1");
        ex.yield_to_scheduler();
        log.push_back("a2");
    });
    // Participant B: log b0, yield, log b1, finish.
    const int b = executor.spawn([&log](BatonExecutor& ex, int) {
        log.push_back("b0");
        ex.yield_to_scheduler();
        log.push_back("b1");
    });

    // Deterministic scheduler order: A, B, A, B, A.
    check(!executor.resume(a), "A not finished after first slice");
    check(!executor.resume(b), "B not finished after first slice");
    check(!executor.resume(a), "A not finished after second slice");
    check(executor.resume(b), "B finished after second slice");
    check(executor.resume(a), "A finished after third slice");
    return log;
}

void test_deterministic_interleaving() {
    const std::vector<std::string> first = run_scenario();
    const std::vector<std::string> second = run_scenario();
    const std::vector<std::string> expected = {
        "a0", "b0", "a1", "b1", "a2"};
    check(first == expected, "interleaving matches the scheduler order");
    check(first == second, "interleaving is deterministic across runs");
}

void test_single_runner_no_overlap() {
    // If two participants ever ran concurrently, this unguarded counter would
    // race; under the baton it cannot. We increment heavily while yielding.
    BatonExecutor executor;
    long counter = 0;
    const int a = executor.spawn([&counter](BatonExecutor& ex, int) {
        for (int i = 0; i < 1000; ++i) {
            ++counter;
            if ((i % 100) == 0) {
                ex.yield_to_scheduler();
            }
        }
    });
    const int b = executor.spawn([&counter](BatonExecutor& ex, int) {
        for (int i = 0; i < 1000; ++i) {
            ++counter;
            if ((i % 100) == 0) {
                ex.yield_to_scheduler();
            }
        }
    });
    bool a_done = false;
    bool b_done = false;
    // Drive both to completion, alternating.
    for (int round = 0; round < 100 && !(a_done && b_done); ++round) {
        if (!a_done) {
            a_done = executor.resume(a);
        }
        if (!b_done) {
            b_done = executor.resume(b);
        }
    }
    check(a_done && b_done, "both participants finished");
    check(counter == 2000, "no lost increments — exactly one ran at a time");
}

void test_shutdown_unwinds_parked_participant() {
    // A participant that yields forever must be unwound cleanly by shutdown
    // (no hang, thread joined), not leak or deadlock.
    bool reached_after_loop = false;
    {
        BatonExecutor executor;
        const int spinner = executor.spawn(
            [&reached_after_loop](BatonExecutor& ex, int) {
                while (true) {
                    ex.yield_to_scheduler();
                }
                // Unreachable; present to prove we do NOT fall through here.
                reached_after_loop = true;  // NOLINT
            });
        check(!executor.resume(spinner), "spinner yields, not finished");
        check(!executor.resume(spinner), "spinner still yielding");
        // executor destructor runs shutdown(): must unwind and join.
    }
    check(!reached_after_loop, "spinner body did not run past its wait loop");
    check(true, "shutdown completed without hanging");
}

void test_never_started_participant_shuts_down() {
    // A spawned-but-never-resumed participant must still join on shutdown.
    BatonExecutor executor;
    (void)executor.spawn([](BatonExecutor&, int) {
        // never resumed before shutdown
    });
    check(executor.participant_count() == 1U, "participant registered");
    // Destructor shutdown() must not hang joining the never-resumed thread.
}

// RGO fiber backend: a host thread_local registered as a fiber local keeps
// one value per participant, though all of them run on one OS thread.
thread_local void* t_test_local = nullptr;
void* swap_test_local(void* value) noexcept {
    void* previous = t_test_local;
    t_test_local = value;
    return previous;
}

void test_fiber_locals() {
    check(register_fiber_local(&swap_test_local), "fiber local registered");
    check(register_fiber_local(&swap_test_local), "registering twice is fine");
    BatonExecutor executor;
    int a_marker = 0, b_marker = 0, scheduler_marker = 0;
    t_test_local = &scheduler_marker;
    bool a_ok = true, b_ok = true;
    const int a = executor.spawn([&](BatonExecutor& ex, int) {
        t_test_local = &a_marker;
        ex.yield_to_scheduler();
        a_ok = t_test_local == &a_marker;
    });
    const int b = executor.spawn([&](BatonExecutor& ex, int) {
        b_ok = t_test_local == nullptr;  // a new participant starts empty
        t_test_local = &b_marker;
        ex.yield_to_scheduler();
        b_ok = b_ok && t_test_local == &b_marker;
    });
    (void)executor.resume(a);
    check(t_test_local == &scheduler_marker, "scheduler's value restored after A");
    (void)executor.resume(b);
    (void)executor.resume(a);
    (void)executor.resume(b);
    check(a_ok && b_ok, "each participant kept its own value");
    check(t_test_local == &scheduler_marker, "scheduler's value restored at the end");
    t_test_local = nullptr;
}

// Participants spawned while others are parked mid-body (the participant
// list grows and moves its elements), with captures large enough for
// std::function to keep on the heap and small enough to keep inline.
void test_spawn_while_parked() {
    BatonExecutor executor;
    std::vector<int> ids;
    std::vector<long> results;
    constexpr int kCount = 40;
    results.resize(kCount);
    for (int index = 0; index < kCount; ++index) {
        std::array<long, 16> big{};
        big[5] = index;
        ids.push_back(executor.spawn([&results, index, big](BatonExecutor& ex, int) {
            long total = big[5];
            for (int step = 0; step < 3; ++step) {
                ex.yield_to_scheduler();
                total += step;
            }
            results[static_cast<std::size_t>(index)] = total;
        }));
        (void)executor.resume(ids.back());
    }
    for (int round = 0; round < 3; ++round)
        for (const int id : ids) (void)executor.resume(id);
    bool all = true;
    for (int index = 0; index < kCount; ++index)
        all = all && results[static_cast<std::size_t>(index)] == index + 3;
    check(all, "every participant finished with its own captured state");
}

// Exceptions thrown and caught inside a participant, and a deep stack.
int recurse(int depth) {
    volatile char frame[1024];
    frame[0] = 1;
    return depth == 0 ? 0 : recurse(depth - 1) + frame[0];
}

void test_exceptions_and_deep_stack() {
    BatonExecutor executor;
    bool caught = false;
    int depth_result = -1;
    const int id = executor.spawn([&](BatonExecutor& ex, int) {
        try {
            ex.yield_to_scheduler();
            throw std::runtime_error("inside a participant");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        depth_result = recurse(4000);  // about 4 MB of stack
    });
    (void)executor.resume(id);
    check(executor.resume(id), "participant finished");
    check(caught, "exception caught inside the participant");
    check(depth_result == 4000, "a 4 MB deep call chain fits the stack");
}

void run_all(const ExecutorBackend backend, const char* name) {
    set_default_executor_backend(backend);
    const int before = failures;
    test_deterministic_interleaving();
    test_single_runner_no_overlap();
    test_shutdown_unwinds_parked_participant();
    test_never_started_participant_shuts_down();
    test_spawn_while_parked();
    test_exceptions_and_deep_stack();
    if (backend == ExecutorBackend::fibers)
        test_fiber_locals();
    BatonExecutor probe;
    check(probe.backend() == backend, "default backend applies");
    if (failures != before) std::cerr << "  (backend: " << name << ")\n";
}

} // namespace

int main() {
    run_all(ExecutorBackend::threads, "threads");
    run_all(ExecutorBackend::fibers, "fibers");
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all executor tests passed\n";
    return 0;
}
