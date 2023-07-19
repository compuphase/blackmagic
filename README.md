# Black Magic Probe - Roll-back

**Nota Bene** This is a fork of the original [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic). It takes a parallel route to the original project, with a focus on stability and reliability. For documentation on the Black Magic Probe, please see the original project, or read my free e-book [Embedded Debugging with the Black Magic Probe](https://github.com/compuphase/Black-Magic-Probe-Book).

## Status
At this point in development, the project should be considered in **beta stage**. There are no *known* bugs, but a number of functions still have to be verified.

This branch is based on release 1.7.1 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic) Since then, functionality and microcontroller support are being copied and/or adapted from the mainline release to this fork.

At this moment, this *Roll-back* fork is on a par with official release 1.8.2, meaning that it runs on BMPv2.3 (hardware release 6) as well as BMPv2.1; and it includes the "fake thread" support needed for GDB 11 and later (see [PR #1125](https://github.com/blackmagic-debug/blackmagic/pull/1125) of the official project).

In addition to the above, this fork features:
* More detailed and more extensive support for microcontrollers in the LPC family (by NXP), such as support for the LPC11Exx series and the LPC18xx series. This release also reports *accurate* sizes for Flash and SRAM, for the LPC family.
* Correct scheduling of driving/polling `SWDIO` relative to `SWCLK` and in regard to set-up and hold times. (This is my own implementation, not based on mainline [PR #1220](https://github.com/blackmagic-debug/blackmagic/pull/1220) which is *still* not entirely correct.)
* Calibration of the ADC through the internal reference voltage ("bandgap" reference). As a result of this: voltage drops due to over-current (due to the target drawing too much current from the probe) and *power backfeeding* through `tpwr` (when `tpwr` is enabled and the target itself is also self-powered), are detected and protected against. (This is also my own implementation, not based on mainline [PR #1434](https://github.com/blackmagic-debug/blackmagic/pull/1434) which has a concurrency problem and fails to detect over-current.)

**Nota Bene**: This fork focuses on the *native* platform (original [Black Magic Probe](https://black-magic.org/)), the "*jeff*" platform ([Jeff Probe](https://flirc.tv/products/flirc-jeffprobe), a low-cost derivative), and the "*hosted*" platform (desktop application). Other platforms compile, but I lack the hardware to test these.

## Why does this fork exist?
The official release 1.7 has always run reliably in our production. *All* official releases after 1.7, however, are either unstable (releases 1.8.x) or broken with regard to LPC microcontrollers. Shortly after the release of 1.7.1, project maintenance moved to a new team (at 1BitSquared), and I am not comfortable with the "move fast and break things" mentality of the new team.

This fork rolls back the project to the last stable release, 1.7.1, after which I started merging features and fixes back from later releases. However, this "merging back" is done in a more conservative and careful manner, and with a focus on verification and testing.
