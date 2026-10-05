# Virtual Weather Station — Product Requirements Document

**Project** Virtual Weather Station Multi-Sensor Fusion Engine
**Author** Arpit Abhigyan Parida
**Status** Implemented and verified
**Document version** 1.0
**Last updated** 2026-10-05

---

## 1. Problem

A real weather station is a handful of cheap parts on an I²C bus, several of
them measuring the same quantity. They are individually noisy, they drift,
they spike, they wedge, and they go silent. Producing *one* reading a person
would act on means filtering the noise, deciding which sensors to believe, and
combining what is left.

That logic is the interesting part of a weather station, and it is the part
that cannot be written or tested without hardware that misbehaves on demand.
Buying seven sensors does not help much either: you cannot ask a real BME280
to wedge its ADC on cue, and waiting for a genuine fault is not a test plan.

**This project supplies the hardware.** A Linux character driver emulates a bank of
sensor chips — register maps, sampling clock, failure modes — and a C++17
engine in user space consumes it, filters it, judges each sensor's health, and
fuses the survivors into a single trustworthy snapshot.

## 2. Goals

| | |
| --- | --- |
| **G1** | Emulate a plausible multi-sensor weather station entirely in software, with no hardware dependency. |
| **G2** | Make every interesting failure mode injectable on demand, at runtime, per sensor. |
| **G3** | Produce one fused reading that stays correct and honest while sensors fail underneath it. |
| **G4** | Exercise the real Linux device-driver interfaces — character device, ioctl, sysfs, procfs, poll — rather than simulating them. |
| **G5** | Keep every mechanism explainable. A reviewer should be able to ask "why that number?" about any value on screen and get an answer from the code. |

### Non-goals

* Not a forecasting system. The "tendency" output is the standard barometric
  rule of thumb, not a weather model.
* Not a driver for real hardware. The emulation deliberately stops at the
  register map; there is no bus layer.
* Not a general sensor framework. It does not use the kernel's IIO subsystem,
  because the point is to build the character-device, sysfs and FIFO
  machinery by hand rather than inherit it.

## 3. Users

| User | Needs |
| --- | --- |
| **Developer of fusion logic** | A sensor stream that can be made to fail predictably and repeatably, so filtering and health logic can be developed and regression-tested without hardware. |
| **Operator of the station** | A live view of what every sensor is doing, which ones are trusted, what the fused reading is, and *why* a sensor was excluded. |
| **Reviewer / assessor** | Evidence that each claimed mechanism works, traceable from a requirement to the code that implements it to the test that proves it. |

## 4. Scope

### In scope

Kernel module (C), user-space engine and dashboard (C++17), a control CLI, an
offline test suite, an end-to-end test suite, and the documentation set.

### Out of scope

Real hardware or bus drivers; networking or remote access; more than one
device instance; big-endian or 32-bit-only hosts (the ioctl ABI is native
byte order, with `compat_ptr_ioctl` covering 32-bit user space on x86-64);
persistence beyond CSV files.

---

## 5. Functional requirements

Each requirement has an ID used by the traceability matrix in §8.

### 5.1 Sensor emulation (kernel)

| ID | Requirement |
| --- | --- |
| **FR-1** | Emulate a bank of ≥6 sensors covering temperature, humidity, pressure, wind, rainfall and illuminance, with **at least two redundant temperature units** so that fusion has something real to do. |
| **FR-2** | Each sensor exposes an emulated **register map** — chip ID, status, control, and big-endian data registers — readable from user space, so the emulation is of a device and not of a number generator. |
| **FR-3** | Sensor values follow a **physically plausible model**: a diurnal curve, per-sensor noise, per-sensor calibration bias, ADC quantisation to a 12-bit grid, and saturation at the sensor's stated range. |
| **FR-4** | A **sampling clock** drives conversion for all enabled sensors, standing in for an ADC end-of-conversion interrupt. Its rate is configurable at runtime, 1–1000 Hz. |
| **FR-5** | The simulated day is compressed to a configurable wall-clock duration so a full diurnal cycle is observable in minutes. |

### 5.2 Fault injection (kernel)

