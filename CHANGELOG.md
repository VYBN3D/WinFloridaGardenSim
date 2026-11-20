# WinFlorida Garden Sim — Changelog

Firmware versions for the 4" S3 and 2.8" S3 WinFlorida Garden Sim boards.
Each tagged version publishes `.bin` files + `version.txt` on the
[Releases page](https://github.com/VYBN3D/WinFloridaGardenSim/releases);
boards update over the air via **Options → CHECK UPDATES → INSTALL**.

## v42 — Reference-style colas, harvest fix, sleep audio fix
- Mature plants redesigned around the Weed Farmer reference: a fat spear
  main cola (4 stacked buds) runs up the stem tip, every branch offshoot
  ends in its own 3-bud spear cola, and mature plants stand taller
  (stage 3/4 height bumped) with 4-5 branches instead of 3
- Removed the floating white pistil flecks that scattered random dots
  across the canopy and mid-air; bud frost speckles now hug the buds
- Flower harvest cutscene: the standing plants now draw at full maturity —
  finishHarvest() reset growth to 0 before the animation ran, so they
  rendered as seedlings
- Power popup no longer sleeps on the click that opened it: the popup
  waits for the BOOT button to be released before timing presses, so a
  single click always shows the quick-press/hold options
- Waking from sleep mode restarts the garden theme: the popup had killed
  the PCM theme stream and the wake path never restarted it, so the
  garden sometimes stayed silent until the next bong hit

## v41 — Plant phenos, cinematic trim, audio balance
- The vector plants are redesigned: every grow rolls a fresh seed, so each
  roommate is a distinct pheno — per-plant vigor (short/bushy to tall/lanky),
  alternating side branches that run longer toward the bottom, crown fan
  leaves, branch buds and a dominant main cola by harvest
- The fullscreen plant view now shows the **same seeded plants** as the small
  garden, magnified — one shared drawing path, so the views always match
- The flower harvest cutscene shows the roommates themselves (same seeds)
- The trimming room cutscene is rebuilt full-bleed in the intro's cinematic
  language: gradient wall, hanging work lamp, deep bench, grower with a
  properly bent arm on the shears
- Sound effects (coin blips, pest alert, fanfares) now sit ~6 dB under the
  theme so they track the music volume knob instead of blasting over it
- The power popup no longer stutters the soundtrack while it is open
- The microSD card is fully retired: plants, art and the installer are all in
  code now; a card slot is no longer needed
- New "bong hit" sample: lands between the boot chant and the theme, and
  plays over smoke breaks

## v40 — Brand picker OTA
- The WIFI / UPDATES page now has a **GAME BRAND** row: tap it to switch the
  update lane between the rebrands (**WINFLORIDA** and **VYBN**)
- CHECK UPDATES reads the selected brand's repo; INSTALL flashes that
  brand's firmware for your board and reboots into it — one board can hop
  between brands entirely over the air
- Installing a different brand is always offered; your own brand lane only
  offers INSTALL when a newer build exists
- The classic CYD build is retired (sketch + CI entry removed)

## v39 — WinFlorida rebrand
- The game is rebranded: **WinFlorida Garden Sim** (formerly Curaleaf Garden)
  — boot splash, header bar, grower ID card brand and all comments updated
- OTA release base now points at the renamed repo
  `VYBN3D/WinFloridaGardenSim`; the OTA asset name follows the renamed
  sketch files (`WinFloridaGardenSim_S3.ino.bin` etc.)
- The leaf logo stays (renamed in code); boards on v38 or older still OTA
  fine — the v39 release also carries the old-named `.bin` copies so every
  device can update to the new name

## v38 — Boot chant, sampled theme loop, hash wash room, bigger sell modal
- **Boot chant**: a crowd-chant sample (uploaded MP3, decoded on the Mac to
  16 kHz mono, IMA ADPCM 4-bit ~24 KB) embedded in flash plays once over the
  boot splash, then hands over to the theme
- **New in-game theme**: the old placeholder chiptune melody is removed;
  the game music is now the user's 92 BPM two-bar loop, ADPCM-compressed
  (~42 KB) and looping seamlessly from flash — all SFX jingles (coin blip,
  death, harvest fanfare, pest alarm) stay and mix over it
- **Hash room wash picker**: the MARKET button in the secret hash room is
  now RUN A WASH — a sell-modal-style picker with big < > arrows and cards
  to choose which stash strain to wash; one run washes 2 trimmed jars into
  1 hash unit of the same strain (quality blended), shows the flower-vs-hash
  value comparison on the card, and finishes with the hash-wash cutscene
- **Bigger strain modal**: the SELL FLOWER / SELL HASH strain modal is
  larger (y58-205) with a much bigger BACK (72x26), taller arrows/card and
  a 28px action bar; the wash picker uses the same layout
- Version string fixed on the 2.8" board (it still reported v34)
- Both boards compile at ~42% of app storage

## v37 — SD-art look without an SD card
- **Garden room now matches the PLANT BMPs even with no microSD**: the
  vector fallback was rebuilt from pixel analysis of the SD art — same
  gradient, two grid systems, trellis wires, beam, four hanging grow
  lights, dark floor, and four terracotta pots with stage-true plants
  (stake stems, sage branches, frosted bud lumps, fan leaves, pistils)
- The old mini-room fallback (3 small plants + one big vector plant) is
  gone; the empty state shows four bare pots + "Plant a seed to begin"
- SD card still wins when present: identical `/PLANT0-4.BMP` path
- No new RAM used; both boards compile at 40% of app storage

## v31 — Flicker-free frame buffer + smoke break on stash
- **Flicker-free rendering**: the game now draws every page into a full-size
  PSRAM frame buffer and pushes it to the panel in one atomic pass — no more
  visible mid-draw tearing anywhere (garden, cutscenes, modals, SD art)
- Transient overlays (farmer visit, nearby popup, death modal) are stamped
  into the buffer before the push, so they no longer blink on full repaints
- **Keyboard flicker fix**: typing on the grower-name, WiFi-password,
  cheat-code, message and send-money keyboards now repaints only the input
  field — the page stops flashing on every key press
- **Smoke break moved to the stash page**: new big SMOKE BREAK button opens a
  chooser (BONG + FLOWER + LIGHTER / BONG + HASH / PAPERS + FLOWER + LIGHTER)
  with an ARE YOU SURE? confirm; the three smoke buttons are gone from the
  market, leaving room for bigger cards
- Market: SELL/GEAR toggles enlarged to big buttons and the buy/sell cards
  moved to the full-width two-row grid
- Benji's beard trimmed to a short stubbly patch (SD art updated too)

## v30 — Benji v2 + Ray-Bans
- Benji redrawn for clarity: individual alternating light/dark dread
  strands framing the face instead of solid side walls, tighter dread
  mass, and Ray-Ban style glasses (square dark lenses, bridge, temple
  arms, lens glare) matching the rest of the growers
- Corner avatar updated to match; header mini gets the glasses for free
- GOWN9x SD art regenerated with the same cleaner face + Ray-Bans

## v29 — Benji grower + faster intro
- New grower **Benji** (10th character in the picker): blonde-brown
  dreadlocks, blue eyes, mustache and chin beard — modeled after the
  reference photo, no bandana
- Benji is drawn everywhere the others are: character-select card,
  corner avatar, mini head, intro close-up and the gown-up cutscene
  (falls back to the generic gown art since no /GOWN9*.BMP exists yet)
- Intro cutscene retimed: the face close-up push-in runs about twice as
  fast (600 ms/frame), the tour frames linger at 1400 ms — the whole
  intro now lands near 8 seconds

## v28 — OTA busy guard + sell-modal flash fix
- WiFi/UPDATES: tapping INSTALL now swaps the button to a dimmed
  "DOWNLOADING ..." that can't be re-tapped; if the download or flash
  fails the button returns to normal so you can retry
- Sell submenu: arrow taps no longer trigger the full-page tail render,
  which briefly flashed the market page under the modal

## v27 — smooth sell-picker arrows
- Tapping the giant < > arrows in the sell submenu now repaints only the
  strain card and the SELL bar (a full-page render used to flash the
  whole screen on every arrow tap)

## v26 — sell picker uses big strain cards
- The market strain submenu is rebuilt: BACK is now a proper button in the
  top-right of the box; each stashed strain is one giant card flanked by
  full-height < > arrows (cycling stocked strains like the other pickers),
  with the strain name, count and per-unit price on the card
- Full-width SELL bar under the card; the ARE YOU SURE? confirmation and
  its big BACK / YES, SELL IT buttons are unchanged

## v25 — hash jars, sell/send confirm menus, bigger OTA buttons
- Hash is now part of the stash list: the STASH page lists flower jars and
  hash units per strain on the same rows (jars grade | hash grade)
- Hash jars are sendable: the send-supplies screen has FLOWER JAR and
  HASH JAR cards side by side (own strain arrows), and recipients get
  "Received a %s hash jar"
- Market sell cards open a strain submenu (3 big rows per page, < >
  paging) instead of the skinny top shelf bar; each row shows strain,
  count and per-unit price
- One-tap confirmation before anything sells (ARE YOU SURE? with
  YES, SELL IT / BACK) and before any product is sent (SEND IT? /
  BACK) — no more accidental sales or gifts
- WiFi / UPDATES page: YOUR BUILD shrunk to a small text line, buttons
  enlarged; Options menu: RESET GAME and UPDATE are now two big
  side-by-side card buttons
- v24/v23 catalog tweaks carry over (owned-pack pill badge, send-screen
  arrows cycle all unlocked strains, money keypad rebuilt)

## v24 — seed pack pill badge
- Seed catalog: every unlocked card gets a pill badge showing how many
  4-packs you hold ("x1", "x2" — gold when stocked, gray "x0" when not)
- The newest unlocked strain no longer hides the count behind a
  "Fresh unlock - try it!" note; the count pill is always shown

## v23 — market shelf picker + send-screen fixes
- Market SELL mode: shelf row with `<` `>` arrows picks the stash strain;
  SELL 1 and SELL ALL now act on the picked strain (cycle the shelf for others)
- Seed catalog: owned count always visible on unlocked cards
  ("You own xN - BUY to restock/start a batch"); the small gold count
  bubble was removed (it overlapped long strain names)
- Seed catalog: BUY 4-PACK button enlarged to full card width
- Send supplies: SEEDS / STASH JAR arrows cycle **all unlocked strains**
  (they previously only cycled strains you hold, so they looked dead
  when you had one stocked strain)
- Send money: keypad rebuilt as two rows of five big keys — the `0`
  no longer falls off the screen; CLR / 000 / DEL and ALL / SEND / BACK
  evenly spaced

## v21 — PEST, CARE and SKIP cheat codes
- PEST: clears the infestation (type + severity)
- CARE: +5 water packs, +5 care kits, +5 pest defense kits
- SKIP: satisfies the open defoliation window without the kill risk so
  growth resumes (no-ops with feedback when nothing is open)

## v20 — button-scale WiFi scan rows
- Network list rows grow from 17 px / font-1 to 24 px / font-2 with a
  24 px tap zone per row (4 per page, `<` `>` page by 4)
- Saved-network marker and RSSI kept; long SSIDs truncate to fit

## v19 — farmer pop-out + overlay repaint fixes
(Folds in the v17/v18 fixes, which were flashed over USB but never tagged)
- Farmer / speech-bubble overlay re-stamps only when something repaints
  underneath — no more continuous per-pass redraw flood
- Pop-outs restore the covered art region through a PSRAM sprite blitted
  in one push (no visible row-by-row rebuild flash)
- Sprite pixels stored in panel wire order — fixes the mismatched-colour
  box the first pop-out blit left on the garden
- No farmer visits during the watering animation or the dying-plant modal

## v16 — baseline feature set
- Per-strain seeds (bought and tracked per strain)
- WiFi login + OTA firmware updates
- Cheat codes: CASH/MONEY, SEEDS, STASH, UNLOCK, HARVEST, TURBO, HYDRO
- Remediation kits (pest / water / care) and catalog-style shops
