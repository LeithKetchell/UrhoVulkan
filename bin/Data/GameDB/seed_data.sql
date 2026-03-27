-- Game Database Seed Data
-- Urho3D Survival Game
-- Apply after schema.sql: sqlite3 game_rules.db < seed_data.sql

-- ============================================================
-- ITEMS
-- ============================================================

-- Raw materials
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(1,   'Rough Stone',     'raw',      20, 0.5, 'A loose stone. Can be knapped into sharp stone.'),
(2,   'Stick',           'raw',      20, 0.3, 'A fallen branch. Foundation of many tools.'),
(3,   'Plant Fiber',     'raw',      30, 0.1, 'Pulled from tall grass. Weak binding material.'),
(4,   'Clay',            'raw',      15, 1.0, 'Dug from riverbanks. Used for pottery and kilns.'),
(5,   'Flint',           'raw',      10, 0.4, 'Sharp-edged stone. Better than rough stone for cutting.'),
(6,   'Berries',         'food',     20, 0.1, 'Seasonal. Restores a little health.'),
(7,   'Raw Meat',        'food',     5,  1.0, 'Spoils fast. Cook it or lose it.'),
(8,   'Cooked Meat',     'food',     5,  0.8, 'Nutritious. Lasts longer than raw.'),
(9,   'Dried Meat',      'food',     10, 0.5, 'Preserved. Lasts through winter.'),
(10,  'Small Fish',      'food',     10, 0.3, 'Caught bare-handed in shallows.'),
(11,  'Log',             'raw',      5,  3.0, 'Chopped from a tree. Heavy.'),
(12,  'Plank',           'material', 10, 1.5, 'Split from a log. Building material.');

-- Animal products
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(20,  'Sinew',           'raw',      20, 0.1, 'The gateway material. Binds everything.'),
(21,  'Hide',            'raw',      5,  2.0, 'Raw animal skin. Needs scraping.'),
(22,  'Leather',         'material', 5,  1.5, 'Processed hide. Durable, flexible.'),
(23,  'Bone',            'raw',      10, 0.3, 'Needles, hooks, small tools.'),
(24,  'Fur',             'raw',      5,  1.0, 'Warmth. Trade value.'),
(25,  'Gut',             'raw',      10, 0.2, 'Bowstrings, snare cord, rope.'),
(26,  'Fat',             'raw',      5,  0.5, 'Fuel, waterproofing, cooking.'),
(27,  'Antler',          'raw',      3,  0.8, 'Digging tool, weapon tip.'),
(28,  'Tusk',            'raw',      2,  1.0, 'Strong point. Digging, piercing.'),
(29,  'Claw',            'raw',      5,  0.2, 'Decoration, small blades.'),
(30,  'Feather',         'raw',      20, 0.05,'Arrow fletching.');

-- Processed materials
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(40,  'Sharp Stone',     'material', 10, 0.4, 'Knapped from rough stone. Cutting edge.'),
(41,  'Cordage',         'material', 10, 0.2, 'Twisted fiber. Stronger than raw.'),
(42,  'Rope',            'material', 5,  0.5, 'Braided cordage or gut. Strong.'),
(43,  'Charcoal',        'fuel',     15, 0.3, 'Burns hotter than wood. Needed for metal smelting.'),
(44,  'Fired Clay',      'material', 10, 1.0, 'Hardened in fire. Bricks, pots.');

-- Tools
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(100, 'Hand Axe',        'tool',     1,  1.5, 'Chop trees, split wood.'),
(101, 'Scraper',         'tool',     1,  0.8, 'Process hide into leather.'),
(102, 'Digging Stick',   'tool',     1,  1.0, 'Dig clay, plant seeds, shallow mining.'),
(103, 'Bone Needle',     'tool',     1,  0.1, 'Sew hide into clothing.'),
(104, 'Fire Kit',        'tool',     1,  0.5, 'Start fires. Friction method.'),
(105, 'Fishing Rod',     'tool',     1,  1.0, 'Reliable fishing.'),
(106, 'Stone Pick',      'tool',     1,  2.0, 'Open-cut mining.'),
(107, 'Hammer Stone',    'tool',     1,  1.5, 'Shaping, knapping, forging.');

