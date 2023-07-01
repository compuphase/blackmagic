# Black Magic Probe - Roll-back

**Nota Bene** This is a fork of the original [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic). It takes a parallel route to the original project, with a different focus. For documentation on the Black Magic Probe, also go to the original project, or read my free e-book [Embedded Debugging with the Black Magic Probe](https://github.com/compuphase/Black-Magic-Probe-Book).

## Status
This branch is based on release 1.7.1 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic) (hence, the name "Roll-back"). Since then, functionality and microcontroller support are being copied and/or adapted from the mainline release to this fork.

At this moment, this *Roll-back* fork is on a par with official release 1.8.2. Notably, this Roll-back fork runs on BMPv2.3 (hardware release 6) as well as BMPv2.1 (hardware releases 3 & 4); and it includes the "fake thread" support needed for GDB 11 and later (see PR #1125 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic)).

In addition to the above, this fork features:
* More detailed and more extensive support for microcontrollers in the LPC family (by NXP).
* Correct scheduling of driving/polling `SWDIO` relative to `SWCLK` and in regard to set-up and hold times (this is my own implementation, not based on PR #1220 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic), which is *still* not entirely correct).

## Why does this fork exist?
The official release 1.7 has always run reliably in our production, but it does not support a few of the microcontrollers of the LPC family (by NXP) that we happen to use in our products. I am specifically referring to the LPC800 series and the LPC1110-**XL** series.

At the time of writing, *all* official releases after 1.7 have been broken when it comes to the support for the LPC series of microcontrollers. Versions 1.9.0 and 1.9.1 are fatally broken in the sense that they don't work *at all* with LPC microcontrollers; versions 1.8.0 to 1.8.2 are unstable, with random failures.

### Rant: Why choose to fork?
At first, I contributed contribute to the project, from filing a report on the issue of `set-up` and `hold` times that plagued the 1.8.x releases, to pin-pointing the division-by-zero that caused the crash in release 1.9.0, to shipping microcontroller boards to 1BitSquared (at no cost to them) to enable them to test Flash programming procedures.

Despite these efforts, every official release since 1.7 has been unusable for us. In my view, the Black Magic Project pays insufficient attention to quality control. When analyzing the random failures on the 1.8.x release, I have been wondering whether I was the first to hook a logic analyzer on the BMP. And I was stunned to realize that release 1.9.0 had not been testen on *any* microcontroller of the LPC family; likewise, it is difficult to imagine that the Flash programming bug in release 1.9.1 would have escaped being noticed, had it been tested on an LPC microcontroller.
