const state = {
  port: null,
  reader: null,
  keepReading: false,
  buffer: "",
  connected: false,
  status: null,
  menus: new Map(),
  items: new Map(),
  snapshotMenus: new Map(),
  snapshotItems: new Map(),
  activeTab: 1,
  pollTimer: null,
  sendChain: Promise.resolve(),
};

const elements = {
  connectButton: document.getElementById("connectButton"),
  reconnectButton: document.getElementById("reconnectButton"),
  disconnectButton: document.getElementById("disconnectButton"),
  refreshButton: document.getElementById("refreshButton"),
  clearLogButton: document.getElementById("clearLogButton"),
  connectionBadge: document.getElementById("connectionBadge"),
  statusGrid: document.getElementById("statusGrid"),
  liveScreen: document.getElementById("liveScreen"),
  liveModeChip: document.getElementById("liveModeChip"),
  liveDriveChip: document.getElementById("liveDriveChip"),
  liveCommChip: document.getElementById("liveCommChip"),
  liveRpmValue: document.getElementById("liveRpmValue"),
  liveTargetValue: document.getElementById("liveTargetValue"),
  weightValue: document.getElementById("weightValue"),
  rpmMatchValue: document.getElementById("rpmMatchValue"),
  weightFill: document.getElementById("weightFill"),
  rpmMatchFill: document.getElementById("rpmMatchFill"),
  torqueGauge: document.getElementById("torqueGauge"),
  weightGauge: document.getElementById("weightGauge"),
  torqueGaugeValue: document.getElementById("torqueGaugeValue"),
  weightGaugeValue: document.getElementById("weightGaugeValue"),
  menuTabs: document.getElementById("menuTabs"),
  menuContent: document.getElementById("menuContent"),
  logOutput: document.getElementById("logOutput"),
  setRpmInput: document.getElementById("setRpmInput"),
  setWeightInput: document.getElementById("setWeightInput"),
  statusCardTemplate: document.getElementById("statusCardTemplate"),
};

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function setConnectedUi(connected) {
  state.connected = connected;
  elements.connectButton.disabled = connected;
  elements.reconnectButton.disabled = connected;
  elements.disconnectButton.disabled = !connected;
  elements.refreshButton.disabled = !connected;
  elements.connectionBadge.textContent = connected ? "USB Online" : "Offline";
  elements.connectionBadge.className = `badge ${connected ? "connected" : "disconnected"}`;
}

function appendLog(line, kind = "raw") {
  const div = document.createElement("div");
  if (kind !== "raw") div.className = kind;
  div.textContent = line;
  elements.logOutput.appendChild(div);
  elements.logOutput.scrollTop = elements.logOutput.scrollHeight;
}

function statusEntries(status) {
  if (!status) {
    return [
      ["State", "No data"],
      ["Drive", "--"],
      ["UART", "--"],
      ["Motor", "--"],
      ["Scale", "--"],
      ["Error", "--"],
    ];
  }

  return [
    ["State", status.state],
    ["Drive", status.drive],
    ["UART", status.uart],
    ["JMC Baud", `${status.jmcBaud ?? "--"}`],
    ["Pref Baud", `${status.jmcPreferredBaud ?? "--"}`],
    ["Comm", status.comm],
    ["Motor", status.motor],
    ["Scale", status.scale],
    ["Set RPM", `${status.setRPM ?? 0}`],
    ["Live RPM", `${status.currentRPM ?? 0}`],
    ["Torque", `${status.torque ?? 0}`],
    ["Set Weight", `${status.setWeight ?? 0}`],
    ["Weight", `${status.weight ?? 0}`],
    ["Error", `${status.error ?? 0} ${status.errorText ?? ""}`.trim()],
  ];
}

function renderStatus() {
  elements.statusGrid.innerHTML = "";
  const fragment = document.createDocumentFragment();

  for (const [label, value] of statusEntries(state.status)) {
    const node = elements.statusCardTemplate.content.firstElementChild.cloneNode(true);
    node.querySelector(".status-label").textContent = label;
    node.querySelector(".status-value").textContent = value;
    fragment.appendChild(node);
  }

  elements.statusGrid.appendChild(fragment);

  if (state.status) {
    elements.setRpmInput.value = state.status.setRPM ?? 0;
    elements.setWeightInput.value = state.status.setWeight ?? 0;
  }
}

function clampPercent(value) {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(100, value));
}

