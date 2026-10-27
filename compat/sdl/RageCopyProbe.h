// Opt-in hardware experiment, included in ShimHostMac.cpp inside namespace ShimHost.
// No renderer calls this. Register definitions follow ATI RRG-G04500-C and X.org r128.
namespace
{
    struct RagePackets
    {
        const Rage128& card;
        bool okay = true;
        unsigned credits = 0;

        bool Space(unsigned words)
        {
            if (!okay) return false;
            if (credits < words)
            {
                const uint64_t limit = ClassicHost::Microseconds() + 250000;
                do
                {
                    credits = card.Read(0x07b8) & 0xfff;
                    if (credits >= words) break;
                    if (ClassicHost::Microseconds() >= limit) return okay = false;
                } while (true);
            }
            credits -= words;
            return true;
        }

        // PM4 PIO accepts pairs through separate even/odd apertures. A type-0
        // packet writes consecutive registers; type-2 pads to an even word count.
        void Register(uint32_t reg, uint32_t value)
        {
            if (!Space(2)) return;
            card.Write(0x1000, reg >> 2);
            card.Write(0x1004, value);
        }
        void Pair(uint32_t reg, uint32_t first, uint32_t second)
        {
            if (!Space(4)) return;
            card.Write(0x1000, (1u << 16) | (reg >> 2));
            card.Write(0x1004, first);
            card.Write(0x1000, second);
            card.Write(0x1004, 0x80000000u);
        }
        void Copy(int sourceX, int sourceY, int x, int y, int width, int height)
        {
            if (!Space(4)) return;
            card.Write(0x1000, (2u << 16) | (kSrcYX >> 2));
            card.Write(0x1004, (static_cast<uint32_t>(sourceY) << 16) | sourceX);
            card.Write(0x1000, ((static_cast<uint32_t>(y) & 0x3fff) << 16) | (x & 0x3fff));
            card.Write(0x1004, (static_cast<uint32_t>(height) << 16) | width);
        }
        bool Idle()
        {
            const uint64_t limit = ClassicHost::Microseconds() + 250000;
            while ((card.Read(0x07b8) & 0x80010000u) != 0
                || (card.Read(0x07b8) & 0xfff) != 64
                || (card.Read(kGuiStat) & 0x80000000u) != 0
                || (card.Read(kGuiStat) & 0xfff) != 64)
                if (ClassicHost::Microseconds() >= limit) return okay = false;
            credits = 0; // reread after a driver call or completed batch
            return okay;
        }
        bool CpuReady()
        {
            if (!Idle()) return false;
            // Engine idle does not mean pixels have reached VRAM. Flush all pixel
            // cache partitions, then use a PPC barrier before CPU framebuffer access.
            card.Write(0x0184, (card.Read(0x0184) & ~0xffu) | 0xffu);
            const uint64_t limit = ClassicHost::Microseconds() + 250000;
            while ((card.Read(0x0184) & 0x80000000u) != 0)
                if (ClassicHost::Microseconds() >= limit) return okay = false;
            __asm__ volatile("sync" ::: "memory");
            return okay;
        }
    };

    // Read the individual RW registers, never the combined write-only aliases.
    constexpr uint32_t kProbeSaved[] = {
        kGuiMaster, 0x1404, 0x1408, 0x15ac, 0x15b0, 0x1414, 0x1418, 0x141c, 0x1420,
        0x1640, 0x1644, 0x1648, 0x164c, 0x1654, 0x165c,
        kDpCntl, kDpDatatype, kDpMix, 0x16cc, kClrCmpCntl, kClrCmpSrc, kClrCmpMask
    };

    struct ProbeClip { int left, top, right, bottom; }; // right/bottom exclusive
    struct ProbePoint { int x, y; };

