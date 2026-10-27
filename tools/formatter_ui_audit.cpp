// Compiled only into FORMATTER_AUDIT UI builds. Driven by RCT_REMOTE console commands.
#include <openrct2/Context.h>
#include <openrct2/config/Config.h>
#include <openrct2/GameState.h>
#include <openrct2/Game.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/park/ParkSetParameterAction.h>
#include <openrct2/Input.h>
#include <openrct2/interface/WindowTypes.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/interface/Widget.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/localisation/FormatterAudit.h>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/localisation/StringWithArgs.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/TrackDesignRepository.h>
#include <openrct2/ride/TrackDesign.h>
#include <openrct2/world/Banner.h>
#include <openrct2/management/Award.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/management/Research.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/TileInspector.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/windows/TileInspectorGlobals.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/paint/entity/Paint.Peep.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <cstdio>
#include <stdexcept>
#include <set>
#include <tuple>

using namespace OpenRCT2;

void FormatterAuditUiCommand(const std::vector<std::string>& args)
{
    try
    {
        if (args.empty()) throw std::invalid_argument("missing audit_ui operation");
        auto* manager = Ui::GetWindowManager();
        if (args[0] == "inspectorseek" || args[0] == "inspectorselect")
        {
            static CoordsXYZ target{};
            static bool found = false;
            const auto kind = std::stoi(args.at(1));
            if (args[0] == "inspectorseek")
            {
                found = false;
                TileElementIterator it{};
                TileElementIteratorBegin(&it);
                while (TileElementIteratorNext(&it))
                {
                    if (EnumValue(it.element->getType()) != kind || it.x < 16 || it.y < 16) continue;
                    const CoordsXY xy{it.x * 32 + 16, it.y * 32 + 16};
                    target = {xy, TileElementHeight(xy)};
                    WindowScrollToLocation(*manager->FindByClass(WindowClass::mainWindow), target);
                    found = true;
                    break;
                }
                std::printf("AUDIT_INSPECTOR seek kind=%d found=%d target=%d,%d,%d\n", kind, found, target.x,target.y,target.z);
            }
            else if (found)
            {
                const auto* viewport = manager->FindByClass(WindowClass::mainWindow)->viewport;
                auto point = Translate3DTo2DWithZ(viewport->rotation, target) - viewport->viewPos;
                point = {viewport->zoom.ApplyInversedTo(point.x) + viewport->pos.x,
                    viewport->zoom.ApplyInversedTo(point.y) + viewport->pos.y};
                auto* window = manager->OpenWindow(WindowClass::tileInspector);
                window->windowPos = {450,40};
                window->onToolDown(0, point);
                // Elevated/water scenery can make the screen picker choose a neighbouring tile.
                // The real inspector X/Y spinners select the requested tile without changing the map.
                const auto tile = TileCoordsXY{target};
                while (windowTileInspectorTile.x != tile.x)
                    window->onMouseDown(windowTileInspectorTile.x < tile.x ? 6 : 7);
                while (windowTileInspectorTile.y != tile.y)
                    window->onMouseDown(windowTileInspectorTile.y < tile.y ? 10 : 11);
                int index = 0;
                bool selected = false;
                for (const auto* element : TileElementsView(windowTileInspectorTile.toCoordsXY()))
                {
                    if (EnumValue(element->getType()) == kind)
                    {
                        window->onScrollMouseDown(0, {100, 1 + (windowTileInspectorElementCount - index - 1) * kScrollableRowHeight});
                        selected = true;
                        break;
                    }
                    ++index;
                }
                window->onPrepareDraw();
                window->invalidate();
                std::printf("AUDIT_INSPECTOR select kind=%d selected=%d tile=%d,%d count=%d index=%d\n",
                    kind, selected, windowTileInspectorTile.x,windowTileInspectorTile.y,
                    windowTileInspectorElementCount, windowTileInspectorSelectedIndex);
                if (!selected) throw std::runtime_error("inspector target selection failed");
            }
        }
        else if (args[0] == "trackwindow")
        {
            static std::vector<TrackDesignFileRef> designs;
            bool opened = false;
            for (auto& ride : RideManager(getGameState()))
            {
                designs = GetContext()->GetTrackDesignRepository()->GetItemsForObjectEntry(ride.type, "");
                if (designs.empty()) continue;
                if (args.at(1) == "place") Ui::Windows::TrackPlaceOpen(&designs.front());
                else if (args.at(1) == "manage") Ui::Windows::TrackManageOpen(&designs.front());
                else throw std::invalid_argument("unknown track window");
                std::printf("AUDIT_TRACK_WINDOW %s %s\n", args.at(1).c_str(), designs.front().path.c_str());
                opened = true;
                break;
            }
            if (args.at(1) == "install" && !opened)
            {
                // InstallTrackOpen unloads park objects: use only the empty-start installation session.
                if (getGameState().park.numGuestsInPark != 0) throw std::runtime_error("installer needs empty test park");
                std::vector<Guest*> arriving;
                for (auto* guest : EntityList<Guest>())
                {
                    if (!guest->outsideOfPark) throw std::runtime_error("installer test guest is inside park");
                    arriving.push_back(guest);
                }
                for (auto* guest : arriving) PeepEntityRemove(guest);
                std::printf("AUDIT_INSTALL_CONTEXT removed_arriving_guests=%zu\n", arriving.size());
                manager->ForceClose(WindowClass::mainWindow);
                const char* path = "/RCT/game/Assets/Black Widow.td6";
                Ui::Windows::InstallTrackOpen(path);
                std::printf("AUDIT_TRACK_WINDOW install %s\n", path);
            }
        }
        else if (args[0] == "tracks")
        {
            Ride* ride = nullptr;
            for (auto& candidate : RideManager(getGameState())) { ride = &candidate; break; }
            std::printf("AUDIT_TRACKS repository=%zu ride=%d\n", GetContext()->GetTrackDesignRepository()->GetCount(), ride != nullptr);
            if (ride)
            {
                auto designs = GetContext()->GetTrackDesignRepository()->GetItemsForObjectEntry(ride->type, "");
                if (!designs.empty())
                {
                    const auto td = TrackDesignImport(designs.front().path.c_str());
                    if (td) std::printf("AUDIT_TRACK_IMPORT path=%s length=%u G=%d,%d,%d rating=%d,%d,%d scenery=%zu\n",
                        designs.front().path.c_str(), td->statistics.rideLength, td->statistics.maxPositiveVerticalG,
                        td->statistics.maxNegativeVerticalG, td->statistics.maxLateralG,
                        td->statistics.ratings.excitement, td->statistics.ratings.intensity, td->statistics.ratings.nausea,
                        td->sceneryElements.size());
                    if (td)
                    {
                        auto pixels = std::make_unique<TrackDesignPreviewBuffer>();
                        TrackDesignDrawPreview(*td, *pixels, true);
                        std::printf("AUDIT_TRACK_PREVIEW G=%d,%d,%d scenery_unavailable=%d cost=%lld\n",
                            td->statistics.maxPositiveVerticalG, td->statistics.maxNegativeVerticalG,
                            td->statistics.maxLateralG, td->gameStateData.flags.has(TrackDesignGameStateFlag::sceneryUnavailable),
                            static_cast<long long>(td->gameStateData.cost));
                    }
                }
                Intent intent(WindowClass::trackDesignList);
                intent.PutExtra(INTENT_EXTRA_RIDE_TYPE, static_cast<uint32_t>(ride->type));
                intent.PutExtra(INTENT_EXTRA_RIDE_ENTRY_INDEX, static_cast<uint32_t>(ride->subtype));
                manager->OpenIntent(&intent);
            }
        }
        else if (args[0] == "banners")
        {
            bool opened = false;
            unsigned count = 0;
            for (unsigned i = 0; i < GetNumBanners(); ++i)
            {
                auto* banner = GetBanner(BannerIndex::FromUnderlying(i));
                if (!banner || banner->isNull()) continue;
                ++count;
                Formatter ft;
                banner->formatTextWithColourTo(ft);
                std::printf("AUDIT_BANNER %u %s\n", i, FormatStringIDLegacy(STR_STRINGID, ft.Data()).c_str());
                if (!opened && !banner->flags.has(BannerFlag::isLargeScenery) && !banner->flags.has(BannerFlag::isWall))
                    opened = Ui::Windows::BannerOpen(i) != nullptr;
            }
            std::printf("AUDIT_BANNERS count=%u window=%d\n", count, opened);
        }
        else if (args[0] == "maptips")
        {
            std::set<std::tuple<unsigned, unsigned, unsigned>> seen;
            TileElementIterator it{};
            TileElementIteratorBegin(&it);
            while (TileElementIteratorNext(&it))
            {
                const auto& element = *it.element;
                unsigned kind = 0, station = 0;
                if (auto* track = element.asTrack())
                {
                    kind = track->isStation() ? 2 : 1;
                    station = track->getStationIndex().ToUnderlying();
                }
                else if (auto* entrance = element.asEntrance())
                {
                    if (entrance->getEntranceType() == EntranceType::parkEntrance) continue;
                    kind = entrance->getEntranceType() == EntranceType::rideEntrance ? 3 : 4;
                    station = entrance->getStationIndex().ToUnderlying();
                }
                else if (auto* path = element.asPath(); path && path->isQueue()) kind = 5;
                else continue;
                const auto ride = element.getRideIndex();
                if (!GetRide(ride) || !seen.emplace(kind, ride.ToUnderlying(), station).second) continue;
                RideSetMapTooltip(element);
                std::printf("AUDIT_MAP_TIP kind=%u ride=%u station=%u %s\n", kind, ride.ToUnderlying(), station,
                    FormatStringIDLegacy(STR_MAP_TOOLTIP_STRINGID, Ui::Windows::GetMapTooltip().Data()).c_str());
            }
            for (const auto* vehicle : EntityList<Vehicle>())
            {
                if (!GetRide(vehicle->ride)) continue;
                vehicle->SetMapToolbar();
                std::printf("AUDIT_MAP_TIP vehicle=%u %s\n", vehicle->id.ToUnderlying(),
                    FormatStringIDLegacy(STR_MAP_TOOLTIP_STRINGID, Ui::Windows::GetMapTooltip().Data()).c_str());
            }
            std::printf("AUDIT_MAP_TIP tile layouts=%zu\n", seen.size());
        }
        else if (args[0] == "notifications")
        {
            for (unsigned i = 0; i < EnumValue(AwardType::count); ++i) AwardGrant(static_cast<AwardType>(i));
            auto& state = getGameState();
            Ride* firstRide = nullptr;
            for (auto& ride : RideManager(state)) { firstRide = &ride; break; }
            if (firstRide)
            {
                RideBreakdownAddNewsItem(*firstRide);
                RidePrepareBreakdown(*firstRide, Breakdown::safetyCutOut);
            }
            // Exercise the actual completion producer for every campaign family.
            const auto previous = state.park.marketingCampaigns;
            state.park.marketingCampaigns.clear();
            for (unsigned i = 0; i < ADVERTISING_CAMPAIGN_COUNT; ++i)
            {
                if (!firstRide && (i == ADVERTISING_CAMPAIGN_RIDE || i == ADVERTISING_CAMPAIGN_RIDE_FREE)) continue;
                MarketingCampaign campaign{};
                campaign.type = i;
                campaign.weeksLeft = 1;
                if (i == ADVERTISING_CAMPAIGN_FOOD_OR_DRINK_FREE) campaign.shopItemType = ShopItem::burger;
                else campaign.rideId = firstRide ? firstRide->id : RideId::GetNull();
                state.park.marketingCampaigns.push_back(campaign);
            }
            MarketingUpdate();
            state.park.marketingCampaigns = previous;
            const auto items = state.researchItemsInvented;
            std::set<unsigned> kinds;
            for (const auto& item : items)
            {
                if (!kinds.insert(EnumValue(item.type)).second) continue;
                ResearchFinishItem(item);
            }
            for (const auto& item : state.newsItems.getRecent()) std::printf("AUDIT_NEWS %s\n", item.text.c_str());
            std::printf("AUDIT_NOTIFICATIONS research kinds=%zu ride=%d\n", kinds.size(), firstRide != nullptr);
        }
        else if (args[0] == "entity")
        {
            for (auto* guest : EntityList<Guest>())
            {
                if (guest->id.ToUnderlying() != std::stoi(args.at(1))) continue;
                std::printf("AUDIT_ENTITY tick=%u id=%u pos=%d,%d,%d orientation=%u group=%u object=%u colours=%u,%u sprites=",
                    getGameState().currentTicks, guest->id.ToUnderlying(), guest->x, guest->y, guest->z,
                    guest->orientation, EnumValue(guest->animationGroup), guest->animationObjectIndex,
                    EnumValue(guest->tShirtColour), EnumValue(guest->trousersColour));
                for (unsigned rotation = 0; rotation < 4; ++rotation)
                {
                    const auto direction = ((guest->orientation + rotation * 8) & 31) >> 3;
                    const auto image = PaintPeepGetBaseImageAndOffset(*guest, direction);
                    std::printf("%u%s", image.baseImageId, rotation == 3 ? "\n" : ",");
                }
            }
        }
        else if (args[0] == "speed")
        {
            gGameSpeed = std::stoi(args.at(1));
        }
        else if (args[0] == "parkopen")
        {
            // No callback may retain the temporary RCT_REMOTE console object.
            GameActions::ParkSetParameterAction action(GameActions::ParkParameter::open);
            GameActions::Execute(&action, getGameState());
        }
        else if (args[0] == "dismiss")
        {
            Ui::Windows::WindowDropdownClose();
            InputSetState(InputState::reset);
        }
        else if (args[0] == "open")
        {
            const auto cls = static_cast<WindowClass>(std::stoi(args.at(1)));
            auto* window = manager->OpenWindow(cls);
            std::printf("AUDIT_UI open %u found=%d\n", EnumValue(cls), window != nullptr);
        }
        else if (args[0] == "guest" || args[0] == "staff")
        {
            Peep* peep = nullptr;
            if (args[0] == "guest") { for (auto* entity : EntityList<Guest>()) { peep = entity; break; } }
            else { for (auto* entity : EntityList<Staff>()) { peep = entity; break; } }
            if (!peep) throw std::runtime_error("no requested peep in park");
            Intent intent(WindowClass::peep);
            intent.PutExtra(INTENT_EXTRA_PEEP, peep);
            manager->OpenIntent(&intent);
            PeepSetMapTooltip(peep);
            std::printf("AUDIT_MAP_TIP %s\n", FormatStringIDLegacy(STR_MAP_TOOLTIP_STRINGID,
                Ui::Windows::GetMapTooltip().Data()).c_str());
            std::printf("AUDIT_UI peep %u\n", peep->id.ToUnderlying());
        }
        else if (args[0] == "loadsave" || args[0] == "scenarios")
        {
            Intent intent(args[0] == "loadsave" ? WindowClass::loadsave : WindowClass::scenarioSelect);
            if (args[0] == "loadsave")
            {
                Config::Get().general.fileBrowserShowSizeColumn = true;
                intent.PutExtra(INTENT_EXTRA_LOADSAVE_ACTION, EnumValue(LoadSaveAction::load));
                intent.PutExtra(INTENT_EXTRA_LOADSAVE_TYPE, EnumValue(LoadSaveType::park));
                intent.PutExtra(INTENT_EXTRA_PATH, std::string("/RCT/user/g3"));
            }
            manager->OpenIntent(&intent);
        }
        else if (args[0] == "input")
        {
            const auto id = args.at(1) == "integer" ? STR_FORMAT_INTEGER : STR_FORMAT_COMMA2DP32;
            Formatter bounds;
            bounds.Add<uint16_t>(15).Add<uint16_t>(1000);
            Ui::Windows::WindowTextInputOpen(nullptr, 0, STR_MAP_SIZE_2, STR_ENTER_MAP_SIZE, bounds, id,
                static_cast<uintptr_t>(std::stoll(args.at(2))), 20);
        }
        else if (args[0] == "objective")
        {
            auto& objective = getGameState().scenarioOptions.objective;
            objective.Type = static_cast<Scenario::ObjectiveType>(std::stoi(args.at(1)));
            objective.NumGuests = 1234;
            objective.Year = 3;
            objective.Currency = 123450;
            if (objective.Type == Scenario::ObjectiveType::tenRollercoastersLength) objective.MinimumLength = 1234;
            if (objective.Type == Scenario::ObjectiveType::finishFiveRollercoasters) objective.MinimumExcitement = 670;
            manager->OpenWindow(WindowClass::editorScenarioOptions);
        }
        else
        {
            const auto cls = static_cast<WindowClass>(std::stoi(args.at(1)));
            auto* window = manager->FindByClass(cls);
            if (!window) throw std::runtime_error("requested window not open");
            if (args[0] == "widgets")
            {
                for (size_t i = 0; i < window->widgets.size(); ++i)
                {
                    const auto& widget = window->widgets[i];
                    std::printf("AUDIT_WIDGET %u %zu %u %u %u %d %d %d %d\n", EnumValue(cls), i,
                        EnumValue(widget.type), widget.text, widget.tooltip,
                        window->windowPos.x + widget.left, window->windowPos.y + widget.top,
                        window->windowPos.x + widget.right, window->windowPos.y + widget.bottom);
                }
            }
            else if (args[0] == "tips")
            {
                for (size_t i = 0; i < window->widgets.size(); ++i)
                {
                    const auto& widget = window->widgets[i];
                    if (widget.tooltip == kStringIdNone || widget.flags.has(WidgetFlag::isHidden)) continue;
                    auto text = window->onTooltip(i, widget.tooltip);
                    std::printf("AUDIT_TIP %u %zu %s\n", EnumValue(cls), i,
                        FormatStringIDLegacy(text.str, text.args.Data()).c_str());
                }
            }
            else
            {
                auto index = std::stoi(args.at(2));
                if (args[0] == "tab" || args[0] == "drop")
                {
                    auto ordinal = index;
                    index = -1;
                    for (size_t i = 0; i < window->widgets.size(); ++i)
                        if (window->widgets[i].type == (args[0] == "tab" ? WidgetType::tab : WidgetType::dropdownMenu)
                            && ordinal-- == 0) { index = i; break; }
                    if (index < 0) { std::printf("AUDIT_UI no %s %u %s\n", args[0].c_str(), EnumValue(cls), args[2].c_str()); return; }
                    if (args[0] == "drop") ++index;
                }
                if (index < 0 || static_cast<size_t>(index) >= window->widgets.size())
                    throw std::invalid_argument("bad widget index");
                gPressedWidget = { window->classification, window->number, static_cast<WidgetIndex>(index) };
                if (args[0] == "tab") window->onMouseDown(index);
                if (args[0] == "down" || args[0] == "drop") window->onMouseDown(index);
                else if (args[0] == "up" || args[0] == "tab") window->onMouseUp(index);
                else throw std::invalid_argument("unknown audit_ui operation");
                window->invalidate();
                window->onPrepareDraw();
            }
        }
    }
    catch (const std::exception& error) { std::printf("AUDIT_UI ERROR %s\n", error.what()); }
}
