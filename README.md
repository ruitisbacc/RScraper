# RScraper

RScraper is a C++20 CLI application for mirroring websites into a local folder for offline browsing.
It downloads pages and assets, rewrites links, and can optionally use a headless Chromium-based browser for dynamic sites.

## What It Does

- Crawls from a seed URL and stores files under `<out>/site/<host>/...`
- Rewrites HTML and CSS links to local relative paths for offline browsing
- Extracts URLs from HTML, CSS, JavaScript, and JSON responses
- Supports sitemap/robots discovery and optional hidden-page probing
- Supports dynamic rendering and runtime URL discovery via Chromium CDP
- Persists crawl state in SQLite for resume

## How It Works (High-Level)

1. Parses CLI arguments or runs the interactive parent mode wizard.
2. Applies profile defaults (`balanced`, `fast`, `deep`, `dynamic-heavy`), while explicit CLI flags override profile values.
3. Initializes render mode:
   - `static`: HTTP-only crawling
   - `dynamic`: requires Chrome/Edge/Chromium (or `--dynamic-browser`)
   - `auto`: fetches the seed page, runs site detection, and enables dynamic rendering only when the site looks hybrid/dynamic
4. If `--detect-site-mode` is enabled, it performs detection and exits without crawling.
5. Initializes storage + frontier (seed URL, optional extra URLs, sitemap discovery paths, optional hidden-page probes).
6. Downloads resources with a libcurl multi pipeline.
7. Classifies response type (HTML/CSS/JS/JSON/asset), extracts new URLs, rewrites content, and saves files.
8. Writes metadata (`manifest.json`, `errors.log`) and keeps a SQLite crawl database for resume.

## Commands and Help

### `rscraper` (no arguments)

Starts **Parent Mode** (interactive wizard).

### `rscraper parent`

Starts the same Parent Mode explicitly.

### `rscraper --help`

Shows top-level help with subcommands and quick-start examples.

### `rscraper parent --help`

Shows a short help page for the parent mode command.

### `rscraper mirror --help`

Shows the full option list for the non-interactive crawler command.

### `rscraper mirror --help-all`

Currently the same as `--help` (no options are hidden right now). It is still useful as a conventional "show everything" entry point.

## Parent Mode (Interactive Wizard)

Parent Mode is designed for common use cases and intentionally does not expose every advanced flag.
Use `rscraper mirror --help` for full control.

### Triggering Parent Mode

```powershell
rscraper
# or
rscraper parent
```

### Prompt Flow

Parent Mode asks, in this order:

1. Seed URL
2. Output directory (default `./mirror`)
3. Profile (`balanced`, `fast`, `deep`, `dynamic-heavy`)
4. Render mode (`static`, `auto`, `dynamic`)
5. Detection-only mode (`--detect-site-mode` equivalent)
6. If detection-only is **off**, it additionally asks:
   - max crawl depth
   - concurrency
   - same-host restriction
   - sitemap discovery
   - common hidden-page probing
   - request rate limit (RPS)
   - resume
7. Summary + final confirmation

### Detection-Only Parent Mode Behavior

If you choose detection-only mode in Parent Mode, crawl/download prompts are skipped because they do not affect the detection path.

## Profiles (Preset Strategies)

`balanced` is the default baseline.

- `balanced`: defaults from the CLI (depth 2, concurrency 16, render mode `auto`)
- `fast`: higher concurrency, smaller dynamic budgets, fewer dynamic expansions
- `deep`: higher crawl depth + more aggressive dynamic/AJAX expansion
- `dynamic-heavy`: forces dynamic render mode and raises interaction/pagination budgets

Important behavior: explicit CLI flags override profile values.
Example: `--profile fast --concurrency 8` keeps `concurrency=8`.

## Option Reference (`rscraper mirror`)

### Required

- `url` (positional): seed URL to crawl from
- `-o, --out <dir>`: output directory for mirrored files and metadata

### Presets

- `--profile <balanced|fast|deep|dynamic-heavy>` (default: `balanced`)

### Crawling

- `-d, --depth <n>` (default: `2`): maximum crawl depth for navigational pages
- `-c, --concurrency <n>` (default: `16`): parallel HTTP downloads
- `--requests-per-second <n>` (default: `0`): throttle request rate (`0` = unlimited)

### Scope

- `--same-host` / `--no-same-host` (default: enabled): restrict crawl to the seed host
- `--discover-sitemaps` / `--no-discover-sitemaps` (default: enabled): probe robots/sitemap endpoints and enqueue sitemap URLs
- `--probe-common-pages` (default: disabled): probe common hidden paths (login/admin/etc.) and robots hints
- `--domain-alias <host>` (repeatable): allow extra hosts/domains while keeping scope control
- `--include-url <regex>` (repeatable): only crawl URLs matching at least one include regex
- `--exclude-url <regex>` (repeatable): skip matching URLs
- `--extra-url <url|path>` (repeatable): enqueue additional URLs/paths even if not linked

