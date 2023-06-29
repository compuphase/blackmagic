# Black Magic Probe (for NXP LPC series)

**Nota Bene** This is a fork of release 1.7 of the original [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic). For newer releases, please see that project. For documentation on the Black Magic Probe, also go to the original project, or read my free e-book [Embedded Debugging with the Black Magic Probe](https://github.com/compuphase/Black-Magic-Probe-Book).

## Why does this fork exist?
The official release 1.7 has always run reliably in our production, but it does not support a few of the micro-controllers of the LPC family (by NXP) that we happen to use in our products. I am specifically referring to the LPC800 series and the LPC1110-**XL** series.

At the time of writing, *all* official releases after 1.7 have been broken when it comes to the support for the LPC series of microcontrollers. Versions 1.9.0 and 1.9.1 are fatally broken in the sense that they don't work at all with LPC microcontrollers; versions 1.8.0 to 1.8.2 are too flaky, with random failures.

### Rant: Why choose to fork?
One might say that, instead of forking, I could also choose to contribute to the original project.

It may be a matter of perspective. I bought the Black Magic Probe (three of them, in fact) for the developers in my company. I need a tool that we can depend on. When the debug probe leaves you guessing whether it's target that just crashed or whether it's the probe that crashed, it is a waste of time. Yet, that is the case for all 1.8.x releases. The issue with release 1.9.0 is baffling: all it takes is a `monitor swdp_scan` (with *any* LPC microcontroller connected) to crash the BMP. That release has not been tested on an LPC microcontroller, not on a single one of them. I have been involved with fixing these issues: I wrote (and emailed) a report on issue of the `set-up` and `hold` times that plagued the 1.8.x releases; I pin-pointed and reported the division-by-zero that caused the crash in release 1.9.0; I shipped boards with microcontrollers that 1BitSquared wanted to test with to them (at no cost to them).

So, my response to the question why I forked rather than contribute to the master project, is that despite these efforts, every official release since 1.7 has been unusable for us, and that we have (by necessity) always run modified firmware in our BMPs.

## Post 1.7 Features and Fixes
While this fork is basically release 1.7, selected fixes and changes from later releases have been merged in. The most notable ones are:
* Added support for LPC800, LPC1110-XL and LPC4000 series.
* Merged in the "fake thread" support needed for GDB 11 and later (see PR #1125 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic)).
* Merged the changes needed for hardware revision 6 of the Black Magic Probe (so this fork runs on both BMP v2.1 and BMP v2.3).
* Correct scheduling of driving/polling `SWDIO` relative to `SWCLK` and in regard to set-up and hold times (this is my own implementation, not based on PR #1220 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic), which is *still* not entirely correct).
