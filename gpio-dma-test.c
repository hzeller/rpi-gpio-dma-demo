/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * Copyright (c) 2015, Henner Zeller <h.zeller@acm.org>
 * This is provided as-is to the public domain.
 *
 * This is not meant to be useful code, just an attempt
 * to understand GPIO performance if sent with DMA (which is: too low).
 * It might serve as an educational example though.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Physical Memory Allocation, from raspberrypi/userland demo.
#include "mailbox.h"

// GPIO which we want to toggle in this example.
#define TOGGLE_GPIO 14

// Raspberry Pi 2 or 1 ? Since this is a simple example, we don't
// bother auto-detecting but have it a compile-time option.
#ifndef PI_VERSION
#  define PI_VERSION 2
#endif

#define BCM2708_PI1_PERI_BASE  0x20000000
#define BCM2709_PI2_PERI_BASE  0x3F000000

// --- General, Pi-specific setup.
#if PI_VERSION == 2
#  define PERI_BASE BCM2709_PI2_PERI_BASE
#  define MEM_FLAG 0xc
#else
#  define PERI_BASE BCM2708_PI1_PERI_BASE
#  define MEM_FLAG 0x4
#endif

#define PAGE_SIZE 4096

// ---- GPIO specific defines
#define GPIO_REGISTER_BASE 0x200000
#define GPIO_SET_OFFSET 0x1C
#define GPIO_CLR_OFFSET 0x28
#define PHYSICAL_GPIO_BUS (0x7E000000 + GPIO_REGISTER_BASE)

// ---- Memory mappping defines
#define BUS_TO_PHYS(x) ((x)&~0xC0000000)

// ---- DMA specific defines
#define DMA_CHANNEL       5   // That usually is free.
#define DMA_BASE          0x007000

// BCM2385 ARM Peripherals 4.2.1.2
#define DMA_CB_TI_NO_WIDE_BURSTS (1<<26)
#define DMA_CB_TI_SRC_INC        (1<<8)
#define DMA_CB_TI_DEST_INC       (1<<4)
#define DMA_CB_TI_TDMODE         (1<<1)

#define DMA_CS_RESET    (1<<31)
#define DMA_CS_ABORT    (1<<30)
#define DMA_CS_DISDEBUG (1<<28)
#define DMA_CS_END      (1<<1)
#define DMA_CS_ACTIVE   (1<<0)

#define DMA_CB_TXFR_LEN_YLENGTH(y) (((y-1)&0x4fff) << 16)
#define DMA_CB_TXFR_LEN_XLENGTH(x) ((x)&0xffff)
#define DMA_CB_STRIDE_D_STRIDE(x)  (((x)&0xffff) << 16)
#define DMA_CB_STRIDE_S_STRIDE(x)  ((x)&0xffff)

#define DMA_CS_PRIORITY(x) ((x)&0xf << 16)
#define DMA_CS_PANIC_PRIORITY(x) ((x)&0xf << 20)


// Documentation: BCM2835 ARM Peripherals @4.2.1.2
struct dma_channel_header {
  uint32_t cs;        // control and status.
  uint32_t cblock;    // control block address.
};

// @4.2.1.1
struct dma_cb {    // 32 bytes.
  uint32_t info;   // transfer information.
  uint32_t src;    // physical source address.
  uint32_t dst;    // physical destination address.
  uint32_t length; // transfer length.
  uint32_t stride; // stride mode.
  uint32_t next;   // next control block; Physical address. 32 byte aligned.
  uint32_t pad[2];
};

// A memory block that represents memory that is allocated in physical
// memory and locked there so that it is not swapped out.
// It is not backed by any L1 or L2 cache, so writing to it will directly
// modify the physical memory (and it is slower of course to do so).
// This is memory needed with DMA applications so that we can write through
// with the CPU and have the DMA controller 'see' the data.
// The UncachedMemBlock_{alloc,free,to_physical}
// functions are meant to operate on these.
struct UncachedMemBlock {
  void *mem;                  // User visible value: the memory to use.
  //-- Internal representation.
  uint32_t bus_addr;
  uint32_t mem_handle;
  size_t size;
};

static int mbox_fd = -1;   // used internally by the UncachedMemBlock-functions.

