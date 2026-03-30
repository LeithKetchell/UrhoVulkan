// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "../Precompiled.h"

#include "../Game/GameDB.h"
#include "../IO/Log.h"
#include "../IO/File.h"
#include "../IO/FileSystem.h"

#ifdef URHO3D_DATABASE_SQLITE

#include <SQLite/sqlite3.h>

#include "../DebugNew.h"

namespace Urho3D
{

GameDB::GameDB(Context* context) :
    Object(context)
{
}

GameDB::~GameDB()
{
    Close();
}

bool GameDB::Open(const String& dbPath)
{
    if (db_)
        Close();

    int rc = sqlite3_open(dbPath.CString(), &db_);
    if (rc != SQLITE_OK)
    {
        URHO3D_LOGERROR("GameDB: Failed to open " + dbPath + ": " + String(sqlite3_errmsg(db_)));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode for concurrent reads
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    URHO3D_LOGINFO("GameDB: Opened " + dbPath);
    return true;
}

void GameDB::Close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
        URHO3D_LOGINFO("GameDB: Closed");
    }
}

bool GameDB::ExecuteFile(const String& sqlPath)
{
    if (!db_)
    {
        URHO3D_LOGERROR("GameDB: Database not open");
        return false;
    }

    // Read file contents
    auto* fileSystem = GetSubsystem<FileSystem>();
    if (!fileSystem || !fileSystem->FileExists(sqlPath))
    {
        URHO3D_LOGERROR("GameDB: SQL file not found: " + sqlPath);
        return false;
    }

    File file(context_, sqlPath, FILE_READ);
    if (!file.IsOpen())
    {
        URHO3D_LOGERROR("GameDB: Could not open SQL file: " + sqlPath);
        return false;
    }

    unsigned size = file.GetSize();
    Vector<char> buffer(size + 1);
    file.Read(&buffer[0], size);
    buffer[size] = '\0';

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, &buffer[0], nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        String error = errMsg ? String(errMsg) : "unknown error";
        sqlite3_free(errMsg);
        URHO3D_LOGERROR("GameDB: Failed to execute " + sqlPath + ": " + error);
        return false;
    }

    URHO3D_LOGINFO("GameDB: Executed " + sqlPath);
    return true;
}

bool GameDB::Execute(const String& sql)
{
    if (!db_)
        return false;

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.CString(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        String error = errMsg ? String(errMsg) : "unknown error";
        sqlite3_free(errMsg);
        URHO3D_LOGERROR("GameDB: SQL error: " + error);
        return false;
    }
    return true;
}

// --- Item queries ---