    // Display Manager's read-only mode list supplies pitch at every resolution,
    // without changing the user's screen mode or saved resolution.
    pascal void ProbeMode(void* data, DMListIndexType, DMDisplayModeListEntryPtr entry)
    {
        if (!entry || !entry->displayModeDepthBlockInfo) return;
        const auto& block = *entry->displayModeDepthBlockInfo;
        if (!block.depthVPBlock || block.depthBlockCount > 64) return;
        auto& out = *static_cast<std::string*>(data);
        for (unsigned long i = 0; i < block.depthBlockCount; i++)
        {
            const auto& depth = block.depthVPBlock[i];
            if (!depth.depthVPBlock || !depth.depthSwitchInfo) continue;
            const auto& v = *depth.depthVPBlock;
            const int w = v.vpBounds.right-v.vpBounds.left, h = v.vpBounds.bottom-v.vpBounds.top;
            if (v.vpPixelSize != 8 || depth.depthSwitchInfo->csPage != 0
                || !((w == 640 && h == 480) || (w == 800 && h == 600) || (w == 1024 && h == 768))) continue;
            char row[100];
            std::snprintf(row,sizeof(row),"[%d,%d,%d,%lu]",w,h,v.vpRowBytes & 0x3fff,
                static_cast<unsigned long>(v.vpBaseOffset));
            if (out.find(row) == std::string::npos) { if (!out.empty()) out += ","; out += row; }
        }
    }

    void ProbeRle(uint8_t* destination, int stride, const OpenRCT2::G1Element& g1, ImageId image,
        const OpenRCT2::Drawing::PaletteMap& palette, int x, int y, const ProbeClip& clip, int zoom)
    {
        const int factor = 1 << zoom;
        const int left = std::max(x, clip.left), top = std::max(y, clip.top);
        const int width = std::min(x + (g1.width+factor-1)/factor, clip.right) - left;
        const int height = std::min(y + (g1.height+factor-1)/factor, clip.bottom) - top;
        if (width <= 0 || height <= 0) return;
        OpenRCT2::Drawing::RenderTarget rt{};
        rt.width = width; rt.height = height; rt.pitch = stride - width;
        rt.zoom_level = ZoomLevel{static_cast<int8_t>(zoom)};
        rt.bits = reinterpret_cast<OpenRCT2::Drawing::PaletteIndex*>(destination + top * stride + left);
        const int srcX = (left-x)*factor, srcY = (top-y)*factor;
        const DrawSpriteArgs args{image,palette,g1,srcX,srcY,
            std::min(width*factor,g1.width-srcX),std::min(height*factor,g1.height-srcY),rt.bits};
        GfxRleSpriteToBuffer(rt,args);
    }

    void ProbeSoftware(uint8_t* destination, int stride, const uint8_t* sprite, int sourceStride,
        int width, int height, int x, int y, bool keyed, const ProbeClip& clip)
    {
        const int left = std::max(x, clip.left), top = std::max(y, clip.top);
        const int right = std::min(x + width, clip.right), bottom = std::min(y + height, clip.bottom);
        for (int yy = top; yy < bottom; yy++)
        {
            auto* to = destination + yy * stride + left;
            const auto* from = sprite + (yy - y) * sourceStride + left - x;
            if (!keyed) CopyRow(to, from, std::max(0, right - left));
            else for (int xx = left; xx < right; xx++, to++, from++) if (*from != 0) *to = *from;
        }
    }

