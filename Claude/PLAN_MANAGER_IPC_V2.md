# PLAN: Manager Communication V2 — Message Spool

**Status:** DRAFT
**Owner:** Planner
**Priority:** High — current IPC drops messages and has race conditions
**Hardware target:** Linux (Unix filesystem semantics)

---

## Problem

The current IPC uses Unix FIFOs and drop-files. This has fundamental issues:

1. **Message loss**: `to_<role>.msg` is a single file — two quick sends overwrite the first
2. **Listener gap**: Between `ipc_listen.sh` exiting and re-launch, the FIFO has no reader. Messages written during that window block (or fail with ENXIO if O_NONBLOCK)
3. **60s timeout churn**: Listener re-arms every minute. Each cycle = dead period where no FIFO reader exists
4. **No delivery guarantee**: Manager writes a wake signal and a drop-file. No way to know if Claude actually read it
5. **Dual delivery fragility**: Hook check AND FIFO listener both look at the same drop-file. Race condition on who reads it first, or both miss it
6. **FIFO byte-stream mismatch**: FIFOs are byte streams, not message queues. We need message semantics (discrete messages, ordered, non-destructive queueing)
7. **Hardcoded roles**: Every new role needs new FIFOs, new fd members, new poll calls in C++

## Solution: Message Spool Directory

Replace FIFOs and drop-files with a **directory-based message spool**. Each message is an atomic file. Multiple messages queue naturally. No FIFOs, no blocking, no races.

---

## Architecture

### Directory Layout

```
/tmp/urho_claude/
    spool/
        to_coder/           ← Messages waiting for Coder to read
            001_manager.msg
            002_planner.msg
        to_planner/         ← Messages waiting for Planner to read
            001_manager.msg
        to_manager/         ← Messages waiting for Manager to read
            001_coder.msg
            002_planner.msg
        to_broadcast/       ← Broadcast: Manager copies to all role dirs
    instances/              ← Unchanged — role files, PID files
    manager.pid             ← Unchanged — singleton guard
```

### Message File Format

Each message is a single file:

```
From: planner
Time: 2026-03-23T14:30:05
Type: chat
---
The ecosystem plan is ready for review.
Check PLAN_ECOSYSTEM.md when you get a chance.
```

- **Filename**: `<sequence>_<from>.msg` — zero-padded sequence for ordering (e.g. `001_manager.msg`)
- **Atomic write**: Write to `.tmp`, rename to `.msg`. Readers never see partial files.
- **Headers**: From, Time, Type (chat/command/status/workboard). Extensible — add headers without breaking readers.
- **Body**: Everything after the `---` separator. Plain text.

### Message Types

| Type | Meaning | Example |
|------|---------|---------|
| `chat` | Human-readable message | "Check the ecosystem plan" |
| `command` | Structured command | `WB:add-ready:...` workboard mutation |
| `status` | Heartbeat / turn complete | `[planner] Turn complete at 14:30` |
| `ack` | Delivery acknowledgment | `ACK:003` — confirms message 003 was read |

### Sequence Numbers

Each spool directory maintains a `.seq` file containing the next sequence number. Writers:

1. Read `.seq` (or start at 1 if missing)
2. Write message as `<seq>_<from>.msg`
3. Increment `.seq`

Protected by flock on the `.seq` file. Simple, no contention.

---

## How It Works

### Manager → Claude

1. User types message in Manager GUI, clicks "Coder"
2. Manager writes `003_manager.msg` atomically into `spool/to_coder/`
3. Manager writes `wake\n` to wake FIFO (kept for instant notification — but message is already safe in spool)
4. Claude's hook or listener sees the wake, scans `spool/to_coder/`, reads all `.msg` files in order
5. Claude processes messages, deletes the files (or moves to `spool/to_coder/read/` for history)
6. Optionally: Claude writes `ACK:003` back to `spool/to_manager/`

**Key difference**: Even if the wake signal is missed, the message files persist in the spool. The next hook check (UserPromptSubmit) will find them. **Messages cannot be lost.**

### Claude → Manager

