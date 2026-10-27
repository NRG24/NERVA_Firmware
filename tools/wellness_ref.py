#!/usr/bin/env python3
"""
Reference implementation of mac-handoff/WELLNESS_ANALYTICS.md, sections 2-7:
data hygiene, nightly summaries, baselines, recovery, the burnout (sustained
strain) trend, daytime stress scores and night hot-flash events.

    python tools/wellness_ref.py history_20261101_081500.csv   # a real sync
    python tools/wellness_ref.py --vectors                     # regenerate
    python tools/wellness_ref.py --selftest                    # check it

The app's Dart port (lib/analytics/...) must give the same answers as this
file. --vectors writes mac-handoff/wellness_vectors.json: 49 days of
synthetic v2 records, as the 64-hex-digit wire format the app already
parses, with every expected output. Port it, feed it the records, compare
(floats to 1e-6).

Where the design doc left a choice open, the choice made here is marked
DECIDED and is part of the contract; WELLNESS_ANALYTICS.md section 11 lists
them. Change both together.

Standard library only: no bleak, so it runs anywhere. The v2 layout is
copied from history_sync.py and pinned by the same vectors.
"""

import argparse
import calendar
import csv
import datetime
import json
import math
import os
import random
import statistics
import struct
import sys

EPOCH_S = 300

F_TIME, F_WORN, F_CHG, F_SLEEP, F_WORKOUT, F_HOT, F_SPOT_N, F_SPOT_D = (
    1 << i for i in range(8))
FLAG_NAMES = ["TIME", "WORN", "CHG", "SLEEP", "WORKOUT", "HOTFLASH",
              "SPOT_NIGHT", "SPOT_DAY"]

REC2 = struct.Struct("<IIHhhBBBBBBBBBBBBBB3xB")
TEMP_NONE = -32768

# Section 4.
BASELINE_NIGHTS = 28
CALIBRATING_BELOW = 7
LOW_CONFIDENCE_BELOW = 14
SD_FLOOR = {"ln_rmssd": 0.05, "rhr": 1.5, "sleep_min": 20.0, "temp": 0.15}
Z_CLAMP = 3.0
MAD_TO_SD = 1.4826

# Section 6.
DAY_BASELINE_DAYS = 28
DAY_BASELINE_MIN = 10
STRESS_STEPS_BEFORE = 200
STRESS_STEPS_WINDOW_S = 30 * 60


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


assert crc8(b"123456789") == 0xF4


# --- records -------------------------------------------------------------------

def parse_hex(h):
    raw = bytes.fromhex(h)
    (seq, t, steps, temp, skin, hr, hr_min, hr_max, rmssd, rest_hr,
     rest_rmssd, rest_beats, rise, motion, batt, flags, hot, boot,
     flags2, crc) = REC2.unpack(raw)
    return {"seq": seq, "time_s": t, "steps": steps,
            "temp_c100": None if temp == TEMP_NONE else temp,
            "hr_bpm": hr, "hr_min_bpm": hr_min, "hr_max_bpm": hr_max,
            "rest_hr_bpm": rest_hr, "rest_rmssd_ms": rest_rmssd,
            "rest_beats": rest_beats, "temp_rise_c10": rise,
            "motion_pct": None if motion == 255 else motion,
            "flags": flags, "hot_flashes": hot,
            "spot_manual": bool(flags2 & 1),
            "spot_breathing": bool(flags2 & 2),
            "crc_ok": crc8(raw[:31]) == crc}


def encode_hex(r):
    raw = REC2.pack(r["seq"], r["time_s"], r["steps"],
                    TEMP_NONE if r["temp_c100"] is None else r["temp_c100"],
                    TEMP_NONE, r["hr_bpm"], r["hr_min_bpm"], r["hr_max_bpm"],
                    0, r["rest_hr_bpm"], r["rest_rmssd_ms"], r["rest_beats"],
                    r["temp_rise_c10"],
                    255 if r["motion_pct"] is None else r["motion_pct"],
                    190, r["flags"], r["hot_flashes"], 0,
                    (1 if r.get("spot_manual") else 0) |
                    (2 if r.get("spot_breathing") else 0), 0)
    return (raw[:31] + bytes([crc8(raw[:31])])).hex()


