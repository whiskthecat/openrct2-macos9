// Symbols the C++ runtime expects that this toolchain's C library lacks.
// The engine's own file access goes through ClassicVfs; these only satisfy
// std::filesystem and friends, which report "not supported".
#ifdef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"

    #include <cerrno>
    #include <cstdint>
    #include <cstring>
    #include <sys/stat.h>
    #include <sys/types.h>

extern "C" {
// 64-bit atomics: there is only one thread.
uint64_t __atomic_load_8(const volatile void* pointer, int)
{
    return *static_cast<const volatile uint64_t*>(pointer);
}
void __atomic_store_8(volatile void* pointer, uint64_t value, int)
{
    *static_cast<volatile uint64_t*>(pointer) = value;
}
uint64_t __atomic_fetch_add_8(volatile void* pointer, uint64_t value, int)
{
    auto* target = static_cast<volatile uint64_t*>(pointer);
    const uint64_t old = *target;
    *target = old + value;
    return old;
}
uint64_t __atomic_exchange_8(volatile void* pointer, uint64_t value, int)
{
    auto* target = static_cast<volatile uint64_t*>(pointer);
    const uint64_t old = *target;
    *target = value;
    return old;
}
bool __atomic_compare_exchange_8(volatile void* pointer, void* expected, uint64_t desired, bool, int, int)
{
    auto* target = static_cast<volatile uint64_t*>(pointer);
    auto* wanted = static_cast<uint64_t*>(expected);
    if (*target == *wanted)
    {
        *target = desired;
        return true;
    }
    *wanted = *target;
    return false;
}

int symlink(const char*, const char*)
{
    errno = ENOSYS;
    return -1;
}
int readlink(const char*, char*, size_t)
{
    errno = ENOSYS;
    return -1;
}
int truncate(const char*, off_t)
{
    errno = ENOSYS;
    return -1;
}
int mkdir(const char*, mode_t)
{
    errno = ENOSYS;
    return -1;
}
int chdir(const char*)
{
    errno = ENOSYS;
    return -1;
}
char* getcwd(char* buffer, size_t size)
{
    if (buffer != nullptr && size >= 5)
        std::strcpy(buffer, "/RCT");
    return buffer;
}
long pathconf(const char*, int)
{
    return 255;
}
int fchmod(int, mode_t)
{
    return 0;
}
int fchmodat(int, const char*, mode_t, int)
{
    return 0;
}
unsigned sleep(unsigned seconds)
{
    const auto until = ClassicHost::Microseconds() + uint64_t(seconds) * 1000000;
    while (ClassicHost::Microseconds() < until)
        ClassicHost::Yield();
    return 0;
}
int usleep(unsigned long microseconds)
{
    const auto until = ClassicHost::Microseconds() + microseconds;
    while (ClassicHost::Microseconds() < until)
    {
    }
    return 0;
}
}
#endif