-- Weapons
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(200, 'Spear',           'weapon',   1,  2.0, 'Melee. Reach advantage.'),
(201, 'Bow',             'weapon',   1,  1.0, 'Ranged. Needs arrows.'),
(202, 'Arrow',           'ammo',     20, 0.1, 'Ammunition for bow.'),
(203, 'Sling',           'weapon',   1,  0.3, 'Ranged. Uses stones as ammo.'),
(204, 'Club',            'weapon',   1,  2.0, 'Heavy. Blunt damage.'),
(205, 'Javelin',         'weapon',   3,  1.5, 'Thrown. One use per throw, recoverable.'),
(206, 'Knife',           'weapon',   1,  0.5, 'Fast, weak. Better for skinning than fighting.');

-- Armor & clothing
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(300, 'Hide Wrap',       'clothing', 1,  1.5, 'Basic coverage. Slight warmth.'),
(301, 'Leather Tunic',   'armor',    1,  3.0, 'First real armor.'),
(302, 'Fur Cloak',       'clothing', 1,  2.0, 'Warmth in winter. No armor value.'),
(303, 'Hide Boots',      'clothing', 1,  1.0, 'Foot protection. Faster on rough terrain.'),
(304, 'Leather Cap',     'armor',    1,  0.8, 'Head protection.'),
(305, 'Wood Shield',     'armor',    1,  3.0, 'Block hits. Requires one hand.');

-- Traps
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(400, 'Simple Snare',    'trap',     3,  0.5, 'Catches small animals. Single use.'),
(401, 'Meat Trap',       'trap',     1,  2.0, 'Baited with meat. Attracts carnivores. Gamble.'),
(402, 'Berry Trap',      'trap',     1,  2.0, 'Baited with berries. Attracts herbivores. Safe.'),
(403, 'Fish Trap',       'trap',     1,  1.5, 'Placed in water. Passive fish collection.'),
(404, 'Pit Trap',        'trap',     1,  0.0, 'Dug into ground. Large animals.'),
(405, 'Deadfall Trap',   'trap',     1,  3.0, 'Heavy stone, trigger stick. Kills outright on success.');

-- Containers
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(500, 'Leather Bag',     'container',1,  1.0, 'Carry +5 inventory slots.'),
(501, 'Clay Pot',        'container',1,  2.0, 'Store water, grain, or stew. Cooking vessel.'),
(502, 'Clay Jar',        'container',5,  1.5, 'Smaller storage. Preserves food longer.'),
(503, 'Basket',          'container',1,  0.5, 'Woven fiber. Carry foraged goods.');

-- Buildings/structures
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(600, 'Lean-To',         'building', 1,  0.0, 'Basic shelter. Blocks wind. Sleep here to heal.'),
(601, 'Hut',             'building', 1,  0.0, 'Real shelter. Warmth, storage, respawn point.'),
(602, 'Stone Ring',      'building', 1,  0.0, 'Fireplace. Required for cooking and fire kit use.'),
(603, 'Drying Rack',     'building', 1,  0.0, 'Hang meat to dry. Preserves food.'),
(604, 'Kiln',            'building', 1,  0.0, 'Fire clay, smelt copper. Needs charcoal.'),
(605, 'Storage Pit',     'building', 1,  0.0, 'Underground storage. Keeps food cool.'),
(606, 'Tanning Frame',   'building', 1,  0.0, 'Stretch and scrape hides. Bulk leather processing.'),
(607, 'Fish Weir',       'building', 1,  0.0, 'Stone dam in stream. Passive fish + gold collection.'),
(608, 'Charcoal Kiln',   'building', 1,  0.0, 'Slow-burn wood into charcoal.');

-- Seeds
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(700, 'Wheat Seed',      'seed',     20, 0.05,'Plant in spring, harvest in autumn.'),
(701, 'Flax Seed',       'seed',     20, 0.05,'Plant in spring. Yields fiber.'),
(702, 'Berry Bush Seed', 'seed',     10, 0.05,'Plant near water. Grows slowly.');