function renderLiveScreen() {
  const status = state.status;
  if (!status) {
    elements.liveScreen.className = "live-screen idle";
    elements.liveModeChip.textContent = "Idle";
    elements.liveDriveChip.textContent = "--";
    elements.liveCommChip.textContent = "--";
    elements.liveRpmValue.textContent = "0";
    elements.liveTargetValue.textContent = "Target 0";
    elements.weightValue.textContent = "0 / 0 mg";
    elements.rpmMatchValue.textContent = "0%";
    elements.torqueGaugeValue.textContent = "0%";
    elements.weightGaugeValue.textContent = "0%";
    elements.weightFill.style.width = "0%";
    elements.rpmMatchFill.style.width = "0%";
    elements.torqueGauge.style.setProperty("--progress", "0deg");
    elements.weightGauge.style.setProperty("--progress", "0deg");
    return;
  }

  const running = ["GRINDING", "GRINDING_GBW", "CALIBRATING"].includes(status.state);
  const purging = status.state === "PURGING";
  elements.liveScreen.className = `live-screen ${purging ? "purging" : running ? "running" : "idle"}`;

  elements.liveModeChip.textContent = status.state;
  elements.liveDriveChip.textContent = `${status.drive} ${status.motor}`;
  elements.liveCommChip.textContent = `${status.uart} / ${status.comm}`;

  const liveRpm = Math.abs(Number(status.currentRPM ?? 0));
  const targetRpm = Math.abs(Number(status.motorSetRPM || status.setRPM || 0));
  const torquePercent = clampPercent(Number(status.torquePercent ?? 0));
  const weight = Math.max(0, Number(status.weight ?? 0));
  const targetWeight = Math.max(0, Number(status.setWeight ?? 0));
  const weightPercent = targetWeight > 0 ? clampPercent((weight / targetWeight) * 100) : 0;
  const rpmMatch = targetRpm > 0 ? clampPercent((liveRpm / targetRpm) * 100) : 0;

  elements.liveRpmValue.textContent = `${liveRpm}`;
  elements.liveTargetValue.textContent = `Target ${targetRpm}`;
  elements.weightValue.textContent = `${weight} / ${targetWeight} mg`;
  elements.rpmMatchValue.textContent = `${Math.round(rpmMatch)}%`;
  elements.torqueGaugeValue.textContent = `${Math.round(torquePercent)}%`;
  elements.weightGaugeValue.textContent = `${Math.round(weightPercent)}%`;

  elements.weightFill.style.width = `${weightPercent}%`;
  elements.rpmMatchFill.style.width = `${rpmMatch}%`;
  elements.torqueGauge.style.setProperty("--progress", `${(torquePercent / 100) * 360}deg`);
  elements.weightGauge.style.setProperty("--progress", `${(weightPercent / 100) * 360}deg`);
}

function ensureActiveTab() {
  if (!state.menus.has(state.activeTab)) {
    const firstMenu = [...state.menus.keys()].sort((a, b) => a - b)[0] ?? 1;
    state.activeTab = firstMenu;
  }
}

function renderMenuTabs() {
  ensureActiveTab();
  elements.menuTabs.innerHTML = "";

  const menuIds = [...state.menus.keys()].sort((a, b) => a - b);
  for (const menuId of menuIds) {
    const menu = state.menus.get(menuId);
    const button = document.createElement("button");
    button.className = `tab ${state.activeTab === menuId ? "active" : ""}`;
    button.textContent = menu.label;
    button.addEventListener("click", () => {
      state.activeTab = menuId;
      renderMenus();
    });
    elements.menuTabs.appendChild(button);
  }
}

function menuItems(menuId) {
  const items = state.items.get(menuId) ?? [];
  return [...items].sort((a, b) => a.index - b.index);
}

function createMenuHeader(item) {
  const header = document.createElement("div");
  header.className = "menu-item-head";

  const titleWrap = document.createElement("div");
  const title = document.createElement("h3");
  title.className = "menu-item-title";
  title.textContent = item.name;
  titleWrap.appendChild(title);

  const meta = document.createElement("div");
  meta.className = "menu-item-meta";
  const unit = item.unit ? ` ${item.unit}` : "";
  meta.textContent = item.kind === "value"
    ? `${item.min}..${item.max}${unit} step ${item.scalar || 1}`
    : item.kind === "bool"
      ? "On / off"
      : item.kind === "baud_action"
        ? `Target ${item.baud}`
        : item.kind;

  header.appendChild(titleWrap);
  header.appendChild(meta);
  return header;
}

