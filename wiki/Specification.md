Each requirement the code implements has a label of the form `S-NN.NNN`, such as `S-02.084`. Code
comments and tests cite these labels (`/* S-02.084 */`), so `git grep S-02.084` finds the code and the
tests that check it. A spot where a requirement leaves a question open is marked `SPEC-Q:` in the code,
with the reading it takes.

The number before the dot names a document by topic:

| Label | Topic |
| --- | --- |
| S-01 | Boot chain and memory map |
| S-02 | SPI flash: slots, selector, writers, recovery, the start log, the boot store |
| S-03 | The eMMC: boot partitions, OS header, user area |
| S-04 | Reboot and metadata records |
| S-05 | The OS hand-off |
| S-06 | Serial framing |
| S-07 | The PSoC link |
| S-08 | The Nordic link |
| S-09 | The SPI bridge between the two processors |
| S-10a | USB device and mass storage |
| S-10b | MTP |
| S-10c | Host commands |
| S-11 | Firmware packages and how they install |
| S-12 | Filesystems |
| S-13a | Clocks and DDR |
| S-13b | Buses and pins |
| S-13c | The display |
| S-14 | Logging |
| S-15 | Menus: keys and actions |
| S-16 | Compatibility and design register |
| S-17 | Board revisions |

The specification itself is not part of the repository.

Where the code for each label lives: [Building and Testing](Building-and-Testing#source-map).
