# Virtual Weather Station — design notes

## Why the driver exists

A real weather station is a handful of cheap parts on an I²C bus, several of
them measuring the same quantity. They drift, they spike, they wedge, they go
quiet. The interesting software is the part that turns that mess into one
number you would act on — and you cannot write or test that part without
hardware that misbehaves on demand.

`vws.ko` is that hardware. It emulates seven sensor chips, including their
register maps, their sampling clock and their failure modes, and exposes them
through the same interfaces a real driver would: a character device for the
data path, sysfs for configuration, procfs for statistics, ioctl for control.

## No floating point in the kernel

Kernel code runs with the user process's FPU state live in the registers.
Touching the FPU without `kernel_fpu_begin()`/`kernel_fpu_end()` corrupts that
state, those calls are expensive, and they are illegal in the atomic context
where the sampling timer runs. So the driver has no floating point at all:

* every value is a fixed-point integer in **milli-units** (23 456 m°C is
  23.456 °C): one part in a thousand of resolution, over a range of roughly
  ±2.1×10⁶ engineering units before an `s32` overflows — comfortably more than
  any of these sensors needs, light included at 10⁸ m-lux in full sun;
* the diurnal curve comes from a 256-entry integer sine table
  (`vws_sin_tbl`, values ±10 000) instead of `sin()`, indexed by a phase
  derived from `ktime_get()` with `div64_u64()`;
* that table is **interpolated**, with a 16-bit phase whose high byte indexes
  the table and whose low byte is the fraction between entries. The table
  alone resolves a cycle into 256 steps, and adjacent entries differ by up to
  245 — which scales to a 0.22 °C jump on temperature and a 2450 lux jump on
  light, both *larger than those sensors' own noise*. The model emitted a
  staircase with flat treads and risers that user space quite rightly read as
  discontinuities, which drove the light channel to FAULTY every dawn and
  accounted for temperature's residual 2.3 % reject rate. Interpolating gives
  65 536 steps per cycle and a 0.0099 °C step per sample — below both the
  noise and the ADC LSB, so the signal is smooth at sample resolution, which
  is what a physical quantity actually is;
* the phase is reduced modulo one day *before* being scaled up, because
  `elapsed_ns * 65536` overflows a `u64` after roughly a day of uptime while
  the remainder is bounded by `day_ns` and cannot;
* scaling is done as `(amplitude * sin_q(phase)) / 10000` — multiply before
  divide, so the intermediate keeps its precision;
* the one place a real division by a runtime value is needed
  (`elapsed_ns * 256 * harmonic / day_ns`) uses `div64_u64()`, because
  64-bit division is not a single instruction on 32-bit targets and the
  compiler would otherwise emit a call to a libgcc helper the kernel does not
  link.

User space converts to `double` exactly once, in `Units.h::toEngineering()`.
That is the whole fixed-point/floating-point boundary.

## Architecture-level details worth pointing at

| Concept | Where |
| --- | --- |
| Register map | `vws_sensor.regs[]`, `VWS_REG_*`, chip ID 0x60 like a BME280, data registers big-endian as most parts are |
| Sampling clock as an interrupt | `hrtimer` → `vws_timer_fn()` in softirq context, re-arming with `hrtimer_forward_now()` |
| ADC quantisation | the model rounds to one LSB of a notional 12-bit converter spanning the sensor's range |
| Signal-dependent noise | the light channel's noise amplitude scales with `int_sqrt(value)`, because photon shot noise grows as √I — a flat amplitude had the part reporting tens of lux at midnight |
| Autocorrelated model state | rainfall intensity is a bounded, mildly mean-reverting random walk in `vws_sensor.model_state`, not a fresh uniform draw per sample — the original model jumped between 40 and 130 mm/h, which is both a flood and white noise |
| Phase interpolation | the sine table is interpolated to 16-bit phase, because 256 steps per cycle produced risers larger than the sensors' noise |
| Saturation | values clamped to `range_min`/`range_max`, flagged `VWS_F_SATURATED` |
| FIFO with overflow | `vws_fifo`, power-of-two depth, masked indices, overwrite-on-full, `VWS_F_RESYNC` on the first sample the reader sees after a gap |
| Packed ABI struct | `struct vws_sample`, 16 bytes, explicitly sized members, `BUILD_BUG_ON` on both sides |
| Endianness | the register map stores big-endian like the real part; the ioctl ABI is native (x86 little-endian) and asserted by size |

