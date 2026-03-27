#!/bin/bash
# tty-inject.sh — Send a message to a Claude Code instance via its pty-proxy socket
# Usage: tty-inject.sh <role> <message>
#
# The message is injected into the instance's TTY as if the user typed it.

ROLE="$1"
shift
MESSAGE="$*"

if [ -z "$ROLE" ] || [ -z "$MESSAGE" ]; then
    echo "Usage: tty-inject.sh <role> <message>" >&2
    exit 1
fi

SOCK="/tmp/urho_claude/tty/${ROLE}.sock"

if [ ! -S "$SOCK" ]; then
    echo "No socket for role '$ROLE' at $SOCK" >&2
    exit 1
fi

python3 -c "
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect('$SOCK')
    # Send message text first
    s.sendall(sys.argv[1].encode())
    # Brief pause so TUI processes the text as input
    time.sleep(0.05)
    # Then send Enter as a separate event
    s.sendall(b'\r')
    s.close()
except Exception as e:
    print(f'Injection failed: {e}', file=sys.stderr)
    sys.exit(1)
" "$MESSAGE"
