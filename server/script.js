const byId = (id) => document.getElementById(id);
    //defaults
    const days = [
      ["S", 1 << 0], ["M", 1 << 1], ["T", 1 << 2], ["W", 1 << 3],
      ["T", 1 << 4], ["F", 1 << 5], ["S", 1 << 6],
    ];
    const ageGroups = {
      newborn: ["0-3 months", 14, 17],
      infant: ["4-12 months", 12, 16],
      toddler: ["1-2 years", 11, 14],
      preschool: ["3-5 years", 10, 13],
      school_age: ["6-12 years", 9, 12],
      teen: ["13-17 years", 8, 10],
      adult_18_60: ["18-60 years", 7, 9],
      adult_61_64: ["61-64 years", 7, 9],
      adult_65: ["65+ years", 7, 8],
    };
    const fallbackTracks = Array.from({ length: 8 }, (_, index) => ({
      number: index + 1,
      label: `Track ${index + 1}`,
      url: "",
    }));
    const comfortDefaults = {
      minTemperature: 18,
      maxTemperature: 26,
    };
    const comfortLimits = {
      minHumidity: 30,
      maxHumidity: 65,
      warnAirQuality: 3,
    };

    let alarms = [];
    let tracks = fallbackTracks;
    let sleepProfile = {};
    let scheduleMode = "wake_at";
    let previewAudio = null;
    let previewStopTimer = null;
    let lastStatus = {};

    function escapeHtml(value) {
      return String(value ?? "").replace(/[&<>"']/g, (ch) => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
      }[ch]));
    }

    function formatEpoch(value) {
      return value ? new Date(value * 1000).toLocaleString() : "--";
    }

    function minutesToTime(total) {
      const day = 24 * 60;
      const minutes = ((Math.round(total) % day) + day) % day;
      return `${String(Math.floor(minutes / 60)).padStart(2, "0")}:${String(minutes % 60).padStart(2, "0")}`;
    }

    function timeToMinutes(value) {
      const [hours, minutes] = String(value || "00:00").split(":").map(Number);
      return (hours || 0) * 60 + (minutes || 0);
    }

    function airQualityLabel(index) {
      return ["", "Excellent", "Good", "Moderate", "Poor", "Unhealthy"][index] || "Unknown";
    }

    function setTone(el, tone) {
      el.className = `metric-value${tone ? " " + tone : ""}`;
    }

    function readComfortSettings() {
      const storedMin = Number(localStorage.getItem("comfortMinTemperature"));
      const storedMax = Number(localStorage.getItem("comfortMaxTemperature"));
      let minTemperature = Number.isFinite(storedMin) ? storedMin : comfortDefaults.minTemperature;
      let maxTemperature = Number.isFinite(storedMax) ? storedMax : comfortDefaults.maxTemperature;

      if (minTemperature > maxTemperature) {
        [minTemperature, maxTemperature] = [maxTemperature, minTemperature];
      }

      return { minTemperature, maxTemperature };
    }

    function writeComfortSettings() {
      const minTemperature = Number(byId("minTemperature").value);
      const maxTemperature = Number(byId("maxTemperature").value);
      if (!Number.isFinite(minTemperature) || !Number.isFinite(maxTemperature)) return;
      localStorage.setItem("comfortMinTemperature", String(minTemperature));
      localStorage.setItem("comfortMaxTemperature", String(maxTemperature));
      renderComfortStatus(lastStatus);
    }

    function initComfortSettings() {
      const settings = readComfortSettings();
      byId("minTemperature").value = settings.minTemperature;
      byId("maxTemperature").value = settings.maxTemperature;
      ["minTemperature", "maxTemperature"].forEach((id) => {
        byId(id).addEventListener("input", writeComfortSettings);
        byId(id).addEventListener("change", writeComfortSettings);
      });
      renderComfortStatus({});
    }

    function setAlert(id, tone = "") {
      const alert = byId(id);
      alert.classList.toggle("hot", tone === "hot");
      alert.classList.toggle("cold", tone === "cold");
      alert.classList.toggle("bad", tone === "bad");
      alert.classList.toggle("caution", tone === "caution");
      alert.setAttribute("aria-pressed", tone ? "true" : "false");
    }

    function renderComfortStatus(status = {}) {
      const { minTemperature, maxTemperature } = readComfortSettings();
      byId("comfortRange").textContent = `${minTemperature.toFixed(1)}-${maxTemperature.toFixed(1)} C`;

      const hasTemperature = status.environment_valid && Number.isFinite(Number(status.temperature_c));
      const temperature = Number(status.temperature_c);
      setAlert("tooColdAlert", hasTemperature && temperature < minTemperature ? "cold" : "");
      setAlert("tooHotAlert", hasTemperature && temperature > maxTemperature ? "hot" : "");

      const hasHumidity = status.environment_valid && Number.isFinite(Number(status.humidity_percent));
      const humidity = Number(status.humidity_percent);
      setAlert("humidityAlert", hasHumidity && humidity > comfortLimits.maxHumidity ? "hot" : hasHumidity && humidity < comfortLimits.minHumidity ? "cold" : "");

      const aqi = Number(status.air_quality_index || 0);
      const hasAirQuality = status.air_quality_valid && aqi > 0;
      setAlert("badAirAlert", hasAirQuality && aqi > comfortLimits.warnAirQuality ? "bad" : hasAirQuality && aqi === comfortLimits.warnAirQuality ? "caution" : "");
    }

    async function fetchJson(url, options) {
      const response = await fetch(url, options);
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || response.statusText);
      return data;
    }

    function normalizeAlarm(alarm, index) {
      const volume = alarm.volume ?? alarm.alarm_volume ?? 20;
      const track = alarm.track ?? alarm.ringtone ?? 1;
      return {
        id: alarm.id || `alarm-${Date.now()}-${index}`,
        label: alarm.label || `Alarm ${index + 1}`,
        enabled: Boolean(alarm.enabled),
        alarm_time: alarm.alarm_time || "07:00",
        snooze_minutes: Number(alarm.snooze_minutes || 5),
        volume: Math.min(30, Math.max(0, Number(volume))),
        track: Math.min(255, Math.max(1, Number(track))),
        days_mask: Number(alarm.days_mask || 127),
      };
    }

    function readAlarmsFromDom() {
      alarms = [...byId("alarmList").querySelectorAll(".alarm-row")].map((row, index) => {
        const current = alarms[Number(row.dataset.index)] || {};
        return normalizeAlarm({
          ...current,
          enabled: row.querySelector('[data-field="enabled"]')?.checked ?? false,
          label: row.querySelector('[data-field="label"]')?.value || `Alarm ${index + 1}`,
          alarm_time: row.querySelector('[data-field="alarm_time"]')?.value || "07:00",
          snooze_minutes: Number(row.querySelector('[data-field="snooze_minutes"]')?.value || 5),
          volume: Number(row.querySelector('[data-field="volume"]')?.value ?? 20),
          track: Number(row.querySelector('[data-field="track"]')?.value ?? 1),
          days_mask: current.days_mask || 127,
        }, index);
      });
      return alarms;
    }

    function preferredCyclesForAge(ageGroup) {
      const [, minHours, maxHours] = ageGroups[ageGroup] || ageGroups.adult_18_60;
      return Math.min(12, Math.max(3, Math.round((((minHours + maxHours) / 2) * 60) / 90)));
    }

    function recommendationCycles(preferred) {
      const start = Math.max(3, preferred - 1);
      const cycles = [];
      for (let value = start; cycles.length < 4 && value <= 12; value += 1) cycles.push(value);
      for (let value = start - 1; cycles.length < 4 && value >= 3; value -= 1) cycles.unshift(value);
      return cycles;
    }

    function renderAgeGroups() {
      byId("ageGroup").innerHTML = Object.entries(ageGroups)
        .map(([key, [label, min, max]]) => {
          const range = max >= 24 ? `${min}+ h` : `${min}-${max} h`;
          return `<option value="${key}">${label} - ${range}</option>`;
        })
        .join("");
    }

    function trackOptionLabel(track) {
      const label = track.label && track.label !== `Track ${track.number}` ? ` - ${track.label}` : "";
      return `Track ${track.number}${label}`;
    }

    function renderTrackOptions(selected) {
      const selectedTrack = Number(selected || 1);
      const hasSelected = tracks.some((track) => Number(track.number) === selectedTrack);
      const availableTracks = hasSelected ? tracks : [{ number: selectedTrack, label: `Track ${selectedTrack}`, url: "" }, ...tracks];
      return availableTracks.map((track) => `
        <option value="${track.number}" data-short="Track ${track.number}" data-full="${escapeHtml(trackOptionLabel(track))}" ${Number(track.number) === selectedTrack ? "selected" : ""}>${escapeHtml(trackOptionLabel(track))}</option>
      `).join("");
    }

    function setTrackSelectLabels(select, expanded) {
      [...select.options].forEach((option) => {
        option.textContent = expanded || !option.selected ? option.dataset.full : option.dataset.short;
      });
    }

    function refreshTrackSelectLabels(expanded = false) {
      byId("alarmList").querySelectorAll('select[data-field="track"]').forEach((select) => {
        setTrackSelectLabels(select, expanded);
      });
    }

    function stopPreviewTrack() {
      if (previewStopTimer) {
        clearTimeout(previewStopTimer);
        previewStopTimer = null;
      }
      if (previewAudio) {
        previewAudio.pause();
        previewAudio.currentTime = 0;
      }
    }

    function previewTrack(trackNumber) {
      const track = tracks.find((item) => Number(item.number) === Number(trackNumber));
      if (!track?.url) {
        byId("message").textContent = "Put a browser-playable copy in server/tracks to preview this track.";
        return;
      }
      if (!previewAudio) previewAudio = new Audio();
      if (previewStopTimer) clearTimeout(previewStopTimer);
      previewAudio.pause();
      previewAudio.src = track.url;
      previewAudio.currentTime = 0;
      previewAudio.play().catch(() => {
        byId("message").textContent = "The browser could not play this track file.";
      });
      previewStopTimer = setTimeout(stopPreviewTrack, 5000);
    }

    function renderAlarms() {
      const legend = `
        <div class="alarm-legend" aria-hidden="true">
          <span>Active</span>
          <span>Alarm name</span>
          <span>Time</span>
          <span>Snooze</span>
          <span>Track</span>
          <span>Volume</span>
          <span>Days</span>
          <span>Cancel</span>
        </div>
      `;
      byId("alarmList").innerHTML = legend + alarms.map((alarm, index) => `
        <div class="alarm-row" data-index="${index}">
          <input data-field="enabled" type="checkbox" ${alarm.enabled ? "checked" : ""} aria-label="Enabled">
          <input data-field="label" type="text" maxlength="23" value="${escapeHtml(alarm.label)}" aria-label="Label">
          <input data-field="alarm_time" type="time" value="${escapeHtml(alarm.alarm_time)}" aria-label="Time">
          <input data-field="snooze_minutes" type="number" min="1" max="120" step="1" value="${alarm.snooze_minutes}" aria-label="Snooze">
          <div class="track-control">
            <select data-field="track" aria-label="Track">${renderTrackOptions(alarm.track)}</select>
            <button type="button" data-preview-track title="Preview track">Play</button>
          </div>
          <div class="volume-control">
            <input data-field="volume" type="range" min="0" max="30" step="1" value="${alarm.volume}" aria-label="Volume">
            <span>${alarm.volume}</span>
          </div>
          <div class="day-set">
            ${days.map(([label, bit]) => `<button type="button" class="day ${(alarm.days_mask & bit) ? "active" : ""}" data-day="${bit}">${label}</button>`).join("")}
          </div>
          <button type="button" class="danger-button" data-remove title="Remove alarm">x</button>
        </div>
      `).join("");
      refreshTrackSelectLabels(false);
    }

    function bindAlarmEvents() {
      byId("alarmList").addEventListener("mousedown", (event) => {
        if (event.target.matches('select[data-field="track"]')) {
          setTrackSelectLabels(event.target, true);
        }
      });

      byId("alarmList").addEventListener("focusin", (event) => {
        if (event.target.matches('select[data-field="track"]')) {
          setTrackSelectLabels(event.target, true);
        }
      });

      byId("alarmList").addEventListener("focusout", (event) => {
        if (event.target.matches('select[data-field="track"]')) {
          setTrackSelectLabels(event.target, false);
        }
      });

      byId("alarmList").addEventListener("input", (event) => {
        const row = event.target.closest(".alarm-row");
        if (!row) return;
        const alarm = alarms[Number(row.dataset.index)];
        const field = event.target.dataset.field;
        if (!alarm || !field) return;
        alarm[field] = event.target.type === "number" || event.target.type === "range" || event.target.tagName === "SELECT" ? Number(event.target.value) : event.target.value;
        if (field === "volume") {
          const value = event.target.closest(".volume-control")?.querySelector("span");
          if (value) value.textContent = alarm.volume;
        }
        renderNextAlarm();
      });

      byId("alarmList").addEventListener("change", (event) => {
        const row = event.target.closest(".alarm-row");
        if (!row) return;
        const alarm = alarms[Number(row.dataset.index)];
        const field = event.target.dataset.field;
        if (alarm && field && field !== "enabled") {
          alarm[field] = event.target.type === "number" || event.target.type === "range" || event.target.tagName === "SELECT" ? Number(event.target.value) : event.target.value;
          if (field === "track") {
            setTimeout(() => setTrackSelectLabels(event.target, false), 0);
          }
          renderNextAlarm();
        }
        if (alarm && event.target.dataset.field === "enabled") {
          alarm.enabled = event.target.checked;
          renderNextAlarm();
        }
      });

      byId("alarmList").addEventListener("click", (event) => {
        const row = event.target.closest(".alarm-row");
        if (!row) return;
        const index = Number(row.dataset.index);
        if (event.target.matches("[data-remove]")) {
          alarms.splice(index, 1);
          renderAlarms();
          renderNextAlarm();
          return;
        }
        if (event.target.matches("[data-day]")) {
          const bit = Number(event.target.dataset.day);
          alarms[index].days_mask ^= bit;
          if (alarms[index].days_mask === 0) alarms[index].days_mask = 127;
          renderAlarms();
          renderNextAlarm();
        }
        if (event.target.matches("[data-preview-track]")) {
          previewTrack(alarms[index]?.track || 1);
        }
      });
    }

    function renderSleepProfile() {
      byId("ageGroup").value = sleepProfile.age_group || "adult_18_60";
      byId("fallAsleep").value = Number(sleepProfile.time_to_fall_asleep ?? 15);
      byId("sleepEveryday").checked = sleepProfile.everyday !== false;
      byId("wakeTime").value = sleepProfile.wake_time || "07:00";
      byId("bedTime").value = sleepProfile.bed_time || "23:00";
      scheduleMode = sleepProfile.schedule_mode || "wake_at";
      renderScheduleMode();
      renderSleepResults();
    }

    function readSleepProfile() {
      return {
        age_group: byId("ageGroup").value,
        time_to_fall_asleep: Number(byId("fallAsleep").value || 15),
        cycle_minutes: 90,
        preferred_cycles: preferredCyclesForAge(byId("ageGroup").value),
        schedule_mode: scheduleMode,
        wake_time: byId("wakeTime").value || "07:00",
        bed_time: byId("bedTime").value || "23:00",
        everyday: byId("sleepEveryday").checked,
      };
    }

    function renderScheduleMode() {
      byId("wakeMode").classList.toggle("active", scheduleMode === "wake_at");
      byId("bedMode").classList.toggle("active", scheduleMode === "bed_at");
      byId("wakeTimeField").style.display = scheduleMode === "wake_at" ? "grid" : "none";
      byId("bedTimeField").style.display = scheduleMode === "bed_at" ? "grid" : "none";
      byId("sleepNowButton").style.display = scheduleMode === "bed_at" ? "inline-flex" : "none";
    }

    function renderSleepResults() {
      sleepProfile = readSleepProfile();
      const group = ageGroups[sleepProfile.age_group] || ageGroups.adult_18_60;
      const [label, minHours, maxHours] = group;
      const preferredCycleCount = preferredCyclesForAge(sleepProfile.age_group);
      const cycles = recommendationCycles(preferredCycleCount);
      const sourceMinutes = scheduleMode === "wake_at" ? timeToMinutes(sleepProfile.wake_time) : timeToMinutes(sleepProfile.bed_time);
      byId("sleepTarget").textContent = `${label} - 90 min cycles`;

      byId("sleepResults").innerHTML = cycles.map((cycleCount) => {
        const sleepMinutes = cycleCount * 90;
        const totalHours = sleepMinutes / 60;
        const targetMinutes = scheduleMode === "wake_at"
          ? sourceMinutes - sleepProfile.time_to_fall_asleep - sleepMinutes
          : sourceMinutes + sleepProfile.time_to_fall_asleep + sleepMinutes;
        const timeValue = minutesToTime(targetMinutes);
        const inRange = totalHours >= minHours && totalHours <= maxHours;
        const preferred = cycleCount === preferredCycleCount;
        const labelText = scheduleMode === "wake_at" ? "Bedtime" : "Wake";
        return `
          <div class="result-row" aria-label="${labelText} ${timeValue}">
            <div class="result-time">${timeValue}</div>
            <div class="result-meta">
              <span class="pill ${inRange ? "ok" : "warn"}">${cycleCount} cycles - ${totalHours.toFixed(1)} h${preferred ? " - Preferred" : ""}</span>
            </div>
            <button type="button" class="primary" data-result-time="${timeValue}">Set</button>
          </div>
        `;
      }).join("");
    }

    function nextAlarmInfo(sourceAlarms = alarms) {
      const now = new Date();
      let best = null;

      for (const alarm of sourceAlarms) {
        if (!alarm.enabled) continue;

        const [hours, minutes] = String(alarm.alarm_time || "07:00").split(":").map(Number);
        for (let offset = 0; offset <= 7; offset += 1) {
          const candidate = new Date(now);
          candidate.setDate(now.getDate() + offset);
          candidate.setHours(hours || 0, minutes || 0, 0, 0);

          const dayBit = 1 << candidate.getDay();
          if ((Number(alarm.days_mask || 127) & dayBit) === 0) continue;
          if (candidate <= now) continue;

          if (!best || candidate < best.date) {
            best = { alarm, date: candidate };
          }
          break;
        }
      }

      return best;
    }

    function renderNextAlarm(status = {}) {
      const nextAlarm = byId("nextAlarm");

      if (status.ringing) {
        nextAlarm.textContent = "Ringing now";
        setTone(nextAlarm, "danger");
        return;
      }

      if (Number(status.snoozed_until_epoch || 0) > 0) {
        nextAlarm.textContent = `Snooze ${formatEpoch(status.snoozed_until_epoch)}`;
        setTone(nextAlarm, "warn");
        return;
      }

      const next = nextAlarmInfo(alarms);
      if (!next) {
        nextAlarm.textContent = "None";
        setTone(nextAlarm, "");
        return;
      }

      const label = next.alarm.label ? `${next.alarm.label} ` : "";
      nextAlarm.textContent = `${label}${next.date.toLocaleString([], { weekday: "short", hour: "2-digit", minute: "2-digit" })}`;
      setTone(nextAlarm, "ok");
    }

    function addAlarmFromSleep(timeValue) {
      const everyDay = byId("sleepEveryday").checked;
      const alarmTime = scheduleMode === "wake_at" ? byId("wakeTime").value : timeValue;
      if (alarms.length >= 8) return;
      alarms.push(normalizeAlarm({
        label: scheduleMode === "wake_at" ? "Wake" : "Sleep cycle",
        enabled: true,
        alarm_time: alarmTime,
        snooze_minutes: 5,
        volume: 20,
        track: 1,
        days_mask: everyDay ? 127 : 62,
      }, alarms.length));
      renderAlarms();
      renderNextAlarm();
    }

    async function loadTracks() {
      const payload = await fetchJson("/api/tracks");
      tracks = Array.isArray(payload.tracks) && payload.tracks.length ? payload.tracks : fallbackTracks;
    }

    async function loadSettings() {
      const settings = await fetchJson("/api/alarm-settings");
      alarms = (settings.alarms || []).map(normalizeAlarm);
      sleepProfile = settings.sleep_profile || {};
      byId("serverTime").textContent = settings.server_iso || "--";
      renderAlarms();
      renderSleepProfile();
      renderNextAlarm();
    }

    async function loadStatus() {
      const payload = await fetchJson("/api/status");
      const status = payload.status || {};
      lastStatus = status;
      byId("serverTime").textContent = payload.server_iso || "--";
      byId("deviceId").textContent = status.device_id || "--";

      const connection = byId("connection");
      connection.textContent = status.wifi_connected ? "Wi-Fi" : "Waiting";
      setTone(connection, status.wifi_connected ? "ok" : "warn");

      const alarmState = byId("alarmState");
      if (status.ringing) {
        const active = (status.alarms || []).filter((alarm) => alarm.active).map((alarm) => alarm.label).join(", ");
        alarmState.textContent = active || "Ringing";
        setTone(alarmState, "danger");
      } else {
        alarmState.textContent = `${status.alarm_count ?? alarms.length} scheduled`;
        setTone(alarmState, status.alarm_enabled ? "ok" : "");
      }

      renderNextAlarm(status);

      byId("deviceTime").textContent = status.time_valid ? formatEpoch(status.device_epoch) : "Not synced";
      byId("temperature").textContent = status.environment_valid && status.temperature_c !== undefined ? `${Number(status.temperature_c).toFixed(1)} C` : "--";
      byId("humidity").textContent = status.environment_valid && status.humidity_percent !== undefined ? `${Number(status.humidity_percent).toFixed(1)} %` : "--";

      const aqi = Number(status.air_quality_index || 0);
      const airQuality = byId("airQuality");
      airQuality.textContent = status.air_quality_valid && aqi > 0 ? `AQI ${aqi} - ${airQualityLabel(aqi)}` : "--";
      setTone(airQuality, aqi >= 4 ? "danger" : aqi === 3 ? "warn" : aqi > 0 ? "ok" : "");
      byId("airChemistry").textContent = status.air_quality_valid ? `${Number(status.tvoc_ppb || 0)} ppb / ${Number(status.eco2_ppm || 0)} ppm` : "--";

      renderComfortStatus(status);
      byId("syncState").textContent = status.received_at ? `Updated ${status.received_at}` : "Waiting";
    }

    async function saveSettings() {
      const button = byId("saveButton");
      button.disabled = true;
      byId("message").textContent = "Saving...";
      try {
        const payload = {
          alarms: readAlarmsFromDom().slice(0, 8).map((alarm, index) => normalizeAlarm(alarm, index)),
          sleep_profile: readSleepProfile(),
        };
        const saved = await fetchJson("/api/alarm-settings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        byId("message").textContent = "Saved";
        alarms = (saved.alarms || payload.alarms).map(normalizeAlarm);
        sleepProfile = saved.sleep_profile || payload.sleep_profile;
        renderAlarms();
        renderSleepProfile();
        renderNextAlarm();
      } catch (error) {
        byId("message").textContent = error.message;
      } finally {
        button.disabled = false;
      }
    }

    function bindSleepEvents() {
      ["ageGroup", "fallAsleep", "sleepEveryday", "wakeTime", "bedTime"].forEach((id) => {
        byId(id).addEventListener("input", renderSleepResults);
        byId(id).addEventListener("change", renderSleepResults);
      });
      byId("wakeMode").addEventListener("click", () => { scheduleMode = "wake_at"; renderScheduleMode(); renderSleepResults(); });
      byId("bedMode").addEventListener("click", () => { scheduleMode = "bed_at"; renderScheduleMode(); renderSleepResults(); });
      byId("sleepResults").addEventListener("click", (event) => {
        const button = event.target.closest("[data-result-time]");
        if (button) addAlarmFromSleep(button.dataset.resultTime);
      });
      byId("sleepNowButton").addEventListener("click", () => {
        const now = new Date();
        byId("bedTime").value = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`;
        scheduleMode = "bed_at";
        renderScheduleMode();
        renderSleepResults();
      });
    }

    renderAgeGroups();
    initComfortSettings();
    bindAlarmEvents();
    bindSleepEvents();
    byId("addAlarmButton").addEventListener("click", () => {
      if (alarms.length >= 8) return;
      alarms.push(normalizeAlarm({ enabled: true, label: `Alarm ${alarms.length + 1}` }, alarms.length));
      renderAlarms();
      renderNextAlarm();
    });
    byId("settingsForm").addEventListener("submit", (event) => {
      event.preventDefault();
      saveSettings();
    });

    loadTracks()
      .catch(() => { tracks = fallbackTracks; })
      .finally(() => loadSettings().catch((error) => byId("message").textContent = error.message));
    loadStatus().catch(() => {});
    setInterval(() => loadStatus().catch(() => {}), 2000);