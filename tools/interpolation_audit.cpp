// Bounded, opt-in graphical replay evidence. Linked only into Linux rctgame.
// It observes the real frame loop; it is not a headless interpolation substitute.
#include <openrct2/GameState.h>
#include <openrct2/entity/EntityTweener.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/Ride.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/util/Util.h>
#include <openrct2/core/OrcaStream.hpp>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace OpenRCT2;
using json = nlohmann::json;
namespace
{
    FILE* file;
    FILE* fields;
    std::string path;
    unsigned picture = 0, records = 0;
    void Trace(uint32_t chunk, uint64_t at, uint64_t size, const char* kind, std::source_location loc)
    {
        if (chunk != 0x32) return;
        std::fprintf(fields,"%x\t%lu\t%lu\t%s\t%s\t%u\n",chunk,
            static_cast<unsigned long>(at),static_cast<unsigned long>(size),kind,loc.file_name(),loc.line());
    }
    void Lifecycle()
    {
        auto& registry = getGameState().entities;
        auto& tween = EntityTweener::get(); tween.restore(); tween.reset();
        Guest* source = nullptr;
        for (auto* e : EntityList<Guest>())
            for (const auto& vp : GetAllViewports())
                if (vp.isVisible && vp.zoom <= ZoomLevel{0} && vp.Contains(Translate3DTo2DWithZ(vp.rotation,e->getLocation())))
                    source = e;
        if (!source) throw std::runtime_error("lifecycle needs eligible guest");
        auto* e = registry.createEntity<Guest>(); const auto id = e->id; const auto index = e->spatialIndex;
        *e = *source; e->id = id; e->spatialIndex = index; e->x = kLocationNull;
        e->moveToAndUpdateSpatialIndex(source->getLocation());
        const auto a = e->getLocation(), b = a + CoordsXYZ{8,8,0};
        tween.preTick(); e->moveTo(b); e->spriteData.width += 5; tween.postTick();
        for (float alpha : {0.f,.5f,1.f})
        {
            tween.tween(alpha);
            const auto want = a + CoordsXYZ{int32_t(8*alpha),int32_t(8*alpha),0};
            const auto screen = Translate3DTo2DWithZ(GetCurrentRotation(),want);
            if (e->getLocation() != want || e->spriteData.spriteRect.getLeft() != screen.x-e->spriteData.width)
                throw std::runtime_error("lifecycle interpolation/bounds mismatch");
        }
        tween.restore();
        if (e->getLocation() != b) throw std::runtime_error("lifecycle restore mismatch");
        tween.reset(); tween.preTick(); e->moveTo(b+CoordsXYZ{8,8,0}); tween.postTick();
        registry.entityRemove(e);
        auto* replacement = registry.createEntityAt<Guest>(id); const auto replacementIndex = replacement->spatialIndex;
        *replacement = *source; replacement->id = id; replacement->spatialIndex = replacementIndex; replacement->x = kLocationNull;
        replacement->moveToAndUpdateSpatialIndex(source->getLocation());
        const auto replacementPosition = replacement->getLocation();
        tween.tween(.5f); tween.restore();
        if (replacement->getLocation() != replacementPosition) throw std::runtime_error("removed entity was reused by tweener");
        tween.reset(); registry.entityRemove(replacement);
        std::puts("PASS interpolation lifecycle: creation, 0/.5/1, changed width, restored bounds, removal and ID reuse");
    }
    // A real pose transition after the first tick of a catch-up picture. The
    // next real Tick consumes it. No tweener method is called by this fixture.
    EntityId poseId = EntityId::GetNull();
    json poseEvent;
    std::vector<std::pair<EntityId, CoordsXYZ>> previous;
    unsigned tickInPicture = 0;
    void IntermediatePose(const char* phase)
    {
        if (!std::getenv("RCT_INTERPOLATION_INTERMEDIATE_POSE")) return;
        if (std::strcmp(phase, "picture") == 0) { tickInPicture = 0; return; }
        if (std::strcmp(phase, "afterTick") != 0) return;
        ++tickInPicture;
        if (picture == 6 && tickInPicture == 1)
        {
            auto* main = WindowGetMain();
            WindowBase* ride = nullptr;
            WindowVisitEach([&](WindowBase* w) { if (w != main && w->viewport) ride = w; });
            if (!main || !main->viewport || !ride) throw std::runtime_error("pose case needs main and ride views");
            Guest* guest = nullptr;
            for (const auto& [id, pos] : previous)
            {
                auto* e = getGameState().entities.getEntity<Guest>(id);
                if (!e || e->getLocation() == pos || e->state != PeepState::walking) continue;
                if (main->viewport->Contains(Translate3DTo2DWithZ(main->viewport->rotation, e->getLocation())))
                { guest = e; break; }
            }
            if (!guest) throw std::runtime_error("pose case needs a moving walking guest");
            poseId = guest->id;
            const auto before = guest->spriteData;
            guest->action = PeepActionType::wave;
            guest->animationFrameNum = guest->animationImageIdOffset = 0;
            guest->updateCurrentAnimationType();
            if (guest->spriteData.width == before.width && guest->spriteData.heightMin == before.heightMin
                && guest->spriteData.heightMax == before.heightMax)
                throw std::runtime_error("pose case did not change sprite dimensions");
            // Main remains the listening viewport; an eligible ride view keeps
            // this guest interpolatable even outside the main's anchor bounds.
            auto* vp = main->viewport;
            const auto screen = Translate3DTo2DWithZ(vp->rotation, guest->getLocation());
            vp->viewPos = {screen.x + std::max(before.width, guest->spriteData.width), screen.y - 100};
            main->savedViewPos = vp->viewPos;
            auto* rv = ride->viewport; rv->zoom = ZoomLevel{0};
            const auto rs = Translate3DTo2DWithZ(rv->rotation, guest->getLocation());
            rv->viewPos = {rs.x - rv->ViewWidth()/2, rs.y - rv->ViewHeight()/2};
            ride->savedViewPos = rv->viewPos;
            rv->flags.unset(ViewportFlag::soundOn); vp->flags.set(ViewportFlag::soundOn);
            gMusicTrackingViewport = vp;
            poseEvent = {{"id",poseId.ToUnderlying()}, {"tick",getGameState().currentTicks},
                {"before_dimensions",{before.width,before.heightMin,before.heightMax}},
                {"after_dimensions",{guest->spriteData.width,guest->spriteData.heightMin,guest->spriteData.heightMax}},
                {"listening_edge",vp->viewPos.x}, {"ride_rotation",rv->rotation}};
        }
        previous.clear();
        for (auto* e : EntityList<Guest>()) previous.emplace_back(e->id,e->getLocation());
    }
    void Audit(const char* phase)
    {
        if (!file || ++records > 1024) return;
        IntermediatePose(phase);
        auto& state = getGameState();
        const auto& rng = ScenarioRandState();
        json row = {{"phase",phase},{"picture",picture},{"tick",state.currentTicks},{"rng",{rng.s0,rng.s1}},
            {"crowd",{gInterpolationStats.crowdWeight,gInterpolationStats.crowdVolume}}};
        if (!poseEvent.is_null())
        {
            row["intermediate_pose"] = poseEvent;
            auto* e = getGameState().entities.getEntity<Guest>(poseId);
            if (e) row["pose_guest"] = {e->id.ToUnderlying(),e->x,e->y,e->z,
                e->spriteData.width,e->spriteData.heightMin,e->spriteData.heightMax,
                e->spriteData.spriteRect.getLeft(),e->spriteData.spriteRect.getRight(),
                EnumValue(e->animationType),e->animationImageIdOffset};
        }
        // Reload can destroy the tracker before Ride::updateAll refreshes it.
        // Observe its liveness without dereferencing a retired viewport.
        const auto& viewports = GetAllViewports();
        const bool listeningViewLive = gMusicTrackingViewport != nullptr
            && std::any_of(viewports.begin(), viewports.end(), [](const Viewport& viewport) {
                return &viewport == gMusicTrackingViewport;
            });
        row["listening_view_live"] = listeningViewLive;
        if (listeningViewLive) row["listening_view"] = {gMusicTrackingViewport->viewPos.x,gMusicTrackingViewport->viewPos.y,
            gMusicTrackingViewport->ViewWidth(),gMusicTrackingViewport->ViewHeight(),gMusicTrackingViewport->rotation};
        json entities = json::array();
        const auto add = [&](auto* e) {
            const auto& r = e->spriteData.spriteRect;
            entities.push_back({e->id.ToUnderlying(), e->x,e->y,e->z,e->spriteData.width,
                e->spriteData.heightMin,e->spriteData.heightMax,r.getLeft(),r.getTop(),r.getRight(),r.getBottom(),
                e->spatialIndex});
        };
        for (auto* e : EntityList<Guest>()) add(e);
        for (auto* e : EntityList<Staff>()) add(e);
        for (auto* e : EntityList<Vehicle>()) add(e);
        row["entities"] = std::move(entities);
        if (std::strcmp(phase,"afterTick") == 0)
        {
            // Canonical exported chunks, with timestamps/authoring excluded.
            MemoryStream bytes; ParkFileExporter exporter; exporter.SkipPreviewImages = true;
            const auto prefix = path + "-" + std::to_string(picture) + "-" + std::to_string(state.currentTicks);
            if (picture == 4 || picture == 29)
            {
                fields = std::fopen((prefix+".fields.tsv").c_str(),"w");
                if (!fields) throw std::runtime_error("cannot open field audit");
                OrcaStream::writeTrace = Trace;
            }
            exporter.Export(state, bytes, 0);
            if (fields)
            {
                OrcaStream::writeTrace = nullptr; std::fclose(fields); fields = nullptr;
                auto* dump = std::fopen((prefix+".park").c_str(),"wb");
                if (!dump) throw std::runtime_error("cannot open field park");
                std::fwrite(bytes.GetData(),1,bytes.GetLength(),dump); std::fclose(dump);
            }
            auto* b = static_cast<const uint8_t*>(bytes.GetData());
            const auto le = [](const uint8_t* p, unsigned n) { uint64_t v=0;
                for (unsigned i=0;i<n;i++) v |= uint64_t(p[i]) << (8*i);
                return v; };
            const auto count = le(b+12,4), body = 64 + count*20;
            json chunks = json::array();
            for (unsigned i=0;i<count;i++)
            {
                const auto* entry = b+64+i*20; const auto id = le(entry,4);
                if (id == 1) continue;
                const auto at=le(entry+4,8), len=le(entry+12,8);
                uint64_t hash = 14695981039346656037ULL;
                for (uint64_t j=0;j<len;j++) { hash ^= b[body+at+j]; hash *= 1099511628211ULL; }
                chunks.push_back({id,len,hash});
            }
            row["chunks"] = std::move(chunks);
            // Playback offsets depend on wall-clock audio. Observe raw fields
            // above; a second export masks only musicPosition, restores it
            // immediately, and never ticks/draws or changes the fixture here.
            struct AudioPositions
            {
                std::vector<std::pair<Ride*,uint32_t>> saved;
                AudioPositions() { for (auto& ride : RideManager(getGameState())) {
                    saved.emplace_back(&ride,ride.musicPosition); ride.musicPosition = 0; } }
                ~AudioPositions() { for (const auto& [ride,value] : saved) ride->musicPosition = value; }
            } audio;
            MemoryStream normalized; ParkFileExporter cleanExporter; cleanExporter.SkipPreviewImages = true;
            cleanExporter.Export(state,normalized,0);
            b = static_cast<const uint8_t*>(normalized.GetData());
            json clean = json::array();
            for (unsigned i=0;i<count;i++)
            {
                const auto* entry = b+64+i*20; const auto id = le(entry,4);
                if (id == 1) continue;
                const auto at=le(entry+4,8), len=le(entry+12,8);
                uint64_t hash = 14695981039346656037ULL;
                for (uint64_t j=0;j<len;j++) { hash ^= b[body+at+j]; hash *= 1099511628211ULL; }
                clean.push_back({id,len,hash});
            }
            row["chunks_without_audio_position"] = std::move(clean);
        }
        const auto line = row.dump(); std::fwrite(line.data(),1,line.size(),file); std::fputc('\n',file);
        if (std::strcmp(phase,"picture") == 0)
        {
            if (picture == 78 && std::getenv("RCT_INTERPOLATION_LIFECYCLE")) Lifecycle();
            picture++;
        }
    }
}
void InitialiseInterpolationAudit()
{
    if (const auto* filename = std::getenv("RCT_INTERPOLATION_AUDIT"))
    {
        path = filename;
        file = std::fopen(filename,"wb");
        if (!file) throw std::runtime_error("cannot open interpolation audit");
        gInterpolationProbe = true;
        gInterpolationAudit = Audit;
        UtilRandSeed(0x1234567F);
    }
}
