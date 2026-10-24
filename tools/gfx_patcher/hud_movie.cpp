#include "hud_movie.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace erui::gfx {
namespace {

constexpr std::uint16_t kTagEnd = 0;
constexpr std::uint16_t kTagPlaceObject2 = 26;
constexpr std::uint16_t kTagDefineSprite = 39;
constexpr std::uint16_t kTagPlaceObject3 = 70;
constexpr float kStageWidth = 1920.0f;
constexpr float kStageHeight = 1080.0f;
// Vanilla gauge bar art spans 1452.7 stage px right of the bar origin at
// scale 1 (HP/MP/SP 136..1607 with origin 154.3). Tarnished HUD sets the
// placements to sx 1.21, so the art ends at 1912.
constexpr double kBarArtRight = 1452.7;
constexpr double kBarFitThreshold = kStageWidth - 70.0;  // ends at or near the 16:9 edge
constexpr std::int32_t kFixedOne = 0x10000;

// stretch: widen in X around the stage centre (one-piece backdrops);
// cover: scale X and Y around the centre (full-screen pictures, crops top/bottom).
// bar_fit: gauge bars a mod drew up to the 16:9 right edge (Tarnished HUD,
// Adaptive Bar Scaling) get their X scale recomputed so they end at the real
// screen edge; vanilla-length bars are left as they are.
// side: left or right, whichever half of the stage the loaded movie puts the
// element in (mods move some corner elements to the other side).
enum class Horizontal : std::uint8_t { left, center, right, stretch, cover, bar_fit, side };
enum class Vertical : std::uint8_t { top, middle, bottom };

struct AnchorRule {
    const char* movie;  // file name (see kMovies)
    const char* parent;  // root placement holding the element, or nullptr
    const char* name;     // instance name, or a label when depth is set
    Horizontal horizontal;
    Vertical vertical;
    std::int16_t depth{-1};  // >= 0: unnamed placement found by its depth
};

// Vanilla 2.7.1 positions (stage pixels) in the comments. Only elements that
// sit at a screen edge or corner are listed: centred messages and the
// world-projected markers (EnemyTag*, LockOn, TargetCursor, PlayerTag) are
// placed by the game every frame and stay untouched.
constexpr AnchorRule kRules[] = {
    // PlayerHUD is a container at the stage centre; its children are the
    // real corner blocks.
    {"01_000_fe.gfx", "PlayerHUD", "Gauge", Horizontal::left, Vertical::top},          // 7,125
    {"01_000_fe.gfx", "PlayerHUD", "ItemPanel", Horizontal::left, Vertical::bottom},   // 232,846
    {"01_000_fe.gfx", "PlayerHUD", "ItemPanel2", Horizontal::left, Vertical::bottom},  // 232,846
    {"01_000_fe.gfx", "PlayerHUD", "PersonalCompass", Horizontal::center, Vertical::top},
    {"01_000_fe.gfx", nullptr, "RemotePCTag0", Horizontal::left, Vertical::middle},    // 197,286..756
    {"01_000_fe.gfx", nullptr, "RemotePCTag1", Horizontal::left, Vertical::middle},
    {"01_000_fe.gfx", nullptr, "RemotePCTag2", Horizontal::left, Vertical::middle},
    {"01_000_fe.gfx", nullptr, "RemotePCTag3", Horizontal::left, Vertical::middle},
    {"01_000_fe.gfx", nullptr, "RemotePCTag4", Horizontal::left, Vertical::middle},
    {"01_000_fe.gfx", nullptr, "RemotePCTag5", Horizontal::left, Vertical::middle},
    {"01_000_fe.gfx", nullptr, "Buddy", Horizontal::left, Vertical::middle},           // 23,561
    {"01_000_fe.gfx", nullptr, "BossList", Horizontal::center, Vertical::bottom},      // 947,908
    {"01_000_fe.gfx", nullptr, "ActionGuide", Horizontal::right, Vertical::bottom},    // 1902,920
    {"01_000_fe.gfx", nullptr, "GetItemLog", Horizontal::right, Vertical::bottom},     // 1409,820
    {"01_001_fe_soul.gfx", nullptr, "Soul", Horizontal::side, Vertical::bottom},          // 1762,1034
    {"01_002_fe_saveicon.gfx", nullptr, "AutoSaveIcon", Horizontal::side, Vertical::top},  // 1800,96 (Convergence: 56,130)
    // Big notices ("LOST GRACE DISCOVERED", "YOU DIED", "ENEMY FELLED"...):
    // every TextEffect_N reuses sprite 602, whose only child is the
    // MENU_KUROOBI band (-64..1984), so widening it inside 602 covers all of
    // them; TextEffect_50 also has a full-screen dimmer (0..1920). The text
    // stays centred and unscaled.
    {"01_000_fe.gfx", "TextEffect.TextEffect_100.#1", "notice band", Horizontal::stretch, Vertical::middle, 1},
    {"01_000_fe.gfx", "TextEffect.TextEffect_50", "notice dimmer", Horizontal::stretch, Vertical::middle, 1},
    // Gauge bars drawn to the 16:9 edge by a bar mod reach the real edge.
    {"01_000_fe.gfx", "PlayerHUD.Gauge", "HP", Horizontal::bar_fit, Vertical::top},
    {"01_000_fe.gfx", "PlayerHUD.Gauge", "MP", Horizontal::bar_fit, Vertical::top},
    {"01_000_fe.gfx", "PlayerHUD.Gauge", "SP", Horizontal::bar_fit, Vertical::top},
    {"01_000_fe.gfx", "PlayerHUD.Gauge", "HPRegain", Horizontal::bar_fit, Vertical::top},

    // Pause menu: everything lives in ItemList (580,636). Left column of
    // entries + key guide; right column with the pouch and gesture slots.
    // The dark backdrop is MENU_TopBase_00 (740 px wide on the stage) placed
    // twice, the right copy mirrored; each half follows its column so the
    // screen edges stay covered.
    {"02_000_ingametop.gfx", "ItemList", "backdrop left", Horizontal::left, Vertical::middle, 1},    // -152..588
    {"02_000_ingametop.gfx", "ItemList", "backdrop right", Horizontal::right, Vertical::middle, 3},  // 1343..2083
    {"02_000_ingametop.gfx", "ItemList", "Item_0_0", Horizontal::left, Vertical::middle},            // 88
    {"02_000_ingametop.gfx", "ItemList", "Item_0_1", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_2", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_3", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_4", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_5", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_6", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_0_7", Horizontal::left, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "StatusBar", Horizontal::left, Vertical::middle},           // key guide
    {"02_000_ingametop.gfx", "ItemList", "pouch/gesture divider 1", Horizontal::right, Vertical::middle, 5},
    {"02_000_ingametop.gfx", "ItemList", "pouch/gesture divider 2", Horizontal::right, Vertical::middle, 6},
    {"02_000_ingametop.gfx", "ItemList", "gestures heading icon", Horizontal::right, Vertical::middle, 7},
    {"02_000_ingametop.gfx", "ItemList", "pouch heading icon", Horizontal::right, Vertical::middle, 8},
    {"02_000_ingametop.gfx", "ItemList", "pouch slot base 1", Horizontal::right, Vertical::middle, 170},
    {"02_000_ingametop.gfx", "ItemList", "pouch slot base 2", Horizontal::right, Vertical::middle, 173},
    {"02_000_ingametop.gfx", "ItemList", "pouch slot base 3", Horizontal::right, Vertical::middle, 176},
    {"02_000_ingametop.gfx", "ItemList", "pouch slot base 4", Horizontal::right, Vertical::middle, 179},
    {"02_000_ingametop.gfx", "ItemList", "Item_1_0", Horizontal::right, Vertical::middle},           // 1784
    {"02_000_ingametop.gfx", "ItemList", "Item_1_1", Horizontal::right, Vertical::middle},           // 1864
    {"02_000_ingametop.gfx", "ItemList", "Item_1_2", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_1_3", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_1_4", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_1_5", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_0", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_1", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_2", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_3", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_4", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "Item_2_5", Horizontal::right, Vertical::middle},
    {"02_000_ingametop.gfx", "ItemList", "CurrentItem", Horizontal::right, Vertical::middle},        // item names
    {"02_000_ingametop.gfx", "ItemList", "RowText", Horizontal::right, Vertical::middle},            // Pouch/Gestures titles

    // Pause-menu screens share one layout: a left pane (list, tabs, title,
    // title base = unnamed root sprite at (960,0) holding MENU_TitleBase (-69..755);
    // key guide) over the BG_S gradient, a detail area on the right and the
    // full-screen MENU_BaseMain_01 backdrop (BG_L or an unnamed depth 1,
    // -82..2012 on the stage), which is stretched to the screen width. The
    // separate detail movies (02_05x) are positioned by the game from the
    // Status_* placeholders of DetailStatusView, so the placeholders move.
    {"02_010_equiptop.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_010_equiptop.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},                     // 380
    {"02_010_equiptop.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},             // 120,1020
    {"02_010_equiptop.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},                // 268,87
    {"02_010_equiptop.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 8},
    {"02_010_equiptop.gfx", nullptr, "ItemList", Horizontal::left, Vertical::middle},              // 380
    {"02_010_equiptop.gfx", nullptr, "ItemName", Horizontal::left, Vertical::top},
    {"02_010_equiptop.gfx", nullptr, "MenuSubTitle", Horizontal::left, Vertical::top},
    {"02_010_equiptop.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},   // 1540
    {"02_010_equiptop.gfx", "DetailStatusView", "Status_Unchiku", Horizontal::right, Vertical::middle},  // 1540
    {"02_010_equiptop.gfx", nullptr, "AdditionalProperties", Horizontal::right, Vertical::top},     // weight, 1559

    {"02_011_equip.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_011_equip.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"02_011_equip.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"02_011_equip.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"02_011_equip.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"02_011_equip.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_011_equip.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 20},
    {"02_011_equip.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},     // 1540
    {"02_011_equip.gfx", nullptr, "AdditionalProperties", Horizontal::right, Vertical::top},

    {"02_020_inventory.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_020_inventory.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"02_020_inventory.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"02_020_inventory.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::top},
    {"02_020_inventory.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"02_020_inventory.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"02_020_inventory.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_020_inventory.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 95},
    {"02_020_inventory.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},
    // Here AdditionalProperties only holds TopItem_0 above the list (x 619).
    {"02_020_inventory.gfx", nullptr, "AdditionalProperties", Horizontal::left, Vertical::top},

    // 02_030_inventory_commandlist (the item pop-up) is not moved: the game
    // places it beside the selected item, which already moves with the list
    // (its ItemList at 245 reaches -48 on the stage, off screen even at
    // 16:9). Moving it as well put it twice as far left, off the screen edge
    // (r135 report at 21:9).

    {"02_070_status.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_070_status.gfx", "BG", "BG_S", Horizontal::stretch, Vertical::middle},   // centred here
    {"02_070_status.gfx", "BG", "Icon_0", Horizontal::left, Vertical::middle},    // 480
    {"02_070_status.gfx", nullptr, "LeftPane", Horizontal::left, Vertical::middle},
    {"02_070_status.gfx", nullptr, "DetailStatus", Horizontal::right, Vertical::middle},  // 1250
    {"02_070_status.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"02_070_status.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},

    // Options (also patched by settings_movie first): the dark panel
    // (MENU_TopBase_01, 336..1600) and its help line and key guide (StatusBar
    // 380, aligned with the panel) stay centred; the backdrop is stretched
    // and the title with its MENU_TitleBase (-69..755) goes left.
    {"02_040_optionsetting.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_040_optionsetting.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_040_optionsetting.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 251},
    {"02_042_pc_graphicsetting.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_042_pc_graphicsetting.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_042_pc_graphicsetting.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 6},

    // ---- Batch 2 (boxes from build-scripts/re-tools/gfxlayout, vanilla 2.7.1) ----
    // Shops: same layout as the equipment screens (list 80..700, detail 670..1830).
    {"03_001_shop.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"03_001_shop.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"03_001_shop.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"03_001_shop.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::top},
    {"03_001_shop.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"03_001_shop.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"03_001_shop.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"03_001_shop.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 84},
    {"03_001_shop.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},
    {"03_002_spexchangeshop.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"03_002_spexchangeshop.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"03_002_spexchangeshop.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"03_002_spexchangeshop.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::top},
    {"03_002_spexchangeshop.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"03_002_spexchangeshop.gfx", nullptr, "ResourceItem", Horizontal::left, Vertical::top},
    {"03_002_spexchangeshop.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"03_002_spexchangeshop.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"03_002_spexchangeshop.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 94},
    {"03_002_spexchangeshop.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},
    // Level up: attributes 73..693 and controls on the left, player stats 683..1840.
    {"03_010_levelup.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"03_010_levelup.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"03_010_levelup.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"03_010_levelup.gfx", nullptr, "Status_0", Horizontal::left, Vertical::middle},
    {"03_010_levelup.gfx", nullptr, "Control", Horizontal::left, Vertical::middle},
    {"03_010_levelup.gfx", nullptr, "Status_Player", Horizontal::right, Vertical::middle},
    {"03_010_levelup.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"03_010_levelup.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 97},
    // Memorize spell and item box: wide left area (to x 1280) + detail 1102..1682.
    {"03_020_spell.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"03_020_spell.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"03_020_spell.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"03_020_spell.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 6},
    {"03_020_spell.gfx", nullptr, "ItemList", Horizontal::left, Vertical::middle},
    {"03_020_spell.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::middle},
    {"03_020_spell.gfx", nullptr, "TabList", Horizontal::left, Vertical::middle},
    {"03_020_spell.gfx", nullptr, "ItemCategory", Horizontal::left, Vertical::top},
    {"03_020_spell.gfx", nullptr, "Page_Left", Horizontal::left, Vertical::top},
    {"03_020_spell.gfx", "DetailStatusView", "Status_Item", Horizontal::right, Vertical::middle},
    {"03_020_spell.gfx", "DetailStatusView", "Status_Unchiku", Horizontal::right, Vertical::middle},
    {"03_050_itembox.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"03_050_itembox.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"03_050_itembox.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"03_050_itembox.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::top},
    {"03_050_itembox.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"03_050_itembox.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"03_050_itembox.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 150},
    {"03_050_itembox.gfx", "DetailStatusView", "Status_Item", Horizontal::right, Vertical::middle},
    {"03_050_itembox.gfx", "DetailStatusView", "Status_Unchiku", Horizontal::right, Vertical::middle},
    // Ashes of War (soulforging): equipment layout.
    {"02_012_soulforging.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_012_soulforging.gfx", "BG", "BG_S", Horizontal::left, Vertical::middle},
    {"02_012_soulforging.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"02_012_soulforging.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"02_012_soulforging.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_012_soulforging.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 66},
    {"02_012_soulforging.gfx", nullptr, "AdditionalProperties", Horizontal::left, Vertical::middle},  // 483..693
    {"02_012_soulforging.gfx", "DetailStatusView", "Status_Player", Horizontal::right, Vertical::middle},
    // Site of grace: centred window; only the full-width top and bottom bars widen.
    {"02_080_warp.gfx", nullptr, "bottom bar", Horizontal::stretch, Vertical::bottom, 253},  // -5..1927
    {"02_080_warp.gfx", nullptr, "top bar", Horizontal::stretch, Vertical::top, 258},        // 0..1920
    // Fast travel list over the map: left panel.
    {"02_122_worldmap_warplist.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"02_122_worldmap_warplist.gfx", nullptr, "BackTabList", Horizontal::left, Vertical::top},
    {"02_122_worldmap_warplist.gfx", nullptr, "TabList", Horizontal::left, Vertical::top},
    {"02_122_worldmap_warplist.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    // Smithing weapon select: left list and its dialog.
    {"03_041_enhancedweaponselect.gfx", nullptr, "WindowList", Horizontal::left, Vertical::middle},
    {"03_041_enhancedweaponselect.gfx", nullptr, "SelectDialog", Horizontal::left, Vertical::middle},
    // Multiplayer and network: centred panels; backdrop, title and guide.
    {"02_151_signranknetwork.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_151_signranknetwork.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},  // 120
    {"02_151_signranknetwork.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_151_signranknetwork.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 9},
    {"02_150_network.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_150_network.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_150_network.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 9},
    // Controller key configuration: list left (85..1346), help and key picture right.
    {"02_160_keyconfiguration.gfx", "BG", "BG_L", Horizontal::stretch, Vertical::middle},
    {"02_160_keyconfiguration.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},
    {"02_160_keyconfiguration.gfx", nullptr, "KeySetting", Horizontal::left, Vertical::middle},
    {"02_160_keyconfiguration.gfx", nullptr, "Help", Horizontal::right, Vertical::middle},       // 1000..1828
    {"02_160_keyconfiguration.gfx", nullptr, "SelectKey", Horizontal::right, Vertical::middle},  // 1177..1651
    {"02_160_keyconfiguration.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_160_keyconfiguration.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 342},
    // Brightness: centred panel; backdrop and title base live inside the states.
    {"02_046_brightnesssetting.gfx", "SDRSetting", "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_046_brightnesssetting.gfx", "SDRSetting", "title base", Horizontal::left, Vertical::top, 21},
    {"02_046_brightnesssetting.gfx", "HDRSetting", "title base", Horizontal::left, Vertical::top, 16},
    {"02_046_brightnesssetting.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_200_coliseum.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"02_200_coliseum.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_200_coliseum.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 55},
    // Tutorial pop-up: the full-screen dimmer widens; the panel stays centred.
    {"02_130_tutorial_modal.gfx", "State_1", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"02_130_tutorial_modal.gfx", "State_0", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"02_131_tutorial_toast.gfx", nullptr, "State_1", Horizontal::left, Vertical::middle},
    {"02_131_tutorial_toast.gfx", nullptr, "State_0", Horizontal::left, Vertical::middle},
    // Loading screens: picture covers the screen (uniform, crops top/bottom),
    // tip text and key guide left, progress gauge right.
    {"02_903_nowloading2.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"02_903_nowloading2.gfx", nullptr, "BackImage", Horizontal::cover, Vertical::middle},
    {"02_903_nowloading2.gfx", nullptr, "Main", Horizontal::left, Vertical::bottom},
    {"02_903_nowloading2.gfx", nullptr, "Gauge", Horizontal::right, Vertical::bottom},
    {"02_904_nowloading3.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"02_904_nowloading3.gfx", nullptr, "loading logo", Horizontal::left, Vertical::bottom, 3},
    // Title screen (the background is the game's 3D scene): info and
    // download progress to the right; the logo screen's black backdrops widen.
    {"05_000_title.gfx", "Top", "Info", Horizontal::right, Vertical::bottom},                 // 1355..1824
    {"05_000_title.gfx", nullptr, "Install_ProgressInfo", Horizontal::right, Vertical::top},
    {"05_000_title.gfx", nullptr, "ProgressInfo", Horizontal::right, Vertical::top},
    {"05_001_title_logo.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_001_title_logo.gfx", "#3", "logo backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_010_profileselect.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_010_profileselect.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"05_010_profileselect.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 11},
    {"05_031_newgame_brightnesssetting.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_031_newgame_brightnesssetting.gfx", "BrightnessSetting", "panel backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_031_newgame_brightnesssetting.gfx", "BrightnessSetting", "title text", Horizontal::left, Vertical::top, 22},
    {"05_031_newgame_brightnesssetting.gfx", "BrightnessSetting", "title base", Horizontal::left, Vertical::top, 24},
    {"05_031_newgame_brightnesssetting.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"05_032_newgame_controlsetting.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_032_newgame_controlsetting.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"05_032_newgame_controlsetting.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 23},

    // ---- Dialogs, fades and logo screens: only their full-screen dimmer or
    // backdrop (0..1920 shapes or MENU_BaseMain_01) widens; boxes, buttons
    // and text stay centred. ----
    {"01_010_messagebox.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"01_012_itemquantityselect.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"01_014_itemquantityselect_recipe.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"01_031_bloodmessage_top.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"01_031_bloodmessage_top.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 70},
    {"01_031_bloodmessage_top.gfx", nullptr, "title", Horizontal::left, Vertical::top, 72},
    {"01_070_commandlist.gfx", "ItemList", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"01_071_commandlist2.gfx", "ItemList", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"01_900_black.gfx", "Fade", "black", Horizontal::stretch, Vertical::middle, 1},
    {"01_910_fade.gfx", "Fade", "black", Horizontal::stretch, Vertical::middle, 1},
    {"01_910_fade.gfx", "Fade", "black 2", Horizontal::stretch, Vertical::middle, 2},
    {"02_110_detailkeyguide.gfx", "ItemList", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"02_152_networkkeywordsetting.gfx", "WindowList", "dimmer", Horizontal::stretch, Vertical::middle, 1},
    {"05_020_titleinformation.gfx", nullptr, "dimmer", Horizontal::stretch, Vertical::middle, 1},
    // Data analysis policy: the window (155..1763) and its key guide
    // (StatusBar 200, aligned with the window) stay centred.
    {"05_040_termofservice.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},
    {"05_050_staffroll.gfx", "Main", "black", Horizontal::stretch, Vertical::middle, 1},
    {"05_900_logo_fromsoft.gfx", nullptr, "black", Horizontal::stretch, Vertical::middle, 1},
    {"05_901_logo_bne.gfx", nullptr, "black", Horizontal::stretch, Vertical::middle, 1},
    {"05_903_warn_illegalcopy.gfx", nullptr, "black", Horizontal::stretch, Vertical::middle, 1},
    {"05_905_logo_copyright.gfx", nullptr, "black", Horizontal::stretch, Vertical::middle, 1},

    // ---- World map (batch 3). Body (267,152) holds the map; Body.Mask clips
    // it to 0..1920 and is widened. The map layers, markers, cursor and place
    // name inside are placed by the game (scroll/zoom) and stay untouched. ----
    {"02_120_worldmap.gfx", nullptr, "backdrop", Horizontal::stretch, Vertical::middle, 1},     // 0..1920
    {"02_120_worldmap.gfx", "Body", "Mask", Horizontal::stretch, Vertical::middle},
    // Opening fade over the map area (-117..2031), same in vanilla and Convergence.
    {"02_120_worldmap.gfx", "Body._.Screen", "map fade", Horizontal::stretch, Vertical::middle, 1},
    {"02_120_worldmap.gfx", "Body", "bottom bar", Horizontal::stretch, Vertical::bottom, 84},    // -78..1985
    {"02_120_worldmap.gfx", "Body", "bottom bar 2", Horizontal::stretch, Vertical::bottom, 86},
    {"02_120_worldmap.gfx", "Body", "Time", Horizontal::right, Vertical::bottom},                // 1493..2094
    {"02_120_worldmap.gfx", "Body", "TimeName", Horizontal::right, Vertical::bottom},            // 1175..1900
    {"02_120_worldmap.gfx", "Body", "compass", Horizontal::right, Vertical::top, 100},           // 1735..1877
    {"02_120_worldmap.gfx", "Body", "edge shade left", Horizontal::left, Vertical::middle, 102}, // -29..51
    {"02_120_worldmap.gfx", "Body", "edge shade right", Horizontal::right, Vertical::middle, 104}, // 1878..1959
    {"02_120_worldmap.gfx", nullptr, "StatusBar", Horizontal::left, Vertical::bottom},           // 75
    {"02_120_worldmap.gfx", nullptr, "MenuTitle", Horizontal::left, Vertical::top},
    {"02_120_worldmap.gfx", nullptr, "title base", Horizontal::left, Vertical::top, 113},

    // NPC subtitles: Caption_0.#1 only holds the dark gradient band (-48..1968).
    {"01_060_caption.gfx", "Caption_0", "subtitle band", Horizontal::stretch, Vertical::bottom, 1},
};

constexpr LayoutMovie kMovies[] = {
    {"01_000_fe.gfx", LayoutGroup::hud},
    {"01_001_fe_soul.gfx", LayoutGroup::hud},
    {"01_002_fe_saveicon.gfx", LayoutGroup::hud},
    {"02_000_ingametop.gfx", LayoutGroup::menu},
    {"02_010_equiptop.gfx", LayoutGroup::menu},
    {"02_011_equip.gfx", LayoutGroup::menu},
    {"02_020_inventory.gfx", LayoutGroup::menu},
    {"02_040_optionsetting.gfx", LayoutGroup::menu},
    {"02_042_pc_graphicsetting.gfx", LayoutGroup::menu},
    {"02_070_status.gfx", LayoutGroup::menu},
    {"02_012_soulforging.gfx", LayoutGroup::menu},
    {"02_046_brightnesssetting.gfx", LayoutGroup::menu},
    {"02_080_warp.gfx", LayoutGroup::menu},
    {"02_122_worldmap_warplist.gfx", LayoutGroup::menu},
    {"02_130_tutorial_modal.gfx", LayoutGroup::menu},
    {"02_131_tutorial_toast.gfx", LayoutGroup::menu},
    {"02_150_network.gfx", LayoutGroup::menu},
    {"02_151_signranknetwork.gfx", LayoutGroup::menu},
    {"02_160_keyconfiguration.gfx", LayoutGroup::menu},
    {"02_200_coliseum.gfx", LayoutGroup::menu},
    {"02_903_nowloading2.gfx", LayoutGroup::menu},
    {"02_904_nowloading3.gfx", LayoutGroup::menu},
    {"03_001_shop.gfx", LayoutGroup::menu},
    {"03_002_spexchangeshop.gfx", LayoutGroup::menu},
    {"03_010_levelup.gfx", LayoutGroup::menu},
    {"03_020_spell.gfx", LayoutGroup::menu},
    {"03_041_enhancedweaponselect.gfx", LayoutGroup::menu},
    {"03_050_itembox.gfx", LayoutGroup::menu},
    {"05_000_title.gfx", LayoutGroup::menu},
    {"05_001_title_logo.gfx", LayoutGroup::menu},
    {"05_010_profileselect.gfx", LayoutGroup::menu},
    {"05_031_newgame_brightnesssetting.gfx", LayoutGroup::menu},
    {"05_032_newgame_controlsetting.gfx", LayoutGroup::menu},
    {"01_010_messagebox.gfx", LayoutGroup::menu},
    {"01_012_itemquantityselect.gfx", LayoutGroup::menu},
    {"01_014_itemquantityselect_recipe.gfx", LayoutGroup::menu},
    {"01_031_bloodmessage_top.gfx", LayoutGroup::menu},
    {"01_070_commandlist.gfx", LayoutGroup::menu},
    {"01_071_commandlist2.gfx", LayoutGroup::menu},
    {"01_900_black.gfx", LayoutGroup::menu},
    {"01_910_fade.gfx", LayoutGroup::menu},
    {"02_110_detailkeyguide.gfx", LayoutGroup::menu},
    {"02_152_networkkeywordsetting.gfx", LayoutGroup::menu},
    {"05_020_titleinformation.gfx", LayoutGroup::menu},
    {"05_040_termofservice.gfx", LayoutGroup::menu},
    {"05_050_staffroll.gfx", LayoutGroup::menu},
    {"05_900_logo_fromsoft.gfx", LayoutGroup::menu},
    {"05_901_logo_bne.gfx", LayoutGroup::menu},
    {"05_903_warn_illegalcopy.gfx", LayoutGroup::menu},
    {"05_905_logo_copyright.gfx", LayoutGroup::menu},
    {"02_120_worldmap.gfx", LayoutGroup::menu},
    {"01_060_caption.gfx", LayoutGroup::menu},
};

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::uint16_t read_u16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    std::uint32_t unsigned_bits(unsigned count) noexcept {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index, ++bit_) {
            if (bit_ / 8 >= size_) {
                failed_ = true;
                return 0;
            }
            value = (value << 1) | ((data_[bit_ / 8] >> (7 - bit_ % 8)) & 1u);
        }
        return value;
    }

    std::int32_t signed_bits(unsigned count) noexcept {
        if (count == 0) return 0;
        std::uint32_t value = unsigned_bits(count);
        if (count < 32 && (value & (1u << (count - 1)))) value |= ~0u << count;
        return static_cast<std::int32_t>(value);
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept { return (bit_ + 7) / 8; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bit_{};
    bool failed_{};
};

class BitWriter {
public:
    void bits(std::uint32_t value, unsigned count) {
        for (unsigned index = count; index-- > 0;) {
            if (bit_ % 8 == 0) bytes_.push_back(0);
            if ((value >> index) & 1u) bytes_.back() |= static_cast<std::uint8_t>(0x80u >> (bit_ % 8));
            ++bit_;
        }
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_{};
    std::size_t bit_{};
};

unsigned signed_bit_count(std::int32_t value) noexcept {
    unsigned count = 1;
    while (count < 32) {
        const std::int64_t low = -(std::int64_t{1} << (count - 1));
        const std::int64_t high = (std::int64_t{1} << (count - 1)) - 1;
        if (value >= low && value <= high) break;
        ++count;
    }
    return count;
}

// SWF MATRIX: scale/rotate in 16.16 fixed point, translation in twips.
struct Matrix {
    std::int32_t scale_x{kFixedOne};
    std::int32_t scale_y{kFixedOne};
    std::int32_t skew0{};
    std::int32_t skew1{};
    std::int32_t translate_x{};
    std::int32_t translate_y{};
};

bool read_matrix(const std::uint8_t* data, std::size_t size, Matrix& matrix, std::size_t& used) noexcept {
    BitReader reader(data, size);
    if (reader.unsigned_bits(1)) {
        const unsigned bits = reader.unsigned_bits(5);
        matrix.scale_x = reader.signed_bits(bits);
        matrix.scale_y = reader.signed_bits(bits);
    }
    if (reader.unsigned_bits(1)) {
        const unsigned bits = reader.unsigned_bits(5);
        matrix.skew0 = reader.signed_bits(bits);
        matrix.skew1 = reader.signed_bits(bits);
    }
    const unsigned bits = reader.unsigned_bits(5);
    matrix.translate_x = reader.signed_bits(bits);
    matrix.translate_y = reader.signed_bits(bits);
    used = reader.bytes_used();
    return !reader.failed();
}

std::vector<std::uint8_t> write_matrix(const Matrix& matrix) {
    BitWriter writer;
    const bool has_scale = matrix.scale_x != kFixedOne || matrix.scale_y != kFixedOne;
    writer.bits(has_scale ? 1 : 0, 1);
    if (has_scale) {
        const unsigned bits = std::max(signed_bit_count(matrix.scale_x), signed_bit_count(matrix.scale_y));
        writer.bits(bits, 5);
        writer.bits(static_cast<std::uint32_t>(matrix.scale_x), bits);
        writer.bits(static_cast<std::uint32_t>(matrix.scale_y), bits);
    }
    const bool has_rotate = matrix.skew0 != 0 || matrix.skew1 != 0;
    writer.bits(has_rotate ? 1 : 0, 1);
    if (has_rotate) {
        const unsigned bits = std::max(signed_bit_count(matrix.skew0), signed_bit_count(matrix.skew1));
        writer.bits(bits, 5);
        writer.bits(static_cast<std::uint32_t>(matrix.skew0), bits);
        writer.bits(static_cast<std::uint32_t>(matrix.skew1), bits);
    }
    const unsigned bits = (matrix.translate_x == 0 && matrix.translate_y == 0)
        ? 0u
        : std::max(signed_bit_count(matrix.translate_x), signed_bit_count(matrix.translate_y));
    writer.bits(bits, 5);
    writer.bits(static_cast<std::uint32_t>(matrix.translate_x), bits);
    writer.bits(static_cast<std::uint32_t>(matrix.translate_y), bits);
    return writer.bytes();
}

struct Tag {
    std::uint16_t code{};
    std::size_t start{};   // header start
    std::size_t header{};  // 2 or 6
    std::size_t length{};  // body length
};

bool read_tag(std::span<const std::uint8_t> data, std::size_t position, std::size_t end, Tag& tag) noexcept {
    if (position + 2 > end) return false;
    const std::uint16_t code_and_length = read_u16(data.data() + position);
    tag.code = static_cast<std::uint16_t>(code_and_length >> 6);
    tag.start = position;
    tag.header = 2;
    tag.length = code_and_length & 0x3Fu;
    if (tag.length == 0x3F) {
        if (position + 6 > end) return false;
        tag.length = read_u32(data.data() + position + 2);
        tag.header = 6;
    }
    return tag.start + tag.header + tag.length <= end;
}

void write_tag_header(std::vector<std::uint8_t>& out, std::uint16_t code, std::size_t length, bool long_form) {
    if (!long_form && length < 0x3F) {
        write_u16(out, static_cast<std::uint16_t>((code << 6) | length));
    } else {
        write_u16(out, static_cast<std::uint16_t>((code << 6) | 0x3F));
        write_u32(out, static_cast<std::uint32_t>(length));
    }
}

struct Placement {
    std::uint16_t depth{};
    bool has_character{};
    std::uint16_t character{};
    bool has_matrix{};
    Matrix matrix{};
    std::size_t matrix_offset{};  // within the tag body
    std::size_t matrix_size{};
    std::string name{};
};

bool skip_cstring(const std::uint8_t* body, std::size_t length, std::size_t& offset) noexcept {
    while (offset < length && body[offset] != 0) ++offset;
    if (offset >= length) return false;
    ++offset;
    return true;
}

bool parse_placement(std::uint16_t code, const std::uint8_t* body, std::size_t length, Placement& placement) noexcept {
    std::size_t offset = 0;
    if (length < (code == kTagPlaceObject3 ? 4u : 3u)) return false;
    const std::uint8_t flags = body[offset++];
    std::uint8_t flags2 = 0;
    if (code == kTagPlaceObject3) flags2 = body[offset++];
    placement.depth = read_u16(body + offset);
    offset += 2;
    placement.has_character = (flags & 0x02) != 0;
    // Only HasClassName adds the class string. The SWF spec also lists
    // HasImage+HasCharacter, but these movies set HasImage with a plain
    // character id (and no string) on PlaceObject3.
    if (code == kTagPlaceObject3 && (flags2 & 0x08)) {
        if (!skip_cstring(body, length, offset)) return false;
    }
    if (placement.has_character) {
        if (offset + 2 > length) return false;
        placement.character = read_u16(body + offset);
        offset += 2;
    }
    placement.has_matrix = (flags & 0x04) != 0;
    if (placement.has_matrix) {
        placement.matrix_offset = offset;
        if (!read_matrix(body + offset, length - offset, placement.matrix, placement.matrix_size)) return false;
        offset += placement.matrix_size;
    }
    if (flags & 0x08) {  // CXFORMWITHALPHA
        BitReader reader(body + offset, length - offset);
        const bool has_add = reader.unsigned_bits(1) != 0;
        const bool has_multiply = reader.unsigned_bits(1) != 0;
        const unsigned bits = reader.unsigned_bits(4);
        if (has_multiply) reader.unsigned_bits(bits * 4);
        if (has_add) reader.unsigned_bits(bits * 4);
        if (reader.failed()) return false;
        offset += reader.bytes_used();
    }
    if (flags & 0x10) offset += 2;  // ratio
    if (flags & 0x20) {
        const std::size_t begin = offset;
        if (!skip_cstring(body, length, offset)) return false;
        placement.name.assign(reinterpret_cast<const char*>(body + begin), offset - begin - 1);
    }
    return true;
}

struct NamedPlacement {
    std::uint16_t sprite{};  // 0 = root timeline
    Placement placement{};
};

bool collect_named(
    std::span<const std::uint8_t> data,
    std::size_t position,
    std::size_t end,
    std::uint16_t sprite,
    int depth_guard,
    std::vector<NamedPlacement>& named) {
    if (depth_guard > 16) return false;
    while (position < end) {
        Tag tag{};
        if (!read_tag(data, position, end, tag)) return false;
        const std::uint8_t* body = data.data() + tag.start + tag.header;
        if (tag.code == kTagDefineSprite) {
            if (tag.length < 4) return false;
            const std::size_t body_start = tag.start + tag.header;
            if (!collect_named(data, body_start + 4, body_start + tag.length, read_u16(body), depth_guard + 1, named)) {
                return false;
            }
        } else if (tag.code == kTagPlaceObject2 || tag.code == kTagPlaceObject3) {
            Placement placement{};
            if (!parse_placement(tag.code, body, tag.length, placement)) return false;
            // Unnamed ones too: some anchored shapes are found by depth.
            if (!placement.name.empty() || placement.has_character) {
                named.push_back({sprite, std::move(placement)});
            }
        }
        position = tag.start + tag.header + tag.length;
        if (tag.code == kTagEnd) break;
    }
    return true;
}

struct Header {
    std::size_t body_start{};
    std::size_t declared{};  // movie bytes; the game's buffers add zero padding
};

bool read_header(std::span<const std::uint8_t> data, Header& header, std::string& message) {
    if (data.size() < 21) {
        message = "file too small";
        return false;
    }
    if (std::memcmp(data.data(), "CFX", 3) == 0 || std::memcmp(data.data(), "CWS", 3) == 0) {
        message = "compressed GFX is not supported";
        return false;
    }
    if (std::memcmp(data.data(), "GFX", 3) != 0 && std::memcmp(data.data(), "FWS", 3) != 0) {
        message = "not a GFX/SWF file";
        return false;
    }
    // Movies read from the game archives keep the archive's zero padding after
    // the declared length (01_001_fe_soul: 3540 declared, 3552 in memory).
    header.declared = read_u32(data.data() + 4);
    if (header.declared < 21 || header.declared > data.size() ||
        std::any_of(data.begin() + header.declared, data.end(), [](std::uint8_t byte) { return byte != 0; })) {
        message = "declared GFX length is invalid";
        return false;
    }
    data = data.first(header.declared);
    BitReader reader(data.data() + 8, data.size() - 8);
    const unsigned bits = reader.unsigned_bits(5);
    for (int index = 0; index < 4; ++index) reader.signed_bits(bits);
    if (reader.failed()) {
        message = "invalid stage rectangle";
        return false;
    }
    header.body_start = 8 + reader.bytes_used() + 4;  // frame rate + frame count
    return header.body_start < data.size();
}

struct Target {
    const AnchorRule* rule{};
    std::uint16_t sprite{};
    std::uint16_t depth{};
    std::uint16_t character{};
    bool character_known{};
    float parent_x{};
    float parent_y{};
    Horizontal horizontal{};  // rule.horizontal with side resolved
    float parent_scale_x{1.0f};  // product of the path levels' scales
    float parent_scale_y{1.0f};
    Matrix original{};
};

struct Resolution {
    std::vector<Target> targets{};
    std::vector<std::string> missing{};
};

const NamedPlacement* find_named(
    const std::vector<NamedPlacement>& named, std::uint16_t sprite, std::string_view name) noexcept {
    for (const NamedPlacement& entry : named) {
        if (entry.sprite == sprite && !name.empty() && entry.placement.name == name) return &entry;
    }
    return nullptr;
}

// First unnamed character placement on that depth (its initial frame).
const NamedPlacement* find_unnamed(
    const std::vector<NamedPlacement>& named, std::uint16_t sprite, std::uint16_t depth) noexcept {
    for (const NamedPlacement& entry : named) {
        if (entry.sprite == sprite && entry.placement.depth == depth) {
            return entry.placement.name.empty() && entry.placement.has_character ? &entry : nullptr;
        }
    }
    return nullptr;
}

// Path levels may carry a scale (no skew): a mod can scale a parent such as
// PlayerHUD.Gauge, and a patched movie carries the HUD scale on its parent
// targets. The patch and its verification must find the same elements, so
// both accept them and transform() converts through the parent scale.
Resolution resolve_targets(HudMovie movie, const std::vector<NamedPlacement>& named) {
    Resolution resolution{};
    for (const AnchorRule& rule : kRules) {
        if (std::strcmp(rule.movie, movie->file) != 0) continue;
        std::uint16_t sprite = 0;
        float parent_x = 0.0f;
        float parent_y = 0.0f;
        float parent_scale_x = 1.0f;
        float parent_scale_y = 1.0f;
        if (rule.parent) {
            // A path from the root: "A", "#3" or "A.B.#1"; each segment is a
            // named placement or "#N", an unnamed one by its depth.
            bool found = true;
            const std::string_view path = rule.parent;
            std::size_t begin = 0;
            while (found && begin <= path.size()) {
                const std::size_t dot = path.find('.', begin);
                const std::string_view segment =
                    path.substr(begin, dot == std::string_view::npos ? std::string_view::npos : dot - begin);
                const NamedPlacement* parent = !segment.empty() && segment[0] == '#'
                    ? find_unnamed(named, sprite,
                        static_cast<std::uint16_t>(std::atoi(std::string(segment.substr(1)).c_str())))
                    : find_named(named, sprite, segment);
                // Every level must be a translation and a non-zero scale (no
                // skew) for the stage maths below.
                if (!parent || !parent->placement.has_character || !parent->placement.has_matrix ||
                    parent->placement.matrix.scale_x == 0 || parent->placement.matrix.scale_y == 0 ||
                    parent->placement.matrix.skew0 != 0 || parent->placement.matrix.skew1 != 0) {
                    found = false;
                    break;
                }
                sprite = parent->placement.character;
                parent_x += parent_scale_x * parent->placement.matrix.translate_x / 20.0f;
                parent_y += parent_scale_y * parent->placement.matrix.translate_y / 20.0f;
                parent_scale_x *= parent->placement.matrix.scale_x / static_cast<float>(kFixedOne);
                parent_scale_y *= parent->placement.matrix.scale_y / static_cast<float>(kFixedOne);
                if (dot == std::string_view::npos) break;
                begin = dot + 1;
            }
            if (!found) {
                resolution.missing.push_back(std::string(rule.parent) + "." + rule.name);
                continue;
            }
        }
        const NamedPlacement* element = rule.depth >= 0
            ? find_unnamed(named, sprite, static_cast<std::uint16_t>(rule.depth))
            : find_named(named, sprite, rule.name);
        if (!element || !element->placement.has_matrix) {
            resolution.missing.push_back(rule.parent ? std::string(rule.parent) + "." + rule.name : rule.name);
            continue;
        }
        Target target{};
        target.rule = &rule;
        target.sprite = sprite;
        target.depth = element->placement.depth;
        target.character = element->placement.character;
        target.character_known = element->placement.has_character;
        target.parent_x = parent_x;
        target.parent_y = parent_y;
        target.parent_scale_x = parent_scale_x;
        target.horizontal = rule.horizontal;
        if (rule.horizontal == Horizontal::side) {
            const float stage_x = parent_x + parent_scale_x * element->placement.matrix.translate_x / 20.0f;
            target.horizontal = stage_x < kStageWidth / 2.0f ? Horizontal::left : Horizontal::right;
        }
        target.parent_scale_y = parent_scale_y;
        target.original = element->placement.matrix;
        resolution.targets.push_back(target);
    }
    return resolution;
}

float anchor_x(Horizontal horizontal) noexcept {
    return horizontal == Horizontal::left ? 0.0f
        : horizontal == Horizontal::right ? kStageWidth : kStageWidth / 2.0f;
}

float anchor_y(Vertical vertical) noexcept {
    return vertical == Vertical::top ? 0.0f
        : vertical == Vertical::bottom ? kStageHeight : kStageHeight / 2.0f;
}

std::int32_t round_to_int(double value) noexcept {
    return static_cast<std::int32_t>(std::lround(value));
}

Matrix transform(const Matrix& matrix, const Target& target, const HudLayout& layout) noexcept {
    const AnchorRule& rule = *target.rule;
    // Stage position of the element and back: the parents translate and scale.
    const double parent_sx = target.parent_scale_x;
    const double parent_sy = target.parent_scale_y;
    const double stage_x = target.parent_x + parent_sx * matrix.translate_x / 20.0;
    const double stage_y = target.parent_y + parent_sy * matrix.translate_y / 20.0;
    const auto local_x = [&](double x) { return round_to_int((x - target.parent_x) / parent_sx * 20.0); };
    const auto local_y = [&](double y) { return round_to_int((y - target.parent_y) / parent_sy * 20.0); };
    if (rule.horizontal == Horizontal::bar_fit) {
        // The parent (Gauge) is anchored left/top: its content moves by -dx and
        // scales by s around x 0, so the bar origin lands at -dx + s * x.
        const double s = layout.scale;
        const double right = stage_x + kBarArtRight * parent_sx * (matrix.scale_x / 65536.0);
        if (right < kBarFitThreshold) return matrix;
        const double margin = kStageWidth - right;
        const double origin = -layout.edge_offset + s * stage_x;
        const double target_right = kStageWidth + layout.edge_offset - margin * s;
        Matrix result = matrix;
        result.scale_x = round_to_int((target_right - origin) / (kBarArtRight * s * parent_sx) * 65536.0);
        return result;
    }
    if (rule.horizontal == Horizontal::cover) {
        const double factor = (kStageWidth / 2.0 + layout.edge_offset) / (kStageWidth / 2.0);
        Matrix result = matrix;
        result.translate_x = local_x(kStageWidth / 2.0 + (stage_x - kStageWidth / 2.0) * factor);
        result.translate_y = local_y(kStageHeight / 2.0 + (stage_y - kStageHeight / 2.0) * factor);
        result.scale_x = round_to_int(matrix.scale_x * factor);
        result.scale_y = round_to_int(matrix.scale_y * factor);
        result.skew0 = round_to_int(matrix.skew0 * factor);
        result.skew1 = round_to_int(matrix.skew1 * factor);
        return result;
    }
    if (rule.horizontal == Horizontal::stretch) {
        // Widen around the stage centre so a full-width backdrop reaches the
        // screen edges: the visible stage is 1920 + 2 * edge_offset wide.
        const double factor = (kStageWidth / 2.0 + layout.edge_offset) / (kStageWidth / 2.0);
        const double new_x = kStageWidth / 2.0 + (stage_x - kStageWidth / 2.0) * factor;
        Matrix result = matrix;
        result.translate_x = local_x(new_x);
        result.scale_x = round_to_int(matrix.scale_x * factor);
        result.skew1 = round_to_int(matrix.skew1 * factor);  // y contribution to x
        return result;
    }
    const float ax = anchor_x(target.horizontal);
    const float ay = anchor_y(rule.vertical);
    const float shift = target.horizontal == Horizontal::left ? -layout.edge_offset
        : target.horizontal == Horizontal::right ? layout.edge_offset : 0.0f;
    const double new_x = ax + shift + (stage_x - ax) * layout.scale;
    const double new_y = ay + (stage_y - ay) * layout.scale;
    Matrix result = matrix;
    result.translate_x = local_x(new_x);
    result.translate_y = local_y(new_y);
    result.scale_x = round_to_int(matrix.scale_x * static_cast<double>(layout.scale));
    result.scale_y = round_to_int(matrix.scale_y * static_cast<double>(layout.scale));
    result.skew0 = round_to_int(matrix.skew0 * static_cast<double>(layout.scale));
    result.skew1 = round_to_int(matrix.skew1 * static_cast<double>(layout.scale));
    return result;
}

const Target* target_for(
    const std::vector<Target>& targets, std::uint16_t sprite, const Placement& placement) noexcept {
    for (const Target& target : targets) {
        if (target.sprite != sprite || target.depth != placement.depth) continue;
        // A later frame may place a different character on the same depth.
        if (placement.has_character && target.character_known && placement.character != target.character) continue;
        return &target;
    }
    return nullptr;
}

struct RewriteState {
    const std::vector<Target>* targets{};
    const HudLayout* layout{};
    std::vector<std::uint32_t> rewritten{};  // per target
};

bool rewrite(
    std::span<const std::uint8_t> data,
    std::size_t position,
    std::size_t end,
    std::uint16_t sprite,
    int depth_guard,
    RewriteState& state,
    std::vector<std::uint8_t>& out) {
    if (depth_guard > 16) return false;
    while (position < end) {
        Tag tag{};
        if (!read_tag(data, position, end, tag)) return false;
        const std::size_t body_start = tag.start + tag.header;
        const std::uint8_t* body = data.data() + body_start;
        const std::size_t next = body_start + tag.length;
        const bool long_form = tag.header == 6;
        bool copied = false;
        if (tag.code == kTagDefineSprite) {
            if (tag.length < 4) return false;
            std::vector<std::uint8_t> children{};
            if (!rewrite(data, body_start + 4, next, read_u16(body), depth_guard + 1, state, children)) return false;
            write_tag_header(out, tag.code, children.size() + 4, long_form);
            out.insert(out.end(), body, body + 4);
            out.insert(out.end(), children.begin(), children.end());
            copied = true;
        } else if (tag.code == kTagPlaceObject2 || tag.code == kTagPlaceObject3) {
            Placement placement{};
            if (!parse_placement(tag.code, body, tag.length, placement)) return false;
            const Target* target = placement.has_matrix
                ? target_for(*state.targets, sprite, placement) : nullptr;
            if (target) {
                const std::vector<std::uint8_t> matrix =
                    write_matrix(transform(placement.matrix, *target, *state.layout));
                const std::size_t suffix = placement.matrix_offset + placement.matrix_size;
                write_tag_header(out, tag.code,
                    placement.matrix_offset + matrix.size() + (tag.length - suffix), long_form);
                out.insert(out.end(), body, body + placement.matrix_offset);
                out.insert(out.end(), matrix.begin(), matrix.end());
                out.insert(out.end(), body + suffix, body + tag.length);
                ++state.rewritten[static_cast<std::size_t>(target - state.targets->data())];
                copied = true;
            }
        }
        if (!copied) out.insert(out.end(), data.data() + tag.start, data.data() + next);
        position = next;
        if (tag.code == kTagEnd) break;
    }
    // Keep any trailing bytes after End exactly as they were.
    if (position < end) out.insert(out.end(), data.data() + position, data.data() + end);
    return true;
}

PatchResult failure(ErrorCode error, std::string message) {
    PatchResult result{};
    result.error = error;
    result.message = std::move(message);
    return result;
}

bool analyse(
    HudMovie movie,
    std::span<const std::uint8_t> input,
    Header& header,
    std::vector<NamedPlacement>& named,
    Resolution& resolution,
    std::string& message) {
    if (!read_header(input, header, message)) return false;
    if (!collect_named(input, header.body_start, header.declared, 0, 0, named)) {
        message = "malformed tag structure";
        return false;
    }
    resolution = resolve_targets(movie, named);
    return true;
}

} // namespace

std::span<const LayoutMovie> layout_movies() noexcept {
    return kMovies;
}

HudMovie classify_hud_movie(std::string_view url) noexcept {
    const std::size_t slash = url.find_last_of("/\\:");
    const std::string_view name = slash == std::string_view::npos ? url : url.substr(slash + 1);
    for (const LayoutMovie& movie : kMovies) {
        if (equals_ignore_case(name, movie.file)) return &movie;
    }
    return nullptr;
}

const char* hud_movie_name(HudMovie movie) noexcept {
    return movie ? movie->file : "none";
}

float hud_edge_offset_for(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0) return 0.0f;
    const float stage_width = kStageHeight * static_cast<float>(width) / static_cast<float>(height);
    return stage_width > kStageWidth ? (stage_width - kStageWidth) / 2.0f : 0.0f;
}

HudInspection inspect_hud_movie(HudMovie movie, std::span<const std::uint8_t> input) noexcept {
    HudInspection inspection{};
    try {
        Header header{};
        std::vector<NamedPlacement> named{};
        Resolution resolution{};
        if (!analyse(movie, input, header, named, resolution, inspection.message)) {
            inspection.error = ErrorCode::invalid_gfx;
            return inspection;
        }
        for (const Target& target : resolution.targets) {
            HudPlacement placement{};
            placement.name = target.rule->parent
                ? std::string(target.rule->parent) + "." + target.rule->name : target.rule->name;
            placement.x = target.parent_x + target.parent_scale_x * target.original.translate_x / 20.0f;
            placement.y = target.parent_y + target.parent_scale_y * target.original.translate_y / 20.0f;
            placement.scale_x = target.original.scale_x / static_cast<float>(kFixedOne);
            placement.scale_y = target.original.scale_y / static_cast<float>(kFixedOne);
            placement.horizontal = target.horizontal == Horizontal::left ? 'l'
                : target.horizontal == Horizontal::right ? 'r'
                : target.rule->horizontal == Horizontal::stretch ? 's'
                : target.rule->horizontal == Horizontal::cover ? 'v'
                : target.rule->horizontal == Horizontal::bar_fit ? 'f' : 'c';
            placement.vertical = target.rule->vertical == Vertical::top ? 't'
                : target.rule->vertical == Vertical::bottom ? 'b' : 'm';
            inspection.found.push_back(std::move(placement));
        }
        inspection.missing = std::move(resolution.missing);
        if (inspection.found.empty()) {
            inspection.error = ErrorCode::unsupported_gfx;
            inspection.message = "no anchored HUD element found";
        }
    } catch (...) {
        inspection.error = ErrorCode::invalid_gfx;
        inspection.message = "exception while reading the movie";
    }
    return inspection;
}

PatchResult patch_hud_movie(
    HudMovie movie, std::span<const std::uint8_t> input, const HudLayout& layout) noexcept {
    try {
        if (!movie || !std::isfinite(layout.edge_offset) || !std::isfinite(layout.scale) ||
            layout.edge_offset < 0.0f || layout.edge_offset > 4000.0f ||
            layout.scale < 0.25f || layout.scale > 4.0f) {
            return failure(ErrorCode::invalid_argument, "invalid HUD movie or layout");
        }
        Header header{};
        std::vector<NamedPlacement> named{};
        Resolution resolution{};
        std::string message{};
        if (!analyse(movie, input, header, named, resolution, message)) {
            return failure(ErrorCode::invalid_gfx, message);
        }
        if (resolution.targets.empty()) {
            return failure(ErrorCode::unsupported_gfx, "no anchored HUD element found");
        }
        std::string skipped{};
        for (const std::string& name : resolution.missing) {
            skipped += skipped.empty() ? "; skipped (not found): " : ", ";
            skipped += name;
        }

        PatchResult result{};
        if (layout.identity()) {
            result.report.already_satisfied = true;
            result.message = "identity layout" + skipped;
            return result;
        }

        RewriteState state{};
        state.targets = &resolution.targets;
        state.layout = &layout;
        state.rewritten.assign(resolution.targets.size(), 0);
        std::vector<std::uint8_t> output(input.begin(), input.begin() + header.body_start);
        output.reserve(input.size() + 1024);
        if (!rewrite(input, header.body_start, header.declared, 0, 0, state, output)) {
            return failure(ErrorCode::invalid_gfx, "malformed tag structure");
        }
        if (output.size() > std::numeric_limits<std::uint32_t>::max()) {
            return failure(ErrorCode::invalid_gfx, "output too large");
        }
        // The declared length moves with the edit; the padding is kept as is.
        const auto size = static_cast<std::uint32_t>(output.size());
        for (int index = 0; index < 4; ++index) output[4 + index] = static_cast<std::uint8_t>(size >> (8 * index));
        output.insert(output.end(), input.begin() + header.declared, input.end());

        // Verify: the output must parse and every element must sit where the
        // layout puts it.
        Header check_header{};
        std::vector<NamedPlacement> check_named{};
        Resolution check{};
        std::string check_message{};
        if (!analyse(movie, output, check_header, check_named, check, check_message) ||
            check.targets.size() != resolution.targets.size()) {
            return failure(ErrorCode::output_verification_failed, "patched movie does not parse back");
        }
        std::size_t moved_tags = 0;
        for (std::size_t index = 0; index < resolution.targets.size(); ++index) {
            const Matrix expected = transform(resolution.targets[index].original, resolution.targets[index], layout);
            const Matrix& actual = check.targets[index].original;
            if (check.targets[index].rule != resolution.targets[index].rule ||
                std::memcmp(&expected, &actual, sizeof(Matrix)) != 0 || state.rewritten[index] == 0) {
                return failure(ErrorCode::output_verification_failed,
                    std::string("unexpected placement after patching: ") + resolution.targets[index].rule->name);
            }
            moved_tags += state.rewritten[index];
        }

        result.output = std::move(output);
        result.report.bytes_added = result.output.size() >= input.size()
            ? result.output.size() - input.size() : 0;
        result.message = "moved " + std::to_string(resolution.targets.size()) + " elements (" +
            std::to_string(moved_tags) + " placement tags)" + skipped;
        return result;
    } catch (...) {
        return failure(ErrorCode::invalid_gfx, "exception while patching the movie");
    }
}

} // namespace erui::gfx
