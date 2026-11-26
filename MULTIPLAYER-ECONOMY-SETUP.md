# Native multiplayer save sync

Status: local draft only. The shared-economy migration has NOT been applied to Supabase. Nothing has been committed, pushed, or published.

## Architecture

- One shared online lobby and direct messaging channel for both games.
- One shared native-save namespace (`Garden`) for both branded games.
- Cash, seeds, flower, hash, and supplies can be transferred between WinFlorida and VYBN players.
- Existing game saves are the starting balance. The new native economy does not add the old prototype's $20.
- No ESP32 firmware changes.
- The old `garden_wallets` / `garden_inventory` prototype tables are not used for native game balances. The earlier migration remains in place for player profiles and lobby name RPC.

## Native save format

The firmware stores:
- Cash as a uint16 value.
- 16 seed counts as uint8 values.
- 16 flower counts and 16 hash counts as uint16 values.
- Separate 16-byte flower and hash quality arrays.
- Native supply/accessory counts as uint8 values.

The browser bridge exposes only the economy fields needed for multiplayer. It never uploads Wi-Fi SSIDs/passwords or the rest of the NVS save. The codec preserves the original packed arrays, per-strain counts, and firmware's integer weighted-quality blend.

## Transfers

The first additive SQL migration creates:
- `garden_native_saves`, keyed by Supabase user ID and game key.
- `garden_native_transfers`, with per-game transfer receipts.
- `garden_native_save_import`, which imports a local save only if no cloud save exists.
- `garden_native_save_update`, which uses optimistic revisions to detect conflicts.
- `garden_native_transfer`, which updates sender and recipient saves atomically.

Supported native assets: cash, seeds, flower jars, hash jars, water packs, care kits, pest kits, remediation kits, bongs, lighters, and rolling papers. Seed/flower/hash transfers retain the selected strain. Flower/hash quality is merged using the same weighted integer formula as the firmware.

The client enables transfers to any selected online grower, regardless of the displayed game branding, once both players have synced the shared save. The new shared-economy migration copies existing single-brand saves into the `Garden` namespace without summing balances. If one account has two different legacy cloud saves, the migration deliberately stops for reconciliation rather than choosing or merging silently. The database enforces shared save rows, balance checks, integer limits, and one-time request IDs. Incoming updates are applied to the browser's local NVS and the game reloads to read the native save.

## Conflict protection

- First import never overwrites an existing cloud save.
- A local save with uncommitted changes is never silently replaced by a newer cloud revision.
- Revision conflicts pause syncing/transfers instead of guessing which copy is correct.
- The bridge allowlists economy keys only and preserves unrelated local game data.
- Anonymous Supabase identities are browser-specific. Clearing browser storage can lose access to an anonymous identity.

## Supabase steps still required

1. Keep Anonymous Sign-Ins enabled.
2. Open `garden-shared-economy-migration.sql` from this folder.
3. Review and apply this second additive migration in the same Supabase project's SQL Editor. Keep the earlier migration and its saved query intact.
4. Do not paste a service-role key into the browser or game files.
5. Test with two separate browser profiles, first in WinFlorida and then separately in VYBN.

## Local tests performed

- Native save codec tests: cash, per-strain seeds, flower quality blending, supplies, limits, legacy save fallback, and immutable input objects.
- Browser bridge tests against both local test pages: game key, native NVS read/write, dirty tracking, array lengths, and exclusion of Wi-Fi fields.
- Multiplayer UI smoke tests: 11 native asset types, all 16 strain options, bridge availability, and no page errors.
- JavaScript syntax checks and Git whitespace checks pass.

## Not yet done

- The shared-economy SQL migration has not been applied or tested against the live Supabase database.
- Real two-browser transfer and reload tests have not yet been run.
- Nothing has been committed, pushed, or published.