bool GameDB::GetItem(int itemId, ItemInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, category, stack_max, weight, durability, decay_time, "
        "model, icon, description, tier FROM items WHERE id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, itemId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.id = sqlite3_column_int(stmt, 0);
        out.name = (const char*)sqlite3_column_text(stmt, 1);
        out.category = (const char*)sqlite3_column_text(stmt, 2);
        out.stackMax = sqlite3_column_int(stmt, 3);
        out.weight = (float)sqlite3_column_double(stmt, 4);
        out.durability = sqlite3_column_int(stmt, 5);
        out.decayTime = (float)sqlite3_column_double(stmt, 6);
        const char* model = (const char*)sqlite3_column_text(stmt, 7);
        out.model = model ? model : "";
        const char* icon = (const char*)sqlite3_column_text(stmt, 8);
        out.icon = icon ? icon : "";
        const char* desc = (const char*)sqlite3_column_text(stmt, 9);
        out.description = desc ? desc : "";
        out.tier = sqlite3_column_int(stmt, 10);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

Vector<ItemInfo> GameDB::GetItemsByCategory(const String& category)
{
    Vector<ItemInfo> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, category, stack_max, weight, durability, decay_time, "
        "model, icon, description, tier FROM items WHERE category = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_text(stmt, 1, category.CString(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ItemInfo info;
        info.id = sqlite3_column_int(stmt, 0);
        info.name = (const char*)sqlite3_column_text(stmt, 1);
        info.category = (const char*)sqlite3_column_text(stmt, 2);
        info.stackMax = sqlite3_column_int(stmt, 3);
        info.weight = (float)sqlite3_column_double(stmt, 4);
        info.durability = sqlite3_column_int(stmt, 5);
        info.decayTime = (float)sqlite3_column_double(stmt, 6);
        const char* model = (const char*)sqlite3_column_text(stmt, 7);
        info.model = model ? model : "";
        const char* icon = (const char*)sqlite3_column_text(stmt, 8);
        info.icon = icon ? icon : "";
        const char* desc = (const char*)sqlite3_column_text(stmt, 9);
        info.description = desc ? desc : "";
        info.tier = sqlite3_column_int(stmt, 10);
        result.Push(info);
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- Creature queries ---

static void ReadCreatureRow(sqlite3_stmt* stmt, CreatureInfo& out)
{
    out.id = sqlite3_column_int(stmt, 0);
    out.name = (const char*)sqlite3_column_text(stmt, 1);
    out.hp = sqlite3_column_int(stmt, 2);
    out.attack = sqlite3_column_int(stmt, 3);
    out.defense = sqlite3_column_int(stmt, 4);
    out.damage = sqlite3_column_int(stmt, 5);
    out.damageVar = sqlite3_column_int(stmt, 6);
    out.speed = sqlite3_column_int(stmt, 7);
    out.detectionRange = (float)sqlite3_column_double(stmt, 8);
    out.aggression = (const char*)sqlite3_column_text(stmt, 9);
    out.packSize = sqlite3_column_int(stmt, 10);
    out.habitat = (const char*)sqlite3_column_text(stmt, 11);
    const char* model = (const char*)sqlite3_column_text(stmt, 12);
    out.model = model ? model : "";
    const char* idle = (const char*)sqlite3_column_text(stmt, 13);
    out.idleAnim = idle ? idle : "";
    const char* run = (const char*)sqlite3_column_text(stmt, 14);
    out.runAnim = run ? run : "";
    const char* atk = (const char*)sqlite3_column_text(stmt, 15);
    out.attackAnim = atk ? atk : "";
    const char* die = (const char*)sqlite3_column_text(stmt, 16);
    out.dieAnim = die ? die : "";
    out.desiredSize = (float)sqlite3_column_double(stmt, 17);
    out.wanderRadius = (float)sqlite3_column_double(stmt, 18);
}

bool GameDB::GetCreature(int creatureId, CreatureInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT * FROM creatures WHERE id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, creatureId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ReadCreatureRow(stmt, out);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

Vector<CreatureInfo> GameDB::GetCreaturesByHabitat(const String& habitat)
{
    Vector<CreatureInfo> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT * FROM creatures WHERE habitat = ? OR habitat = 'any'", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_text(stmt, 1, habitat.CString(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        CreatureInfo info;
        ReadCreatureRow(stmt, info);
        result.Push(info);
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- Loot queries ---

Vector<LootDrop> GameDB::GetLoot(int creatureId)
{
    Vector<LootDrop> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT lt.item_id, i.name, lt.quantity, lt.chance, lt.tool_req "
        "FROM loot_table lt JOIN items i ON i.id = lt.item_id "
        "WHERE lt.creature_id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, creatureId);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        LootDrop drop;
        drop.itemId = sqlite3_column_int(stmt, 0);
        drop.itemName = (const char*)sqlite3_column_text(stmt, 1);
        drop.quantity = sqlite3_column_int(stmt, 2);
        drop.chance = (float)sqlite3_column_double(stmt, 3);
        drop.toolReq = sqlite3_column_int(stmt, 4);
        result.Push(drop);
    }
    sqlite3_finalize(stmt);
    return result;
}

// --- Recipe queries ---

static void ReadRecipeRow(sqlite3_stmt* stmt, RecipeInfo& out)
{
    out.id = sqlite3_column_int(stmt, 0);
    out.name = (const char*)sqlite3_column_text(stmt, 1);
    const char* desc = (const char*)sqlite3_column_text(stmt, 2);
    out.description = desc ? desc : "";
    out.outputId = sqlite3_column_int(stmt, 3);
    out.outputQty = sqlite3_column_int(stmt, 4);
    out.craftTime = (float)sqlite3_column_double(stmt, 5);
    out.toolReq = sqlite3_column_int(stmt, 6);
    out.stationReq = sqlite3_column_int(stmt, 7);
    out.tier = sqlite3_column_int(stmt, 8);
}

bool GameDB::GetRecipe(int recipeId, RecipeInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, description, output_id, output_qty, craft_time, tool_req, station_req, tier "
        "FROM recipes WHERE id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, recipeId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ReadRecipeRow(stmt, out);
        found = true;
    }
    sqlite3_finalize(stmt);

    if (!found) return false;

    // Populate inputs
    rc = sqlite3_prepare_v2(db_,
        "SELECT item_id, quantity, consumed FROM recipe_inputs WHERE recipe_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return true; // recipe found, inputs failed — still return true

    sqlite3_bind_int(stmt, 1, recipeId);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        RecipeInfo::Input input;
        input.itemId = sqlite3_column_int(stmt, 0);
        input.quantity = sqlite3_column_int(stmt, 1);
        input.consumed = sqlite3_column_int(stmt, 2) != 0;
        out.inputs.Push(input);
    }
    sqlite3_finalize(stmt);
    return true;
}

Vector<RecipeInfo> GameDB::GetRecipesForTier(int maxTier)
{
    Vector<RecipeInfo> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, description, output_id, output_qty, craft_time, tool_req, station_req, tier "
        "FROM recipes WHERE tier <= ? ORDER BY tier, id", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, maxTier);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        RecipeInfo info;
        ReadRecipeRow(stmt, info);
        result.Push(info);
    }
    sqlite3_finalize(stmt);

    // Populate inputs for each recipe
    for (unsigned i = 0; i < result.Size(); ++i)
    {
        rc = sqlite3_prepare_v2(db_,
            "SELECT item_id, quantity, consumed FROM recipe_inputs WHERE recipe_id = ?",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, result[i].id);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            RecipeInfo::Input input;
            input.itemId = sqlite3_column_int(stmt, 0);
            input.quantity = sqlite3_column_int(stmt, 1);
            input.consumed = sqlite3_column_int(stmt, 2) != 0;
            result[i].inputs.Push(input);
        }
        sqlite3_finalize(stmt);
    }

    return result;
}