1. Claude's hook writes `001_coder.msg` into `spool/to_manager/`
2. Manager's `PollSpool()` (replaces `PollFIFOs()`) scans `spool/to_manager/` on each Update tick
3. Manager reads, processes, deletes/archives

**Key difference**: No FIFO write timeout. No SIGPIPE. No blocking. Just atomic file create → rename.

### Claude → Claude (via Manager relay)

1. Planner writes message with `Type: relay` and `Target: coder` into `spool/to_manager/`
2. Manager reads it, copies body into `spool/to_coder/` with `From: planner`
3. Normal delivery from there

### Broadcast

1. Manager writes to `spool/to_broadcast/`
2. On next poll, Manager copies the file into every active role's spool directory
3. Deletes from broadcast dir

Or simpler: Manager just writes to each role's spool directly. No broadcast dir needed.

---

## Claude-Side Changes

### Hook: `claude_ipc.sh check` (replaces current check)

```bash
check)
    ROLE=$(get_role)
    SPOOL_DIR="$IPC_DIR/spool/to_${ROLE}"
    mkdir -p "$SPOOL_DIR"

    # Read all pending messages in order
    for msg in $(ls -1 "$SPOOL_DIR"/*.msg 2>/dev/null | sort); do
        echo ""
        echo "=== MESSAGE FROM WORKBOARD MANAGER ==="
        # Skip headers, print body
        sed '1,/^---$/d' "$msg"
        echo "=== END MESSAGE ==="
        echo ""
        rm -f "$msg"
    done
    exit 0
    ;;
```

That's it. No FIFO reads. No timing windows. Just list files, read them, delete them. If there are 5 queued messages, all 5 are delivered in order.

### Hook: `claude_ipc.sh report` (replaces current report)

```bash
report)
    ROLE=$(get_role)
    SPOOL_DIR="$IPC_DIR/spool/to_manager"
    mkdir -p "$SPOOL_DIR"

    # Atomic write
    SEQ=$(cat "$SPOOL_DIR/.seq" 2>/dev/null || echo 1)
    NEXT=$((SEQ + 1))
    PADDED=$(printf "%03d" "$SEQ")
    TMP="$SPOOL_DIR/${PADDED}_${ROLE}.tmp"
    MSG="$SPOOL_DIR/${PADDED}_${ROLE}.msg"

    cat > "$TMP" <<EOF
From: $ROLE
Time: $(date -Iseconds)
Type: status
---
[$ROLE] Turn complete at $(date '+%H:%M:%S')
EOF
    mv "$TMP" "$MSG"
    echo "$NEXT" > "$SPOOL_DIR/.seq"
    exit 0
    ;;
```

### Hook: `claude_ipc.sh send` (replaces current send)

```bash
send)
    TARGET="$2"; MSG="$3"
    if [ -z "$MSG" ]; then MSG="$TARGET"; TARGET="manager"; fi
    ROLE=$(get_role)
    SPOOL_DIR="$IPC_DIR/spool/to_${TARGET}"
    mkdir -p "$SPOOL_DIR"

    SEQ=$(flock "$SPOOL_DIR/.seq.lock" cat "$SPOOL_DIR/.seq" 2>/dev/null || echo 1)
    NEXT=$((SEQ + 1))
    PADDED=$(printf "%03d" "$SEQ")
    TMP="$SPOOL_DIR/${PADDED}_${ROLE}.tmp"
    FINAL="$SPOOL_DIR/${PADDED}_${ROLE}.msg"

    cat > "$TMP" <<EOF
From: $ROLE
Time: $(date -Iseconds)
Type: chat
---
$MSG
EOF
    mv "$TMP" "$FINAL"
    flock "$SPOOL_DIR/.seq.lock" bash -c "echo $NEXT > '$SPOOL_DIR/.seq'"
    echo "Sent to $TARGET: $MSG"
    exit 0
    ;;
```

### Listener: `ipc_listen.sh` (simplified)