// Allocate a block of memory of the given size (which is rounded up to the next
// full page). The memory will be aligned on a page boundary and zeroed out.
static struct UncachedMemBlock UncachedMemBlock_alloc(size_t size) {
  if (mbox_fd < 0) {
    mbox_fd = mbox_open();
    assert(mbox_fd >= 0);  // Uh, /dev/vcio not there ?
  }
  // Round up to next full page.
  size = size % PAGE_SIZE == 0 ? size : (size + PAGE_SIZE) & ~(PAGE_SIZE - 1);

  struct UncachedMemBlock result;
  result.size = size;
  result.mem_handle = mem_alloc(mbox_fd, size, PAGE_SIZE, MEM_FLAG);
  result.bus_addr = mem_lock(mbox_fd, result.mem_handle);
  result.mem = mapmem(BUS_TO_PHYS(result.bus_addr), size);
  fprintf(stderr, "Alloc: %6d bytes;  %p (bus=0x%08x, phys=0x%08x)\n",
          (int)size, result.mem, result.bus_addr, BUS_TO_PHYS(result.bus_addr));
  assert(result.bus_addr);  // otherwise: couldn't allocate contiguous block.
  memset(result.mem, 0x00, size);

  return result;
}

// Free block previously allocated with UncachedMemBlock_alloc()
static void UncachedMemBlock_free(struct UncachedMemBlock *block) {
  if (block->mem == NULL) return;
  assert(mbox_fd >= 0);  // someone should've initialized that on allocate.
  unmapmem(block->mem, block->size);
  mem_unlock(mbox_fd, block->mem_handle);
  mem_free(mbox_fd, block->mem_handle);
  block->mem = NULL;
}


// Given a pointer to memory that is in the allocated block, return the
// physical bus addresse needed by DMA operations.
static uintptr_t UncachedMemBlock_to_physical(const struct UncachedMemBlock *blk,
                                              void *p) {
    uint32_t offset = (uint8_t*)p - (uint8_t*)blk->mem;
    assert(offset < blk->size);   // pointer not within our block.
    return blk->bus_addr + offset;
}

// Return a pointer to a periphery subsystem register.
static void *mmap_bcm_register(off_t register_offset) {
  const off_t base = PERI_BASE;

  int mem_fd;
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    perror("can't open /dev/mem: ");
    fprintf(stderr, "You need to run this as root!\n");
    return NULL;
  }

  uint32_t *result =
    (uint32_t*) mmap(NULL,                  // Any adddress in our space will do
                     PAGE_SIZE,
                     PROT_READ|PROT_WRITE,  // Enable r/w on GPIO registers.
                     MAP_SHARED,
                     mem_fd,                // File to map
                     base + register_offset // Offset to bcm register
                     );
  close(mem_fd);

  if (result == MAP_FAILED) {
    fprintf(stderr, "mmap error %p\n", result);
    return NULL;
  }
  return result;
}

void initialize_gpio_for_output(volatile uint32_t *gpio_registerset, int bit) {
  *(gpio_registerset+(bit/10)) &= ~(7<<((bit%10)*3));  // prepare: set as input
  *(gpio_registerset+(bit/10)) |=  (1<<((bit%10)*3));  // set as output.
}

/* --------------------------------------------------------------------------
 * In each of the following run_* demos, we have a somewhat repetetive setup
 * for each of these. This is intentional, so that it is easy to read each
 * of these as independent example.
 * --------------------------------------------------------------------------
 */

/*
 * Direct output of data to the GPIO in a tight loop. This is the highest speed
 * it can get.
 */
void run_cpu_direct() {
  // Prepare GPIO
  volatile uint32_t *gpio_port = mmap_bcm_register(GPIO_REGISTER_BASE);
  initialize_gpio_for_output(gpio_port, TOGGLE_GPIO);
  volatile uint32_t *set_reg = gpio_port + (GPIO_SET_OFFSET / sizeof(uint32_t));
  volatile uint32_t *clr_reg = gpio_port + (GPIO_CLR_OFFSET / sizeof(uint32_t));

  // Do it. Endless loop, directly setting.
  printf("1) CPU: Writing to GPIO directly in tight loop\n"
         "== Press Ctrl-C to exit.\n");
  for (;;) {
    *set_reg = (1<<TOGGLE_GPIO);
    *clr_reg = (1<<TOGGLE_GPIO);
  }
}

