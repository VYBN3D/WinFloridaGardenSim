import { createClient } from "https://esm.sh/@supabase/supabase-js@2";
import { readNativeEconomy, buildNativeSaveSnapshot } from "./garden-native-save.mjs";

const SUPABASE_URL = "https://yttjghsdllyvyrisosya.supabase.co";
const SUPABASE_PUBLISHABLE_KEY = "sb_publishable_uyAhpzUF2RKGviTisA8hUA_r8jfjMrp";
const GAME = document.body.dataset.multiplayerGame || "Garden Sim";
const ECONOMY_KEY = "Garden";
const TOPIC = "garden-nearby-v2";
const STORAGE_NAME = "garden-mp-display-name-v1";

const css = document.createElement("style");
css.textContent = "#gardenMp{position:fixed;right:16px;bottom:16px;width:min(440px,calc(100vw - 24px));max-height:min(82vh,760px);z-index:99999;box-sizing:border-box;font:13px/1.35 ui-monospace,SFMono-Regular,Menlo,monospace;color:#e8f5eb;pointer-events:auto}\n#gardenMp[hidden]{display:none!important}\n#gardenMp .mpPanel{box-sizing:border-box;border:1px solid #477451;border-radius:10px;background:rgba(7,20,12,.97);padding:13px;display:flex;flex-direction:column;gap:9px;box-shadow:0 5px 24px #000a;min-height:170px;max-height:inherit;overflow-y:auto}\n#gardenMp .mpHead{display:flex;gap:10px;justify-content:space-between;align-items:center;color:#b9f0c8;font-weight:700;letter-spacing:.5px;font-size:13px}\n#gardenMp .mpStatus{color:#b6cdbb;font-size:11px;min-height:15px;white-space:normal;overflow-wrap:anywhere}\n#gardenMp .mpStatus[data-state=\"online\"]{color:#72e69a}\n#gardenMp .mpControls{display:flex;gap:6px;min-height:32px}\n#gardenMp input{min-width:0;flex:1;background:#101f15;color:#effff2;border:1px solid #45684d;border-radius:6px;padding:6px 8px;font:12px ui-monospace,monospace}\n#gardenMp button{background:#205a34;color:#eaffef;border:1px solid #5b956a;border-radius:6px;padding:6px 10px;font:12px ui-monospace,monospace;cursor:pointer;white-space:nowrap}\n#gardenMp button:disabled{opacity:.5}\n#gardenMp .mpCount{color:#a5d6b0;font-size:11px}\n#gardenMp ul{list-style:none;margin:0;padding:0;display:grid;grid-template-columns:repeat(auto-fit,minmax(165px,1fr));gap:5px;overflow:visible;min-height:30px;flex:0 0 auto}\n#gardenMp li{margin:0;min-width:0}\n#gardenMp .mpPlayer{width:100%;min-height:42px;display:flex;justify-content:space-between;align-items:center;gap:8px;text-align:left;padding:9px 10px;border-color:#345c40;background:#102318;overflow:hidden}\n#gardenMp .mpPlayer[aria-pressed=\"true\"]{background:#245b37;border-color:#82d99b}\n#gardenMp .mpName{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n#gardenMp .mpGame{color:#9ab9a2;font-size:10px;white-space:nowrap}\n#gardenMp .mpChatHead{color:#b9f0c8;font-size:11px;font-weight:700;border-top:1px solid #345c40;padding-top:7px}\n#gardenMp #mpBadge{margin-left:auto;white-space:nowrap}\n#gardenMp .mpDmToggle{margin-left:0}\n#gardenMp .mpDmView{display:none;border-top:1px solid #345c40;padding-top:9px;gap:8px}\n#gardenMp.mpDmOpen .mpDmView{display:flex;flex-direction:column}\n#gardenMp.mpDmOpen .mpEconomy{display:none}\n#gardenMp.mpDmOpen .mpDmToggle{background:#173d25}\n#gardenMp .mpDmToolbar{display:flex;align-items:center;justify-content:space-between;gap:8px;color:#b9f0c8;font-weight:700;font-size:11px}\n#gardenMp #mpChatLog{height:82px;overflow:auto;background:#09160e;border:1px solid #294a33;border-radius:6px;padding:6px;font-size:11px}\n#gardenMp .mpChatLine{overflow-wrap:anywhere;margin:0 0 5px}\n#gardenMp .mpChatLine b{color:#9de8b0}\n#gardenMp .mpEconomy{border-top:1px solid #345c40;padding-top:8px;display:grid;gap:6px}\n#gardenMp .mpEconomyTitle{color:#b9f0c8;font-weight:700;font-size:11px}\n#gardenMp .mpBalances{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px;color:#b6cdbb;font-size:10px}\n#gardenMp .mpBalances strong{color:#effff2;font-weight:700}\n#gardenMp .mpTransfer{display:grid;grid-template-columns:1.25fr 1.4fr 58px auto;gap:4px}\n#gardenMp select,#gardenMp .mpAmount{min-width:0;background:#101f15;color:#effff2;border:1px solid #45684d;border-radius:6px;padding:6px 5px;font:11px ui-monospace,monospace}\n#gardenMp .mpTransfer button{padding:6px 8px}\n#gardenMp .mpToast{display:none;position:relative;z-index:2;padding:10px 12px;border:1px solid #65bd7b;border-radius:8px;background:linear-gradient(110deg,rgba(20,75,39,.98),rgba(9,35,20,.98));box-shadow:0 4px 18px #0009;color:#effff2;font-size:12px;line-height:1.45;pointer-events:none;animation:mpToastIn .18s ease-out}\n#gardenMp .mpToast.isVisible{display:block}\n#gardenMp .mpToast strong{display:block;color:#a8f5b9;font-size:14px}\n@keyframes mpToastIn{from{opacity:0;transform:translateY(-5px)}to{opacity:1;transform:translateY(0)}}\n@media(max-width:520px){#gardenMp{right:8px;bottom:8px;width:calc(100vw - 16px);max-height:84vh}#gardenMp .mpTransfer{grid-template-columns:1fr 1fr 64px auto}#gardenMp .mpPlayer{min-height:46px}}";
document.head.append(css);