```bash
#!/bin/bash
ROLE="${1:-coder}"
IPC_DIR="/tmp/urho_claude"
FIFO="$IPC_DIR/wake_${ROLE}"
SPOOL="$IPC_DIR/spool/to_${ROLE}"

[ -p "$FIFO" ] || mkfifo "$FIFO" 2>/dev/null
mkdir -p "$SPOOL"

# Block until wake signal OR timeout
read -t 60 line < "$FIFO" 2>/dev/null

# Drain spool (all pending messages, in order)
for msg in $(ls -1 "$SPOOL"/*.msg 2>/dev/null | sort); do
    sed '1,/^---$/d' "$msg"
    echo "---"
    rm -f "$msg"
done

# If nothing was in spool, say so
ls "$SPOOL"/*.msg 2>/dev/null | grep -q . || echo "[wake: no pending messages]"
```

The wake FIFO is still useful for instant notification. But it's now optional — if it fails, the spool is still there. Belt and suspenders.

---

## Manager-Side Changes

### Replace `PollFIFOs()` with `PollSpool()`

```cpp
void WorkboardManager::PollSpool()
{
    PollSpoolDir("to_manager", [this](const String& from, const String& type, const String& body) {
        if (type == "status")
            UpdateActivityTimer(from);
        else if (type == "command")
            HandleWorkboardCommand(body);
        else if (type == "relay")
            RelayMessage(from, body);  // parse Target: header, copy to target spool
        else
            AppendLog(from + " → Manager", body);
    });
}

void WorkboardManager::PollSpoolDir(const String& dirName, MessageHandler handler)
{
    String spoolPath = String(IPC_DIR) + "/spool/" + dirName;
    // Scan *.msg files, sort by name (sequence order), process each
    // Parse headers (From, Time, Type), extract body after "---"
    // Delete processed files (or move to /read/ subdirectory)
}
```

### Replace `SendMessage()` with spool write

```cpp
void WorkboardManager::SendMessage(const String& target, const String& message)
{
    String spoolDir = String(IPC_DIR) + "/spool/to_" + target;
    // Ensure dir exists
    // Read .seq, compute filename
    // Write .tmp with headers + body
    // Rename to .msg (atomic)
    // Increment .seq
    // Wake instance (keep wake FIFO for instant nudge)
}
```

### Remove FIFO infrastructure

- Remove `OpenFIFOs()`, `CloseFIFOs()`, `PollFIFOs()`
- Remove `fdFromCoderRead_`, `fdFromPlannerRead_`, `fdFromUnassignedRead_` members
- Remove FIFO buffer members
- Keep wake FIFOs (optional fast-path notification)

### inotify (Optional Upgrade)

Instead of polling the spool directory every frame, use Linux `inotify` to watch `spool/to_manager/` for new files. Fire callback on `IN_MOVED_TO` (atomic rename). Zero polling cost.

```cpp
int inotifyFd_ = inotify_init1(IN_NONBLOCK);
int watchFd_ = inotify_add_watch(inotifyFd_, spoolPath, IN_MOVED_TO);
// In HandleUpdate: poll inotifyFd_ with read(), process events
```

This is a nice-to-have. Polling once per frame (60 Hz) is already fine — `readdir()` on a small directory is microseconds. But inotify eliminates even that.

---

## What This Fixes

| Problem | Current | V2 |
|---------|---------|-----|
| **Message loss** | Single drop-file, overwrite | Spool directory, files persist until read |
| **Listener gap** | FIFO has no reader between re-arms | Spool files wait indefinitely |
| **60s timeout dead period** | Listener exits, messages in limbo | Hook check reads spool directly |
| **Delivery guarantee** | None | ACK files optional, but messages persist until consumed |
| **Dual delivery race** | Hook + FIFO listener compete | One consumer: whoever reads first deletes the file |
| **FIFO complexity** | O_RDWR tricks, SIGPIPE, EOF handling | mkdir + write + rename. No file descriptors to manage. |
| **Hardcoded roles** | New FIFO per role, new fd, new poll call | New directory per role. Manager scans dynamically. |
| **Message ordering** | Single message, no queue | Sequence-numbered files, natural ordering |
| **Message history** | Gone after delivery | Move to `/read/` subdirectory instead of delete |

---

## Migration Path

### Phase 1: Add spool alongside FIFO (backward compatible)