/*
 * Read data from memory and write to GPIO.
 * Memory is organized as 32-Bit data to be written to the GPIO. We expand
 * these using a mask to do the actual set/reset sequence.
 * This is pretty compact in memory and probably the 'usual' way how you
 * would send data to GPIO.
 */
void run_cpu_from_memory_masked() {
  // Prepare GPIO
  volatile uint32_t *gpio_port = mmap_bcm_register(GPIO_REGISTER_BASE);
  initialize_gpio_for_output(gpio_port, TOGGLE_GPIO);
  volatile uint32_t *set_reg = gpio_port + (GPIO_SET_OFFSET / sizeof(uint32_t));
  volatile uint32_t *clr_reg = gpio_port + (GPIO_CLR_OFFSET / sizeof(uint32_t));

  // Prepare data.
  const int n = 256;
  uint32_t *gpio_data = (uint32_t*) malloc(n * sizeof(*gpio_data));
  for (int i = 0; i < n; ++i) {
    // To toggle our pin, alternate between set and not set.
    gpio_data[i] = (i % 2 == 0) ? (1<<TOGGLE_GPIO) : 0;
  }

  // Do it. Endless loop: reading, writing.
  printf("2) CPU: reading word from memory, write masked to GPIO set/clr.\n"
         "== Press Ctrl-C to exit.\n");
  const uint32_t mask = (1<<TOGGLE_GPIO);
  const uint32_t *start = gpio_data;
  const uint32_t *end   = start + n;
  for (;;) {
    for (const uint32_t *run = start; run < end; ++run) {
      if (( *run & mask) != 0) *set_reg =  *run & mask;
      if ((~*run & mask) != 0) *clr_reg = ~*run & mask;
    }
  }

  free(gpio_data);  // (though never reached due to Ctrl-C)
}

/*
 * Read data from memory. Memory layout has set/reset already prepared.
 * While this is not necessarily useful for regular processing, it is a
 * good prepatory step to understand the layout in the DMA case.
 */
void run_cpu_from_memory_set_reset() {
  // Prepare GPIO
  volatile uint32_t *gpio_port = mmap_bcm_register(GPIO_REGISTER_BASE);
  initialize_gpio_for_output(gpio_port, TOGGLE_GPIO);
  volatile uint32_t *set_reg = gpio_port + (GPIO_SET_OFFSET / sizeof(uint32_t));
  volatile uint32_t *clr_reg = gpio_port + (GPIO_CLR_OFFSET / sizeof(uint32_t));

  // Layout of our input data. The values are pre-expanded to set and clr.
  struct GPIOData {
    uint32_t set;
    uint32_t clr;
  };

  // Prepare data.
  const int n = 256;
  struct GPIOData *gpio_data = (struct GPIOData*) malloc(n * sizeof(*gpio_data));
  for (int i = 0; i < n; ++i) {
    gpio_data[i].set = (1<<TOGGLE_GPIO);
    gpio_data[i].clr = (1<<TOGGLE_GPIO);
  }

  // Do it. Endless loop: reading, writing.
  printf("3) CPU: reading prepared set/clr from memory, write to GPIO.\n"
         "== Press Ctrl-C to exit.\n");
  const struct GPIOData *start = gpio_data;
  const struct GPIOData *end = start + n;
  for (;;) {
    for (const struct GPIOData *run = start; run < end; ++run) {
      *set_reg = run->set;
      *clr_reg = run->clr;
    }
  }

  free(gpio_data);  // (though never reached due to Ctrl-C)
}

/*
 * Writing data via DMA to GPIO. We do that in a 2D write with a stride that
 * skips the gap between the GPIO registers. Each of these GPIO operations
 * is described as a single control block.
 *
 * This requires a lot of overhead memory for the control blocks (each 8 byte
 * data requires a 32 byte control block). So we use about 40 bytes per
 * one 32-bit set/clear operation.
 */
