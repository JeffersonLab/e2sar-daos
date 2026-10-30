# Reference

Options and behavior of the apps, scripts and build.

* [e2sar_daos_recv](e2sar_daos_recv.md) - The receiver: how writing works, every option, synthetic mode, the progress line and the Timing block.
* [Reading a run from DAOS: e2sar_daos_read](e2sar_daos_read.md) - The reader: --live, --live-rm and --offline, what each writes, what --live-rm removes, and every option.
* [The LB with lbadm: reservation, senders, workers](lbadm.md) - EJFAT LB tokens and URIs, inspecting the LB, reserving and freeing an instance, the sender allowlist and workers, with lbadm.
* [e2sar-daos scripts](scripts.md) - The bash and Python scripts that drive the apps on the JLab test nodes: the e2e bench, its rate sweep, the layout sweep and the site receiver.
* [Meson options](meson-options.md) - The project's Meson options (meson_options.txt): buildtest, daos_pool and daos_cont, with their defaults and how to change them.
