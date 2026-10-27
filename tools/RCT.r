#include "Types.r"
#include "Processes.r"
#include "CodeFragments.r"
#include "../compat/classic/ClassicVersion.h"
#include "RCTIcon.r"

// The whole engine lives in this application: give it a large partition and
// a deep stack (the Process Manager takes the stack size from 'cfrg').
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    isHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    256 * 1024 * 1024,   /* preferred: includes 81 MiB of shared, preloaded sound samples */
    100 * 1024 * 1024     /* minimum: launch smaller parks with read-on-demand audio */
};

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        1024 * 1024, kNoAppSubFolder,
        kApplicationCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "RCT"
    }
};

resource 'vers' (1) {
    RCT_VERSION_MAJOR, (RCT_VERSION_MINOR << 4) | RCT_VERSION_PATCH,
#if RCT_VERSION_DEVELOPMENT
    development, RCT_VERSION_DEVELOPMENT, verUS,
#else
    final, 0, verUS,
#endif
    RCT_PORT_VERSION, "OpenRCT2 Classic Mac " RCT_PORT_VERSION
};
