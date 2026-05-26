-- Building System Seed Data

-- Tier 1: Stick and hide
INSERT OR IGNORE INTO building_types VALUES
(1,  'Windbreak',     'shelter', 1, 3.0, 1.0, 1.5, 30,  2.0, 5.0,  0, 1, 0, 'free',
     'Models/Buildings/Windbreak.mdl', 'Models/Buildings/Windbreak_Ghost.mdl',
     'Three sticks and a hide. Blocks wind. Barely shelter.'),
(2,  'Lean-To',       'shelter', 1, 3.0, 2.0, 2.0, 50,  1.5, 10.0, 2, 1, 0, 'free',
     'Models/Buildings/LeanTo.mdl', 'Models/Buildings/LeanTo_Ghost.mdl',
     'Angled roof on poles. Rain protection. Sleep for one.'),
(3,  'Hide Tent',     'shelter', 1, 4.0, 4.0, 2.5, 80,  1.0, 15.0, 4, 2, 1, 'free',
     'Models/Buildings/HideTent.mdl', 'Models/Buildings/HideTent_Ghost.mdl',
     'Proper shelter. Fits a family. Respawn point.');

-- Tier 1: Stick fence
INSERT OR IGNORE INTO building_types VALUES
(10, 'Stick Fence',   'wall',    1, 2.0, 0.3, 1.2, 20,  3.0, 0.0, 0, 0, 0, 'wall',
     'Models/Buildings/Fence.mdl', 'Models/Buildings/StickFence_Ghost.mdl',
     'Woven sticks. Marks a boundary. Won''t stop much.'),
(11, 'Stick Gate',    'gate',    1, 2.0, 0.3, 1.2, 20,  3.0, 0.0, 0, 0, 0, 'gate',
     'Models/Buildings/StickGate.mdl', 'Models/Buildings/StickGate_Ghost.mdl',
     'Opening in stick fence. Toggles open/closed.');

-- Tier 2: Wood
INSERT OR IGNORE INTO building_types VALUES
(20, 'Wood Wall',     'wall',    2, 2.0, 0.4, 2.5, 150, 0.5, 0.0, 0, 0, 0, 'wall',
     'Models/Buildings/WoodWall.mdl', 'Models/Buildings/WoodWall_Ghost.mdl',
     'Log palisade. Keeps wolves out.'),
(21, 'Wood Gate',     'gate',    2, 2.0, 0.4, 2.5, 120, 0.5, 0.0, 0, 0, 0, 'gate',
     'Models/Buildings/WoodGate.mdl', 'Models/Buildings/WoodGate_Ghost.mdl',
     'Heavy wood gate. Barred from inside.'),
(22, 'Wood Corner',   'wall',    2, 0.4, 0.4, 2.5, 200, 0.5, 0.0, 0, 0, 0, 'corner',
     'Models/Buildings/WoodCorner.mdl', 'Models/Buildings/WoodCorner_Ghost.mdl',
     'Corner post. Walls snap to this.'),
(23, 'Hut',           'shelter', 2, 5.0, 5.0, 3.0, 200, 0.3, 20.0, 6, 3, 1, 'interior',
     'Models/Buildings/Hut.mdl', 'Models/Buildings/Hut_Ghost.mdl',
     'Wattle and daub. Warm, dry. Home.'),
(24, 'Longhouse',     'shelter', 2, 10.0,5.0, 3.5, 350, 0.3, 25.0, 12,6, 1, 'interior',
     'Models/Buildings/Longhouse.mdl', 'Models/Buildings/Longhouse_Ghost.mdl',
     'Extended dwelling. Room for a clan.');

-- Tier 2: Wood utility
INSERT OR IGNORE INTO building_types VALUES
(30, 'Storage Hut',   'storage', 2, 3.0, 3.0, 2.5, 150, 0.3, 0.0, 20,0, 0, 'interior',
     'Models/Buildings/StorageHut.mdl', 'Models/Buildings/StorageHut_Ghost.mdl',
     'Keeps items dry. 20 storage slots.'),
(31, 'Workshop',      'workshop',2, 4.0, 4.0, 2.5, 150, 0.3, 5.0, 0, 0, 0, 'interior',
     'Models/Buildings/Workshop.mdl', 'Models/Buildings/Workshop_Ghost.mdl',
     'Crafting station. Recipes requiring workshop unlock here.'),
