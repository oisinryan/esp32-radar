(() => {
  const WALK_DIST = 3;
  const STEPS = [
    {
      title: "Welcome",
      prompt: "I'll help calibrate distance measurements. Say next when ready, or click Next.",
      detail: "This wizard sets room size, node positions, and the RSSI-to-metres model. Voice works best in Chrome.",
    },
    {
      title: "Room size",
      prompt: "How wide is the room in metres? Say a number like twelve, or use the Map slider.",
      detail: "Width first, then depth.",
      room: true,
    },
    {
      title: "Node layout",
      prompt: "Spread boards A, B, and C in a triangle on the Map. Drag the markers, then say done.",
      detail: "Switching to Map tab. Match marker positions to where the boards actually sit.",
      tab: "map",
    },
    {
      title: "Pick device",
      prompt: "Choose a Wi-Fi device to track — your phone hotspot or a router. Say its name or click it in the list.",
      detail: "Wi-Fi BSSIDs are stable. BLE addresses rotate and are poor for calibration.",
    },
    {
      title: "Reference point",
      prompt: "Hold the device one metre from the nearest board. Say capture when ready.",
      detail: "We'll record RSSI at the reference distance and set P0.",
      capture: true,
    },
    {
      title: "Walk test",
      prompt: "Walk three metres away. Say capture. Then say tighter, looser, or done.",
      detail: "We'll tune path-loss n from the second reading. Tighter = devices appear closer; looser = farther.",
      capture: true,
    },
  ];

  const $ = (id) => document.getElementById(id);
  const wizardEl = $("wizard");
  const stepEl = $("wizardStep");
  const promptEl = $("wizardPrompt");
  const detailEl = $("wizardDetail");
  const liveEl = $("wizardLive");
  const bannerEl = $("wizardBanner");
  const micDot = $("wizardMic");
  const micLabel = $("wizardMicLabel");
  const btnBack = $("wizardBack");
  const btnRepeat = $("wizardRepeat");
  const btnCapture = $("wizardCapture");
  const btnNext = $("wizardNext");
  const btnClose = $("wizardClose");

  let step = 0;
  let listening = false;
  let recognition = null;
  let roomPhase = "width";
  let calContact = null;
  let rssiRef = null;
  let rssiWalk = null;

  const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;

  function api() {
    return window.xrayCal;
  }

  function speak(text) {
    if (!window.speechSynthesis) return;
    window.speechSynthesis.cancel();
    const u = new SpeechSynthesisUtterance(text);
    u.rate = 0.95;
    window.speechSynthesis.speak(u);
  }

  function showBanner(msg) {
    if (!msg) {
      bannerEl.classList.add("hidden");
      bannerEl.textContent = "";
      return;
    }
    bannerEl.textContent = msg;
    bannerEl.classList.remove("hidden");
  }

  function parseNumber(text) {
    const lower = text.toLowerCase().replace(/metres?|meters?|m\b/gi, " ").trim();
    const words = {
      one: 1, two: 2, three: 3, four: 4, five: 5, six: 6,
      seven: 7, eight: 8, nine: 9, ten: 10, eleven: 11, twelve: 12,
      thirteen: 13, fourteen: 14, fifteen: 15, sixteen: 16,
      eighteen: 18, twenty: 20, thirty: 30, forty: 40,
    };
    for (const [w, n] of Object.entries(words)) {
      if (lower.includes(w)) return n;
    }
    const m = lower.match(/(\d+(?:\.\d+)?)/);
    return m ? parseFloat(m[1]) : null;
  }

  function matchContact(text) {
    const x = api();
    if (!x) return null;
    const t = text.toLowerCase();
    const contacts = x.getContacts().filter((c) => c.type !== "BLE");
    let best = null;
    let bestScore = 0;
    for (const c of contacts) {
      const name = (c.name || c.addr || "").toLowerCase();
      if (!name.trim()) continue;
      if (t.includes(name) || name.includes(t.trim())) return c;
      const parts = name.split(/[\s_-]+/);
      let score = 0;
      for (const p of parts) {
        if (p.length > 2 && t.includes(p)) score += p.length;
      }
      if (score > bestScore) {
        bestScore = score;
        best = c;
      }
    }
    return bestScore >= 3 ? best : null;
  }

  function currentRssiLine() {
    const x = api();
    if (!x || !calContact) return "";
    const rssi = x.bestRssi(calContact);
    const dist = x.contactDist(calContact);
    return `Tracking ${calContact.name || calContact.addr}: ${rssi} dBm ≈ ${x.formatDist(dist)}`;
  }

  function updateLive() {
    liveEl.textContent = currentRssiLine();
  }

  function renderStep() {
    const s = STEPS[step];
    stepEl.textContent = `Step ${step + 1} of ${STEPS.length}`;
    promptEl.textContent = s.prompt;
    detailEl.textContent = s.detail || "";
    btnBack.disabled = step === 0;
    btnCapture.classList.toggle("hidden", !s.capture);
    btnNext.textContent = step === STEPS.length - 1 ? "Finish" : "Next";

    if (s.tab === "map") api()?.switchTab("map");

    if (step === 1) {
      const cal = api()?.getCal();
      if (cal) {
        detailEl.textContent += roomPhase === "width"
          ? ` Current: ${cal.roomW} m wide.`
          : ` Current: ${cal.roomH} m deep.`;
      }
      promptEl.textContent = roomPhase === "width"
        ? "How wide is the room in metres? Say a number like twelve, or use the Map slider."
        : "How deep is the room in metres?";
    }

    if (step >= 3 && calContact) {
      detailEl.textContent = currentRssiLine();
    }

    if (step === 3 && !calContact) {
      const wifi = api()?.getContacts().filter((c) => c.type !== "BLE").slice(0, 8) || [];
      if (wifi.length) {
        detailEl.innerHTML = (s.detail || "") + '<div class="wizard-picks">' +
          wifi.map((c) => `<button type="button" class="pick" data-addr="${c.addr}">${c.name || c.addr}</button>`).join("") +
          "</div>";
        detailEl.querySelectorAll(".pick").forEach((btn) => {
          btn.addEventListener("click", () => {
            const c = wifi.find((x) => x.addr === btn.dataset.addr);
            if (c) {
              calContact = c;
              api()?.setSelected(c.addr);
              detailEl.textContent = currentRssiLine();
              speak(`Selected ${c.name || c.addr}. Say next.`);
            }
          });
        });
      }
    }

    updateLive();
    speak(promptEl.textContent);
  }

  function openWizard() {
    step = 0;
    roomPhase = "width";
    calContact = null;
    rssiRef = null;
    rssiWalk = null;
    wizardEl.classList.remove("hidden");
    wizardEl.setAttribute("aria-hidden", "false");

    if (!window.speechSynthesis) {
      showBanner("Speech output unavailable. Use the buttons.");
    } else if (!SpeechRecognition) {
      showBanner("Voice input unavailable in this browser. Use the buttons — Chrome works best.");
    } else {
      showBanner("");
    }

    renderStep();
    startListening();
    liveTimer = setInterval(updateLive, 800);
  }

  function closeWizard() {
    wizardEl.classList.add("hidden");
    wizardEl.setAttribute("aria-hidden", "true");
    stopListening();
    if (liveTimer) {
      clearInterval(liveTimer);
      liveTimer = null;
    }
    window.speechSynthesis?.cancel();
  }

  let liveTimer = null;

  function startListening() {
    if (!SpeechRecognition) {
      micDot.className = "mic-dot off";
      micLabel.textContent = "Voice off — use buttons";
      return;
    }
    if (recognition) {
      try { recognition.stop(); } catch { /* ignore */ }
    }
    recognition = new SpeechRecognition();
    recognition.continuous = false;
    recognition.interimResults = false;
    recognition.lang = "en-GB";

    recognition.onstart = () => {
      listening = true;
      micDot.className = "mic-dot on";
      micLabel.textContent = "Listening…";
    };
    recognition.onend = () => {
      listening = false;
      micDot.className = "mic-dot off";
      micLabel.textContent = "Voice ready";
      if (!wizardEl.classList.contains("hidden")) {
        setTimeout(() => {
          try { recognition.start(); } catch { /* ignore */ }
        }, 400);
      }
    };
    recognition.onerror = (e) => {
      if (e.error === "not-allowed") {
        micLabel.textContent = "Mic denied — use buttons";
        showBanner("Microphone permission denied. Use the buttons.");
      }
    };
    recognition.onresult = (ev) => {
      const text = ev.results[0][0].transcript;
      handleVoice(text);
    };

    try {
      recognition.start();
    } catch {
      micLabel.textContent = "Voice off — use buttons";
    }
  }

  function stopListening() {
    if (recognition) {
      try { recognition.stop(); } catch { /* ignore */ }
      recognition = null;
    }
    listening = false;
    micDot.className = "mic-dot off";
  }

  function handleVoice(raw) {
    const text = raw.toLowerCase().trim();
    if (/\b(repeat|again|say again)\b/.test(text)) {
      speak(STEPS[step].prompt);
      return;
    }
    if (/\b(back|previous)\b/.test(text)) {
      goBack();
      return;
    }
    if (/\b(capture|record|snapshot)\b/.test(text)) {
      doCapture();
      return;
    }
    if (/\btighter\b/.test(text)) {
      nudgeN(-0.15);
      return;
    }
    if (/\blooser\b/.test(text)) {
      nudgeN(0.15);
      return;
    }
    if (/\b(done|finish|complete)\b/.test(text)) {
      if (step === 2 || step === STEPS.length - 1) {
        if (step === STEPS.length - 1) finishWizard();
        else goNext();
      }
      return;
    }
    if (/\b(skip|next)\b/.test(text)) {
      goNext();
      return;
    }

    if (step === 1) {
      const n = parseNumber(text);
      if (n != null) {
        if (roomPhase === "width") {
          api()?.setCalValue("roomW", Math.max(4, Math.min(40, n)));
          roomPhase = "depth";
          speak(`Room width set to ${n} metres. How deep is the room?`);
          renderStep();
        } else {
          api()?.setCalValue("roomH", Math.max(4, Math.min(40, n)));
          speak(`Room depth set to ${n} metres. Say next.`);
        }
        return;
      }
    }
    if (step === 3) {
      const c = matchContact(text);
      if (c) {
        calContact = c;
        api()?.setSelected(c.addr);
        speak(`Selected ${c.name || c.addr}. Say next.`);
        detailEl.textContent = currentRssiLine();
        return;
      }
    }
  }

  function nudgeN(delta) {
    const x = api();
    if (!x) return;
    const cal = x.getCal();
    const next = Math.max(1.5, Math.min(4.5, cal.nExp + delta));
    x.setCalValue("nExp", Math.round(next * 10) / 10);
    const dist = calContact ? x.formatDist(x.contactDist(calContact)) : "";
    speak(`Path loss n is now ${next.toFixed(1)}.${dist ? " Device now about " + dist + "." : ""} Say tighter, looser, or done.`);
    updateLive();
  }

  function doCapture() {
    const x = api();
    if (!x || !calContact) {
      speak("Pick a device first.");
      return;
    }
    const rssi = x.bestRssi(calContact);
    if (step === 4) {
      rssiRef = rssi;
      x.setCalValue("d0", 1);
      x.setCalValue("p0", Math.round(rssi));
      speak(`Reference captured at ${rssi} dBm. Walk ${WALK_DIST} metres away and say capture.`);
      detailEl.textContent = `Reference: ${rssi} dBm at 1 m · P0 set to ${rssi} dBm`;
    } else if (step === 5) {
      rssiWalk = rssi;
      const cal = x.getCal();
      const d0 = cal.d0;
      const ratio = WALK_DIST / d0;
      if (ratio <= 1 || rssiRef == null) {
        speak("Need a reference capture first.");
        return;
      }
      let n = (cal.p0 - rssiWalk) / (10 * Math.log10(ratio));
      n = Math.max(1.5, Math.min(4.5, n));
      x.setCalValue("nExp", Math.round(n * 10) / 10);
      const dist = x.formatDist(x.contactDist(calContact));
      speak(`Walk captured at ${rssi} dBm. Path loss n is ${n.toFixed(1)}. Device about ${dist}. Say tighter, looser, or done.`);
      detailEl.textContent = `Walk: ${rssi} dBm at ${WALK_DIST} m · n=${n.toFixed(1)} · est ${dist}`;
    }
    updateLive();
  }

  function goBack() {
    if (step <= 0) return;
    if (step === 2) roomPhase = "depth";
    step -= 1;
    if (step === 1) roomPhase = "width";
    renderStep();
  }

  function goNext() {
    if (step === 1 && roomPhase === "width") {
      roomPhase = "depth";
      renderStep();
      return;
    }
    if (step === 3 && !calContact) {
      speak("Please select a Wi-Fi device first.");
      return;
    }
    if (step === 4 && rssiRef == null) {
      speak("Say capture at the reference point first, or click Capture.");
      return;
    }
    if (step >= STEPS.length - 1) {
      finishWizard();
      return;
    }
    step += 1;
    renderStep();
  }

  function finishWizard() {
    const x = api();
    if (x) {
      x.setRingLabelMode("metres");
      x.saveStore({ wizardDone: true });
      if (calContact) {
        const dist = x.formatDist(x.contactDist(calContact));
        speak(`Calibration saved. ${calContact.name || "Device"} is about ${dist} away.`);
      } else {
        speak("Calibration saved.");
      }
    }
    closeWizard();
  }

  btnBack.addEventListener("click", goBack);
  btnRepeat.addEventListener("click", () => speak(STEPS[step].prompt));
  btnCapture.addEventListener("click", doCapture);
  btnNext.addEventListener("click", goNext);
  btnClose.addEventListener("click", closeWizard);

  $("openWizard")?.addEventListener("click", openWizard);
  $("openWizardMap")?.addEventListener("click", openWizard);

  wizardEl.addEventListener("click", (e) => {
    if (e.target === wizardEl) closeWizard();
  });

  document.addEventListener("keydown", (e) => {
    if (wizardEl.classList.contains("hidden")) return;
    if (e.key === "Escape") closeWizard();
  });
})();
