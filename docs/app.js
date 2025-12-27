const variantsEl = document.getElementById("variants");
const template = document.getElementById("variant-card");
const compat = document.getElementById("compat");
const hasWebSerial = "serial" in navigator;

if (!hasWebSerial) {
  compat.textContent = "WebSerial wird hier nicht unterstuetzt. Nutze Chrome oder Edge auf Desktop.";
  compat.classList.remove("hidden");
}

const buildCard = (variant) => {
  const node = template.content.cloneNode(true);
  const card = node.querySelector(".card");

  node.querySelector(".variant-name").textContent = variant.name;
  node.querySelector(".variant-desc").textContent = variant.description || "";
  node.querySelector(".chip").textContent = variant.chipFamily || "";

  const versionEl = node.querySelector(".version");
  versionEl.textContent = variant.version ? `Version ${variant.version}` : "Version unbekannt";

  const statusEl = node.querySelector(".status");
  if (variant.available) {
    statusEl.textContent = "Bereit";
    statusEl.classList.add("status--ready");
  } else {
    statusEl.textContent = variant.note || "Bald verfuegbar";
  }

  const actions = node.querySelector(".actions");
  const install = document.createElement("esp-web-install-button");
  install.setAttribute("manifest", variant.manifest);
  install.setAttribute("install-button-text", "Flashen");

  if (!hasWebSerial || !variant.available) {
    install.setAttribute("disabled", "");
    card.classList.add("card--disabled");
  }

  actions.appendChild(install);

  if (variant.binary) {
    const link = document.createElement("a");
    link.href = variant.binary;
    link.textContent = "Bin herunterladen";
    link.className = "ghost";
    link.setAttribute("download", "");
    actions.appendChild(link);
  }

  const subnote = node.querySelector(".subnote");
  if (variant.note && variant.available) {
    subnote.textContent = variant.note;
  }

  return node;
};

fetch("variants.json", { cache: "no-store" })
  .then((response) => response.json())
  .then((data) => {
    data.variants.forEach((variant) => {
      variantsEl.appendChild(buildCard(variant));
    });
  })
  .catch(() => {
    compat.textContent = "Konfiguration konnte nicht geladen werden.";
    compat.classList.remove("hidden");
  });
