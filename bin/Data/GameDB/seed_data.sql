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
(12,  'Plank',           'material', 10, 1.5, 'Split from a log. Building material.'),
(13,  'Milk',            'food',     5,  0.5, 'From a tamed cow. Nutritious.');

-- Fire fuel (Fire System Phase 1a — IDs coordinated with ResourceMap.h Phase 1b)
-- Softwood + hardwood are BOTH required for friction ignition per PLAN_FIRE_SYSTEM.md.
-- Category 'fuel' matches Charcoal (id 43). Rock for fire-pit construction reuses
-- item 1 'Rough Stone' — no new rock item is needed.
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(15,  'Softwood',        'fuel',     15, 0.8, 'Lightweight wood. Kindles easily, burns fast. Paired with hardwood for friction fires.'),
(16,  'Hardwood',        'fuel',     10, 1.2, 'Dense wood. Slow to light, long burn. Paired with softwood for friction fires.');

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
(107, 'Hammer Stone',    'tool',     1,  1.5, 'Shaping, knapping, forging.'),
(108, 'Torch',           'tool',     3,  0.6, 'Unlit. Light from an existing fire to carry flame.'),
(109, 'Burning Torch',   'tool',     1,  0.6, 'Lit torch. Burns for a few minutes. Can ignite a cold fire pit.');

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

-- Crop harvest yields
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, description) VALUES
(710, 'Wheat Grain',     'food',     20, 0.2, 'Harvested wheat. Edible raw, better cooked.'),
(711, 'Flax Fiber',      'raw',      20, 0.15,'Harvested flax. Strong fiber for cordage.'),
(712, 'Fresh Berries',   'food',     15, 0.1, 'Cultivated berries. Reliable food source.'),
(720, 'Medicinal Herbs', 'material', 10, 0.2, 'Forest herbs. Heals wounds, fights infection.');

INSERT OR IGNORE INTO food_properties (item_id, hunger, health, warmth, spoil_time, cooked, poison_chance) VALUES
(710, 8, 1, 0, 3600, 0, 0),
(712, 6, 1, 0, 600, 0, 0);

-- Crop type rules (seed -> harvest mapping)
INSERT OR IGNORE INTO crop_types (seed_item_id, harvest_item_id, harvest_qty, seed_return, plant_season, harvest_season, grow_days, min_flat, near_water_range, tool_req, model) VALUES
(700, 710, 4, 2, 'spring', 'autumn', 90, 0.9, 30.0, 102, 'Models/Crops/WheatCrop.mdl'),
(701, 711, 3, 1, 'spring', 'autumn', 90, 0.9, 30.0, 102, 'Models/Crops/FlaxCrop.mdl'),
(702, 712, 5, 2, 'any',    'any',    60, 0.85, 15.0, 102, 'Models/Crops/BerryBush.mdl');

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
(817, 'Bronze Knife',    'weapon',   1,  1.0, 4, 'Fast, sharp. Skinning and close work.'),
(820, 'Iron Axe',        'tool',     1,  2.0, 5, 'Cuts anything.'),
(821, 'Iron Sword',      'weapon',   1,  3.0, 5, 'The pinnacle.'),
(822, 'Iron Armor',      'armor',    1,  10.0,5, 'Fortress on legs.'),
-- Phase 32: Steel Age
(825, 'Steel Ingot',     'material', 10, 3.0, 6, 'Hardened iron-carbon alloy. The apex of metallurgy.'),
(830, 'Steel Axe',       'tool',     1,  2.5, 6, 'Unbreakable edge. Fells trees in half the time.'),
(831, 'Steel Sword',     'weapon',   1,  3.5, 6, 'Holds its edge forever. The ultimate blade.'),
(832, 'Steel Armor',     'armor',    1,  12.0,6, 'Plate steel. Near-impenetrable.'),
(833, 'Steel Spear',     'weapon',   1,  2.5, 6, 'Long reach. Formation weapon.'),
(834, 'Iron Shield',     'armor',    1,  6.0, 5, 'Iron-bound wood. Blocks one strike per round.'),
(835, 'Steel Shield',    'armor',    1,  7.0, 6, 'Full steel. The wall between life and death.'),
-- Phase 37: Trace element ores
(840, 'Manganese Ore',   'raw',      10, 2.0, 5, 'Dark heavy ore. Toughens steel when smelted.'),
(841, 'Chromium Ore',    'raw',      10, 2.5, 6, 'Silvery ore. Resists corrosion.'),
(842, 'Tungsten Ore',    'raw',      5,  4.0, 7, 'Extremely dense. Holds an edge like nothing else.'),
(843, 'Nickel Ore',      'raw',      10, 2.0, 5, 'Pale metallic ore. Resists cold and weather.'),
(844, 'Rich Carbon',     'raw',      15, 1.0, 4, 'Black mineral carbon. Hardens iron dramatically.'),
-- Phase 38: Mystery alloy
(850, 'Mystery Alloy Ingot', 'material', 5, 3.0, 5, 'Strange metal. Different from normal iron. Properties unknown.'),
-- Phase 40: Named alloy ingots
(851, 'Manganese Steel Ingot', 'material', 5, 3.0, 7, 'Tough steel. Absorbs impact without cracking.'),
(852, 'Chromium Steel Ingot',  'material', 5, 3.0, 7, 'Stainless steel. Resists corrosion and weather.'),
(853, 'Tungsten Steel Ingot',  'material', 5, 4.0, 8, 'Ultra-hard steel. Holds an edge indefinitely.'),
(854, 'Nickel Steel Ingot',    'material', 5, 3.0, 7, 'Weather-proof steel. Performs in any conditions.'),
(855, 'Carbon Steel Ingot',    'material', 5, 3.0, 6, 'Hard but brittle. The sharpest edge known.'),
-- Phase 41: Hybrid alloy
(860, 'Hybrid Alloy Ingot',   'material', 5, 4.0, 8, 'Master-blended steel. Two trace elements fused. The apex.'),
-- Fire Carrying Phase 1: Tree Resin
(870, 'Tree Resin',           'material', 20, 0.3, 1, 'Sticky aromatic sap from Acacia. Burns slowly. Essential for resin torches.'),
-- Fire Carrying Phase 2: Resin Torch
(871, 'Resin Torch',          'weapon',   1, 0.8, 2, 'Slow-burning torch coated in tree resin. Survives light rain. Burns for hours.'),
-- Fire Carrying Phase 3: Fire Bundle
(872, 'Fire Bundle',          'material', 1, 0.5, 3, 'Bark-wrapped resin bundle. Carries fire for a full day. Unwrap to light a torch.'),
-- Fire Carrying Phase 4: Fire Pot
(873, 'Bark Vessel',          'material', 1, 0.8, 3, 'Hardwood bark holding wet embers. Water slows the burn to a days-long smolder.'),
-- Botanical Discovery: Willow Bark
(875, 'Willow Extract',        'material', 10, 0.3, 2, 'Rooting hormone from willow bark. Apply to cuttings and seeds to accelerate growth.'),
-- Botanical Discovery: Eucalyptus
(877, 'Eucalyptus Leaves',   'material', 15, 0.1, 1, 'Pungent aromatic leaves. Clears lungs, wards illness, repels insects.'),
(879, 'She-Oak Bark',        'material', 10, 0.4, 2, 'Thick waterproof bark from She-Oak. Shaped into vessels that hold water or fire.'),
(878, 'Eucalyptus Poultice', 'consumable', 5, 0.2, 2, 'Crushed leaf compress. Heals wounds and prevents infection. Restores 20 HP.');

