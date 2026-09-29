# jrun

A fast, keyboard-driven Linux CLI utility written in **C** that combines frecency-based
directory jumping (like `zoxide`) with a project resolver and an arbitrary command launcher.

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

- **Arbitrary command execution** — resolves a project directory and runs any command with it
  as the working directory (`jrun hypr nvim`, `jrun constituent git status`).
- **Safe process spawning** — POSIX `fork()`, `chdir()` and `execvp()`. Arguments are passed
  through untouched; there is no shell concatenation and no `system()`.
- **Frecency database** — SQLite at `~/.local/share/jrun/jrun.db`, combining access frequency,
  recency decay and match quality. Visiting a directory can only ever raise its rank.
- **Cached directory index** — the filesystem walk is cached in SQLite and replayed until it
  goes stale, so a lookup does not pay for a full scan every time. Changing any search setting
  invalidates it immediately.
- **Project awareness** — directories holding a `.git`, `Cargo.toml`, `package.json`, `go.mod`
  and similar marker are ranked higher and labelled in the selector.
- **Interactive selector** — when several directories match, a terminal UI appears with live
  fuzzy filtering, match highlighting and arrow navigation. It draws on `/dev/tty`, so it works
  even when jrun's stdout is captured by the shell integration.
- **Directory jumper and shell integration** — a `j` function for Bash, Zsh and Fish with
  automatic directory tracking.
- **Light dependencies** — C11, POSIX, pthreads and the system SQLite library.

---

## Installation & Build

### Requirements

- A C11 compiler (`gcc` or `clang`)
- `make`
- The system SQLite library (`libsqlite3`). If your distribution also ships the development
  headers (`libsqlite3-dev`, `sqlite-devel`) they are used; otherwise jrun falls back to the
  copy of `sqlite3.h` vendored in `thirdparty/sqlite3/` and checks at runtime that the
  installed library is not older than that header.

### Building with Make

```bash
make                  # optimized build into build/jrun
make debug            # unoptimized, with debug symbols
make test             # build and run the test suite
make asan             # run the test suite under AddressSanitizer + UBSan
make check            # both of the above
make help             # list every target

make && sudo make install
```

Build as your normal user and install as root, as shown above. `make install` deliberately
does not build, so `sudo make install` cannot leave you with a root-owned `build/` directory.

### Building with Nob

```bash
cc -o nob nob.c
./nob -test
sudo cp build/jrun /usr/local/bin/
```

---

## Quick Start & Usage

### 1. Launching commands in projects

```bash
jrun hypr nvim                  # open Neovim in your Hyprland config
jrun hypr grep "blur_passes"    # grep inside it
jrun constituent git status     # git status in another project

jrun hypr -- nvim               # '--' separates target from command explicitly
jrun constituent -- npm test
```

### 2. Disambiguating duplicate directories

If a target matches several directories:

```text
~/Documents/constituent
~/development/Mereb/constituent
~/projects/constituent
```

`jrun constituent nvim` opens the selector:

```text
┌─ Jump to ──────────────────────────────────────────────────── 3/3 ┐
│ ❯ constituent                                                     │
├───────────────────────────────────────────────────────────────────┤
│❯ ~/development/Mereb/constituent                    rust    118.4 │
│  ~/Documents/constituent                             git     92.1 │
│  ~/projects/constituent                                      80.0 │
├───────────────────────────────────────────────────────────────────┤
│ ↑↓ move · ⏎ open · ^W word · ^U clear · esc cancel                │
└───────────────────────────────────────────────────────────────────┘
```

| Key                            | Action                                          |
| ------------------------------ | ----------------------------------------------- |
| `↑` `↓`, `Ctrl-P` `Ctrl-N`     | Move through candidates (wraps)                 |
| `PgUp` `PgDn`, `Home` `End`    | Jump a page, or to the ends                     |
| type                           | Filter live; matched characters are highlighted |
| `←` `→`, `Ctrl-A` `Ctrl-E`     | Move the cursor within the query                |
| `Ctrl-W` / `Ctrl-U` / `Ctrl-K` | Delete a word / clear / kill to end             |
| `Ctrl-L`                       | Redraw                                          |
| `Enter`                        | Select                                          |
| `Esc`, `Ctrl-C`                | Cancel (exit status 130)                        |

The column on the right shows the ranking score; a label such as `rust` or `git` marks a
directory holding a project file. Both are dropped automatically on narrow terminals.

### 3. Directory jumper mode

```bash
jrun constituent        # print the resolved path
jrun --cd constituent   # same thing, explicitly
```

---

## Shell Integration

```bash
# ~/.bashrc
eval "$(jrun --init bash)"

# ~/.zshrc
eval "$(jrun --init zsh)"

# ~/.config/fish/config.fish
jrun --init fish | source
```

This defines a `j` function and a hook that records directories as you move between them.
The hook fires only when the working directory actually changed.

