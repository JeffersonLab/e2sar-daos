# Site guides

One file per machine or testbed. A file whose `stale_after` has passed needs checking before use; `status: draft` means not run yet.

* [Mac without DAOS](mac-container.md) - Build and run the unit suite on a Mac in the Rocky 9 dev container under linux/amd64 emulation.
* [ebpf2201 / ebpf2203 testbed](ebpf-testbed.md) - The JLab ebpf nodes (pool sc26, native daos-devel): prerequisites, build and test suites, and the applications end to end over loopback and across the two nodes. (stale)
* [Slurm TB nodes (DAOS pool but no daos-devel)](slurm-tb.md) - Build and test on the JLab Slurm TB nodes with podman: image storage, the daos_agent socket, SELinux, InfiniBand and batch jobs.
* [LCF guide: one LCF sender, two receivers, through the ESnet LB into DAOS](perlmutter.md) - Send e2sar_perf events from a Perlmutter compute node through the ESnet LB to two e2sar_daos_recv receivers at JLab, verify in DAOS, and clean up. (stale)
* [ALCF Aurora (planned)](aurora-plan.md) - Why Aurora needs its own procedure (PBS, SLES, Slingshot, DAOS on compute nodes only), the test phases, and the open questions. Not yet run. (draft)
* [Testing workflow on ALCF Aurora](aurora-workflow.md) - The commands for one test cycle on Aurora, in order: one-time setup, build, then one PBS job per stage. Not yet run. (draft)