function enqueueSnapshot(delayMs = 250) {
  window.clearTimeout(enqueueSnapshot.timer);
  enqueueSnapshot.timer = window.setTimeout(() => {
    if (state.connected) sendCommand("api snapshot");
  }, delayMs);
}

function createValueEditor(menuId, item) {
  const row = document.createElement("div");
  row.className = "value-row";

  const input = document.createElement("input");
  input.type = "number";
  input.value = item.value;
  input.min = item.min;
  input.max = item.max;
  input.step = item.scalar || 1;
  row.appendChild(input);

  const button = document.createElement("button");
  button.textContent = "Save";
  button.addEventListener("click", async () => {
    await sendCommand(`api set ${menuId} ${item.index} ${input.value}`);
    enqueueSnapshot();
  });
  row.appendChild(button);

  return row;
}

function createBoolEditor(menuId, item) {
  const actions = document.createElement("div");
  actions.className = "menu-item-actions";

  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.checked = Number(item.value) !== 0;
  actions.appendChild(checkbox);

  const button = document.createElement("button");
  button.textContent = "Save";
  button.addEventListener("click", async () => {
    await sendCommand(`api set ${menuId} ${item.index} ${checkbox.checked ? 1 : 0}`);
    enqueueSnapshot();
  });
  actions.appendChild(button);

  return actions;
}

function createActionButton(label, command, confirmText = "") {
  const button = document.createElement("button");
  button.textContent = label;
  button.addEventListener("click", async () => {
    if (confirmText && !window.confirm(confirmText)) return;
    await sendCommand(command);
    enqueueSnapshot(600);
  });
  return button;
}

function renderMenuItem(menuId, item) {
  if (item.kind === "nav" || item.kind === "submenu") return null;

  const card = document.createElement("div");
  card.className = "menu-item";
  card.appendChild(createMenuHeader(item));

  if (item.kind === "value") {
    card.appendChild(createValueEditor(menuId, item));
  } else if (item.kind === "bool") {
    card.appendChild(createBoolEditor(menuId, item));
  } else if (item.kind === "action") {
    const actions = document.createElement("div");
    actions.className = "menu-item-actions";
    actions.appendChild(createActionButton("Run", `api action ${item.action}`));
    card.appendChild(actions);
  } else if (item.kind === "danger_action") {
    const actions = document.createElement("div");
    actions.className = "menu-item-actions";
    actions.appendChild(
      createActionButton(
        "Reset Defaults",
        "api action reset_defaults confirm",
        "Reset defaults and reboot the board?"
      )
    );
    card.appendChild(actions);
  } else if (item.kind === "baud_action") {
    const actions = document.createElement("div");
    actions.className = "menu-item-actions";
    const button = createActionButton("Switch", `api action jmc_baud ${item.baud}`);
    actions.appendChild(button);
    const chip = document.createElement("span");
    chip.className = item.activeBaud ? "active-chip" : "subtle-chip";
    chip.textContent = item.activeBaud ? `✓ ${item.baud}` : `${item.baud}`;
    actions.appendChild(chip);
    card.appendChild(actions);
  }

  return card;
}

function renderMenus() {
  renderMenuTabs();
  elements.menuContent.innerHTML = "";
  const items = menuItems(state.activeTab);

  if (!items.length) {
    const empty = document.createElement("div");
    empty.className = "menu-item";
    empty.textContent = "No menu data yet.";
    elements.menuContent.appendChild(empty);
    return;
  }

  for (const item of items) {
    const rendered = renderMenuItem(state.activeTab, item);
    if (rendered) elements.menuContent.appendChild(rendered);
  }
}

function processJsonMessage(message) {
  switch (message.type) {
    case "snapshot_begin":
      state.snapshotMenus = new Map();
      state.snapshotItems = new Map();
      break;
    case "status":
      state.status = message;
      renderStatus();
      renderLiveScreen();
      break;
    case "menu":
      state.snapshotMenus.set(message.menu, message);
      if (!state.snapshotItems.has(message.menu)) state.snapshotItems.set(message.menu, []);
      break;
    case "item": {
      if (!state.snapshotItems.has(message.menu)) state.snapshotItems.set(message.menu, []);
      state.snapshotItems.get(message.menu).push(message);
      break;
    }
    case "snapshot_end":
      state.menus = state.snapshotMenus;
      state.items = state.snapshotItems;
      renderMenus();
      break;
    case "ack":
      appendLog(`[ack] ${message.cmd}: ${message.ok ? "ok" : "fail"}${message.message ? ` - ${message.message}` : ""}`, message.ok ? "json" : "err");
      break;
    default:
      appendLog(JSON.stringify(message), "json");
      break;
  }
}

