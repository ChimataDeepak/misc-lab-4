# LEARN: PCIe drivers (probe, BAR MMIO, MSI)

## 1. What

A PCI(e) driver is a kernel module that registers a `pci_driver` with an id table. When the PCI core finds a device whose vendor:device id matches, it calls `probe()`. The driver then:

1. enables the device
2. maps its **BAR** (a window of device registers in physical address space)
3. sets up interrupts (MSI/MSI-X or legacy INTx)
4. exposes something to user space

`edu_pci.ko` does all of this for QEMU's emulated `edu` teaching device (1234:11e8).

## 2. Why it exists

M.2 cellular modems are PCIe devices. Qualcomm's `mhi_pci_generic` uses this exact skeleton (id table, probe, BAR mapping, MSI vectors) before it starts the MHI layer on top.

## 3. How it works

```
PCI core enumerates bus ---- id match 1234:11e8 ----> edu_probe(pdev)
  pcim_enable_device(pdev)            // managed: undone automatically on remove
  pcim_iomap_regions(pdev, BIT(0))    // claim + ioremap BAR0 (1 MiB)
  pci_set_master(pdev)                // allow DMA; MSI is a bus-master write
  ioread32(bar0 + 0x00) & 0xff == 0xed   // identity check
  pci_alloc_irq_vectors(pdev,1,1, MSI|LEGACY); irq = pci_irq_vector(pdev,0)
  request_irq(irq, edu_isr, IRQF_SHARED)
  misc_register -> /dev/edu0
```

### Register map used

| offset | register | use |
|---|---|---|
| 0x00 | ident | 0xRRrr00ed |
| 0x04 | liveness | reads back ~written |
| 0x08 | factorial | write n, later read n! |
| 0x20 | status | bit 0x80 = raise an IRQ when the factorial is done |
| 0x24 | irq status | read inside the ISR |
| 0x60 | irq raise | software-triggered IRQ |
| 0x64 | irq ack | clears the IRQ |

### Interrupt-driven operation

```
ioctl(FACTORIAL, in=10)
  reinit_completion(done)
  iowrite32(0x80, STATUS); iowrite32(10, FACT)     // device starts computing
  wait_for_completion_timeout(done, 2s)  ......... sleeping
                                  MSI -> GICv3 ITS -> edu_isr():
                                     st = ioread32(IRQ_STAT); if !st return IRQ_NONE
                                     iowrite32(st, IRQ_ACK); complete(done)
  out = ioread32(FACT) = 3628800
```

## 4. Where in this repo

| item | location |
|---|---|
| skeleton comment | `kernel/edu_pci/edu_pci.c` header comment (lines 1-25) |
| ISR | `kernel/edu_pci/edu_pci.c:59` |
| ioctl (cmd validated first) | `:73`, `:83-86` |
| factorial via IRQ | `:103` |
| probe | `:141` (enable `:156`, BAR `:159`, IRQ vectors `:171`) |
| remove | `:199` |
| id table + alias | `:210-214` |
| uapi | `kernel/include/edu_uapi.h` |
| guest test (10 checks) | `kernel/tests/test_edu.c` |
| QEMU with `-device edu`, GICv3 | `kernel/qemu/build-and-boot.sh:23-26` |

## 5. How it is verified

```bash
MLAB_KERNEL=1 ./run-tests.sh
```

The guest checks:

- ident `& 0xff == 0xed`
- BAR0 is 1 MiB
- liveness `== ~in`
- 10! = 3628800 and 12! = 479001600, each through an interrupt
- a software IRQ with status 0x100
- `irq_count` rises by 3
- an unknown ioctl returns ENOTTY

`/proc/interrupts` is printed before and after. Status: 10/10 at `1837752`. Not rerun while writing these notes.

## 6. Real vs simulated

- **Real:** a PCI driver bound by the real PCI core in a real 6.1 kernel, with MMIO through a mapped BAR and real MSI delivery through the emulated GICv3 ITS.
- **Simulated:** the device is QEMU's `edu`, not a modem. It uses no DMA rings (edu has a simple DMA engine that this driver does not use).
- **Not done:** MSI-X with several vectors, DMA mapping (`dma_alloc_coherent`), runtime power management, AER.

## 7. Interview Q&A

1. **What is a BAR?** A Base Address Register. The device advertises a memory or I/O window, the firmware or kernel assigns it an address, and the driver ioremaps it and uses `ioread32`/`iowrite32`.
2. **Why `ioread32` and not a plain pointer dereference?** MMIO needs ordering and no caching or merging by the compiler. The accessors include the right barriers and volatile semantics.
3. **MSI vs INTx?** MSI is an in-band memory write per vector, so it is not shared and there are no line-level issues. INTx is a shared level-triggered line, so the ISR must check "is it mine?" and return IRQ_NONE if not.
4. **Why `pci_set_master`?** It enables bus mastering, which DMA and MSI writes need.
5. **What do `pcim_*` helpers buy you?** Devres-managed cleanup. Error paths and remove do not have to undo each step by hand.
6. **What is MODULE_DEVICE_TABLE for?** It exports aliases so udev/modprobe can load the module when the device appears.
7. **What must an ISR not do?** Sleep. Do the minimum (ack, record, `complete()` or schedule work) and defer the rest to a threaded IRQ or a workqueue.

## 8. Try it yourself

1. Use edu's DMA engine: `dma_alloc_coherent` a buffer, program the DMA src/dst/count/cmd registers (see QEMU `docs/specs/edu.rst`), and verify the copy.
2. Convert to `request_threaded_irq` and measure latency.
3. Boot QEMU with `-device edu,msi=off` (if your QEMU supports it) and confirm the INTx path and `irq_kind == 0`.