void run_dma_single_transfer_per_cb() {
  // Prepare GPIO
  volatile uint32_t *gpio_port = mmap_bcm_register(GPIO_REGISTER_BASE);
  initialize_gpio_for_output(gpio_port, TOGGLE_GPIO);

  // Layout of our input data.
  struct GPIOData {
    uint32_t set;
    uint32_t clr;
  };

  // Prepare data. This needs to be in uncached memory. We only set up
  // a single GPIOData because we'll be setting up the DMA controller into
  // a loop (note, we can allocate one memblock for this and the dma_cb
  // to keep everything nicely tight in memory, but let's separate this here
  // for easier understanding).
  struct GPIOData *gpio_data;
  struct UncachedMemBlock memblock = UncachedMemBlock_alloc(sizeof(*gpio_data));
  gpio_data = (struct GPIOData*) memblock.mem;
  gpio_data->set = (1<<TOGGLE_GPIO);
  gpio_data->clr = (1<<TOGGLE_GPIO);

  // Prepare DMA control block. This needs to be in uncached data, so that
  // when we set it up, it is immediately visible to the DMA controller.
  // Also, only UncachedMemBlock allows us to conveniently get the physical
  // address.
  struct UncachedMemBlock cb_memblock
    = UncachedMemBlock_alloc(sizeof(struct dma_cb));
  struct dma_cb *cb = (struct dma_cb*) cb_memblock.mem;
  cb->info   = (DMA_CB_TI_SRC_INC | DMA_CB_TI_DEST_INC |
                DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE);
  cb->src    = UncachedMemBlock_to_physical(&memblock, gpio_data);
  cb->dst    = PHYSICAL_GPIO_BUS + GPIO_SET_OFFSET;
  // Setting up two transfers, each with length 4. Reading set and clr
  // respectively.
  cb->length = DMA_CB_TXFR_LEN_YLENGTH(2) | DMA_CB_TXFR_LEN_XLENGTH(4);

  // For the output, after the first 4 byte write, there is an 8 byte gap,
  // that we have to skip between the set and clear GPIO register.
  cb->stride = DMA_CB_STRIDE_D_STRIDE(8) | DMA_CB_STRIDE_S_STRIDE(0);

  // Now set the 'next' block up, which it ourself. So essentially loop back.
  cb->next   = UncachedMemBlock_to_physical(&cb_memblock, cb);

  printf("4) DMA: Single control block per set/reset GPIO\n"
         "== Press <RETURN> to exit (with CTRL-C DMA keeps going).");

  char *dmaBase = mmap_bcm_register(DMA_BASE);
  // 4.2.1.2
  struct dma_channel_header* channel
    = (struct dma_channel_header*)(dmaBase + 0x100 * DMA_CHANNEL);

  channel->cs |= DMA_CS_END;
  channel->cblock = UncachedMemBlock_to_physical(&cb_memblock, cb);
  channel->cs = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG;

  // The following operation starts the loop.
  channel->cs |= DMA_CS_ACTIVE;  // Aaaand action.

  // At this point, the DMA controller loops by itself, the CPU is free.
  getchar();

  // Shutdown DMA channel.
  channel->cs |= DMA_CS_ABORT;
  usleep(100);
  channel->cs &= ~DMA_CS_ACTIVE;
  channel->cs |= DMA_CS_RESET;

  UncachedMemBlock_free(&memblock);
  UncachedMemBlock_free(&cb_memblock);
}

/*
 * Here, we use a different trick with strides. We set up the source data
 * in a way that it mimicks the layout of the GPIO set/clr registers and
 * copy that from the source to the GPIO registers; we use the destination
 * stride to go _backwards_ to the start of the registers while we keep
 * going reading from the source.
 *
 * There is some stuff between the set and clr we are interested in, which
 * means there are 8 dead bytes between our payload. It means we have to
 * copy more data and waste more data 'setup' area. However, overall, we
 * are using less memory than in the
 */