bool GameDB::CanCraft(int recipeId, const HashMap<int, int>& inventory)
{
    if (!db_) return false;

    // Get recipe inputs
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT item_id, quantity, consumed FROM recipe_inputs WHERE recipe_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, recipeId);
    bool canCraft = true;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int itemId = sqlite3_column_int(stmt, 0);
        int qty = sqlite3_column_int(stmt, 1);
        bool consumed = sqlite3_column_int(stmt, 2) != 0;

        if (consumed)
        {
            auto it = inventory.Find(itemId);
            if (it == inventory.End() || it->second_ < qty)
            {
                canCraft = false;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);

    if (!canCraft) return false;

    // Check tool requirement
    rc = sqlite3_prepare_v2(db_,
        "SELECT tool_req FROM recipes WHERE id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, recipeId);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int toolReq = sqlite3_column_int(stmt, 0);
        if (toolReq != 0)
        {
            auto it = inventory.Find(toolReq);
            if (it == inventory.End() || it->second_ < 1)
                canCraft = false;
        }
    }
    sqlite3_finalize(stmt);

    return canCraft;
}

// --- Combat queries ---

bool GameDB::GetCombatStats(int itemId, CombatInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT attack_mod, defense_mod, damage, damage_var, range, speed_mod, slot, two_handed "
        "FROM combat_stats WHERE item_id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, itemId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.attackMod = sqlite3_column_int(stmt, 0);
        out.defenseMod = sqlite3_column_int(stmt, 1);
        out.damage = sqlite3_column_int(stmt, 2);
        out.damageVar = sqlite3_column_int(stmt, 3);
        out.range = (float)sqlite3_column_double(stmt, 4);
        out.speedMod = sqlite3_column_int(stmt, 5);
        out.slot = (const char*)sqlite3_column_text(stmt, 6);
        out.twoHanded = sqlite3_column_int(stmt, 7) != 0;
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// --- Survival queries ---

bool GameDB::GetFoodProperties(int itemId, FoodInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT hunger, health, warmth, spoil_time, cooked, poison_chance "
        "FROM food_properties WHERE item_id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, itemId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.hunger = sqlite3_column_int(stmt, 0);
        out.health = sqlite3_column_int(stmt, 1);
        out.warmth = (float)sqlite3_column_double(stmt, 2);
        out.spoilTime = (float)sqlite3_column_double(stmt, 3);
        out.cooked = sqlite3_column_int(stmt, 4) != 0;
        out.poisonChance = (float)sqlite3_column_double(stmt, 5);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetClimateRules(const String& season, const String& timeOfDay, ClimateInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT base_temp, wind_chill, rain_chill FROM climate_rules "
        "WHERE season = ? AND time_of_day = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, season.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, timeOfDay.CString(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.baseTemp = (float)sqlite3_column_double(stmt, 0);
        out.windChill = (float)sqlite3_column_double(stmt, 1);
        out.rainChill = (float)sqlite3_column_double(stmt, 2);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// --- Survival rule queries (singleton rows) ---

bool GameDB::GetHungerRules(HungerRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT max_hunger, drain_per_day, sprint_mult, work_mult, swim_mult, "
        "starve_hp_day, low_threshold, critical_threshold FROM hunger_rules LIMIT 1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.maxHunger = sqlite3_column_int(stmt, 0);
        out.drainPerDay = (float)sqlite3_column_double(stmt, 1);
        out.sprintMult = (float)sqlite3_column_double(stmt, 2);
        out.workMult = (float)sqlite3_column_double(stmt, 3);
        out.swimMult = (float)sqlite3_column_double(stmt, 4);
        out.starveHpDay = (float)sqlite3_column_double(stmt, 5);
        out.lowThreshold = sqlite3_column_int(stmt, 6);
        out.criticalThreshold = sqlite3_column_int(stmt, 7);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetThirstRules(ThirstRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT max_thirst, drain_per_day, heat_mult, sprint_mult, work_mult, "
        "dehydrate_hp_day, low_threshold, critical_threshold FROM thirst_rules LIMIT 1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.maxThirst = sqlite3_column_int(stmt, 0);
        out.drainPerDay = (float)sqlite3_column_double(stmt, 1);
        out.heatMult = (float)sqlite3_column_double(stmt, 2);
        out.sprintMult = (float)sqlite3_column_double(stmt, 3);
        out.workMult = (float)sqlite3_column_double(stmt, 4);
        out.dehydrateHpDay = (float)sqlite3_column_double(stmt, 5);
        out.lowThreshold = sqlite3_column_int(stmt, 6);
        out.criticalThreshold = sqlite3_column_int(stmt, 7);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetWarmthRules(WarmthRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT comfort_min, cold_threshold, severe_cold, heat_threshold, "
        "cold_hp_per_day, severe_hp_per_day, heat_hp_per_day, "
        "fire_warmth, fire_range, activity_warmth, sprint_heat "
        "FROM warmth_rules LIMIT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.comfortMin = (float)sqlite3_column_double(stmt, 0);
        out.coldThreshold = (float)sqlite3_column_double(stmt, 1);
        out.severeCold = (float)sqlite3_column_double(stmt, 2);
        out.heatThreshold = (float)sqlite3_column_double(stmt, 3);
        out.coldHpPerDay = (float)sqlite3_column_double(stmt, 4);
        out.severeHpPerDay = (float)sqlite3_column_double(stmt, 5);
        out.heatHpPerDay = (float)sqlite3_column_double(stmt, 6);
        out.fireWarmth = (float)sqlite3_column_double(stmt, 7);
        out.fireRange = (float)sqlite3_column_double(stmt, 8);
        out.activityWarmth = (float)sqlite3_column_double(stmt, 9);
        out.sprintHeat = (float)sqlite3_column_double(stmt, 10);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetStaminaRules(StaminaRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT max_stamina, regen_per_second, regen_sleeping, sprint_cost_sec, "
        "melee_cost, ranged_cost, chop_cost, mine_cost, build_cost, swim_cost_sec, "
        "low_threshold FROM stamina_rules LIMIT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.maxStamina = sqlite3_column_int(stmt, 0);
        out.regenPerSecond = (float)sqlite3_column_double(stmt, 1);
        out.regenSleeping = (float)sqlite3_column_double(stmt, 2);
        out.sprintCostSec = (float)sqlite3_column_double(stmt, 3);
        out.meleeCost = (float)sqlite3_column_double(stmt, 4);
        out.rangedCost = (float)sqlite3_column_double(stmt, 5);
        out.chopCost = (float)sqlite3_column_double(stmt, 6);
        out.mineCost = (float)sqlite3_column_double(stmt, 7);
        out.buildCost = (float)sqlite3_column_double(stmt, 8);
        out.swimCostSec = (float)sqlite3_column_double(stmt, 9);
        out.lowThreshold = sqlite3_column_int(stmt, 10);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetDeathRules(DeathRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT respawn_delay, hp_on_respawn, hunger_on_respawn, thirst_on_respawn, "
        "stamina_on_respawn, drop_inventory, corpse_duration, skill_loss "
        "FROM death_rules LIMIT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.respawnDelay = (float)sqlite3_column_double(stmt, 0);
        out.hpOnRespawn = sqlite3_column_int(stmt, 1);
        out.hungerOnRespawn = sqlite3_column_int(stmt, 2);
        out.thirstOnRespawn = sqlite3_column_int(stmt, 3);
        out.staminaOnRespawn = sqlite3_column_int(stmt, 4);
        out.dropInventory = sqlite3_column_int(stmt, 5) != 0;
        out.corpseDuration = (float)sqlite3_column_double(stmt, 6);
        out.skillLoss = (float)sqlite3_column_double(stmt, 7);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool GameDB::GetWaterSource(const String& type, WaterSourceInfo& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, type, thirst_restore, interact_time, disease_chance, requires "
        "FROM water_sources WHERE type = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, type.CString(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.id = sqlite3_column_int(stmt, 0);
        out.name = (const char*)sqlite3_column_text(stmt, 1);
        out.type = (const char*)sqlite3_column_text(stmt, 2);
        out.thirstRestore = sqlite3_column_int(stmt, 3);
        out.interactTime = (float)sqlite3_column_double(stmt, 4);
        out.diseaseChance = (float)sqlite3_column_double(stmt, 5);
        out.requires = sqlite3_column_int(stmt, 6);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

Vector<WaterSourceInfo> GameDB::GetAllWaterSources()
{
    Vector<WaterSourceInfo> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, name, type, thirst_restore, interact_time, disease_chance, requires "
        "FROM water_sources", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        WaterSourceInfo info;
        info.id = sqlite3_column_int(stmt, 0);
        info.name = (const char*)sqlite3_column_text(stmt, 1);
        info.type = (const char*)sqlite3_column_text(stmt, 2);
        info.thirstRestore = sqlite3_column_int(stmt, 3);
        info.interactTime = (float)sqlite3_column_double(stmt, 4);
        info.diseaseChance = (float)sqlite3_column_double(stmt, 5);
        info.requires = sqlite3_column_int(stmt, 6);
        result.Push(info);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool GameDB::GetClothingWarmth(int itemId, float& warmth, float& rainResist)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT warmth, rain_resist FROM clothing_warmth WHERE item_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, itemId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        warmth = (float)sqlite3_column_double(stmt, 0);
        rainResist = (float)sqlite3_column_double(stmt, 1);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// ============================================================================
// Inventory queries
// ============================================================================

bool GameDB::GetInventoryRules(InventoryRules& out)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT base_slots, max_weight, heavy_weight, max_weight_absolute, "
        "encumbered_speed, heavy_speed, heavy_stamina_mult FROM inventory_rules WHERE id = 1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.baseSlots = sqlite3_column_int(stmt, 0);
        out.maxWeight = (float)sqlite3_column_double(stmt, 1);
        out.heavyWeight = (float)sqlite3_column_double(stmt, 2);
        out.maxWeightAbsolute = (float)sqlite3_column_double(stmt, 3);
        out.encumberedSpeed = (float)sqlite3_column_double(stmt, 4);
        out.heavySpeed = (float)sqlite3_column_double(stmt, 5);
        out.heavyStaminaMult = (float)sqlite3_column_double(stmt, 6);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

Vector<InventorySlot> GameDB::GetPlayerInventory(int playerId)
{
    Vector<InventorySlot> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT item_id, quantity, durability, slot FROM player_inventory WHERE player_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, playerId);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        InventorySlot s;
        s.itemId = sqlite3_column_int(stmt, 0);
        s.quantity = sqlite3_column_int(stmt, 1);
        s.durability = sqlite3_column_int(stmt, 2);
        const char* slot = (const char*)sqlite3_column_text(stmt, 3);
        s.slotType = slot ? slot : "bag";
        result.Push(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

float GameDB::GetPlayerWeight(int playerId)
{
    if (!db_) return 0.0f;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT COALESCE(SUM(pi.quantity * i.weight), 0) "
        "FROM player_inventory pi JOIN items i ON i.id = pi.item_id "
        "WHERE pi.player_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0.0f;

    sqlite3_bind_int(stmt, 1, playerId);
    float weight = 0.0f;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        weight = (float)sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return weight;
}

int GameDB::GetItemCount(int playerId, int itemId)
{
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT COALESCE(SUM(quantity), 0) FROM player_inventory "
        "WHERE player_id = ? AND item_id = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int(stmt, 1, playerId);
    sqlite3_bind_int(stmt, 2, itemId);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool GameDB::AddItemToInventory(int playerId, int itemId, int qty)
{
    if (!db_ || qty <= 0) return false;

    // Get item info for stack_max and weight check
    ItemInfo item;
    if (!GetItem(itemId, item))
        return false;

    // Weight check
    InventoryRules rules;
    if (!GetInventoryRules(rules))
        return false;

    float currentWeight = GetPlayerWeight(playerId);
    float addedWeight = item.weight * qty;
    if (currentWeight + addedWeight > rules.maxWeightAbsolute)
        return false;

    // Try to stack into existing bag slot first
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT quantity FROM player_inventory "
        "WHERE player_id = ? AND item_id = ? AND slot = 'bag'",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, playerId);
    sqlite3_bind_int(stmt, 2, itemId);

    bool existingSlot = false;
    int existingQty = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        existingQty = sqlite3_column_int(stmt, 0);
        existingSlot = true;
    }
    sqlite3_finalize(stmt);

    if (existingSlot && existingQty + qty <= item.stackMax)
    {
        // Update existing stack
        rc = sqlite3_prepare_v2(db_,
            "UPDATE player_inventory SET quantity = quantity + ? "
            "WHERE player_id = ? AND item_id = ? AND slot = 'bag'",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, qty);
        sqlite3_bind_int(stmt, 2, playerId);
        sqlite3_bind_int(stmt, 3, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }

    if (!existingSlot)
    {
        // Check available slots
        if (GetAvailableSlots(playerId) <= 0)
            return false;

        // Insert new slot
        rc = sqlite3_prepare_v2(db_,
            "INSERT INTO player_inventory (player_id, item_id, quantity, durability, slot) "
            "VALUES (?, ?, ?, ?, 'bag')",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, playerId);
        sqlite3_bind_int(stmt, 2, itemId);
        sqlite3_bind_int(stmt, 3, qty);
        sqlite3_bind_int(stmt, 4, item.durability > 0 ? item.durability : -1);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }

    // Existing slot but would exceed stack_max — not enough room
    return false;
}

bool GameDB::RemoveItemFromInventory(int playerId, int itemId, int qty)
{
    if (!db_ || qty <= 0) return false;

    // Check current count
    int have = GetItemCount(playerId, itemId);
    if (have < qty)
        return false;

    // Decrement from bag slot first
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT quantity FROM player_inventory "
        "WHERE player_id = ? AND item_id = ? AND slot = 'bag'",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, playerId);
    sqlite3_bind_int(stmt, 2, itemId);

    int bagQty = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        bagQty = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (bagQty <= qty)
    {
        // Remove the entire row
        rc = sqlite3_prepare_v2(db_,
            "DELETE FROM player_inventory WHERE player_id = ? AND item_id = ? AND slot = 'bag'",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, playerId);
        sqlite3_bind_int(stmt, 2, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    else
    {
        // Decrement
        rc = sqlite3_prepare_v2(db_,
            "UPDATE player_inventory SET quantity = quantity - ? "
            "WHERE player_id = ? AND item_id = ? AND slot = 'bag'",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, qty);
        sqlite3_bind_int(stmt, 2, playerId);
        sqlite3_bind_int(stmt, 3, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return true;
}

int GameDB::GetAvailableSlots(int playerId)
{
    if (!db_) return 0;

    InventoryRules rules;
    if (!GetInventoryRules(rules))
        return 0;

    int maxSlots = rules.baseSlots;

    // Check for container bonus from equipped back slot
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT cb.extra_slots FROM player_inventory pi "
        "JOIN container_bonus cb ON cb.item_id = pi.item_id "
        "WHERE pi.player_id = ? AND (pi.slot = 'back' OR pi.slot = 'hand')",
        -1, &stmt, nullptr);
    if (rc == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, playerId);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            maxSlots += sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // Count occupied bag slots
    rc = sqlite3_prepare_v2(db_,
        "SELECT COUNT(*) FROM player_inventory WHERE player_id = ? AND slot = 'bag'",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int(stmt, 1, playerId);
    int used = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        used = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return maxSlots - used;
}

}

#endif // URHO3D_DATABASE_SQLITE
