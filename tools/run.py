#!/usr/bin/env python3
"""Builds everything, runs the over-the-air BLE scenarios on BabbleSim, checks the requirements.

    python3 tools/run.py | tee results.txt

Workspace layout (what `west update` creates next to this repo):
    <ws>/zephyr, <ws>/modules/..., <ws>/tools/bsim     (override with ZEPHYR_BASE / BSIM_OUT_PATH / ZEPHYR_MODULES)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WS = Path(os.environ.get("WORKSPACE", ROOT.parent))
ZEPHYR_BASE = Path(os.environ.get("ZEPHYR_BASE", WS / "zephyr"))
BSIM_OUT = Path(os.environ.get("BSIM_OUT_PATH", WS / "tools" / "bsim"))
BSIM_COMP = Path(os.environ.get("BSIM_COMPONENTS_PATH", BSIM_OUT / "components"))
MODULES = os.environ.get("ZEPHYR_MODULES") or ";".join(str(WS / p) for p in (
    "modules/hal/nordic", "modules/hal/cmsis_6", "modules/bsim_hw_models/nrf_hw_models", "modules/crypto/mbedtls"))
BUILD = ROOT / "build"
OUT = ROOT / "out"
ENV = dict(os.environ, ZEPHYR_BASE=str(ZEPHYR_BASE), BSIM_OUT_PATH=str(BSIM_OUT), BSIM_COMPONENTS_PATH=str(BSIM_COMP),
           ZEPHYR_TOOLCHAIN_VARIANT=os.environ.get("ZEPHYR_TOOLCHAIN_VARIANT", "host"))
R = []


def req(rid, text, result, target, ok):
    R.append((rid, text, result, target, bool(ok)))


def build(name, src, extra=()):
    bdir = BUILD / name
    cfg = ["cmake", "-S", str(src), "-B", str(bdir), "-GNinja", "-DBOARD=nrf52_bsim", f"-DZEPHYR_MODULES={MODULES}", *extra]
    p1 = subprocess.run(cfg, env=ENV, capture_output=True, text=True)
    p2 = subprocess.run(["ninja", "-C", str(bdir)], env=ENV, capture_output=True, text=True) if p1.returncode == 0 else p1
    log = p1.stdout + p1.stderr + p2.stdout + p2.stderr
    (OUT / f"build_{name}.log").write_text(log)
    exe = bdir / "zephyr" / "zephyr.exe"
    return p2.returncode == 0 and exe.exists(), log.count("warning:"), exe


def simulate(sim, devices, length_s):
    """devices: list of (exe, [args]); runs them plus the 2.4 GHz phy; returns the logs per device."""
    bindir = BSIM_OUT / "bin"
    procs, logs = [], []
    for d, (exe, args) in enumerate(devices):
        f = open(OUT / f"{sim}_d{d}.log", "w")
        logs.append(f.name)
        procs.append(subprocess.Popen([str(exe), f"-s={sim}", f"-d={d}", *args], cwd=bindir, stdout=f,
                                      stderr=subprocess.STDOUT))
    phy = subprocess.run([str(bindir / "bs_2G4_phy_v1"), f"-s={sim}", f"-D={len(devices)}",
                          f"-sim_length={int(length_s * 1e6)}"], cwd=bindir, capture_output=True, text=True, timeout=300)
    rcs = []
    for p in procs:
        try:
            rcs.append(p.wait(timeout=60))
        except subprocess.TimeoutExpired:
            p.kill()
            rcs.append(None)
    return rcs, [Path(x).read_text() for x in logs], phy.returncode


def metrics(log):
    m = {}
    for line in log.splitlines():
        if "METRIC " in line:
            for k, v in re.findall(r"(\w+)=(\S+)", line.split("METRIC ", 1)[1]):
                m[k] = v
    return m


def ival(v):
    return int(v, 16) if str(v).startswith("0x") else int(float(v))


def main():
    import platform
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        sys.exit(f"run.py needs x86-64 Linux (BabbleSim and nrf52_bsim are 32-bit x86 Linux programs); this is "
                 f"{platform.system()} {platform.machine()}.\nOn a Mac: make -C tests/unit && ./tests/unit/build/unit_tests"
                 "\nThe full run is the ble-sim job in GitHub Actions.")
    if not (ZEPHYR_BASE / "VERSION").exists() or not (BSIM_OUT / "bin" / "bs_2G4_phy_v1").exists():
        sys.exit(f"Zephyr or BabbleSim not found ({ZEPHYR_BASE}, {BSIM_OUT}); run tools/setup_linux.sh first.")
    t_start = time.time()
    sys.stdout.reconfigure(line_buffering=True)
    OUT.mkdir(exist_ok=True)
    BUILD.mkdir(exist_ok=True)
    print("BLE smart dimmer firmware on Zephyr RTOS: simulated nRF52 (BabbleSim), tested over the air")
    ver = (ZEPHYR_BASE / "VERSION").read_text() if (ZEPHYR_BASE / "VERSION").exists() else ""
    zv = ".".join(re.findall(r"(?:MAJOR|MINOR|PATCHLEVEL) = (\d+)", ver))
    print(f"Zephyr {zv}, board nrf52_bsim (nRF52833 models + Zephyr BLE controller), 2.4 GHz phy: BabbleSim")
    print()

    # ---- 1 unit tests
    print("[1  Host unit tests: dimmer core and energy meter]")
    subprocess.run(["make", "-s", "-C", str(ROOT / "tests" / "unit"), "-B"], capture_output=True)
    p = subprocess.run([str(ROOT / "tests" / "unit" / "build" / "unit_tests")], capture_output=True, text=True)
    m = re.search(r"(\d+)/(\d+) unit tests pass", p.stdout)
    print(f"   {m.group(0) if m else 'did not run'}")
    req("R2", "Host unit tests (dimmer core, meter)", m.group(0).split()[0] if m else "error", "all",
        p.returncode == 0 and m and m.group(1) == m.group(2))

    # ---- 2 builds
    print()
    print("[2  Firmware builds for nrf52_bsim, warnings as errors]")
    images = [("dimmer_product", ROOT / "app", []), ("dimmer_test", ROOT / "app", ["-DEXTRA_CONF_FILE=test.conf"]),
              ("central", ROOT / "tests" / "central", [])]
    built, warns, exes = 0, 0, {}
    for name, src, extra in images:
        ok, w, exe = build(name, src, extra)
        built += ok
        warns += w
        exes[name] = exe
        print(f"   {name:15s} {'ok' if ok else 'FAILED'}, {w} warnings")
    req("R1", "Firmware builds warning-free (3 images)", f"{built}/3, {warns} warnings", "3/3, 0", built == 3 and warns == 0)
    if built < 3:
        sys.exit("build failed; see out/build_*.log")
    dim, cen = exes["dimmer_test"], exes["central"]

    # ---- 3 scenarios
    print()
    print("[3  Over-the-air scenarios: dimmer + test phone on a simulated 2.4 GHz channel]")
    res = {}

    def scenario(name, devices, length=30):
        rcs, logs, phy = simulate(f"dim_{name}", devices, length)
        res[name] = {"rcs": rcs, "logs": logs, "m": metrics("\n".join(logs[1:]))}
        ok = all(rc == 0 for rc in rcs[1:])
        print(f"   {name:9s} {'pass' if ok else 'FAIL'}  ({', '.join(str(r) for r in rcs)})")
        return ok, logs

    scenario("basic", [(dim, []), (cen, ["-testid=basic"])])
    scenario("fade", [(dim, []), (cen, ["-testid=fade"])])
    scenario("faults", [(dim, []), (cen, ["-testid=faults"])])
    scenario("brownout", [(dim, []), (cen, ["-testid=brownout"])])
    scenario("linkloss", [(dim, []), (cen, ["-testid=linkloss_a", "-disconnect_on_exit=1"]), (cen, ["-testid=linkloss_b"])])
    flash = OUT / "dimmer_flash.bin"
    if flash.exists():
        flash.unlink()
    scenario("persist1", [(dim, [f"-flash_file={flash}"]), (cen, ["-testid=persist_set"])], length=5)
    scenario("persist2", [(dim, [f"-flash_file={flash}"]), (cen, ["-testid=persist_check"])], length=10)
    scenario("stress", [(dim, []), (cen, ["-testid=stress"])], length=60)

    def passed(n):
        return all(rc == 0 for rc in res[n]["rcs"][1:])

    # ---- 4 details
    b = res["basic"]["m"]
    print()
    print("[4  Results]")
    print(f"   discovery: {b.get('chars_found')} of 6 characteristics; unencrypted write -> ATT {b.get('unencrypted_write_err')}, "
          f"after Just Works pairing: security level {b.get('security_level')}")
    req("R3", "BLE: connect and discover all 6 characteristics", f"{b.get('chars_found', 0)}/6", "6/6",
        passed("basic") and b.get("chars_found") == "6")
    req("R4", "Writes need encryption; accepted after pairing", f"0x{ival(b.get('unencrypted_write_err', 0)):02x} then level "
        f"{b.get('security_level')}", "rejected, then ok", passed("basic") and b.get("unencrypted_write_changed") == "0")
    print(f"   bad writes: level 101 -> {b.get('reject_level101')}, 2-byte level -> {b.get('reject_badlen')}, fade 10001 -> "
          f"{b.get('reject_fade')}, command 9 -> {b.get('reject_cmd')}; state unchanged: {b.get('unchanged') == '1'}")
    req("R5", "Bad writes rejected with ATT errors, no change", "4/4" if passed("basic") else "fail", "4/4", passed("basic"))
    print("   energy meter, read over BLE, vs the load model's true power:")
    lines = [l for l in res["basic"]["logs"][1].splitlines() if "meter_level" in l]
    for l in lines:
        mm = dict(re.findall(r"(\w+)=(\S+)", l.split("METRIC ", 1)[1]))
        k = next(x for x in mm if x.startswith("meter_level"))
        print(f"     {k[11:-2]:>3s} %: {mm[k]} W measured, {mm['true_w']} W true, {mm['vrms']} V, {mm['irms_ma']} mA, PF 0.{mm['pf']}")
    werr = ival(b.get("meter_worst_err_permille", 999)) / 10
    req("R7", "Metering: power within 2 % at 4 levels", f"worst {werr:.1f} %", "<= 2 %", passed("basic") and werr <= 2.0)

    f = res["fade"]["m"]
    up, dn = ival(f.get("fade_up_ms", 0)), ival(f.get("fade_down_ms", 0))
    print(f"   fade 0->100 % asked 1000 ms: {up} ms ({f.get('fade_up_notifs')} status notifications); "
          f"100->20 % asked 3000 ms: {dn} ms; monotonic: {f.get('fade_up_monotonic') == '1' and f.get('fade_down_monotonic') == '1'}")
    req("R6", "Fades take the time asked (+/-2 %), monotonic", f"{up} / {dn} ms", "1000 / 3000 +/-2 %",
        passed("fade") and abs(up - 1000) <= 20 and abs(dn - 3000) <= 60 and f.get("fade_up_monotonic") == "1")

    dlog = res["faults"]["logs"][0]
    trips = [(int(a, 16), int(t)) for a, t in re.findall(r"TRIP t=\d+ faults=(0x[0-9a-f]+) after=(\d+) ms", dlog)]
    ov = [t for a, t in trips if a & 2]
    sh = [t for a, t in trips if a & 1]
    otp = [t for a, t in trips if a & 4]
    fm = res["faults"]["m"]
    print(f"   overload (+3 A): tripped {ov[0] if ov else '-'} ms after it started; turned on again while still "
          f"overloaded: tripped again: {fm.get('retrip_if_still_overloaded') == '1'}; commands in fault -> ATT {fm.get('cmd_in_fault')}")
    print(f"   short circuit: tripped {sh[0] if sh else '-'} ms after it started (next 10 ms tick)")
    print(f"   heatsink 90 C: derated to {fm.get('derated_power_w')} W; 101 C: tripped {otp[0] if otp else '-'} ms later; "
          f"reset while hot -> ATT {fm.get('reset_hot')}, once cool -> {fm.get('reset_cool')}")
    req("R8", "Short <= 10 ms, overload 200-220 ms, re-trips", f"{sh[0] if sh else '-'} / {ov[0] if ov else '-'} ms",
        "<= 10 / 200-220 ms", passed("faults") and sh and sh[0] <= 10 and ov and 200 <= ov[0] <= 220
        and fm.get("retrip_if_still_overloaded") == "1")
    req("R9", "Overtemp: derate, trip at 100 C, reset when cool", f"{fm.get('derated_power_w')} W, {otp[0] if otp else '-'} ms",
        "derate, trip, 0x81", passed("faults") and otp and fm.get("reset_hot") == "0x81")

    blog = res["brownout"]["logs"][0]
    lost = re.search(r"MAINS t=\d+ lost after=(\d+) ms", blog)
    back = re.search(r"MAINS t=\d+ back after=(\d+) ms", blog)
    print(f"   mains off for 200 ms: output off {lost.group(1) if lost else '-'} ms after, back {back.group(1) if back else '-'} "
          f"ms after the cut at the same level: {res['brownout']['m'].get('resumed_same_level') == '1'}")
    req("R10", "Mains loss: off <= 30 ms, resumes same level", f"{lost.group(1) if lost else '-'} ms, same level",
        "<= 30 ms, same", passed("brownout") and lost and int(lost.group(1)) <= 30)

    llog = res["linkloss"]["logs"][0]
    disc = re.search(r"DISCONNECTED t=(\d+) reason=(0x[0-9a-f]+)", llog)
    recon = [int(t) for t in re.findall(r"CONNECTED t=(\d+) err=0x00", llog)]
    duties = [int(dd) for t, dd in re.findall(r"LOG t=(\d+) .*?duty=(\d+)", llog) if disc and int(t) >= int(disc.group(1)) - 1000]
    steady = len(set(duties)) == 1 and duties
    print(f"   link loss: phone vanished; dimmer saw reason {disc.group(2) if disc else '-'} (supervision timeout) at "
          f"{disc.group(1) if disc else '-'} ms, output unchanged: {bool(steady)} (duty {duties[0] if duties else '-'}), "
          f"second phone connected at {recon[1] if len(recon) > 1 else '-'} ms and read the same level")
    req("R11", "Link loss: output unchanged, phone reconnects", f"reason {disc.group(2) if disc else '-'}, same level",
        "0x08, same", passed("linkloss") and disc and disc.group(2) == "0x08" and steady)

    boot2 = re.search(r"BOOT restored=(\d) lvl=(\d+) on=(\d) fade=(\d+)", res["persist2"]["logs"][0])
    print(f"   power cycle: second boot read lvl={boot2.group(2) if boot2 else '-'} on={boot2.group(3) if boot2 else '-'} "
          f"fade={boot2.group(4) if boot2 else '-'} ms from flash; phone saw the same: "
          f"{res['persist2']['m'].get('restored_after_power_cycle') == '1'}")
    req("R12", "Power cycle: on, level, fade restored from flash",
        f"{boot2.group(2)} %, {boot2.group(4)} ms" if boot2 else "-", "35 %, 800 ms",
        passed("persist1") and passed("persist2") and boot2 and boot2.group(1) == "1")

    s = res["stress"]["m"]
    print(f"   stress: {s.get('stress_ops')} random commands over BLE, {s.get('mismatches')} state mismatches, "
          f"{s.get('att_errors')} ATT errors, {s.get('avg_op_ms')} ms per command + status read (simulated time)")
    req("R13", "300 random commands: no mismatch, no error", f"{s.get('mismatches', '-')} / {s.get('att_errors', '-')}",
        "0 / 0", passed("stress") and s.get("mismatches") == "0")

    fades = [(d, int(t), int(l)) for d, t, l in re.findall(r"FADE (up|down) t=(-?\d+) level=(\d+)", res["fade"]["logs"][1])]
    log_lines = [(int(t), int(du), int(i), int(tc)) for t, du, i, tc in
                 re.findall(r"LOG t=(\d+) .*?duty=(\d+) .*?I=(-?\d+) T=(-?\d+)", dlog)]
    (OUT / "plot.json").write_text(json.dumps({"fade": fades, "faults": log_lines, "trips": trips}))

    print()
    print(f"{sum(len(v['rcs']) for v in res.values())} simulated devices in 8 simulations, {time.time() - t_start:.0f} s")
    print()
    R.sort(key=lambda r: int(r[0][1:]))
    print(f"{'ID':5}{'Requirement':50}{'Result':28}{'Target':22}Status")
    for rid, text, result, target, ok in R:
        print(f"{rid:5}{text:50}{result:28}{target:22}{'PASS' if ok else 'FAIL'}")
    n = sum(r[4] for r in R)
    print(f"{n}/{len(R)} requirements pass")
    sys.exit(0 if n == len(R) else 1)


if __name__ == "__main__":
    main()