1. Manager creates spool directories on startup
2. Manager writes messages to BOTH spool AND drop-file (transition period)
3. New `check` hook reads spool first, falls back to drop-file
4. Old `ipc_listen.sh` still works via wake FIFO + drop-file

### Phase 2: Claude hooks switch to spool-only

1. Update `claude_ipc.sh check` to read spool directory
2. Update `claude_ipc.sh send` to write spool directory
3. Update `ipc_listen.sh` to drain spool instead of reading drop-file
4. Remove drop-file writes from Manager

### Phase 3: Remove FIFO infrastructure from Manager

1. Remove `OpenFIFOs()`, `CloseFIFOs()`, `PollFIFOs()`
2. Remove FIFO fd members and buffer members
3. Keep wake FIFOs (lightweight, useful for instant notification)
4. `PollSpool()` replaces `PollFIFOs()` in `HandleUpdate()`

### Phase 4: Dynamic role discovery

1. Manager scans `spool/to_*/` directories to discover roles dynamically
2. No more hardcoded `coder/planner/unassigned` in poll loop
3. Adding a new role = creating a spool directory. That's it.

### Phase 5: Optional enhancements

1. **inotify**: Watch spool directories for instant delivery, no polling
2. **Message history**: `/read/` subdirectory per role, pruned by age
3. **ACK protocol**: Receiver writes ACK file, sender can check delivery
4. **Priority messages**: Filename prefix `P0_003_manager.msg` for urgent messages sorted before normal
5. **Structured commands**: JSON body for workboard mutations instead of pipe-delimited strings

---

## Implementation Scope

### Shell-side (claude_ipc.sh + ipc_listen.sh)

- Rewrite `check`, `report`, `send` actions to use spool
- Simplify `ipc_listen.sh` to drain spool on wake
- Keep `announce`, `cleanup`, `assume` unchanged (they manage roles, not messages)
- Keep `wb-*` commands unchanged (they edit workboard.md directly)

### C++ side (WorkboardManager)

- New `PollSpool()` method replacing `PollFIFOs()`
- New `SendToSpool()` method replacing FIFO-based `SendMessage()`
- `CreateIPCPaths()` creates spool directories
- Remove FIFO open/close/poll infrastructure
- ~150 lines of new code, ~100 lines removed

### Files to Modify

| File | Change |
|------|--------|
| `.claude/hooks/claude_ipc.sh` | `check`, `report`, `send` actions → spool |
| `.claude/hooks/ipc_listen.sh` | Drain spool instead of reading drop-file |
| `Source/Tools/WorkboardManager/WorkboardManager.h` | Remove FIFO fds, add spool methods |
| `Source/Tools/WorkboardManager/WorkboardManager.cpp` | PollSpool, SendToSpool, remove FIFO code |
| `Claude/IPC_PROTOCOL.md` | Update documentation |

---

## Why Spool Over Alternatives

| Alternative | Why Not |
|-------------|---------|
| **Unix domain sockets** | Connection-oriented. Claude hooks are short-lived scripts — connecting/disconnecting per invocation adds overhead. Manager would need accept loop. |
| **POSIX message queues** | `mq_open` needs linking, not available everywhere, kernel limits on queue depth. Overkill. |
| **Shared memory** | Fast but needs synchronization primitives. Complex for what is essentially "send text to another process." |
| **Local TCP** | Manager is already an Urho3D app — adding a TCP server means threading or poll integration. HTTP would be nice but heavy. |
| **SQLite** | Single-file DB, good for persistence, but polling and locking overhead for what should be simple message passing. |
| **File spool** | Atomic via rename. Multiple messages queue naturally. `ls + sort` gives ordering. `rm` is the ack. Works with `cat` and `bash`. Zero dependencies. Matches the Unix philosophy. |

The filesystem IS the message queue. We're just using it properly this time.

---

## Relationship to Other Plans

- **WorkboardManager IPC Downloads** — download commands become `Type: command` spool messages instead of FIFO payloads. Same semantics, better delivery.
- **Any future tool** — any process that can `mkdir` and `cat > file` can participate in IPC. No FIFO setup, no library dependencies.