    std::string RageCopyProbe(int stage, int selection, uint32_t requestedCount)
    {
        if (NativeVSync::active) return "{\"error\":\"disable VSync before running the destructive card probe\"}";
        if (!gOpen || gWindow == nullptr || gDirect32 || gSuspended)
            return "{\"error\":\"needs foreground 8-bit native screen\"}";
        if (stage > 7) return "{\"error\":\"use stage 5 fixtures, 6 guest, or 7 vehicle\"}";
        std::string out = "{";
        const int zoom = stage >= 6 ? std::clamp(-selection,0,2) : 0;
        const int factor = 1 << zoom;
        const OpenRCT2::G1Element* real = nullptr;
        ImageId image{};
        OpenRCT2::Drawing::PaletteIndex mapping[256];
        for (int i = 0; i < 256; i++) mapping[i] = static_cast<OpenRCT2::Drawing::PaletteIndex>(i);
        OpenRCT2::Drawing::PaletteMap palette{std::span(mapping)};
        auto remap = [&](OpenRCT2::Drawing::PaletteIndex start, OpenRCT2::Drawing::Colour colour) {
            auto source = GetPaletteMapForColour(static_cast<OpenRCT2::Drawing::FilterPaletteID>(colour));
            if (source) palette.Copy(
                start,*source,OpenRCT2::Drawing::PaletteIndex::primaryRemap0,12);
        };
        auto usable = [](const OpenRCT2::G1Element* g1) {
            return g1 && g1->offset && g1->width > 0 && g1->width <= 128 && g1->height > 0 && g1->height <= 128
                && g1->flags.has(OpenRCT2::G1Flag::hasRLECompression);
        };
        if (stage == 6)
            for (auto* guest : OpenRCT2::EntityList<OpenRCT2::Guest>())
            {
                const auto index = PaintPeepGetBaseImageAndOffset(*guest,0).baseImageId;
                if (!usable(GfxGetG1Element(index))) continue;
                real = GfxGetG1Element(index);
                image = ImageId(index).WithPrimary(guest->tShirtColour).WithSecondary(guest->trousersColour);
                remap(OpenRCT2::Drawing::PaletteIndex::primaryRemap0,guest->tShirtColour);
                remap(OpenRCT2::Drawing::PaletteIndex::secondaryRemap0,guest->trousersColour);
                break;
            }
        if (stage == 7)
            for (auto* vehicle : OpenRCT2::EntityList<OpenRCT2::Vehicle>())
            {
                const auto* entry = vehicle->Entry();
                if (!entry || !usable(GfxGetG1Element(entry->baseImageId))) continue;
                real = GfxGetG1Element(entry->baseImageId);
                image = ImageId(entry->baseImageId).WithPrimary(vehicle->colours.Body)
                    .WithSecondary(vehicle->colours.Trim).WithTertiary(vehicle->colours.Tertiary);
                remap(OpenRCT2::Drawing::PaletteIndex::primaryRemap0,vehicle->colours.Body);
                remap(OpenRCT2::Drawing::PaletteIndex::secondaryRemap0,vehicle->colours.Trim);
                remap(OpenRCT2::Drawing::PaletteIndex::tertiaryRemap0,vehicle->colours.Tertiary);
                break;
            }
        if (stage >= 6 && !real) return "{\"error\":\"no suitable loaded sprite\"}";
        Rage128 card;
        // FindRage128 rejects Classic before any registry/MMIO operation and accepts
        // only ATI 1002:5052. Also require this screen to lie in that card's aperture.
        if (!FindRage128(card, out) || card.frameBuffer == nullptr)
            return out + "\"error\":\"not native Rage 128\"}";
        const uint32_t memory = card.Read(0x00f8) & 0x03ffffff;
        const auto aperture = reinterpret_cast<uintptr_t>(card.frameBuffer);
        auto offset = [&](const uint8_t* pointer, int stride, int height, uint32_t& at) {
            const auto address = reinterpret_cast<uintptr_t>(pointer);
            if (address < aperture || address - aperture >= 0x04000000u) return false;
            at = (address - aperture) & 0x01ffffffu;
            return memory != 0 && at < memory && static_cast<uint64_t>(at) + stride * height <= memory
                && (at & 31) == 0 && stride > 0 && (stride & 15) == 0;
        };
        uint32_t screenAt = 0;
        if (!offset(gScreen, gStride, gHeight, screenAt) || gWidth < 640 || gHeight < 480
            || (card.Read(0x0704) >> 28) != 7)
            return out + "\"error\":\"unsupported aperture, surface or PM4 mode\"}";
        RagePackets packets{card};
        if (!packets.CpuReady()) return out + "\"error\":\"initial engine busy\"}";
        const uint32_t scratch = card.Read(0x15e0);
        packets.Register(0x15e0, 0x13579bdf);
        const bool accepted = packets.Idle() && card.Read(0x15e0) == 0x13579bdf;
        const uint32_t observed = card.Read(0x15e0);
        packets.Register(0x15e0, scratch);
        packets.CpuReady();
        char text[500];
        std::snprintf(text, sizeof(text), "\"vram\":%lu,\"screen\":[%lu,%d],\"packet_scratch\":[%s,%lu],",
            static_cast<unsigned long>(memory), static_cast<unsigned long>(screenAt), gStride,
            accepted ? "true" : "false", static_cast<unsigned long>(observed));
        out += text;
        if (!accepted || !packets.okay) return out + "\"error\":\"PIO packet not executed\"}";
        std::string modes;
        DisplayIDType display = 0;
        DMListType list = nullptr;
        DMListIndexType nModes = 0;
        OSErr modeError = DMGetDisplayIDByGDevice(gDevice,&display,false);
        if (modeError == noErr) modeError = DMNewDisplayModeList(display,kDMModeListPreferSafeModesMask,0,&nModes,&list);
        const auto callback = NewDMDisplayModeListIteratorUPP(ProbeMode);
        if (modeError == noErr && list && callback && nModes <= 4096)
            for (DMListIndexType i = 0; i < nModes && modeError == noErr; i++)
                modeError = DMGetIndexedDisplayModeFromList(list,i,0,callback,&modes);
        if (callback) DisposeDMDisplayModeListIteratorUPP(callback);
        if (list) DMDisposeList(list);
        out += "\"modes\":[" + modes + "],\"zoom\":" + std::to_string(zoom) + ",";

        Rect bounds{0, 0, 128, 128};
        GWorldPtr world = nullptr;
        const auto colours = (**(**gDevice).gdPMap).pmTable;
        const auto error = NewGWorld(&world, 8, &bounds, colours, nullptr, useDistantHdwrMem);
        if (error != noErr || world == nullptr) return out + "\"error\":\"no video GWorld\"}";
        const auto pixels = GetGWorldPixMap(world);
        const bool locked = LockPixels(pixels);
        auto* cardBits = reinterpret_cast<uint8_t*>(GetPixBaseAddr(pixels));
        const int cardStride = (**pixels).rowBytes & 0x3fff;
        uint32_t cardAt = 0;
        if (!locked || !offset(cardBits, cardStride, 128, cardAt))
        {
            if (locked) UnlockPixels(pixels);
            DisposeGWorld(world);
            return out + "\"error\":\"GWorld not aligned in VRAM\"}";
        }
        std::snprintf(text, sizeof(text), "\"offscreen\":[%lu,%d],", static_cast<unsigned long>(cardAt), cardStride);
        out += text;
        const size_t screenBytes = static_cast<size_t>(gStride) * gHeight;
        std::vector<uint8_t> original(screenBytes), expected(screenBytes), initial(screenBytes), ram(screenBytes);
        std::vector<uint8_t> sprite(128 * 128);
        if (real)
        {
            const ProbeClip clip{0,0,128,128};
            ProbeRle(sprite.data(),128,*real,image,palette,0,0,clip,zoom);
            std::vector<uint8_t> mask(128 * 128,255);
            ProbeRle(mask.data(),128,*real,image,palette,0,0,clip,zoom);
            int zeroOpaque = 0, pixelsDrawn = 0;
            for (int y = 0; y < real->height; y++) for (int x = 0; x < real->width; x++)
            {
                pixelsDrawn += sprite[y * 128 + x] != 0;
                zeroOpaque += sprite[y * 128 + x] == 0 && mask[y * 128 + x] != 255;
            }
            std::snprintf(text,sizeof(text),"\"sprite\":[%d,%lu,%d,%d,%d,%d],",stage,
                static_cast<unsigned long>(image.GetIndex()),real->width,real->height,pixelsDrawn,zeroOpaque);
            out += text;
            if (zeroOpaque)
            {
                UnlockPixels(pixels); DisposeGWorld(world);
                return out + "\"error\":\"colour key cannot represent written zero\"}";
            }
        }
        else for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++)
                sprite[y * 128 + x] = (x * 7 + y * 13) % 10 < 4 ? 0 : static_cast<uint8_t>(16 + ((x + y) & 127));
        GrafPtr savedPort;
        GetPort(&savedPort);
        auto port = GetWindowPort(gWindow);
        const auto fg = port->fgColor, bg = port->bkColor;
        const auto rgbFg = port->rgbFgColor, rgbBg = port->rgbBkColor;
        SetPort(reinterpret_cast<GrafPtr>(port));
        port->fgColor = 255; port->bkColor = 0;
        port->rgbFgColor = {0,0,0}; port->rgbBkColor = {0xffff,0xffff,0xffff};
        HideCursor();
        packets.CpuReady();
        std::memcpy(original.data(), gScreen, screenBytes);
        for (size_t i = 0; i < screenBytes; i++) initial[i] = 180 + (i * 17 % 60);
        // Source on screen stays disjoint from all destinations, including clip cases.
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++)
        {
            cardBits[y * cardStride + x] = sprite[y * 128 + x];
            initial[(y + 8) * gStride + x + 8] = sprite[y * 128 + x];
        }
        __asm__ volatile("sync" ::: "memory");
        uint32_t saved[sizeof(kProbeSaved) / sizeof(kProbeSaved[0])];
        for (size_t i = 0; i < std::size(kProbeSaved); i++) saved[i] = card.Read(kProbeSaved[i]);
        auto restore = [&] {
            // Master may implicitly load defaults; restore individual state afterwards.
            for (size_t i = 0; i < std::size(kProbeSaved); i++) packets.Register(kProbeSaved[i], saved[i]);
            return packets.CpuReady();
        };
        auto setup = [&](bool screenSource, bool keyed, const ProbeClip& clip) {
            uint32_t master = 0x22cc32dbu; // 8-bit colour source, SRCCOPY, explicit pitches and destination clip
            if (!keyed) master |= 0x10000000;
            packets.Register(kGuiMaster, master);
            packets.Pair(kSrcPitchOffset, (static_cast<uint32_t>((screenSource ? gStride : cardStride) / 8) << 21)
                | ((screenSource ? screenAt : cardAt) >> 5), (static_cast<uint32_t>(gStride / 8) << 21) | (screenAt >> 5));
            packets.Register(kDpCntl, 3);
            packets.Register(0x16cc, 0xffffffff);
            packets.Register(kClrCmpCntl, keyed ? 0x01000005 : 0);
            packets.Register(kClrCmpSrc, 0);
            packets.Register(kClrCmpMask, 0xff);
            packets.Register(kScTopLeft, (static_cast<uint32_t>(clip.top) << 16) | clip.left);
            packets.Register(kScBottomRight, (static_cast<uint32_t>(clip.bottom - 1) << 16) | (clip.right - 1));
        };
        auto compare = [&] {
            int wrong = 0, first = -1;
            __asm__ volatile("sync" ::: "memory");
            const auto* actual = reinterpret_cast<volatile uint8_t*>(gScreen);
            for (int y = 0; y < gHeight; y++) for (int x = 0; x < gWidth; x++)
                if (actual[y * gStride + x] != expected[y * gStride + x])
                { if (first < 0) first = y * gStride + x; wrong++; }
            return std::pair<int,int>{wrong, first};
        };
        const ProbeClip whole{0,0,gWidth,gHeight};
        const int count = std::min<uint32_t>(requestedCount, 2048);
        const int widths[] = {real ? (real->width+factor-1)/factor : 16,64};
        const int heights[] = {real ? (real->height+factor-1)/factor : 24,32};
        out += "\"columns\":[\"source\",\"key\",\"w\",\"h\",\"wrong\",\"first\",\"n\",\"gpu_us\",\"cpu_vram_us\",\"cpu_ram_us\",\"qd_wrong\",\"gpu_batch_wrong\",\"cpu_batch_wrong\",\"ram_batch_wrong\"],\"cases\":[";
        bool firstCase = true;
        bool allGood = true;
        for (int size = 0; size < (real ? 1 : 2) && packets.okay; size++) for (int source = 0; source < 2 && packets.okay; source++)
            for (int keyed = 0; keyed < 2 && packets.okay; keyed++)
        {
            const int caseId = size * 4 + source * 2 + keyed;
            if (selection > 0 && selection != caseId + 1) continue;
            const int w = widths[size], h = heights[size];
            auto software = [&](uint8_t* to,int x,int y,const ProbeClip& clip) {
                if (real && keyed) ProbeRle(to,gStride,*real,image,palette,x,y,clip,zoom);
                else ProbeSoftware(to,gStride,sprite.data(),128,w,h,x,y,keyed,clip);
            };
            expected = initial;
            std::memcpy(gScreen, initial.data(), screenBytes);
            __asm__ volatile("sync" ::: "memory");
            const ProbePoint tests[] = {{260,180},{-5,160},{gWidth-w+5,250},{300,-6},{420,gHeight-h+6},
                {180,160},{gWidth+5,300}};
            for (size_t i = 0; i < std::size(tests); i++)
            {
                const int insetX = std::max(1,w/4), insetY = std::max(1,h/4);
                const ProbeClip clip = i == 5 ? ProbeClip{180+insetX,160+insetY,180+w-insetX,160+h-insetY} : whole;
                setup(source == 0, keyed, clip);
                packets.Copy(source == 0 ? 8 : 0, source == 0 ? 8 : 0, tests[i].x, tests[i].y, w,h);
                software(expected.data(),tests[i].x,tests[i].y,clip);
            }
            if (!packets.CpuReady()) break;
            const auto [wrong, first] = compare();
            allGood &= wrong == 0;
            double gpu = 0, cpu = 0, cpuRam = 0;
            int gpuWrong = 0, cpuWrong = 0, ramWrong = 0;
            std::vector<ProbePoint> positions;
            if (wrong == 0 && count > 0)
            {
                uint32_t seed = 12345;
                for (int i = 0; i < count; i++)
                {
                    seed = seed * 1103515245u + 12345u;
                    positions.push_back({160 + static_cast<int>((seed >> 8) % (gWidth - w - 160)),
                        140 + static_cast<int>((seed >> 20) % (gHeight - h - 140))});
                }
                expected = initial;
                for (const auto& p : positions) software(expected.data(),p.x,p.y,whole);
                std::memcpy(gScreen, initial.data(), screenBytes);
                __asm__ volatile("sync" ::: "memory");
                const uint64_t begin = ClassicHost::Microseconds();
                setup(source == 0,keyed,whole);
                for (const auto& p : positions) packets.Copy(source == 0 ? 8 : 0,source == 0 ? 8 : 0,p.x,p.y,w,h);
                packets.CpuReady();
                restore();
                const uint64_t end = ClassicHost::Microseconds();
                gpu = static_cast<double>(end - begin) / count;
                gpuWrong = compare().first;
                allGood &= packets.okay && gpuWrong == 0;
                std::memcpy(gScreen, initial.data(), screenBytes);
                const uint64_t cpuBegin = ClassicHost::Microseconds();
                for (const auto& p : positions) software(gScreen,p.x,p.y,whole);
                __asm__ volatile("sync" ::: "memory");
                cpu = static_cast<double>(ClassicHost::Microseconds() - cpuBegin) / count;
                cpuWrong = compare().first;
                allGood &= cpuWrong == 0;
                ram = initial;
                const uint64_t ramBegin = ClassicHost::Microseconds();
                for (const auto& p : positions) software(ram.data(),p.x,p.y,whole);
                cpuRam = static_cast<double>(ClassicHost::Microseconds() - ramBegin) / count;
                for (size_t i = 0; i < ram.size(); i++) ramWrong += ram[i] != expected[i];
                allGood &= ramWrong == 0;
            }
            restore();
            // Driver interoperation: make a fresh opaque/transparent QuickDraw copy
            // after restoring state, and check background holes and all guard pixels.
            std::memcpy(gScreen, initial.data(), screenBytes);
            expected = initial;
            Rect from{0,0,static_cast<short>(h),static_cast<short>(w)};
            Rect to{180,260,static_cast<short>(180+h),static_cast<short>(260+w)};
            CopyBits(reinterpret_cast<const BitMap*>(*pixels), &reinterpret_cast<GrafPtr>(port)->portBits,
                &from,&to,keyed ? transparent : srcCopy,nullptr);
            const uint64_t qdLimit = ClassicHost::Microseconds() + 250000;
            while (!QDDone(reinterpret_cast<GrafPtr>(port)) && ClassicHost::Microseconds() < qdLimit) {}
            packets.CpuReady();
            ProbeSoftware(expected.data(),gStride,sprite.data(),128,w,h,260,180,keyed,whole);
            const int qdWrong = compare().first;
            allGood &= qdWrong == 0;
            if (!firstCase) out += ",";
            firstCase = false;
            std::snprintf(text,sizeof(text),"[%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d,%d,%d,%d]",
                source,keyed,w,h,wrong,first,count,gpu,cpu,cpuRam,qdWrong,gpuWrong,cpuWrong,ramWrong);
            out += text;
        }
        out += "],";
        int overlapWrong = 0;
        if (packets.okay)
            for (int keyed = 0; keyed < 2; keyed++) for (int direction = 0; direction < 4; direction++)
            {
                expected = initial;
                std::memcpy(gScreen,initial.data(),screenBytes);
                __asm__ volatile("sync" ::: "memory");
                const int x = direction & 1 ? 13 : 3, y = direction & 2 ? 13 : 3;
                setup(true,keyed,whole);
                packets.Register(kDpCntl,(x > 8 ? 0 : 1) | (y > 8 ? 0 : 2));
                const int dx = x > 8 ? 63 : 0, dy = y > 8 ? 31 : 0;
                packets.Copy(8+dx,8+dy,x+dx,y+dy,64,32);
                ProbeSoftware(expected.data(),gStride,sprite.data(),128,64,32,x,y,keyed,whole);
                packets.CpuReady();
                overlapWrong += compare().first;
            }
        allGood &= overlapWrong == 0;
        const bool restored = restore();
        int stateWrong = 0;
        for (size_t i = 0; i < std::size(kProbeSaved); i++) if (card.Read(kProbeSaved[i]) != saved[i]) stateWrong++;
        if (restored) std::memcpy(gScreen, original.data(), screenBytes);
        __asm__ volatile("sync" ::: "memory");
        port->fgColor = fg; port->bkColor = bg; port->rgbFgColor = rgbFg; port->rgbBkColor = rgbBg;
        ::ShowCursor(); SetPort(savedPort);
        UnlockPixels(pixels);
        DisposeGWorld(world);
        gRecolour = true;
        std::snprintf(text,sizeof(text),"\"idle\":%s,\"state_wrong\":%d,\"overlap_wrong\":%d,\"exact\":%s,\"pm4\":%lu}",
            restored ? "true" : "false",stateWrong,overlapWrong,allGood && restored && stateWrong == 0 ? "true" : "false",
            static_cast<unsigned long>(card.Read(0x07b8)));
        return out + text;
    }
}