def load_csv(path):
    """A history_sync.py v2 CSV. wall_time is local and becomes `t`."""
    out = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if not row.get("wall_time") or row.get("crc_ok") == "False":
                continue

            def num(k, scale=1, none=None):
                v = row.get(k, "")
                return none if v == "" else int(round(float(v) * scale))
            flags = 0
            for name in (row.get("flags") or "").split("|"):
                if name in FLAG_NAMES:
                    flags |= 1 << FLAG_NAMES.index(name)
            t = calendar.timegm(datetime.datetime.fromisoformat(
                row["wall_time"]).timetuple())
            out.append({"seq": int(row["seq"]), "t": t,
                        "steps": num("steps", none=0),
                        "temp_c100": num("temp_c", 100),
                        "hr_bpm": num("hr_bpm", none=0),
                        "hr_min_bpm": num("hr_min_bpm", none=0),
                        "hr_max_bpm": num("hr_max_bpm", none=0),
                        "rest_hr_bpm": num("rest_hr_bpm", none=0),
                        "rest_rmssd_ms": num("rest_rmssd_ms", none=0),
                        "rest_beats": num("rest_beats", none=0),
                        "temp_rise_c10": num("temp_rise_c", 10, 0),
                        "motion_pct": num("motion_pct"),
                        "flags": flags, "hot_flashes": num("hot_flashes", none=0),
                        "spot_manual": row.get("spot_manual") == "True",
                        "spot_breathing": row.get("spot_breathing") == "True",
                        "crc_ok": True})
    return out


# --- section 2: hygiene ----------------------------------------------------------

def clean(records, tz_offset_s=0):
    """
    Drop bad records, add `t` (local wall-clock seconds) and two verdicts:
    temp_ok and rest_ok. Records must already be dated: TIME_VALID set, or
    dated by the app (handoff section 6) before they get here.

    DECIDED: a "worn run" is consecutive WORN records no more than 10 min
    apart; temperature is skipped for its first 30 min.
    """
    out = []
    for r in records:
        if not r.get("crc_ok", True):
            continue
        r = dict(r)
        if "t" not in r:
            if not r["flags"] & F_TIME:
                continue
            r["t"] = r["time_s"] + tz_offset_s
        out.append(r)
    out.sort(key=lambda r: (r["t"], r["seq"]))

    run_start = None
    prev_t = None
    for r in out:
        worn = bool(r["flags"] & F_WORN)
        if worn and (run_start is None or prev_t is None or r["t"] - prev_t > 600):
            run_start = r["t"]
        if not worn:
            run_start = None
        prev_t = r["t"] if worn else None
        r["temp_ok"] = (r["temp_c100"] is not None and worn and
                        not r["flags"] & F_CHG and
                        r["t"] - run_start >= 30 * 60)
        r["rest_ok"] = (bool(r["flags"] & (F_SPOT_N | F_SPOT_D)) and
                        r["rest_beats"] >= 30 and r["rest_rmssd_ms"] > 0)
    return out


# --- small statistics, defined exactly so a port can match ------------------------

def median(xs):
    """Mean of the two middle values for an even count."""
    return statistics.median(xs)


def percentile_nearest_rank(xs, p):
    """DECIDED: nearest rank, index ceil(p * n) - 1 into the sorted values."""
    s = sorted(xs)
    return s[max(0, math.ceil(p * len(s)) - 1)]


def robust(xs, floor):
    """(median, SD) with SD = MAD x 1.4826, floored."""
    m = median(xs)
    mad = median([abs(x - m) for x in xs])
    return m, max(mad * MAD_TO_SD, floor)


def zscore(v, centre, sd):
    return max(-Z_CLAMP, min(Z_CLAMP, (v - centre) / sd))


def sample_sd(xs):
    """DECIDED: n - 1 in the denominator."""
    return statistics.stdev(xs) if len(xs) > 1 else 0.0


def day_of(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).date()


def hour_of(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).hour


# --- section 3: nights -------------------------------------------------------------

def nights(recs):
    """
    The longest run of ASLEEP records (gaps up to 30 min) ending 03:00-14:00,
    per date it ends on. DECIDED: "longest" counts ASLEEP records; a gap is
    the time between consecutive ASLEEP records, so a run survives up to
    30 min of waking.
    """
    runs = []
    cur = []
    for r in recs:
        if not r["flags"] & F_SLEEP:
            continue
        if cur and r["t"] - cur[-1]["t"] > 30 * 60:
            runs.append(cur)
            cur = []
        cur.append(r)
    if cur:
        runs.append(cur)

    best = {}
    for run in runs:
        end = run[-1]["t"]
        if not 3 <= hour_of(end) < 14:
            continue
        d = day_of(end)
        if d not in best or len(run) > len(best[d]):
            best[d] = run

    out = []
    for d in sorted(best):
        run = best[d]
        t0, t1 = run[0]["t"], run[-1]["t"]
        span = [r for r in recs if t0 <= r["t"] <= t1]
        out.append(summarise_night(d, span, t0, t1))
    return out


