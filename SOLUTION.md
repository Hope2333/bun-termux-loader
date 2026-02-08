# Running Bun Bundled Binaries on Termux — Technical Details

## The Problem

Termux uses **Bionic** (Android's libc), but Bun is compiled against **glibc**. The `glibc-runner` package (`grun`) allows running glibc binaries:

```bash
grun ./my-glibc-binary
# Equivalent to:
/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1 ./my-glibc-binary
```

This works for most binaries, but **fails for Bun bundled executables**.

### Why Bun Bundled Binaries Fail

Bun's `bun build --compile` creates a single executable by appending JavaScript bytecode to the Bun runtime:

```
┌─────────────────────────────┐
│      Bun ELF Binary         │
├─────────────────────────────┤
│   Embedded JS Bytecode      │
├─────────────────────────────┤
│   Metadata + Offsets        │
├─────────────────────────────┤
│   "---- Bun! ----" marker   │
├─────────────────────────────┤
│   8-byte file size          │
└─────────────────────────────┘
```

At runtime, Bun detects embedded JavaScript by:
1. Reading `/proc/self/exe` to get its own path
2. Opening that file and seeking to the end
3. Looking for the `---- Bun! ----` magic marker
4. Reading offsets to find and execute the embedded bytecode

**The `/proc/self/exe` Problem:**

When you run via `grun`, the kernel sets `/proc/self/exe` based on `execve()`:

```c
execve("/path/to/ld-linux-aarch64.so.1", ["ld.so", "./my-app"], environ);
```

Result: `/proc/self/exe` points to **ld.so**, not the binary:

```bash
$ grun ./my-app -e "console.log(require('fs').readlinkSync('/proc/self/exe'))"
/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1
```

Bun reads `/proc/self/exe`, opens `ld.so`, doesn't find the marker, and falls back to CLI mode.

## The Solution: Self-Contained Userland Exec

### Overview

The solution combines two techniques:

1. **Userland exec** — load glibc's `ld.so` via `mmap()` instead of `execve()`, keeping `/proc/self/exe` unchanged
2. **Embedded extraction with caching** — the Bun ELF runtime is embedded inside the wrapper binary and extracted to a cache file on first run

The wrapper is written in **plain C** (~200 lines, zero dependencies, 11 KB compiled).

### Binary Layout

```
┌─────────────────────────────────────┐
│  Wrapper ELF (Bionic-linked C)      │  Android's linker loads this (~11 KB)
├─────────────────────────────────────┤
│  "BUNWRAP1" magic (8 bytes)         │  Metadata marker
│  bun_elf_size (8 bytes LE)          │  Size of embedded Bun ELF
├─────────────────────────────────────┤
│  Bun ELF binary (~92 MB)            │  Full glibc-linked Bun runtime
├─────────────────────────────────────┤
│  "BUNLIBS1" block (optional)        │  Native libs + shim, auto-extracted
├─────────────────────────────────────┤
│  Embedded JS bytecode               │  From original Bun bundled binary
├─────────────────────────────────────┤
│  Bun metadata + offsets             │  Bun's internal format
├─────────────────────────────────────┤
│  "---- Bun! ----" marker            │  Bun looks for this at EOF
├─────────────────────────────────────┤
│  8-byte file size (patched)         │  Patched to match total file size
└─────────────────────────────────────┘
```

### How Userland Exec Works

Instead of:
```
Process calls execve(ld.so) → Kernel loads ld.so → /proc/self/exe = ld.so ❌
```

We do:
```
Process manually loads ld.so via mmap() → jumps to entry point → /proc/self/exe = our binary ✓
```

The wrapper does this in C:

1. **Parse ld.so's ELF header** to find `PT_LOAD` segments
2. **Reserve contiguous memory** with `mmap(PROT_NONE)` for the full virtual address range
3. **Map each segment** with `mmap(MAP_FIXED)` using the correct permissions (R/W/X)
4. **Zero BSS sections** (where `p_memsz > p_filesz`)
5. **Build a new stack** with argc, argv, envp, and the auxiliary vector
6. **Jump to ld.so's entry point** with inline assembly:

```c
__asm__ volatile(
    "mov sp, %[sp]\n"
    "mov x0, sp\n"
    "br  %[entry]\n"
    : : [sp] "r"(sp), [entry] "r"(entry)
    : "x0","x1","x2","x3","x4","x5","x30","memory"
);
```

The process image is replaced (like `execve()`), but the kernel is never notified, so `/proc/self/exe` remains unchanged.

### Execution Flow

```
User runs ./my-app-termux
        │
        ▼
Android's linker64 loads wrapper ELF (Bionic binary)
        │
        ▼
Wrapper reads /proc/self/exe (its own binary)
        │
        ▼
Parses own ELF headers → finds wrapper ELF end
        │
        ▼
Reads BUNWRAP1 metadata at wrapper ELF end
        │
        ▼
Extracts Bun ELF to $TMPDIR/bun-termux-cache/bun-<hash>
(skipped if cache already exists and size matches)
        │
        ▼
Reads BUNLIBS1 → extracts native libs + shim to bunfs-libs/
(skipped if no BUNLIBS1 block or cache already exists)
        │
        ▼
Userland exec: mmap(ld.so) + --preload shim + jump to entry
        │
        ▼
glibc ld.so loads shim, then loads cached Bun binary
        │
        ▼
Bun calls dlopen("/$bunfs/root/lib.so") → shim rewrites → loads from cache ✓
        │
        ▼
Bun reads /proc/self/exe → opens ./my-app-termux
        │
        ▼
Finds "---- Bun! ----" at EOF → loads embedded JS → runs ✓
```

### Finding the Wrapper's Own ELF End

The wrapper needs to know where its own ELF data ends so it can find the `BUNWRAP1` metadata block. It uses two methods and takes the maximum:

1. **PT_LOAD segments**: `max(p_offset + p_filesz)` for all `PT_LOAD` program headers
2. **Section headers**: `e_shoff + e_shentsize * e_shnum` (if present)

This is robust for stripped binaries (where section headers may be absent) since PT_LOAD segments are always present.

### Stack Layout

The wrapper builds a complete stack for ld.so's entry point:

```
[Higher addresses]
  AT_PLATFORM string ("aarch64")
  AT_RANDOM (16 random bytes)
  argv/envp strings
[Auxiliary vector entries]    ← AT_PHDR, AT_BASE, AT_ENTRY, etc.
[envp pointers + NULL]
[argv pointers + NULL]
[argc]                        ← sp points here (16-byte aligned)
[Lower addresses]
```

Key auxv entries:
- `AT_PHDR` — found via `PT_PHDR` segment (not `e_phoff`, which is a file offset)
- `AT_BASE` — ld.so's load base address
- `AT_ENTRY` — ld.so's entry point
- `AT_HWCAP` / `AT_HWCAP2` — passed through from the current process
- `AT_SYSINFO_EHDR` — vDSO base, passed through
- `AT_RANDOM` — 16 bytes from `/dev/urandom`

### Why Not memfd_create?

The initial approach used `memfd_create()` to create an anonymous in-memory file descriptor and pass `/proc/self/fd/N` to ld.so, avoiding disk I/O entirely.

However, **Android's SELinux policy blocks `mmap(PROT_EXEC)` on memfd file descriptors**:

```
/proc/self/fd/4: error while loading shared libraries:
/proc/self/fd/4: cannot open shared object file: Permission denied
```

The cache-file approach works because cached files are on a real filesystem where exec is permitted.

### Cache System

- **Location**: `$TMPDIR/bun-termux-cache/` (defaults to `/data/data/com.termux/files/usr/tmp/bun-termux-cache/`)
- **Naming**: `bun-<hash>` where hash is FNV-1a of (first 4096 bytes + size)
- **Validation**: checks that cached file exists and size matches `bun_elf_size`
- **Atomic writes**: writes to a `.tmp` file first, then `rename()` (prevents corruption)
- **First run**: extracts ~92 MB (a few seconds)
- **Subsequent runs**: instant (cache hit)

## Native Library Support (bunfs shim)

### The Problem

Some Bun-compiled binaries embed native shared libraries (`.so`, `.node`) inside Bun's virtual filesystem (`/$bunfs/root/`). Bun normally intercepts file operations to this path and serves files from embedded data. However, `dlopen()` goes directly to glibc, which doesn't know about Bun's virtual filesystem:

```
Failed to open library "/$bunfs/root/libopentui-xgx85zdr.so":
  cannot open shared object file: No such file or directory
```

Pure JS binaries (like amp/claude-code) work fine — they don't call `dlopen()` on bunfs paths. But binaries like OpenCode embed native TUI rendering libraries, PTY handlers, and file watcher addons.

### The Solution: LD_PRELOAD Shim

A tiny glibc `.so` (`bunfs_shim.c`, ~40 lines, 4 KB compiled) that intercepts `dlopen()` at the function call level:

```c
void *dlopen(const char *filename, int flags) {
    if (filename starts with "/$bunfs/root/")
        rewrite to "$BUNFS_CACHE_DIR/basename"
    call real dlopen
}
```

- Loaded via ld.so's `--preload` flag (not `LD_PRELOAD` env, which would leak to child processes)
- Only intercepts `dlopen()` — regular file I/O stays handled by Bun's bunfs
- Uses `RTLD_NEXT` + `dlsym` to find the real `dlopen`
- `BUNFS_CACHE_DIR` env var set by the wrapper points to extraction directory
- Zero overhead on non-bunfs `dlopen()` calls (single `strncmp`)

### Why not intercept open()/openat()?

Bun's virtual filesystem handles regular file reads (`open`, `read`, `stat`) for bunfs paths internally. Intercepting those would conflict with Bun's own handling and break `.scm`, `.wasm`, and `.js` files that currently work fine. Only `dlopen()` bypasses Bun's VFS because it goes through glibc's dynamic linker.

### Auto-Detection and Embedding

`build.py` scans the input binary for `/$bunfs/root/*.so` and `/$bunfs/root/*.node` path references, locates the corresponding ELF blobs by signature matching (e.g., `setLogCallback` → libopentui, `napi_register_module` → watcher), and embeds them in a `BUNLIBS1` metadata block:

```
"BUNLIBS1" (8 bytes)
num_libs   (4 bytes LE)
For each lib:
  name_len (2 bytes LE)
  name     (name_len bytes)
  data_len (8 bytes LE)
  data     (data_len bytes)
```

The wrapper reads this block after the Bun ELF, extracts each lib to `$TMPDIR/bun-termux-cache/bunfs-libs/`, and passes `--preload bunfs_shim.so` to ld.so.

### Why not binary-patch the paths?

Termux writable paths (`/data/data/com.termux/files/...`) are 30+ characters. The `/$bunfs/root/` prefix is only 13 characters. Same-length binary patching is impossible when the replacement path is longer than the original.

### Backwards Compatibility

- Binaries without native libs: no BUNLIBS1 block exists, wrapper skips the shim entirely
- The shim is only loaded when BUNLIBS1 metadata is found and extracted
- No behavior change for pure JS binaries

### Critical Implementation Details

#### 1. Use `--library-path` instead of `LD_LIBRARY_PATH`

When Bun spawns child processes (e.g., `bash`, `ls`), those use Termux's Bionic, not glibc. If `LD_LIBRARY_PATH` points to glibc, child processes fail:

```
CANNOT LINK EXECUTABLE "bash": cannot find "libc.so"
```

The wrapper passes glibc path via ld.so's `--library-path` flag, which only affects the initial binary load.

#### 2. Remove `LD_PRELOAD`

Termux sets `LD_PRELOAD` for path translation. This interferes with glibc loading. The wrapper filters it out along with `LD_LIBRARY_PATH`.

#### 3. Patch the File Size

Bun validates the file size stored in the last 8 bytes. After assembling the final binary, build.py patches it with the actual total size.

## Build Process

```bash
# 1. Compile the wrapper (requires clang, ships with Termux)
make

# 2. Compile the bunfs shim (requires glibc cross-compilation toolchain)
#    bunfs_shim.so is compiled against glibc (not Bionic) since it runs
#    inside the glibc-loaded Bun process. Pre-compiled .so is included.
gcc -shared -fPIC -O2 -o bunfs_shim.so bunfs_shim.c -ldl

# 3. Build a self-contained binary
python3 build.py              # uses ./droid → creates ./droid-termux
python3 build.py ./my-app     # creates ./my-app-termux

# 4. Run
./droid-termux [args...]
```

The build script:
1. Reads the input Bun bundled binary
2. Verifies the `---- Bun! ----` marker exists
3. Splits it into Bun ELF (before ELF end) and embedded data (after)
4. Scans embedded data for `/$bunfs/root/*.so` and `*.node` path references
5. Locates native lib ELF blobs by name, signature matching, or elimination
6. Packs found libs + `bunfs_shim.so` into a `BUNLIBS1` block (skipped if no native libs)
7. Concatenates: `wrapper + BUNWRAP1 metadata + bun_elf + BUNLIBS1 block + embedded_data`
8. Patches the last 8 bytes with the total file size

## File Structure

```
bun-termux-loader/
├── wrapper.c        # C wrapper — userland exec + cache extraction
├── bunfs_shim.c     # LD_PRELOAD shim — redirects /$bunfs/root/ dlopen calls
├── Makefile         # Builds wrapper binary (bionic)
├── build.py         # Assembles self-contained binaries, auto-embeds native libs
├── README.md
└── SOLUTION.md
```

## Comparison: grun vs bun-termux

| Aspect | grun | bun-termux |
|--------|------|------------|
| Mechanism | `execve(ld.so, [binary])` | `mmap(ld.so)` + `jmp entry` |
| `/proc/self/exe` | Points to ld.so ❌ | Points to our binary ✓ |
| Bun bundled binaries | Broken | Works ✓ |
| Native lib support | None | Auto-detected + shim ✓ |
| Self-contained | No | Yes (single file) |
| External files needed | Yes (the binary) | No (extracted to cache) |
| First-run overhead | None | ~92 MB cache write |

## Why C?

The wrapper was originally written in Rust using the `userland-execve` crate, which pulled in 8 transitive dependencies (`goblin`, `scroll`, `nix`, `syn`, `quote`, `proc-macro2`, `scroll_derive`, `libc`) for what amounts to a few syscalls.

| | C | Rust |
|---|---|---|
| Binary size | **11 KB** | 355 KB |
| Dependencies | **0** | 8 crates |
| Compile time | **instant** | ~90 seconds |
| Toolchain | clang (ships with Termux) | `pkg install rust` |

The wrapper runs for milliseconds then replaces itself — memory safety benefits don't apply.

## Limitations

1. **Architecture-specific**: Only works on aarch64 (ARM64)
2. **First-run cost**: Cache extraction writes ~92 MB on first run
3. **Cache storage**: Requires ~92 MB in `$TMPDIR` per unique Bun version
4. **Android SELinux**: Cannot use `memfd_create` for zero-disk-IO loading
5. **Native lib signature matching**: `build.py` uses heuristic signature matching to identify embedded ELF blobs (e.g., `setLogCallback` → libopentui, `napi_register_module` → watcher). New or unrecognized native libraries may require adding signatures to the `SIGNATURES` table, though elimination-based matching (any unclaimed shared object ELF) provides a fallback.

## References

- [ELF specification](https://refspecs.linuxfoundation.org/elf/elf.pdf)
- [Bun bundled executable format](https://bun.sh/docs/bundler/executables)
- [Bun Issue #26752](https://github.com/oven-sh/bun/issues/26752) - Request for `BUN_SELF_EXE` env var
- [Bun Issue #8685](https://github.com/oven-sh/bun/issues/8685) - Bun on Termux documentation
