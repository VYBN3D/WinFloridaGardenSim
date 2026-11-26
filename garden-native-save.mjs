const STRAINS = 16;
const U8_MAX = 255;
const U16_MAX = 65535;

function readInt(value, fallback = 0, max = U16_MAX) {
  const n = value === undefined || value === null || value === "" ? fallback : Number(value);
  if (!Number.isInteger(n) || n < 0 || n > max) throw new Error("Invalid native save number");
  return n;
}

function decodeHex(value, byteLength, key) {
  if (typeof value !== "string" || value.length !== byteLength * 2 || !/^[0-9a-f]*$/i.test(value)) {
    throw new Error("Native save field " + key + " must be exactly " + (byteLength * 2) + " hex characters");
  }
  return Uint8Array.from({length: byteLength}, (_, i) => parseInt(value.slice(i * 2, i * 2 + 2), 16));
}

function encodeHex(bytes) {
  return Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
}

function readU8Array(nvs, key, fallback = new Uint8Array(STRAINS)) {
  if (nvs[key] === undefined || nvs[key] === "") return fallback.slice();
  return decodeHex(String(nvs[key]), STRAINS, key);
}

function readU16Array(nvs, key, legacyKey) {
  let bytes;
  if (nvs[key] !== undefined && nvs[key] !== "") {
    bytes = decodeHex(String(nvs[key]), STRAINS * 2, key);
  } else {
    bytes = new Uint8Array(STRAINS * 2);
    if (legacyKey && nvs[legacyKey] !== undefined) {
      const legacy = readInt(nvs[legacyKey], 0, U16_MAX);
      bytes[6] = legacy & 255;
      bytes[7] = legacy >>> 8;
    }
  }
  return Array.from({length: STRAINS}, (_, i) => bytes[i * 2] | (bytes[i * 2 + 1] << 8));
}

function writeU16Array(values) {
  const bytes = new Uint8Array(STRAINS * 2);
  values.forEach((value, i) => {
    bytes[i * 2] = value & 255;
    bytes[i * 2 + 1] = value >>> 8;
  });
  return encodeHex(bytes);
}

function readSeeds(nvs) {
  if (nvs.seedArr !== undefined && nvs.seedArr !== "") return Array.from(readU8Array(nvs, "seedArr"));
  const seeds = new Uint8Array(STRAINS);
  const strain = readInt(nvs.seedType, 0, STRAINS - 1);
  seeds[strain] = readInt(nvs.seeds, 0, U8_MAX);
  return Array.from(seeds);
}

function readQuality(nvs, key) {
  if (nvs[key] !== undefined && nvs[key] !== "") return Array.from(readU8Array(nvs, key));
  // Match the firmware's original initializer exactly: twelve 70s, then four zeroes.
  return [...Array(12).fill(70), 0, 0, 0, 0];
}

export function readNativeEconomy(nvs) {
  if (!nvs || typeof nvs !== "object" || Array.isArray(nvs)) throw new Error("Native save is not an object");
  return {
    credits: readInt(nvs.credits, 18, U16_MAX),
    seeds: readSeeds(nvs),
    flower: readU16Array(nvs, "stashArr", "stash"),
    hash: readU16Array(nvs, "hashArr", "hash"),
    flowerQuality: readQuality(nvs, "sqArr"),
    hashQuality: readQuality(nvs, "hqArr"),
    rawFlower: readInt(nvs.rawFlower, 0, U16_MAX),
    rawStrain: readInt(nvs.rawStrain, 0, STRAINS - 1),
    rawQuality: readInt(nvs.rawQual, 70, U8_MAX),
    waterPacks: readInt(nvs.waterPk, 3, U8_MAX),
    careKits: readInt(nvs.careKit, 2, U8_MAX),
    pestKits: readInt(nvs.pestKit, 2, U8_MAX),
    remediationKits: readInt(nvs.remKit, 1, U8_MAX),
    bongs: readInt(nvs.bongs, 0, U8_MAX),
    lighters: readInt(nvs.lighters, 0, U8_MAX),
    papers: readInt(nvs.papers, 0, U8_MAX)
  };
}

function cloneNvs(nvs) {
  return {...nvs};
}

function blendQuality(oldQuality, oldUnits, incomingQuality, incomingUnits) {
  const total = oldUnits + incomingUnits;
  if (!total) return incomingQuality;
  return Math.floor((oldQuality * oldUnits + incomingQuality * incomingUnits) / total);
}

