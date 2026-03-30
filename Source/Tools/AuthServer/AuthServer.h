// AuthServer — private central authority for TerrainNode network
// NOT a public sample. Handles login, patch ownership, peer brokering.
// Runs with a debug GUI for monitoring connections and activity.

#pragma once

#include <Urho3D/Engine/Application.h>
#include <Urho3D/Network/Network.h>
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Network/Connection.h>
#include <Urho3D/Database/DbResult.h>
#include <Urho3D/Database/DbConnection.h>
#include <Urho3D/Database/Database.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/ListView.h>
#include <Urho3D/UI/Window.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/IO/VectorBuffer.h>
#include <Urho3D/IO/MemoryBuffer.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Physics/CollisionShape.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/Graphics/TerrainBrush.h>
#include <Urho3D/Resource/Image.h>
#include <Urho3D/Resource/JSONFile.h>
#include <Urho3D/Resource/JSONValue.h>
#include <Urho3D/Network/HttpRequest.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/Viewport.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/UI/MelbourneClock.h>
#include <Urho3D/Game/GameDB.h>
#include "TerrainGenerator.h"
#include "TerrainJournal.h"

using namespace Urho3D;

class AuthServer : public Application
{
    URHO3D_OBJECT(AuthServer, Application);

public:
    explicit AuthServer(Context* context);

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    // Database
    void InitDatabase();
    DbConnection* db_{};

    // Input
    void HandleKeyDown(StringHash eventType, VariantMap& eventData);

    // Network events
    void HandleClientConnected(StringHash eventType, VariantMap& eventData);
    void HandleClientDisconnected(StringHash eventType, VariantMap& eventData);
    void HandleClientIdentity(StringHash eventType, VariantMap& eventData);
    void HandleNetworkMessage(StringHash eventType, VariantMap& eventData);
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    /// PAKE: provide password hash during key exchange.
    void HandleKeyExchangeAuth(StringHash eventType, VariantMap& eventData);
    /// PAKE: first successful decrypt confirms authentication.
    void HandleClientAuthenticated(StringHash eventType, VariantMap& eventData);

    // Auth
    bool AuthenticateUser(const String& username, const String& passwordHash, int& adminLevel);
    bool RegisterUser(const String& username, const String& passwordHash);

    // Patch ownership
    struct PatchInfo
    {
        int patchX;
        int patchZ;
        String ownerName;
        String ownerAddress;
        unsigned short ownerPort;
    };
    bool ClaimPatch(int patchX, int patchZ, const String& username);
    bool AllocateRandomPatch(const String& username, int& outPatchX, int& outPatchZ);
    PatchInfo QueryPatchOwner(int patchX, int patchZ);

    // Connected clients
    struct ClientSession
    {
        String username;
        bool authenticated{false};
        int adminLevel{0};
        String guid;                           // client's NAT GUID (sent after auth)
        IntVector2 lastPatchPos{0x7FFFFFFF, 0x7FFFFFFF};  // last known patch position
        HashSet<unsigned long long> sentPatches;           // patchKey(x,z) already sent

        // Survival state (server-authoritative)
        int hp{20}, maxHp{20};
        float hunger{100.0f};
        float thirst{100.0f};
        float stamina{100.0f};
        float warmth{15.0f};
        bool alive{true};
        float speedMult{1.0f};

        // Last-sent vitals for delta detection (send-on-change)
        int sentHp{-1};
        int sentHunger{-1};
        int sentThirst{-1};
        int sentStamina{-1};
    };
    HashMap<Connection*, ClientSession> sessions_;

    // Survival system
    void InitGameDB();
    void SurvivalTick(float dt);
    void SendVitalUpdate(Connection* connection, ClientSession& session, bool force = false);
    void HandleEat(Connection* connection, MemoryBuffer& msg);
    void HandleDrink(Connection* connection, MemoryBuffer& msg);

    // Inventory system
    void HandlePickup(Connection* connection, MemoryBuffer& msg);
    void HandleDrop(Connection* connection, MemoryBuffer& msg);
    void SendInventoryUpdate(Connection* connection, int playerId);
    void SendInventoryDelta(Connection* connection, int itemId, int quantity, bool added);
    int GetPlayerId(const String& username);

#ifdef URHO3D_DATABASE_SQLITE
    SharedPtr<GameDB> gameDB_;
    HungerRules hungerRules_{};
    ThirstRules thirstRules_{};
    InventoryRules inventoryRules_{};
    bool survivalRulesLoaded_{false};
    bool inventoryRulesLoaded_{false};
    float survivalTickTimer_{0.0f};
    static constexpr float SURVIVAL_TICK_INTERVAL = 1.0f;  // check every second, drain is rate-based
    float gameTimeScale_{24.0f};  // 1 real hour = 1 game day (24 game-hours)
#endif

    // Peer brokering
    void HandleRelayToAuth(Connection* connection, MemoryBuffer& msg);
    void IntroducePeers(Connection* requester, Connection* owner, int patchX, int patchZ);
    Connection* FindSessionByUsername(const String& username);
    Connection* FindSessionByGuid(const String& guid);

    // Server-authoritative edits
    void HandleEditTerrain(Connection* connection, MemoryBuffer& msg);
    void HandleEditObject(Connection* connection, MemoryBuffer& msg);
    bool ValidateEditLocation(const String& username, int adminLevel, const Vector3& worldPos);
    void BroadcastEdit(int editMsgType, const VectorBuffer& data, const String& username, Connection* excludeConnection);

    unsigned short listenPort_{9090};

