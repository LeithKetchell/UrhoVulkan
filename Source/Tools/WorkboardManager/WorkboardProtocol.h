// WorkboardProtocol.h — Shared event types and data structures for
// WorkboardManager ↔ WorkboardClient reliable UDP protocol.

#pragma once

#include <Urho3D/Core/Object.h>
#include <Urho3D/Network/Connection.h>

using namespace Urho3D;

// ── Server → Client remote events ──

/// Post-auth welcome. Payload: serverName, version, clientCount.
URHO3D_EVENT(E_WB_WELCOME, WbWelcome)
{
    URHO3D_PARAM(P_SERVERNAME, ServerName);   // String
    URHO3D_PARAM(P_VERSION, Version);         // String
    URHO3D_PARAM(P_CLIENTCOUNT, ClientCount); // int
}

/// Full workboard markdown pushed on connect and on change.
URHO3D_EVENT(E_WB_WORKBOARD_FULL, WbWorkboardFull)
{
    URHO3D_PARAM(P_MARKDOWN, Markdown);       // String
}

/// Plan file listing.
URHO3D_EVENT(E_WB_PLAN_LIST, WbPlanList)
{
    URHO3D_PARAM(P_FILENAMES, Filenames);     // String (newline-separated)
}

/// Single plan file content (response to client request).
URHO3D_EVENT(E_WB_PLAN_CONTENT, WbPlanContent)
{
    URHO3D_PARAM(P_FILENAME, Filename);       // String
    URHO3D_PARAM(P_CONTENT, Content);         // String
}

/// Mutation acknowledgement.
URHO3D_EVENT(E_WB_MUTATION_ACK, WbMutationAck)
{
    URHO3D_PARAM(P_SUCCESS, Success);         // bool
    URHO3D_PARAM(P_REASON, Reason);           // String
}

/// Connected remote client list.
URHO3D_EVENT(E_WB_CLIENT_LIST, WbClientList)
{
    URHO3D_PARAM(P_CLIENTS, Clients);         // String (newline-separated "name:role")
}

// ── Client → Server remote events ──

/// Client requests a specific plan file.
URHO3D_EVENT(E_WB_REQUEST_PLAN, WbRequestPlan)
{
    URHO3D_PARAM(P_FILENAME, Filename);       // String
}

/// Client sends a workboard mutation (wb-* command).
URHO3D_EVENT(E_WB_MUTATION, WbMutation)
{
    URHO3D_PARAM(P_COMMAND, Command);         // String — e.g. "add-ready", "move-done"
    URHO3D_PARAM(P_ARGS, Args);              // String — command arguments
}

/// Client sets its display identity.
URHO3D_EVENT(E_WB_SET_IDENTITY, WbSetIdentity)
{
    URHO3D_PARAM(P_NAME, Name);               // String
    URHO3D_PARAM(P_ROLE, Role);               // String
}

// ── Client info tracked by server ──

struct WbClientInfo
{
    Connection* connection_{};
    String name_;
    String role_;
    bool authenticated_{false};
};

// ── Shared workboard data structures ──

/// A parsed table row from the workboard markdown.
struct WorkboardRow
{
    Vector<String> cells;
};

/// A parsed table section (Ready, In Progress, Done, etc.)
struct WorkboardSection
{
    String title;
    Vector<String> headers;
    Vector<WorkboardRow> rows;
};
