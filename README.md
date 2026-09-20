# jrun

A fast, lightweight, keyboard-driven Linux CLI utility written in **C** that combines intelligent frecency-based directory jumping (like `zoxide`) with a general-purpose project resolver and arbitrary command launcher.

```text
             zoxide
                │
                │
       directory intelligence
                │
                ▼
        ┌───────────────┐
        │     jrun      │
        └───────────────┘
          │      │      │
          ▼      ▼      ▼
        search  TUI   execute
          │      │      │
          ▼      ▼      ▼
       resolve  choose  command
       directory       in directory
```

## Features

- **Arbitrary Command Execution**: Resolves a project/directory target and executes any command with that directory as the working directory (`jrun hypr nvim`, `jrun constituent git status`).
- **Safe Process Spawning**: Uses POSIX `fork()`, `chdir()`, and `execvp()`. Arguments are preserved without unsafe shell concatenation or `system()` calls.
- **Persistent Frecency Database**: Built on embedded SQLite (`~/.local/share/jrun/jrun.db`). Combines access frequency, recency decay weighting, and matching quality.
- **Configured Filesystem Roots**: Recursively indexes and searches project roots (`~/development`, `~/projects`, `~/Documents`, `~/dotfiles`, etc.) up to configurable depth.
- **Interactive Disambiguation TUI**: When multiple plausible project candidates exist (e.g. duplicate project names across different roots), an interactive, zero-overhead terminal UI appears with real-time fuzzy filtering, arrow navigation, and instant selection.
- **Directory Jumper Mode & Shell Integration**: Works as a drop-in directory jumper (`j <target>`). Supports Bash, Zsh, and Fish with automatic directory tracking hooks.
- **Zero Heavy Dependencies**: Pure C11, standard POSIX libraries, and embedded SQLite.

---

## Installation & Build

### Requirements

- A C11-compliant C compiler (`gcc` or `clang`)
- Standard POSIX libraries and SQLite (`libsqlite3`)
- `make`

### Building with Make

```bash
# Build release binary (optimized -O3)
make release

# Run the comprehensive test suite
make test

# Install to /usr/local/bin (or specify PREFIX)
sudo make install
```

### Building with Nob (Self-building C runner)

Following the style of `tatr`:

```bash
cc -o nob nob.c
./nob -test
sudo cp build/jrun /usr/local/bin/
```

---

## Quick Start & Usage

### 1. Launching Commands in Projects

```bash
# Open Neovim in your Hyprland configuration directory
jrun hypr nvim

# Run grep inside Hyprland config
jrun hypr grep "blur_passes"

# Check git status in your constituent project
jrun constituent git status

# Use the '--' delimiter to disambiguate target and command
jrun hypr -- nvim
jrun constituent -- npm test
```

### 2. Disambiguating Duplicate Directories

If a target matches multiple directories across your roots:

```text
~/Documents/constituent
~/development/Mereb/constituent
~/projects/constituent
```

Running `jrun constituent nvim` will automatically launch the interactive TUI:

```text
┌─ Select directory ─────────────────────────────────┐
│ Search: constituent                                │
├────────────────────────────────────────────────────┤
│                                                    │
│ ❯ ~/development/Mereb/constituent                  │
│   ~/Documents/constituent                          │
│   ~/projects/constituent                           │
│                                                    │
├────────────────────────────────────────────────────┤
│ ↑↓ Navigate   Enter Select   Esc Cancel            │
└────────────────────────────────────────────────────┘
```

- **Arrow Keys (Up / Down)**: Navigate candidates
- **Ctrl-N / Ctrl-P**: Same as down / up
- **Typing**: Live fuzzy filters the candidate list in real time
- **Ctrl-U / Ctrl-W**: Clear the query, or delete the last word
- **Enter**: Confirm selection and launch command
- **Esc / Ctrl-C**: Cancel

### 3. Directory Jumper Mode

```bash
# Prints resolved path to stdout
jrun constituent

# Or with --cd
jrun --cd constituent
```

---

## Shell Integration

Add `jrun` to your shell to get the `j` helper function and automatic directory tracking.

### Bash

Add to `~/.bashrc`:

```bash
eval "$(jrun --init bash)"
```

### Zsh

