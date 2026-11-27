# Share the Linux full-gate build

**Status** Proposed.

**Context** Each of the six Linux full-gate shards compiled the same Release
tree. Their compiler caches were restored from the package job, which usually
builds a smaller set of targets. In a [green PR run](https://github.com/exordos/workspace_native/actions/runs/46020243191),
each shard spent 8 to 10 minutes building before it started CTest.

**Options** Let all six shards build with a shared compiler cache, or compile
once and transfer the build tree within the workflow run. Compilation caching
still leaves each shard to configure and link every target.

**Decision** The Linux release workflow builds the full Release tree once and
saves a dedicated compiler cache. The six shards download that run's archive
and extract it at the same workspace path, where the generated CTest commands
expect it. `package-linux` builds its existing targets in parallel for its
installed-runtime checks.

**Consequences** A build, upload or download failure keeps `full-gate-linux`
red. Each shard runs its existing CTest slice on its own desktop. The manual
and tag fallback paths can still build and run the full gate independently.
Measure build, transfer, queue and test time in GitHub Actions before changing
the shard count. A rerun after the one-day artifact expiry must include the
build job.
