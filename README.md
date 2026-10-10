# app-search

esp-claw WASM app `search`: web and news search through the device's `tavily` or `brave` credential profile. The app never sees the API keys; the firmware adds them to each request.

Replaces the firmware's built-in `web_search` capability (`cap_web_search`, Kconfig `APP_CLAW_CAP_WEB_SEARCH`).

Part of [esp-claw (community)](https://github.com/esp-claw), an unofficial community fork of espressif/esp-claw. It is not affiliated with Espressif.

## Tools

| Tool | Purpose |
|---|---|
| `search_web(query, max_results?, topic?)` | Web search. `max_results` 1-10 (default 5), `topic` `general` (default), `news` or `finance` |
| `search_news(query, max_results?)` | Same with topic `news` |

Tavily is tried first. If it is not configured or fails, Brave Search is used (`topic` `news` uses Brave's news endpoint, the others its web search). Results are compact JSON:

```json
{"provider":"tavily","results":[{"title":"...","url":"https://...","snippet":"..."}]}
```

Titles and snippets are cut to 300 bytes (of JSON-escaped text). If both providers fail the result is `{"error":"search failed","tavily_status":…,"brave_status":…,"detail":"…"}`; a status of `-2` means the profile is missing.

## Device profiles

Firmware with the search-profile migration creates these entries in `api_profiles` from `search_tavily_key` / `search_brave_key` at boot. Otherwise add them yourself:

```json
{"name": "tavily", "base_url": "https://api.tavily.com", "hosts": ["api.tavily.com"],
 "auth": {"type": "bearer", "token": "<TAVILY_API_KEY>"}, "read_only_posts": ["/search"]},
{"name": "brave", "base_url": "https://api.search.brave.com", "hosts": ["api.search.brave.com"],
 "auth": {"type": "header", "name": "X-Subscription-Token", "value": "<BRAVE_API_KEY>"}}
```

Tavily's search is a POST. Without `"read_only_posts": ["/search"]` (firmware support needed) the firmware treats it as a mutation, asks for confirmation in Telegram and refuses it elsewhere.

## Build and check

```sh
git clone --recursive https://github.com/esp-claw/app-search.git && cd app-search
./sdk/build-app.sh .                          # -> build/app.wasm, build/manifest.json
python3 sdk/tools/manifest_check.py .
python3 sdk/tools/wasm_check.py build
```

Needs clang with the wasm32 target and wasm-ld (Debian/Ubuntu: `apt install clang lld`). CI runs the same steps through `esp-claw/app-sdk/.github/workflows/app-build.yml`.

## Releases and install

Bump `version` in `manifest.json`, publish a release tagged `v<version>`; CI attaches `app.wasm` and `manifest.json`. Sign locally and upload the package:

```sh
v=1.0.0
gh release download "v$v" -p app.wasm -p manifest.json -D dist
python3 sdk/tools/ecapp.py pack dist --key /path/to/key.pem   # -> dist/search-$v.ecapp
gh release upload "v$v" dist/search-$v.ecapp
```

On the device (key in `app_trusted_keys`), in Telegram:

```
/app install https://github.com/esp-claw/app-search/releases/download/v1.0.0/search-1.0.0.ecapp
```

## License

Apache-2.0
