(() => {
  const el = document.getElementById("fwFooter");
  if (!el) return;

  const link =
    '<a href="https://github.com/softwarecrash/BambuBeacon" target="_blank" rel="noopener">SoftWareCrash</a>';

  fetch("/info.json", { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => {
      if (!j || !j.version) return;
      el.innerHTML = "Firmware v" + j.version + " by " + link;
    })
    .catch(() => {});
})();
