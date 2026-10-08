# jrun

Fast, project-aware directory jumper and arbitrary command launcher for Linux. Written in **C11**.

Combines frecency-based directory jumping (like `zoxide`) with a cached project index and instant command execution—without changing your shell directory or cluttering history.

```bash
make && sudo make install          # build & install
eval "$(jrun --init bash)"         # enable 'j' in your shell (or zsh / fish)

j api                              # jump to project (like zoxide)
jrun api nvim                      # launch Neovim in api without cd-ing
jrun @ make test                   # run command at the project root
```

---

## Why jrun? (vs zoxide)

`zoxide` is a great `cd` replacement, but modern terminal workflows require running commands across projects, finding unvisited repositories, and managing sessions. `jrun` is designed as a superset:

| Feature | `zoxide` | `jrun` |
| :--- | :---: | :---: |
| **Directory jumping** (`j <target>`) | Yes | Yes (drop-in replacement) |
| **Run command in directory** (`jrun <target> <cmd>`) | No *(requires `cd` first)* | **Yes** (atomic `fork()`/`execvp()`) |
| **Discover unvisited projects** | No *(history only)* | **Yes** (cached search roots) |
| **Project marker awareness** (`.git`, `Cargo.toml`, etc.) | No | **Yes** (boosted rank & badges) |
| **Climb to project root** (`@`, `@/docs`) | No | **Yes** (nearest repo/marker root) |
| **Interactive TUI selector** | Needs `fzf` | **Built-in** (`/dev/tty` fuzzy selector) |
| **Bookmarks / Pinning** (`jrun mark`) | No | **Yes** (instant exact priority) |
| **Multi-directory batch execution** (`-a`, `-m`) | No | **Yes** (run across matching projects) |
| **tmux session switcher** (`-s`) | No | **Yes** (create/switch project session) |
| **Destructive command safety** (`rm`, `mv`, `dd`) | No | **Yes** (interactive confirmation prompt) |
| **Runtime & Dependencies** | Rust binary | **Pure C11**, SQLite, <5 MB RAM |

---

## Performance

Tested on Linux x86_64 across 50,000+ directories:

| Metric | Result |
| :--- | :--- |
| **Resolve & execute command** | **< 1.0 ms** (p50) |
| **Directory lookup (cached index)** | **~2.5 ms** |
| **Process memory RSS** | **~2–5 MB** |
| **Background daemon** | **None** (zero daemon overhead) |
| **Cold binary startup** | **< 1.5 ms** |

---

## Common Workflows

### 1. Execute Anywhere (No `cd` Required)
Run any command inside a resolved project directory without leaving your current folder:
```bash
jrun hypr nvim                  # open Neovim in your Hyprland config
jrun constituent git status     # check git status in constituent
jrun api -- npm test            # '--' explicitly separates target and command
```

### 2. Jump & Path Navigation
```bash
j constituent                   # jump to top frecency match
j dev/api                       # match path parts in order (~/.../dev/.../api)
j @                             # jump to root of current git/project repo
j @/docs                        # jump to docs/ inside the current project root
j -                             # jump to previous directory
```

### 3. Interactive TUI Selector
When multiple directories match, an interactive fuzzy selector appears on `/dev/tty`:
```text
┌─ Jump to ──────────────────────────────────────────────────── 3/3 ┐
│ ❯ constituent                                                     │
├───────────────────────────────────────────────────────────────────┤
│❯ ~/development/Mereb/constituent                    rust    118.4 │
│  ~/Documents/constituent                             git     92.1 │
│  ~/projects/constituent                                      80.0 │
├───────────────────────────────────────────────────────────────────┤
│ ↑↓ / ^j^k move · ⏎ select · ⇥ mark · ^W word · ^U clear · esc cancel │
└───────────────────────────────────────────────────────────────────┘
```
- **Navigation:** `↑` / `↓`, `Ctrl-J` / `Ctrl-K`, `Ctrl-N` / `Ctrl-P`
- **Filtering:** Live fuzzy filtering with character highlighting
- **Actions:** `Enter` to select, `Tab` to mark multiple rows (`-m`), `Esc` to cancel

