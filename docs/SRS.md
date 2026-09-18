Build a Linux CLI utility in **C** that combines the directory-jumping behavior of **zoxide** with a general-purpose project/folder resolver and command launcher.

The project should be designed as a real, polished, production-quality open-source CLI—not as a simple shell script.

## 1. Core idea

The program should remember and intelligently resolve directories, similar to zoxide, but it should additionally allow the user to execute arbitrary commands against the resolved directory.

The basic concept is:

```text
<program> <target> <command> [arguments...]
```

Examples:

```bash
jrun hypr nvim
jrun waybar nvim
jrun constituent nvim
jrun hypr grep "blur_passes"
jrun constituent git status
jrun OSTA_PMS nvim
```

The program resolves `hypr`, `constituent`, etc. to a directory and then executes the requested command with that directory as the working directory or target argument, depending on the command.

The program should also provide normal directory-jumping functionality comparable to zoxide.

---

# 2. Zoxide-like functionality

Implement the important concepts of zoxide:

### Directory database

Maintain a persistent database of directories the user visits.

The database should contain information such as:

```text
path
frequency
last_access_time
```

Use this information to rank directories.

For example:

```bash
jrun constituent
```

should search the database for matching directories and rank them intelligently.

The ranking system should consider:

* frequency
* recency
* fuzzy matching quality
* exact name/path matches

The implementation does NOT need to reproduce zoxide's exact internal ranking algorithm. The goal is equivalent user-facing behavior.

### Automatic directory tracking

Provide shell integration so that visited directories can automatically be added/updated in the database.

Support at least:

```text
bash
zsh
fish
```

if practical.

The integration should be optional and clearly documented.

---

# 3. Project/folder search

In addition to the directory database, support configured filesystem roots.

For example:

```text
~/development
~/Documents
~/projects
~/dotfiles
~/work
```

Configuration should be stored in a user configuration file, for example:

```text
~/.config/jrun/config.toml
```

or another simple format appropriate for a C implementation.

Example:

```toml
roots = [
    "~/development",
    "~/Documents",
    "~/projects",
    "~/dotfiles"
]
```

The program should recursively search these locations when resolving a target.

Do not blindly scan the entire home directory on every invocation.

Consider caching/indexing to keep searches fast.

---

# 4. Duplicate project names

This is one of the most important features.

The same directory/project name may exist in multiple locations:

```text
~/Documents/constituent
~/development/Mereb/constituent
~/projects/constituent
```

Running:

```bash
jrun constituent nvim
```

must NOT arbitrarily select one.

If there is a single high-confidence match, launch it directly.

If multiple plausible matches exist, show an interactive TUI.

Example:

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

The TUI should support:

* arrow-key navigation
* Enter to select
* Escape to cancel
* fuzzy filtering while typing
* displaying the full path
* displaying useful ranking information when appropriate

Use **ncurses** or another lightweight C-compatible terminal UI library.

---

# 5. Arbitrary command execution

The second part of the command should not be restricted to predefined commands.

Examples:

```bash
jrun hypr nvim
jrun hypr vim
jrun hypr grep "blur_passes"
jrun hypr ls
jrun hypr git status
jrun constituent npm test
jrun constituent python script.py
```

The program should safely parse:

```text
jrun <target> <command> [args...]
```

and execute the command with the selected directory as the working directory.

Use POSIX process APIs such as:

```c
fork()
execvp()
waitpid()
chdir()
```

where appropriate.

Do NOT implement command execution using unsafe shell string concatenation such as:

```c
system("...");
```

unless there is a very specific reason.

Arguments must be passed safely without unnecessary shell interpretation.

---

# 6. Directory mode

There must also be a mode that behaves like a traditional directory jumper.

For example:

```bash
jrun constituent
```

should allow the shell to change into the selected directory.

Since a child process cannot change the parent shell's working directory, provide shell integration.

For example, the shell integration could expose:

```bash
j constituent
```

which changes the current shell directory.

Possible architecture:

```text
jrun
jrun --cd <target>
```