## Locking

Two locks, because there are two kinds of contention:

* **`fifo_lock`** (spinlock, `irqsave`) guards everything the sampling timer
  touches: the FIFO, the per-sensor runtime state (`last_value`, `drift_accum`,
  `regs`), and `period`. It must be a spinlock because the timer callback runs
  in softirq context and cannot sleep.
* **`cfg_lock`** (mutex) serialises process-context configuration writers, so
  that a read-modify-write from sysfs cannot interleave with one from ioctl,
  and covers the open/release transitions that start and stop the timer.

A writer takes `cfg_lock`, then `fifo_lock` around the store itself — the
`vws_cfg_begin()`/`vws_cfg_end()` pair in `vws.h`. The one deliberate
exception is `vws_release()`, which drops `cfg_lock` before `hrtimer_cancel()`
because that call waits for a running callback to finish.

`vws_set_rate_locked()` does **not** restart the timer: the callback re-arms
itself from `vd->period`, so writing the new period under `fifo_lock` is
enough and the next expiry picks it up.

## The fusion pipeline

```
raw sample → range + median/MAD gate → health state machine
           → noise-variance estimate → fusion → derived metrics
```

**Outlier rejection** is median-based, not mean-based. The gate is a modified
z-score built on the *median absolute deviation*:

```
z = 0.6745 * (x - median) / MAD        reject when |z| > 3.5
```

The standard deviation is dragged around by the very outlier it is supposed to
catch — one 20 °C spike widens the gate enough to let the next one through.
MAD is not. The 0.6745 factor is the normal distribution's MAD, so the number
is comparable to an ordinary z-score on clean Gaussian data. A rejected
reading still enters the window, otherwise a genuine step change could never
drag the window across and the sensor could never recover.

Two corrections turned out to matter, both found by watching the thing run
rather than by reasoning about it:

**The window is detrended first.** A plain median gate quietly assumes the
true value is stationary across the window, and that is false for a channel
moving fast compared with its own noise. Daylight swings 0 → 100 klux over a
simulated morning — about 66 lux per sample against a noise sigma near 600,
so roughly 2000 lux of real movement per window. The median lags behind, every
fresh sample looks like an outlier, and **43 % of perfectly good light
readings were being thrown away**. The filter now fits a robust slope through
the window, extrapolates it to the incoming sample, and applies the MAD test
to the residual about that line. A flat channel gives slope 0 and behaves
exactly as before; the light channel's false-reject rate fell to under 1 %.

The slope is the rise between the medians of the window's two halves, over the
distance between their midpoints — the idea behind Tukey's resistant line.
Least squares was rejected because one spike levers it around, and spikes are
precisely what this window holds. The median of symmetrically-opposed pair
slopes was tried and rejected too: those pairs have baselines from 2 up to
n−1 samples, so the short-baseline estimates carry about (n/2)× the noise of
the long ones and drag the median off the truth — it read 31 against a true
66 per sample. One median per half gives every input the same long baseline.

**The gate is widened by `sqrt(lever)`**, where `lever = (n+1)/2` is the
extrapolation distance. The residual MAD is measured *inside* the window,
where the fitted line is anchored, but the test is applied one step beyond its
trailing edge — and for a channel that wanders rather than ramping (rainfall
intensity is a random walk) the prediction error grows as the square root of
that distance. Without the scaling the gate sat at about 1.4 standard
deviations of its own prediction error and discarded ~2.5 % of good rain.

The scale comes from **this window's own detrended MAD and nothing else**.
That restriction is the important part, and it was learned the hard way. An
earlier version floored the MAD using the fusion engine's variance estimate,
on the reasoning that the gate should never close tighter than the sensor's
known noise — which also fixed a real 2.3 %/2.5 % false-reject rate on
temperature and pressure. But it was a feedback loop and a layering inversion
both: the filter was being tuned by a statistic computed downstream of itself.