(32, 'Watchtower',    'defense', 2, 2.0, 2.0, 5.0, 200, 0.5, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Watchtower.mdl', 'Models/Buildings/Watchtower_Ghost.mdl',
     'Elevated platform. Ranged advantage. Spot enemies further.');

-- Tier 3: Stone
INSERT OR IGNORE INTO building_types VALUES
(40, 'Stone Wall',    'wall',    3, 2.0, 0.6, 2.5, 500, 0.1, 0.0, 0, 0, 0, 'wall',
     'Models/Buildings/StoneWall.mdl', 'Models/Buildings/StoneWall_Ghost.mdl',
     'Stacked dry stone. Lasts generations.'),
(41, 'Stone Gate',    'gate',    3, 2.0, 0.6, 2.5, 400, 0.1, 0.0, 0, 0, 0, 'gate',
     'Models/Buildings/StoneGate.mdl', 'Models/Buildings/StoneGate_Ghost.mdl',
     'Heavy stone archway with wood door.'),
(42, 'Stone House',   'shelter', 3, 6.0, 6.0, 3.0, 600, 0.05,30.0, 10,4, 1, 'interior',
     'Models/Buildings/StoneHouse.mdl', 'Models/Buildings/StoneHouse_Ghost.mdl',
     'Permanent dwelling. Warm, strong, dry.');
-- (Phase 33 Iron Gate + Stone Watchtower defined by coder at ~line 119)

-- Utility buildings
INSERT OR IGNORE INTO building_types VALUES
(50, 'Stone Ring',    'utility', 1, 1.5, 1.5, 0.5, 999, 0.0, 15.0, 0, 0, 0, 'free',
     'Models/Buildings/Bonfire.mdl', 'Models/Buildings/Bonfire.mdl',
     'Fireplace. Warmth, cooking, light.'),
(51, 'Drying Rack',  'utility', 1, 2.0, 1.0, 2.0, 40,  1.0, 0.0,  3, 0, 0, 'free',
     'Models/Buildings/DryingRack.mdl', 'Models/Buildings/DryingRack_Ghost.mdl',
     'Hang meat to preserve it.'),
(52, 'Tanning Frame', 'utility',1, 2.0, 3.0, 2.0, 40,  1.0, 0.0,  4, 0, 0, 'free',
     'Models/Buildings/TanningFrame.mdl','Models/Buildings/TanningFrame_Ghost.mdl',
     'Stretch and scrape hides.'),
(53, 'Kiln',         'workshop',2, 2.0, 2.0, 2.0, 200, 0.2, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/Kiln.mdl', 'Models/Buildings/Kiln_Ghost.mdl',
     'Fire clay, smelt copper.'),
(54, 'Charcoal Kiln','workshop',2, 2.0, 2.0, 2.5, 150, 0.3, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/CharcoalKiln.mdl','Models/Buildings/CharcoalKiln_Ghost.mdl',
     'Slow-burn wood to charcoal.'),
(55, 'Fish Weir',    'utility', 1, 4.0, 2.0, 1.0, 60,  1.5, 0.0,  5, 0, 0, 'free',
     'Models/Buildings/FishWeir.mdl', 'Models/Buildings/FishWeir_Ghost.mdl',
     'Stone dam in stream. Passive fish and gold collection.'),
-- Fire system Phase 2b: Woodpile stores softwood + hardwood independently.
-- storageSlots field repurposed as per-wood-type capacity (20 burn-units each).
(56, 'Woodpile',     'utility', 1, 1.5, 1.0, 1.0, 80,  1.0, 0.0, 20, 0, 0, 'free',
     'Models/Buildings/Woodpile.mdl', 'Models/Buildings/Woodpile_Ghost.mdl',
     'Stack of firewood. Holds softwood and hardwood independently. Wetness from rain.');

-- Water Phase 4: Well
INSERT OR IGNORE INTO building_types VALUES
(57, 'Well',          'utility', 2, 1.5, 1.5, 1.5, 300, 0.1, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/Well.mdl', 'Models/Buildings/Well_Ghost.mdl',
     'Dug well. Provides water. Yield depends on terrain — high near rivers, low on hills.');

-- Water collection
INSERT OR IGNORE INTO building_types VALUES
(58, 'Water Barrel',  'utility', 2, 1.0, 1.0, 1.5, 80,  0.5, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/WaterBarrel.mdl', 'Models/Buildings/WaterBarrel_Ghost.mdl',
     'Bark and wood barrel. Collects rainwater passively. Place in the open — roofs block collection.');

