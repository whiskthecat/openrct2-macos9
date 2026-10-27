#include <openrct2/core/OrcaStream.hpp>
// The game itself: the real engine with its real user interface, on top of
// our own window/input/display layer instead of SDL (see compat/sdl).
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <openrct2-ui/Ui.h>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/audio/AudioContext.h>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/command_line/CommandLine.hpp>
#include <openrct2/ui/UiContext.h>

#ifdef OPENRCT2_PAK_VFS
    #include "ClassicAudio.h"
    #include "ClassicAutosave.h"
    #include "ClassicLoadProgress.h"
#endif
#ifdef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"
    #include "ClassicDesktop.h"
    #include "../compat/sdl/ShimHost.h"

    #include <MacMemory.h>
    #include <iostream>
    #include <string>
    #include <vector>

extern "C" void ClassicTraceMarker();
extern "C" unsigned long ClassicAllocatorBytes(void);
extern "C" void ClassicAllocatorFailure(unsigned long*, unsigned long*, unsigned long*, unsigned long*);
static void ReportMemory()
{
    unsigned long count = 0, size = 0, freeBytes = 0, largest = 0;
    ClassicAllocatorFailure(&count, &size, &freeBytes, &largest);
    std::printf("memory: in use %lu, free now %ld, largest block %ld; failed requests %lu, last %lu bytes (free then %lu, largest %lu)\n",
        ClassicAllocatorBytes(), static_cast<long>(FreeMem()), static_cast<long>(MaxBlock()), count, size, freeBytes, largest);
}
#endif

#include "BuildIdentity.h"

using namespace OpenRCT2;
using namespace OpenRCT2::Audio;
using namespace OpenRCT2::Ui;

#ifdef OPENRCT2_FORMATTER_AUDIT
#include <openrct2/localisation/FormatterAudit.h>
void FormatterAuditUiCommand(const std::vector<std::string>&);
#endif

