// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

namespace Urho3D
{

/// Client->server: send VariantMap of identity and authentication data.
static const int MSG_IDENTITY = 0x87;
/// Client->server: send controls (buttons and mouse movement).
static const int MSG_CONTROLS = 0x88;
/// Client->server: scene has been loaded and client is ready to proceed.
static const int MSG_SCENELOADED = 0x89;
/// Client->server: request a package file.
static const int MSG_REQUESTPACKAGE = 0x8A;

/// Server->client: package file data fragment.
static const int MSG_PACKAGEDATA = 0x8B;
/// Server->client: load new scene. In case of empty filename the client should just empty the scene.
static const int MSG_LOADSCENE = 0x8C;
/// Server->client: wrong scene checksum, can not participate.
static const int MSG_SCENECHECKSUMERROR = 0x8D;
/// Server->client: create new node.
static const int MSG_CREATENODE = 0x8E;
/// Server->client: node delta update.
static const int MSG_NODEDELTAUPDATE = 0x8F;
/// Server->client: node latest data update.
static const int MSG_NODELATESTDATA = 0x90;
/// Server->client: remove node.
static const int MSG_REMOVENODE = 0x91;
/// Server->client: create new component.
static const int MSG_CREATECOMPONENT = 0x92;
/// Server->client: component delta update.
static const int MSG_COMPONENTDELTAUPDATE = 0x93;
/// Server->client: component latest data update.
static const int MSG_COMPONENTLATESTDATA = 0x94;
/// Server->client: remove component.
static const int MSG_REMOVECOMPONENT = 0x95;

/// Client->server and server->client: remote event.
static const int MSG_REMOTEEVENT = 0x96;
/// Client->server and server->client: remote node event.
static const int MSG_REMOTENODEEVENT = 0x97;
/// Server->client: info about package.
static const int MSG_PACKAGEINFO = 0x98;

/// Packet that includes all the above messages
static const int MSG_PACKED_MESSAGE = 0x99;

// ─── ENGINE SERVICES (0x9A–0xBF) ──────────────────────────────────────────────
// Core engine protocol: auth, terrain sync, water streaming, peer management.
// MSG_USER (0x200+) is reserved for game developers — engine never touches it.
// ──────────────────────────────────────────────────────────────────────────────

/// Client->server: send client public key for key exchange.
static const int MSG_KEY_EXCHANGE = 0x9A;
/// Server->client: send server public key for key exchange.
static const int MSG_KEY_EXCHANGE_REPLY = 0x9B;

/// AuthServer->client: introduce a peer for NAT punchthrough (GUID + token + patch coords).
static const int MSG_PEER_INTRODUCE = 0x9C;
/// Client->AuthServer: peer connection established, reporting assigned role.
static const int MSG_PEER_READY = 0x9D;
/// Client->AuthServer: NAT punchthrough to peer failed.
static const int MSG_PEER_CONNECT_FAILED = 0x9E;
/// Client->AuthServer: peer connection lost.
static const int MSG_PEER_DISCONNECTED = 0x9F;
/// Subclient->subserver: relay an inner message to AuthServer.
static const int MSG_RELAY_TO_AUTH = 0xA0;
/// Subserver->subclient: relay an AuthServer response to subclient.
static const int MSG_RELAY_FROM_AUTH = 0xA1;

/// Client->server: terrain brush edit request (optimistic — client applies first).
static const int MSG_EDIT_TERRAIN = 0xA2;
/// Client->server: object create/delete/transform edit request.
static const int MSG_EDIT_OBJECT = 0xA3;
/// Server->client: edit rejected, client should rollback.
static const int MSG_EDIT_REJECT = 0xA4;
/// Server->client(s): validated edit broadcast to replay.
static const int MSG_EDIT_BROADCAST = 0xA5;

/// Server->client: full water heightmap sent on connect.
static const int MSG_WATER_MAP = 0xB0;
/// Client->server or server->client(s): incremental water edit (brush stroke).
static const int MSG_WATER_EDIT = 0xB1;
/// Server->client: resource data for one terrain patch region.
static const int MSG_RESOURCE_PATCH = 0xB2;
/// Client->server: player's current patch position (triggers streaming).
static const int MSG_PATCH_POSITION = 0xB3;

/// Client->server: terrain sync request (version + hash for journal replay).
static const int MSG_TERRAIN_SYNC = 0xB4;
/// Server->client: journal of terrain edits to replay (incremental sync).
static const int MSG_TERRAIN_JOURNAL = 0xB5;
/// Server->client: full heightmap transfer (hash mismatch or journal gap).
static const int MSG_TERRAIN_FULLSYNC = 0xB6;

/// Client->server: eat item from inventory (item_id).
static const int MSG_EAT = 0xC0;
/// Client->server: drink from water source (source type).
static const int MSG_DRINK = 0xC1;
/// Server->client: vital stats update (hp, hunger, thirst, stamina, warmth).
static const int MSG_VITAL_UPDATE = 0xC2;

/// Client->server: pick up world item (node_id).
static const int MSG_PICKUP = 0xD0;
/// Client->server: drop item from inventory (item_id, quantity).
static const int MSG_DROP = 0xD1;
/// Server->client: full inventory snapshot on connect.
static const int MSG_INVENTORY_UPDATE = 0xD2;
/// Server->client: single item add/remove delta.
static const int MSG_INVENTORY_DELTA = 0xD3;
/// Client->server: equip item to slot (item_id, slot_name).
static const int MSG_EQUIP = 0xD4;
/// Client->server: unequip slot (slot_name).
static const int MSG_UNEQUIP = 0xD5;
/// Client->server: craft recipe (recipe_id).
static const int MSG_CRAFT = 0xD6;
/// Client->server: open storage container (building_id).
static const int MSG_OPEN_STORAGE = 0xD7;
/// Client->server: close storage container (building_id).
static const int MSG_CLOSE_STORAGE = 0xD8;
/// Client->server: transfer item between inventory and storage (item_id, qty, direction).
static const int MSG_TRANSFER = 0xD9;
/// Server->client: storage contents for a building (building_id, items[]).
static const int MSG_STORAGE_CONTENTS = 0xDA;

/// Client->server: place building (building_type_id, pos_x, pos_y, pos_z, rotation).
static const int MSG_BUILD = 0xF0;
/// Client->server: demolish building (placed_building_id).
static const int MSG_DEMOLISH = 0xF1;
/// Server->client: build result (success, placed_building_id or error string).
static const int MSG_BUILD_RESULT = 0xF2;
/// Server->broadcast: new building spawned (placed_id, type_id, pos, rotation, hp).
static const int MSG_BUILDING_SPAWN = 0xF3;
/// Server->broadcast: building removed (placed_building_id).
static const int MSG_BUILDING_REMOVE = 0xF4;

/// Used to define custom messages, usually of the form MSG_USER + x, where x is an integer value.
static const int MSG_USER = 0x200;

/// Fixed content ID for client controls update.
static const unsigned CONTROLS_CONTENT_ID = 1;
/// Package file fragment size.
static const unsigned PACKAGE_FRAGMENT_SIZE = 1024;

}