The failure mode is worth spelling out, because it is quiet. There is no gate
until the window holds `kMinForMad` samples, so a cold start accepts whatever
arrives. The engine's variance estimate is a *mean* of squared successive
differences — not robust — so the spikes accepted during that window poisoned
it. Measured on a sensor with 0.35 °C of noise taking 40 % spikes from cold:
the estimate read **18.07 °C**, the gate opened to **253 °C**, and all 600
samples including every 20.8 °C spike were accepted. Permanently, because
nothing was ever rejected, so nothing ever left the estimate. A median-based
scale cannot do this: MAD is unmoved by up to half the window being wrong.
Scaling by `sqrt(lever)` happens to absorb the MAD sampling variance that the
noise floor was introduced to fix, so nothing was lost by removing it.

The gate leaves ample margin for what it exists to catch — an injected spike
is a sixth of the sensor's full range, an order of magnitude or more outside
the resulting gate on every channel. Drift is deliberately not caught here;
see the fault table.

`MAD == 0` means the window is one repeated value. The gate is skipped in that
case, because it would otherwise reject every *correct* reading from a sensor
that is coming back to life. Detecting that condition is the health monitor's
job instead.

**Noise variance** is estimated from the mean square of successive
differences, `Var(noise) ≈ Var(Δx)/2`, rather than from the plain window
variance. On a signal riding a diurnal ramp the window variance is dominated
by the real change in temperature, which would make a perfectly good sensor
look noisy; first differences cancel the ramp.

**The health state machine** counts uninterrupted *runs*: one accepted
reading clears the rejection counter and vice versa, and a transition does not
clear either, so `confirmAfterAccepts` (25) is the total length of the clean
run needed to confirm a recovery rather than a further count on top of
`recoverAfterAccepts` (10).