-- Metals (later tiers)
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, tier, description) VALUES
(800, 'Copper Ore',      'raw',      15, 1.0, 3, 'Smelted into copper. First metal.'),
(801, 'Tin Ore',         'raw',      15, 1.0, 3, 'Useless alone. Vital for bronze.'),
(802, 'Iron Ore',        'raw',      15, 1.5, 5, 'Needs high-temp forge to smelt.'),
(803, 'Gold Nugget',     'raw',      10, 2.0, 3, 'Trade currency. Too soft for tools.'),
(804, 'Copper Ingot',    'material', 10, 1.5, 3, 'Smelted copper. Soft metal.'),
(805, 'Tin Ingot',       'material', 10, 1.5, 3, 'Smelted tin. Combine with copper.'),
(806, 'Bronze Ingot',    'material', 10, 2.0, 4, 'The alloy that changes everything.'),
(807, 'Iron Ingot',      'material', 10, 2.5, 5, 'Superior metal.'),
(810, 'Copper Axe',      'tool',     1,  1.5, 3, 'Better than stone, still soft.'),
(811, 'Copper Spear',    'weapon',   1,  2.0, 3, 'Holds an edge longer than stone.'),
(812, 'Bronze Axe',      'tool',     1,  2.0, 4, 'Hard alloy. Real tool.'),
(813, 'Bronze Spear',    'weapon',   1,  2.0, 4, 'The weapon of armies.'),
(814, 'Bronze Sword',    'weapon',   1,  2.5, 4, 'Close combat. Fast, deadly.'),
(815, 'Bronze Armor',    'armor',    1,  8.0, 4, 'Heavy. Very hard to penetrate.'),
(816, 'Bronze Shield',   'armor',    1,  5.0, 4, 'Blocks most attacks.'),
(820, 'Iron Axe',        'tool',     1,  2.0, 5, 'Cuts anything.'),
(821, 'Iron Sword',      'weapon',   1,  3.0, 5, 'The pinnacle.'),
(822, 'Iron Armor',      'armor',    1,  10.0,5, 'Fortress on legs.');

-- ============================================================
-- RECIPES
-- ============================================================

