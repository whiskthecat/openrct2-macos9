// Adapted from Apple InterruptDisableLib (Quinn, 1998), accompanying TN1137.
// Copyright (c) 1998 Apple Computer, Inc. All rights reserved.
// Apple permits incorporation without restriction, AS IS, with responsibility
// for operation resting with the application. This is modified Apple sample
// code, not an unmodified distribution of DSC Sample Code.
// https://developer.apple.com/library/archive/technotes/tn/tn1137.html
#pragma once
#include <MixedMode.h>
#include <Memory.h>

namespace ClassicInterruptMask
{
    // Raw classic 68k entry points, as in Apple's sample. Mixed Mode maps D0;
    // no routine descriptor or memory allocation is needed by the callback.
    alignas(4) static UInt16 getSR[] = {0x40c0, 0x4e75}; // move sr,d0; rts
    alignas(4) static UInt16 setSR[] = {0x46c0, 0x4e75}; // move d0,sr; rts
    constexpr ProcInfoType getInfo = kRegisterBased | RESULT_SIZE(SIZE_CODE(sizeof(UInt16)))
        | REGISTER_RESULT_LOCATION(kRegisterD0);
    constexpr ProcInfoType setInfo = kRegisterBased
        | REGISTER_ROUTINE_PARAMETER(1, kRegisterD0, SIZE_CODE(sizeof(UInt16)));
    inline UInt16 SR() { return CallUniversalProc(reinterpret_cast<UniversalProcPtr>(getSR), getInfo); }
    inline void SR(UInt16 value) { CallUniversalProc(reinterpret_cast<UniversalProcPtr>(setSR), setInfo, value); }
    inline UInt16 Get() { return (SR() >> 8) & 7; }
    inline UInt16 Set(UInt16 mask)
    {
        const auto old = SR();
        SR((old & 0xf8ff) | (mask << 8));
        return (old >> 8) & 7;
    }
    inline bool Prepare()
    {
        // Task-time only. Keep the tiny emulator code resident for every later
        // VBL, warm Mixed Mode, and prove that it restores the incoming mask.
        static bool ready = false;
        if (ready) return true;
        if (HoldMemory(getSR, sizeof(getSR)) != noErr) return false;
        if (HoldMemory(setSR, sizeof(setSR)) != noErr)
        { UnholdMemory(getSR, sizeof(getSR)); return false; }
        const auto before = Get();
        const auto old = Set(7);
        const auto during = Get();
        Set(old);
        ready = old == before && during == 7 && Get() == before;
        // These four-byte static helpers live for the application lifetime.
        return ready;
    }
    class Guard
    {
        UInt16 old;
    public:
        Guard() : old(Set(7)) { __asm__ volatile("sync" ::: "memory"); }
        ~Guard() { __asm__ volatile("sync" ::: "memory"); Set(old); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
}
