"""watches the PC so Manikin can speak up when something worth mentioning happens

poll() runs every ~10 s from a thread, takes one Sample, keeps 60 s of CPU/GPU for the averages
and returns the Observations that just became true. each kind is edge triggered, it fires once
and only rearms on a poll where its condition cleared (below the hysteresis level) AND its been
COOLDOWN secs since it fired. so if it comes back during the cooldown and is still true when the
cooldown ends it isnt reported, it has to clear first
"""

import logging
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable

import psutil

log = logging.getLogger(__name__)

WINDOW = 60.0              # secs of samples averaged for CPU and GPU
MIN_SPAN = 50.0            # dont trust an average until it covers this much (6 polls at 10 s)
COOLDOWN = 30 * 60.0       # min gap between two firings of the same kind

CPU_HIGH, CPU_CLEAR = 90.0, 75.0      # CPU and GPU are averaged over WINDOW
GPU_HIGH, GPU_CLEAR = 90.0, 75.0
RAM_HIGH, RAM_CLEAR = 90.0, 80.0      # instant reading, no average
BATTERY_LOW, BATTERY_LOW_CLEAR = 15.0, 20.0             # plugging in clears these too
BATTERY_CRITICAL, BATTERY_CRITICAL_CLEAR = 5.0, 10.0
NIGHT_HOURS = range(1, 5)             # 01:00 to 04:59 local time


@dataclass(frozen=True)
class Observation:
    kind: str    # "cpu_high" | "ram_high" | "gpu_high" | "battery_low" | "late_night"
    text: str    # short factual line for the event prompt


@dataclass(frozen=True)
class Sample:
    """one reading. None means not available here and that kind never fires"""
    cpu: float | None = None            # percent, all cores
    ram: float | None = None            # percent
    ram_used_gb: float | None = None
    ram_total_gb: float | None = None
    gpu: float | None = None            # utilisation %, the max across NVIDIA GPUs
    battery: float | None = None        # percent
    plugged: bool | None = None


class SystemSampler:
    """reads the real machine through psutil, and NVML if its there"""

    def __init__(self):
        psutil.cpu_percent(None)        # the first call only primes the counter
        self._nvml = None               # the pynvml module once initialised
        self._nvml_failed = False

    def __call__(self) -> Sample:
        cpu = ram = used = total = battery = plugged = None
        try:
            cpu = float(psutil.cpu_percent(None))
        except Exception as e:
            log.debug("cpu_percent failed: %s", e)
        try:
            vm = psutil.virtual_memory()
            ram, used, total = float(vm.percent), vm.used / 2**30, vm.total / 2**30
        except Exception as e:
            log.debug("virtual_memory failed: %s", e)
        try:
            bat = psutil.sensors_battery()       # None on desktops
            if bat is not None:
                battery = float(bat.percent)
                plugged = bat.power_plugged      # can be None too if unknown
        except Exception as e:
            log.debug("sensors_battery failed: %s", e)
        return Sample(cpu=cpu, ram=ram, ram_used_gb=used, ram_total_gb=total,
                      gpu=self._gpu(), battery=battery, plugged=plugged)

    def _gpu(self) -> float | None:
        nvml = self._init_nvml()
        if nvml is None:
            return None
        loads = []
        try:
            count = nvml.nvmlDeviceGetCount()
        except Exception:
            return None
        for i in range(count):
            try:
                h = nvml.nvmlDeviceGetHandleByIndex(i)
                loads.append(float(nvml.nvmlDeviceGetUtilizationRates(h).gpu))
            except Exception:
                # an idle laptop dGPU is powered down and says "Unknown Error" till something wakes it,
                # reinit would wake it (~0.5 s and battery) so report nothing, it answers once busy
                pass
        return max(loads) if loads else None

    def _init_nvml(self):
        # init once and keep it. NVML init is refcounted so the init/shutdown pair in
        # tools.system_info doesnt close ours. if it fails theres no NVIDIA driver, dont retry
        if self._nvml is not None or self._nvml_failed:
            return self._nvml
        try:
            import pynvml
            pynvml.nvmlInit()
            self._nvml = pynvml
        except Exception as e:
            log.info("no NVIDIA GPU info for the watcher: %s", e)
            self._nvml_failed = True
        return self._nvml


class _Trigger:
    """edge trigger with hysteresis and a cooldown"""

    def __init__(self):
        self.armed = True
        self.last_fired: float | None = None

    def update(self, now: float, high: bool, clear: bool) -> bool:
        """True when it fires. high and clear are both False when theres no data"""
        if not self.armed:
            if clear and (self.last_fired is None or now - self.last_fired >= COOLDOWN):
                self.armed = True
            return False
        if high:
            self.armed, self.last_fired = False, now
            return True
        return False

    def consume(self, now: float) -> None:
        """counts as fired without reporting (a more urgent one covered it)"""
        self.armed, self.last_fired = False, now


