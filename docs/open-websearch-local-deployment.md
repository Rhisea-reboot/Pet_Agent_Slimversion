# open-webSearch Local Deployment

This project uses `open-webSearch` only through its REST daemon. It does not use Docker or the MCP HTTP transport.

## Fixed upstream version

- Tag: `v2.1.11`
- Commit: `3094fa558fce35a8373e45ed5a6c43362e206906`
- Source directory: `vendor/open-webSearch`

The install script validates both values before installing dependencies with `npm ci` and building the TypeScript project.

## Security boundary

- The daemon must bind only to `127.0.0.1` or `::1`.
- `ENABLE_CORS` is explicitly set to `false` by the start script.
- The REST daemon has no built-in authentication. Its current protection is loopback-only binding.
- Do not expose port `3210` on a LAN or the Internet.
- Do not enable the MCP HTTP transport, `/mcp`, `/sse`, or `/messages`.
- VPet V1 will call only `POST /search`. It will not call `/fetch-web` or any other fetch endpoint.

A future remote deployment must place an authenticated TLS reverse proxy in front of the daemon.

## Commands

From the repository root:

```powershell
.\scripts\open-websearch\Install-OpenWebSearch.ps1
.\scripts\open-websearch\Start-OpenWebSearch.ps1
.\scripts\open-websearch\Test-OpenWebSearch.ps1
.\scripts\open-websearch\Stop-OpenWebSearch.ps1
```

For a Bing-only daemon, use the request parser directly:

```powershell
.\scripts\open-websearch\Start-OpenWebSearch.ps1 -AllowedSearchEngines bing -DefaultSearchEngine bing -SearchMode request
.\scripts\open-websearch\Test-OpenWebSearch.ps1 -IncludeSearch -Engines bing -SearchMode request -SearchQuery "Qt 6 network"
```

If a local HTTP proxy is available, enable it explicitly with `-UseProxy -ProxyUrl http://127.0.0.1:7890`. The start script does not assume that port is open.

`Test-OpenWebSearch.ps1` checks `/health` and `/status`. Add `-IncludeSearch` only when validating a real external search request:

```powershell
.\scripts\open-websearch\Test-OpenWebSearch.ps1 -IncludeSearch -SearchQuery "Qt 6 network"
```

The optional real-search check uses a 15-second desktop-interaction budget. A timeout means that an upstream engine did not finish in time; it does not by itself mean that the local daemon is unhealthy. Check `/health` and `/status` separately before diagnosing an engine problem.

The default allowed engine is now `bing` in `request` mode. DuckDuckGo and Startpage are intentionally excluded because their real HTTPS requests failed or timed out in this environment. `sogou` remains excluded from the default configuration even though its pinned-release live behavior was separately checked.

The pinned release does not implement a Google search engine. It implements Bing, and Bing can be used without Google credentials. Do not add `google` to `ALLOWED_SEARCH_ENGINES`; it is not a valid engine name in this release.

## Bing and Google check (2026-08-03)

- Direct Bing HTTPS access succeeded, and the pinned Bing CLI search returned three results for `Qt 6 network`.
- The loopback daemon returned HTTP 200 with three Bing results using `SEARCH_MODE=request`.
- The Bing-only loopback daemon returned HTTP 200 with three results for both `Qt 6 network` and the Chinese query `北京天气`; the Chinese result count may be zero when Bing returns no eligible result, which remains a valid successful envelope with no fabricated result.
- Direct Google HTTPS access timed out in this environment. Google is also not present in the pinned release's supported engine list, so switching to Google is not a supported fix.
- The earlier DuckDuckGo and Startpage failures are therefore upstream connectivity/anti-bot path issues, not a local REST daemon failure. Bing request mode is the currently verified fallback.

## P0 engine verification (2026-07-31)

The pinned release was checked locally with upstream fixtures and the running loopback daemon:

- `/health` and `/status` passed. The daemon reported `version: unknown` in serve mode; deployment identity remains the fixed Git commit and `package.json` version.
- DuckDuckGo English (`Qt 6 network`) timed out at the daemon boundary after approximately 20 seconds.
- DuckDuckGo Chinese (`北京天气`) failed during the upstream connection attempt.
- Startpage English (`Qt 6 network`) timed out during TLS connection setup. The upstream `test:startpage` test also timed out after 15 seconds.
- Startpage Chinese (`北京天气`) failed during the upstream connection attempt.
- Sogou Chinese (`北京天气`) was rejected by the daemon allow-list, so no live request was made. The pinned upstream Sogou parser, verification-page, and safe-redirect tests passed, but this is not live engine acceptance.

Conclusion: the Bing-only local service passes the current acceptance matrix. Keep DuckDuckGo, Startpage and Sogou out of the default allow-list. Do not increase VPet's 15-second user-facing search budget to hide upstream timeouts.

## Runtime files

The start script creates the following untracked files:

```text
vendor/open-webSearch/.runtime/open-websearch.pid
vendor/open-webSearch/.runtime/open-websearch.stdout.log
vendor/open-webSearch/.runtime/open-websearch.stderr.log
```

Check the stderr log if startup or health verification fails.
