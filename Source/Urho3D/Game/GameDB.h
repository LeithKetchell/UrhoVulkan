// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include "../Core/Object.h"
#include "../Container/Str.h"
#include "../Container/Vector.h"
#include "../Container/HashMap.h"

#ifdef URHO3D_DATABASE_SQLITE

struct sqlite3;

namespace Urho3D
{

/// Result row from a creature query.
struct CreatureInfo
{
    int id;
    String name;
    int hp, attack, defense, damage, damageVar, speed;
    float detectionRange;
    String aggression, habitat;
    String model, idleAnim, runAnim, attackAnim, dieAnim;
    float desiredSize, wanderRadius;
    int packSize;
};

/// Result row from a loot query.
struct LootDrop
{
    int itemId;
    String itemName;
    int quantity;
    float chance;
    int toolReq;
};

/// Result row from a recipe query.
struct RecipeInfo
{
    int id;
    String name;
    int outputId, outputQty;
    float craftTime;
    int toolReq, stationReq, tier;
    /// Single recipe input.
    struct Input
    {
        int itemId;
        int quantity;
        bool consumed;
    };
    Vector<Input> inputs;
};

/// Result row from a combat stats query.
struct CombatInfo
{
    int attackMod, defenseMod, damage, damageVar;
    float range;
    int speedMod;
    String slot;
    bool twoHanded;
};

/// Result row from a food query.
struct FoodInfo
{
    int hunger, health;
    float warmth, spoilTime, poisonChance;
    bool cooked;
};

/// Result row from a climate query.
struct ClimateInfo
{
    float baseTemp, windChill, rainChill;
};

/// Hunger rules from database.
struct HungerRules
{
    int maxHunger;
    float drainPerDay, sprintMult, workMult, swimMult;
    float starveHpDay;
    int lowThreshold, criticalThreshold;
};

/// Thirst rules from database.
struct ThirstRules
{
    int maxThirst;
    float drainPerDay, heatMult, sprintMult, workMult;
    float dehydrateHpDay;
    int lowThreshold, criticalThreshold;
};

/// Warmth rules from database.
struct WarmthRules
{
    float comfortMin, coldThreshold, severeCold, heatThreshold;
    float coldHpPerDay, severeHpPerDay, heatHpPerDay;
    float fireWarmth, fireRange, activityWarmth, sprintHeat;
};

/// Stamina rules from database.
struct StaminaRules
{
    int maxStamina;
    float regenPerSecond, regenSleeping;
    float sprintCostSec, meleeCost, rangedCost;
    float chopCost, mineCost, buildCost, swimCostSec;
    int lowThreshold;
};

/// Water source info from database.
struct WaterSourceInfo
{
    int id;
    String name, type;
    int thirstRestore;
    float interactTime, diseaseChance;
    int requires;
};

/// Death/respawn rules from database.
struct DeathRules
{
    float respawnDelay;
    int hpOnRespawn, hungerOnRespawn, thirstOnRespawn, staminaOnRespawn;
    bool dropInventory;
    float corpseDuration, skillLoss;
};

/// Result row from an item query.
struct ItemInfo
{
    int id;
    String name, category;
    int stackMax;
    float weight;
    int durability;
    float decayTime;
    String model, icon, description;
    int tier;
};

/// Inventory rules from database.
struct InventoryRules
{
    int baseSlots;
    float maxWeight, heavyWeight, maxWeightAbsolute;
    float encumberedSpeed, heavySpeed, heavyStaminaMult;
};

/// A single inventory slot (bag or equipment).
struct InventorySlot
{
    int itemId;
    int quantity;
    int durability;  // -1 = use item default
    String slotType; // "bag", "hand", "body", etc.
};

/// SQL-driven game rules database. Server-side subsystem.
/// The game lives in the database. The engine is a dumb executor.
class URHO3D_API GameDB : public Object
{
    URHO3D_OBJECT(GameDB, Object);

public:
    explicit GameDB(Context* context);
    ~GameDB() override;

    /// Open the database file. Returns true on success.
    bool Open(const String& dbPath);
    /// Close the database.
    void Close();
    /// Execute a SQL file (schema, seed data, or balance patch). Supports multiple statements.
    bool ExecuteFile(const String& sqlPath);
    /// Execute a single SQL statement. For quick queries/updates.
    bool Execute(const String& sql);

    /// Return true if database is open.
    bool IsOpen() const { return db_ != nullptr; }

    // --- Item queries ---

    /// Get item info by ID.
    bool GetItem(int itemId, ItemInfo& out);
    /// Get all items in a category.
    Vector<ItemInfo> GetItemsByCategory(const String& category);

    // --- Creature queries ---

    /// Get creature info by ID.
    bool GetCreature(int creatureId, CreatureInfo& out);
    /// Get all creatures for a habitat.
    Vector<CreatureInfo> GetCreaturesByHabitat(const String& habitat);

    // --- Loot queries ---

    /// Get loot drops for a creature. Caller rolls chance.
    Vector<LootDrop> GetLoot(int creatureId);

    // --- Recipe queries ---

    /// Get recipe by ID, with inputs populated.
    bool GetRecipe(int recipeId, RecipeInfo& out);
    /// Get all recipes available at a given tech tier or below.
    Vector<RecipeInfo> GetRecipesForTier(int maxTier);
    /// Check if player has ingredients (takes inventory item_id -> quantity map).
    bool CanCraft(int recipeId, const HashMap<int, int>& inventory);

    // --- Combat queries ---

    /// Get combat stats for an item.
    bool GetCombatStats(int itemId, CombatInfo& out);

    // --- Survival queries ---

    /// Get food properties for an item.
    bool GetFoodProperties(int itemId, FoodInfo& out);
    /// Get climate rules for a season/time combination.
    bool GetClimateRules(const String& season, const String& timeOfDay, ClimateInfo& out);

    /// Get hunger rules (singleton row).
    bool GetHungerRules(HungerRules& out);
    /// Get thirst rules (singleton row).
    bool GetThirstRules(ThirstRules& out);
    /// Get warmth rules (singleton row).
    bool GetWarmthRules(WarmthRules& out);
    /// Get stamina rules (singleton row).
    bool GetStaminaRules(StaminaRules& out);
    /// Get death/respawn rules (singleton row).
    bool GetDeathRules(DeathRules& out);
    /// Get water source by type.
    bool GetWaterSource(const String& type, WaterSourceInfo& out);
    /// Get all water sources.
    Vector<WaterSourceInfo> GetAllWaterSources();
    /// Get clothing warmth for an item.
    bool GetClothingWarmth(int itemId, float& warmth, float& rainResist);

    /// Apply a SQL balance patch file.
    bool ApplyPatch(const String& patchPath) { return ExecuteFile(patchPath); }

    // --- Inventory queries ---

    /// Get inventory rules (singleton row).
    bool GetInventoryRules(InventoryRules& out);
    /// Get all inventory slots for a player.
    Vector<InventorySlot> GetPlayerInventory(int playerId);
    /// Get total carried weight for a player.
    float GetPlayerWeight(int playerId);
    /// Count how many of an item the player has (across all slots).
    int GetItemCount(int playerId, int itemId);
    /// Add item to player inventory. Handles stacking. Returns false if over weight/slots.
    bool AddItemToInventory(int playerId, int itemId, int qty);
    /// Remove item from player inventory. Returns false if insufficient.
    bool RemoveItemFromInventory(int playerId, int itemId, int qty);
    /// Get available bag slot count for a player.
    int GetAvailableSlots(int playerId);

private:
    sqlite3* db_{nullptr};
};

}

#endif // URHO3D_DATABASE_SQLITE
