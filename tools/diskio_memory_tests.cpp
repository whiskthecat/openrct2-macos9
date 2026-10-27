#ifdef NDEBUG
#undef NDEBUG
#endif
#include "BuildIdentity.h"
#include "ClassicPreload.h"
#include <cassert>
#include <cstdio>
using namespace ClassicPreload;
int main()
{
    if (!RctBuild::Start()) return 125;
    const uint64_t bank = 81 * MiB;
    // Constrained heaps are rejected before allocating, including fragmented
    // heaps with a large free total but no sufficiently large contiguous block.
    assert(Choose({128*MiB,128*MiB,64*MiB,12*MiB,8*MiB},2,bank).bytes == 0);
    assert(Choose({128*MiB,128*MiB,80*MiB,48*MiB,40*MiB},2,bank).bytes == 8*MiB);
    assert(!Choose({256*MiB,256*MiB,180*MiB,96*MiB,80*MiB},2,bank).full);
    assert(Choose({512*MiB,512*MiB,256*MiB,144*MiB,128*MiB},2,bank).full);
    assert(!Choose({512*MiB,512*MiB,256*MiB,144*MiB,64*MiB},2,bank).full);
    assert(!Choose({128*MiB,512*MiB,256*MiB,144*MiB,128*MiB},2,bank).full);
    assert(Choose({128*MiB,512*MiB,256*MiB,144*MiB,128*MiB},1,bank).full);
    assert(!Choose({128*MiB,128*MiB,64*MiB,12*MiB,8*MiB},1,bank).full);
    assert(Choose({512*MiB,512*MiB,256*MiB,144*MiB,128*MiB},0,bank).bytes == 8*MiB);
    std::puts("PASS physical RAM, actual partition, free/contiguous headroom, explicit on/off, minimum-heap admission");
}
