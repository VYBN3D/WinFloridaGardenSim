# 🌿 WinFlorida Garden Sim

**A cannabis grow-sim that runs entirely on a $15 CYD display board.**
Plant seeds, keep your rooms watered and defoliated, fight mites and aphids,
harvest, trim, press hash, run the secret wash room, build a stash, and cash
out at the market — with your own grower, your own stash art, and a
soundtrack baked into the firmware.

Three boards. One firmware family. Everything below is live in the latest
[**Releases**](https://github.com/VYBN3D/WinFloridaGardenSim/releases).

---

## 📑 Contents

- [About the game](#about-the-game)
- [Play it in your browser (web emulator)](#play-it-in-your-browser-web-emulator)
- [Supported boards](#supported-boards)
- [Installing / updating](#installing--updating)
  - [Over the air (OTA)](#over-the-air-ota)
  - [USB flash from source](#usb-flash-from-source)
- [The manual](#the-manual)
  - [First launch](#first-launch)
  - [The garden loop](#the-garden-loop)
  - [The harvest room: flower or hash](#the-harvest-room-flower-or-hash)
  - [The trimming room](#the-trimming-room)
  - [The secret hash room](#the-secret-hash-room)
  - [Stash & smoke breaks](#stash--smoke-breaks)
  - [The market](#the-market)
  - [Supplies shop](#supplies-shop)
  - [Nearby growers](#nearby-growers)
  - [The grow clock, sleep & power](#the-grow-clock-sleep--power)
  - [The charge LED](#the-charge-led)
- [Strain catalog](#strain-catalog)
- [Options & settings](#options--settings)
- [Sound & audio](#sound--audio)
- [microSD plant art](#microsd-plant-art)
- [Cheat codes](#cheat-codes)
- [Building from source](#building-from-source)
- [Firmware versions](#firmware-versions)

---

## About the game

WinFlorida Garden Sim is a full grow-house economy squeezed onto a small TFT
touchscreen. You pick a grower, build a catalog of 16 strains one unlock at a
time, and run rooms of 4 plants from seed to sale.

Every choice matters:

- **Harvest a room as FLOWER** and you get 4 untrimmed units that need a trim
  session before they sell.
- **Harvest as HASH** and the secret room presses 2 hash units directly —
  worth roughly 2.5× the flower, but the room is a paid upgrade.
- **Wash trimmed flower into hash** at any time from the hash room: 2 jars in,
  1 hash unit out, quality carried over.
- **Vigor at harvest time grades your jar.** Well-cared-for plants sell at a
  premium; stressed crops sell cheap.
- Pests, drought and over-defoliation kill plants. The LED on the back of the
  board tells the whole story at a glance.

The game keeps growing on **wall-clock time** — water before bed, wake up to a
mature room. Deep sleep between sessions costs almost nothing.


## Play it in your browser (web emulator)

No board? No problem. The firmware's grow-room core runs as a **web emulator**:

**→ [vybn3d.github.io/WinFloridaGardenSim](https://vybn3d.github.io/WinFloridaGardenSim/)**

- The page is a full landing page + manual wrapped around the emulator, which
  renders inside an ESP32 case frame — tap the panel like the touchscreen and
  press the on-case **BOOT** button like the board.
- **Pixel-faithful to the S3 build**: 480×320 panel, the same wrapper geometry
  (×1.5 / ×1.33 scaling), the exact TFT_eSPI bitmap fonts (5×7 GLCD, 16px,
  26px RLE), the leaf logo mask, the vector plant art pipeline with spear
  colas, and the IMA-ADPCM boot chant → bong → theme loop decoded live to
  WebAudio.
- **Same game rules**: real-time grow clock (30 s ticks), hydration drain +
  wilt-timer death, pests and care kits, harvest grading, 16-strain market,
  NORMAL/FAST/WOW speeds, sleep and deep-sleep via BOOT.
- **Fullscreen mode** for phones — the emulator fills the screen and locks
  landscape where the browser allows it.
- **Saves in the browser** and keeps growing on wall-clock time while you're
  away, just like the board does through sleep.
- **Tracks the firmware releases**: the OPTIONS → build tag is checked against
  the latest `version.txt` from this repo's releases.

The emulator is compiled automatically from `firmware/WinFloridaGardenSim_S3.ino`
when `main` is updated. GitHub Actions publishes only the generated WebAssembly
artifacts to `gh-pages`; the landing page and Supabase Nearby Growers interface
remain presentation/integration code, not a second copy of the game.
A plain-text link manifest is kept up to date alongside it.

## Supported boards

| Board | Panel | Touch | Sketch | Notes |
|---|---|---|---|---|
| **CYD classic** | 2.8" ILI9341, 320×240 | Resistive (XPT2046) | `WinFloridaGardenSim.ino` | Single SPI bus, SD + touch share |
| **S3 2.8"** | 2.8" ST7789-class, 320×240 | Capacitive | `WinFloridaGardenSim_S28.ino` | ESP32-S3, separate SD bus |
| **S3 4"** | 4" ST7796, 480×320 | Capacitive (FT6336U) | `WinFloridaGardenSim_S3.ino` | ESP32-S3, SDIO SD, PSRAM frame buffer |

All three share the same game logic; the S3 builds add the PSRAM render
buffer (flicker-free pages), the ES8311 audio path, and the charge LED.

## Installing / updating

### Over the air (OTA)

Every board checks GitHub Releases for a newer `version.txt`:

1. Open **OPTIONS → UPDATE → CHECK UPDATES**.
2. If a new version is listed, tap **INSTALL**.
3. The board flashes itself over WiFi and reboots into the new build.

No cables, no SD card required — the release assets are the exact `.bin`
files the boards fetch.

### USB flash from source

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \
  --build-path build WinFloridaGardenSim_S3/

arduino-cli upload \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \
  --input-dir build --port /dev/cu.usbmodem11201 WinFloridaGardenSim_S3/
```

The classic CYD uses the default ESP32 FQBN with `User_Setup_classic.h`.

---

## The manual

### First launch

1. **Pick a grower.** Fifteen characters, including Bruce, Felix, Sammie,
   Bradley, and Ashley, each get a name you can change from the grower menu.
2. **Pick your first strain.** Northern Lights (EASY, $8) is the starting
   pack; the rest of the catalog unlocks as you successfully harvest.
3. **Plant the room.** Tap the 4 pots (or just tap once — the tray fills).
4. The garden opens and the grow clock starts.

### The garden loop

The garden page is the plant room: plant art on the left (SD bitmap or the
built-in vector room), live stats on the right, and one big action button.

- **WATER** — soil dries every 30-second tick. Bone-dry soil drains vigor
  fast and can kill the plant; overwatering drowns it. Keep the meter near
  the strain's need and you'll grow faster.
- **DEFOLIATE** — every so often the plant demands a prune window. Cut the
  allowed number of leaves (each strain has its own tolerance); skip the
  window and growth pauses; overdo it and the plant dies.
- **Harvest-ready LED** — the back LED flashes green when the room is
  mature; a fast red strobe means pests; a slow red pulse means charging.

Stats shown: **GROWTH**, **HYDRATION**, **VIGOR**, current strain, day count,
and a pest banner when something has moved in.

### The harvest room: flower or hash

When growth hits 100%, the page becomes a choice:

| | Yields | Notes |
|---|---|---|
| **FLOWER** | 4 untrimmed units | Must be trimmed before selling |
| **HASH** | 2 hash units | Requires the Secret Hash Room upgrade |

Roughly one harvest in five hits a **mold event**: some of the batch is
tossed unless you spend **remediation kits** (1 kit buys 1 unit back).

### The trimming room

FLOWER harvests land here. **TRIM FLOWERS** runs the trim scene and moves the
batch into your stash jar for that strain. Trimmed flower keeps the harvest
vigor as its jar quality.

### The secret hash room

Buy the upgrade from SUPPLIES for **$150**. The room gives you:

- **RUN A WASH** — a strain picker with big `<`/`>` arrows and a card per
  stocked strain. One run washes **2 trimmed jars into 1 hash unit** of the
  same strain (quality blended with your existing hash). The card shows what
  the jars sell for as flower versus what the washed hash sells for, so you
  can decide strain by strain. Washing finishes with the hash-wash cutscene.
- **GARDEN** — back to the rooms.
- Harvest-as-hash and washed hash both sell at the market's hash lane.

### Stash & smoke breaks

The stash page lists every jar (flower and hash, units + quality) and gives
you the consumer side of the game:

- **TRIM FLOWERS** — jump to the trim room when a batch is waiting.
- **SMOKE BREAK** — pick your poison: *bong + flower*, *bong + hash*, or roll
  a *joint* (papers + flower + lighter). You'll need the gear from the
  market. Every break raises the stoned meter and ends with the smoke scene
  and a rainbow flourish on the board LED.

### The market

Sell lane (pick a strain from a big-card modal, one jar or the whole jar):

- Flower sells at the strain's flower value × jar quality grade.
- Hash sells at ~2.5× the flower value of the strain.

Gear lane:

| Item | Price | Effect |
|---|---|---|
| BONG | $24 | Unlocks bong smoke breaks |
| LIGHTERS ×3 | $6 | Fuel for breaks & joints |
| ROLLING PAPERS ×5 | $5 | Joints |
| (Supplies tab) | | Water / care / pest / remediation kits |

### Supplies shop

| Item | Price | Effect |
|---|---|---|
| WATER PACKS ×3 | $5 | Refill soil faster |
| CARE KITS ×3 | $7 | Vigor top-ups |
| SEED SHOP | varies | Buy unlocked strains |
| PEST DEFENSE ×2 | $10 | TREAT invasions |
| REMEDIATION ×2 | $12 | Rescue mold-hit harvest units |
| SECRET HASH ROOM | $150 | One-time unlock |

### Nearby growers

The NEARBY tab uses ESP-NOW radio to find other WinFlorida boards around you.
Say hello, browse who's out there, and **send supplies** — seeds, flower jars
or hash units travel over the air with a confirm-first flow.

### The grow clock, sleep & power

- Growth runs on wall-clock time; sleep and reboots catch up on missed ticks.
- **QUICK press of BOOT** = sleep screen (display off, near-zero drain).
- **HOLD BOOT ~1.5 s** = deep sleep. Wake = full reboot with the garden
  exactly where you left it.
- On USB power, sleep mode keeps the charge light running: pulsing red while
  charging, solid green when full, **dark whenever unplugged**.

### The charge LED

Priority order: charging pulse red → full green → harvest-ready flashing
green → pest strobe → death strobe → smoke-break rainbow → dark.

---

## Strain catalog

Unlocks in order as you harvest. Prices are seeds; values are per unit at
full quality (quality grades scale the actual price).

| # | Strain | Type | Difficulty | Notes | Seed | Flower | Hash |
|---|---|---|---|---|---|---|---|
| 1 | Northern Lights | INDICA | EASY | Earthy pine body calm | $8 | $10 | $25 |
| 2 | Blue Dream | SATIVA | MEDIUM | Sweet berry heady lift | $12 | $14 | $35 |
| 3 | Banana Papaya | HYBRID | MEDIUM | Tropical banana cream | $12 | $14 | $35 |
| 4 | Superbuff Cherry | HYBRID | MEDIUM | Cherry gas social buzz | $12 | $14 | $35 |
| 5 | Jack Herer | SATIVA | MEDIUM | Pine citrus bright focus | $12 | $14 | $35 |
| 6 | Granddaddy Purple | INDICA | MEDIUM | Grape berry cozy body | $12 | $14 | $35 |
| 7 | White Widow | HYBRID | MEDIUM | Pepper earth steady buzz | $12 | $14 | $35 |
| 8 | Blue Lobster | HYBRID | HARD | Blue candy gas | $18 | $20 | $50 |
| 9 | OG Kush | INDICA | HARD | Fuel pine couch calm | $18 | $20 | $50 |
| 10 | Sour Diesel | SATIVA | HARD | Diesel citrus daytime fuel | $18 | $20 | $50 |
| 11 | Gelato | HYBRID | HARD | Creamy dessert calm | $18 | $20 | $50 |
| 12 | Titan Express | HYBRID | HARD | Fast gas express rush | $18 | $20 | $50 |
| 13 | Zesty Garlic Cookies | HYBRID | HARD | Garlic cookie funk | $18 | $20 | $50 |
| 14 | Moon Beam Gelato | HYBRID | HARD | Moonlit cream dessert | $18 | $20 | $50 |
| 15 | Toad Venom | HYBRID | EXPERT | Swamp funk mind melt | $30 | $32 | $80 |
| 16 | Foreign Kushmints | INDICA | EXPERT | Minty gas deep melt | $30 | $32 | $80 |

Harder strains grow slower and drink more; EXPERT drinks the most.

## Options & settings

| Option | What it does |
|---|---|
| GAME SPEED | RELAXED (normal ~30 min grow) → QUICK (3× pace) → EXTREME (fastest ticks) |
| MUSIC | Volume steps 0–100%; speaker mute toggle on the grower picker |
| FAST HARVEST | Demo a full harvest instantly (for testing) |
| CHEATS | Cheat console (see below) |
| RESET GAME | Full wipe (double-tap arm) |
| UPDATE | OTA check + install |
| TOUCH CAL | 4-point recalibration (classic board) |
| GROWER CARD | Change grower / name |

## Sound & audio

The S3 boards ship with a real audio path: **ESP32-S3 I2S → ES8311 codec →
FM8002E amp → speaker connector**.

- **Boot chant** — a crowd-chant sample plays once over the boot splash
  (ADPCM, stored in flash).
- **Theme loop** — a 92 BPM sampled loop is the in-game music, looping
  seamlessly.
- **Sound effects** — coin blips on purchases and sales, harvest fanfare,
  death jingle, pest alarm. All mix over the theme; MUSIC=0 silences
  everything including the boot chant.

## microSD plant art

Drop `PLANT0.BMP` … `PLANT4.BMP` (178×144, 24-bit, one per growth stage) on
a FAT-formatted microSD and the garden renders the hand-drawn grow room. No
card? No problem — the firmware contains a vector rebuild of the exact same
art, so the garden looks right either way. Cards can be written with the
built-in **SD INSTALL MODE** (serial transfer) or any card reader.

## Cheat codes

Open **OPTIONS → CHEATS**, type the code, tap **ACTIVATE**.
(Case-insensitive.)

| Code | Effect |
|---|---|
| `CASH` / `MONEY` | +$500 |
| `SEEDS` | +8 seeds of your planted strain |
| `STASH` | +10 flower jars (A grade) of your planted strain |
| `UNLOCK` | Unlock every strain in the catalog |
| `HARVEST` | Your plant jumps to harvest-ready |
| `TURBO` | Extreme speed + a growth boost for 60 ticks |
| `HYDRO` | Soil soaked to 100% |
| `PEST` | Garden becomes pest-free |
| `CARE` | +5 water packs, care kits and pest kits |
| `SKIP` | Satisfies an open defoliation window safely |

## Building from source

- **arduino-cli** + ESP32 core 3.3.12 (pinned in CI)
- Libraries: TFT_eSPI 2.5.43, NimBLE-Arduino, Adafruit NeoPixel, XPT2046_Touchscreen
- S3 boards need `FlashSize=16M, PSRAM=opi, PartitionScheme=app3M_fat9M_16MB`
- PCM samples (`boot_chant.h`, `theme_loop.h`) are IMA ADPCM blobs generated
  from the source MP3s; the CI workflow copies them next to the sketch

Pushing a tag `vN` builds all three boards and publishes a release the
devices OTA against.

## Firmware versions

- **v39** — WinFlorida rebrand
- **v38** — boot chant, sampled theme loop, hash room wash picker, bigger strain modal
- **v37** — no-SD garden art rebuilt to match the PLANT BMPs
- **v36** — code-only intro (no stale SD caption art)
- **v31–v35** — frame-buffer rendering, smoke breaks on stash, grower-driven cutscenes
- Older — see [CHANGELOG.md](CHANGELOG.md)
