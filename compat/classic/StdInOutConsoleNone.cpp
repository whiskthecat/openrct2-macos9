// There is no terminal on Mac OS 9: console output goes to the log file.
#include <cstdio>
#include <openrct2/interface/StdInOutConsole.h>

void StdInOutConsole::Start()
{
}

std::future<void> StdInOutConsole::Eval(const std::string&)
{
    return std::future<void>();
}

void StdInOutConsole::ProcessEvalQueue()
{
}

void StdInOutConsole::Clear()
{
}

void StdInOutConsole::Close()
{
}

void StdInOutConsole::WriteLine(const std::string& text, OpenRCT2::FormatToken)
{
    std::printf("%s\n", text.c_str());
}