```bash
j constituent       # cd into constituent
j -                 # cd to the previous directory
j                   # cd to $HOME
j hypr nvim         # run a command there
```

`jrun` itself is left alone — it stays the binary, so `jrun <target>` still prints a path
rather than changing directory. Only `j` is a shell function.

---

## CLI Options & Commands

```text
Usage:
  jrun <target> <command> [args...]     Execute command in the resolved directory
  jrun <target> -- <command> [args...]  Unambiguous target and command syntax
  jrun <target>                         Resolve the directory and print the path
  jrun --cd <target>                    Same, explicitly

Options:
  -h, --help          Print help
  -v, --version       Print version
  -d, --debug         Enable debug logging
  -q, --quiet         Suppress non-essential messages
  -i, --interactive   Always show the selector
  -y, --yes           Skip confirmation for destructive commands

Database & maintenance:
  jrun add <path>     Add or update a directory in the database
  jrun remove <path>  Remove a directory from the database
  jrun list           List tracked directories with frecency scores
  jrun query <target> Show matches and their ranking scores
  jrun prune          Drop directories that no longer exist
  jrun reindex        Rebuild the cached directory index now
  jrun doctor         Report environment, database, index and root diagnostics

Configuration:
  jrun config         Open the configuration editor
  jrun config show    Print the configuration as TOML
  jrun root list      List search roots
  jrun root add <path>
  jrun root remove <path>

Shell integration:
  jrun --init <bash|zsh|fish>
```

---

## Configuration

Stored at `~/.config/jrun/config.toml` (or `$XDG_CONFIG_HOME/jrun/config.toml`).

```toml
[search]
roots = [
    "~/development",
    "~/Documents",
    "~/projects",
    "~/dotfiles",
]

# Directory names never descended into.
ignore = [
    ".git",
    "node_modules",
    "__pycache__",
    ".cache",
    "vendor",
]
max_depth = 4
follow_symlinks = false
# Seconds a cached directory index stays usable. 0 rescans every time.
cache_ttl = 300

[behavior]
fuzzy = true
interactive = true
# Rank directories holding a project marker (.git, Cargo.toml, ...) higher.
prefer_projects = true
frecency_threshold = 2.00

[safety]
# Prompt before running these commands in a resolved directory.
confirm = true
commands = ["rm", "rmdir", "mv", "unlink", "shred", "dd", "chmod", "chown"]
```

Keys are read from their own section. Values are written with proper TOML escaping, and the
file is replaced atomically, so a crash mid-write cannot leave you with a truncated config.

### The directory index

A lookup matches against two sources: the frecency database, and an index of every directory
under your search roots. Building that index means walking the filesystem, so the result is
cached in SQLite and reused for `cache_ttl` seconds.

The trade-off: a directory created after the index was built is invisible until the cache
expires. Run `jrun reindex` when you want it immediately, or set `cache_ttl = 0` to scan on
every lookup. Any change to `roots`, `ignore`, `max_depth` or `follow_symlinks` invalidates
the cache straight away rather than waiting for the TTL.

### Editing configuration interactively

```bash
jrun config          # editor
jrun config show     # print to stdout
```

Keys: `↑↓` move, `Enter`/`Space` toggle or edit, `a` add, `d` delete, `←→` adjust numbers,
`Ctrl-S` save, `q` quit. The line above the footer explains whatever is selected.

Or from the command line:

```bash
jrun root list
jrun root add ~/work
jrun root remove ~/Documents
```

### Destructive commands

Commands listed under `[safety]` prompt for confirmation before running. The prompt defaults
to **No**, and declining leaves no trace: nothing runs, and no frecency is recorded. Use
`-y`/`--yes` to skip a prompt, or `confirm = false` to disable them. Without a terminal the
command is refused rather than run unattended.

---

## Architecture

```text
jrun/
├── src/
│   ├── main.c           # CLI entry point and orchestration
│   ├── cli.h / .c       # Argument parsing and subcommands
│   ├── config.h / .c    # TOML configuration parser and writer
│   ├── database.h / .c  # SQLite frecency store and cached directory index
│   ├── scanner.h / .c   # Threaded filesystem walk with cycle prevention
│   ├── matcher.h / .c   # Exact, prefix, acronym and fuzzy matching
│   ├── resolver.h / .c  # Candidate gathering, ranking and disambiguation
│   ├── tui.h / .c       # /dev/tty terminal UI: selector, confirm, config editor
│   ├── executor.h / .c  # fork/chdir/execvp process runner
│   ├── shell.h / .c     # Shell integration generators
│   ├── path_util.h / .c # Path normalization, project markers, atomic writes
│   └── common.h         # Shared definitions and logging macros
├── tests/
│   └── test_main.c      # Unit and integration tests (sandboxed per run)
├── .github/workflows/   # CI: gcc/clang, release/debug, sanitizers, shell syntax
├── Makefile
├── nob.c                # Alternative C build script
└── README.md
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
