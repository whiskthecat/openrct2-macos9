#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace RctBuild
{
    const char* Source();
    const char* Id();
    const char* Lane();
    inline bool Start()
    {
        std::printf("build: source=%s build=%s lane=%s\n", Source() + 14, Id() + 13, Lane());
#ifndef OPENRCT2_CLASSIC_MAC
        // Every development-container invocation carries the content state it is
        // executing; tests exit before loading data if handed another state's executable.
        const char* expected = std::getenv("RCT_SOURCE_STATE");
        if (expected != nullptr && std::strcmp(expected, Source() + 14) != 0)
        {
            std::fprintf(stderr, "[rct] STALE SOURCE: this executable was built from other sources; rebuild\n");
            return false;
        }
#endif
        return true;
    }
}
