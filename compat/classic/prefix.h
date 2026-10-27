// Force-included in every engine source when building for Mac OS 9 (Retro68).
#pragma once
// int32_t is plain int in this build (see CMakeLists.txt); make the limit
// macros agree so `auto x = INT32_MIN` has the same type.
#include <stdint.h>
#undef INT32_MIN
#undef INT32_MAX
#undef UINT32_MAX
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define OPENRCT2_ARCHITECTURE "PowerPC"
#define OPENRCT2_PLATFORM "Mac OS 9"
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#ifdef __cplusplus
    #include <atomic>
    #include <cstdint>
    #include <ctime>
    #include <functional>
    #include <future>
    #include <mutex>

// A steady clock on the Toolbox microsecond timer; the C library clock on this
// platform is too coarse for the game loop.
    #include <chrono>
struct ClassicClock
{
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<ClassicClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept;
};

// std::random_device has no entropy source on this platform and ends the
// program when used; seed from the microsecond timer instead.
    #include <random>
namespace std
{
    struct ClassicRandomDevice
    {
        using result_type = unsigned int;
        static constexpr result_type min()
        {
            return 0;
        }
        static constexpr result_type max()
        {
            return 0xFFFFFFFFu;
        }
        result_type operator()() noexcept
        {
            static result_type state = 0x9E3779B9u;
            state ^= static_cast<result_type>(ClassicClock::now().time_since_epoch().count());
            state = state * 1664525u + 1013904223u;
            return state;
        }
    };
} // namespace std
    #define random_device ClassicRandomDevice

// The Classic toolchain has no thread support. The game runs on one
// cooperative thread, so locks are no-ops. Code that started worker threads
// is replaced by synchronous versions in compat/classic.
namespace std
{
    class mutex
    {
    public:
        constexpr mutex() noexcept = default;
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;
        void lock() noexcept
        {
        }
        bool try_lock() noexcept
        {
            return true;
        }
        void unlock() noexcept
        {
        }
    };
    using recursive_mutex = mutex;

    class condition_variable
    {
    public:
        void notify_one() noexcept
        {
        }
        void notify_all() noexcept
        {
        }
        template<typename Lock>
        void wait(Lock&) noexcept
        {
        }
        template<typename Lock, typename Predicate>
        void wait(Lock&, Predicate predicate)
        {
            // Nothing else can change the condition; it must already hold.
            (void)predicate();
        }
    };


    // Futures complete immediately: std::async runs its function inline.
    template<typename T>
    class future
    {
    public:
        future() noexcept = default;
        explicit future(bool valid) noexcept
            : _valid(valid)
        {
        }
        bool valid() const noexcept
        {
            return _valid;
        }
        void wait() const noexcept
        {
        }
        void get() noexcept
        {
            _valid = false;
        }

    private:
        bool _valid = false;
    };
    template<typename T>
    class promise
    {
    public:
        future<T> get_future()
        {
            return future<T>(true);
        }
        void set_value() noexcept
        {
        }
    };
    template<typename Function>
    future<void> async(launch, Function&& function)
    {
        function();
        return future<void>(true);
    }
} // namespace std
#endif
