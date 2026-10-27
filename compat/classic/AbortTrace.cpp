// When the C library aborts (failed assertion, std::terminate, ...) record
// where it came from, since Mac OS 9 otherwise just ends the application.
// The addresses can be named with container/macsym.sh.
#ifdef OPENRCT2_CLASSIC_MAC
    #include <Processes.h>
    #include <cstdint>
    #include <cstdio>

extern "C" void ClassicTraceMarker()
{
}

extern "C" void ClassicPrintTrace(const char* reason)
{
    // A function pointer is a descriptor whose first word is the code address.
    const auto marker = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const void*>(&ClassicTraceMarker));
    std::printf("%s; marker ClassicTraceMarker at %08lx; return addresses:", reason, static_cast<unsigned long>(marker));
    const auto* frame = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    for (int depth = 0; depth < 24 && frame != nullptr; depth++)
    {
        const auto* next = reinterpret_cast<const uintptr_t*>(frame[0]);
        if (next == nullptr || next <= frame)
            break;
        std::printf(" %08lx", static_cast<unsigned long>(next[2]));
        frame = next;
    }
    std::printf("\n");
    std::fflush(stdout);
}

extern "C" void abort(void)
{
    ClassicPrintTrace("abort called");
    ExitToShell();
    for (;;)
    {
    }
}
#endif