Add to `~/.zshrc`:

```bash
eval "$(jrun --init zsh)"
```

### Fish

Add to `~/.config/fish/config.fish`:

```fish
jrun --init fish | source
```

Now you can jump anywhere effortlessly:

```bash
j constituent       # cd into constituent
j -                 # cd to previous directory
j                   # cd to $HOME
j hypr nvim         # still runs commands directly!
```

---

## CLI Options & Maintenance Commands

```text
Usage:
  jrun <target> <command> [args...]       Execute command in resolved directory
  jrun <target> -- <command> [args...]    Unambiguous target and command syntax
  jrun <target>                           Resolve directory and print path
  jrun --cd <target>                      Resolve directory and print path

Options:
  -h, --help                            Print help message
  -v, --version                         Print version information
  -d, --debug                           Enable debug logging
  -q, --quiet                           Suppress non-essential messages
  -i, --interactive                     Always prompt with TUI selector
  -y, --yes                             Skip confirmation for destructive commands

Database & Maintenance Commands:
  jrun add <path>                       Add or update directory in database
  jrun remove <path>                    Remove directory from database
  jrun list                             List tracked directories with frecency scores
  jrun query <target>                   Query matches and display ranking scores
  jrun prune                            Remove non-existent directories from database
  jrun doctor                           Run environment, database, and root diagnostics

Configuration Commands:
  jrun config                           Open interactive config TUI (reads/writes TOML)
  jrun config show                      Print configuration as TOML
  jrun config edit                      Same as `jrun config`
  jrun root list                        List configured search roots
  jrun root add <path>                  Add a filesystem search root
  jrun root remove <path>               Remove a filesystem search root

Shell Integration:
  jrun --init <bash|zsh|fish>           Generate shell integration script
```

---

## Configuration

Configuration is stored at `~/.config/jrun/config.toml`:

```toml
[search]
roots = [
    "~/development",
    "~/Documents",
    "~/projects",
    "~/dotfiles"
]
max_depth = 4
follow_symlinks = false

[behavior]
fuzzy = true
interactive = true
frecency_threshold = 2.00

[safety]
# Prompt before running these commands in a resolved directory.
# Disable with confirm = false, or skip once with -y / --yes.
confirm = true
commands = [
    "rm",
    "rmdir",
    "mv",
    "unlink",
    "shred",
    "dd",
    "chmod",
    "chown"
]
```

You can edit the same file in a TUI (it loads and saves this TOML, it does not replace it):

```bash
jrun config          # interactive editor
jrun config show     # print TOML to stdout
```

Keys: ↑↓ move, Enter/Space toggle or edit, `a` add root/command, `d` delete, `+/-` adjust numbers, Ctrl-S save, `q` quit.

Or change roots from the CLI:

```bash
jrun root list
jrun root add ~/work
jrun root remove ~/Documents
```

Destructive commands listed under `[safety]` (for example `rm` and `mv`) show a confirmation prompt before they run. Use `-y` / `--yes` to skip a prompt, or set `confirm = false` to disable them.

---

## Architecture

The project is designed with a clean, modular POSIX C architecture:

```text
jrun/
├── src/
│   ├── main.c           # CLI entry point and orchestration
│   ├── cli.h / .c       # Argument parsing and subcommands
│   ├── config.h / .c    # TOML configuration parser and manager
│   ├── database.h / .c  # SQLite persistent frecency database
│   ├── scanner.h / .c   # Filesystem root scanner with cycle prevention
│   ├── matcher.h / .c   # Fuzzy subsequence and exact path matcher
│   ├── resolver.h / .c  # Unified candidate ranking and disambiguation
│   ├── tui.h / .c       # ANSI escape & termios raw-mode TUI selector
│   ├── executor.h / .c  # Safe POSIX fork/chdir/execvp process runner
│   ├── shell.h / .c     # Shell integration code generators
│   ├── path_util.h / .c # Path normalization and tilde utilities
│   └── common.h         # Common definitions and logging macros
├── tests/
│   └── test_main.c      # Comprehensive unit & integration test suite
├── docs/
│   └── SRS.md           # Requirements specification
├── Makefile             # Standard build system
├── nob.c                # C build script (Tsoding nob style)
├── README.md
└── LICENSE
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