def summarise_night(d, span, t0, t1):
    spot = [r for r in span if r["flags"] & F_SPOT_N and r["rest_hr_bpm"] > 0]
    asleep = [r for r in span if r["flags"] & F_SLEEP]

    if len(spot) >= 2:
        rhr, rhr_src = float(median([r["rest_hr_bpm"] for r in spot])), "spot"
    else:
        mins = [r["hr_min_bpm"] for r in asleep
                if r["flags"] & F_WORN and r["hr_min_bpm"] > 0]
        if len(mins) >= 12:
            rhr, rhr_src = float(percentile_nearest_rank(mins, 0.10)), "estimated"
        else:
            rhr, rhr_src = None, None

    lns = [math.log(r["rest_rmssd_ms"]) for r in spot if r["rest_ok"]]
    ln_rmssd = statistics.fmean(lns) if len(lns) >= 2 else None

    sleep_min = sum(EPOCH_S for r in asleep if not r["flags"] & F_CHG) / 60.0
    motions = [r["motion_pct"] for r in asleep if r["motion_pct"] is not None]
    temps = [r["temp_c100"] / 100.0 for r in asleep
             if r["flags"] & F_WORN and r["temp_ok"]]

    return {"date": d.isoformat(), "start_t": t0, "end_t": t1,
            "rhr": rhr, "rhr_source": rhr_src,
            "ln_rmssd": ln_rmssd, "night_readings": len(lns),
            "sleep_min": sleep_min,
            "restless": statistics.fmean(motions) if motions else None,
            # DECIDED: 2 hours = 24 usable epochs.
            "temp_mean": statistics.fmean(temps) if len(temps) >= 24 else None,
            "hot_flashes": sum(r["hot_flashes"] for r in span),
            "qualifies": sleep_min >= 180}


# --- section 4: baselines ----------------------------------------------------------

def baseline(prior, metric, floor):
    vals = [n[metric] for n in prior if n[metric] is not None]
    if len(vals) < CALIBRATING_BELOW:
        return None
    return robust(vals, floor)


def prior_nights(all_nights, before_date, count=BASELINE_NIGHTS):
    q = [n for n in all_nights if n["qualifies"] and n["date"] < before_date]
    return q[-count:]


# --- section 5: recovery, strain ---------------------------------------------------

def recovery(all_nights):
    """
    Per qualifying night. DECIDED: no score without ln_rmssd and rhr; a
    missing temperature counts as z 0. temp_dev is against the median of
    the baseline nights' temp_mean.
    """
    out = {}
    for n in all_nights:
        if not n["qualifies"]:
            continue
        prior = prior_nights(all_nights, n["date"])
        rec = {"n_baseline": len(prior), "score": None, "temp_dev": None,
               "confidence": None}
        out[n["date"]] = rec
        if len(prior) < CALIBRATING_BELOW:
            rec["confidence"] = "calibrating"
            continue
        rec["confidence"] = "low" if len(prior) < LOW_CONFIDENCE_BELOW else "normal"
        b_ln = baseline(prior, "ln_rmssd", SD_FLOOR["ln_rmssd"])
        b_rhr = baseline(prior, "rhr", SD_FLOOR["rhr"])
        b_sleep = baseline(prior, "sleep_min", SD_FLOOR["sleep_min"])
        b_temp = baseline(prior, "temp_mean", SD_FLOOR["temp"])
        if n["temp_mean"] is not None and b_temp:
            rec["temp_dev"] = n["temp_mean"] - b_temp[0]
        if n["ln_rmssd"] is None or n["rhr"] is None or not (b_ln and b_rhr and b_sleep):
            continue
        z_ln = zscore(n["ln_rmssd"], *b_ln)
        z_rhr = zscore(n["rhr"], *b_rhr)
        z_sleep = zscore(n["sleep_min"], *b_sleep)
        z_temp = zscore(n["temp_mean"], *b_temp) if rec["temp_dev"] is not None else 0.0
        rec.update(z_ln_rmssd=z_ln, z_rhr=z_rhr, z_sleep=z_sleep, z_temp=z_temp)
        rec["score"] = max(0.0, min(100.0, 50 + 12 * z_ln - 8 * z_rhr +
                                    5 * z_sleep - 4 * abs(z_temp)))
    return out