### 4. Bookmarks (Pin Named Directories)
```bash
jrun mark api                   # bookmark current directory as "api"
jrun mark work ~/work/core/api  # bookmark a specific path
jrun mark                       # list bookmarks
jrun unmark api                 # remove bookmark
j api                           # instantly jumps to bookmark (bypasses fuzzy match)
```

### 5. Multi-Directory Runs & tmux Sessions
```bash
jrun -a constituent git pull    # run in every directory named exactly "constituent"
jrun -m proj npm test           # pick directories interactively (Tab marks, Enter runs)
jrun -s api                     # open or switch to a tmux session in api
```

---

## Installation & Setup

### Requirements
- C11 compiler (`gcc` or `clang`)
- `make`
- `libsqlite3` (system SQLite library; vendored header fallback included)

### Build & Install
```bash
make                  # optimized build
make test             # run test suite (or: make asan)
sudo make install     # install binary to /usr/local/bin
```
*Alternative build with [Nob](nob.c): `cc -o nob nob.c && ./nob && sudo cp build/jrun /usr/local/bin/`*

### Shell Integration
Add to your shell configuration:

```bash
# ~/.bashrc
eval "$(jrun --init bash)"

# ~/.zshrc (place after compinit)
eval "$(jrun --init zsh)"

# ~/.config/fish/config.fish
jrun --init fish | source
```

**Tab completion:** The first argument completes directory names (`jrun pol<Tab>`). Trailing arguments complete commands and arguments *inside* that target directory (`jrun polif git checkout <Tab>`).

---

## How It Works

- **Dual-Source Ranking:** Combines SQLite frecency (access frequency + recency decay) with a cached filesystem scan. Project markers (`.git`, `Cargo.toml`, `package.json`, `go.mod`, etc.) receive rank boosts and visual badges.
- **Safe Process Spawning:** Spawns commands via POSIX `fork()`, `chdir()`, and `execvp()`. Arguments pass through untouched without shell concatenation or `system()`. Destructive commands (`rm`, `mv`, `dd`) prompt for confirmation by default.
- **Direct `/dev/tty` TUI:** Selector renders directly to the terminal device, preserving clean stdout for shell redirection (`dir=$(jrun constituent)`).

---

## Configuration

Stored at `~/.config/jrun/config.toml` (or `$XDG_CONFIG_HOME/jrun/config.toml`):

```toml
[search]
roots = ["~/development", "~/projects", "~/dotfiles"]
ignore = [".git", "node_modules", "__pycache__", ".cache", "vendor"]
max_depth = 4
cache_ttl = 300                 # seconds directory cache remains valid (0 = rescan every time)

[behavior]
fuzzy = true
interactive = true
prefer_projects = true          # boost directories holding project markers
frecency_threshold = 2.00

[safety]
confirm = true                  # prompt before destructive commands
commands = ["rm", "rmdir", "mv", "unlink", "shred", "dd", "chmod", "chown"]
```

Manage search roots and settings via CLI or interactive editor:
```bash
jrun config                     # open interactive configuration TUI
jrun root add ~/work            # add search root
jrun root list                  # list search roots
jrun reindex                    # rebuild cached directory index immediately
jrun doctor                     # run diagnostic checks on database and roots
```

---

## CLI Options

```text
Usage:
  jrun <target> <command> [args...]     Execute command in resolved directory
  jrun <target> -- <command> [args...]  Unambiguous target and command syntax
  jrun <target>                         Resolve directory and print path
  jrun --cd <target>                    Same, explicitly

Options:
  -i, --interactive   Always prompt with TUI selector
  -s, --session       Open or switch to a tmux session in the directory
  -a, --all           Run command in all directories with exact matching name
  -m, --multi         Interactively pick directories to run command in (Tab marks)
  -y, --yes           Skip confirmation for destructive commands
  -q, --quiet         Suppress non-essential messages
  -d, --debug         Enable debug logging
  -h, --help          Print help
  -v, --version       Print version
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