-- Memorial
INSERT OR IGNORE INTO building_types VALUES
(60, 'Grave',         'memorial',0, 1.0, 1.0, 0.5, 10,  0.0, 0.0,  0, 0, 0, 'free',
     'Models/Nature/stump_round.mdl', NULL,
     'Stone cairn marking a burial. Remembers the dead.');

-- Transport
INSERT OR IGNORE INTO building_types VALUES
(70, 'Dugout Canoe',   'transport',3, 3.0, 1.5, 1.0, 100, 1.0, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Canoe.mdl', NULL,
     'Hollowed log canoe. Allows water crossing and deep-water fishing.');

-- Mining infrastructure (Phase 31)
INSERT OR IGNORE INTO building_types VALUES
(80, 'Mine Shaft',     'industry', 3, 2.0, 2.0, 3.0, 300, 0.3, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/MineShaft.mdl', 'Models/Buildings/MineShaft_Ghost.mdl',
     'Deep mining access. +50% yield within 15m. Requires Knapping 5+ and Woodwork 4+.'),
-- Phase 32: Forge — upgrade from Kiln, steel-only recipes
(81, 'Forge',          'industry', 4, 2.5, 2.5, 3.0, 400, 0.2, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Forge.mdl', 'Models/Buildings/Forge_Ghost.mdl',
     'Advanced metalwork. Steel recipes. +2 to metal craft DC rolls.'),
-- Phase 35: Workshops — specialised buildings with craft bonuses
(82, 'Smithy',         'workshop',4, 4.0, 4.0, 3.0, 300, 0.2, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Smithy.mdl', 'Models/Buildings/Smithy_Ghost.mdl',
     'Dedicated metalwork shop. +3 to metal craft DC.'),
(83, 'Tannery',        'workshop',3, 4.0, 3.0, 2.5, 200, 0.3, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Tannery.mdl', 'Models/Buildings/Tannery_Ghost.mdl',
     'Leather processing. +2 to leather craft DC.'),
(84, 'Granary',        'workshop',3, 5.0, 5.0, 4.0, 400, 0.1, 0.0, 0, 0, 100, 'free',
     'Models/Buildings/Granary.mdl', 'Models/Buildings/Granary_Ghost.mdl',
     'Bulk food storage. 5x slower decay. +100 food capacity.'),
(85, 'Herbalist Hut',  'workshop',2, 3.0, 3.0, 2.5, 150, 0.3, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/HerbalistHut.mdl', 'Models/Buildings/HerbalistHut_Ghost.mdl',
     'Medicinal herb workshop. +50% herb yield. Heal +10 HP.');

-- Military (Phase 36)
INSERT OR IGNORE INTO building_types VALUES
(90, 'Garrison',       'military',4, 5.0, 5.0, 3.5, 350, 0.2, 10.0, 0, 4, 0, 'free',
     'Models/Buildings/Garrison.mdl', 'Models/Buildings/Garrison_Ghost.mdl',
     'Barracks for guards. Holds 4 NPCs. Guards sleep here, respond faster to threats.');

-- Iron-age defences (Phase 33)
INSERT OR IGNORE INTO building_types VALUES
(43, 'Iron Gate',        'gate',    4, 2.0, 0.6, 2.5, 300, 0.05, 0.0, 0, 0, 0, 'gate',
     'Models/Buildings/IronGate.mdl', 'Models/Buildings/IronGate_Ghost.mdl',
     'Iron-reinforced gate. Predators cannot break through.'),
(33, 'Stone Watchtower', 'defense', 3, 2.5, 2.5, 6.0, 400, 0.1,  0.0, 0, 0, 0, 'free',
     'Models/Buildings/StoneWatchtower.mdl', 'Models/Buildings/StoneWatchtower_Ghost.mdl',
     'Taller stone tower. 80m detection radius. Spot enemies earlier.');

-- Loom (Weaving station, Phase 3 textiles)
INSERT OR IGNORE INTO building_types VALUES
(91, 'Loom',            'workshop',2, 2.5, 1.5, 2.0, 100, 0.5, 0.0, 0, 0, 0, 'free',
     'Models/Buildings/Loom.mdl', 'Models/Buildings/Loom_Ghost.mdl',
     'Wooden frame loom. Weave fine cloth, nets, and garments.');

