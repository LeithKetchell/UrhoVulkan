# IPC Protocol — Claude Code Instances + WorkboardManager

## Overview

Multiple Claude Code instances coordinate via **TTY injection** through pty-proxy Unix sockets and a shared workboard file. The old spool/FIFO system was replaced (commit 5d25e00, Mar 28 2026).

## Architecture

### Directory Layout

```
/tmp/urho_claude/
    tty/
        planner.sock        # pty-proxy Unix socket for planner TTY injection
        coder1.sock         # pty-proxy socket for coder instance 1
        coder2.sock         # pty-proxy socket for coder instance 2 (etc.)
    instances/
        <role>.pid          # PID of instance owning this role
        <TTY_ID>.role       # Line 1: role name, Line 2: PID
    locks/                  # File I/O locks (mkdir-based, auto-managed by hooks)
    manager.pid             # WorkboardManager singleton guard
    hook_debug.log          # Debug breadcrumbs
    workboard.lock          # flock for workboard file mutations
```

### TTY Injection (the IPC mechanism)

Messages are injected directly into Claude Code terminals via pty-proxy Unix sockets. This is the **only** working input injection path on kernel 6.8 (TIOCSTI disabled, /dev/pts/N is output-only).

**How it works:**
1. Each Claude instance launches through `pty-proxy --socket <path> -- claude`
2. pty-proxy creates a pseudoterminal and a Unix socket
3. To send a message to an instance, write bytes to its socket
4. pty-proxy forwards socket data to the pty's master fd (appears as keyboard input)
5. Bracketed paste detection: send text as bulk, sleep 150ms, then send `\r` separately

### Launching Instances

Two bash functions in `~/.bashrc`:

**`planner`** — guarded singleton:
```bash
planner() {
    if [ -e "/tmp/urho_claude/tty/planner.sock" ]; then
        echo "Planner is already running."
        return 1
    fi
    env -u CLAUDECODE gnome-terminal -- .claude/hooks/pty-proxy \
        --socket /tmp/urho_claude/tty/planner.sock \
        -- claude "Planner startup..."
}
```

**`coder`** — auto-incrementing:
```bash
coder() {
    local n=1
    while [ -e "/tmp/urho_claude/tty/coder${n}.sock" ]; do
        n=$((n + 1))
    done
    env -u CLAUDECODE gnome-terminal -- .claude/hooks/pty-proxy \
        --socket "/tmp/urho_claude/tty/coder${n}.sock" \
        -- claude "Coder startup..."
}
```

**Usage** — from the project directory:
```
planner
coder
coder
```
Three terminals appear. Each gets its own pty-proxy socket, own PID, own IPC registration.

## Session Startup (every Claude instance)

1. **Auto-registered as `unassigned`** — the `SessionStart` hook writes a role file with PID to `/tmp/urho_claude/instances/`
2. **Read the workboard**: `Claude/WORKBOARD.md` — team roles, tasks, what's ready
3. **Check who's online**: `ls /tmp/urho_claude/instances/*.pid`
4. **Assume a role**: `.claude/hooks/claude_ipc.sh assume <role>`
5. **React to incoming TTY messages** — messages arrive between prompts via socket injection

## Message Flow

### Sending messages to Claude instances

1. Find the target's socket: `/tmp/urho_claude/tty/<role>.sock`
2. Write message text to the socket (bulk)
3. Sleep 150ms (bracketed paste detection window)
4. Write `\r` to the socket (triggers submit)

### Claude → Claude

Write to the target's pty-proxy socket directly via `tty-inject.sh` or `claude_ipc.sh send`.
## Liveness & Dead Instance Culling

1. **Sweep `.role` files** — reads PID from line 2, checks `kill(pid, 0)`. Dead? Remove files + socket.
2. **Sweep orphaned `.pid` files** — checks if PID is alive. Dead? Remove.
3. **Activity timers** — reset on any message received from a role. 5-minute timeout.
4. **GUI updates** — status bar shows ONLINE (green) / OFFLINE (gray) per role with PID.

## Hook Configuration

Defined in `.claude/settings.local.json`:

| Event | Action | Hook Script |
|-------|--------|-------------|
| `SessionStart` | Register instance, start TTY listener | `claude_ipc.sh announce` |
| `UserPromptSubmit` | Check for pending messages | `claude_ipc.sh check` |
| `Stop` | Write status | `claude_ipc.sh report` |
| `SessionEnd` | Unregister instance, remove socket | `claude_ipc.sh cleanup` |

## Scripts

| Script | Purpose |
|--------|---------|
| `.claude/hooks/claude_ipc.sh` | Main IPC hook — announce, check, report, cleanup, assume, send, wb-* mutations |
| `.claude/hooks/pty-proxy` | C binary — creates pty + Unix socket for TTY injection |
| `.claude/hooks/tty-inject.sh` | Send a message to a pty-proxy socket |
| `.claude/hooks/safe_build.sh` | Per-target flock wrapper around make |

## Workboard Mutations

Flock-serialized direct file editing:

```bash
.claude/hooks/claude_ipc.sh wb-add-ready <pri> <plan> <file> <owner> <review> <summary>
.claude/hooks/claude_ipc.sh wb-add-inprogress <task> <owner> <started> <review> <notes>
.claude/hooks/claude_ipc.sh wb-add-done <task> <owner> <date> <review> <notes>
.claude/hooks/claude_ipc.sh wb-move-done <task>
.claude/hooks/claude_ipc.sh wb-remove <match-text>
.claude/hooks/claude_ipc.sh wb-update-review <task> <review>
.claude/hooks/claude_ipc.sh wb-assign <task> <role>
```

## History

| Version | Date | Mechanism |
|---------|------|-----------|
| V1 | Mar 2026 | FIFO + drop-file per role |
| V2 | Mar 23 | Atomic spool directories |
| V3 | Mar 28 | TTY injection via pty-proxy (commit 5d25e00). Spool gutted. |
| V3.1 | Mar 29 | Bash launch functions (`planner`, `coder`). Planner guard. |