const root = document.createElement("section");
root.id = "gardenMp";
root.innerHTML = `
 <div class="mpPanel">\n  <div class="mpToast" id="mpReceiveToast" role="status" aria-live="polite"></div>
  <div class="mpHead"><span>ONLINE GROWERS</span><span id="mpBadge">OFFLINE</span><button class="mpDmToggle" id="mpDmToggle" type="button" aria-expanded="false">DIRECT MESSAGE</button></div>
  <div class="mpStatus" id="mpStatus" data-state="offline">Join the shared lobby</div>
  <div class="mpControls"><input id="mpName" maxlength="24" autocomplete="nickname" placeholder="Grower name"><button id="mpJoin" type="button">JOIN</button></div>
  <div class="mpCount" id="mpCount">0 online</div>
  <ul id="mpPlayers"><li>Online players appear here</li></ul>
  <div class="mpDmView" id="mpDmView">
   <div class="mpDmToolbar"><span id="mpChatHead">DIRECT MESSAGE · select a grower</span><button id="mpDmBack" type="button">BACK</button></div>
   <div id="mpChatLog" aria-live="polite"></div>
   <div class="mpControls"><input id="mpChatInput" maxlength="240" placeholder="Message selected grower" disabled><button id="mpSend" type="button" disabled>SEND</button></div>
  </div>
  <div class="mpEconomy">
   <div class="mpEconomyTitle">GAME SAVE SYNC</div>
   <div class="mpBalances"><span>Native cash: <strong id="mpCash">not synced</strong></span><span>Supplies: <strong id="mpSupply">not synced</strong></span><span>Flower jars: <strong id="mpFlower">not synced</strong></span><span>Hash jars: <strong id="mpHash">not synced</strong></span></div>
   <div class="mpStatus" id="mpEconomyStatus">Native save integration is being built. The old duplicate test wallet is paused.</div>
   <div class="mpTransfer"><select id="mpAsset" aria-label="Asset to send" disabled><option value="cash">Cash</option><option value="seed">Seeds</option><option value="flower">Flower jars</option><option value="hash">Hash jars</option><option value="water_pack">Water packs</option><option value="care_kit">Care kits</option><option value="pest_kit">Pest kits</option><option value="remediation_kit">Remediation kits</option><option value="bong">Bongs</option><option value="lighter">Lighters</option><option value="paper">Rolling papers</option></select><select id="mpStrain" aria-label="Strain" disabled><option value="0">Northern Lights</option><option value="1">Blue Dream</option><option value="2">Banana Papaya</option><option value="3">Superbuff Cherry</option><option value="4">Jack Herer</option><option value="5">Granddaddy Purple</option><option value="6">White Widow</option><option value="7">Blue Lobster</option><option value="8">OG Kush</option><option value="9">Sour Diesel</option><option value="10">Gelato</option><option value="11">Titan Express</option><option value="12">Zesty Garlic Cookies</option><option value="13">Moon Beam Gelato</option><option value="14">Toad Venom</option><option value="15">Foreign Kushmints</option></select><input class="mpAmount" id="mpAmount" type="number" min="1" max="65535" step="1" value="1" aria-label="Transfer amount" disabled><button id="mpTransferButton" type="button" disabled>SEND</button></div>
  </div>
 </div>`;
