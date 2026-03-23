# IPC Protocol — Claude Code Instances + WorkboardManager

## Overview

Multiple Claude Code instances communicate with each other and with the WorkboardManager GUI via a **message spool directory** and wake FIFOs. Messages are atomic files — no FIFO byte-stream issues, no message loss, natural queueing.

## Message Spool Architecture

### Directory Layout

```
/tmp/urho_claude/
    spool/
        to_coder/           ← Messages waiting for Coder to read
            001_manager.msg
            002_planner.msg
        to_planner/         ← Messages waiting for Planner to read
        to_manager/         ← Messages waiting for Manager to read
        to_unassigned/      ← Messages waiting for Unassigned to read
    instances/
        <role>.pid          # PID of instance owning this role
        <TTY_ID>.role       # Line 1: role name, Line 2: PID
    wake_<role>             # FIFO: instant notification nudge (optional)
    manager.pid             # WorkboardManager singleton guard
    hook_debug.log          # Debug breadcrumbs
    workboard.lock          # flock for workboard file mutations
```

### Message File Format

Each message is a single file with headers and body:

```
From: planner
Time: 2026-03-23T14:30:05+11:00
Type: chat
---
The ecosystem plan is ready for review.
```

- **Filename**: `<seq>_<from>.msg` — zero-padded 3-digit sequence for ordering
- **Atomic write**: `.tmp` → rename to `.msg`. Readers never see partial files.
- **Headers**: `From`, `Time`, `Type` (chat/command/status). Extensible.
- **Body**: Everything after `---` separator. Plain text.

### Message Types

| Type | Meaning | Example |
|------|---------|---------|
| `chat` | Human-readable message | "Check the ecosystem plan" |
| `command` | Structured command | `WB:add-ready:...` workboard mutation |
| `status` | Heartbeat / turn complete | `[planner] Turn complete at 14:30` |

### Sequence Numbers

Each spool directory has a `.seq` file with the next sequence number. Writers use flock (shell) or single-process guarantee (Manager) to increment atomically.

## Session Startup (every Claude instance)

1. **Auto-registered as `unassigned`** — the `SessionStart` hook writes a TTY-based role file with PID to `/tmp/urho_claude/instances/` and creates spool directories
2. **Read the workboard**: `Claude/WORKBOARD.md` — team roles, tasks, what's ready
3. **Check who's online**: `ls /tmp/urho_claude/instances/*.pid`
4. **Assume a role**: `.claude/hooks/claude_ipc.sh assume <role>` — creates spool dir for role
5. **Start the IPC listener**: Launch `.claude/hooks/ipc_listen.sh <role>` as a **background Bash task**
6. **Re-listen after every wake**: When the listener task completes, read output, respond, re-launch

## Message Flow

### Manager → Claude

1. User types message in Manager GUI, clicks a role button (Coder/Planner/Broadcast)
2. Manager writes atomic message file to `spool/to_<role>/`
3. Manager writes `wake\n` to wake FIFO for instant notification
4. Claude's background `ipc_listen.sh` unblocks, drains spool directory, outputs all messages
5. Even if wake signal is missed, `UserPromptSubmit` hook drains the spool on next turn
6. **Messages cannot be lost** — files persist until consumed

### Claude → Manager

1. Claude's hook writes message file to `spool/to_manager/`
2. Manager's `PollSpool()` scans `spool/to_manager/` every frame
3. Manager reads, processes (log/command/relay), deletes consumed files

### Claude → Claude (via spool)

1. Claude can write directly to another role's spool: `spool/to_<target>/`
2. Or write a relay message to Manager, which copies to target's spool
3. Wake FIFO pings target for instant delivery

### Broadcast

Manager writes to each alive role's spool directory directly.

## What Changed (V1 → V2)

| Aspect | V1 (FIFO + drop-file) | V2 (Spool) |
|--------|----------------------|------------|
| Message storage | Single drop-file per role | Directory of numbered files |
| Queueing | One message at a time | Unlimited queue |
| Delivery | FIFO byte stream + delimiter parsing | Atomic files, `ls | sort` |
| Loss risk | High (overwrite, timing gaps) | Zero (files persist) |
| Ordering | Within FIFO stream | Filename sequence numbers |
| Manager→Claude | `PollFIFOs()` with O_RDWR trick | `PollSpool()` with readdir |
| Claude→Manager | FIFO write with timeout | File write + rename (instant) |

## Liveness & Dead Instance Culling

Unchanged from V1:

1. **Sweep `.role` files** — reads PID from line 2, checks `kill(pid, 0)`. Dead? Remove files.
2. **Sweep orphaned `.pid` files** — checks if PID is alive. Dead? Remove.
3. **Activity timers** — reset on any message received from a role. 5-minute timeout.
4. **GUI updates** — status bar shows ONLINE (green) / OFFLINE (gray) per role with PID.
5. **Beacon broadcast** — UDP port 31337 broadcasts current status.

## Hook Configuration

Defined in `.claude/settings.local.json`:

| Event | Action | Hook Script |
|-------|--------|-------------|
| `SessionStart` | Register instance, create spool dirs | `claude_ipc.sh announce` |
| `UserPromptSubmit` | Drain spool for messages | `claude_ipc.sh check` |
| `Notification` | Drain spool for messages | `claude_ipc.sh check` |
| `Stop` | Write status to manager spool | `claude_ipc.sh report` |
| `SessionEnd` | Unregister instance | `claude_ipc.sh cleanup` |

## Scripts

| Script | Purpose |
|--------|---------|
| `.claude/hooks/claude_ipc.sh` | Main IPC hook — announce, check, report, cleanup, assume, send, workboard mutations |
| `.claude/hooks/ipc_listen.sh` | Wake FIFO listener — blocks until wake or 60s timeout, drains spool |

## Workboard Mutations

Unchanged — still use flock-serialized direct file editing:

```bash
.claude/hooks/claude_ipc.sh wb-add-ready <pri> <plan> <file> <owner> <review> <summary>
.claude/hooks/claude_ipc.sh wb-add-inprogress <task> <owner> <started> <review> <notes>
.claude/hooks/claude_ipc.sh wb-add-done <task> <owner> <date> <review> <notes>
.claude/hooks/claude_ipc.sh wb-move-done <task>
.claude/hooks/claude_ipc.sh wb-remove <match-text>
.claude/hooks/claude_ipc.sh wb-update-review <task> <review>
```
