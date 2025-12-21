(function () {
  const c = document.getElementById("particleCanvas");
  if (!c) return;
  const ctx = c.getContext("2d");

  function resize() {
    c.width = window.innerWidth;
    c.height = window.innerHeight;
  }
  window.addEventListener("resize", resize);
  resize();

  const pts = [];
  const N = 36;
  function rnd(a, b) { return a + Math.random() * (b - a); }

  for (let i = 0; i < N; i++) {
    pts.push({
      x: rnd(0, c.width),
      y: rnd(0, c.height),
      vx: rnd(-0.25, 0.25),
      vy: rnd(-0.25, 0.25),
      r: rnd(1, 2.2)
    });
  }

  function tick() {
    ctx.clearRect(0, 0, c.width, c.height);

    ctx.globalAlpha = 0.85;
    for (const p of pts) {
      p.x += p.vx; p.y += p.vy;
      if (p.x < 0 || p.x > c.width) p.vx *= -1;
      if (p.y < 0 || p.y > c.height) p.vy *= -1;

      ctx.beginPath();
      ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.globalAlpha = 0.10;
    for (let i = 0; i < pts.length; i++) {
      for (let j = i + 1; j < pts.length; j++) {
        const a = pts[i], b = pts[j];
        const dx = a.x - b.x, dy = a.y - b.y;
        const d = Math.sqrt(dx*dx + dy*dy);
        if (d < 140) {
          ctx.lineWidth = 1;
          ctx.beginPath();
          ctx.moveTo(a.x, a.y);
          ctx.lineTo(b.x, b.y);
          ctx.stroke();
        }
      }
    }

    requestAnimationFrame(tick);
  }

  ctx.fillStyle = "rgba(0, 229, 255, 0.45)";
  ctx.strokeStyle = "rgba(0, 229, 255, 0.55)";
  tick();
})();