| ID | Requirement |
| --- | --- |
| **FR-6** | Support five injectable fault modes per sensor: **stuck** value, **drift**, **spike**, **dropout**, and excess **noise**. |
| **FR-7** | Faults are injectable and clearable at runtime, per sensor or across the whole bank, without reloading the module or restarting the consumer. |
| **FR-8** | A **dropout** suppresses the sample entirely rather than emitting a sentinel value, so user space sees absence, not a bad reading. |

### 5.3 Kernel interfaces

| ID | Requirement |
| --- | --- |
| **FR-9** | A character device delivers samples as fixed-size packed records. Reads return **whole records only**; a short read is never a partial record. |
| **FR-10** | `poll()` is supported, and `read()` honours `O_NONBLOCK`. A blocked reader is woken when samples become available and is interruptible by a signal. |
| **FR-11** | A **FIFO** buffers samples between the sampling clock and the reader, with a defined, documented overflow policy and counters for what was lost. |
| **FR-12** | Overflow **overwrites the oldest** unread sample, so a starved reader resumes at real time rather than draining a stale backlog. The first sample a reader sees after a gap is flagged. |
| **FR-13** | An **ioctl** control plane provides: device info, per-sensor info, set sample rate, inject fault, clear fault, enable/disable sensor, flush, and statistics. |
| **FR-14** | **sysfs** exposes device and per-sensor configuration as the hardware configuration interface, including the raw register dump. |
| **FR-15** | **procfs** exposes human-readable statistics and a sensor table. |
| **FR-16** | The sampling clock runs only while the device is open, so an idle system does no work. |

### 5.4 Fusion engine (user space)

| ID | Requirement |
| --- | --- |
| **FR-17** | Reject readings outside the sensor's physical range, and readings too far from the rolling trend of that sensor's own recent history. |
| **FR-18** | The outlier test must not penalise a channel that is legitimately **changing fast** relative to its noise. |
| **FR-19** | Track each sensor's health through a state machine: **OK → SUSPECT → FAULTY → RECOVERING → OK**, driven by rejection runs, stuck-value detection, silence, and sustained rejection rate. |
| **FR-20** | Fuse the redundant temperature sensors by **inverse-variance weighting**, available both as a recursive estimator (1-D Kalman) and as a direct weighted combination, selectable at runtime. A noisier sensor must automatically count for less. |
| **FR-21** | **Degrade gracefully.** A sensor the health monitor condemns stops contributing; the estimate continues on the survivors. With no usable sensor the system reports unavailability rather than publishing a stale value. |
| **FR-22** | Compute derived metrics: **dew point**, **heat index**, and a least-squares **pressure tendency** with a rising/steady/falling classification. |
| **FR-23** | Estimate each sensor's noise from its own accepted history, for use as the fusion weight. |

### 5.5 Presentation and control (user space)

| ID | Requirement |
| --- | --- |
| **FR-24** | A terminal **dashboard** shows every sensor's raw and accepted value, health, fusion weight, noise estimate and rejection rate; the fused values; the derived metrics; kernel and user-space counters; and a short history of the fused temperature. |
| **FR-25** | The dashboard states **why** a sensor is in its current state, not merely that it is. |
| **FR-26** | Faults, sensor enable/disable, fusion mode, sample rate and logging are controllable from the dashboard without leaving it. |
| **FR-27** | A **CLI** (`vwsctl`) performs every control operation from a shell, resolving sensors by name or id. |
| **FR-28** | **CSV logging** of the fused series and the per-sensor series, with absent channels left empty so a dropout is distinguishable from a reading of zero. |
| **FR-29** | A **headless mode** produces the same information as plain text, for use without a terminal. |

---

## 6. Non-functional requirements