void run_dma_multi_transfer_per_cb() {
  // Prepare GPIO
  volatile uint32_t *gpio_port = mmap_bcm_register(GPIO_REGISTER_BASE);
  initialize_gpio_for_output(gpio_port, TOGGLE_GPIO);

  // Layout of our input data. It mimicks the same layout of the GPIO registers.
  // It covers an 'reserved' area between the set/clear registers, which we
  // just will be writing zeroes to and hope the GPIO subsystem doesn't mind.
  struct GPIOData {
    uint32_t set;
    uint32_t ignored_upper_set_bits; // bits 33..54 of GPIO. Not needed.
    uint32_t reserved_area;          // gap between GPIO registers.
    uint32_t clr;
  };

  // Prepare data. This needs to be in uncached memory. We set up a bunch
  // of data that we then send via DMA in a loop to GPIO.
  const int n = 256;
  struct GPIOData *gpio_data;
  struct UncachedMemBlock memblock
    = UncachedMemBlock_alloc(n * sizeof(*gpio_data));
  gpio_data = (struct GPIOData*) memblock.mem;
  for (int i = 0; i < n; ++i) {
    gpio_data[i].set = (1<<TOGGLE_GPIO);
    gpio_data[i].clr = (1<<TOGGLE_GPIO);
  }

  // Prepare DMA control block. This needs to be in uncached data, so that
  // when we set it up, it is immediately visible to the DMA controller.
  // Also, only UncachedMemBlock allows us to conveniently get the physical
  // address.
  struct UncachedMemBlock cb_memblock
    = UncachedMemBlock_alloc(sizeof(struct dma_cb));
  struct dma_cb *cb = (struct dma_cb*) cb_memblock.mem;
  cb->info   = (DMA_CB_TI_SRC_INC | DMA_CB_TI_DEST_INC |
                DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE);
  cb->src    = UncachedMemBlock_to_physical(&memblock, gpio_data);
  cb->dst    = PHYSICAL_GPIO_BUS + GPIO_SET_OFFSET;
  // Setting up n transfers, that is the number of samples we have prepared
  // for GPIO, each with length of the struct GPIOData, which
  // is 16 bytes.
  cb->length = DMA_CB_TXFR_LEN_YLENGTH(n) | DMA_CB_TXFR_LEN_XLENGTH(16);

  // After the transfer, we want to go back 16 bytes on the destination side,
  // so that we start with the registers again - so we have a stride of -16,
  // while we just continue reading on the source side.
  cb->stride = DMA_CB_STRIDE_D_STRIDE(-16) | DMA_CB_STRIDE_S_STRIDE(0);

  // Now set the 'next' block up, which it ourself. So essentially loop back.
  cb->next   = UncachedMemBlock_to_physical(&cb_memblock, cb);

  printf("5) DMA: Sending a sequence of set/clear with one DMA control block "
         "and negative destination stride.\n"
         "== Press <RETURN> to exit (with CTRL-C DMA keeps going).");

  char *dmaBase = mmap_bcm_register(DMA_BASE);
  // 4.2.1.2
  struct dma_channel_header* channel
    = (struct dma_channel_header*)(dmaBase + 0x100 * DMA_CHANNEL);

  channel->cs |= DMA_CS_END;
  channel->cblock = UncachedMemBlock_to_physical(&cb_memblock, cb);
  channel->cs = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG;

  // The following operation starts the loop.
  channel->cs |= DMA_CS_ACTIVE;  // Aaaand action.

  // At this point, the DMA controller loops by itself, the CPU is free.
  getchar();

  // Shutdown DMA channel.
  channel->cs |= DMA_CS_ABORT;
  usleep(100);
  channel->cs &= ~DMA_CS_ACTIVE;
  channel->cs |= DMA_CS_RESET;

  UncachedMemBlock_free(&memblock);
  UncachedMemBlock_free(&cb_memblock);
}

static int usage(const char *prog) {
  fprintf(stderr, "Usage %s [1...5]\n", prog);
  fprintf(stderr, "Give number of test operation as argument to %s\n", prog);
  fprintf(stderr, "Test operation\n"
          "== Baseline tests, using CPU directly ==\n"
          "1 - CPU: Writing to GPIO directly in tight loop\n"
          "2 - CPU: reading word from memory, write masked to GPIO set/clr.\n"
          "3 - CPU: reading prepared set/clr from memory, write to GPIO.\n"
          "\n== DMA tests, using DMA to pump data to ==\n"
          "4 - DMA: Single control block per set/reset GPIO\n"
          "5 - DMA: Sending a sequence of set/clear with one DMA control block and negative destination stride.\n");
  return 1;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    return usage(argv[0]);
  }

  switch (atoi(argv[1])) {
  case 1:
    run_cpu_direct();
    break;
  case 2:
    run_cpu_from_memory_masked();
    break;
  case 3:
    run_cpu_from_memory_set_reset();
    break;
  case 4:
    run_dma_single_transfer_per_cb();
    break;
  case 5:
    run_dma_multi_transfer_per_cb();
    break;
  default:
    return usage(argv[0]);
  }

  return 0;
}