with a shell wrapper that evaluates the resulting directory.

Design this carefully and document the security implications of shell evaluation.

---

# 7. Command syntax

Support a clear syntax.

Primary form:

```bash
jrun <target> <command> [arguments...]
```

Examples:

```bash
jrun hypr nvim
jrun hypr grep "blur_passes"
jrun constituent git status
```

Also support directory-jump mode:

```bash
jrun <target>
```

or:

```bash
jrun cd <target>
```

Choose the syntax that produces the cleanest UX, but document it clearly.

Consider supporting:

```bash
jrun <target> -- <command> [arguments...]
```

to make the boundary between the target and command completely unambiguous.

For example:

```bash
jrun hypr -- nvim
jrun constituent -- git status
```

The normal shorthand should still be convenient.

---

# 8. Fuzzy matching

Implement fuzzy directory matching.

For example:

```bash
jrun const
```

should potentially match:

```text
~/development/Mereb/constituent
~/Documents/constituent-api
```

Matching should consider:

* exact directory name
* path components
* partial names
* character subsequences
* recent/frequent directories

Give exact and high-confidence matches priority.

Do not make matching so aggressive that unrelated directories are constantly selected.

---

# 9. Fast search

Performance is important.

The tool should feel instantaneous for normal usage.

Do not recursively scan thousands of files/directories every time if a persistent database/index can solve the problem.

Use a combination of:

1. directory database
2. configured search roots
3. filesystem scanning only when necessary
4. caching/indexing where useful

Avoid following symlink loops.

Handle:

* inaccessible directories
* deleted directories
* broken symlinks
* permission errors
* very large directory trees

gracefully.

---

# 10. Command working-directory behavior

For:

```bash
jrun hypr nvim
```

the expected behavior is effectively:

```bash
cd ~/dotfiles/hypr/.config/hypr
nvim
```

For:

```bash
jrun constituent git status
```

the expected behavior is:

```bash
cd ~/development/Mereb/constituent
git status
```

Do not automatically append the directory path as an argument unless the command specifically requires it.

The selected directory should normally become the process's working directory.

This distinction is important.

---

# 11. Configuration

Provide a simple configuration system.

Example:

```toml
[search]
roots = [
    "~/development",
    "~/Documents",
    "~/projects",
    "~/dotfiles"
]

[behavior]
fuzzy = true
interactive = true
follow_symlinks = false
```

Allow users to add/remove search roots.

Potential commands:

```bash
jrun config
jrun root add ~/work
jrun root remove ~/Documents
jrun root list
```

Keep the initial implementation simple; these can be expanded later.

---

# 12. Database

Use a lightweight embedded database.

SQLite is preferred unless there is a strong technical reason not to use it.

Potential schema:

```sql
CREATE TABLE directories (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL,
    frequency INTEGER NOT NULL DEFAULT 1,
    last_access INTEGER NOT NULL
);
```

Add appropriate indexes.

Automatically remove or ignore paths that no longer exist.

Do not allow the database to grow indefinitely without cleanup.

---

# 13. TUI behavior

The TUI should only appear when it is useful.

### One clear match

```bash
jrun hypr nvim
```

→ execute immediately.

### Multiple plausible matches

```bash
jrun constituent nvim
```

→ show selector.

### No match

Display a useful error:

```text
No directory found for: constituent

Searched:
  ~/development
  ~/Documents
  ~/projects
  ~/dotfiles
```

Do not open a TUI unnecessarily when there is only one obvious match.

---

# 14. CLI options

Implement conventional options such as:

```text
-h, --help
-v, --version
-d, --debug
-q, --quiet
-i, --interactive
```

Potential additional commands:

```bash
jrun list
jrun query constituent
jrun remove constituent
jrun prune
jrun doctor
```

These can expose and maintain the directory database.

---

# 15. Security

Treat command execution and shell integration carefully.

Requirements:

* do not blindly construct shell commands
* avoid `system()` where possible
* use `execvp()`/related APIs
* preserve argument boundaries
* handle paths containing spaces
* handle quotes correctly
* do not execute arbitrary database contents as shell code
* do not evaluate untrusted directory paths
* clearly document shell integration behavior