-- Textiles and Weaving
INSERT OR IGNORE INTO items (id, name, category, stack_max, weight, durability, description) VALUES
(410, 'Reed Basket',     'container', 3,  0.8, 1, 'Woven basket. Storage and fish trapping.'),
(411, 'Woven Mat',       'material',  5,  1.0, 1, 'Floor covering. Slight warmth bonus.'),
(412, 'Flax Thread',     'material', 20,  0.1, 1, 'Spun from flax fiber. Weaving input.'),
(413, 'Rough Cloth',     'material', 10,  0.5, 1, 'Hand-woven cloth. Coarse but functional.'),
(414, 'Fine Cloth',      'material', 10,  0.4, 1, 'Loom-woven. Smooth, strong.'),
(415, 'Cloth Wrap',      'clothing',  1,  0.8, 2, 'Simple cloth garment. Cool and light.'),
(416, 'Cloth Tunic',     'clothing',  1,  1.0, 2, 'Proper garment. Decent warmth.'),
(417, 'Cloth Bandage',   'medical',  10,  0.1, 1, 'Stops bleeding. Heals minor wounds.'),
(418, 'Fishing Net',     'tool',      1,  1.5, 2, 'Cast net. Area catch in shallow water.'),
(419, 'Cargo Net',       'tool',      1,  2.0, 2, 'Carry more. +4 inventory slots when equipped.'),
-- Iron Age Textiles: Wool from alpacas
(420, 'Wool',            'material', 10,  0.3, 2, 'Raw alpaca wool. Warm and soft.'),
(421, 'Wool Thread',     'material', 10,  0.2, 2, 'Spun wool. Ready for weaving.'),
(422, 'Wool Cloth',      'material', 10,  0.5, 3, 'Woven wool fabric. Excellent insulation.'),
(423, 'Wool Tunic',      'clothing',  1,  1.2, 3, 'Warm wool garment. Better than hide.'),
(424, 'Wool Cloak',      'clothing',  1,  1.8, 3, 'Heavy wool cloak. Best warmth.');

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
(35, 'Digging Stick',   102,1, 0,   0,   'Horn or antler tip on a stick.'),
(36, 'Torch',           108,1, 0,   0,   'Stick wrapped in fiber. Light from fire to carry.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(30, 21, 1),
(31, 2, 2), (31, 3, 1),
(32, 1, 8),
(33, 2, 6), (33, 22, 2), (33, 41, 3),
(34, 23, 1), (34, 40, 1),
(35, 2, 1), (35, 27, 1), (35, 20, 1),
(36, 2, 1), (36, 3, 2);

-- Tier 1: Food & preservation
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(40, 'Cook Meat',        8,  1, 0, 602, 'Roast over fire. Heals more, lasts longer.'),
(41, 'Drying Rack',      603,1, 100, 0, 'Hang meat to dry. Needs axe.'),
(42, 'Dry Meat',         9,  2, 0, 603, 'Preserved. Survives winter.'),
(43, 'Fish Trap',        403,1, 0,   0, 'Woven trap placed in water. Passive fish collection.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(40, 7, 1), (40, 43, 1),
(41, 2, 4), (41, 41, 2), (41, 22, 1),
(42, 7, 3),
(43, 410, 1), (43, 2, 4);   -- Reed Basket x1 + Stick x4 (was: 6 Stick + 4 Plant Fiber)

-- Tier 1: Weaving basics (craft_fiber → SKILL_WEAVING)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(150, 'Reed Basket',   410, 1, 0,   0,  'Woven from plant fiber. First weaving product.'),
(151, 'Woven Mat',     411, 1, 0,   0,  'Plaited fiber mat. Floor and bedding.'),
(152, 'Flax Thread',   412, 3, 0,   0,  'Spin flax into thread.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(150, 3, 8),          -- Plant Fiber x8
(151, 3, 12),         -- Plant Fiber x12
(152, 711, 2);        -- Flax Fiber x2

-- Tier 2: Hand-woven cloth (needs Bone Needle)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(153, 'Rough Cloth',   413, 1, 103, 0,  'Hand-weave thread into cloth. Needs Bone Needle.'),
(155, 'Cloth Wrap',    415, 1, 103, 0,  'Simple cloth garment.'),
(157, 'Cloth Bandage', 417, 3, 103, 0,  'Tear cloth into strips.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(153, 412, 4),         -- Flax Thread x4
(155, 413, 2), (155, 41, 1),  -- Rough Cloth x2 + Cordage x1
(157, 413, 1);         -- Rough Cloth x1

-- Tier 3: Loom-woven textiles (station_req = 91 = Loom)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(154, 'Fine Cloth',    414, 2, 103, 91, 'Loom-woven cloth. Superior quality.'),
(156, 'Cloth Tunic',   416, 1, 103, 91, 'Loom-made tunic. Good warmth.'),
(158, 'Fishing Net',   418, 1, 0,   91, 'Woven net for area fishing. Loom required.'),
(159, 'Cargo Net',     419, 1, 0,   91, 'Woven carry net. Loom required.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(154, 412, 6),         -- Flax Thread x6
(156, 414, 3), (156, 412, 2),  -- Fine Cloth x3 + Flax Thread x2
(158, 412, 10), (158, 41, 4),  -- Flax Thread x10 + Cordage x4
(159, 412, 8), (159, 41, 6);   -- Flax Thread x8 + Cordage x6

-- Tier 4: Wool textiles (station_req = 91 = Loom for cloth/garments, 0 for spinning)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(160, 'Spin Wool',      421, 2, 0,   0,  'Spin raw wool into thread by hand.'),
(161, 'Wool Cloth',     422, 1, 103, 91, 'Weave wool thread on loom. Warm fabric.'),
(162, 'Wool Tunic',     423, 1, 103, 91, 'Sew wool garment. Better than hide.'),
(163, 'Wool Cloak',     424, 1, 103, 91, 'Heavy wool cloak. Best winter protection.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(160, 420, 3),                           -- Wool x3 → Wool Thread x2
(161, 421, 4),                           -- Wool Thread x4 → Wool Cloth x1
(162, 422, 3), (162, 421, 2),            -- Wool Cloth x3 + Wool Thread x2 → Wool Tunic
(163, 422, 5), (163, 421, 3), (163, 24, 2);  -- Wool Cloth x5 + Wool Thread x3 + Fur x2 → Wool Cloak

-- Tier assignments for weaving recipes
UPDATE recipes SET tier = 1 WHERE id IN (150, 151, 152);
UPDATE recipes SET tier = 2 WHERE id IN (153, 155, 157);
UPDATE recipes SET tier = 3 WHERE id IN (154, 156, 158, 159);
UPDATE recipes SET tier = 4 WHERE id IN (160, 161, 162, 163);

-- Craft action for weaving recipes → craft_fiber → SKILL_WEAVING
-- (CraftForOwner maps craft_fiber to SKILL_WEAVING at line 7036)

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
(99, 'Bronze Shield',   816, 1, 107, 604, 'Blocks most attacks.'),
(100,'Bronze Knife',    817, 1, 107, 604, 'Fast, sharp. Skinning tool.');

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
(99, 806, 2), (99, 11, 1), (99, 22, 1),
(100, 806, 1), (100, 22, 1), (100, 20, 1);

-- Tier 5: Iron Age (DC 20 — master smith only)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(110, 'Smelt Iron',     807, 1, 0,   604, 'High-temp smelt. Kiln only.'),
(111, 'Iron Axe',       820, 1, 107, 604, 'Forge an iron axe. Cuts anything.'),
(112, 'Iron Sword',     821, 1, 107, 604, 'Forge an iron sword. The pinnacle.'),
(113, 'Iron Armor',     822, 1, 107, 604, 'Forge iron armor. Fortress on legs.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(110, 802, 5), (110, 43, 3),          -- Iron Ore x5 + Charcoal x3
(111, 807, 2), (111, 2, 1), (111, 20, 2),  -- Iron Ingot x2 + Stick + Sinew x2
(112, 807, 2), (112, 22, 1), (112, 20, 1), -- Iron Ingot x2 + Leather + Sinew
(113, 807, 5), (113, 22, 3), (113, 20, 4); -- Iron Ingot x5 + Leather x3 + Sinew x4

-- Tier 6: Steel Age (DC 23 — master smith at Forge)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(120, 'Forge Steel',     825, 1, 107, 81, 'Iron-carbon alloy. Forge only.'),
(121, 'Steel Axe',       830, 1, 107, 81, 'Unbreakable edge.'),
(122, 'Steel Sword',     831, 1, 107, 81, 'The ultimate blade.'),
(123, 'Steel Armor',     832, 1, 107, 81, 'Near-impenetrable plate.'),
(124, 'Steel Spear',     833, 1, 107, 81, 'Formation weapon. Long reach.'),
(125, 'Iron Shield',     834, 1, 107, 81, 'Iron-bound wood shield.'),
(126, 'Steel Shield',    835, 1, 107, 81, 'Full steel shield.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(120, 807, 2), (120, 43, 2),                -- Iron Ingot x2 + Charcoal x2
(121, 825, 2), (121, 2, 1), (121, 20, 2),   -- Steel Ingot x2 + Stick + Sinew x2
(122, 825, 2), (122, 22, 1), (122, 20, 1),  -- Steel Ingot x2 + Leather + Sinew
(123, 825, 5), (123, 22, 3), (123, 20, 4),  -- Steel Ingot x5 + Leather x3 + Sinew x4
(124, 825, 1), (124, 2, 2), (124, 20, 2),   -- Steel Ingot + Stick x2 + Sinew x2
(125, 807, 2), (125, 12, 2), (125, 22, 1),  -- Iron Ingot x2 + Plank x2 + Leather
(126, 825, 3), (126, 22, 1);                -- Steel Ingot x3 + Leather

-- Tier 7: Named Alloys (Phase 40 — settlement-gated via alloy_knowledge)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(130, 'Manganese Steel', 851, 1, 107, 81, 'Tough alloy. Impact resistant.'),
(131, 'Chromium Steel',  852, 1, 107, 81, 'Stainless alloy. Corrosion proof.'),
(132, 'Tungsten Steel',  853, 1, 107, 81, 'Ultra-hard alloy. Extreme edge retention.'),
(133, 'Nickel Steel',    854, 1, 107, 81, 'Weather-proof alloy. Cold resistant.'),
(134, 'Carbon Steel',    855, 1, 107, 81, 'Hard brittle alloy. Maximum edge.');

INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(130, 807, 2), (130, 840, 1), (130, 43, 2),  -- Iron x2 + Manganese Ore + Charcoal x2
(131, 807, 2), (131, 841, 1), (131, 43, 2),  -- Iron x2 + Chromium Ore + Charcoal x2
(132, 807, 2), (132, 842, 1), (132, 43, 3),  -- Iron x2 + Tungsten Ore + Charcoal x3
(133, 807, 2), (133, 843, 1), (133, 43, 2),  -- Iron x2 + Nickel Ore + Charcoal x2
(134, 807, 2), (134, 844, 2), (134, 43, 1);  -- Iron x2 + Rich Carbon x2 + Charcoal x1

-- ============================================================
-- RECIPE TIERS (D&D-style DC: higher tier = harder to craft)
-- DC = 5 + tier * 3.  Skill check: d20 + craftLevel >= DC.
-- Tier 0: DC 5  (trivial — knapping, twisting fiber)
-- Tier 1: DC 8  (easy — lashing, simple traps, basic tools)
-- Tier 2: DC 11 (moderate — bows, clothing, shelters)
-- Tier 3: DC 14 (hard — kilns, smelting, pottery)
-- Tier 4: DC 17 (expert — bronze weapons, armor)
-- Tier 5: DC 20 (master — iron smelting, iron weapons, iron armor)
-- Tier 6: DC 23 (grandmaster — steel alloy, steel weapons/armor at Forge)
-- ============================================================
UPDATE recipes SET tier = 0 WHERE id IN (1, 2, 3, 4);
UPDATE recipes SET tier = 1 WHERE id IN (10, 11, 20, 21, 22, 23, 24, 30, 31, 32, 34, 35, 36);
UPDATE recipes SET tier = 2 WHERE id IN (33, 40, 41, 42, 43, 50, 51, 52, 53, 54, 55, 56, 60, 61, 62, 63, 64, 65, 70, 71, 72, 73);
UPDATE recipes SET tier = 3 WHERE id IN (80, 81, 82, 83, 84, 85, 86);
UPDATE recipes SET tier = 4 WHERE id IN (90, 91, 92, 93, 94, 95, 96, 97, 98, 99);
UPDATE recipes SET tier = 5 WHERE id IN (110, 111, 112, 113);
UPDATE recipes SET tier = 6 WHERE id IN (120, 121, 122, 123, 124, 125, 126);
UPDATE recipes SET tier = 7 WHERE id IN (130, 131, 133, 134);  -- Named alloys (Smelting 9+)
UPDATE recipes SET tier = 8 WHERE id IN (132);                  -- Tungsten (Smelting 10)

-- ============================================================
-- FIRE CARRYING RECIPES (Phases 1-4)
-- ============================================================
-- Tap Tree: player/NPC action, not a craft recipe — handled by HandleTapTree server-side
-- Resin Torch: Torch + Resin, no station (Herbalism 2+)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(140, 'Resin Torch',     871, 1, 0, 'Coat a torch in tree resin. Burns for hours, survives light rain.');
INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(140, 108, 1), (140, 870, 1);  -- Torch(108) + Tree Resin(870)

-- Fire Bundle: Bark + Resin x2 + Dry Grass, FireMaking 4+
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(141, 'Fire Bundle',     872, 1, 0, 'Wrap resin and tinder in bark. Carries fire for a full day.');
INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(141, 3, 4), (141, 870, 2);  -- Fiber/Grass(3) x4 + Tree Resin(870) x2

-- Fire Pot: Clay Pot + Resin x3, FireMaking 5+ and Pottery
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, station_req, description) VALUES
(142, 'Bark Vessel',     873, 1, 0, 0,   'Shape hardwood bark into a watertight vessel. Carries fire or water.');
INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(142, 879, 2), (142, 41, 1);  -- She-Oak Bark(879) x2 + Cordage(41) — shaped bark vessel

-- Unwrap Fire Bundle: consumes bundle, creates lit torch (no skill check — tier 0)
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(143, 'Unwrap Fire Bundle', 109, 1, 0, 'Tear open the bundle. The embers light a fresh torch.');
INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(143, 872, 1);  -- Fire Bundle(872) → Burning Torch(109)

UPDATE recipes SET tier = 1 WHERE id IN (140);    -- Resin Torch: DC 8 (easy, Herbalism 2+)
UPDATE recipes SET tier = 3 WHERE id IN (141);    -- Fire Bundle: DC 14 (hard, FireMaking 4+)
UPDATE recipes SET tier = 4 WHERE id IN (142);    -- Fire Pot: DC 17 (expert, FireMaking 5+ + Pottery)
UPDATE recipes SET tier = 0 WHERE id IN (143);    -- Unwrap: DC 5 (trivial, anyone can do it)

-- Botanical Discovery: Willow Extract is not crafted into products —
-- it is applied directly to saplings/crops as a growth accelerator.
-- No recipe needed. Server applies the multiplier when NPC uses extract on a planting.

-- Botanical Discovery: Eucalyptus Poultice recipe
INSERT OR IGNORE INTO recipes (id, name, output_id, output_qty, tool_req, description) VALUES
(147, 'Eucalyptus Poultice', 878, 1, 0, 'Crush leaves into a healing compress. Prevents infection.');
INSERT OR IGNORE INTO recipe_inputs (recipe_id, item_id, quantity) VALUES
(147, 877, 2), (147, 41, 1);  -- Eucalyptus Leaves x2 + Cordage(41) → Poultice

UPDATE recipes SET tier = 2 WHERE id IN (147);    -- Poultice: DC 11 (Herbalism 4+)

-- ============================================================
-- ALLOY PROPERTIES (Phase 40 — damage/defense/durability mods per alloy material)
-- ============================================================
-- Applied when gear is crafted from named alloy ingots.
-- alloy_item_id = the ingot; mods are additive to base stats.

CREATE TABLE IF NOT EXISTS alloy_properties (
    alloy_item_id   INTEGER PRIMARY KEY,
    damage_mod      INTEGER DEFAULT 0,
    defense_mod     INTEGER DEFAULT 0,
    durability_pct  INTEGER DEFAULT 0,      -- percentage bonus (+50 = 50% more durable)
    special         TEXT DEFAULT ''          -- keyword for special effects
);

INSERT OR IGNORE INTO alloy_properties VALUES
(851,  0,  2,  20, 'impact_absorb'),    -- Manganese: tough, shield bonus
(852,  0,  0,   0, 'corrosion_proof'),   -- Chromium: no weather decay
(853,  1,  0,  50, 'edge_retention'),    -- Tungsten: hardest edge
(854,  0,  1,  10, 'cold_immune'),       -- Nickel: no cold penalty
(855,  3, -1, -10, 'brittle_hard');      -- Carbon: hardest but fragile

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
(817,  4, 4, 1.0, 'hand'),
(820,  5, 6, 2.5, 'hand'),
(821,  8, 10, 1.5, 'hand'),
-- Phase 32: Steel weapons (+2 dmg over iron, spear has 3.0 range)
(830,  6, 8,  2.5, 'hand'),   -- Steel Axe
(831, 10, 12, 1.5, 'hand'),   -- Steel Sword
(833,  7, 8,  3.0, 'hand'),   -- Steel Spear (long range, formation)
-- Fire Carrying: Resin Torch (weak weapon, but you're swinging fire)
(871,  1, 3,  1.5, 'hand');   -- Resin Torch (low damage, fire damage potential)

INSERT OR IGNORE INTO combat_stats (item_id, defense_mod, slot) VALUES
(300,  1, 'body'),
(301,  2, 'body'),
(302,  0, 'body'),
(303,  1, 'feet'),
(304,  1, 'head'),
(305,  3, 'offhand'),
(815,  5, 'body'),
(816,  5, 'offhand'),
(822,  8, 'body'),
-- Phase 32: Steel armor + shields (+3 def over iron)
(832, 11, 'body'),    -- Steel Armor
(834,  5, 'offhand'), -- Iron Shield
(835,  8, 'offhand'); -- Steel Shield

-- ============================================================
-- COMBAT MODIFIERS (Phase 3)
-- Server applies these situationally during HandleAttack.
-- ============================================================

INSERT OR IGNORE INTO combat_modifiers VALUES ('high_ground',  1,  0,  0, 'Attacker uphill by >2m');
INSERT OR IGNORE INTO combat_modifiers VALUES ('in_water',    -2, -2, -3, 'Waist-deep water');
INSERT OR IGNORE INTO combat_modifiers VALUES ('night_blind', -3,  0,  0, 'No torch at night');
INSERT OR IGNORE INTO combat_modifiers VALUES ('ambush',       4,  0,  0, 'Target unaware');
INSERT OR IGNORE INTO combat_modifiers VALUES ('wounded',     -2,  0, -1, 'Below 25% HP');
INSERT OR IGNORE INTO combat_modifiers VALUES ('cornered',     2,  0,  0, 'No escape route — desperate');
INSERT OR IGNORE INTO combat_modifiers VALUES ('home_ground',  0,  2,  0, 'Defending own territory');

-- ============================================================
-- CREATURES
-- IDs must match GetCreatureId() in species headers and spawn table in TerrainNode.cpp
-- ============================================================

INSERT OR IGNORE INTO creatures VALUES
-- id  name        hp  atk def dmg var spd  detect  aggro       pack habitat
--     model, idle, run, attack, die, size, wander, sound_near, sound_far,
--     flee_speed, flee_dist, min_idle, max_idle, vision_range, vision_angle, predator, scavenger, food_grass_wt
(1,  'Rabbit',    2,  0, 12,  0, 0,  9,  10.0, 'flee',        1, 'grassland',
     'Models/Animals/Rabbit.mdl', 'Idle', 'Run', NULL, 'Die', 0.35, 12.0, 0.5, 15.0,
     7.0, 20.0, 2.0, 6.0, 15.0, 0.4, 0, 0, 0.8),
(2,  'Deer',      8,  1, 11,  2, 4,  8,  20.0, 'flee',        3, 'forest',
     'Models/Animals/Ult_Deer.mdl', 'Idle', 'Gallop', 'Attack_Headbutt', 'Death', 1.5, 30.0, 1.0, 40.0,
     8.0, 30.0, 4.0, 10.0, 30.0, 0.4, 0, 0, 0.4),
(3,  'Fox',       5,  2, 13,  2, 4,  7,  15.0, 'flee',        1, 'forest',
     'Models/Animals/Ult_Fox.mdl', 'Idle', 'Gallop', 'Attack', 'Death', 0.8, 25.0, 0.8, 30.0,
     7.0, 25.0, 3.0, 7.0, 25.0, 0.5, 1, 1, 0.0),
(4,  'Stag',     12,  3, 12,  4, 4,  7,  25.0, 'defensive',   2, 'forest',
     'Models/Animals/Ult_Stag.mdl', 'Idle', 'Gallop', 'Attack_Headbutt', 'Death', 1.8, 35.0, 1.5, 60.0,
     9.0, 35.0, 5.0, 12.0, 35.0, 0.4, 0, 0, 0.4),
(5,  'Wolf',     12,  5, 13,  5, 4,  8,  25.0, 'aggressive',  4, 'forest',
     'Models/Animals/Ult_Wolf.mdl', 'Idle', 'Gallop', 'Attack', 'Death', 1.0, 35.0, 2.0, 80.0,
     9.0, 35.0, 3.0, 8.0, 35.0, 0.5, 1, 1, 0.0),
(6,  'Bull',     20,  4, 14,  8, 6,  5,  12.0, 'territorial', 1, 'grassland',
     'Models/Animals/Ult_Bull.mdl', 'Idle', 'Gallop', 'Attack_Headbutt', 'Death', 2.0, 20.0, 2.0, 70.0,
     7.0, 15.0, 5.0, 15.0, 25.0, 0.5, 0, 0, 0.7),
(7,  'Cow',      12,  1, 10,  2, 2,  4,  10.0, 'flee',        3, 'grassland',
     'Models/Animals/Ult_Cow.mdl', 'Idle', 'Gallop', 'Attack_Kick', 'Death', 1.8, 15.0, 1.5, 60.0,
     5.0, 20.0, 6.0, 15.0, 20.0, 0.4, 0, 0, 0.7),
(8,  'Fish',      1,  0, 14,  0, 0,  8,   5.0, 'flee',        1, 'water',
     'Models/Animals/Fish.mdl', 'Swim', 'Swim', NULL, NULL, 0.2, 10.0, 0.3, 5.0,
     4.0, 10.0, 2.0, 5.0, 0.0, 0.5, 0, 0, 0.0),
(9,  'Donkey',   10,  1, 11,  3, 4,  5,  12.0, 'flee',        2, 'grassland',
     'Models/Animals/Ult_Donkey.mdl', 'Idle', 'Gallop', 'Attack_Kick', 'Death', 1.6, 20.0, 2.0, 80.0,
     6.0, 25.0, 5.0, 12.0, 25.0, 0.4, 0, 0, 0.7),
(10, 'Horse',    14,  2, 12,  4, 4,  9,  20.0, 'flee',        3, 'grassland',
     'Models/Animals/Ult_Horse.mdl', 'Idle', 'Gallop', 'Attack_Kick', 'Death', 2.2, 40.0, 2.0, 80.0,
     12.0, 45.0, 4.0, 10.0, 35.0, 0.3, 0, 0, 0.8),
(11, 'Alpaca',    8,  0, 10,  1, 2,  5,  12.0, 'flee',        4, 'mountain',
     'Models/Animals/Ult_Alpaca.mdl', 'Idle', 'Gallop', 'Attack_Kick', 'Death', 1.2, 15.0, 1.0, 40.0,
     6.0, 20.0, 4.0, 10.0, 20.0, 0.4, 0, 0, 0.7),
(12, 'Husky',     6,  3, 12,  3, 4,  8,  20.0, 'defensive',   2, 'any',
     'Models/Animals/Ult_Husky.mdl', 'Idle', 'Gallop', 'Attack', 'Death', 0.7, 20.0, 1.5, 60.0,
     8.0, 15.0, 2.0, 5.0, 25.0, 0.5, 1, 1, 0.0),
(13, 'ShibaInu',  4,  2, 11,  2, 2,  7,  15.0, 'flee',        1, 'any',
     'Models/Animals/Ult_ShibaInu.mdl', 'Idle', 'Gallop', 'Attack', 'Death', 0.5, 15.0, 1.0, 40.0,
     7.0, 15.0, 2.0, 6.0, 20.0, 0.5, 0, 0, 0.6),
(20, 'CaveMan',  15,  3, 12,  4, 4,  5,  20.0, 'defensive',   3, 'inland',
     'Models/Characters/CavemanMan.mdl', 'Caveman_idle', 'Caveman_running', 'Caveman_attack', 'Caveman_die', 1.8, 20.0, 1.5, 50.0,
     6.0, 20.0, 3.0, 8.0, 30.0, 0.5, 0, 0, 0.6),
(21, 'CaveWoman', 12,  2, 11,  3, 4,  5,  20.0, 'defensive',   3, 'inland',
     'Models/Characters/CavemanWoman.mdl', 'Caveman_idle', 'Caveman_running', 'Caveman_attack', 'Caveman_die', 1.65, 20.0, 1.5, 50.0,
     6.0, 20.0, 3.0, 8.0, 30.0, 0.5, 0, 0, 0.6);

-- ============================================================
-- LOOT TABLES
-- ============================================================

-- Rabbit (small: sinew, meat, fur, bone)
INSERT OR IGNORE INTO loot_table VALUES
(1, 20, 1, 1.0, 0),
(1, 7,  1, 1.0, 0),
(1, 24, 1, 0.8, 0),
(1, 23, 1, 0.5, 0);

-- Deer (medium herbivore: sinew, meat, hide, bone, antler, gut)
INSERT OR IGNORE INTO loot_table VALUES
(2, 20, 3, 1.0, 0),
(2, 7,  3, 1.0, 0),
(2, 21, 2, 1.0, 0),
(2, 23, 2, 0.8, 0),
(2, 27, 1, 0.7, 0),
(2, 25, 1, 0.5, 206);

-- Fox (small predator: sinew, meat, fur, bone)
INSERT OR IGNORE INTO loot_table VALUES
(3, 20, 1, 1.0, 0),
(3, 7,  1, 1.0, 0),
(3, 24, 1, 0.9, 0),
(3, 23, 1, 0.5, 0);

-- Stag (large herbivore: sinew, meat, hide, bone, antler, gut)
INSERT OR IGNORE INTO loot_table VALUES
(4, 20, 4, 1.0, 0),
(4, 7,  4, 1.0, 0),
(4, 21, 3, 1.0, 0),
(4, 23, 2, 0.8, 0),
(4, 27, 2, 0.9, 0),
(4, 25, 2, 0.6, 206);

-- Wolf (predator: sinew, meat, fur, bone, gut)
INSERT OR IGNORE INTO loot_table VALUES
(5, 20, 2, 1.0, 0),
(5, 7,  2, 1.0, 0),
(5, 24, 1, 1.0, 0),
(5, 23, 2, 0.7, 0),
(5, 25, 1, 0.6, 206);

-- Bull (large bovine: sinew, meat, hide, bone, fat, gut)
INSERT OR IGNORE INTO loot_table VALUES
(6, 20, 5, 1.0, 0),
(6, 7,  6, 1.0, 0),
(6, 21, 3, 1.0, 0),
(6, 23, 3, 0.8, 0),
(6, 26, 3, 0.9, 206),
(6, 25, 2, 0.6, 206);

-- Cow (large bovine: sinew, meat, hide, bone, fat)
INSERT OR IGNORE INTO loot_table VALUES
(7, 20, 4, 1.0, 0),
(7, 7,  5, 1.0, 0),
(7, 21, 3, 1.0, 0),
(7, 23, 2, 0.8, 0),
(7, 26, 2, 0.7, 206);

-- Fish (small: meat, bone)
INSERT OR IGNORE INTO loot_table VALUES
(8, 7,  1, 1.0, 0),
(8, 23, 1, 0.3, 0);

-- Donkey (medium: sinew, meat, hide, bone)
INSERT OR IGNORE INTO loot_table VALUES
(9, 20, 3, 1.0, 0),
(9, 7,  3, 1.0, 0),
(9, 21, 2, 1.0, 0),
(9, 23, 2, 0.7, 0);

-- Horse (large: sinew, meat, hide, bone, gut)
INSERT OR IGNORE INTO loot_table VALUES
(10, 20, 4, 1.0, 0),
(10, 7,  5, 1.0, 0),
(10, 21, 3, 1.0, 0),
(10, 23, 2, 0.8, 0),
(10, 25, 2, 0.6, 206);

-- Alpaca (medium: sinew, meat, fur/wool, bone)
INSERT OR IGNORE INTO loot_table VALUES
(11, 20, 2, 1.0, 0),
(11, 7,  2, 1.0, 0),
(11, 24, 2, 1.0, 0),
(11, 23, 1, 0.7, 0);

-- Husky (small predator: sinew, meat, fur, bone)
INSERT OR IGNORE INTO loot_table VALUES
(12, 20, 1, 1.0, 0),
(12, 7,  1, 1.0, 0),
(12, 24, 1, 0.9, 0),
(12, 23, 1, 0.5, 0);

-- ShibaInu (small: sinew, meat, fur, bone)
INSERT OR IGNORE INTO loot_table VALUES
(13, 20, 1, 1.0, 0),
(13, 7,  1, 1.0, 0),
(13, 24, 1, 0.8, 0),
(13, 23, 1, 0.4, 0);

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

-- trap_id, creature_id, difficulty, bait_item, radius, cooldown
INSERT OR IGNORE INTO trap_rules VALUES
(400, 1,  2,  0, 20.0, 60.0),   -- Simple Snare: Rabbit
(400, 3,  8,  0, 15.0, 90.0),   -- Simple Snare: Fox
(400, 13, 6,  0, 15.0, 90.0),   -- Simple Snare: ShibaInu
(401, 5, 10,  7, 40.0, 180.0),  -- Meat Trap: Wolf
(401, 12, 8,  7, 30.0, 120.0),  -- Meat Trap: Husky
(402, 2,  6,  6, 40.0, 120.0),  -- Berry Trap: Deer
(402, 7,  6,  6, 30.0, 120.0),  -- Berry Trap: Cow
(402, 11, 4,  6, 25.0, 90.0),   -- Berry Trap: Alpaca
(403, 8,  2,  0, 10.0, 60.0),   -- Fish Trap: Fish
(404, 4,  5,  6, 25.0, 120.0),  -- Deadfall: Stag
(404, 2,  4,  6, 25.0, 90.0),   -- Deadfall: Deer
(404, 6, 10,  7, 20.0, 300.0),  -- Deadfall: Bull
(405, 5,  6,  7, 20.0, 180.0),  -- Pit Trap: Wolf
(405, 10, 8,  6, 25.0, 150.0);  -- Pit Trap: Horse

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
(304, 2.0, 0.1),
(423, 10.0, 0.2),  -- Wool Tunic: better warmth than fur cloak
(424, 14.0, 0.3);  -- Wool Cloak: best warmth in game

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

-- Tool/weapon/armor durability values (uses before breaking)
UPDATE items SET durability = 50  WHERE id = 100;  -- Hand Axe
UPDATE items SET durability = 30  WHERE id = 101;  -- Scraper
UPDATE items SET durability = 40  WHERE id = 102;  -- Digging Stick
UPDATE items SET durability = 20  WHERE id = 103;  -- Bone Needle
UPDATE items SET durability = 25  WHERE id = 104;  -- Fire Kit
UPDATE items SET durability = 40  WHERE id = 105;  -- Fishing Rod
UPDATE items SET durability = 60  WHERE id = 106;  -- Stone Pick
UPDATE items SET durability = 50  WHERE id = 107;  -- Hammer Stone
UPDATE items SET durability = 40  WHERE id = 200;  -- Spear
UPDATE items SET durability = 60  WHERE id = 201;  -- Bow
UPDATE items SET durability = 30  WHERE id = 203;  -- Sling
UPDATE items SET durability = 50  WHERE id = 204;  -- Club
UPDATE items SET durability = 15  WHERE id = 205;  -- Javelin
UPDATE items SET durability = 30  WHERE id = 206;  -- Knife
UPDATE items SET durability = 30  WHERE id = 300;  -- Hide Wrap
UPDATE items SET durability = 40  WHERE id = 301;  -- Leather Tunic
UPDATE items SET durability = 40  WHERE id = 302;  -- Fur Cloak
UPDATE items SET durability = 30  WHERE id = 303;  -- Hide Boots
UPDATE items SET durability = 30  WHERE id = 304;  -- Leather Cap
UPDATE items SET durability = 60  WHERE id = 305;  -- Wood Shield

-- Metal tools/weapons/armor — tougher than stone/leather equivalents
UPDATE items SET durability = 70  WHERE id = 810;  -- Copper Axe
UPDATE items SET durability = 60  WHERE id = 811;  -- Copper Spear
UPDATE items SET durability = 100 WHERE id = 812;  -- Bronze Axe
UPDATE items SET durability = 90  WHERE id = 813;  -- Bronze Spear
UPDATE items SET durability = 80  WHERE id = 814;  -- Bronze Sword
UPDATE items SET durability = 120 WHERE id = 815;  -- Bronze Armor
UPDATE items SET durability = 100 WHERE id = 816;  -- Bronze Shield
UPDATE items SET durability = 60  WHERE id = 817;  -- Bronze Knife
UPDATE items SET durability = 150 WHERE id = 820;  -- Iron Axe
UPDATE items SET durability = 120 WHERE id = 821;  -- Iron Sword
UPDATE items SET durability = 180 WHERE id = 822;  -- Iron Armor
-- Phase 32: Steel durability (+30% over iron)
UPDATE items SET durability = 195 WHERE id = 830;  -- Steel Axe
UPDATE items SET durability = 156 WHERE id = 831;  -- Steel Sword
UPDATE items SET durability = 234 WHERE id = 832;  -- Steel Armor
UPDATE items SET durability = 156 WHERE id = 833;  -- Steel Spear
UPDATE items SET durability = 150 WHERE id = 834;  -- Iron Shield
UPDATE items SET durability = 195 WHERE id = 835;  -- Steel Shield

-- Food decay times (game days until spoiled — 0 means no decay)
UPDATE items SET decay_time = 7.0  WHERE id = 6;    -- Berries (1 week)
UPDATE items SET decay_time = 5.0  WHERE id = 7;    -- Raw Meat
UPDATE items SET decay_time = 10.0 WHERE id = 8;    -- Cooked Meat
UPDATE items SET decay_time = 30.0 WHERE id = 9;    -- Dried Meat
UPDATE items SET decay_time = 3.0  WHERE id = 10;   -- Small Fish (raw)
UPDATE items SET decay_time = 5.0  WHERE id = 13;   -- Milk
UPDATE items SET decay_time = 50.0 WHERE id = 710;  -- Wheat Grain (long shelf life)

-- ============================================================
-- ITEM MODELS — Wire existing 3D models to item definitions
-- ============================================================

-- Stone Age tools
UPDATE items SET model = 'Models/Survival/Axe.mdl'          WHERE id = 100;  -- Hand Axe (primitive)
UPDATE items SET model = 'Models/Props/Stone.mdl'            WHERE id = 101;  -- Scraper
UPDATE items SET model = 'Models/Props/Torch_Metal.mdl'      WHERE id = 108;  -- Torch (unlit)
UPDATE items SET model = 'Models/Props/Torch_Metal.mdl'      WHERE id = 109;  -- Burning Torch
UPDATE items SET model = 'Models/Props/Pickaxe_Bronze.mdl'   WHERE id = 106;  -- Stone Pick (reuse bronze pick)
UPDATE items SET model = 'Models/Props/Anvil_Log.mdl'        WHERE id = 107;  -- Hammer Stone

-- Stone Age weapons
UPDATE items SET model = 'Models/Weapons/Spear.mdl'          WHERE id = 200;  -- Spear
UPDATE items SET model = 'Models/Weapons/Bow_Wooden.mdl'     WHERE id = 201;  -- Bow
UPDATE items SET model = 'Models/Weapons/Dagger.mdl'         WHERE id = 206;  -- Knife

-- Copper Age
UPDATE items SET model = 'Models/Survival/Axe_Small.mdl'     WHERE id = 810;  -- Copper Axe
UPDATE items SET model = 'Models/Weapons/Spear.mdl'          WHERE id = 811;  -- Copper Spear

-- Bronze Age
UPDATE items SET model = 'Models/Weapons/Axe.mdl'            WHERE id = 812;  -- Bronze Axe
UPDATE items SET model = 'Models/Weapons/Spear.mdl'          WHERE id = 813;  -- Bronze Spear
UPDATE items SET model = 'Models/Weapons/Sword.mdl'          WHERE id = 814;  -- Bronze Sword
UPDATE items SET model = 'Models/Weapons/Shield_Round.mdl'   WHERE id = 816;  -- Bronze Shield
UPDATE items SET model = 'Models/Weapons/Dagger.mdl'         WHERE id = 817;  -- Bronze Knife

-- Iron Age
UPDATE items SET model = 'Models/Weapons/Axe.mdl'            WHERE id = 820;  -- Iron Axe
UPDATE items SET model = 'Models/Weapons/Sword.mdl'          WHERE id = 821;  -- Iron Sword
UPDATE items SET model = 'Models/Weapons/Shield_Round.mdl'   WHERE id = 834;  -- Iron Shield

-- Steel Age
UPDATE items SET model = 'Models/Weapons/Axe_Double.mdl'     WHERE id = 830;  -- Steel Axe (double-headed)
UPDATE items SET model = 'Models/Weapons/Sword_2.mdl'        WHERE id = 831;  -- Steel Sword (upgraded model)
UPDATE items SET model = 'Models/Weapons/Spear.mdl'          WHERE id = 833;  -- Steel Spear
UPDATE items SET model = 'Models/Weapons/Shield_Heater.mdl'  WHERE id = 835;  -- Steel Shield (heater shape)

-- Props / Resources
UPDATE items SET model = 'Models/Nature/WoodLog.mdl'         WHERE id = 11;   -- Log
UPDATE items SET model = 'Models/Props/Bag.mdl'              WHERE id = 24;   -- Fur
UPDATE items SET model = 'Models/Props/Rope_1.mdl'           WHERE id = 41;   -- Cordage
UPDATE items SET model = 'Models/Props/Bucket_Wooden_1.mdl'  WHERE id = 46;   -- Clay Jar
UPDATE items SET model = 'Models/Props/Workbench.mdl'        WHERE id = 107;  -- Hammer Stone (alt: workbench for station)