document.body.append(root);
root.hidden = false;

const $ = (id) => root.querySelector("#" + id);
function makePlayerId() {
  if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
  // RFC 4122 v4 fallback for HTTP LAN pages where randomUUID may be unavailable.
  const bytes = new Uint8Array(16);
  if (globalThis.crypto?.getRandomValues) globalThis.crypto.getRandomValues(bytes);
  else for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = [...bytes].map(b => b.toString(16).padStart(2, "0")).join("");
  return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20)}`;
}
function isUuid(value) {
  return typeof value === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(value);
}
let id = "";
$("mpName").value = localStorage.getItem(STORAGE_NAME) || "";
let client, channel, joined = false, selectedPlayerId = "";
let nativeSyncReady = false, nativeSyncBlocked = false, nativeSyncRunning = false;
let nativeSyncTimer = 0, nativeSyncAgain = false;
const nativeRevisionKey = () => "garden-native-revision-v1:" + GAME + ":" + id;
const nativeBridge = () => window.__gardenNativeSaveBridge;
window.__gardenNativeSaveChanged = () => {
  const bridge = nativeBridge();
  if (!bridge) return;
  try { localStorage.setItem(bridge.dirtyKey, "1"); } catch {}
  if (!joined || !nativeSyncReady || nativeSyncBlocked) return;
  clearTimeout(nativeSyncTimer);
  nativeSyncTimer = setTimeout(() => syncNativeSave(), 900);
};

function setStatus(message, state = "offline") {
  $("mpStatus").textContent = message;
  $("mpStatus").dataset.state = state;
  $("mpBadge").textContent = state === "online" ? "ONLINE" : state === "connecting" ? "CONNECTING" : "OFFLINE";
}

function updateTransferControls() {
  const bridge = nativeBridge();
  let target = null;
  if (channel && selectedPlayerId) {
    for (const records of Object.values(channel.presenceState())) {
      const candidate = records?.[records.length - 1];
      if (candidate?.player_id === selectedPlayerId) { target = candidate; break; }
    }
  }
  const enabled = joined && nativeSyncReady && !nativeSyncBlocked && !!target && !!bridge;
  $("mpAsset").disabled = !enabled;
  $("mpAmount").disabled = !enabled;
  $("mpTransferButton").disabled = !enabled;
  const usesStrain = ["seed", "flower", "hash"].includes($("mpAsset").value);
  $("mpStrain").disabled = !enabled || !usesStrain;
  if (!nativeSyncReady) $("mpEconomyStatus").textContent = "Sync your native save before transferring items.";
}

function renderPlayers() {
  if (!channel) return;
  const all = channel.presenceState();
  const unique = new Map();
  for (const [key, records] of Object.entries(all)) {
    const p = records?.[records.length - 1];
    if (isUuid(p?.player_id) && p.player_id !== id) unique.set(p.player_id, p);
    else if (isUuid(p?.player_id) && p.player_id === id) unique.set(p.player_id, p);
  }
  const players = [...unique.values()].sort((a,b) => String(a.display_name).localeCompare(String(b.display_name)));
  $("mpCount").textContent = players.length + (players.length === 1 ? " online · select a grower" : " online · select a grower");
  const list = $("mpPlayers");
  list.replaceChildren();
  if (!players.length) {
    const li = document.createElement("li");
    li.textContent = "You're the first grower here. Invite someone in!";
    list.append(li);
    return;
  }
  for (const p of players) {
    const li = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "mpPlayer";
    button.setAttribute("aria-pressed", String(selectedPlayerId === p.player_id));
    const name = document.createElement("span");
    name.className = "mpName";
    name.textContent = (p.player_id === id ? p.display_name + " (you)" : p.display_name || "Grower");
    const game = document.createElement("span");
    game.className = "mpGame";
    game.textContent = p.game || "Garden";
    button.append(name, game);
    button.addEventListener("click", () => {
      selectedPlayerId = p.player_id;
      renderPlayers();
      const target = p.player_id === id ? "yourself" : (p.display_name || "grower");
      $("mpChatHead").textContent = "DIRECT MESSAGE · " + target;
      $("mpChatInput").disabled = p.player_id === id || !joined;
      $("mpSend").disabled = p.player_id === id || !joined;
      $("mpChatLog").replaceChildren();
      updateTransferControls();
    });
    li.append(button);
    list.append(li);
  }
}

async function leaveLobby() {
  joined = false;
  if (channel) {
    const old = channel;
    channel = null;
    try { await old.untrack(); } catch {}
    try { await old.unsubscribe(); } catch {}
  }
  setStatus("Not connected");
}

async function joinLobby() {
  const name = $("mpName").value.trim().slice(0,24);
  if (!name) {
    $("mpName").focus();
    setStatus("Enter a grower name to join");
    return;
  }
  $("mpJoin").disabled = true;
  setStatus("Connecting to shared lobby…", "connecting");
  localStorage.setItem(STORAGE_NAME, name);
  try {
    client ||= createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY, {
      realtime: { params: { eventsPerSecond: 10 } }
    });
    if (channel) await leaveLobby();
    const { data: sessionData, error: sessionError } = await client.auth.getSession();
    if (sessionError) throw sessionError;
    let session = sessionData?.session;
    if (!session) {
      const { data: authData, error: authError } = await client.auth.signInAnonymously();
      if (authError) throw new Error("Anonymous sign-in failed. Enable Anonymous Sign-Ins in Supabase Authentication: " + authError.message);
      session = authData?.session;
    }
    if (!session?.user?.id) throw new Error("Supabase did not return a player identity");
    id = session.user.id;
    const { error: profileError } = await client.rpc("garden_set_display_name", { p_display_name: name });
    if (profileError) throw new Error("Player profile setup failed. Apply the latest economy SQL migration: " + profileError.message);
    channel = client.channel(TOPIC, { config: { presence: { key: id } } });
    channel
      .on("presence", { event: "sync" }, renderPlayers)
      .on("presence", { event: "join" }, renderPlayers)
      .on("presence", { event: "leave" }, renderPlayers)
      .on("broadcast", { event: "native-save-updated" }, ({ payload }) => {
        handleNativeSaveNotification(payload).catch(error => {
          $("mpEconomyStatus").textContent = "Incoming save sync failed: " + (error?.message || "unknown error");
        });
      })
      .on("broadcast", { event: "dm" }, ({ payload }) => {
        if (!payload || (payload.to !== id && payload.from !== id)) return;
        const line = document.createElement("div");
        line.className = "mpChatLine";
        const who = document.createElement("b");
        who.textContent = (payload.from === id ? "You" : (payload.name || "Grower")) + ": ";
        line.append(who, document.createTextNode(String(payload.body || "").slice(0,240)));
        $("mpChatLog").append(line);
        $("mpChatLog").scrollTop = $("mpChatLog").scrollHeight;
      });
    channel.subscribe(async (status) => {
      if (status === "SUBSCRIBED") {
        try {
          await channel.track({
            player_id: id,
            display_name: name,
            game: GAME,
            platform: "web",
            joined_at: new Date().toISOString()
          });
          joined = true;
          setStatus("Connected to the shared garden lobby", "online");
          renderPlayers();
          await initializeNativeSaveSync();
          updateTransferControls();
          if (selectedPlayerId && selectedPlayerId !== id) {
            $("mpChatInput").disabled = false;
            $("mpSend").disabled = false;
            updateTransferControls();
          }
          $("mpJoin").textContent = "LEAVE";
          $("mpJoin").disabled = false;
        } catch (error) {
          setStatus("Presence failed: " + (error?.message || "unknown error"));
          $("mpJoin").disabled = false;
        }
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT" || status === "CLOSED") {
        setStatus("Connection issue: " + status);
        $("mpJoin").textContent = "RETRY";
        $("mpJoin").disabled = false;
      }
    });
  } catch (error) {
    setStatus("Could not connect: " + (error?.message || "unknown error"));
    $("mpJoin").disabled = false;
  }
}

async function handleJoinAction() {
  if ($("mpJoin").disabled) return;
  if (joined) {
    await leaveLobby();
    $("mpJoin").textContent = "JOIN";
    $("mpCount").textContent = "0 growers online";
    $("mpPlayers").replaceChildren();
    $("mpChatInput").disabled = true;
    $("mpSend").disabled = true;
    $("mpAmount").disabled = true;
    $("mpTransferButton").disabled = true;
    $("mpAsset").disabled = true;
    $("mpStrain").disabled = true;
    $("mpChatHead").textContent = "DIRECT MESSAGE · join and select a grower";
    selectedPlayerId = "";
  } else {
    await joinLobby();
  }
}

// Pointer-up works consistently across mouse, touch, and embedded webviews.
const joinButton = $("mpJoin");
let joinActionRunning = false;
let lastJoinActivation = 0;
const activateJoin = (event) => {
  event?.preventDefault?.();
  if (joinActionRunning || Date.now() - lastJoinActivation < 900) return;
  lastJoinActivation = Date.now();
  joinActionRunning = true;
  setStatus("Button tapped. Starting connection…", "connecting");
  Promise.resolve(handleJoinAction()).finally(() => { joinActionRunning = false; });
};
joinButton.addEventListener("pointerdown", activateJoin, true);
joinButton.addEventListener("touchstart", activateJoin, {capture:true, passive:false});
joinButton.addEventListener("click", activateJoin, true);
joinButton.addEventListener("keydown", (event) => {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    handleJoinAction();
  }
});
async function sendPrivateMessage() {
  const body = $("mpChatInput").value.trim().slice(0,240);
  if (!body || !joined || !channel || !selectedPlayerId || selectedPlayerId === id) return;
  const selected = channel.presenceState();
  let target = null;
  for (const records of Object.values(selected)) {
    const candidate = records?.[records.length - 1];
    if (candidate?.player_id === selectedPlayerId) { target = candidate; break; }
  }
  if (!target) { setStatus("That grower is no longer online"); return; }
  try {
    const result = await channel.send({ type: "broadcast", event: "dm", payload: {
      to: selectedPlayerId, from: id, name: $("mpName").value.trim(), body, sent_at: new Date().toISOString()
    }});
    if (result !== "ok") throw new Error("Message was not accepted");
    const line = document.createElement("div");
    line.className = "mpChatLine";
    const who = document.createElement("b");
    who.textContent = "You: ";
    line.append(who, document.createTextNode(body));
    $("mpChatLog").append(line);
    $("mpChatLog").scrollTop = $("mpChatLog").scrollHeight;
    $("mpChatInput").value = "";
  } catch (error) { setStatus("Message failed: " + (error?.message || "unknown error")); }
}
function renderNativeEconomy() {
  const bridge = nativeBridge();
  if (!bridge) {
    $("mpEconomyStatus").textContent = "Waiting for the native game save bridge…";
    return;
  }
  const state = readNativeEconomy(bridge.read());
  $("mpCash").textContent = "$" + state.credits;
  $("mpSupply").textContent = String(state.waterPacks + state.careKits + state.pestKits + state.remediationKits);
  $("mpFlower").textContent = String(state.flower.reduce((a,b) => a+b, 0));
  $("mpHash").textContent = String(state.hash.reduce((a,b) => a+b, 0));
}

async function syncNativeSave() {
  if (!joined || !client || !id || !nativeSyncReady || nativeSyncBlocked) return false;
  if (nativeSyncRunning) { nativeSyncAgain = true; return false; }
  nativeSyncRunning = true;
  try {
    const bridge = nativeBridge();
    if (!bridge) throw new Error("Native game save is not ready");
    const expected = Number(localStorage.getItem(nativeRevisionKey()) || 0);
    if (!Number.isSafeInteger(expected) || expected < 1) throw new Error("Cloud save revision is missing; sync is paused to protect your local save");
    const state = buildNativeSaveSnapshot(bridge.read());
    const { data, error } = await client.rpc("garden_native_save_update", {
      p_game_key: ECONOMY_KEY, p_state: state, p_expected_revision: expected
    });
    if (error) {
      if (/changed elsewhere|revision/i.test(error.message || "")) nativeSyncBlocked = true;
      throw error;
    }
    const row = Array.isArray(data) ? data[0] : data;
    if (!row?.save_revision) throw new Error("Cloud save update returned no revision");
    localStorage.setItem(nativeRevisionKey(), String(row.save_revision));
    localStorage.removeItem(bridge.dirtyKey);
    renderNativeEconomy();
    $("mpEconomyStatus").textContent = "Shared garden save synced · revision " + row.save_revision;
    return true;
  } catch (error) {
    $("mpEconomyStatus").textContent = "Save sync paused: " + (error?.message || "unknown error");
    return false;
  } finally {
    nativeSyncRunning = false;
    if (nativeSyncAgain) {
      nativeSyncAgain = false;
      if (!nativeSyncBlocked) setTimeout(() => syncNativeSave(), 300);
    }
  }
}

function applyCloudNativeSave(state, revision) {
  const bridge = nativeBridge();
  if (!bridge) throw new Error("Native save bridge is unavailable");
  window.__gardenApplyingNativeSave = true;
  try {
    bridge.apply(state);
    localStorage.setItem(nativeRevisionKey(), String(revision));
    localStorage.removeItem(bridge.dirtyKey);
  } finally {
    window.__gardenApplyingNativeSave = false;
  }
  // The game reads NVS through the live Module bridge. Applying the economy
  // fields in place keeps the running simulation and Realtime lobby connected.
  renderNativeEconomy();
  $("mpEconomyStatus").textContent = "Shared garden save updated in place · revision " + revision;
}

async function initializeNativeSaveSync() {
  const bridge = nativeBridge();
  if (!bridge || bridge.gameKey !== GAME) {
    $("mpEconomyStatus").textContent = "Native save bridge does not match this game. Transfers remain paused.";
    return;
  }
  nativeSyncReady = false;
  nativeSyncBlocked = false;
  const localState = buildNativeSaveSnapshot(bridge.read());
  const { data, error } = await client.rpc("garden_native_save_import", {
    p_game_key: ECONOMY_KEY, p_state: localState
  });
  if (error) {
    $("mpEconomyStatus").textContent = "Native save setup failed. Apply the new native-save SQL migration: " + error.message;
    return;
  }
  const row = Array.isArray(data) ? data[0] : data;
  if (!row?.save_revision) {
    $("mpEconomyStatus").textContent = "Cloud save setup returned no revision. Transfers remain paused.";
    return;
  }
  const localRevision = Number(localStorage.getItem(nativeRevisionKey()) || 0);
  const dirty = localStorage.getItem(bridge.dirtyKey) === "1";

  if (row.was_created) {
    localStorage.setItem(nativeRevisionKey(), String(row.save_revision));
    localStorage.removeItem(bridge.dirtyKey);
    nativeSyncReady = true;
    renderNativeEconomy();
    $("mpEconomyStatus").textContent = "Your existing game save was imported into the shared garden economy. No starting balance was added.";
    return;
  }
  if (localRevision === row.save_revision) {
    nativeSyncReady = true;
    renderNativeEconomy();
    if (dirty) await syncNativeSave();
    else $("mpEconomyStatus").textContent = "Shared garden save synced · revision " + row.save_revision;
    return;
  }
  if (dirty) {
    nativeSyncBlocked = true;
    $("mpEconomyStatus").textContent = "Save conflict: this device has local changes and the cloud copy also changed. Nothing was overwritten; transfers are paused.";
    return;
  }
  if (localRevision === 0 || localRevision < row.save_revision) {
    applyCloudNativeSave(row.native_state, row.save_revision);
    // Applying the newer cloud snapshot in place replaces the old reload-based
    // recovery path. Mark sync ready now so transfer controls can be enabled.
    nativeSyncReady = true;
    nativeSyncBlocked = false;
    return;
  }
  nativeSyncBlocked = true;
  $("mpEconomyStatus").textContent = "Save revision mismatch. Nothing was overwritten; transfers are paused.";
}

let receiveToastTimer = 0;
function showReceivedTransfer(transfer, senderName) {
  const labels = {
    cash: "cash", seed: "seeds", flower: "flower jars", hash: "hash jars",
    water_pack: "water packs", care_kit: "care kits", pest_kit: "pest kits",
    remediation_kit: "remediation kits", bong: "bongs", lighter: "lighters", paper: "rolling papers"
  };
  const amount = Number(transfer.amount);
  if (!Number.isInteger(amount) || amount < 1) return;
  const asset = labels[transfer.asset_type] || "items";
  let headline = transfer.asset_type === "cash" ? "+$" + amount.toLocaleString() + " cash received" :
    amount.toLocaleString() + " " + asset + " received";
  if (["seed", "flower", "hash"].includes(transfer.asset_type)) {
    const strain = $("mpStrain").options[Number(transfer.strain_index)]?.textContent;
    if (strain) headline += " · " + strain;
  }
  const toast = $("mpReceiveToast");
  toast.replaceChildren();
  const strong = document.createElement("strong");
  strong.textContent = headline;
  const detail = document.createElement("span");
  detail.textContent = "From " + (senderName || "a nearby grower");
  toast.append(strong, detail);
  toast.classList.add("isVisible");
  clearTimeout(receiveToastTimer);
  receiveToastTimer = setTimeout(() => toast.classList.remove("isVisible"), 4200);
}

async function handleNativeSaveNotification(payload) {
  if (!payload || payload.to !== id || !isUuid(payload.transfer_id)) return;
  const receipt = await client.from("garden_native_transfers").select("id,sender_id,asset_type,amount,strain_index")
    .eq("id", payload.transfer_id).eq("recipient_id", id).eq("game_key", ECONOMY_KEY).maybeSingle();
  if (receipt.error || !receipt.data) return;
  const saved = await client.from("garden_native_saves").select("state,revision")
    .eq("user_id", id).eq("game_key", ECONOMY_KEY).single();
  if (saved.error || !saved.data) return;
  const localRevision = Number(localStorage.getItem(nativeRevisionKey()) || 0);
  if (saved.data.revision <= localRevision) return;
  applyCloudNativeSave(saved.data.state, saved.data.revision);
  let senderName = "";
  if (channel) {
    for (const records of Object.values(channel.presenceState())) {
      const player = records?.[records.length - 1];
      if (player?.player_id === receipt.data.sender_id) {
        senderName = player.display_name || "";
        break;
      }
    }
  }
  showReceivedTransfer(receipt.data, senderName);
}

async function sendAsset() {
  const amount = Number($("mpAmount").value);
  const asset = $("mpAsset").value;
  const strain = ["seed", "flower", "hash"].includes(asset) ? Number($("mpStrain").value) : null;
  if (!Number.isInteger(amount) || amount < 1 || amount > 65535) {
    $("mpEconomyStatus").textContent = "Enter a whole number from 1 to 65,535.";
    return;
  }
  if (!joined || !channel || !isUuid(id) || !isUuid(selectedPlayerId) || selectedPlayerId === id) {
    $("mpEconomyStatus").textContent = "Join and select another online grower first.";
    return;
  }
  let target = null;
  for (const records of Object.values(channel.presenceState())) {
    const candidate = records?.[records.length - 1];
    if (candidate?.player_id === selectedPlayerId) { target = candidate; break; }
  }
  if (!target) {
    $("mpEconomyStatus").textContent = "That grower is no longer online.";
    updateTransferControls();
    return;
  }
  if (!nativeSyncReady || nativeSyncBlocked) {
    $("mpEconomyStatus").textContent = "Native save sync is not ready. Transfer blocked to protect your save.";
    return;
  }
  $("mpTransferButton").disabled = true;
  $("mpEconomyStatus").textContent = "Syncing your native save before transfer…";
  try {
    const synced = await syncNativeSave();
    if (!synced) throw new Error("Your save could not be synced safely. No transfer was sent.");
    const { data, error } = await client.rpc("garden_native_transfer", {
      p_game_key: ECONOMY_KEY,
      p_recipient_id: selectedPlayerId,
      p_asset_type: asset,
      p_amount: amount,
      p_strain_index: strain,
      p_idempotency_key: makePlayerId()
    });
    if (error) throw error;
    const transferId = data;
    const notification = await channel.send({
      type: "broadcast", event: "native-save-updated",
      payload: { to: selectedPlayerId, transfer_id: transferId }
    });
    const saved = await client.from("garden_native_saves").select("state,revision")
      .eq("user_id", id).eq("game_key", ECONOMY_KEY).single();
    if (saved.error || !saved.data) throw new Error("Transfer completed, but the updated save could not be loaded. Rejoin to sync it.");
    applyCloudNativeSave(saved.data.state, saved.data.revision);
    $("mpAmount").value = "";
    if (notification === "ok") {
      $("mpEconomyStatus").textContent = "Transfer complete · your game and lobby stayed live.";
    } else {
      $("mpEconomyStatus").textContent = "Transfer complete · recipient will sync when their client reconnects.";
    }
    updateTransferControls();
  } catch (error) {
    $("mpEconomyStatus").textContent = "Transfer failed: " + (error?.message || "unknown error");
    $("mpTransferButton").disabled = false;
  }
}
$("mpAsset").addEventListener("change", updateTransferControls);
$("mpTransferButton").addEventListener("click", sendAsset);

$("mpSend").addEventListener("click", sendPrivateMessage);
$("mpDmToggle").addEventListener("click", () => {
  const open = !root.classList.contains("mpDmOpen");
  root.classList.toggle("mpDmOpen", open);
  $("mpDmToggle").setAttribute("aria-expanded", String(open));
  $("mpDmToggle").textContent = open ? "CLOSE MESSAGE" : "DIRECT MESSAGE";
  if (open && selectedPlayerId && selectedPlayerId !== id) $("mpChatInput").focus();
});
$("mpDmBack").addEventListener("click", () => {
  root.classList.remove("mpDmOpen");
  $("mpDmToggle").setAttribute("aria-expanded", "false");
  $("mpDmToggle").textContent = "DIRECT MESSAGE";
});
$("mpChatInput").addEventListener("keydown", event => { if (event.key === "Enter") sendPrivateMessage(); });
$("mpName").addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !joined) joinLobby();
});
window.addEventListener("beforeunload", () => {
  if (channel) channel.untrack();
});
