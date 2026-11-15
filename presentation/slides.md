# CT-353 Operating Systems — CCA Presentation

## Slide 1: Project

**Cross-platform system calls and multi-process Kalman filtering**

Department of Computer Science & Information Technology, NED University

Team: Muhammad Nouman Fazil, Muhammad Usman Murtaze, Muneeb ur Rehman

---

## Slide 2: Objectives

- Demonstrate native Linux and Windows interfaces for file I/O, processes, memory, IPC, and permissions.
- Integrate native operations into a sensor-data Kalman Filter.
- Compare OS behavior and evaluate performance using documented workloads.

---

## Slide 3: System architecture

```text
Sensor CSV → native file I/O → shared mapped arrays
                                   ↓
                        workers process independent tracks
                                   ↓
                         native output CSV + validation
```

Linux uses `mmap` and `fork`; Windows uses file mappings and `CreateProcess`. Persistent workers communicate through pipes on Linux and events on Windows. Each benchmark trial resets the filter states.

---

## Slide 4: Interface coverage

| Category | Linux | Windows |
|---|---|---|
| File I/O | `open`, `read`, `write`, `close` | `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle` |
| Processes | `fork`, `execv`, `wait` | `CreateProcess`, `WaitForSingleObject` |
| Memory | `mmap`, `munmap` | `VirtualAlloc`, `VirtualFree` |
| IPC | `pipe`, `shmget`, `shmat` | `CreatePipe`, `CreateFileMapping`, `MapViewOfFile` |
| Permissions | `chmod`, `chown`, `umask` | `SetFileSecurity` |

The rubric lists 15 Linux interfaces and 12 Windows APIs. The interpretation of the 15-call requirement for Windows remains to be confirmed.

---

## Slide 5: Historical system-call measurements

Medians from the saved baseline result CSVs, in microseconds:

| Workload | Linux | Windows |
|---|---:|---:|
| Open / CreateFile | 2.797 | 29.650 |
| Write 1 MB | 200.424 | 61.950 |
| Full process spawn + wait | 2,248.864 | 10,670.000 |

The operations and OS semantics differ; these timings characterize the recorded workloads rather than all workloads or machines. Source: `results/Linux/linux_results.csv` and `results/Windows/windows_results.csv`.

---

## Slide 6: OS differences and measurement scope

- Linux `fork` duplicates an existing process; Windows `CreateProcess` starts a new executable. They are not equivalent single operations.
- `chmod` and Windows security descriptors express different permission models.
- The reported process CPU and memory figures describe the measured process and scope defined by the benchmark, not automatically the entire process group.
- Linux TSC ticks and Windows process cycle counts have different semantics and are not directly comparable.

---

## Slide 7: Tracking scenario

- Synthetic, seeded data: **80,000 observations across eight independent tracks**.
- Each observation includes a timestamp, track ID, ground truth, and noisy X/Y position.
- A separate filter is maintained per track; chronological order within each track is preserved.
- Ground truth is used to evaluate error; it is not used as a filter measurement.

---

## Slide 8: Kalman Filter model

For each X or Y axis, the state is position and velocity. The transition is constant velocity with elapsed time `dt`. The implementation uses continuous white-noise acceleration with covariance:

```text
Q = q × [ dt³/3   dt²/2 ]
        [ dt²/2   dt    ]
```

`q = 1500` and measurement variance `r = 2500` in the existing model. See `common/kalman_core.hpp`.

---

## Slide 9: Saved filter accuracy

For the committed synthetic input and saved output:

| Metric | Value |
|---|---:|
| Raw 2D position RMSE | 70.81 m |
| Filtered 2D position RMSE | 22.95 m |
| Reduction | 67.58% |

The saved platform outputs match the independent Python reference at the precision exported. Their historical provenance is not independently established because the older verifier could overwrite both files.

---

## Slide 10: Historical and current worker designs

The **historical** saved benchmark created new workers for each iteration. Median times for 1 and 8 workers were:

| Platform | 1 worker | 8 workers |
|---|---:|---:|
| Linux | 1.926 ms | 7.951 ms |
| Windows | 1.953 ms | 43.463 ms |

Those runs show slowdown, not speedup. The current implementation reuses workers and collects 20k/40k/80k-record experiments; controlled cross-platform results for that version still need to be collected. Do not compare historical and persistent-worker runs as if they used the same protocol.

---

## Slide 11: Verification and reproducibility

- Native sequential and persistent-worker results are checked for position and velocity agreement at 2, 4, and 8 workers.
- GitHub Actions builds and exercises the Linux filter in an isolated directory; Windows can be tested in the same way locally.
- Fresh persistent-worker experiments write individual timings separately from historical benchmark CSVs.
- Record environment and dataset identity; compare parent/worker CPU and memory only with their scopes and sampling limitations stated.

---

## Slide 12: Conclusions and next steps

- Both platforms demonstrate OS interfaces and a shared 2D tracking model.
- The saved baseline reduces tracking error on the synthetic dataset; historical worker creation overhead exceeds the savings for the tested workload.
- Persistent workers and raw samples enable a new, controlled scaling experiment. Report observed results without assuming speedup.
- Future work: a validated 3D Extended Kalman Filter and, optionally, multiple motion models.
