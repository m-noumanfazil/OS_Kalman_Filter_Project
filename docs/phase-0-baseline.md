# Phase 0: Baseline and evidence inventory

> **Later report update:** This is a dated Phase 0 inventory. The current consolidated report is now [report/final_report.md](../report/final_report.md); both reports identified as drafts below are historical.

## Baseline and report status

Baseline commit: `104402e805727e8b959a5d75180d7c061e95b047` (2026-11-15, “Final Report draft uploaded”). This commit preserves the source and tracked artifacts as they existed before this checkpoint. No existing measurements or outputs were regenerated for this inventory.

Working report candidate: [`report/Final_report_draft.md`](../report/Final_report_draft.md), the newer draft and the one that reflects the current saved filter timings and accuracy more closely. Its title is still “draft”; final submission authority has not been confirmed. [`report/report.md`](../report/report.md) is the older report (last changed at `f83bc09`), contains conflicting benchmark and accuracy values, and should be treated as historical until reconciled. [`presentation/slides.md`](../presentation/slides.md) also contains older figures and requires reconciliation. Do not treat any draft as verified merely because it exists.

## Configuration and provenance

- The README documents native Ubuntu 22.04 and Windows 10 on the same dual-boot HP ZBook; this is a project claim, not independently verified here. The report gives further environment details.
- [`common/sensor_data.csv`](../common/sensor_data.csv) is the 80,000-record input. [`common/generate_sensor_data.py`](../common/generate_sensor_data.py) uses seed 42. The current generator and file are tracked, but their historical generation run is not independently established.
- Filter sources specify 50 measured iterations, five warm-ups, and 1/2/4/8-worker configurations. Part A summaries record 1,000 iterations for most calls and 200 for process operations. CSV rows are aggregate statistics, not individual samples.
- Results are in [`results/Linux`](../results/Linux/) and [`results/Windows`](../results/Windows/); logs are in [`logs`](../logs/); category and filter transcripts accompany their sources. Charts are in [`results/charts`](../results/charts/). Git commit `b2388f4` is the latest commit touching the benchmark CSVs, not proof of their execution environment or that every row came from the same run.
- [`common/verify_part_b.py`](../common/verify_part_b.py) simulates partitioning and can overwrite both platform output CSVs. The two saved output files are identical; independent native origin is therefore unconfirmed. Existing platform logs contain successful four-worker position-parity entries, but this inventory does not independently rerun them.
- `git show 104402e:<path>` retrieves tracked baseline files without depending on later workspace changes. The original non-revised PDF and the locally ignored planning documents are not tracked in this baseline.

## Requirement-to-source-to-evidence matrix

Presence indicates reviewable evidence, not a successful independent test or full compliance. The assignment's generic Linux `exec()` is implemented using `execv()`.