def strain_flags(all_nights, stress_readings, date):
    """
    The section-5 flags for one calendar date. DECIDED: "the last 7 nights"
    are the qualifying nights dated date-6..date (at least 4 of them), and
    their baseline is the 28 qualifying nights dated before date-6 (at
    least 7). Day stress needs 3 scored scheduled readings (not manual) in
    the same 7 days.
    """
    d = datetime.date.fromisoformat(date)
    lo = (d - datetime.timedelta(days=6)).isoformat()
    recent = [n for n in all_nights if n["qualifies"] and lo <= n["date"] <= date]
    prior = prior_nights(all_nights, lo)
    if len(recent) < 4 or len(prior) < CALIBRATING_BELOW:
        return None

    flags = {}
    r_ln = [n["ln_rmssd"] for n in recent if n["ln_rmssd"] is not None]
    p_ln = [n["ln_rmssd"] for n in prior if n["ln_rmssd"] is not None]
    if len(r_ln) >= 4 and len(p_ln) >= CALIBRATING_BELOW:
        m, sd = robust(p_ln, SD_FLOOR["ln_rmssd"])
        flags["hrv_down"] = statistics.fmean(r_ln) < m - 0.5 * sd
        cv_r = sample_sd(r_ln) / statistics.fmean(r_ln)
        cv_p = sample_sd(p_ln) / statistics.fmean(p_ln)
        flags["hrv_erratic"] = cv_r > cv_p * 1.5
    r_rhr = [n["rhr"] for n in recent if n["rhr"] is not None]
    p_rhr = [n["rhr"] for n in prior if n["rhr"] is not None]
    if len(r_rhr) >= 4 and len(p_rhr) >= CALIBRATING_BELOW:
        m, sd = robust(p_rhr, SD_FLOOR["rhr"])
        mean = statistics.fmean(r_rhr)
        flags["rhr_up"] = mean > m + 0.5 * sd and mean - m >= 2.0
    m_sleep = median([n["sleep_min"] for n in prior])
    mean_sleep = statistics.fmean([n["sleep_min"] for n in recent])
    flags["sleep_short"] = mean_sleep < m_sleep - 30 or mean_sleep < 360
    scores = [s["score"] for s in stress_readings
              if s["score"] is not None and not s["manual"] and lo <= s["date"] <= date]
    if len(scores) >= 3:
        flags["day_stress_high"] = statistics.fmean(scores) > 60
    flags["count"] = sum(1 for k, v in flags.items() if v is True)
    return flags


def strain_levels(all_nights, stress_readings):
    """Steady / strained / sustained, for every date from the first night."""
    if not all_nights:
        return {}, {}
    d0 = datetime.date.fromisoformat(all_nights[0]["date"])
    d1 = datetime.date.fromisoformat(all_nights[-1]["date"])
    flags, counts = {}, {}
    d = d0
    while d <= d1:
        f = strain_flags(all_nights, stress_readings, d.isoformat())
        flags[d.isoformat()] = f
        counts[d] = f["count"] if f else 0
        d += datetime.timedelta(days=1)

    levels = {}
    for d in sorted(counts):
        last14 = [counts.get(d - datetime.timedelta(days=i), 0) for i in range(14)]
        last7 = last14[:7]
        if sum(1 for c in last14 if c >= 3) >= 10:
            levels[d.isoformat()] = "sustained"
        elif sum(1 for c in last7 if c >= 2) >= 5:
            levels[d.isoformat()] = "strained"
        else:
            levels[d.isoformat()] = "steady"
    return flags, levels


# --- section 6: stress ---------------------------------------------------------------

def band(score):
    if score is None:
        return None
    if score < 35:
        return "relaxed"
    if score <= 65:
        return "normal"
    if score <= 80:
        return "elevated"
    return "high"