-- Farming & livestock buildings
INSERT OR IGNORE INTO building_types VALUES
(86, 'Barn',            'shelter', 3, 8.0, 6.0, 4.0, 350, 0.2, 10.0, 20, 0, 0, 'free',
     'Models/Buildings/Barn.mdl', NULL,
     'Timber barn. Bulk storage for crops and materials.'),
(87, 'Silo',            'utility', 3, 2.0, 2.0, 5.0, 300, 0.1, 0.0, 40, 0, 0, 'free',
     'Models/Buildings/Silo.mdl', NULL,
     'Grain silo. Large capacity food storage with slow decay.'),
(88, 'Chicken Coop',    'utility', 2, 2.5, 2.5, 2.0, 100, 0.5, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/ChickenCoop.mdl', NULL,
     'Enclosed coop. Passive egg production.'),
(89, 'Windmill',        'workshop',3, 4.0, 4.0, 6.0, 300, 0.2, 0.0,  0, 0, 0, 'free',
     'Models/Buildings/Windmill.mdl', NULL,
     'Stone windmill. Grinds grain to flour. +2 to cooking DC.');

-- Furniture & decoration (Megakit props)
INSERT OR IGNORE INTO building_types VALUES
(100, 'Chest',           'furniture',  2, 1.0, 0.5, 0.8, 80,  0.3, 0.0, 8, 0, 0, 'free',
      'Models/Megakit/Chest_Wood.mdl', NULL,
      'Wooden chest. Personal storage.'),
(101, 'Bed',             'furniture',  2, 2.0, 1.0, 0.8, 60,  0.3, 0.0, 0, 1, 0, 'free',
      'Models/Megakit/Bed_Twin1.mdl', NULL,
      'Simple bed. Sleep indoors for warmth bonus.'),
(102, 'Bed (Large)',     'furniture',  2, 2.0, 1.2, 0.8, 60,  0.3, 0.0, 0, 1, 0, 'free',
      'Models/Megakit/Bed_Twin2.mdl', NULL,
      'Wider bed. Same function, different style.'),
(103, 'Bench',           'furniture',  1, 1.5, 0.5, 0.8, 40,  0.5, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Bench.mdl', NULL,
      'Wooden bench. Sit to rest stamina.'),
(104, 'Table',           'furniture',  2, 2.0, 1.0, 0.8, 60,  0.3, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Table_Large.mdl', NULL,
      'Large table. Crafting surface.'),
(105, 'Barrel',          'furniture',  1, 0.8, 0.8, 1.2, 80,  0.3, 0.0, 6, 0, 0, 'free',
      'Models/Megakit/Barrel.mdl', NULL,
      'Storage barrel. Holds liquids or dry goods.'),
(106, 'Apple Barrel',    'furniture',  1, 0.8, 0.8, 1.2, 80,  0.3, 0.0, 6, 0, 0, 'free',
      'Models/Megakit/Barrel_Apples.mdl', NULL,
      'Barrel of apples. Food storage.'),
(107, 'Bookcase',        'furniture',  2, 1.5, 0.5, 2.0, 60,  0.3, 0.0, 4, 0, 0, 'free',
      'Models/Megakit/Bookcase_2.mdl', NULL,
      'Tall bookcase. Stores scrolls and knowledge.'),
(108, 'Cauldron',        'furniture',  2, 1.0, 1.0, 0.8, 200, 0.1, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Cauldron.mdl', NULL,
      'Iron cauldron. Cooking station for stews and potions.'),
(109, 'Market Stall',    'decoration', 2, 3.0, 2.0, 2.5, 80,  0.5, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Stall_Empty.mdl', NULL,
      'Open market stall. Trade display.'),
(110, 'Market Cart',     'decoration', 2, 3.0, 2.0, 2.0, 80,  0.5, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Stall_Cart_Empty.mdl', NULL,
      'Wheeled market cart. Mobile trade.'),
(111, 'Chair',           'furniture',  1, 0.5, 0.5, 0.8, 30,  0.5, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Chair_1.mdl', NULL,
      'Wooden chair. Sit to rest.'),
(112, 'Cabinet',         'furniture',  2, 1.0, 0.5, 1.5, 60,  0.3, 0.0, 6, 0, 0, 'free',
      'Models/Megakit/Cabinet.mdl', NULL,
      'Storage cabinet. Keeps goods dry.'),
