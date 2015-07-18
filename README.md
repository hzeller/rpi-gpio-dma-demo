Direct and DMA output to GPIO on the Raspberry Pi
=================================================

Experiments to measure speed of various ways to output data to
GPIO.

```
Usage ./gpio-dma-test [1...5]
Give number of test operation as argument to ./gpio-dma-test
Test operation
== Baseline tests, using CPU directly ==
1 - CPU: Writing to GPIO directly in tight loop
2 - CPU: reading word from memory, write masked to GPIO set/clr.
3 - CPU: reading prepared set/clr from memory, write to GPIO.

== DMA tests, using DMA to pump data to ==
4 - DMA: Single control block per set/reset GPIO
5 - DMA: Sending a sequence of set/clear with one DMA control block and negative destination stride.
```

