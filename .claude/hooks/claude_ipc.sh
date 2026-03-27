#!/bin/bash
# Claude IPC hook for WorkboardManager
# Usage: claude_ipc.sh <action> [args...]
#   action: announce | check | report | cleanup | assume <role>
#   action: wb-add-done <task> <owner> <date> <review> <notes>
#   action: wb-add-ready <pri> <plan> <file> <owner> <review> <summary>
#   action: wb-add-inprogress <task> <owner> <started> <review> <notes>
#   action: wb-move-done <task>   (move from In Progress to Done)

ACTION="${1:-check}"
IPC_DIR="/tmp/urho_claude"
INST_DIR="$IPC_DIR/instances"
WORKBOARD="$(cd "$(dirname "$0")/../.." && pwd)/Claude/WORKBOARD.md"
LOCKFILE="$IPC_DIR/workboard.lock"

mkdir -p "$INST_DIR"

# Get a stable identifier for this terminal session.
# TTY is stable for the lifetime of the terminal — no PID issues.
get_tty_id() {
    local t
    t=$(tty 2>/dev/null | tr '/' '_')
    # tty returns "not a tty" in some contexts — fall back to PPID
    if [ -z "$t" ] || [ "$t" = "not a tty" ] || [ "$t" = "_dev_null" ]; then
        # In hook context, walk up to find the terminal's TTY via /proc
        local walk="$PPID"
        while [ "$walk" -gt 1 ] 2>/dev/null; do
            local ctty
            ctty=$(readlink /proc/$walk/fd/0 2>/dev/null)
            if [ -n "$ctty" ] && [[ "$ctty" == /dev/pts/* ]]; then
                echo "$ctty" | tr '/' '_'
                return
            fi
            walk=$(ps -o ppid= -p "$walk" 2>/dev/null | tr -d ' ')
            [ -z "$walk" ] && break
        done
        # Last resort: use PPID (imperfect but better than nothing)
        echo "pid_$PPID"
    else
        echo "$t"
    fi
}

# Find the Claude Code process PID by walking up the process tree.
# Uses only ps (no /proc reads that might block).
get_claude_pid() {
    local walk="$PPID"
    local best="$PPID"
    while [ "$walk" -gt 1 ] 2>/dev/null; do
        local args
        args=$(ps -o args= -p "$walk" 2>/dev/null)
        case "$args" in
            *claude*) best="$walk" ;;
        esac
        walk=$(ps -o ppid= -p "$walk" 2>/dev/null | tr -d ' ')
        [ -z "$walk" ] && break
    done
    echo "$best"
}

# Resolve role from TTY-based role file
get_role() {
    local tty_id
    tty_id=$(get_tty_id)
    if [ -f "$INST_DIR/${tty_id}.role" ]; then
        head -1 "$INST_DIR/${tty_id}.role"
        return
    fi
    echo "${CLAUDE_ROLE:-unassigned}"
}

# Check if WorkboardManager is running
manager_running() {
    local pidfile="$IPC_DIR/manager.pid"
    [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile" 2>/dev/null)" 2>/dev/null
}

# Auto-launch WorkboardManager if not running
ensure_manager() {
    if manager_running; then
        return 0
    fi
    # Find the binary
    local project_root
    project_root="$(cd "$(dirname "$0")/../.." && pwd)"
    local mgr_bin="$project_root/build/bin/WorkboardManager"
    if [ ! -x "$mgr_bin" ]; then
        return 1
    fi
    # Launch in background, detached from this shell
    nohup "$mgr_bin" > /dev/null 2>&1 &
    # Give it a moment to start and create FIFOs
    sleep 1
    manager_running
}


case "$ACTION" in

announce)
    # SessionStart — register as unassigned via TTY-based role file
    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)

    # Clean up any stale .pid files that point to OUR PID from a previous role
    for pidfile in "$INST_DIR"/*.pid; do
        [ -f "$pidfile" ] || continue
        STORED=$(cat "$pidfile" 2>/dev/null)
        if [ "$STORED" = "$CLAUDE_PID" ]; then
            rm -f "$pidfile"
        fi
    done

    # Sweep orphaned builds from dead Claude instances
    for pidfile in "$INST_DIR"/*.pid; do
        [ -f "$pidfile" ] || continue
        STORED_PID=$(cat "$pidfile" 2>/dev/null)
        if [ -n "$STORED_PID" ] && ! kill -0 "$STORED_PID" 2>/dev/null; then
            DEAD_ROLE=$(basename "$pidfile" .pid)
            echo "Sweeping stale instance: $DEAD_ROLE (PID $STORED_PID)"
            rm -f "$pidfile"
            for rolefile in "$INST_DIR"/*.role; do
                [ -f "$rolefile" ] || continue
                if [ "$(head -1 "$rolefile")" = "$DEAD_ROLE" ]; then
                    rm -f "$rolefile"
                fi
            done
        fi
    done

    # Kill orphaned make processes with no living Claude parent
    for mpid in $(pgrep -f "make.*-j[0-9]" 2>/dev/null); do
        OWNED=false
        for pidfile in "$INST_DIR"/*.pid; do
            [ -f "$pidfile" ] || continue
            OWNER_PID=$(cat "$pidfile" 2>/dev/null)
            if [ -n "$OWNER_PID" ] && kill -0 "$OWNER_PID" 2>/dev/null; then
                OWNED=true
                break
            fi
        done
        if [ "$OWNED" = false ]; then
            echo "Killing orphaned build process: PID $mpid"
            kill -TERM "$mpid" 2>/dev/null
            sleep 1
            kill -9 "$mpid" 2>/dev/null
        fi
    done

    # Auto-number unassigned: unassigned, unassigned2, unassigned3, ...
    UNASSIGNED_ROLE="unassigned"
    if [ -f "$INST_DIR/unassigned.pid" ]; then
        OWNER_PID=$(cat "$INST_DIR/unassigned.pid" 2>/dev/null)
        if [ -n "$OWNER_PID" ] && [ "$OWNER_PID" != "$CLAUDE_PID" ] && kill -0 "$OWNER_PID" 2>/dev/null; then
            N=2
            while true; do
                CANDIDATE="unassigned${N}"
                if [ ! -f "$INST_DIR/${CANDIDATE}.pid" ]; then
                    UNASSIGNED_ROLE="$CANDIDATE"
                    break
                fi
                CPID=$(cat "$INST_DIR/${CANDIDATE}.pid" 2>/dev/null)
                if [ -z "$CPID" ] || [ "$CPID" = "$CLAUDE_PID" ] || ! kill -0 "$CPID" 2>/dev/null; then
                    UNASSIGNED_ROLE="$CANDIDATE"
                    break
                fi
                N=$((N + 1))
                if [ "$N" -gt 20 ]; then
                    echo "REFUSED: too many unassigned instances (20+)." >&2
                    exit 1
                fi
            done
        fi
    fi

    # Line 1: role name, Line 2: PID (for Manager wake-up and liveness checks)
    printf '%s\n%s\n' "$UNASSIGNED_ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
    # Also write <role>.pid for backward compat with Manager's ReadInstancePID()
    echo "$CLAUDE_PID" > "$INST_DIR/${UNASSIGNED_ROLE}.pid"

    echo "Claude instance registered as ${UNASSIGNED_ROLE} (TTY $TTY_ID, PID $CLAUDE_PID)"
    exit 0
    ;;

reannounce)
    # PostCompact — re-register PID for current role (compaction gives us a new process tree)
    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)

    # Read current role (keep it — don't reset to unassigned)
    ROLE="unassigned"
    if [ -f "$INST_DIR/${TTY_ID}.role" ]; then
        ROLE=$(head -1 "$INST_DIR/${TTY_ID}.role")
    fi

    # Clean up stale .pid files pointing to our OLD PID
    for pidfile in "$INST_DIR"/*.pid; do
        [ -f "$pidfile" ] || continue
        STORED=$(cat "$pidfile" 2>/dev/null)
        # Remove if process is dead (stale from before compaction)
        if [ -n "$STORED" ] && ! kill -0 "$STORED" 2>/dev/null; then
            rm -f "$pidfile"
        fi
    done

    # Update role file with fresh PID
    printf '%s\n%s\n' "$ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
    echo "$CLAUDE_PID" > "$INST_DIR/${ROLE}.pid"

    echo "PostCompact: re-registered as $ROLE (TTY $TTY_ID, PID $CLAUDE_PID)"
    exit 0
    ;;

check)
    # UserPromptSubmit / Notification — refresh PID (compaction changes process tree)
    ROLE=$(get_role)

    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)
    STORED_PID=""
    if [ -f "$INST_DIR/${ROLE}.pid" ]; then
        STORED_PID=$(cat "$INST_DIR/${ROLE}.pid" 2>/dev/null)
    fi
    if [ "$STORED_PID" != "$CLAUDE_PID" ]; then
        printf '%s\n%s\n' "$ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
        echo "$CLAUDE_PID" > "$INST_DIR/${ROLE}.pid"
    fi
    exit 0
    ;;

report)
    # Stop — no-op now that spool is removed (TTY is push-based from Manager)
    exit 0
    ;;

cleanup)
    # SessionEnd — unregister this instance, clean up both .role and .pid files
    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)
    if [ -f "$INST_DIR/${TTY_ID}.role" ]; then
        ROLE_NAME=$(head -1 "$INST_DIR/${TTY_ID}.role")
        # Remove .pid file only if it's ours
        if [ -f "$INST_DIR/${ROLE_NAME}.pid" ]; then
            STORED_PID=$(cat "$INST_DIR/${ROLE_NAME}.pid" 2>/dev/null)
            if [ "$STORED_PID" = "$CLAUDE_PID" ]; then
                rm -f "$INST_DIR/${ROLE_NAME}.pid"
            fi
        fi
        rm -f "$INST_DIR/${TTY_ID}.role"
    fi

    # Kill any orphaned build processes spawned by this instance
    if [ -n "$CLAUDE_PID" ]; then
        # Kill make processes whose parent is our Claude PID
        pkill -TERM -P "$CLAUDE_PID" make 2>/dev/null
        # Also kill any make processes in our process group
        PGID=$(ps -o pgid= -p "$CLAUDE_PID" 2>/dev/null | tr -d ' ')
        if [ -n "$PGID" ] && [ "$PGID" != "0" ]; then
            pkill -TERM -g "$PGID" make 2>/dev/null
        fi
    fi
    exit 0
    ;;

assume)
    # Role change — called by Claude via Bash tool
    # Usage: claude_ipc.sh assume <new_role>
    NEW_ROLE="$2"

    if [ -z "$NEW_ROLE" ]; then
        echo "Usage: claude_ipc.sh assume <role>" >&2
        exit 1
    fi

    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)
    OLD_ROLE="unassigned"
    if [ -f "$INST_DIR/${TTY_ID}.role" ]; then
        OLD_ROLE=$(head -1 "$INST_DIR/${TTY_ID}.role")
    fi

    # Enforce singleton for planner — refuse if already taken by a different live instance
    # Coders auto-number: coder, coder2, coder3, ... (no limit)
    if [ "$NEW_ROLE" = "planner" ] && [ -f "$INST_DIR/${NEW_ROLE}.pid" ]; then
        OWNER_PID=$(cat "$INST_DIR/${NEW_ROLE}.pid" 2>/dev/null)
        if [ -n "$OWNER_PID" ] && [ "$OWNER_PID" != "$CLAUDE_PID" ] && kill -0 "$OWNER_PID" 2>/dev/null; then
            echo "REFUSED: planner is a singleton, already taken by PID $OWNER_PID." >&2
            exit 1
        fi
    fi

    # Auto-number coders: if "coder" is taken, try coder2, coder3, ...
    if [ "$NEW_ROLE" = "coder" ] && [ -f "$INST_DIR/coder.pid" ]; then
        OWNER_PID=$(cat "$INST_DIR/coder.pid" 2>/dev/null)
        if [ -n "$OWNER_PID" ] && [ "$OWNER_PID" != "$CLAUDE_PID" ] && kill -0 "$OWNER_PID" 2>/dev/null; then
            # coder is taken — find next available number
            N=2
            while true; do
                CANDIDATE="coder${N}"
                if [ ! -f "$INST_DIR/${CANDIDATE}.pid" ]; then
                    NEW_ROLE="$CANDIDATE"
                    break
                fi
                CPID=$(cat "$INST_DIR/${CANDIDATE}.pid" 2>/dev/null)
                if [ -z "$CPID" ] || [ "$CPID" = "$CLAUDE_PID" ] || ! kill -0 "$CPID" 2>/dev/null; then
                    NEW_ROLE="$CANDIDATE"
                    break
                fi
                N=$((N + 1))
                # Safety: don't loop forever
                if [ "$N" -gt 20 ]; then
                    echo "REFUSED: too many coders (20+)." >&2
                    exit 1
                fi
            done
        fi
    fi

    # Remove old role's .pid file (only if it points to our PID)
    if [ -f "$INST_DIR/${OLD_ROLE}.pid" ]; then
        STORED_PID=$(cat "$INST_DIR/${OLD_ROLE}.pid" 2>/dev/null)
        if [ "$STORED_PID" = "$CLAUDE_PID" ]; then
            rm -f "$INST_DIR/${OLD_ROLE}.pid"
        fi
    fi

    # Write updated role file with PID
    printf '%s\n%s\n' "$NEW_ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
    # Write new role's .pid file
    echo "$CLAUDE_PID" > "$INST_DIR/${NEW_ROLE}.pid"
    # TTY injection: symlink role socket to spawn socket if running under pty-proxy
    if [ -n "$PTY_PROXY_SOCK" ] && [ -S "$PTY_PROXY_SOCK" ]; then
        TTY_SOCK_DIR="/tmp/urho_claude/tty"
        ROLE_SOCK="$TTY_SOCK_DIR/${NEW_ROLE}.sock"
        # Remove old symlink if exists
        rm -f "$ROLE_SOCK"
        ln -s "$PTY_PROXY_SOCK" "$ROLE_SOCK"
        echo "TTY socket mapped: ${NEW_ROLE}.sock -> $(basename "$PTY_PROXY_SOCK")"
    fi

    echo "Role changed: $OLD_ROLE -> $NEW_ROLE (TTY $TTY_ID, PID $CLAUDE_PID)"
    exit 0
    ;;

send)
    # Send a message via TTY injection
    # Usage: claude_ipc.sh send <target-role> <message>
    TARGET="$2"
    MSG="$3"

    if [ -z "$TARGET" ] || [ -z "$MSG" ]; then
        echo "Usage: claude_ipc.sh send <target-role> <message>" >&2
        exit 1
    fi

    HOOKS_DIR="$(cd "$(dirname "$0")" && pwd)"
    "$HOOKS_DIR/tty-inject.sh" "$TARGET" "$MSG"
    echo "Sent to $TARGET via TTY: $MSG"
    exit 0
    ;;

wb-add-done)
    TASK="$2"; OWNER="$3"; DATE="$4"; REVIEW="$5"; NOTES="$6"
    if [ -z "$TASK" ] || [ -z "$OWNER" ]; then
        echo "Usage: claude_ipc.sh wb-add-done <task> <owner> <date> <review> <notes>" >&2
        exit 1
    fi
    ROW="| $TASK | $OWNER | $DATE | $REVIEW | $NOTES |"
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
wb = '$WORKBOARD'
lines = open(wb).readlines()
task = '''$TASK'''
in_done = False
for line in lines:
    if line.strip().startswith('## Done'): in_done = True; continue
    if in_done and not line.startswith('|') and not line.strip() == '': break
    if in_done and task in line and '---' not in line and 'Task' not in line:
        print(f'Already exists in Done: {task}'); sys.exit(0)
# Auto-cleanup: remove matching entry from In Progress
in_progress = False
remove_indices = []
for i, line in enumerate(lines):
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress and not line.startswith('|') and not line.strip() == '': in_progress = False
    if in_progress and task in line and line.startswith('|') and '---' not in line and 'Task' not in line:
        remove_indices.append(i)
for idx in reversed(remove_indices):
    removed = lines.pop(idx).strip()
    print(f'Auto-cleaned from In Progress: {removed}')
row = '$ROW\n'
in_done = False; insert_at = -1
for i, line in enumerate(lines):
    if line.strip().startswith('## Done'): in_done = True; continue
    if in_done:
        if line.startswith('|') and '---' not in line and 'Task' not in line: insert_at = i + 1
        elif in_done and insert_at > 0 and not line.startswith('|'): break
if insert_at > 0:
    lines.insert(insert_at, row); open(wb, 'w').writelines(lines); print(f'Added to Done: {task}')
else: print('Could not find Done table', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    exit $?
    ;;

wb-add-ready)
    PRI="$2"; PLAN="$3"; FILE="$4"; OWNER="$5"; REVIEW="$6"; SUMMARY="$7"
    if [ -z "$PRI" ] || [ -z "$PLAN" ]; then
        echo "Usage: claude_ipc.sh wb-add-ready <pri> <plan> <file> <owner> <review> <summary>" >&2
        exit 1
    fi
    # Always use direct file edit (with flock) — Manager path caused duplicates
    ROW="| $PRI | $PLAN | \`$FILE\` | $OWNER | $REVIEW | $SUMMARY |"
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
wb = '$WORKBOARD'
lines = open(wb).readlines()
plan = '''$PLAN'''
# Dedup: skip if plan name already exists in Ready table
in_ready = False
for line in lines:
    if line.strip().startswith('## Planned') or line.strip().startswith('## Ready'): in_ready = True; continue
    if in_ready and not line.startswith('|') and not line.strip() == '': break
    if in_ready and plan in line and '---' not in line and 'Pri' not in line:
        print(f'Already exists in Planned: {plan}'); sys.exit(0)
row = '$ROW\n'
# Auto-cleanup: remove matching entry from In Progress
in_progress = False
remove_indices = []
for i, line in enumerate(lines):
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress and not line.startswith('|') and not line.strip() == '': in_progress = False
    if in_progress and plan in line and line.startswith('|') and '---' not in line and 'Task' not in line:
        remove_indices.append(i)
for idx in reversed(remove_indices):
    removed = lines.pop(idx).strip()
    print(f'Auto-cleaned from In Progress: {removed}')
# Insert into Ready
in_ready = False; insert_at = -1
for i, line in enumerate(lines):
    if line.strip().startswith('## Planned') or line.strip().startswith('## Ready'): in_ready = True; continue
    if in_ready:
        if line.startswith('|') and '---' not in line and 'Pri' not in line: insert_at = i + 1
        elif in_ready and insert_at > 0 and not line.startswith('|'): break
if insert_at > 0:
    lines.insert(insert_at, row); open(wb, 'w').writelines(lines); print(f'Added to Planned: {plan}')
else: print('Could not find Planned table', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    exit $?
    ;;

wb-add-inprogress)
    TASK="$2"; OWNER="$3"; STARTED="$4"; REVIEW="$5"; NOTES="$6"
    if [ -z "$TASK" ] || [ -z "$OWNER" ]; then
        echo "Usage: claude_ipc.sh wb-add-inprogress <task> <owner> <started> <review> <notes>" >&2
        exit 1
    fi
    ROW="| $TASK | $OWNER | $STARTED | $REVIEW | $NOTES |"
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
wb = '$WORKBOARD'
lines = open(wb).readlines()
task = '''$TASK'''
in_progress = False
for line in lines:
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress and not line.startswith('|') and not line.strip() == '': break
    if in_progress and task in line and '---' not in line and 'Task' not in line:
        print(f'Already exists in In Progress: {task}'); sys.exit(0)
row = '$ROW\n'
in_progress = False; insert_at = -1
for i, line in enumerate(lines):
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress:
        if line.startswith('|') and '---' not in line and 'Task' not in line: insert_at = i + 1
        elif in_progress and insert_at > 0 and not line.startswith('|'): break
if insert_at > 0:
    lines.insert(insert_at, row); open(wb, 'w').writelines(lines); print(f'Added to In Progress: {task}')
else: print('Could not find In Progress table', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    exit $?
    ;;

wb-move-done)
    TASK="$2"
    if [ -z "$TASK" ]; then
        echo "Usage: claude_ipc.sh wb-move-done <task>" >&2
        exit 1
    fi
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
lines = open('$WORKBOARD').readlines()
task = '''$TASK'''
in_progress = False; remove_idx = -1; removed_row = None
for i, line in enumerate(lines):
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress and line.startswith('|') and task in line and '---' not in line and 'Task' not in line:
        removed_row = line.rstrip(); remove_idx = i; break
if remove_idx < 0: print(f'Task not found: {task}', file=sys.stderr); sys.exit(1)
lines.pop(remove_idx)
in_done = False; insert_at = -1
for i, line in enumerate(lines):
    if line.strip().startswith('## Done'): in_done = True; continue
    if in_done:
        if line.startswith('|') and '---' not in line and 'Task' not in line: insert_at = i + 1
        elif in_done and insert_at > 0 and not line.startswith('|'): break
if insert_at > 0:
    lines.insert(insert_at, removed_row + '\n'); open('$WORKBOARD', 'w').writelines(lines); print(f'Moved to Done: {task}')
else: print('Could not find Done table', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    exit $?
    ;;

wb-remove)
    MATCH="$2"
    if [ -z "$MATCH" ]; then
        echo "Usage: claude_ipc.sh wb-remove <text>" >&2
        exit 1
    fi
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
lines = open('$WORKBOARD').readlines()
match = '''$MATCH'''
new_lines = []; removed = False
for line in lines:
    if not removed and match in line and line.startswith('|') and '---' not in line: removed = True; continue
    new_lines.append(line)
if removed: open('$WORKBOARD', 'w').writelines(new_lines); print(f'Removed: {match}')
else: print(f'Not found: {match}', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    exit $?
    ;;

wb-update-review)
    TASK="$2"; REVIEW="$3"
    if [ -z "$TASK" ] || [ -z "$REVIEW" ]; then
        echo "Usage: claude_ipc.sh wb-update-review <task> <review>" >&2
        exit 1
    fi
    echo "wb-update-review not yet implemented via TTY — edit workboard directly" >&2
    exit 1
    ;;

wb-download)
    URL="$2"; DEST="$3"
    if [ -z "$URL" ] || [ -z "$DEST" ]; then
        echo "Usage: claude_ipc.sh wb-download <url> <dest-relative-to-project-root>" >&2
        exit 1
    fi
    echo "wb-download not yet implemented via TTY — use curl directly" >&2
    exit 1
    ;;

wb-add-shared)
    FILE="$2"; REASON="$3"
    if [ -z "$FILE" ] || [ -z "$REASON" ]; then
        echo "Usage: claude_ipc.sh wb-add-shared <file> <reason>" >&2
        exit 1
    fi
    echo "wb-add-shared not yet implemented via TTY — edit workboard directly" >&2
    exit 1
    ;;

spawn-coder)
    # Launch a new Claude Code instance in the user's preferred terminal
    # Usage: claude_ipc.sh spawn-coder
    TERM_EMU=$(which x-terminal-emulator 2>/dev/null)
    if [ -z "$TERM_EMU" ]; then
        # Fallback chain
        for t in gnome-terminal xfce4-terminal konsole xterm; do
            TERM_EMU=$(which "$t" 2>/dev/null) && break
        done
    fi
    if [ -z "$TERM_EMU" ]; then
        echo "No terminal emulator found" >&2
        exit 1
    fi

    # Resolve the real binary (x-terminal-emulator is often a symlink)
    REAL_TERM=$(readlink -f "$TERM_EMU" 2>/dev/null || echo "$TERM_EMU")

    # Initial prompt tells the instance to follow the startup protocol
    INIT_PROMPT="Follow the Session Startup Protocol: assume the coder role by running .claude/hooks/claude_ipc.sh assume coder — then read Claude/WORKBOARD.md and check in with Leith for your assignment. Use .claude/hooks/safe_build.sh for all builds, never raw make."

    # Determine pty-proxy socket path — use a provisional name until the coder assumes a role
    # The socket will be at /tmp/urho_claude/tty/spawn_<N>.sock initially
    # Once the coder assumes a role, the socket is symlinked to the role name
    HOOKS_DIR="$(cd "$(dirname "$0")" && pwd)"
    PTY_PROXY="$HOOKS_DIR/pty-proxy"
    TTY_DIR="/tmp/urho_claude/tty"
    mkdir -p "$TTY_DIR"

    # Find next available spawn number
    SPAWN_NUM=1
    while [ -S "$TTY_DIR/spawn_${SPAWN_NUM}.sock" ]; do
        SPAWN_NUM=$((SPAWN_NUM + 1))
    done
    SPAWN_SOCK="$TTY_DIR/spawn_${SPAWN_NUM}.sock"

    if [ -x "$PTY_PROXY" ]; then
        # Launch through pty-proxy for message injection support
        case "$REAL_TERM" in
            *gnome-terminal*)
                env -u CLAUDECODE gnome-terminal -- "$PTY_PROXY" --socket "$SPAWN_SOCK" -- claude "$INIT_PROMPT" 2>/dev/null &
                ;;
            *xfce4-terminal*)
                env -u CLAUDECODE xfce4-terminal -e "\"$PTY_PROXY\" --socket \"$SPAWN_SOCK\" -- claude \"$INIT_PROMPT\"" 2>/dev/null &
                ;;
            *konsole*)
                env -u CLAUDECODE konsole -e "$PTY_PROXY" --socket "$SPAWN_SOCK" -- claude "$INIT_PROMPT" 2>/dev/null &
                ;;
            *)
                env -u CLAUDECODE "$TERM_EMU" -e "$PTY_PROXY" --socket "$SPAWN_SOCK" -- claude "$INIT_PROMPT" 2>/dev/null &
                ;;
        esac
        echo "Spawned new Claude Code instance via $REAL_TERM with pty-proxy (socket: $SPAWN_SOCK)"
    else
        # Fallback: no pty-proxy binary, launch directly
        case "$REAL_TERM" in
            *gnome-terminal*)
                env -u CLAUDECODE gnome-terminal -- claude "$INIT_PROMPT" 2>/dev/null &
                ;;
            *xfce4-terminal*)
                env -u CLAUDECODE xfce4-terminal -e "claude \"$INIT_PROMPT\"" 2>/dev/null &
                ;;
            *konsole*)
                env -u CLAUDECODE konsole -e claude "$INIT_PROMPT" 2>/dev/null &
                ;;
            *)
                env -u CLAUDECODE "$TERM_EMU" -e claude "$INIT_PROMPT" 2>/dev/null &
                ;;
        esac
        echo "Spawned new Claude Code instance via $REAL_TERM (no pty-proxy, no TTY injection)"
    fi

    # Wait for the instance to register, then find the newest unassigned and send role assignment
    sleep 5
    NEWEST_UNASSIGNED=""
    for uf in "$INST_DIR"/unassigned*.pid; do
        [ -f "$uf" ] || continue
        UPID=$(cat "$uf" 2>/dev/null)
        if [ -n "$UPID" ] && kill -0 "$UPID" 2>/dev/null; then
            UNAME=$(basename "$uf" .pid)
            NEWEST_UNASSIGNED="$UNAME"
        fi
    done
    TARGET="${NEWEST_UNASSIGNED:-unassigned}"
    echo "Role assignment queued for ${TARGET}"
    exit 0
    ;;

esac
