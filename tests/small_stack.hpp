#ifndef NEOJSON_TESTS_SMALL_STACK_HPP_
#define NEOJSON_TESTS_SMALL_STACK_HPP_

#include <cstddef>
#include <exception>
#include <functional>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace neojson::test {

// Runs `fn` on a new thread whose stack is `stack_bytes` large and waits for it. An exception that leaves `fn` is
// returned as its message (empty when there was none), so that the caller can report it as a test failure.
// Assertions made inside `fn` are reported by GoogleTest as usual.
inline std::string run_with_stack(std::size_t stack_bytes, std::function<void()> fn) {
    struct Job {
        std::function<void()> fn;
        std::string error;
    } job{std::move(fn), {}};

    auto body = [](Job* j) {
        try {
            j->fn();
        } catch (const std::exception& e) {
            j->error = std::string("exception: ") + e.what();
        } catch (...) {
            j->error = "unknown exception";
        }
    };

#if defined(_WIN32)
    struct Start {
        Job* job;
        decltype(body) run;
    } start{&job, body};
    auto entry = [](void* p) -> unsigned {
        auto* s = static_cast<Start*>(p);
        s->run(s->job);
        return 0;
    };
    // The stack size argument of _beginthreadex is the committed-and-reserved size of the new thread's stack.
    HANDLE h = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, static_cast<unsigned>(stack_bytes), entry, &start, 0, nullptr));
    if (h == nullptr) {
        return "could not start the thread";
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
#else
    struct Start {
        Job* job;
        decltype(body) run;
    } start{&job, body};
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return "pthread_attr_init failed";
    }
    if (pthread_attr_setstacksize(&attr, stack_bytes) != 0) {
        pthread_attr_destroy(&attr);
        return "pthread_attr_setstacksize failed";
    }
    pthread_t t;
    auto entry = [](void* p) -> void* {
        auto* s = static_cast<Start*>(p);
        s->run(s->job);
        return nullptr;
    };
    const int rc = pthread_create(&t, &attr, entry, &start);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        return "pthread_create failed";
    }
    pthread_join(t, nullptr);
#endif
    return job.error;
}

} // namespace neojson::test

#endif // NEOJSON_TESTS_SMALL_STACK_HPP_
