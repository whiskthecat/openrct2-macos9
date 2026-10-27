#include "Types.r"
#include "Processes.r"
#include "CodeFragments.r"

// The whole engine lives in this application: give it a large partition and
// a deep stack (the Process Manager takes the stack size from 'cfrg').
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    isHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    320 * 1024 * 1024,
    170 * 1024 * 1024
};

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        1024 * 1024, kNoAppSubFolder,
        kApplicationCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "RCTSim"
    }
};

resource 'vers' (1) {
    0x00, 0x58, development, 0x05, verUS, "0.5.8-simnext2", "OpenRCT2 Classic Mac 0.5.8-simnext2"
};