| Requirement / interfaces | Source | Existing evidence and limitations |
|---|---|---|
| Linux file I/O: `open`, `read`, `write`, `close`; Windows: `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle` | [`Linux/File_IO`](../Linux/File_IO/), [`Windows/File_IO`](../Windows/File_IO/) | Category transcripts, platform logs, and file-I/O rows in both benchmark CSVs. Filter sources also use native file I/O. |
| Linux process: `fork`, `exec`, `wait`; Windows: `CreateProcess`, `WaitForSingleObject` | [`Linux/Process`](../Linux/Process/), [`Windows/Process`](../Windows/Process/) | Category transcripts and process benchmark rows. Interfaces are not all one-to-one equivalents. |
| Linux memory: `mmap`, `munmap`; Windows: `VirtualAlloc`, `VirtualFree` | [`Linux/Memory`](../Linux/Memory/), [`Windows/Memory`](../Windows/Memory/) | Category transcripts and allocation/free rows in both CSVs. Windows allocation and file mapping have different semantics. |
| Linux IPC: `pipe`, `shmget`, `shmat`; Windows: `CreatePipe`, `CreateFileMapping`, `MapViewOfFile` | [`Linux/IPC`](../Linux/IPC/), [`Windows/IPC`](../Windows/IPC/) | Category transcripts and IPC benchmark rows. Integrated filtering uses shared mappings, rather than every Part A IPC call. |
| Linux permissions: `chmod`, `chown`, `umask`; Windows: `SetFileSecurity` | [`Linux/Permissions`](../Linux/Permissions/), [`Windows/Permissions`](../Windows/Permissions/) | Category transcripts and permission benchmark rows. Linux expected-denial logging has an identified inverted success flag. |
| Filter computation and mapped input/output arrays | [`common/kalman_core.hpp`](../common/kalman_core.hpp), [`Linux filter`](../Linux/Kalman_Filter/Kalman_Filter_Linux.cpp), [`Windows filter`](../Windows/Kalman_Filter/Kalman_Filter_Windows.cpp) | Saved filtered CSVs, filter transcripts, and logs. The core uses separate X/Y constant-velocity filters. Output provenance caveat above applies. |
| Multi-process execution and IPC | Both platform filter sources | Shared mappings and track-partitioned workers are implemented. Logs record four-worker success; saved timings show slowdown at 2/4/8 workers, so the speedup objective is not demonstrated. |
| Errors, logging, and usability | [`common/logger_linux.hpp`](../common/logger_linux.hpp), [`common/logger.hpp`](../common/logger.hpp), category and filter sources; [`README.md`](../README.md) | Logs and instructions exist. Integrated filter paths do not consistently propagate worker, export, or cleanup failures. |
| Call performance, CPU, memory, and reproducibility | [`common/metrics_linux.hpp`](../common/metrics_linux.hpp), [`common/metrics.hpp`](../common/metrics.hpp), platform result CSVs | Repeated aggregate latency statistics and CPU/memory-related values exist. Parent-only resource scope for workers, cleanup overhead, timing differences, and missing per-iteration samples limit interpretation. |
| Report, presentation, and charts | [`report/Final_report_draft.md`](../report/Final_report_draft.md), [`presentation/slides.md`](../presentation/slides.md), [`common/generate_charts.py`](../common/generate_charts.py) | Artifacts exist. The older report and slides contain figures inconsistent with current CSVs; charts and claims need reconciliation. |

## Baseline filter measurements

From the committed CSVs, median microseconds for 1/2/4/8 workers are Linux **1925.985 / 5225.232 / 5353.943 / 7951.375** and Windows **1952.950 / 29948.450 / 33824.850 / 43462.550**. These are saved values, not measurements made during Phase 0. The saved sensor and filtered records produce approximately 70.805 m raw and 22.953 m filtered positional RMSE; matching output files do not prove independent native runs.

## Open requirements and decisions

1. **Windows count:** both rubrics require 15 calls but name only 12 Windows APIs. Instructor clarification is outstanding; supporting APIs must be distinguished from named requirements.
2. **Report authority:** the newer final draft is the working candidate; the team must confirm which report will be submitted.
3. **Performance:** the rubric asks for multi-process speedup, but the saved workload slows down. Persistent-worker improvements and new measurements belong to Phase 1; do not assert speedup before observing it.
4. **Provenance:** the original run configuration, individual timing samples, and independent origins of the two filtered outputs cannot be reconstructed from the tracked aggregate artifacts alone.
5. **Teacher guidance:** increased filter complexity was relayed by the user; the exact expected scope and grading weight beyond the written rubric remain unconfirmed.

## Phase 0 checkpoint

- [x] Identified a working report candidate and documented older/conflicting artifacts without claiming final approval.
- [x] Inventoried tracked baseline inputs, outputs, logs, configuration, and known provenance limits without replacing them.
- [x] Mapped requirements to implementation and existing evidence.
- [x] Recorded unresolved requirements and decisions.

Next: implement F01–F13 against the existing baseline as Phase 1, as directed by the local workflow. No Phase 1 changes are included in this checkpoint.
