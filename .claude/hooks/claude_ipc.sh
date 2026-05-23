#!/bin/bash
# Claude IPC hook for WorkboardManager
# Usage: claude_ipc.sh <action> [args...]
#   action: announce | check | report | cleanup | assume [auto]
#   action: wb-add-done <task> <owner> <date> <review> <notes>
#   action: wb-add-ready <pri> <plan> <file> <owner> <review> <summary>
#   action: wb-add-inprogress <task> <owner> <started> <review> <notes>
#   action: wb-move-done <task>   (move from In Progress to Done)
#   action: wb-assign <task-name> <coder-role>  (atomic claim + TTY notify)

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

get-role)
    get_role
    exit 0
    ;;

announce)
    # SessionStart — register via TTY-based role file
    ensure_manager
    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)

    # If spawned via pty-proxy with a coder socket name, go straight to assume
    if [ -n "$PTY_PROXY_SOCK" ]; then
        SOCK_BASE=$(basename "$PTY_PROXY_SOCK" .sock)
        case "$SOCK_BASE" in
            coder|coder[0-9]*)
                "$0" assume auto
                exit $?
                ;;
        esac
    fi

    # If this TTY already has a non-unassigned role, keep it — don't stomp
    if [ -f "$INST_DIR/${TTY_ID}.role" ]; then
        EXISTING_ROLE=$(head -1 "$INST_DIR/${TTY_ID}.role")
        if [ -n "$EXISTING_ROLE" ] && [[ "$EXISTING_ROLE" != unassigned* ]]; then
            # Update PID in case it changed, but preserve the role
            printf '%s\n%s\n' "$EXISTING_ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
            echo "$CLAUDE_PID" > "$INST_DIR/${EXISTING_ROLE}.pid"
            echo "Claude instance re-registered as ${EXISTING_ROLE} (TTY $TTY_ID, PID $CLAUDE_PID) — role preserved"
            exit 0
        fi
    fi

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
    # UserPromptSubmit / Notification — refresh PID, ensure Manager is alive
    ensure_manager
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

    # Sticky note delivery — check for messages from coders or Manager
    STICKY_DIR="$IPC_DIR/sticky"
    STICKY_FILE="$STICKY_DIR/${ROLE}.msg"
    if [ -f "$STICKY_FILE" ]; then
        MSG=$(cat "$STICKY_FILE" 2>/dev/null)
        if [ -n "$MSG" ]; then
            echo "IPC message(s):"
            echo "$MSG"
        fi
        rm -f "$STICKY_FILE"
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
    # Grab the next free coder slot. First arrival gets "coder" (elder), rest get coder2, coder3, ...
    TTY_ID=$(get_tty_id)
    CLAUDE_PID=$(get_claude_pid)

    exec 9>/tmp/urho_claude/assume.lock
    flock -w 10 9 || { echo "FAILED: could not acquire assume lock." >&2; exit 1; }

    OLD_ROLE="unassigned"
    [ -f "$INST_DIR/${TTY_ID}.role" ] && OLD_ROLE=$(head -1 "$INST_DIR/${TTY_ID}.role")

    # --- Promote to fill vacant elder slot ---
    # Elder = "coder" (unnumbered). If dead/empty, promote lowest-numbered living coder.
    _elder_pid=$(head -1 "$INST_DIR/coder.pid" 2>/dev/null)
    if [ ! -f "$INST_DIR/coder.pid" ] || [ -z "$_elder_pid" ] || \
       { [ "$_elder_pid" != "$CLAUDE_PID" ] && ! kill -0 "$_elder_pid" 2>/dev/null; }; then
        rm -f "$INST_DIR/coder.pid"
        for _pn in $(seq 2 20); do
            [ -f "$INST_DIR/coder${_pn}.pid" ] || continue
            _ppid=$(head -1 "$INST_DIR/coder${_pn}.pid" 2>/dev/null)
            # Skip our own PID — we'll be assigned in the next section
            if [ -n "$_ppid" ] && [ "$_ppid" != "$CLAUDE_PID" ] && kill -0 "$_ppid" 2>/dev/null; then
                # Promote coderN -> coder (elder)
                echo "$_ppid" > "$INST_DIR/coder.pid"
                rm -f "$INST_DIR/coder${_pn}.pid"
                # Update .role file to match
                for _rf in "$INST_DIR"/*.role; do
                    [ -f "$_rf" ] || continue
                    [ "$(head -1 "$_rf")" = "coder${_pn}" ] || continue
                    _rpid=$(sed -n '2p' "$_rf")
                    printf '%s\n%s\n' "coder" "$_rpid" > "$_rf"
                done
                # Move TTY socket symlink
                _tty_dir="/tmp/urho_claude/tty"
                if [ -e "$_tty_dir/coder${_pn}.sock" ]; then
                    _sock_tgt=$(readlink "$_tty_dir/coder${_pn}.sock" 2>/dev/null)
                    rm -f "$_tty_dir/coder${_pn}.sock"
                    [ -n "$_sock_tgt" ] && ln -sf "$_sock_tgt" "$_tty_dir/coder.sock"
                fi
                # Notify promoted coder via sticky
                mkdir -p "$IPC_DIR/sticky"
                echo "ROLE REASSIGNED: You are now 'coder' (elder). Was coder${_pn}. Update your internal role." \
                    > "$IPC_DIR/sticky/coder.msg"
                echo "Promoted: coder${_pn} -> coder (elder, PID $_ppid)"
                break
            else
                rm -f "$INST_DIR/coder${_pn}.pid"
            fi
        done
    fi

    # Find first free coder slot
    NEW_ROLE="coder"
    if [ -f "$INST_DIR/coder.pid" ]; then
        CPID=$(head -1 "$INST_DIR/coder.pid" 2>/dev/null)
        if [ -n "$CPID" ] && [ "$CPID" != "$CLAUDE_PID" ] && kill -0 "$CPID" 2>/dev/null; then
            N=2
            while [ "$N" -le 20 ]; do
                CPID=$(head -1 "$INST_DIR/coder${N}.pid" 2>/dev/null)
                if [ -z "$CPID" ] || [ "$CPID" = "$CLAUDE_PID" ] || ! kill -0 "$CPID" 2>/dev/null; then
                    NEW_ROLE="coder${N}"; break
                fi
                N=$((N + 1))
            done
        fi
    fi

    # Clean up old role PID if it's ours
    if [ -f "$INST_DIR/${OLD_ROLE}.pid" ]; then
        [ "$(cat "$INST_DIR/${OLD_ROLE}.pid" 2>/dev/null)" = "$CLAUDE_PID" ] && rm -f "$INST_DIR/${OLD_ROLE}.pid"
    fi
    # Always clean stale unassigned.pid pointing to us (announce writes it before assume runs)
    if [ -f "$INST_DIR/unassigned.pid" ]; then
        [ "$(cat "$INST_DIR/unassigned.pid" 2>/dev/null)" = "$CLAUDE_PID" ] && rm -f "$INST_DIR/unassigned.pid"
    fi

    # Claim
    printf '%s\n%s\n' "$NEW_ROLE" "$CLAUDE_PID" > "$INST_DIR/${TTY_ID}.role"
    echo "$CLAUDE_PID" > "$INST_DIR/${NEW_ROLE}.pid"
    exec 9>&-

    # Set up TTY socket
    TTY_SOCK_DIR="/tmp/urho_claude/tty"
    ROLE_SOCK="$TTY_SOCK_DIR/${NEW_ROLE}.sock"
    rm -f "$ROLE_SOCK"

    if [ -n "$PTY_PROXY_SOCK" ] && [ -S "$PTY_PROXY_SOCK" ]; then
        ln -s "$PTY_PROXY_SOCK" "$ROLE_SOCK"
        echo "TTY socket mapped: ${NEW_ROLE}.sock -> $(basename "$PTY_PROXY_SOCK")"
    else
        # No PTY_PROXY_SOCK — walk process tree to find our spawn socket
        _found_spawn=""
        _walk_pid="$$"
        while [ "$_walk_pid" -gt 1 ] 2>/dev/null; do
            for _sf in "$TTY_SOCK_DIR"/spawn_*.sock; do
                [ -S "$_sf" ] || continue
                _spf="${_sf%.sock}.pid"
                [ -f "$_spf" ] || continue
                if [ "$(cat "$_spf" 2>/dev/null)" = "$_walk_pid" ]; then
                    _found_spawn="$_sf"
                    break 2
                fi
            done
            _walk_pid=$(ps -o ppid= -p "$_walk_pid" 2>/dev/null | tr -d ' ')
            [ -z "$_walk_pid" ] && break
        done
        if [ -n "$_found_spawn" ] && [ -S "$_found_spawn" ]; then
            ln -s "$_found_spawn" "$ROLE_SOCK"
            echo "TTY socket mapped: ${NEW_ROLE}.sock -> $(basename "$_found_spawn")"
        fi
        # No socket is OK — sticky notes provide reliable message delivery
    fi

    echo "Role changed: $OLD_ROLE -> $NEW_ROLE (TTY $TTY_ID, PID $CLAUDE_PID)"
    exit 0
    ;;

send)
    # Send a message to another coder via sticky note
    # Sticky notes are checked on every UserPromptSubmit hook (see 'check' action)
    # Usage: claude_ipc.sh send <target-role> <message>
    TARGET="$2"
    MSG="$3"

    if [ -z "$TARGET" ] || [ -z "$MSG" ]; then
        echo "Usage: claude_ipc.sh send <target-role> <message>" >&2
        exit 1
    fi

    # Verify target is alive
    if [ -f "$INST_DIR/${TARGET}.pid" ]; then
        TARGET_PID=$(cat "$INST_DIR/${TARGET}.pid" 2>/dev/null)
        if [ -z "$TARGET_PID" ] || ! kill -0 "$TARGET_PID" 2>/dev/null; then
            echo "WARNING: Target '$TARGET' appears dead (PID $TARGET_PID)" >&2
        fi
    else
        echo "WARNING: Target '$TARGET' not registered" >&2
    fi

    # Identify sender: try TTY-based role, fall back to PID-based lookup
    SENDER=$(get_role)
    if [ "$SENDER" = "unassigned" ]; then
        CLAUDE_PID=$(get_claude_pid)
        for _pf in "$INST_DIR"/*.pid; do
            [ -f "$_pf" ] || continue
            if [ "$(cat "$_pf" 2>/dev/null)" = "$CLAUDE_PID" ]; then
                SENDER=$(basename "$_pf" .pid)
                break
            fi
        done
    fi
    TIMESTAMP=$(date '+%H:%M:%S')
    FORMATTED="[$TIMESTAMP $SENDER] $MSG"

    # Try Unix socket first — delivers immediately into target's terminal
    TTY_SOCK_DIR="$IPC_DIR/tty"
    TARGET_SOCK="$TTY_SOCK_DIR/${TARGET}.sock"
    SOCK_SENT=0
    if [ -S "$TARGET_SOCK" ]; then
        python3 -c "
import socket, sys
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    sock.connect('$TARGET_SOCK')
    sock.sendall(('$FORMATTED' + '\n').encode())
    sock.close()
except Exception as e:
    print(f'Socket send failed: {e}', file=sys.stderr)
    sys.exit(1)
" 2>/dev/null && SOCK_SENT=1
    fi

    # Sticky note as fallback — picked up on next UserPromptSubmit
    if [ "$SOCK_SENT" -eq 0 ]; then
        STICKY_DIR="$IPC_DIR/sticky"
        mkdir -p "$STICKY_DIR"
        echo "$FORMATTED" >> "$STICKY_DIR/${TARGET}.msg"
    fi

    echo "Sent to $TARGET: $MSG"
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

wb-assign)
    TASK="$2"; CODER="$3"
    if [ -z "$TASK" ] || [ -z "$CODER" ]; then
        echo "Usage: claude_ipc.sh wb-assign <task-name> <coder-role>" >&2
        exit 1
    fi
    # Verify coder is alive
    if [ -f "$INST_DIR/${CODER}.pid" ]; then
        CODER_PID=$(cat "$INST_DIR/${CODER}.pid" 2>/dev/null)
        if [ -z "$CODER_PID" ] || ! kill -0 "$CODER_PID" 2>/dev/null; then
            echo "REJECTED: Coder '$CODER' is not alive (PID $CODER_PID)" >&2
            exit 1
        fi
    else
        echo "REJECTED: Coder '$CODER' not registered (no PID file)" >&2
        exit 1
    fi
    TODAY=$(date +%Y-%m-%d)
    (
        flock -w 10 200 || { echo "Failed to acquire workboard lock" >&2; exit 1; }
        python3 -c "
import sys
lines = open('$WORKBOARD').readlines()
task = '''$TASK'''
coder = '''$CODER'''
today = '''$TODAY'''
# Check task is NOT already in In Progress, and coder doesn't already own a task
in_progress = False
for line in lines:
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress and not line.startswith('|') and not line.strip() == '': in_progress = False
    if in_progress and line.startswith('|') and '---' not in line and 'Task' not in line:
        cells = [c.strip() for c in line.split('|')]
        if any(c == task for c in cells):
            print(f'REJECTED: Task already in In Progress: {task}', file=sys.stderr); sys.exit(1)
        if any(c == coder for c in cells):
            print(f'REJECTED: {coder} already owns a task in In Progress', file=sys.stderr); sys.exit(1)
# Find task in Planned section
in_planned = False; found_idx = -1; found_line = None
for i, line in enumerate(lines):
    if line.strip().startswith('## Planned'): in_planned = True; continue
    if in_planned and not line.startswith('|') and not line.strip() == '': in_planned = False
    if in_planned and line.startswith('|') and '---' not in line and 'Pri' not in line:
        cells = [c.strip() for c in line.split('|')]
        if not any(c == task for c in cells): continue
        found_idx = i; found_line = line.rstrip(); break
if found_idx < 0: print(f'REJECTED: Task not found in Planned: {task}', file=sys.stderr); sys.exit(1)
# Remove from Planned
lines.pop(found_idx)
# Build In Progress row: | Task | Owner | Started | Review | Notes |
ip_row = f'| {task} | {coder} | {today} | — | Assigned via wb-assign |\n'
# Insert into In Progress
in_progress = False; insert_at = -1
for i, line in enumerate(lines):
    if line.strip().startswith('## In Progress'): in_progress = True; continue
    if in_progress:
        if line.startswith('|') and '---' not in line and 'Task' not in line: insert_at = i + 1
        elif in_progress and insert_at > 0 and not line.startswith('|'): break
# If no rows yet, insert after the separator line
if insert_at < 0:
    for i, line in enumerate(lines):
        if line.strip().startswith('## In Progress'): in_progress = True; continue
        if in_progress and '---' in line: insert_at = i + 1; break
if insert_at > 0:
    lines.insert(insert_at, ip_row)
    open('$WORKBOARD', 'w').writelines(lines)
    print(f'ASSIGNED: {task} -> {coder}')
else:
    print('Could not find In Progress table', file=sys.stderr); sys.exit(1)
"
    ) 200>"$LOCKFILE"
    RESULT=$?
    if [ $RESULT -eq 0 ]; then
        # Notify the assigned coder via sticky note
        STICKY_DIR="$IPC_DIR/sticky"
        mkdir -p "$STICKY_DIR"
        echo "[$(date '+%H:%M:%S') manager] TASK ASSIGNED: $TASK — You own this. Check the workboard and start working." \
            >> "$STICKY_DIR/${CODER}.msg"
    fi
    exit $RESULT
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
    # Launch a new Claude Code instance via ClaudeTerminal
    # Usage: claude_ipc.sh spawn-coder
    # ClaudeTerminal owns the PTY, IPC socket, and injection scheduling.
    # No gnome-terminal, no pty-proxy needed.

    # Role is auto-detected by 'assume' under flock — no need to pre-decide

    # Find ClaudeTerminal binary — check build/bin first, then PATH
    HOOKS_DIR="$(cd "$(dirname "$0")" && pwd)"
    PROJECT_ROOT="$(cd "$HOOKS_DIR/../.." && pwd)"
    CLAUDE_TERM="$PROJECT_ROOT/build/bin/ClaudeTerminal"

    if [ ! -x "$CLAUDE_TERM" ]; then
        CLAUDE_TERM=$(which ClaudeTerminal 2>/dev/null)
    fi

    if [ -z "$CLAUDE_TERM" ] || [ ! -x "$CLAUDE_TERM" ]; then
        echo "ClaudeTerminal not found. Falling back to gnome-terminal." >&2

        # Fallback to old gnome-terminal + pty-proxy path
        TERM_EMU=$(which gnome-terminal 2>/dev/null || which x-terminal-emulator 2>/dev/null)
        if [ -z "$TERM_EMU" ]; then
            echo "No terminal emulator found" >&2
            exit 1
        fi
        INIT_PROMPT="Follow the Session Startup Protocol: assume your role by running .claude/hooks/claude_ipc.sh assume -- then read Claude/WORKBOARD.md and check in with Leith for your assignment. Use .claude/hooks/safe_build.sh for all builds, never raw make."
        PTY_PROXY="$HOOKS_DIR/pty-proxy"
        TTY_DIR="/tmp/urho_claude/tty"
        mkdir -p "$TTY_DIR"
        SPAWN_NUM=1
        while [ -S "$TTY_DIR/spawn_${SPAWN_NUM}.sock" ]; do
            SPAWN_NUM=$((SPAWN_NUM + 1))
        done
        SPAWN_SOCK="$TTY_DIR/spawn_${SPAWN_NUM}.sock"
        if [ -x "$PTY_PROXY" ]; then
            env -u CLAUDECODE gnome-terminal -- "$PTY_PROXY" --socket "$SPAWN_SOCK" -- claude --dangerously-skip-permissions "$INIT_PROMPT" 2>/dev/null &
        else
            env -u CLAUDECODE gnome-terminal -- claude --dangerously-skip-permissions "$INIT_PROMPT" 2>/dev/null &
        fi
        echo "Spawned via gnome-terminal fallback"
        exit 0
    fi

    # Launch ClaudeTerminal — role auto-detected by assume
    cd "$PROJECT_ROOT/build/bin"
    env -u CLAUDECODE "$CLAUDE_TERM" 2>/dev/null &
    echo "Spawned ClaudeTerminal (PID: $!) — role will be auto-assigned"
    exit 0
    ;;

esac
