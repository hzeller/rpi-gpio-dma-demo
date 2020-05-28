GPIO Speed using CPU and DMA on the Raspberry Pi
================================================

Experiments to measure speed of various ways to output data to
GPIO. Also convenient code snippets to help get you started with GPIO.

I provide the code in [gpio-dma-test.c](./gpio-dma-test.c) to the public domain. If you
use DMA you need the mailbox implementation; for that note the Broadcom copyright header
with permissive license in [mailbox.h](./mailbox.h).

You can compile this for Raspberry Pi 1 or 2 and 3 by passing the `PI_VERSION`
variable when compiling

     PI_VERSION=1 make
     PI_VERSION=2 make  # works for Pi 2 and 3
     PI_VERSION=4 make  # works for Pi 4

The resulting program gives you a set of 6 experiments to conduct. By default, it toggles
GPIO 14 (which is pin 8 on the Raspberry Pi header).

```
Usage ./gpio-dma-test [1...6]
Give number of test operation as argument to ./gpio-dma-test
Test operation
== Baseline tests, using CPU directly ==
1 - CPU: Writing to GPIO directly in tight loop
2 - CPU: reading word from memory, write masked to GPIO set/clr.
3 - CPU: reading prepared set/clr from memory, write to GPIO.
4 - CPU: reading prepared set/clr from UNCACHED memory, write to GPIO.

== DMA tests, using DMA to pump data to ==
5 - DMA: Single control block per set/reset GPIO
6 - DMA: Sending a sequence of set/clear with one DMA control block and negative destination stride.
```

To understand the details, you want to read [BCM2835 ARM Peripherals][BCM2835-doc], an excellent
dataheet to get started (if you are the datasheet-reading kinda person).

# Measurements

In these experiments, we want to see how fast things can go, so we do a very simple operation
in which we toggle the output of a pin (see `TOGGLE_GPIO` in the source). In real applications,
the data would certainly be slightly more useful :)

The pictures in these experiments show the output wave-form on the given pin for the
various Raspberry Pis. These are screen-shots straight from an oscilloscope, the time-base
is with 100ns the same for all measurements to be able to compare them easily.

All measurements were done on unmodified Pis in their respective default clock speed with a default minimal Raspian operating-system.

TODO: For the Pi4, there are no images yet, just some preliminary measurements. Stay tuned.

## Writing from CPU to GPIO

The most common way to get data out on the GPIO port is using the CPU to send
the data. Let's do some measurements how the Pis perform here.

### Direct Output Loop to GPIO

`sudo ./gpio-dma-test 1`

In this simplest way to control the output, we essentially
just write to the GPIO set and clear register in a tight loop:
```c
// Pseudocode
for (;;) {
    *gpio_set_register = (1<<TOGGLE_PIN);
    *gpio_clr_register = (1<<TOGGLE_PIN);
}
```

##### Result
The resulting output wave on the Raspberry Pi 1 of **22.7Mhz**, the Raspberry Pi 2
reaches **41.7Mhz** and the Raspberry Pi 3 **65.8 Mhz**.

Raspberry Pi 1               | Raspberry Pi 2                | Raspberry Pi 3               | Raspberry Pi 4
-----------------------------|-------------------------------|------------------------------|----------------
![](img/rpi1-direct-loop.png)|![](img/rpi2-direct-loop.png)  |![](img/rpi3-direct-loop.png) | (about 131Mhz)

The limited resolution in the 100ns range of the scope did not read the frequency correctly
for the Pi 3 (so it only shows 58.8Mhz above) but if we zoom in, we see the 65.8Mhz

![](img/rpi3-direct-loop-zoom.png)

### Reading Word from memory, write masked set/clr

`sudo ./gpio-dma-test 2`

The most common way you'd probably send data to GPIO: you have an array of
32 bit data representing the bits to be written to GPIO and a mask that
defines which are the relevant bits in your application.

```c
// Pseudocode
uint32_t data[256];         // Words to be written to GPIO

const uint32_t mask = ...;  // The GPIO pins used in the program.
const uint32_t *start = data;
const uint32_t *end = start + 256;
for (const uint32_t *it = start; it < end; ++it) {
    if (( *it & mask) != 0) *gpio_set_register =  *it & mask;
    if ((~*it & mask) != 0) *gpio_clr_register = ~*it & mask;
}
```