| ID | Requirement | Rationale |
| --- | --- | --- |
| **NFR-1** | **No floating point anywhere in kernel code.** All kernel-side quantities are fixed-point integers in milli-units. | Kernel code runs with the user process's FPU state live; using it without `kernel_fpu_begin()` corrupts that state, and those calls are illegal in the atomic context where the sampling clock runs. |
| **NFR-2** | No sleeping, no allocation and no `copy_to_user` in the timer callback. | It runs in softirq context. |
| **NFR-3** | Documented lock discipline, with the spinlock covering everything the timer touches and the mutex serialising process-context writers. | The two contexts cannot share one lock type. |
| **NFR-4** | The driver must sustain the maximum sample rate (1000 Hz × all sensors = 7000 samples/s) without loss to a reader that keeps up. | Headroom demonstrates the data path is not the bottleneck. |
| **NFR-5** | Clean build: no warnings under `-Wall -Wextra` (kernel) and `-Wall -Wextra -Wpedantic -Wshadow` (C++17). | |
| **NFR-6** | No kernel `BUG`, `WARNING`, or lock-debugging complaint during any test run. | |
| **NFR-7** | Shutdown on `SIGINT`/`SIGTERM` is prompt and ordered; the terminal is always restored. | `poll()` timeouts bound the shutdown latency; ncurses teardown is RAII. |
| **NFR-8** | A single shared header defines the kernel↔user ABI, with the record size asserted at compile time on both sides and a version checked at open. | A module/binary mismatch must fail loudly, not mis-parse samples. |
| **NFR-9** | Every fusion mechanism must be verifiable **without** the kernel module, so the logic is testable on any machine. | |

---

## 7. Acceptance criteria

The system is accepted when all of the following hold. Each maps to automated
checks in `make unit` (offline) and `make test` (end to end).

| ID | Criterion | Verified by |
| --- | --- | --- |
| **AC-1** | Module builds against the running kernel and loads with no warnings; `/dev/vws`, `/sys/class/vws/vws/` and `/proc/vws/` all appear. | `make test` §1, §3, §4 |
| **AC-2** | ABI version and record size agree between module and binaries. | `make test` §2 |
| **AC-3** | All eight ioctl commands succeed and return sane data. | `make test` §2 |
| **AC-4** | `poll()` + `read()` deliver whole samples from every sensor. | `make test` §5 |
| **AC-5** | With all sensors healthy, both temperature units contribute and derived metrics are produced. | `make test` §6 |
| **AC-6** | A stuck sensor reaches FAULTY, the reason is reported, and fusion continues on the survivor. | `make test` §8 |
| **AC-7** | Injected spikes are rejected by the outlier filter. | `make test` §8, `make unit` |
| **AC-8** | A dropout is detected through silence rather than through bad values. | `make test` §8 |
| **AC-9** | A starved reader overflows the ring; the backlog stays bounded by the ring's span, not by the stall duration; the gap is flagged where the reader sees it. | `make test` §9 |
| **AC-10** | No `BUG`/`WARNING` from the module in the kernel log after a full run. | `make test` §10 |
| **AC-11** | A healthy sensor's rejection rate is ≈0; a sensor rejecting a sustained third of its samples is condemned. | `make unit` |
| **AC-12** | With every temperature sensor condemned, the system reports unavailability and withholds dependent derived metrics. | `make unit` |
| **AC-13** | Both fusion estimators work end to end. In inverse-variance mode the weights are a partition of unity, the quieter unit carries the larger weight, and the fused value satisfies `(f−a)/(b−a) = w_b` exactly. | `make test` §7 |

---

## 8. Traceability