(113, 'Shelf',           'furniture',  2, 1.5, 0.4, 1.8, 40,  0.3, 0.0, 4, 0, 0, 'free',
      'Models/Megakit/Shelf_Simple.mdl', NULL,
      'Wall shelf. Display or storage.'),
(114, 'Arch Shelf',      'furniture',  2, 1.5, 0.4, 2.0, 50,  0.3, 0.0, 4, 0, 0, 'free',
      'Models/Megakit/Shelf_Arch.mdl', NULL,
      'Arched decorative shelf.'),
(115, 'Crate (Wood)',    'furniture',  1, 1.0, 1.0, 1.0, 40,  0.5, 0.0, 4, 0, 0, 'free',
      'Models/Megakit/Crate_Wooden.mdl', NULL,
      'Wooden crate. Basic storage.'),
(116, 'Crate (Metal)',   'furniture',  2, 1.0, 1.0, 1.0, 120, 0.1, 0.0, 6, 0, 0, 'free',
      'Models/Megakit/Crate_Metal.mdl', NULL,
      'Reinforced metal crate. Secure storage.'),
(117, 'Workbench',       'furniture',  2, 2.0, 1.0, 1.0, 80,  0.3, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Workbench.mdl', NULL,
      'Sturdy workbench. General crafting surface.'),
(118, 'Workbench (Drawers)','furniture',2, 2.0, 1.0, 1.0, 80, 0.3, 0.0, 4, 0, 0, 'free',
      'Models/Megakit/Workbench_Drawers.mdl', NULL,
      'Workbench with drawers. Crafting surface plus tool storage.'),
(119, 'Stool',           'furniture',  1, 0.4, 0.4, 0.6, 20,  0.5, 0.0, 0, 0, 0, 'free',
      'Models/Megakit/Stool.mdl', NULL,
      'Simple stool. Sit to rest stamina.');

-- Master Builder unlock: Longhouse (Woodwork 8+ required)
INSERT OR IGNORE INTO building_types VALUES
(25, 'Longhouse',      'shelter', 3, 10.0, 6.0, 3.5, 400, 0.3, 25.0, 16, 8, 1, 'interior',
     'Models/Buildings/Longhouse.mdl', 'Models/Buildings/Longhouse_Ghost.mdl',
     'Grand communal hall. 8 beds, 16 storage. Master Builder only.');

-- Building Recipes
-- Tier 1: Stick & hide
INSERT OR IGNORE INTO building_recipes VALUES
(1,  2, 3),  (1,  21, 1),
(2,  2, 6),  (2,  21, 2), (2,  41, 2),
(3,  2, 8),  (3,  21, 4), (3,  41, 4), (3, 20, 2),
(10, 2, 6),  (10, 3, 8),
(11, 2, 4),  (11, 3, 4), (11, 41, 2);

-- Tier 2: Wood
INSERT OR IGNORE INTO building_recipes VALUES
(20, 11, 4), (20, 42, 1),
(21, 11, 3), (21, 42, 1), (21, 22, 1),
(22, 11, 2), (22, 42, 1),
(23, 11, 8), (23, 12, 6), (23, 42, 3), (23, 4, 10),
(24, 11,16), (24, 12,12), (24, 42, 6), (24, 4, 20),
(30, 11, 6), (30, 12, 4), (30, 42, 2),
(31, 11, 6), (31, 12, 8), (31, 42, 2), (31, 1, 10),
(32, 11, 6), (32, 12, 4), (32, 42, 3);

-- Tier 3: Stone
INSERT OR IGNORE INTO building_recipes VALUES
(40, 1, 20),
(41, 1, 15), (41, 11, 2), (41, 22, 1),
(42, 1, 40), (42, 11, 8), (42, 4, 20), (42, 42, 4);

-- Utility
INSERT OR IGNORE INTO building_recipes VALUES
(50, 1, 8),
(51, 2, 4), (51, 41, 2), (51, 22, 1),
(52, 2, 4), (52, 22, 1), (52, 42, 1),
(53, 1, 15),(53, 4, 10),
(54, 1, 12),(54, 4, 6),
(55, 1, 20),(55, 11, 4),
-- Phase 2b: Woodpile recipe — sticks + plant fiber for the lashing
(56, 2, 4), (56, 3, 2),
-- Water Phase 4: Well — 10 stone + 5 softwood (Woodwork 4+)
(57, 1, 10), (57, 15, 5),
-- Rain Collection: Water Barrel — 5 planks + 2 She-Oak Bark (Woodwork 3+)
(58, 12, 5), (58, 879, 2);