-- Tier 0: Bare hands
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(1,  'Knap Sharp Stone',    40, 1, 0, 'Knock two stones together.'),
(2,  'Twist Cordage',       41, 1, 0, 'Twist fiber into cord.'),
(3,  'Simple Snare',        400,1, 0, 'A basic trap for small game.'),
(4,  'Basket',              503,1, 0, 'Woven from fiber. Carry more.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(1, 1, 2),
(2, 3, 3),
(3, 2, 2), (3, 3, 3),
(4, 3, 8);

-- Tier 0.5: Bait traps
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(10, 'Meat Trap',    401, 1, 0, 'Baited with meat. Attracts carnivores.'),
(11, 'Berry Trap',   402, 1, 0, 'Baited with berries. Attracts herbivores.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(10, 2, 3), (10, 3, 1), (10, 7, 1),
(11, 2, 3), (11, 3, 1), (11, 6, 3);

-- Tier 1: Sinew-bound tools
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(20, 'Hand Axe',     100, 1, 0, 'Sharp stone lashed to stick. Chops trees.'),
(21, 'Spear',        200, 1, 0, 'Reach. The first real weapon.'),
(22, 'Scraper',      101, 1, 0, 'For processing hides.'),
(23, 'Club',         204, 1, 0, 'Heavy stick. Blunt force.'),
(24, 'Knife',        206, 1, 0, 'Small, fast. Skinning tool.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(20, 40, 1), (20, 2, 1), (20, 20, 2),
(21, 2, 1),  (21, 40, 1),(21, 20, 2),
(22, 40, 1), (22, 2, 1), (22, 20, 1),
(23, 2, 1),  (23, 1, 1), (23, 20, 1),
(24, 5, 1),  (24, 2, 1), (24, 20, 1);

-- Tier 1: Processing
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(30, 'Scrape Hide',     22, 1, 101, 0,   'Leather from raw hide. Needs scraper.'),
(31, 'Fire Kit',        104,1, 0,   0,   'Friction fire. Two sticks and hope.'),
(32, 'Stone Ring',      602,1, 0,   0,   'Fireplace. Needed for cooking.'),
(33, 'Lean-To',         600,1, 100, 0,   'Basic shelter. Needs axe for poles.'),
(34, 'Bone Needle',     103,1, 0,   0,   'Carved from bone with sharp stone.'),
(35, 'Digging Stick',   102,1, 0,   0,   'Horn or antler tip on a stick.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(30, 21, 1),
(31, 2, 2), (31, 3, 1),
(32, 1, 8),
(33, 2, 6), (33, 22, 2), (33, 41, 3),
(34, 23, 1), (34, 40, 1),
(35, 2, 1), (35, 27, 1), (35, 20, 1);

-- Tier 1: Food & preservation
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(40, 'Cook Meat',        8,  1, 0, 602, 'Roast over fire. Heals more, lasts longer.'),
(41, 'Drying Rack',      603,1, 100, 0, 'Hang meat to dry. Needs axe.'),
(42, 'Dry Meat',         9,  2, 0, 603, 'Preserved. Survives winter.'),
(43, 'Fish Trap',        403,1, 0,   0, 'Passive fishing in water.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(40, 7, 1), (40, 43, 1),
(41, 2, 4), (41, 41, 2), (41, 22, 1),
(42, 7, 3),
(43, 2, 6), (43, 3, 4);

-- Tier 2: Advanced tools
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(50, 'Bow',           201, 1, 206, 'Stick + gut string. Ranged hunting.'),
(51, 'Arrow x5',      202, 5, 206, 'Fletched shafts.'),
(52, 'Fishing Rod',   105, 1, 0,   'Stick + gut + bone hook.'),
(53, 'Sling',         203, 1, 0,   'Hide + cordage. Flings stones.'),
(54, 'Javelin x3',    205, 3, 206, 'Thrown spear. Recoverable.'),
(55, 'Rope',          42,  1, 0,   'Braided cordage or gut. Strong.'),
(56, 'Stone Pick',    106, 1, 0,   'For mining surface ore.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(50, 2, 1), (50, 25, 1), (50, 20, 1),
(51, 2, 5), (51, 5, 3), (51, 30, 5),
(52, 2, 1), (52, 25, 1), (52, 23, 1),
(53, 21, 1),(53, 41, 2),
(54, 2, 3), (54, 40, 3), (54, 20, 3),
(55, 41, 5),
(56, 1, 2), (56, 2, 1), (56, 20, 2);

-- Tier 2: Clothing & armor
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(60, 'Hide Wrap',      300, 1, 103, 'Basic body coverage. Needs needle.'),
(61, 'Leather Tunic',  301, 1, 103, 'First real armor. Needs needle.'),
(62, 'Fur Cloak',      302, 1, 103, 'Warmth in winter. Needs needle.'),
(63, 'Hide Boots',     303, 1, 103, 'Foot protection.'),
(64, 'Leather Cap',    304, 1, 103, 'Head protection.'),
(65, 'Wood Shield',    305, 1, 100, 'Needs axe to shape planks.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(60, 21, 2), (60, 20, 2),
(61, 22, 3), (61, 20, 3),
(62, 24, 3), (62, 20, 2),
(63, 22, 1), (63, 20, 1),
(64, 22, 1), (64, 20, 1),
(65, 12, 3), (65, 20, 2), (65, 22, 1);

-- Tier 2: Buildings
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(70, 'Hut',            601, 1, 100, 'Real shelter. Warmth, storage, respawn.'),
(71, 'Storage Pit',    605, 1, 102, 'Underground storage. Keeps food cool.'),
(72, 'Tanning Frame',  606, 1, 100, 'Bulk hide processing.'),
(73, 'Split Log',      12,  2, 100, 'Axe splits log into planks.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(70, 11, 8), (70, 42, 2), (70, 22, 4), (70, 41, 4),
(71, 1, 10), (71, 11, 2),
(72, 2, 4), (72, 22, 1), (72, 42, 1),
(73, 11, 1);

-- Tier 3: Fire-based processing
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(80, 'Charcoal Kiln',   608, 1, 100, 0,   'Slow-burn wood to charcoal.'),
(81, 'Burn Charcoal',   43,  3, 0,   608, 'Logs become charcoal.'),
(82, 'Kiln',            604, 1, 100, 0,   'Clay + stone. Fires pottery, smelts copper.'),
(83, 'Fire Clay',       44,  2, 0,   604, 'Harden clay in kiln.'),
(84, 'Clay Pot',        501, 1, 0,   604, 'Cooking vessel, water storage.'),
(85, 'Clay Jar',        502, 2, 0,   604, 'Small sealed storage.'),
(86, 'Fish Weir',       607, 1, 100, 0,   'Stone dam in stream. Passive yield.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(80, 1, 12), (80, 4, 6),
(81, 11, 3),
(82, 1, 15), (82, 4, 10),
(83, 4, 3),
(84, 4, 5),
(85, 4, 3),
(86, 1, 20), (86, 11, 4);

-- Tier 3: Metal (copper age)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(90, 'Smelt Copper',    804, 1, 0, 604, 'Ore to ingot. Kiln heat.'),
(91, 'Smelt Tin',       805, 1, 0, 604, 'Ore to ingot.'),
(92, 'Alloy Bronze',    806, 1, 0, 604, 'Copper + tin = the alloy that changes everything.'),
(93, 'Copper Axe',      810, 1, 107, 604, 'Hammered copper. Better than stone.'),
(94, 'Copper Spear',    811, 1, 107, 604, 'Holds edge longer than stone.'),
(95, 'Bronze Axe',      812, 1, 107, 604, 'Hard alloy tool.'),
(96, 'Bronze Spear',    813, 1, 107, 604, 'The weapon of armies.'),
(97, 'Bronze Sword',    814, 1, 107, 604, 'Close combat. Fast.'),
(98, 'Bronze Armor',    815, 1, 107, 604, 'Heavy plate. Very protective.'),
(99, 'Bronze Shield',   816, 1, 107, 604, 'Blocks most attacks.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(90, 800, 3), (90, 43, 1),
(91, 801, 3), (91, 43, 1),
(92, 804, 2), (92, 805, 1), (92, 43, 2),
(93, 804, 2), (93, 2, 1), (93, 20, 2),
(94, 804, 1), (94, 2, 1), (94, 20, 2),
(95, 806, 2), (95, 2, 1), (95, 20, 2),
(96, 806, 1), (96, 2, 1), (96, 20, 2),
(97, 806, 2), (97, 22, 1), (97, 20, 1),
(98, 806, 5), (98, 22, 3), (98, 20, 4),
(99, 806, 2), (99, 11, 1), (99, 22, 1);

-- ============================================================
-- COMBAT STATS
-- ============================================================

INSERT OR IGNORE INTO combat_stats (item_id, attack_mod, damage, range, slot) VALUES
(200,  3, 4, 2.5, 'hand'),
(201,  2, 3, 30.0,'hand'),
(203,  1, 2, 20.0,'hand'),
(204,  1, 5, 1.5, 'hand'),
(205,  2, 4, 15.0,'hand'),
(206,  1, 2, 1.0, 'hand'),
(811,  4, 5, 2.5, 'hand'),
(813,  5, 6, 2.5, 'hand'),
(814,  6, 7, 1.5, 'hand'),
(821,  7, 9, 1.5, 'hand');

INSERT OR IGNORE INTO combat_stats (item_id, defense_mod, slot) VALUES
(300,  1, 'body'),
(301,  2, 'body'),
(302,  0, 'body'),
(303,  1, 'feet'),
(304,  1, 'head'),
(305,  3, 'offhand'),
(815,  5, 'body'),
(816,  5, 'offhand'),
(822,  6, 'body');

-- ============================================================
-- CREATURES
-- ============================================================

INSERT OR IGNORE INTO creatures VALUES
(1,  'Rabbit',  2,  0, 12, 0, 0,  9, 10.0, 'flee',        1, 'grassland',
     'Models/Animals/Rabbit.mdl', 'Idle', 'Run', NULL, 'Die', 0.3, 15.0),
(2,  'Deer',    8,  1, 11, 2, 4,  8, 20.0, 'flee',        3, 'forest',
     'Models/Animals/Deer.mdl', 'Idle', 'Run', 'Attack', 'Die', 1.2, 30.0),
(3,  'Fox',     5,  2, 13, 2, 4,  7, 15.0, 'flee',        1, 'forest',
     'Models/Animals/Fox.mdl', 'Idle', 'Gallop', 'Attack', 'Die', 0.5, 25.0),
(4,  'Boar',    15, 4, 12, 6, 6,  6,  8.0, 'aggressive',  1, 'forest',
     'Models/Animals/Boar.mdl', 'Idle', 'Run', 'Attack', 'Die', 0.8, 20.0),
(5,  'Wolf',    12, 5, 13, 5, 4,  8, 25.0, 'aggressive',  4, 'forest',
     'Models/Animals/Wolf.mdl', 'Idle', 'Run', 'Attack', 'Die', 0.7, 30.0),
(6,  'Bear',    30, 6, 14, 10,6,  5, 15.0, 'territorial', 1, 'forest',
     'Models/Animals/Bear.mdl', 'Idle', 'Walk', 'Attack', 'Die', 1.8, 25.0),
(7,  'Goat',    10, 2, 11, 3, 4,  6, 12.0, 'flee',        3, 'mountain',
     'Models/Animals/Goat.mdl', 'Idle', 'Run', 'Attack', 'Die', 0.8, 20.0),
(8,  'Fish',    1,  0, 14, 0, 0,  8,  5.0, 'flee',        1, 'water',
     'Models/Animals/Fish.mdl', 'Swim', 'Swim', NULL, NULL, 0.2, 10.0),
(9,  'Bird',    1,  0, 16, 0, 0, 10, 20.0, 'flee',        5, 'any',
     'Models/Animals/Bird.mdl', 'Idle', 'Fly', NULL, NULL, 0.15, 40.0);

-- ============================================================
-- LOOT TABLES
-- ============================================================

-- Rabbit
INSERT OR IGNORE INTO loot_table VALUES
(1, 20, 1, 1.0, 0),
(1, 7,  1, 1.0, 0),
(1, 24, 1, 0.8, 0),
(1, 23, 1, 0.5, 0);

-- Deer
INSERT OR IGNORE INTO loot_table VALUES
(2, 20, 3, 1.0, 0),
(2, 7,  3, 1.0, 0),
(2, 21, 2, 1.0, 0),
(2, 23, 2, 0.8, 0),
(2, 27, 1, 0.7, 0),
(2, 25, 1, 0.5, 206);

-- Fox
INSERT OR IGNORE INTO loot_table VALUES
(3, 20, 1, 1.0, 0),
(3, 7,  1, 1.0, 0),
(3, 24, 1, 0.9, 0),
(3, 23, 1, 0.5, 0);

-- Boar
INSERT OR IGNORE INTO loot_table VALUES
(4, 20, 2, 1.0, 0),
(4, 7,  4, 1.0, 0),
(4, 21, 2, 1.0, 0),
(4, 28, 1, 0.8, 0),
(4, 26, 2, 0.7, 206),
(4, 25, 1, 0.5, 206);

-- Wolf
INSERT OR IGNORE INTO loot_table VALUES
(5, 20, 2, 1.0, 0),
(5, 7,  2, 1.0, 0),
(5, 24, 1, 1.0, 0),
(5, 23, 2, 0.7, 0),
(5, 25, 1, 0.6, 206);

-- Bear
INSERT OR IGNORE INTO loot_table VALUES
(6, 20, 5, 1.0, 0),
(6, 7,  6, 1.0, 0),
(6, 21, 3, 1.0, 0),
(6, 26, 3, 1.0, 206),
(6, 23, 3, 0.8, 0),
(6, 29, 2, 0.9, 0);

-- Goat
INSERT OR IGNORE INTO loot_table VALUES
(7, 20, 2, 1.0, 0),
(7, 7,  2, 1.0, 0),
(7, 21, 1, 1.0, 0),
(7, 23, 1, 0.7, 0),
(7, 25, 1, 0.4, 206);

-- Fish
INSERT OR IGNORE INTO loot_table VALUES
(8, 7,  1, 1.0, 0),
(8, 23, 1, 0.3, 0);

-- Bird
INSERT OR IGNORE INTO loot_table VALUES
(9, 7,  1, 1.0, 0),
(9, 30, 3, 1.0, 0);

-- ============================================================
-- GATHER SOURCES
-- ============================================================

INSERT OR IGNORE INTO gather_sources VALUES
(1, 'Loose Stone',    1,  1, 0,   'any',       -999, 999, 'Models/Props/Stone.mdl',     0,    'any'),
(2, 'Fallen Stick',   2,  1, 0,   'forest',    -999, 999, 'Models/Props/Stick.mdl',     600,  'any'),
(3, 'Tall Grass',     3,  2, 0,   'grassland', -999, 10,  'Models/Props/TallGrass.mdl', 300,  'spring,summer'),
(4, 'River Clay',     4,  1, 0,   'riverbank', -999, 8,   NULL,                         1800, 'any'),
(5, 'Berry Bush',     6,  3, 0,   'forest',    -999, 15,  'Models/Props/BerryBush.mdl', 3600, 'summer,autumn'),
(6, 'Flint Outcrop',  5,  2, 0,   'mountain',  15,   999, 'Models/Props/FlintRock.mdl', 0,    'any'),
(7, 'Reed Bed',       3,  3, 0,   'water',     -999, 7,   'Models/Props/Reeds.mdl',     600,  'spring,summer,autumn');

-- ============================================================
-- TRAP RULES
-- ============================================================

INSERT OR IGNORE INTO trap_rules VALUES
(400, 1, 2,  0, 20.0, 60.0),
(400, 3, 8,  0, 15.0, 90.0),
(401, 5, 10, 7, 40.0, 180.0),
(401, 6, 16, 7, 30.0, 300.0),
(401, 4, 8,  7, 30.0, 120.0),
(402, 2, 6,  6, 40.0, 120.0),
(402, 7, 8,  6, 30.0, 150.0),
(403, 8, 2,  0, 10.0, 60.0),
(404, 4, 5,  7, 20.0, 120.0),
(404, 2, 4,  6, 25.0, 90.0),
(404, 6, 10, 7, 20.0, 300.0),
(405, 4, 3,  7, 15.0, 120.0),
(405, 5, 6,  7, 20.0, 180.0);

-- ============================================================
-- FOOD PROPERTIES
-- ============================================================

INSERT OR IGNORE INTO food_properties VALUES
(6,  5,  1,  0,    300,   0, 0),
(7,  15, 3,  0,    180,   0, 0.1),
(8,  20, 5,  0.1,  1800,  1, 0),
(9,  12, 3,  0,    86400, 1, 0),
(10, 8,  2,  0,    120,   0, 0);

-- ============================================================
-- CLIMATE RULES
-- ============================================================

INSERT OR IGNORE INTO climate_rules VALUES
('spring', 'day',   15.0, 2.0, 3.0),
('spring', 'night', 8.0,  3.0, 4.0),
('summer', 'day',   25.0, 1.0, 2.0),
('summer', 'night', 15.0, 2.0, 3.0),
('autumn', 'day',   10.0, 3.0, 4.0),
('autumn', 'night', 3.0,  4.0, 5.0),
('winter', 'day',   2.0,  5.0, 6.0),
('winter', 'night', -5.0, 7.0, 8.0);

-- ============================================================
-- CLOTHING WARMTH
-- ============================================================

INSERT OR IGNORE INTO clothing_warmth VALUES
(300, 3.0, 0.1),
(301, 4.0, 0.2),
(302, 8.0, 0.3),
(303, 2.0, 0.1),
(304, 2.0, 0.1);

-- ============================================================
-- BUILDING RULES
-- ============================================================

INSERT OR IGNORE INTO building_rules VALUES
(600, 0.8,  0, 0, 3.0, 2.0, 0,  10.0, 0),
(601, 0.85, 0, 0, 4.0, 4.0, 10, 20.0, 1),
(602, 0.7,  0, 0, 1.5, 1.5, 0,  15.0, 0),
(603, 0.8,  0, 0, 2.0, 1.0, 3,  0,    0),
(604, 0.85, 0, 0, 2.0, 2.0, 0,  0,    0),
(605, 0.85, 0, 0, 2.0, 2.0, 15, 0,    0),
(606, 0.8,  1, 0, 2.0, 3.0, 4,  0,    0),
(607, 0.5,  1, 0, 4.0, 2.0, 5,  0,    0),
(608, 0.8,  0, 0, 2.0, 2.0, 0,  0,    0);