##### Result
Raspberry Pi 2 and Pi 3 are unimpressed and output in the same speed as writing
directly, Raspberry Pi 1 takes a performance hit and drops to 14.7Mhz:

Raspberry Pi 1                       | Raspberry Pi 2                      | Raspberry Pi 3                      | Raspberry Pi 4
-------------------------------------|-------------------------------------|-------------------------------------|----------------
![](img/rpi1-cpu-mem-word-masked.png)|![](img/rpi2-cpu-mem-word-masked.png)|![](img/rpi3-cpu-mem-word-masked.png)|(about 131Mhz)

### Reading prepared set/clr from memory

`sudo ./gpio-dma-test 3`

This would be a bit more unusal way to prepare and write data: break out the
set and clr bits beforehand and store in memory before writing them to GPIO.
It uses twice as much memory per operation.
It does help the Raspberry Pi 1 to be as fast as possible writing from memory
though, while there is no additional advantage for the Raspberry Pi 2 or 3.

Primarily, this is a good preparation to understand the way we have to send data with DMA.

```c
// Pseudocode
struct GPIOData {
   uint32_t set;
   uint32_t clr;
};
struct GPIOData data[256];  // Preprocessed set/clr to be written to GPIO

const struct GPIOData *start = data;
const struct GPIOData *end = start + 256;
for (const struct GPIOData *it = start; it < end; ++it) {
    *gpio_set_register = it->set;
    *gpio_clr_register = it->clr;
}
```

##### Result
The Raspberry Pi 2 and Pi 3 have the same high speed as in the previous examples, but
Raspberry Pi 1 can digest the prepared data faster and gets up to 20.8Mhz
out of this (compared to the 14.7Mhz we got with masked writing):

Raspberry Pi 1                   | Raspberry Pi 2                  | Raspberry Pi 3                  | Raspberry Pi 4
---------------------------------|---------------------------------|---------------------------------|----------------
![](img/rpi1-cpu-mem-set-clr.png)|![](img/rpi2-cpu-mem-set-clr.png)|![](img/rpi3-cpu-mem-set-clr.png)|(about 83Mhz)


### Reading prepared set/clr from UNCACHED memory

`sudo ./gpio-dma-test 4`

This next example is not useful in real life, but it helps to better
understand the performance impact of accessing memory that does *not* go
through a cache (L1 or L2).

The DMA subsystem, which we are going to explore in the next examples, has to
read from physical memory, as it cannot use the caches (or can it ? Somewhere
I read that it can make at least use of L2 cache ?).

The example is the same as before: reading pre-processed set/clr values from
memory and writing them to GPIO. Only the type of memory is different.

##### Result

The speed is significantly reduced - it is very slow to read from uncached
memory (a testament of how fast CPUs are these days or slow DRAM actually
is).

One interesting finding is, that the Raspberry Pi 2 and Pi 3 both are actually significantly
slower than the Raspberry Pi 1. Maybe the makers were relying more on various
caches and choose to equip the machine with slower memory to keep the price while
increasing memory ? At least the Pi 3 is faster than the 2, so the relative order there is
preserved.

Raspberry Pi 1                            | Raspberry Pi 2                           | Raspberry Pi 3                           | Raspberry Pi 4
------------------------------------------|------------------------------------------|------------------------------------------|----------------
![](img/rpi1-cpu-uncached-mem-set-clr.png)|![](img/rpi2-cpu-uncached-mem-set-clr.png)|![](img/rpi3-cpu-uncached-mem-set-clr.png)|(about 2.7Mhz)

## Using DMA to write to GPIO

Using the Direct Memory Access (DMA) subsystem allows to free the CPU and
let independently running hardware do the job.

In various code that involve using DMA and GPIO on the Raspberry Pi, it is used
in conjunction with the PWM or PCM hardware to create slower paced output with
very reliable timing. Examples are [PiBits] by richardghirst or the
[icrobotics PiFM transmitter][PiFM].