def stress(recs, all_nights):
    """
    Every SPOT_DAY reading. DECIDED: excluded (no score, no baseline) when
    its own record has WORKOUT or SPOT_BREATHING (flags2 bit 1, v0.9.0+: a
    breathing exercise raises RMSSD on purpose), or the records in the 30
    min before it hold more than 200 steps. A SPOT_MANUAL reading that is
    not breathing ("check my stress", flags2 bit 0) is scored but never
    enters a baseline: people press the button when they already feel
    stressed. The night RHR baseline is the median rhr of the last 28
    qualifying nights that ended before the reading (at least 7). The day
    baseline is the last 28 days of scheduled, non-excluded readings with
    an RMSSD (at least 10); hr_up for each of those is taken against the
    same night baseline as the reading being scored. z is clamped to +-3.
    """
    readings = [r for r in recs if r["flags"] & F_SPOT_D and r["rest_hr_bpm"] > 0]
    for r in readings:
        before = sum(x["steps"] for x in recs
                     if r["t"] - STRESS_STEPS_WINDOW_S <= x["t"] < r["t"])
        r["_excluded"] = (bool(r["flags"] & F_WORKOUT) or r.get("spot_breathing", False)
                          or before > STRESS_STEPS_BEFORE)

    out = []
    for r in readings:
        item = {"seq": r["seq"], "t": r["t"], "date": day_of(r["t"]).isoformat(),
                "hour": hour_of(r["t"]), "hr": r["rest_hr_bpm"],
                "rmssd": r["rest_rmssd_ms"] if r["rest_ok"] else None,
                "manual": bool(r.get("spot_manual", False)),
                "excluded": r["_excluded"], "score": None, "band": None,
                "confidence": None}
        out.append(item)
        if r["_excluded"]:
            continue
        rhrs = [n["rhr"] for n in all_nights
                if n["qualifies"] and n["end_t"] < r["t"] and n["rhr"] is not None]
        rhrs = rhrs[-BASELINE_NIGHTS:]
        base = [x for x in readings if r["t"] - DAY_BASELINE_DAYS * 86400 <= x["t"] < r["t"]
                and not x["_excluded"] and x["rest_ok"] and not x.get("spot_manual", False)]
        if len(rhrs) < CALIBRATING_BELOW or len(base) < DAY_BASELINE_MIN:
            continue
        rhr_base = median(rhrs)
        hr_up = r["rest_hr_bpm"] - rhr_base
        hr_up_med = median([x["rest_hr_bpm"] - rhr_base for x in base])
        if r["rest_ok"]:
            m, sd = robust([math.log(x["rest_rmssd_ms"]) for x in base],
                           SD_FLOOR["ln_rmssd"])
            z = zscore(math.log(r["rest_rmssd_ms"]), m, sd)
            score = 50 - 15 * z + 1.5 * (hr_up - hr_up_med)
            item["confidence"] = "normal"
        else:
            score = 50 + 1.5 * (hr_up - hr_up_med)
            item["confidence"] = "low"
        item["score"] = max(0.0, min(100.0, score))
        item["band"] = band(item["score"])
    return out


# --- section 7: hot flashes ----------------------------------------------------------

def hot_flash_events(recs, all_nights):
    """
    DECIDED: an event starts at a HOT_FLASH record with hot_flashes > 0 and
    runs through the following HOT_FLASH records no more than 10 min apart
    (a new hot_flashes > 0 starts a new event). Woke: any record from the
    start to two records after the end with motion_pct >= 20, or without
    ASLEEP when the record before the event had it.
    """
    events = []
    i = 0
    while i < len(recs):
        r = recs[i]
        if not (r["flags"] & F_HOT and r["hot_flashes"] > 0):
            i += 1
            continue
        j = i + 1
        while (j < len(recs) and recs[j]["flags"] & F_HOT and recs[j]["hot_flashes"] == 0
               and recs[j]["t"] - recs[j - 1]["t"] <= 600):
            j += 1
        ev = recs[i:j]
        tail = recs[i:min(len(recs), j + 2)]
        was_asleep = i > 0 and bool(recs[i - 1]["flags"] & F_SLEEP)
        woke = any((x["motion_pct"] or 0) >= 20 for x in tail) or (
            was_asleep and any(not x["flags"] & F_SLEEP for x in tail))
        night = next((n for n in all_nights if n["start_t"] <= r["t"] <= n["end_t"]), None)
        hr_peak = max((x["hr_max_bpm"] for x in ev), default=0)
        events.append({
            "start_seq": r["seq"], "start_t": r["t"] - EPOCH_S,
            "duration_min": len(ev) * EPOCH_S // 60,
            "peak_rise_c": max(x["temp_rise_c10"] for x in ev) / 10.0,
            "hr_change": (hr_peak - night["rhr"]) if night and night["rhr"] and hr_peak else None,
            "woke": woke, "night": night["date"] if night else None})
        i = j
    return events


# --- everything ------------------------------------------------------------------------

def analyse(records, tz_offset_s=0):
    recs = clean(records, tz_offset_s)
    ns = nights(recs)
    st = stress(recs, ns)
    flags, levels = strain_levels(ns, st)
    return {"nights": ns, "recovery": recovery(ns), "stress": st,
            "strain_flags": flags, "strain_level": levels,
            "hot_flash_events": hot_flash_events(recs, ns)}


# --- synthetic wearer, for the vectors -----------------------------------------------------

START = calendar.timegm((2026, 9, 1, 0, 0, 0))
DAYS = 49
STRAIN_FROM = 35            # nights 35.. sleep less, HRV falls and scatters, RHR rises
HOT_FLASH_NIGHTS = (10, 20, 30, 40, 41, 42)
WOKE_NIGHT = 41
WORKOUT_WEEKDAY = 3         # days with day % 7 == 3 have a 17:00-18:00 workout
WALK_WEEKDAY = 5            # ...and day % 7 == 5 a 10:30-11:00 walk before a reading
CHECK_EVERY = 5             # a "check my stress" press at 15:30 on day % 5 == 2
BREATHING_EVERY = 9         # a breathing exercise at 20:30 on day % 9 == 4


