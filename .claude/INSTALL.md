# Claude Code Multi-Instance IPC System

This archive contains the hook scripts and configuration for running multiple Claude Code instances as a coordinated team on the same project.

## What's Included

```
hooks/
    claude_ipc.sh      # Role management, instance registration, spawning, workboard commands
    ipc_listen.sh      # Background message listener for inter-instance communication
    safe_build.sh      # flock-guarded build wrapper — prevents concurrent build corruption
    file_lock.sh       # Atomic file locking for concurrent Read/Write/Edit operations
settings.local.json    # Hook configuration (SessionStart, UserPromptSubmit, Stop, etc.)
INSTALL.md             # This file
```

## Features

- **Role-based coordination**: Planner (architecture/docs) + multiple Coders (implementation)
- **Auto-registration**: Instances register on startup, auto-number (coder, coder2, coder3, ...)
- **spawn-coder**: Launch new Coder instances from Planner with initial prompt
- **Message spool**: Atomic file-based messaging between instances (no messages lost)
- **Wake FIFOs**: Instant notification when messages arrive
- **Build safety**: Per-target flock prevents concurrent `make` corruption
- **File locking**: Atomic mkdir-based locks on Read/Write/Edit to prevent race conditions
- **Dead instance culling**: Stale PIDs detected and cleaned automatically

## Installation

### Where `.claude` Lives

Claude Code looks for project-level configuration in a `.claude/` directory at the project root:

| Platform | Location |
|----------|----------|
| **Linux** | `<project-root>/.claude/` |
| **macOS** | `<project-root>/.claude/` |
| **Windows (WSL)** | `<project-root>/.claude/` |
| **Windows (native)** | `<project-root>\.claude\` (hooks require WSL or Git Bash) |

User-level settings (not in this archive) live at:

| Platform | Location |
|----------|----------|
| **Linux** | `~/.claude/` |
| **macOS** | `~/.claude/` |
| **Windows** | `%USERPROFILE%\.claude\` |

### Steps

1. Extract this archive into your project root:
   ```bash
   cd /path/to/your/project
   unzip claude-ipc-hooks.zip
   ```
   This creates `.claude/hooks/` and `.claude/settings.local.json`.

2. Make the hook scripts executable:
   ```bash
   chmod +x .claude/hooks/*.sh
   ```

3. Create the IPC runtime directory:
   ```bash
   mkdir -p /tmp/urho_claude/{instances,spool}
   ```

4. Start Claude Code normally — the SessionStart hook will register the instance automatically.

### Optional: WorkboardManager

The GUI dashboard (`WorkboardManager`) provides visual instance status, message composition, and workboard display. It's an Urho3D application built separately — see the main README for build instructions.

## Usage

```bash
# From any running Claude Code instance:

# Assume a role
.claude/hooks/claude_ipc.sh assume planner
.claude/hooks/claude_ipc.sh assume coder

# Spawn a new Coder in a separate terminal
.claude/hooks/claude_ipc.sh spawn-coder

# Safe builds (prevents concurrent make corruption)
.claude/hooks/safe_build.sh TargetName

# Workboard commands
.claude/hooks/claude_ipc.sh wb-add-ready "Task description"
.claude/hooks/claude_ipc.sh wb-move-done "Task description"
```

## How It Works

1. **SessionStart** hook runs `claude_ipc.sh announce` — registers PID and TTY as `unassigned`
2. **UserPromptSubmit** hook runs `claude_ipc.sh check` — delivers any queued spool messages
3. **Notification** hook runs `claude_ipc.sh check` — same delivery on background notifications
4. **Stop** hook runs `claude_ipc.sh report` — notifies Manager that a turn completed
5. **SessionEnd** hook runs `claude_ipc.sh cleanup` — unregisters PID, removes role files
6. **PreToolUse/PostToolUse** hooks run `file_lock.sh` — atomic locks on file operations

## Requirements

- Claude Code CLI (`claude` in PATH)
- Bash shell
- `flock` command (standard on Linux, available via Homebrew on macOS)
- `mkfifo` for wake FIFOs
- A terminal emulator (gnome-terminal, xfce4-terminal, konsole, or xterm) for spawn-coder