export function applyNativeTransfer(senderNvs, recipientNvs, transfer) {
  const sender = cloneNvs(senderNvs);
  const recipient = cloneNvs(recipientNvs);
  const s = readNativeEconomy(sender);
  const r = readNativeEconomy(recipient);
  const amount = readInt(transfer?.amount, -1, U16_MAX);
  if (amount < 1) throw new Error("Transfer amount must be a positive integer");
  const strain = transfer?.strain;
  const type = transfer?.type;

  if (type === "cash") {
    if (s.credits < amount) throw new Error("Not enough cash");
    if (r.credits + amount > U16_MAX) throw new Error("Recipient cash balance would exceed the game's limit");
    sender.credits = String(s.credits - amount);
    recipient.credits = String(r.credits + amount);
  } else if (type === "seed" || type === "flower" || type === "hash") {
    if (!Number.isInteger(strain) || strain < 0 || strain >= STRAINS) throw new Error("Choose a valid strain");
    const source = type === "seed" ? s.seeds : type === "flower" ? s.flower : s.hash;
    const target = type === "seed" ? r.seeds : type === "flower" ? r.flower : r.hash;
    const max = type === "seed" ? U8_MAX : U16_MAX;
    if (source[strain] < amount) throw new Error("Not enough " + type);
    if (target[strain] + amount > max) throw new Error("Recipient " + type + " count would exceed the game's limit");
    const recipientOldCount = target[strain];
    source[strain] -= amount;
    target[strain] += amount;
    if (type === "seed") {
      sender.seedArr = encodeHex(Uint8Array.from(s.seeds));
      recipient.seedArr = encodeHex(Uint8Array.from(r.seeds));
    } else if (type === "flower") {
      const q = s.flowerQuality[strain];
      r.flowerQuality[strain] = blendQuality(r.flowerQuality[strain], recipientOldCount, q, amount);
      sender.stashArr = writeU16Array(s.flower);
      recipient.stashArr = writeU16Array(r.flower);
      recipient.sqArr = encodeHex(Uint8Array.from(r.flowerQuality));
    } else {
      const q = s.hashQuality[strain];
      r.hashQuality[strain] = blendQuality(r.hashQuality[strain], recipientOldCount, q, amount);
      sender.hashArr = writeU16Array(s.hash);
      recipient.hashArr = writeU16Array(r.hash);
      recipient.hqArr = encodeHex(Uint8Array.from(r.hashQuality));
    }
  } else {
    const fields = {
      water_pack: ["waterPk", "waterPacks"],
      care_kit: ["careKit", "careKits"],
      pest_kit: ["pestKit", "pestKits"],
      remediation_kit: ["remKit", "remediationKits"],
      bong: ["bongs", "bongs"],
      lighter: ["lighters", "lighters"],
      paper: ["papers", "papers"]
    };
    const field = fields[type];
    if (!field) throw new Error("Unsupported native item type");
    const [key, property] = field;
    if (s[property] < amount) throw new Error("Not enough " + type);
    if (r[property] + amount > U8_MAX) throw new Error("Recipient item count would exceed the game's limit");
    sender[key] = String(s[property] - amount);
    recipient[key] = String(r[property] + amount);
  }

  return {sender, recipient};
}

export function buildNativeSaveSnapshot(nvs) {
  const state = readNativeEconomy(nvs);
  return {
    credits: String(state.credits),
    seedArr: encodeHex(Uint8Array.from(state.seeds)),
    stashArr: writeU16Array(state.flower),
    hashArr: writeU16Array(state.hash),
    sqArr: encodeHex(Uint8Array.from(state.flowerQuality)),
    hqArr: encodeHex(Uint8Array.from(state.hashQuality)),
    rawFlower: String(state.rawFlower),
    rawStrain: String(state.rawStrain),
    rawQual: String(state.rawQuality),
    waterPk: String(state.waterPacks),
    careKit: String(state.careKits),
    pestKit: String(state.pestKits),
    remKit: String(state.remediationKits),
    bongs: String(state.bongs),
    lighters: String(state.lighters),
    papers: String(state.papers)
  };
}

export const NATIVE_SAVE_LIMITS = Object.freeze({strains: STRAINS, uint8: U8_MAX, uint16: U16_MAX});