    // Authoritative scene + avatars
    SharedPtr<Scene> scene_;
    String sceneName_{"Scenes/TestScene.xml"};
    void LoadScene();
    Node* CreatePlayerAvatar(int patchX, int patchZ);
    void HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData);
    HashMap<Connection*, WeakPtr<Node>> serverObjects_;

    // GUI
    void CreateUI();
    void CreateMenuBar(BorderImage* bg, Font* font);
    void CreateNetworkingPanel(BorderImage* bg, Font* font);
    void CreateDatabasePanel(BorderImage* bg, Font* font);
    void CreateWeatherPanel(BorderImage* bg, Font* font);
    void RefreshWeatherPanel();
    void SwitchTab(int tab);
    void HandleTabClicked(StringHash eventType, VariantMap& eventData);

    void RefreshClientList();
    void LogMessage(const String& msg);

    // Menu bar
    Button* networkingTab_{};
    Button* databaseTab_{};
    Button* weatherTab_{};
    int activeTab_{0};

    // Panels
    BorderImage* networkingPanel_{};
    BorderImage* databasePanel_{};
    BorderImage* weatherPanel_{};
    Text* weatherConditionText_{};
    Text* weatherTempText_{};
    Text* weatherHumidityText_{};
    Text* weatherWindText_{};
    Text* weatherCloudText_{};
    Text* weatherPrecipText_{};
    Text* weatherFetchTimeText_{};
    Text* weatherBroadcastText_{};

    // Networking panel
    Text* statusText_{};
    Text* clientCountText_{};
    ListView* clientList_{};
    ListView* logList_{};

    // Database panel
    DropDownList* tableSelector_{};
    ListView* tableView_{};
    Text* tableSchemaText_{};
    LineEdit* sqlInput_{};
    ListView* sqlResultView_{};
    String currentTable_;
    Vector<String> currentColumns_;
    Vector<int> primaryKeyIndices_;  // which columns are PKs

    void RefreshTableList();
    void LoadTableData(const String& tableName);
    void HandleTableSelected(StringHash eventType, VariantMap& eventData);
    void HandleSqlExecute(StringHash eventType, VariantMap& eventData);
    void HandleAddRow(StringHash eventType, VariantMap& eventData);
    void HandleDeleteRow(StringHash eventType, VariantMap& eventData);
    void HandleEditRow(StringHash eventType, VariantMap& eventData);

    // Edit dialog
    SharedPtr<Window> editDialog_;
    Vector<LineEdit*> editFields_;
    unsigned editRowIndex_{};
    void CreateEditDialog(unsigned rowIndex);
    void HandleEditOK(StringHash eventType, VariantMap& eventData);
    void HandleEditCancel(StringHash eventType, VariantMap& eventData);

    float uptime_{};
    static const unsigned MAX_LOG_LINES = 200;

    // Melbourne clock display
    SharedPtr<MelbourneClock> melbourneClock_;

    // BOM Weather
    void FetchBOMWeather();
    void ProcessBOMResponse();
    void BroadcastWeather(Connection* singleClient = nullptr);
    void SendWeatherToClient(Connection* connection);

    SharedPtr<HttpRequest> bomObsRequest_;      // observations in-flight
    SharedPtr<HttpRequest> bomForecastRequest_;  // forecast in-flight
    String bomObsData_;
    String bomForecastData_;
    float weatherFetchTimer_{0.0f};
    float weatherBroadcastTimer_{0.0f};
    static constexpr float WEATHER_FETCH_INTERVAL = 600.0f;     // fetch from BOM every 10 min
    static constexpr float WEATHER_BROADCAST_INTERVAL = 10800.0f; // broadcast to clients every 3 hours
    bool weatherReady_{false};

    // Parsed weather state (broadcast to clients)
    float weatherCloudCover_{};
    float weatherPrecipitation_{};
    float weatherWindSpeed_{};
    float weatherWindAngle_{};
    float weatherTemperature_{};
    float weatherHumidity_{};
    String weatherCondition_;  // "rain", "cloudy", "mostly_sunny", etc.

    // Water heightmap (server-authoritative, same resolution as terrain)
    SharedPtr<Image> waterHeightMap_;
    void LoadOrCreateWaterMap();
    void SaveWaterMap();
    void SendWaterMapToClient(Connection* connection);
    void HandleWaterEdit(Connection* connection, MemoryBuffer& msg);

    // Per-patch resource streaming
    void SendResourcePatch(Connection* connection, const String& resourceID, Image* resourceMap, int patchX, int patchZ);
    void SendPatchNeighbourhood(Connection* connection, int centerX, int centerZ);
    void HandlePatchPosition(Connection* connection, MemoryBuffer& msg);
    void BroadcastAffectedPatch(int patchX, int patchZ, const String& resourceID, Image* resourceMap);
    static unsigned long long PatchKey(int x, int z) { return ((unsigned long long)(unsigned)x << 32) | (unsigned)z; }

    // Server-authoritative terrain brush
    SharedPtr<TerrainBrush> terrainBrush_;
    void InitTerrainBrush();

    // Terrain generation
    TerrainGenerator terrainGen_;
    HashMap<IntVector2, WeakPtr<Terrain>> terrainGrid_;  // grid coords → terrain
    unsigned worldSeed_{42};  // base seed for all terrain generation
    SharedPtr<Image> GenerateTerrainHeightmap(int gridX, int gridZ);
    void RegisterExistingTerrain();  // register the original TestScene terrain as grid (0,0)

    // Terrain edit journal (versioned sync for reconnecting clients)
    TerrainJournalManager journalManager_;
    void HandleTerrainSync(Connection* connection, MemoryBuffer& msg);
    float journalTrimTimer_{0.0f};
    static constexpr float JOURNAL_TRIM_INTERVAL = 3600.0f;  // trim journals every hour
};