int main(int argc, const char** argv)
{
#ifdef OPENRCT2_PAK_VFS
    OpenRCT2::OrcaStream::profileSave = std::getenv("RCT_SAVE_PROFILE") != nullptr;
#endif
#if defined(OPENRCT2_PAK_VFS) && !defined(OPENRCT2_CLASSIC_MAC)
    ClassicLoadProgress::Configure(
        !std::getenv("RCT_LEGACY_LOAD_PROGRESS"), std::getenv("RCT_LOAD_TRACE"), !std::getenv("RCT_NO_LOAD_REDRAW"));
#endif
#ifdef OPENRCT2_FORMATTER_AUDIT
    FormatterAudit::UiCommand = FormatterAuditUiCommand;
#endif
#ifdef OPENRCT2_CLASSIC_MAC
    // No command line on Mac OS 9: optional arguments (one per line, for
    // example a park to open) come from RCT.txt beside the application, and
    // console output goes to RCT.log.
    MaxApplZone();
    for (int i = 0; i < 16; i++)
        MoreMasters();
    static std::ios_base::Init streamInit;
    const auto directory = ClassicHost::ApplicationDirectory();
    std::freopen((directory + "RCT.log").c_str(), "w", stdout);
    std::freopen((directory + "RCT.err").c_str(), "w", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    static std::vector<std::string> lines{ "rct" };
    if (FILE* file = std::fopen((directory + "RCT.txt").c_str(), "rb"))
    {
        std::string text;
        for (int c = std::fgetc(file);; c = std::fgetc(file))
        {
            if (c == EOF || c == '\n' || c == '\r')
            {
                if (!text.empty())
                    lines.push_back(text);
                text.clear();
                if (c == EOF)
                    break;
            }
            else
                text.push_back(static_cast<char>(c));
        }
        std::fclose(file);
    }
    // Our own settings in the argument file: "--mute" (no sound at all) and "--volume=NN" (limit
    // on sound output in percent, default 25). They are not passed on to the game.
    bool installDesktop = false;
    for (size_t i = 1; i < lines.size();)
    {
        if (lines[i] == "--install-desktop-alias")
            installDesktop = true;
        else if (lines[i] == "--trace-load")
            ClassicLoadProgress::Configure(true, true);
        else if (lines[i] == "--no-load-redraw")
            ClassicLoadProgress::Configure(true, true, false);
        else if (lines[i] == "--legacy-load-progress")
            ClassicLoadProgress::Configure(false);
        else if (lines[i] == "--native-event-trace")
            ShimHost::EventDiagnostics("{\"reset\":true,\"on\":true}");
        else if (lines[i] == "--no-preload")
        {
            OpenRCT2::Audio::ClassicAudioSetPreloadEnabled(false);
            ClassicVfs::SetPreloadEnabled(false);
        }
        else if (lines[i] == "--preload=on")
            OpenRCT2::Audio::ClassicAudioSetPreloadMode(1);
        else if (lines[i] == "--preload=off")
            OpenRCT2::Audio::ClassicAudioSetPreloadMode(0);
        else if (lines[i] == "--preload=automatic")
            OpenRCT2::Audio::ClassicAudioSetPreloadMode(2);
        else if (lines[i] == "--sync-autosave")
            ClassicAutosave::SetEnabled(false);
        else if (lines[i].rfind("--audio-mixer=", 0) == 0)
        {
            const auto backend = lines[i].substr(14);
            unsigned mode = backend == "app-stereo" ? 1 : backend == "app-mono" ? 2 : backend == "app-44100" ? 3 : 0;
            OpenRCT2::Audio::ClassicAudioSetMixerBackend(mode);
        }
        else if (lines[i] == "--mute")
            OpenRCT2::Audio::ClassicAudioSetOutputLimit(0.0f);
        else if (lines[i].rfind("--volume=", 0) == 0)
            OpenRCT2::Audio::ClassicAudioSetOutputLimit(static_cast<float>(std::atoi(lines[i].c_str() + 9)) / 100.0f);
        else
        {
            i++;
            continue;
        }
        lines.erase(lines.begin() + static_cast<long>(i));
    }
    if (installDesktop)
        return ClassicDesktop::Install();
    static std::vector<const char*> pointers;
    for (auto& text : lines)
        pointers.push_back(text.c_str());
    argc = static_cast<int>(pointers.size());
    argv = pointers.data();
    std::printf("RollerCoaster Tycoon engine on Mac OS 9: %s, free heap %ld\n", directory.c_str(), static_cast<long>(FreeMem()));
    {
        // Address of a known function, for naming addresses in crash reports (container/macsym.sh marker=...).
        std::printf("code marker %08lx\n", static_cast<unsigned long>(*reinterpret_cast<const uintptr_t*>(reinterpret_cast<const void*>(&ClassicTraceMarker))));
    }
    std::printf("sizes: game state %lu, entity table %lu (%lu each), rides %lu (%lu each)\n",
        static_cast<unsigned long>(sizeof(OpenRCT2::GameState_t)), static_cast<unsigned long>(sizeof(OpenRCT2::EntityRegistry)),
        static_cast<unsigned long>(sizeof(OpenRCT2::Entity_t)), static_cast<unsigned long>(sizeof(OpenRCT2::GameState_t::rides)),
        static_cast<unsigned long>(sizeof(OpenRCT2::Ride)));
    OpenRCT2::TimeMark("program start");
    OpenRCT2::OrcaStream::profileSave = std::getenv("RCT_SAVE_PROFILE") != nullptr;
    ClassicVfs::Initialise();
    OpenRCT2::TimeMark("packs mounted");
    if (!ClassicVfs::Problems().empty())
        std::printf("file system: %s", ClassicVfs::Problems().c_str());
#endif
    if (!RctBuild::Start())
        return 125;
#ifndef OPENRCT2_CLASSIC_MAC
    extern void InitialiseInterpolationAudit();
    InitialiseInterpolationAudit();
#endif
    std::set_terminate([] {
        std::printf("terminate called");
        if (auto current = std::current_exception())
        {
            try
            {
                std::rethrow_exception(current);
            }
            catch (const std::exception& e)
            {
                std::printf(": %s", e.what());
            }
            catch (...)
            {
                std::printf(": unknown exception");
            }
        }
        std::printf("\n");
        std::fflush(stdout);
        std::abort();
    });
    std::atexit([] { std::printf("exit handlers running\n"); });
#ifdef OPENRCT2_PAK_VFS
    std::atexit([] { (void)ClassicAutosave::Drain(); });
#endif
    try
    {
        auto runGame = CommandLineRun(argv, argc);
        if (runGame != OpenRCT2::CommandLine::ExitCode::launch)
            return runGame == OpenRCT2::CommandLine::ExitCode::fail ? 1 : 0;
        auto env = CreatePlatformEnvironment();
        auto uiContext = CreateUiContext(*env);
        auto context = CreateContext(
#ifdef OPENRCT2_PAK_VFS
            std::move(env), CreateClassicAudioContext(), std::move(uiContext));
#else
            std::move(env), CreateDummyAudioContext(), std::move(uiContext));
#endif
        const int result = context->RunOpenRCT2(argc, argv);
#ifdef OPENRCT2_PAK_VFS
        ClassicAutosave::Drain();
#endif
        std::printf("game loop returned %d\n", result);
        return result;
    }
    catch (const std::exception& e)
    {
        std::printf("fatal: %s\n", e.what());
#ifdef OPENRCT2_CLASSIC_MAC
        ReportMemory();
#endif
        return 1;
    }
}
