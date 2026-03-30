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
     'Models/Buildings/StickFence.mdl', 'Models/Buildings/StickFence_Ghost.mdl',
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

-- Utility buildings
INSERT OR IGNORE INTO building_types VALUES
(50, 'Stone Ring',    'utility', 1, 1.5, 1.5, 0.5, 999, 0.0, 15.0, 0, 0, 0, 'free',
     'Models/Buildings/StoneRing.mdl', 'Models/Buildings/StoneRing_Ghost.mdl',
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
     'Stone dam in stream. Passive fish and gold collection.');

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
(55, 1, 20),(55, 11, 4);

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
INSERT OR IGNORE INTO wall_strength VALUES
(10, 1, 1), (10, 3, 1), (10, 2, 0), (10, 4, 0), (10, 5, 0), (10, 6, 0),
(20, 1, 1), (20, 2, 1), (20, 3, 1), (20, 4, 1), (20, 5, 1), (20, 6, 0),
(40, 6, 1);
