# MoonMeow Streaming Core Library

Fork of [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) for [Meowverse](https://github.com/meowerse) — adds Meow viewport and control extensions on top of the upstream GameStream core.

**Upstream:** `moonlight-stream/moonlight-common-c` (original) + `ClassicOldSong/moonlight-common-c` (networks preserved as remotes `upstream` / `classicsong`). This fork tracks upstream `master` (merged at `62e0663`) and adds `meow` branch on top.

**Default branch:** `meow` — contains `feat(viewport-event)` (`LiSendViewportEventForced`, desktop-extent echo, retry) plus Meow control additions. `master` mirrors upstream for easy sync. Switch default to `meow` in GitHub Settings → Branches if not already set.

**Usage:** Same as upstream — requires the bundled ENet submodule (`enet/`). MoonMeow clients (`moonmeow` Android) consume this via submodule `app/src/main/jni/moonlight-core/moonlight-common-c` → `https://github.com/meowerse/moonmeow-common-c`.

## Note to Developers (from upstream)

Moonlight-common-c requires the _specific_ version of ENet that is bundled as a submodule. This version has changes required for IPv6 compatibility and retransmission reliability, among other things. These are breaking API/ABI changes which make Moonlight-common-c incompatible with other versions of the ENet library. Attempting to runtime link to another libenet library will cause your client to crash when connecting to recent versions of GeForce Experience.

## Meow changes vs upstream

- `feat(control): add viewport event to the control stream` + `feat(viewport-event)` / `feat(control): add LiSendViewportEventForced`
- Desktop extent on viewport echo, final-flush, retry and ABI notes
- Viewport echo v2 (frame index), cursor position (`0x3004`) and receiver report / applied bitrate (`0x3005`) — wire format in [docs/meow-protocol.md](docs/meow-protocol.md), codec tests in `tests/meow/` (`make -C tests/meow`)
- Tracks upstream fixes (e.g., MbedTLS PSA, `LI_CTYPE_STEAM`, nanors bumps)

Sync: `git fetch upstream --prune && git checkout meow && git merge --ff-only upstream/master` then `git push origin meow`.
