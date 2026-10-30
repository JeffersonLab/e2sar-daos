# How-to guides

Step-by-step procedures. Site-specific details are in [sites/](../sites/index.md).

* [Build e2sar-daos](build.md) - Prerequisites, pointing pkg-config and Meson at E2SAR and its Boost, configure/compile/test, rebuilds, the container build and the integration test.
* [Install E2SAR from a GitHub release](install-e2sar.md) - Install the latest E2SAR release with root, without root (testbed), or on an LCF (adding re2), verify it, and build e2sar-daos against it.
* [Testing e2sar-daos](run-tests.md) - The test scenarios (Mac, ebpf testbed, LB, Slurm TB, Aurora), the unit and integration suites, and which environment runs which.
* [End-to-end example (no control plane)](e2e-no-control-plane.md) - Send events with e2sar_perf straight to e2sar_daos_recv, check them with e2sar_daos_read, view restored EVIO files over an SSH tunnel, and clean up.
* [Walkthrough: --live-rm streaming of an EVIO file](streaming-walkthrough.md) - Stream the frames of an EVIO file node to node on the ebpf testbed, read them live with --live-rm, and check every frame against the file.
* [Testing through the ESnet EJFAT load balancer](lb-test.md) - Send files with e2sar_ft from ebpf2201 through the ESnet EJFAT LB to e2sar_daos_recv on ebpf2203, verify the restored files, and clean up.
* [End-to-end benchmark: EVIO frames → E2SAR → DAOS](e2e-bench.md) - Run scripts/e2sar_e2e_bench.sh: the tools, the events, the options, and the sender and receiver settings it defaults to.