| Requirement | Implemented in | Verified by |
| --- | --- | --- |
| FR-1, FR-3 | `kernel/vws_model.c` (`vws_sensor_bank_init`, `vws_base_model`) | `make test` §2; screenshot 03 |
| FR-2 | `kernel/vws.h` (`enum vws_reg`), `vws_model.c` (`vws_regs_store`), `vws_sysfs.c` (`registers`) | `make test` §3; screenshot 04 |
| FR-4, FR-16 | `kernel/vws_main.c` (`vws_timer_fn`, `vws_open`, `vws_release`) | `make test` §3, §5 |
| FR-5 | `kernel/vws_model.c` (`vws_day_phase`) | `make test` §2 |
| FR-6, FR-7, FR-8 | `kernel/vws_model.c` (`vws_sensor_convert`), `vws_main.c` (`vws_ioctl_inject`) | `make test` §8 |
| FR-9, FR-10 | `kernel/vws_main.c` (`vws_read`, `vws_poll`) | `make test` §5 |
| FR-11, FR-12 | `kernel/vws_fifo.c` | `make test` §9; screenshot 05 |
| FR-13 | `include/vws_ioctl.h`, `kernel/vws_main.c` (`vws_ioctl`) | `make test` §2 |
| FR-14 | `kernel/vws_sysfs.c` | `make test` §3 |
| FR-15 | `kernel/vws_proc.c` | `make test` §4 |
| FR-17, FR-18 | `user/Filters.cpp` (`MedianOutlierFilter`, `robustSlope`) | `make unit` (filter suite) |
| FR-19 | `user/SensorHealth.cpp` | `make unit` (health suite); `make test` §8 |
| FR-20, FR-23 | `user/Filters.cpp` (`KalmanFilter1D`), `user/FusionEngine.cpp` (`fuseTemperature`, `noiseVariance`) | `make unit` (fusion suite); `make test` §7 |
| FR-21 | `user/FusionEngine.cpp` (`fuseTemperature`, `rebuildSnapshot`) | `make unit`; `make test` §7, §8; screenshot 02 |
| FR-22 | `user/DerivedMetrics.cpp` | `make unit` (metrics suite) |
| FR-24, FR-25, FR-26 | `user/Dashboard.cpp` | screenshots 01, 02 |
| FR-27 | `user/Cli.cpp` | `make test` §2, §8, §9 |
| FR-28 | `user/CsvLogger.cpp` | `make test` §6 |
| FR-29 | `user/main.cpp` (`printHeadless`) | `make test` §6, §7, §8 |
| NFR-1 | `kernel/vws_model.c` (integer sine table, `div64_u64`) | compile-time: no float in kernel objects |
| NFR-3 | `kernel/vws.h` (`vws_cfg_begin`/`vws_cfg_end`), `vws_main.c` header comment | `make test` §10 |
| NFR-4 | — | `make test` §9 (7000 samples/s sustained) |
| NFR-5 | `kernel/Makefile`, `user/Makefile` | build output |
| NFR-6 | — | `make test` §10 |
| NFR-8 | `include/vws_ioctl.h`, `VwsDevice::VwsDevice` | `make test` §2 |
| NFR-9 | `user/tests.cpp` | `make unit` |

---

## 9. Known limitations

These are deliberate and documented rather than defects.

1. **Fusion is implemented for temperature only.** The per-sensor half of the
   pipeline — filtering, health, variance estimation — is type-agnostic, but
   `FusionEngine::fuseTemperature()` iterates the temperature ids and
   `rebuildSnapshot()` assumes one sensor per other channel. Adding a second
   pressure unit means generalising those two functions to group by type.
2. **One sampling clock drives the whole bank.** A real bus has each part
   converting at its own rate.
3. **Drift is deliberately not caught by the outlier filter.** It is slow
   enough to pass any gate that does not also reject genuine weather. This is
   the fault that redundancy alone cannot fix, and the system is honest about
   it: the two temperature units' disagreement is visible on the dashboard,
   but neither is condemned.
4. **The ioctl ABI is native byte order.** A big-endian client would need
   byte swapping. `compat_ptr_ioctl` covers 32-bit user space on x86-64,
   where every structure in the ABI already has identical layout in both.
5. **The module is unsigned**, so Secure Boot must be off or the module
   signed.

## 10. Risks and assumptions

| | Risk | Mitigation |
| --- | --- | --- |
| R1 | Kernel API churn breaks the build on a different kernel version. | `hrtimer_setup` vs `hrtimer_init` is already version-guarded; the module builds against the running kernel's headers. |
| R2 | A reviewer cannot load kernel modules on their machine. | The entire fusion engine is verifiable with `make unit`, which needs neither root nor the module. |
| R3 | Emulated behaviour diverges from plausible physics and discredits the result. | Each model is stated in the design document with its units and reasoning, and the acceptance criteria in §7 are checked against measured output rather than against the implementation. |