### HTTP

- `--user-agent <str>` (default: `rscraper/0.1`)
- `--timeout <seconds>` (default: `30`)
- `--max-bytes <n>` (default: `52428800` = 50 MiB)
- `--exclude-content-type <regex>` (repeatable): skip responses by content type
- `--header "Name: Value"` (repeatable): custom request headers
- `--cookie "name=value"` (repeatable): custom cookies

### Dynamic Rendering / Detection

- `--render-mode <static|auto|dynamic>` (default: `auto`)
- `--detect-site-mode` (default: off): run site mode detection and exit without crawling
- `--dynamic-browser <path>`: explicit Chrome/Edge/Chromium executable
- `--dynamic-wait-ms <n>` (default: `5000`): render virtual-time budget
- `--dynamic-timeout-ms <n>` (default: `30000`): hard timeout for one dynamic render
- `--dynamic-max-renders <n>` (default: `0`): max dynamically rendered pages (`0` = unlimited)
- `--dynamic-interactions` / `--no-dynamic-interactions` (default: enabled): CDP interactions (pagination/load-more/lazy hydration/runtime AJAX capture)
- `--dynamic-interaction-steps <n>` (default: `8`)
- `--dynamic-idle-ms <n>` (default: `700`): idle window between interaction rounds
- `--dynamic-persistent-browser` / `--no-dynamic-persistent-browser` (default: enabled)
- `--dynamic-block-heavy-resources` / `--no-dynamic-block-heavy-resources` (default: enabled)
- `--dynamic-timeout-storm-threshold <n>` (default: `4`): disables dynamic rendering after repeated timeout-like failures (`0` = disabled)
- `--ajax-max-pages <n>` (default: `50`): max auto-expanded JSON/API pagination pages

### Runtime

- `--resume` / `--no-resume` (default: enabled): resume from `<out>/_meta/crawl.sqlite`
- `--log-level <trace|debug|info|warn|warning|error>` (default: `info`)

## Typical Usage Examples

### Start with Parent Mode (recommended first run)

```powershell
rscraper
```

### Basic mirror

```powershell
rscraper mirror https://example.com --out ./mirror
```

### Faster crawl

```powershell
rscraper mirror https://example.com --out ./mirror --profile fast
```

### Deeper crawl with hidden-page probing

```powershell
rscraper mirror https://example.com --out ./mirror --profile deep --probe-common-pages
```

### Force dynamic rendering for JS-heavy sites

```powershell
rscraper mirror https://example.com --out ./mirror --render-mode dynamic
```

### Detection only (no crawl)

```powershell
rscraper mirror https://example.com --out ./detect-run --detect-site-mode
```

### Authenticated/customized crawl

```powershell
rscraper mirror https://example.com --out ./mirror `
  --header "Authorization: Bearer <token>" `
  --cookie "sessionid=abc123"
```

### Add CDN host + skip logout/cart pages

```powershell
rscraper mirror https://example.com --out ./mirror `
  --domain-alias cdn.example.com `
  --exclude-url "logout|cart"
```

## Output Structure

```text
<out>/
|-- site/
|   `-- <host>/
|       |-- index.html
|       |-- ...
`-- _meta/
    |-- rscraper.log
    |-- crawl.sqlite
    |-- manifest.json
    `-- errors.log
```

Notes:

- Non-default ports are stored as `site/<host>__p_<port>/`
- Query-string variants are stored with a `__q_<hash>` filename suffix
- In detection-only mode, the crawl is skipped; in practice you should expect at least `_meta/rscraper.log` (and not a full mirrored site)

## Help and Error UX Notes

- Missing required args and invalid option values print a clear error and then the relevant help page.
- `rscraper` without arguments intentionally starts the wizard (it does not show help).
- Use `Ctrl+C` to cancel a running crawl. During Parent Mode, EOF/cancelled input exits the wizard cleanly.

## Requirements

- CMake 3.20+
- C++20 compiler (MSVC 2022, GCC 11+, Clang 14+)
- vcpkg (recommended dependency manager)
- For dynamic mode: Chrome, Edge, or Chromium (unless you pass `--render-mode static`)

## Build (Windows / PowerShell Example)

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Development Notes

- Core CLI behavior lives in `src/main.cpp`
- Crawl orchestration is implemented in `src/CrawlEngine.cpp`
- URL/path mapping is implemented in `src/Url.cpp` and `src/PathMapper.cpp`
- HTML/CSS/JS/JSON extraction and rewriting are split into dedicated modules under `src/`

## License

MIT License