---

# 16. Project structure

Use a clean modular C architecture.

Suggested structure:

```text
jrun/
├── src/
│   ├── main.c
│   ├── cli.c
│   ├── cli.h
│   ├── config.c
│   ├── config.h
│   ├── database.c
│   ├── database.h
│   ├── scanner.c
│   ├── scanner.h
│   ├── matcher.c
│   ├── matcher.h
│   ├── resolver.c
│   ├── resolver.h
│   ├── tui.c
│   ├── tui.h
│   ├── executor.c
│   ├── executor.h
│   ├── shell.c
│   └── shell.h
│
├── tests/
├── docs/
├── Makefile
├── README.md
└── LICENSE
```

Keep modules small and independently testable.

---

# 17. Build system

Use a straightforward build system.

Start with:

```text
Makefile
```

Support:

```bash
make
make debug
make release
make test
make clean
sudo make install
```

Compile with strong warnings:

```text
-Wall
-Wextra
-Wpedantic
-Werror
```

where practical.

Use a reasonable C standard such as:

```text
C11
```

or newer if required.

---

# 18. Testing

Write tests for:

* exact matching
* fuzzy matching
* duplicate directory names
* ranking
* deleted directories
* inaccessible directories
* paths containing spaces
* nested paths
* command arguments
* database operations
* configuration parsing
* shell integration
* TUI selection logic

Separate filesystem/database/matching logic from the UI so it can be unit tested without an interactive terminal.

---

# 19. UX principles

The tool should feel like a native Linux utility.

Normal usage should be extremely short:

```bash
jrun hypr nvim
```

should be enough.

Avoid excessive output.

Successful non-interactive execution should generally produce no unnecessary messages.

Errors should be concise and useful.

The TUI should be keyboard-first.

---

# 20. Future extensibility

Design the architecture so these can be added later without rewriting the core:

* aliases
* project metadata
* Git repository detection
* project type detection
* command aliases
* directory bookmarks
* shell completions
* Bash/Zsh/Fish integration
* configurable ranking
* fzf integration
* background indexing
* daemon/indexer mode
* recent projects
* project tags
* opening projects in IDEs
* environment-specific commands

For example, eventually users could configure:

```toml
[commands]
edit = "nvim"
code = "code ."
shell = "bash"
```

and use:

```bash
jrun hypr edit
jrun constituent code
```

but do not over-engineer the first version.

---

# 21. Initial implementation priority

Build the project incrementally.

### Phase 1

Implement:

```text
CLI argument parsing
filesystem search
exact matching
directory resolution
safe command execution
```

### Phase 2

Implement:

```text
SQLite database
frequency
recency
ranking
automatic directory tracking
```

### Phase 3

Implement:

```text
ncurses TUI
multiple-match selection
fuzzy filtering
```

### Phase 4

Implement:

```text
configuration
search roots
shell integration
```

### Phase 5

Implement:

```text
tests
shell completions
performance improvements
cleanup/pruning
documentation
```

Do not attempt to implement every feature at once.

---

# 22. Most important expected behavior

After installation, I want to be able to do:

```bash
jrun hypr nvim
```

and have it find my Hyprland configuration/project automatically.

If there are multiple `hypr` directories:

```text
~/dotfiles/hypr
~/Documents/hypr
~/development/hypr
```

I want a TUI to choose between them.

Then:

```bash
jrun hypr grep "blur_passes"
```

should run `grep` from the selected directory.

And:

```bash
jrun hypr
```

should behave as a directory jumper.

The final product should combine:

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

The most important goal is **fast, predictable, keyboard-driven directory/project resolution with optional interactive disambiguation and arbitrary command execution**.

Before writing large amounts of code, first inspect the repository, identify the platform/dependency constraints, propose the implementation plan and file structure, and then implement the project incrementally. Do not blindly copy zoxide's implementation; use its user-facing behavior as inspiration while designing an independent C implementation.