-- Transport (Phase 26): Dugout Canoe — 5 planks
INSERT OR IGNORE INTO building_recipes VALUES
(70, 12, 5);

-- Mining infrastructure (Phase 31): Mine Shaft — 20 planks + 10 rough stone
INSERT OR IGNORE INTO building_recipes VALUES
(80, 12, 20), (80, 1, 10);

-- Iron-age defences (Phase 33): Iron Gate (3 iron ingots), Stone Watchtower (30 stone + 8 planks)
INSERT OR IGNORE INTO building_recipes VALUES
(43, 807, 3),
(33, 1, 30), (33, 12, 8);

-- Phase 32: Forge — 20 stone + 5 iron ingots
INSERT OR IGNORE INTO building_recipes VALUES
(81, 1, 20), (81, 807, 5);

-- Phase 35: Workshop recipes
INSERT OR IGNORE INTO building_recipes VALUES
(82, 1, 10), (82, 12, 5),        -- Smithy: 10 stone + 5 planks (near Forge)
(83, 12, 10), (83, 22, 5),       -- Tannery: 10 planks + 5 leather
(84, 12, 20), (84, 1, 10),       -- Granary: 20 planks + 10 stone
(85, 12, 10), (85, 720, 5);      -- Herbalist Hut: 10 planks + 5 medicinal herbs

-- Phase 36: Garrison — 20 planks + 10 stone + 5 iron ingots
INSERT OR IGNORE INTO building_recipes VALUES
(90, 12, 20), (90, 1, 10), (90, 807, 5);

-- Loom — 6 planks + 4 cordage + 4 sticks
INSERT OR IGNORE INTO building_recipes VALUES
(91, 12, 6), (91, 41, 4), (91, 2, 4);

-- Master Builder: Longhouse — 30 planks + 20 stone + 10 leather
INSERT OR IGNORE INTO building_recipes VALUES
(25, 12, 30), (25, 1, 20), (25, 22, 10);

-- Snap rules
INSERT OR IGNORE INTO snap_rules VALUES
('wall',   'wall',   2.0, 0.0, 'end'),
('wall',   'corner', 0.2, 0.0, 'end'),
('wall',   'gate',   2.0, 0.0, 'end'),
('gate',   'wall',   2.0, 0.0, 'end'),
('corner', 'wall',   0.0, 0.0, 'corner');

-- Repair costs
INSERT OR IGNORE INTO repair_costs VALUES
(20, 11, 1, 25),
(23, 11, 1, 20), (23, 4, 2, 20),
(40, 1, 3, 50);

-- Weather damage
INSERT OR IGNORE INTO weather_damage VALUES
('storm',      1, 5.0),
('storm',      2, 2.0),
('storm',      3, 0.0),
('blizzard',   1, 8.0),
('blizzard',   2, 3.0),
('heavy_rain', 1, 2.0);

-- Wall strength vs creatures
-- Tier 1 (stick, id=10): blocks small — rabbit, fox, shiba, husky, alpaca
-- Tier 2 (wood, id=20): blocks medium — all except bull
-- Tier 3 (stone, id=40): blocks everything
INSERT OR IGNORE INTO wall_strength VALUES
-- Stick walls (tier 1): small animals only
(10, 1, 1), (10, 3, 1), (10, 11, 1), (10, 12, 1), (10, 13, 1),
-- Wood walls (tier 2): everything except bull
(20, 1, 1), (20, 2, 1), (20, 3, 1), (20, 4, 1), (20, 5, 1),
(20, 7, 1), (20, 9, 1), (20, 10, 1), (20, 11, 1), (20, 12, 1), (20, 13, 1),
-- Stone walls (tier 3): blocks everything (higher tier than wood)
(40, 1, 1), (40, 2, 1), (40, 3, 1), (40, 4, 1), (40, 5, 1),
(40, 6, 1), (40, 7, 1), (40, 9, 1), (40, 10, 1), (40, 11, 1), (40, 12, 1), (40, 13, 1),
-- Iron Gate (tier 4, id=43): blocks ALL species — impenetrable
(43, 1, 1), (43, 2, 1), (43, 3, 1), (43, 4, 1), (43, 5, 1),
(43, 6, 1), (43, 7, 1), (43, 9, 1), (43, 10, 1), (43, 11, 1), (43, 12, 1), (43, 13, 1);