In our example, by contrast, we want to measure the raw speed that is possible
using DMA (which is not very impressive as we'll see).

In order to use DMA, the DMA controller needs access to the actual memory bus address,
as it can't deal with virtual memory (which means as well: it needs to be in physical
memory and can't be swapped). There are various ways to allocate that memeory and do
the mapping, but it looks like a reliable way for all PIs is to use
the `/dev/vcio` interface provided by the Pi kernel; we are using
a [mailbox implementation][RPI-mbox] provided in an
[raspberrypi/userland][RPI-userland] fft example.
We have an abstraction around that called `UncachedMemBlock` in gpio-dma-test.c.

The DMA channel we are using in these examples is channel 5, as it is usually free, but
you can configure that in the source. It can not be a Lite channel, as we need DMA 2D
features for both examples.

### DMA: using one Control Block per GPIO operation

`sudo ./gpio-dma-test 5`

With DMA, we can't do any data manipulation operations (such as masking) at the time the
data is written, so just like in the last CPU example, we have to prepare the source data
as separate set and clear operations:

```c
struct GPIOData {
   uint32_t set;
   uint32_t clr;
};
```

The output needs to be written to the GPIO registers. Unfortunately, we can't
just do a plain 1:1 memory copy from the source data to the destination registers,
as the layout is a bit different than our input data: The *set* and *clr* register
are a few bytes apart, so there is a gap between the two write operations:

|Addr-offset | GPIO Register          | width             | Operation
|-----------:|------------------------|-------------------|-------------------
|       0x1c | **set** (lower-32 bits)| 32 bits = 4 bytes | &lt;-write here
|       0x20 | *unused upper bits*    | 32 bits = 4 bytes | skip
|       0x24 | *(reserved)*           | 32 bits = 4 bytes | skip
|       0x28 | **clr** (lower-32 bits)| 32 bits = 4 bytes | &lt;-.. and here

So what we are doing is to set up a DMA Control Block with a 2D operation:

   - Read single GPIO Data block as two read operations of 4 bytes, no stride between reads.
   - Write these as two 4 byte blocks, starting from the origin of the GPIO register block,
     0x1c, with a destination stride of 8 to skip the intermediate registers we are not
     interested in.

We set up the control block's 'next' pointer to point to itself, so the DMA controller
goes in an endless loop. No CPU needed, yay :)

(If you are wondering what "DMA 2D" operations means, this is not the place to explain the
details, but there is an old [embedded article][embedded-2d-dma-article]
to get you started on this standard feature of many DMA controllers.
Please also look at the code which contains some documentation.)

One thing to note is that we only can set up a *single* output operation in this matter.
Once we have written to the registers, the destination pointer is at the end of the relevant
register block and the only way to go back to the beginning is to start with a fresh
control block that has the starting address set to the beginning of the register block.
We'll address that in the next example.

This is incredibly inefficent in use of memory, in particular if you need to send more
than just a few blocks.
We need **40 Bytes per output** operation (8 Bytes GPIO data + 32 Bytes Control block).
Note, all this memory needs to be locked into RAM.

##### Result
First thing we notice is how slow things are in comparison to the write from the CPU.
As found out in the uncached CPU example, we see the influence of slower memory in the
Raspberry Pi 2 and Pi 3 here as well. The latter two show exactly the same speed.

The live scope shows that the output has quite a bit of jitter, so DMA alone will not give
you very reliable timing, you always have to combine that with PWM/PCM gating if you need
this in a realtime context.

Another interesting finding is the asymmetry between the set/clr time. It looks like it
takes about 100ns after the set operation until the clear operation arrives - but
that then is lasting much longer. This is probably due some extra time needed when
switching between control blocks (even though the 'next' control block is exactly the same):

Raspberry Pi 1                     | Raspberry Pi 2                    | Raspberry Pi 3                    | Raspberry Pi 4
-----------------------------------|-----------------------------------|-----------------------------------|-------------
![](img/rpi1-dma-one-op-per-cb.png)|![](img/rpi2-dma-one-op-per-cb.png)|![](img/rpi3-dma-one-op-per-cb.png)| (about 1.82Mhz)


### DMA: multiple GPIO operations per Control Block

`sudo ./gpio-dma-test 6`

One of the obvious down-sides of the previous example is, that we have to set up one
DMA control block for each write operation which is a lot of memory overhead. Does it
also mean a bad performance impact ?

