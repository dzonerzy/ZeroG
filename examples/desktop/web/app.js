// The page of the desktop demo: buttons call Desktop.* (main.c), and every
// event the native side reports ("native" CustomEvents) goes to the log.

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => document.querySelectorAll(selector);

function log(type, detail) {
  const item = document.createElement("li");
  const time = new Date().toLocaleTimeString();
  item.innerHTML = `<span class="time">${time}</span><span class="type"></span><span class="detail"></span>`;
  item.querySelector(".type").textContent = type;
  item.querySelector(".detail").textContent = detail === undefined ? "" : JSON.stringify(detail);
  const list = $("#log");
  list.prepend(item);
  while (list.children.length > 200)
    list.lastChild.remove();
}

function refreshStatus() {
  const info = Desktop.info();
  const where = info.x === null ? "no position (Wayland)" : `${info.x}, ${info.y}`;
  $("#status").textContent = `${info.state} · ${info.width}×${info.height} · ${where} · screen ${info.screen}`;
}

function showPaths(paths, label) {
  const list = $("#paths");
  list.innerHTML = "";
  if (!paths.length) {
    list.textContent = `${label}: cancelled`;
    return;
  }
  for (const path of paths) {
    const item = document.createElement("li");
    item.textContent = `${label}: ${path}`;
    list.append(item);
  }
}

// ---- Native events ----

window.addEventListener("native", (event) => {
  const { type, ...detail } = event.detail;
  log(type, detail);
  if (type === "file_dialog")
    showPaths(detail.paths, detail.kind);
  if (type === "files_dropped")
    showPaths(detail.paths, "dropped");
  if (type === "state_changed")
    $("#fullscreen").checked = detail.state === "fullscreen";
  refreshStatus();
});

// ---- Window ----

for (const button of $$("[data-call]")) {
  button.addEventListener("click", () => {
    const argument = button.dataset.arg;
    const result = argument === undefined ? Desktop[button.dataset.call]() : Desktop[button.dataset.call](argument === "true");
    if (result === false)
      log("refused", button.dataset.call);
  });
}

$("#hide").addEventListener("click", () => {
  Desktop.hide();
  // The page keeps running while its window is hidden.
  setTimeout(() => Desktop.show(), 2000);
});
$("#setSize").addEventListener("click", () => Desktop.setSize(+$("#width").value, +$("#height").value) || log("refused", "setSize"));
$("#setPosition").addEventListener("click", () => Desktop.setPosition(+$("#x").value, +$("#y").value) || log("refused", "setPosition (Wayland places windows itself)"));
$("#minSize").addEventListener("click", () => Desktop.setMinSize(800, 600));
$("#maxSize").addEventListener("click", () => Desktop.setMaxSize(1200, 800));
$("#noLimits").addEventListener("click", () => { Desktop.setMinSize(640, 480); Desktop.setMaxSize(0, 0); });
$("#resizable").addEventListener("change", (event) => Desktop.resizable(event.target.checked));
$("#onTop").addEventListener("change", (event) => Desktop.alwaysOnTop(event.target.checked));
$("#fullscreen").addEventListener("change", (event) => Desktop.fullscreen(event.target.checked));
$("#setTitle").addEventListener("click", () => Desktop.title($("#title").value));

// ---- Screens ----

function showScreens() {
  const cards = $("#screens");
  cards.innerHTML = "";
  for (const screen of Desktop.screens()) {
    const card = document.createElement("div");
    card.className = "card";
    card.innerHTML = `<b></b><br>bounds ${screen.width}×${screen.height} at ${screen.x}, ${screen.y}` +
      `<br>work area ${screen.workWidth}×${screen.workHeight} at ${screen.workX}, ${screen.workY}` +
      `<br>scale ${screen.scale} · ${screen.refresh} Hz${screen.primary ? " · primary" : ""}`;
    card.querySelector("b").textContent = screen.name || "(unnamed)";
    cards.append(card);
  }
}
$("#refreshScreens").addEventListener("click", showScreens);

// ---- Dialogs ----

for (const button of $$("[data-message]"))
  button.addEventListener("click", () => Desktop.message(+button.dataset.message));

// ---- Menus ----

$("#enableSave").addEventListener("change", (event) => Desktop.enableSave(event.target.checked));
$("#popup").addEventListener("click", (event) => {
  const box = event.target.getBoundingClientRect();
  Desktop.popup(box.left, box.bottom);
});

// ---- Clipboard ----

$("#copyText").addEventListener("click", () => {
  Desktop.clipboardWrite($("#clipText").value, "");
  log("clipboard", "text written");
});
$("#copyHTML").addEventListener("click", () => {
  const text = $("#clipText").value;
  Desktop.clipboardWrite(text, `<b style="color:#0b6f85">${text.replace(/</g, "&lt;")}</b> <i>(HTML from ZeroG)</i>`);
  log("clipboard", "text and HTML written: paste into Word or a mail to see the HTML");
});
$("#paste").addEventListener("click", () => {
  const clipboard = Desktop.clipboardRead();
  $("#clipboard").textContent = `text/plain: ${clipboard.hasText ? clipboard.text : "(none)"}\n` +
    `text/html: ${clipboard.hasHTML ? clipboard.html : "(none)"}`;
});

// ---- Light and dark ----

const darkQuery = matchMedia("(prefers-color-scheme: dark)");
function showScheme() {
  $("#scheme").textContent = `the page is ${darkQuery.matches ? "dark" : "light"} · the desktop is ${Desktop.systemColorScheme()}`;
}
darkQuery.addEventListener("change", () => {
  log("prefers-color-scheme", darkQuery.matches ? "dark" : "light");
  showScheme();
});
for (const button of $$("[data-scheme]"))
  button.addEventListener("click", () => Desktop.colorScheme(+button.dataset.scheme));

// ---- Icon, drops, windows ----

for (const button of $$("[data-icon]"))
  button.addEventListener("click", () => Desktop.setIcon(+button.dataset.icon));

const dropZone = $("#dropZone");
document.addEventListener("dragover", (event) => { event.preventDefault(); dropZone.classList.add("over"); });
document.addEventListener("dragleave", () => dropZone.classList.remove("over"));
document.addEventListener("drop", (event) => {
  event.preventDefault();
  dropZone.classList.remove("over");
  const names = [...event.dataTransfer.files].map((file) => `${file.name} (${file.size} bytes)`);
  log("page drop", names);
});
$("#keepDrops").addEventListener("change", (event) => Desktop.setKeepDrops(event.target.checked));

for (const button of $$("[data-window]"))
  button.addEventListener("click", () => Desktop.openWindow(button.dataset.window));
$("#confirmClose").addEventListener("change", (event) => Desktop.setConfirmClose(event.target.checked));

$("#clearLog").addEventListener("click", () => { $("#log").innerHTML = ""; });

// ---- Start ----

refreshStatus();
showScreens();
showScheme();
log("ready", "every control calls a Desktop.* binding; native events show here");
