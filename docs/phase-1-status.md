# Phase 1 verification status

This status records the work on `feature/kalman-improvements`. Phase 1 cannot be certified complete against every acceptance condition until the remaining evidence below is addressed.

| Fix | Current evidence | Remaining qualification |
|---|---|---|
| F01 | Both filter implementations reject failed worker startup, execution, and shutdown before recording that configuration. Fault-injection tests cover startup and shutdown failures. | Timeout and every individual OS failure path have not been independently fault-injected. |
| F02 | Output and benchmark file errors propagate failure; tests cover inaccessible output, sample, and summary paths. | If summary writing fails after raw samples are appended, a failed run can leave partial files; check the experiment manifest exit status before using them. |
| F03 | `common/verify_part_b.py` compares native CSVs to a Python reference without writing either platform output; tests cover missing/corrupted results. | CSV comparison alone cannot establish how a pre-existing output was produced. |
| F04 | Native input checks reject invalid IDs, non-finite values, repeated timestamps, malformed fields, trailing garbage, and more than 100,000 records; Windows worker arguments are checked. | Strict validation does not establish correctness for every possible malformed file. |
| F05 | Worker failure/shutdown checks, bounded waits, process reaping, and mapping cleanup are implemented and tested for selected failure cases. | A complete audit of every supporting system call and forced resource-cleanup failure remains outstanding. |
| F06 | Native 2/4/8-worker validation checks track, timestamp, position, and velocity against the sequential result at absolute tolerance `1e-9`; non-finite differences fail. | Python verifies exported values at three-decimal precision; it is a separate check. |
| F07 | Persistent benchmark records parent CPU and aggregate worker CPU in raw samples. Parent-only memory scope and shared-page limitations are documented in `persistent-benchmark.md`. | No individual worker memory or unique shared-memory measurement is provided; Windows CPU accounting may be coarse. |
| F08 | The presentation uses baseline CSV figures; historical reports are identified; charts are regenerated from baseline CSVs, including a median-based speedup chart. | A controlled Windows–Linux comparison for the current persistent design has not been made. The working report still contains historical analysis, not current measurements. |
| F09 | Persistent workers use Linux pipes or Windows events, with separate launch timing and 20k/40k/80k input workloads for the standard dataset. | Results must be interpreted for the measured environment. Launch timing excludes readiness and end-to-end processing. |
| F10 | Linux permission-denial logging now records the actual failed `open()` status and a separate successful verification. | Re-run the category demo on the target Linux environment for fresh transcript evidence. |
| F11 | `phase-0-baseline.md` maps all rubric interface categories to both source trees and saved evidence; README explains the 15-versus-12 ambiguity. | The instructor's interpretation of the Windows call count is still unconfirmed. |
| F12 | Per-iteration samples, group-level parent/worker CPU values, run identifiers, and an isolated experiment runner with hashes and environment manifest are available. | Hardware/power details and build command are supplied or noted by the operator. GitHub artifacts expire; preserve any results intended for submission. |
| F13 | Current filter core and working report use the same continuous white-noise acceleration covariance; the presentation now shows this equation. | The older `report/report.md` is marked historical and retains its original content. |

## Existing experimental evidence

- Historical Part A and Part B CSVs belong to the baseline per-iteration process-launch implementation, not the persistent-worker implementation. Their provenance limits are documented in `phase-0-baseline.md`.
- Linux GitHub Actions runs execute the native Linux code in a hosted Ubuntu environment. Artifacts are retained for 14 days and are not controlled performance comparisons with the local Windows machine.
- The local Windows experiment under `build/experiments/windows-phase1-a886b12` was run on Windows 11 with an AMD processor, using the binary and input hashes in its manifest. Its power configuration and RAM were not recorded. It is ignored by Git and is not automatically shared with collaborators.

## Still required for a fully supported cross-platform claim

Run the same defined workload on both systems under a documented, comparable setup; preserve the resulting manifests and raw samples; then update the report and charts with those results. The available GitHub-hosted Ubuntu VM and local Windows machine are different environments, so their measurements cannot establish a same-machine OS comparison. Instructor clarification of the Windows interface count is also outstanding.