If we set up the input data in a way that it has the same layout as the output registers, we
could use the stride operation on the destination side to go back to the beginning after
each write and so can do many write operations with a single control block setup.

```c
// Input data has same layout as the output registers.
struct GPIOData {
    uint32_t set;
    uint32_t ignored_upper_set_bits; // bits 33..54 of GPIO. Not needed.
    uint32_t reserved_area;          // gap between GPIO registers.
    uint32_t clr;
};
```

Of course, this means that we are writing to 'reserved' places in the GPIO registers. The
first 4 bytes after the 'set' register are benign, as these are essentially upper bits
of GPIO bits - if we write 0 to these it should be fine.
Slightly problematic _could_ be the next block of 4 bytes, as it is 'reserved' according
to the data sheet. We are writing zeros in here and hope for the best that this is not
doing any harm (it does seem to be fine :) ).

So in this example the 2D DMA operation is set up the following way:

   - Readig _n_ GPIO blocks of size 16 bytes, no stride between reads.
   - Write each block to the GPIO registers and stride **backwards** 16 bytes so that at the
     end of that operation, we are back at the beginning of the register block.

Now, we only need one control-block per _n_ operations, but each operation takes 16 bytes to
store in memory. Amortized still better than in the previous case.

As usual, if we want to do that endlessly, we can link that control block back to itself.

##### Result
Again, the Raspberry Pi 2 is slightly slower than the Raspberry Pi 1. In general, this method is
even _slower_ than one control block per data item. And again, Raspberry Pi 3 shows the same
speed as Raspberry Pi 2.

Similar to the previous example, the output has quite some jitter.

It is interesting, that the positive pulse is shorter (about 50ns) than in the previous example.
It suggests that writing the data in sequence with 8 dead bytes is faster than
the 8 byte stride skip that we had in the previous example. Also it means that the DMA probably
has a small cache for the 16 byte block, as it emits that part faster than it can read from the
uncached memory.

Now the 'low' part of the pulse is even longer than before, apparently the
minus 16 Byte stride takes its sweet time even though we don't switch between control blocks:

Raspberry Pi 1                       | Raspberry Pi 2                      | Raspberry Pi 3                      | Raspberry Pi 4
-------------------------------------|-------------------------------------|-------------------------------------|----------------
![](img/rpi1-dma-multi-op-per-cb.png)|![](img/rpi2-dma-multi-op-per-cb.png)|![](img/rpi3-dma-multi-op-per-cb.png)| (about 1.54Mhz)

# Conclusions

   - On output via direct write from the CPU, Raspberry Pi 2 maintains the same
     impressive speed of **41.7Mhz** independent if written directly from code or
     read from memory. The Raspberry Pi 3 even reaches **65.8Mhz**
   - Raspberry Pi 1 is in the 20Mhz range for direct and prepared output and sligtly
     slower if it has to do the mask-operation first.
   - DMA is slow because it has to read from unached memory. It only makes sense if you want
     to output data in a slower pace or really need to relieve the CPU from continuous updates.
     (**Is that it, can DMA be faster ?** If you know how, please let me know).
   - Using a single control block per output operation is slightly faster than doing
     multiple, but is very inefficient in use of memory (10x the actual payload).
   - Using the stride in 2D DMA seems to be _slower_ than actually writing the same number
     of bytes ?
   - The stride operation seems to take extra time as time going on to next DMA control blocks.
   - DMA on Rasbperry Pi 2 and 3 is slightly *slower* than on Raspberry Pi 1, maybe because the
     DRAM is slower ?

[BCM2835-doc]: https://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
[PiBits]: https://github.com/richardghirst/PiBits
[PiFM]: http://www.icrobotics.co.uk/wiki/index.php/Turning_the_Raspberry_Pi_Into_an_FM_Transmitter
[RPI-mbox]: https://github.com/raspberrypi/userland/blob/master/host_applications/linux/apps/hello_pi/hello_fft/mailbox.h
[RPI-userland]: https://github.com/raspberrypi/userland
[embedded-2d-dma-article]: http://www.embedded.com/design/mcus-processors-and-socs/4006782/Using-Direct-Memory-Access-effectively-in-media-based-embedded-applications--Part-1