class Watcher:
    def __init__(self, sampler: Callable[[], Sample] | None = None, *,
                 localtime: Callable[[float], time.struct_time] = time.localtime):
        self._sampler = sampler if sampler is not None else SystemSampler()
        self._localtime = localtime
        self._cpu: deque[tuple[float, float]] = deque()
        self._gpu: deque[tuple[float, float]] = deque()
        self._triggers = {k: _Trigger() for k in
                          ("cpu_high", "ram_high", "gpu_high", "battery_low",
                           "battery_critical", "late_night")}

    def poll(self, now: float | None = None) -> list[Observation]:
        """takes a sample and returns whatever just became worth mentioning, never raises"""
        now = time.time() if now is None else now
        try:
            s = self._sampler()
        except Exception as e:
            log.warning("watcher sampler failed: %s", e)
            s = Sample()
        out: list[Observation] = []
        for check in (self._check_cpu, self._check_ram, self._check_gpu,
                      self._check_battery, self._check_night):
            try:
                obs = check(now, s)
            except Exception as e:      # a bad reading shouldnt kill the polling thread
                log.warning("watcher check failed: %s", e)
                obs = None
            if obs:
                out.append(obs)
        return out

    @staticmethod
    def _average(window: deque, now: float, value: float | None) -> tuple[float | None, bool]:
        """adds value to the window and drops stale entries. returns (the mean, or None till
        the window covers MIN_SPAN, whether the window is empty)"""
        if value is not None:
            window.append((now, value))
        # also drop entries from the "future" in case the clock jumped back
        kept = [(t, v) for t, v in window if now - WINDOW <= t <= now]
        window.clear()
        window.extend(kept)
        if not kept or kept[-1][0] - kept[0][0] < MIN_SPAN:
            return None, not kept
        return sum(v for _, v in kept) / len(kept), False

    # for CPU and GPU a whole minute with no readings counts as clear. an idle laptop dGPU
    # powers down and NVML cant read it so after a game gpu_high would never see a low reading
    # and get stuck. firing waits for a fresh reading so the text always has a "now" number

    def _check_cpu(self, now: float, s: Sample) -> Observation | None:
        avg, empty = self._average(self._cpu, now, s.cpu)
        high = s.cpu is not None and avg is not None and avg >= CPU_HIGH
        clear = empty or (avg is not None and avg < CPU_CLEAR)
        if self._triggers["cpu_high"].update(now, high, clear):
            return Observation("cpu_high", f"CPU has averaged {avg:.0f}% over the last minute "
                                           f"({s.cpu:.0f}% now)")
        return None

    def _check_gpu(self, now: float, s: Sample) -> Observation | None:
        avg, empty = self._average(self._gpu, now, s.gpu)
        high = s.gpu is not None and avg is not None and avg >= GPU_HIGH
        clear = empty or (avg is not None and avg < GPU_CLEAR)
        if self._triggers["gpu_high"].update(now, high, clear):
            return Observation("gpu_high", f"GPU utilisation has averaged {avg:.0f}% over the "
                                           f"last minute ({s.gpu:.0f}% now)")
        return None

    def _check_ram(self, now: float, s: Sample) -> Observation | None:
        high = s.ram is not None and s.ram >= RAM_HIGH
        clear = s.ram is not None and s.ram < RAM_CLEAR
        if not self._triggers["ram_high"].update(now, high, clear):
            return None
        text = f"RAM is {s.ram:.0f}% full"
        if s.ram_used_gb is not None and s.ram_total_gb is not None:
            text += f" ({s.ram_used_gb:.1f} GB of {s.ram_total_gb:.1f} GB used)"
        return Observation("ram_high", text)

    def _check_battery(self, now: float, s: Sample) -> Observation | None:
        if s.battery is None or s.plugged is None:
            return None            # no battery, or the OS cant tell if its charging
        on_battery = not s.plugged
        low, critical = self._triggers["battery_low"], self._triggers["battery_critical"]
        fired_critical = critical.update(
            now, on_battery and s.battery <= BATTERY_CRITICAL,
            s.plugged or s.battery > BATTERY_CRITICAL_CLEAR)
        if fired_critical:
            if low.armed:
                low.consume(now)   # one remark instead of "low" and "critical" in the same breath
            return Observation("battery_low", f"Battery is critical: {s.battery:.0f}% "
                                              "and not plugged in")
        if low.update(now, on_battery and s.battery <= BATTERY_LOW,
                      s.plugged or s.battery > BATTERY_LOW_CLEAR):
            return Observation("battery_low", f"Battery is low: {s.battery:.0f}% "
                                              "and not plugged in")
        return None

    def _check_night(self, now: float, _: Sample) -> Observation | None:
        t = self._localtime(now)
        night = t.tm_hour in NIGHT_HOURS
        if self._triggers["late_night"].update(now, night, not night):
            return Observation("late_night", f"It is {t.tm_hour:02d}:{t.tm_min:02d} local time, "
                                             "late at night")
        return None
