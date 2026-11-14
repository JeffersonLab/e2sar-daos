# How-to guides

Step-by-step procedures. Site-specific details are in [sites/](../sites/index.md).

* [Build e2sar-daos](build.md) - Prerequisites, pointing pkg-config and Meson at E2SAR and its Boost, configure/compile/test, rebuilds, the container build and the integration test.
* [Install E2SAR from a GitHub release](install-e2sar.md) - Install the latest E2SAR release with root, without root (testbed), or on an LCF (adding re2), verify it, and build e2sar-daos against it.
* [Testing e2sar-daos](run-tests.md) - The test scenarios (Mac, ebpf testbed, LB, Slurm TB, Aurora), the unit and integration suites, and which environment runs which.
* [End-to-end example (no control plane)](e2e-no-control-plane.md) - Send events with e2sar_perf straight to e2sar_daos_recv, check them with e2sar_daos_read, view restored EVIO files over an SSH tunnel, and clean up.
* [Walkthrough: a live run on two ebpf nodes, from LB to EVIO file](streaming-walkthrough.md) - Every step of one run on the ebpf testbed: a valid LB instance URI, NIC monitors, receivers on ebpf2201 and ebpf2203, the --live-rm reader, DAOS monitor and EVIO live viewer on ebpf2203, the pages forwarded to a laptop, an EVIO file sent through the LB, stopping, checking every frame, and one bundle turned into an EVIO file for someone else.
* [Live monitoring of a run from a laptop (SC26 booth)](live-monitoring.md) - Run the DAOS monitor and the EVIO live viewer next to e2sar_daos_read, forward both pages through one SSH connection, and rehearse with the demo modes.
* [Keep a live run up unattended: receiver watchdog and hourly rotation](unattended-live-run.md) - Run the live pipeline on one ebpf node with nobody watching: a watchdog restarts a crashed receiver (and the pages), and an hourly job starts a fresh run and deletes runs that got no events. Why it runs on the node, the guards that keep it from fighting the pipeline, the tests, and the two scripts.
* [Replay a run in the DAOS monitor and the EVIO live viewer](replay-run.md) - After a run, play it again in both pages without DAOS, receivers or a sender: the DAOS monitor from its JSON lines file, the EVIO viewer from the reader's --live files or from one EVIO file with live_replay.py.
* [Testing through the ESnet EJFAT load balancer](lb-test.md) - Send files with e2sar_ft from ebpf2201 through the ESnet EJFAT LB to e2sar_daos_recv on ebpf2203, verify the restored files, and clean up.
* [End-to-end benchmark: EVIO frames → E2SAR → DAOS](e2e-bench.md) - Run scripts/e2sar_e2e_bench.sh: the tools, the events, the options, and the sender and receiver settings it defaults to.