def generate(seed=2026):
    """
    DAYS days of 5-minute records from START (UTC, tz offset 0). Night n
    ends on the morning of day n and has one set of physiology; day n's
    readings follow night n. Night 0 began before the first record.
    """
    rng = random.Random(seed)
    nightp = []
    for n in range(DAYS + 1):
        strained = n >= STRAIN_FROM
        nightp.append({
            "ln": rng.gauss(3.80 - (0.30 if strained else 0.0),
                            0.25 if strained else 0.07),
            "rhr": rng.gauss(56.0 if strained else 52.0, 1.0),
            # minutes from the midnight that ends the night: 23:00 = -60
            "onset": -60 + rng.randint(-25, 25),
            "len": (390 if strained else 450) + rng.randint(-20, 20),
            "skin": rng.gauss(33.0, 0.1)})

    recs = []
    for k in range(DAYS * 288):
        t = START + k * EPOCH_S
        day, m = divmod(k * 5, 1440)
        if m - 1440 >= nightp[day + 1]["onset"]:
            n, rel = day + 1, m - 1440          # tonight, already asleep
        else:
            n, rel = day, m
        p = nightp[n]
        asleep = p["onset"] <= rel < p["onset"] + p["len"]
        r = {"seq": k + 1, "time_s": t, "steps": 0, "temp_c100": 3300,
             "hr_bpm": 0, "hr_min_bpm": 0, "hr_max_bpm": 0, "rest_hr_bpm": 0,
             "rest_rmssd_ms": 0, "rest_beats": 0, "temp_rise_c10": 0,
             "motion_pct": 2, "flags": F_TIME, "hot_flashes": 0}
        recs.append(r)

        if not asleep and 18 * 60 <= m < 19 * 60:
            r.update(flags=F_TIME | F_CHG, temp_c100=3650, motion_pct=0)
            continue

        if asleep:
            mins_in = rel - p["onset"]
            r.update(flags=F_TIME | F_WORN | F_SLEEP,
                     hr_bpm=int(p["rhr"] + 3), hr_min_bpm=int(p["rhr"]),
                     hr_max_bpm=int(p["rhr"] + 7),
                     temp_c100=int(p["skin"] * 100) + rng.randint(-5, 5),
                     motion_pct=rng.randint(0, 6))
            # Night readings: 15 min after onset, then hourly.
            if mins_in >= 15 and (mins_in - 15) % 60 < 5:
                r["flags"] |= F_SPOT_N
                r["rest_hr_bpm"] = int(round(p["rhr"] + rng.gauss(0, 0.8)))
                r["rest_rmssd_ms"] = max(5, int(round(math.exp(
                    p["ln"] + rng.gauss(0, 0.05)))))
                r["rest_beats"] = 150
            # A hot flash at 03:00, three epochs.
            if n in HOT_FLASH_NIGHTS and 180 <= rel < 195:
                i = (rel - 180) // 5
                r["flags"] |= F_HOT
                r["hot_flashes"] = 1 if i == 0 else 0
                r["temp_rise_c10"] = (12, 18, 16)[i]
                r["temp_c100"] += (120, 180, 160)[i]
                r["hr_max_bpm"] = int(p["rhr"] + 12)
                if n == WOKE_NIGHT and i == 1:
                    r["motion_pct"] = 35
            continue

        flags = F_TIME | F_WORN
        steps = rng.randint(0, 30)      # desk work: never 200 in 30 min
        if day % 7 == WORKOUT_WEEKDAY and 17 * 60 <= m < 18 * 60:
            flags |= F_WORKOUT
            steps = 900
        if day % 7 == WALK_WEEKDAY and 10 * 60 + 30 <= m < 11 * 60:
            steps = 120
        r.update(flags=flags, steps=steps, hr_bpm=72, hr_min_bpm=66,
                 hr_max_bpm=85,
                 temp_c100=int(p["skin"] * 100) - 150 + rng.randint(-10, 10),
                 motion_pct=rng.randint(5, 40))
        # Manual readings, off the hour so they never meet a scheduled one.
        if day >= 7 and day % CHECK_EVERY == 2 and m == 15 * 60 + 30:
            r["flags"] |= F_SPOT_D
            r["spot_manual"] = True
            r["steps"] = 0
            r["rest_hr_bpm"] = int(round(p["rhr"] + 22))      # pressed when tense
            r["rest_rmssd_ms"] = max(5, int(round(math.exp(p["ln"] - 0.6))))
            r["rest_beats"] = 160
        if day >= 7 and day % BREATHING_EVERY == 4 and m == 20 * 60 + 30:
            r["flags"] |= F_SPOT_D
            r["spot_manual"] = True
            r["spot_breathing"] = True
            r["steps"] = 0
            r["rest_hr_bpm"] = int(round(p["rhr"] + 2))
            r["rest_rmssd_ms"] = max(5, int(round(math.exp(p["ln"] + 0.5))))
            r["rest_beats"] = 160
        # Day readings on the hour, 09:00-17:00; afternoons run stressed.
        if 9 <= m // 60 <= 17 and m % 60 == 0:
            afternoon = m // 60 >= 13
            r["flags"] |= F_SPOT_D
            r["steps"] = 0
            r["rest_hr_bpm"] = int(round(p["rhr"] + 8 + (6 if afternoon else 0)
                                         + rng.gauss(0, 2)))
            ln = p["ln"] - 0.2 - (0.25 if afternoon else 0.0) + rng.gauss(0, 0.1)
            r["rest_rmssd_ms"] = max(5, int(round(math.exp(ln))))
            # Now and then a reading too short for an RMSSD.
            r["rest_beats"] = 25 if (day * 24 + m // 60) % 23 == 0 else 160
    return recs


def round_floats(o, nd=6):
    if isinstance(o, float):
        return round(o, nd)
    if isinstance(o, dict):
        return {k: round_floats(v, nd) for k, v in o.items()}
    if isinstance(o, list):
        return [round_floats(v, nd) for v in o]
    return o


def write_vectors(path):
    records = generate()
    hexes = [encode_hex(r) for r in records]
    parsed = [parse_hex(h) for h in hexes]
    result = analyse(parsed)
    doc = {
        "about": "Test vectors for WELLNESS_ANALYTICS.md sections 2-7, from "
                 "ring-fw/tools/wellness_ref.py. Parse `records` with the v2 "
                 "parser, run the analytics with tz_offset_s, compare to "
                 "`expected`; floats to 1e-6. DECIDED choices: section 11.",
        "tz_offset_s": 0, "epoch_s": EPOCH_S, "seed": 2026,
        "records": hexes,
        "expected": round_floats(result),
    }
    with open(path, "w") as f:
        json.dump(doc, f, separators=(",", ":"), default=str)
    return records, result


# --- selftest ----------------------------------------------------------------------------

def selftest():
    records = generate()
    for r in records[:200]:
        assert parse_hex(encode_hex(r))["crc_ok"]
    res = analyse([parse_hex(encode_hex(r)) for r in records])
    ns, rec, lv, st, ev = (res["nights"], res["recovery"], res["strain_level"],
                           res["stress"], res["hot_flash_events"])
    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    check(len(ns) == DAYS, "%d nights from %d days (want %d)" % (len(ns), DAYS, DAYS))
    check(all(n["qualifies"] for n in ns) and
          all(n["night_readings"] >= 6 for n in ns[1:]),
          "every night qualifies; every full night has 6+ night readings")
    first = [rec[n["date"]] for n in ns[:7]]
    check(all(r["confidence"] == "calibrating" and r["score"] is None for r in first),
          "first 7 nights: calibrating, no score")
    mid = [rec[n["date"]]["score"] for n in ns[14:STRAIN_FROM]]
    late = [rec[n["date"]]["score"] for n in ns[STRAIN_FROM + 2:]]
    check(statistics.fmean(mid) > 40 and statistics.fmean(late) < statistics.fmean(mid) - 20,
          "recovery: steady weeks %.0f, strained weeks %.0f" % (
              statistics.fmean(mid), statistics.fmean(late)))
    steady_days = [lv[n["date"]] for n in ns[14:STRAIN_FROM]]
    check(all(x == "steady" for x in steady_days), "steady before the strain begins")
    check(lv[ns[-1]["date"]] == "sustained",
          "sustained strain by the last day (%s)" % lv[ns[-1]["date"]])
    first_strained = next((n["date"] for n in ns if lv[n["date"]] != "steady"), None)
    check(first_strained is not None and first_strained >= ns[STRAIN_FROM]["date"],
          "first non-steady day %s, not before night %d (%s)" % (
              first_strained, STRAIN_FROM, ns[STRAIN_FROM]["date"]))
    check(len(ev) == len(HOT_FLASH_NIGHTS), "%d hot-flash events (want %d)" % (
        len(ev), len(HOT_FLASH_NIGHTS)))
    check([e["night"] for e in ev] == [ns[n]["date"] for n in HOT_FLASH_NIGHTS],
          "events land on the right nights")
    woke = [e for e in ev if e["woke"]]
    check(len(woke) == 1 and woke[0]["night"] == ns[WOKE_NIGHT]["date"],
          "exactly one event woke the wearer, on night %d" % WOKE_NIGHT)
    check(all(e["duration_min"] == 15 and e["peak_rise_c"] == 1.8 and
              e["hr_change"] is not None and e["hr_change"] > 8 for e in ev),
          "events: 15 min, 1.8 C peak, HR up >8 bpm")
    scored = [s for s in st if s["score"] is not None]
    am = [s["score"] for s in scored if s["hour"] < 13]
    pm = [s["score"] for s in scored if s["hour"] >= 13]
    check(bool(scored) and statistics.fmean(pm) > statistics.fmean(am) + 10,
          "afternoons score higher than mornings (%.0f vs %.0f)" % (
              statistics.fmean(pm), statistics.fmean(am)))
    check(any(s["confidence"] == "low" for s in scored),
          "a reading under 30 beats is scored HR-only, low confidence")
    d0 = datetime.date(2026, 9, 1)
    workout = [s for s in st if s["hour"] == 17 and
               (datetime.date.fromisoformat(s["date"]) - d0).days % 7 == WORKOUT_WEEKDAY]
    check(len(workout) == 7 and all(s["excluded"] for s in workout),
          "the 17:00 reading on each of the 7 workout days is excluded")
    walk = [s for s in st if s["hour"] == 11 and
            (datetime.date.fromisoformat(s["date"]) - d0).days % 7 == WALK_WEEKDAY]
    check(len(walk) == 7 and all(s["excluded"] for s in walk),
          "the 11:00 reading after each of the 7 walks is excluded")
    breathing = [s for s in st if s["hour"] == 20]
    checks = [s for s in st if s["hour"] == 15 and s["manual"]]
    check(len(breathing) > 0 and all(s["excluded"] and s["score"] is None for s in breathing),
          "%d breathing exercises: excluded, never scored" % len(breathing))
    check(len(checks) > 0 and all(s["score"] is not None and s["band"] in ("elevated", "high")
                                  for s in checks if not s["excluded"]),
          "%d 'check my stress' presses are scored, and read stressed" % len(checks))
    check(not any(s["excluded"] for s in st
                  if s not in workout and s not in walk and s not in breathing),
          "nothing else is excluded")
    # Manual readings must not move any scheduled reading's score.
    plain = [dict(r) for r in records if not r.get("spot_manual")]
    res2 = analyse([parse_hex(encode_hex(r)) for r in plain])
    s2 = {s["seq"]: s["score"] for s in res2["stress"]}
    check(all(s["score"] == s2[s["seq"]] for s in st if not s["manual"]),
          "manual readings leave every scheduled score unchanged")
    print("selftest: %d failures" % len(fails))
    return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("csv", nargs="?", help="history_*.csv from history_sync.py")
    ap.add_argument("--vectors", nargs="?", const=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "mac-handoff",
        "wellness_vectors.json"), help="write the test vectors (default: mac-handoff/)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(0 if selftest() else 1)
    if args.vectors:
        records, res = write_vectors(args.vectors)
        print("wrote %s: %d records, %d nights, %d stress readings, %d events" % (
            os.path.normpath(args.vectors), len(records), len(res["nights"]),
            len(res["stress"]), len(res["hot_flash_events"])))
        return
    if not args.csv:
        ap.error("give a CSV, --vectors or --selftest")

    res = analyse(load_csv(args.csv))
    print("night        rhr   lnRMSSD  sleep  temp   flashes  recovery")
    for n in res["nights"]:
        r = res["recovery"].get(n["date"], {})
        print("%s  %5s  %7s  %5.0f  %5s  %7d  %s" % (
            n["date"], "-" if n["rhr"] is None else "%.0f" % n["rhr"],
            "-" if n["ln_rmssd"] is None else "%.2f" % n["ln_rmssd"],
            n["sleep_min"], "-" if r.get("temp_dev") is None else "%+.2f" % r["temp_dev"],
            n["hot_flashes"],
            r.get("confidence") if r.get("score") is None else "%.0f" % r["score"]))
    print("\nstrain:", ", ".join("%s %s" % kv for kv in list(res["strain_level"].items())[-7:]))
    print("stress readings: %d (%d scored)" % (
        len(res["stress"]), sum(1 for s in res["stress"] if s["score"] is not None)))
    for e in res["hot_flash_events"]:
        print("hot flash  night %s  %d min  +%.1f C  HR %s  %s" % (
            e["night"], e["duration_min"], e["peak_rise_c"],
            "-" if e["hr_change"] is None else "%+.0f" % e["hr_change"],
            "woke" if e["woke"] else ""))


if __name__ == "__main__":
    main()