Alongside those runs there is a **sustained-rate rule**: more than 25 % of the
last 64 outcomes rejected condemns the sensor, more than 10 % makes it
SUSPECT. The run-based rules have a blind spot without it. A sensor spiking
40 % of the time rejects about a third of its samples and is unambiguously
broken, but the rejections are interleaved — eight in a row has probability
0.4⁸, which comes up about 0.4 times in 600 samples — so it merely oscillated
between OK and SUSPECT and was never condemned. The fused value was protected
regardless (the gate threw the spikes out, and the inflated variance cut the
sensor's weight), but saying which unit needs replacing is half the point of a
health monitor. A healthy sensor in this bank rejects well under 1 %, so the
thresholds have a wide margin; the rule only ever escalates, leaving recovery
to the run-based rules.

Only FAULTY is excluded from fusion. SUSPECT keeps contributing — it means
"watch this one", and the variance estimate has already shrunk the weight of a
sensor that started misbehaving.

The stuck detector has one exemption worth stating, because it is the kind of
thing that only shows up at three in the morning: a sensor **pinned at an end
of its range** repeats its value for entirely honest reasons. Rainfall is
exactly zero most of the time, and light is exactly zero all night once the
clipped model is clamped at `range_min`. `FusionEngine::ingest()` therefore
tells the monitor when a reading is at a limit (or carries `VWS_F_SATURATED`),
and those repeats are not counted. Repeats only mean a wedged converter when
they happen in the middle of the range.

**Fusion** of the two redundant temperature units is available two ways
(`--fusion`, or `f` in the dashboard):

* `kalman` — one scalar Kalman filter, updated once per sensor per tick with
  that sensor's own variance. Its process variance (10⁻⁴ by default) says how
  much the true value may move between samples; set it too high and the
  posterior variance never drops below the measurement noise, which defeats
  the point of filtering at all. Sequential measurement updates mean the gain
  `K = P/(P+r)` is automatically smaller for the noisier unit. This is
  inverse-variance weighting arrived at recursively, and it also smooths.
* `weighted` — the direct maximum-likelihood combination,
  `x = Σ(xᵢ/varᵢ) / Σ(1/varᵢ)` with `var = 1/Σ(1/varᵢ)`. No memory, so it
  reacts instantly and is easier to reason about.

Fallback needs no special case. A sensor the health monitor has marked FAULTY
simply stops contributing measurements, so the estimate follows whatever is
left. With nothing left, the station reports `UNAVAILABLE` rather than
publishing a stale number as though it were current.

**Derived metrics**: Magnus-Tetens dew point, the NWS Rothfusz heat index
(including both edge corrections), and a least-squares pressure slope. Because
the driver compresses a day into `day_seconds` of wall time, the slope is
reported per *simulated* hour — that is the only scale on which the standard
±0.1 hPa/h tendency gate means anything.

## Backpressure

The sample ring **overwrites the oldest unread sample** when it fills. It
originally did the opposite — discarded the incoming sample — and the
asymmetry mattered far more than it looks.

Seven sensors at 20 Hz is 140 samples/s, so a 1024-deep ring spans 7.3
seconds. Once it filled, the driver went on serving seven-second-old samples
and threw away everything fresh, so the reader could not catch up until it had
drained the entire backlog, and every sample taken during that stretch was
lost. A transient overload became a permanent lag: the Kalman filter
integrating ancient measurements, the dashboard displaying the past while the
header claimed "live". Overwriting makes the overflow self-limiting instead —
the ring always holds the most recent `depth` samples, so the reader is back
at real time on its very next `read()`.

Measured with `vwsctl stall 3`, which holds the device open without reading
(7 sensors at 1000 Hz = 7000 samples/s into a 1024-sample ring, so it fills in
about 150 ms):

| | discard incoming | overwrite oldest |
| --- | --- | --- |
| age of first sample read, after a 3 s stall | 2.999 s | ~0.15 s (the ring's span) |
| `VWS_F_RESYNC` seen by the reader | 0 of 32 | every gap, once |

The second row was the other half of the bug. `VWS_F_RESYNC` can only be
attached to a sample that actually lands, and under the old policy nothing
landed while the ring was full — so the flag sat 1023 samples away and the
reader never saw it. The feature was dead code in practice. It is now written
onto the sample at the *new tail*, which is the first one the reader will see
after the gap; if a later overflow abandons that sample too, the flag goes
with it, so whatever the reader eventually reads carries exactly one flag per
gap.

`overflows` counts distinct episodes and `dropped` counts individual samples.
They used to be incremented together, which made them the same number and one
of them useless.

One consequence worth noting: the producer now writes `f->tail`, which used to
belong to the consumer alone. Both ends are already serialised by
`fifo_lock`, so this is safe, but it rules out turning the ring into a
lockless single-producer/single-consumer structure later without revisiting
the policy.

## Fault modes

| Mode | `param` | What it emulates | How it is caught |
| --- | --- | --- | --- |
| `stuck` | — | a wedged ADC returning the same code | identical-value run in the health monitor; nothing else can, the value is plausible every time |
| `drift` | m-units per sample | calibration drift | slow enough to pass the outlier gate — this is what redundancy alone cannot fix, and why disagreement between the two units matters |
| `spike` | % of samples | a bad connection or EMI | detrended median/MAD gate |
| `dropout` | % of samples suppressed | a part that stops answering | staleness: there is nothing to reject, only nothing to read |
| `noise` | m-unit amplitude | a failing reference or supply | reject rate climbs, and the variance estimate shrinks its fusion weight |

## Data structures

**Kernel**

| Type | Role |
| --- | --- |
| `struct vws_sample` | 16-byte packed ABI record: id, type, flags, `s32` milli-units, `u64` ns |
| `struct vws_sensor` | one emulated chip: model parameters, fault state, register array, embedded `kobject` |
| `struct vws_fifo` | power-of-two sample ring with head/tail and an overflow counter |
| `struct vws_device` | the whole device: cdev, sensor bank, FIFO, locks, wait queue, hrtimer, counters |

**User space**

| Type | Role |
| --- | --- |
| `RingBuffer<T>` | templated rolling window, the user-space counterpart to `vws_fifo` |
| `SampleQueue<T>` | bounded blocking queue, mutex + condition variable, drops oldest |
| `VwsDevice` | RAII fd owner; the only place `close()` is called |
| `IFilter` | interface for a processing stage; `MedianOutlierFilter` rejects, `KalmanFilter1D` smooths |
| `SensorHealthMonitor` | the OK/SUSPECT/FAULTY/RECOVERING state machine |
| `FusedSnapshot` | immutable published result, absent channels as `std::optional` |

## Interfaces

### ioctl (`include/vws_ioctl.h`)

| Request | Direction | Payload |
| --- | --- | --- |
| `VWS_IOC_GET_INFO` | R | `struct vws_info` |
| `VWS_IOC_GET_SENSOR` | RW | `struct vws_sensor_info` (`id` in, rest out) |
| `VWS_IOC_SET_RATE` | W | `__u32` Hz |
| `VWS_IOC_INJECT_FAULT` | W | `struct vws_fault_req` |
| `VWS_IOC_CLEAR_FAULT` | W | `__u8` sensor id, or `VWS_ALL_SENSORS` |
| `VWS_IOC_SET_ENABLE` | W | `struct vws_enable_req` |
| `VWS_IOC_FLUSH` | — | discards the FIFO |
| `VWS_IOC_GET_STATS` | R | `struct vws_stats` |

Commands with `_IOC_WRITE` set require the fd to have been opened for writing.

### sysfs

```
/sys/class/vws/vws/
├── sample_rate          rw   sampling clock, Hz
├── day_seconds          rw   wall-clock length of one simulated day
├── fifo_depth           ro
├── n_sensors            ro
└── sensorN/
    ├── name type unit chip_id range   ro
    ├── value                          ro   last conversion, milli-units
    ├── registers                      ro   raw register dump
    ├── enable                         rw
    ├── noise_amp                      rw
    ├── fault_mode                     rw   name ("drift") or number
    └── fault_param                    rw
```

The per-sensor directories are `kobject`s embedded in `struct vws_sensor`, so
a store callback recovers its sensor with `container_of()` and no lookup.

### procfs

`/proc/vws/stats` — counters, one `key: value` per line.
`/proc/vws/sensors` — the bank as a table.

## Coverage map

| Area | Where it shows up |
| --- | --- |
| Device drivers | `cdev`, `file_operations`, `read`/`poll`/`unlocked_ioctl`, ring buffer, wait queue, `hrtimer`, spinlock vs mutex, sysfs via class/device/kobject, procfs with `seq_file`, `module_param` |
| Linux system programming | `poll()`, blocking vs `O_NONBLOCK`, threads, `sigaction` without `SA_RESTART` for prompt shutdown, file I/O, Kbuild |
| C++17 | class hierarchy behind `IFilter`, templated `RingBuffer`/`SampleQueue`, RAII for the fd and for ncurses, `unique_ptr`, `optional`, `std::thread`, `condition_variable`, structured bindings |
| Computer architecture | register maps, timer as interrupt source, FIFO, fixed-point arithmetic, big-endian register layout, ADC quantisation and saturation |
| Hardware/software interface | a driver emulating real parts; sysfs as the configuration interface; ioctl as the control plane |

## Known limitations

* The ioctl ABI is native-endian, so a big-endian client would need
  byte-swapping; `compat_ptr_ioctl` handles 32-bit userspace on x86-64, where
  every structure in the ABI is already padded to the same layout in both.
* `vws_timer_fn()` samples every sensor on one tick, so the per-sensor
  `sample_rate` of a real bus (where each part converts at its own rate) is
  not modelled — one clock drives the whole bank.
* Only temperature is redundant, and the fusion step is written for that one
  channel: `FusionEngine::fuseTemperature()` iterates `tempIds_`, and
  `rebuildSnapshot()` assumes one sensor per other type. Adding a second
  pressure unit means adding it to the template table in `vws_model.c` *and*
  generalising those two functions to group sensors by type. The per-sensor
  half of the pipeline — filtering, health, variance — is already
  type-agnostic.