function handleLine(line) {
  if (!line) return;
  const trimmed = line.trim();
  if (!trimmed) return;
  appendLog(trimmed, trimmed.startsWith("{") ? "json" : "raw");

  if (!trimmed.startsWith("{")) return;

  try {
    processJsonMessage(JSON.parse(trimmed));
  } catch {
    appendLog(`[parse] ${trimmed}`, "err");
  }
}

async function readLoop() {
  while (state.port?.readable && state.keepReading) {
    state.reader = state.port.readable.getReader();
    try {
      while (state.keepReading) {
        const { value, done } = await state.reader.read();
        if (done) break;
        state.buffer += decoder.decode(value, { stream: true });
        const lines = state.buffer.split(/\r?\n/);
        state.buffer = lines.pop() ?? "";
        for (const line of lines) handleLine(line);
      }
    } catch (error) {
      appendLog(`[read] ${error.message}`, "err");
    } finally {
      state.reader.releaseLock();
      state.reader = null;
    }
  }
}

async function writeCommand(command) {
  if (!state.port?.writable) throw new Error("serial port is not writable");
  const writer = state.port.writable.getWriter();
  try {
    appendLog(`> ${command}`, "out");
    await writer.write(encoder.encode(`${command}\n`));
  } finally {
    writer.releaseLock();
  }
}

function sendCommand(command) {
  state.sendChain = state.sendChain
    .then(() => writeCommand(command))
    .catch((error) => appendLog(`[write] ${error.message}`, "err"));
  return state.sendChain;
}

async function disconnect() {
  window.clearInterval(state.pollTimer);
  state.pollTimer = null;
  state.keepReading = false;

  if (state.reader) {
    try {
      await state.reader.cancel();
    } catch {
      // ignore
    }
  }

  if (state.port) {
    try {
      await state.port.close();
    } catch {
      // ignore
    }
  }

  state.port = null;
  state.reader = null;
  setConnectedUi(false);
}

async function connectPort(port) {
  if (!("serial" in navigator)) {
    appendLog("Web Serial is not available in this browser.", "err");
    return;
  }

  await disconnect();
  await port.open({ baudRate: 115200, bufferSize: 4096 });
  state.port = port;
  state.keepReading = true;
  setConnectedUi(true);
  appendLog("[port] connected", "json");
  readLoop();
  sendCommand("api snapshot");
  state.pollTimer = window.setInterval(() => {
    if (state.connected) sendCommand("api snapshot");
  }, 1200);
}

async function requestPort() {
  try {
    const port = await navigator.serial.requestPort();
    await connectPort(port);
  } catch (error) {
    appendLog(`[connect] ${error.message}`, "err");
  }
}

async function reconnectGrantedPort() {
  try {
    const ports = await navigator.serial.getPorts();
    if (!ports.length) {
      appendLog("[connect] no granted ports found", "err");
      return;
    }
    await connectPort(ports[0]);
  } catch (error) {
    appendLog(`[connect] ${error.message}`, "err");
  }
}

function bindQuickControls() {
  document.querySelectorAll("[data-command='setrpm']").forEach((button) => {
    button.addEventListener("click", async () => {
      await sendCommand(`api setrpm ${elements.setRpmInput.value || 0}`);
      enqueueSnapshot();
    });
  });

  document.querySelectorAll("[data-command='setweight']").forEach((button) => {
    button.addEventListener("click", async () => {
      await sendCommand(`api setweight ${elements.setWeightInput.value || 0}`);
      enqueueSnapshot();
    });
  });

  document.querySelectorAll("[data-action]").forEach((button) => {
    button.addEventListener("click", async () => {
      await sendCommand(`api action ${button.dataset.action}`);
      enqueueSnapshot();
    });
  });

  document.querySelectorAll("[data-drive]").forEach((button) => {
    button.addEventListener("click", async () => {
      await sendCommand(`api action drive ${button.dataset.drive}`);
      enqueueSnapshot();
    });
  });
}

elements.connectButton.addEventListener("click", requestPort);
elements.reconnectButton.addEventListener("click", reconnectGrantedPort);
elements.disconnectButton.addEventListener("click", disconnect);
elements.refreshButton.addEventListener("click", () => sendCommand("api snapshot"));
elements.clearLogButton.addEventListener("click", () => {
  elements.logOutput.textContent = "";
});

bindQuickControls();
renderStatus();
renderLiveScreen();
renderMenus();
setConnectedUi(false);
