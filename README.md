# bun-termux

Run Bun bundled executables (`bun build --compile`) on Termux/Android as **self-contained single binaries**.

## The Problem

Termux uses Bionic (Android's libc), but Bun is compiled against glibc. The standard workaround is using `grun` (glibc-runner), but this breaks Bun bundled executables because `/proc/self/exe` points to `ld.so` instead of the actual binary.

Bun bundled executables detect their embedded JavaScript by reading `/proc/self/exe` and looking for the `---- Bun! ----` magic trailer. When `/proc/self/exe` points to `ld.so`, Bun can't find its embedded code and falls back to CLI mode.

Additionally, some Bun binaries embed native `.so`/`.node` files in Bun's virtual filesystem (`/$bunfs/root/`). These fail to load via `dlopen()` since the virtual path doesn't exist on the real filesystem.

## The Solution

This tool creates a **self-contained binary** that embeds both the Bun runtime and your JavaScript code. It uses three key techniques:

1. **Userland exec** — loads glibc's `ld.so` via `mmap()` and jumps to it directly, without calling `execve()`. Since the kernel only updates `/proc/self/exe` on `execve()`, it stays pointing to our binary which contains the embedded JavaScript.

2. **Cache-based extraction** — the embedded Bun ELF runtime is extracted to a cache file (`$TMPDIR/bun-termux-cache/`) on first run and reused on subsequent runs. This avoids the Android SELinux restriction that blocks `mmap(PROT_EXEC)` on `memfd_create` fds.

3. **BunFS shim** — a tiny glibc-linked `.so` loaded via `LD_PRELOAD` that intercepts `dlopen()` calls. When it sees `/$bunfs/root/X`, it rewrites to `$BUNFS_CACHE_DIR/X`. Native `.so`/`.node` files are auto-detected by `build.py`, embedded in a `BUNLIBS1` metadata block, and auto-extracted by the wrapper on first run. Binaries without native libs skip this entirely (backwards compatible).

## Installation

```bash
pkg install clang glibc-runner

git clone https://github.com/umairimtiaz9/bun-termux-loader
cd bun-termux-loader
make
```

To build the BunFS shim (only needed for binaries with native libs):

```bash
GLIBC=/data/data/com.termux/files/usr/glibc
clang --target=aarch64-linux-gnu --sysroot=$GLIBC -shared -fPIC -O2 -nostdlib -I$GLIBC/include -L$GLIBC/lib -Wl,--dynamic-linker=$GLIBC/lib/ld-linux-aarch64.so.1 -Wl,-rpath,$GLIBC/lib -o bunfs_shim.so bunfs_shim.c -lc -ldl
```

## Usage

### Bundle a Bun executable for Termux

```bash
# Creates my-app-termux (self-contained, single file)
python3 build.py ./my-app

# Specify output name
python3 build.py ./my-app ./my-app-final

# Custom wrapper binary path
python3 build.py ./my-app --wrapper /path/to/wrapper

# Custom shim path
python3 build.py ./my-app --shim /path/to/bunfs_shim.so

# Run it directly — no other files needed
./my-app-termux --port 3000
```

### build.py options

```
Usage:
  python3 build.py <input> [output] [--wrapper <path>] [--shim <path>]

Arguments:
  input    Input Bun bundled binary (required)
  output   Output binary path (default: <input-name>-termux)

Options:
  --wrapper <path>  Path to wrapper binary (default: ./wrapper)
  --shim <path>     Path to bunfs_shim.so (default: ./bunfs_shim.so)

Native .so/.node libs are auto-detected and embedded.
```

## How It Works

### Binary Layout

```
┌─────────────────────────────────────┐
│  Wrapper ELF (Bionic-linked C)      │  ← Runs first on Android
├─────────────────────────────────────┤
│  Metadata: "BUNWRAP1" + ELF size    │  ← 16 bytes: magic + bun_elf_size
├─────────────────────────────────────┤
│  Bun ELF binary (~92 MB)            │  ← Extracted to cache on first run
├─────────────────────────────────────┤
│  BUNLIBS1 block (optional)          │  ← Native libs + shim, auto-extracted
├─────────────────────────────────────┤
│  Embedded JS bytecode               │  ← Bun finds this via /proc/self/exe
├─────────────────────────────────────┤
│  ---- Bun! ---- marker + metadata   │  ← Bun's trailer stays at EOF
└─────────────────────────────────────┘
```

### Execution Flow

1. User runs `./my-app-termux`
2. Android's linker loads the wrapper (it's a Bionic binary)
3. Wrapper reads its own binary, finds the embedded Bun ELF via `BUNWRAP1` metadata
4. Extracts Bun ELF to cache (`$TMPDIR/bun-termux-cache/bun-<hash>`) if not already cached
5. If `BUNLIBS1` block is present, extracts native libs + shim to `$TMPDIR/bun-termux-cache/bunfs-libs/`
6. Wrapper does userland exec: loads glibc's `ld.so` via `mmap()`, jumps to it
7. glibc `ld.so` loads the cached Bun binary (with `LD_PRELOAD=bunfs_shim.so` if native libs are present)
8. Bun reads `/proc/self/exe` → points to `my-app-termux` → finds `---- Bun! ----` marker → runs embedded JS

### Why not `grun`?

| Aspect | grun | bun-termux |
|--------|------|------------|
| Mechanism | `execve(ld.so, [binary])` | `mmap(ld.so)` + `jmp entry` |
| `/proc/self/exe` | Points to ld.so ❌ | Points to our binary ✓ |
| Bun bundled binaries | Broken | Works ✓ |
| Self-contained | N/A | Single file ✓ |
| Native .so/.node libs | Not handled | Auto-extracted + shimmed ✓ |

### Cache Details

- Location: `$TMPDIR/bun-termux-cache/` (typically `/data/data/com.termux/files/usr/tmp/bun-termux-cache/`)
- Cache key: FNV-1a hash of first 4096 bytes + size of the Bun ELF
- First run extracts the Bun ELF (~92 MB write), subsequent runs skip extraction
- Cache is validated by checking file size matches
- Native libs (if present) are extracted to `$TMPDIR/bun-termux-cache/bunfs-libs/`

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GLIBC_LD_SO` | `/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1` | Path to glibc dynamic linker |
| `GLIBC_LIB_PATH` | `/data/data/com.termux/files/usr/glibc/lib` | Path to glibc libraries |
| `TMPDIR` | `/data/data/com.termux/files/usr/tmp` | Cache directory base |
| `BUNFS_CACHE_DIR` | `$TMPDIR/bun-termux-cache/bunfs-libs` | Directory for extracted native libs (set automatically by wrapper) |

## Technical Details

See [SOLUTION.md](SOLUTION.md) for the full technical writeup including:
- Why `/proc/self/exe` matters for Bun
- How userland exec works
- The ELF loading and cache extraction process
- Why `memfd_create` doesn't work on Android (SELinux)

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

## Limitations

1. **Architecture-specific**: Only works on aarch64 (ARM64)
2. **First-run cost**: ~92 MB cache extraction on first run (instant after that)
3. **Cache storage**: Requires ~92 MB in `$TMPDIR` per unique Bun version
4. **Native lib detection**: Signature-based heuristics for detecting embedded `.so`/`.node` files may need updating for new Bun versions

## Related

- [Bun Issue #26752](https://github.com/oven-sh/bun/issues/26752) - Request for `BUN_SELF_EXE` env var
- [Bun Issue #8685](https://github.com/oven-sh/bun/issues/8685) - Bun on Termux documentation

## License

MIT
