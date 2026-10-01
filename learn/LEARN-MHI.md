# LEARN: MHI (Modem Host Interface)

> **Status in this lab: concepts only. Real MHI is NOT implemented.** No MHI device is available to emulate. What exists is a *model* of its char-device edge (`fake_mhi_modem.ko`) and a real PCI driver skeleton (`edu_pci.ko`). This guide marks clearly which is which.

## 1. What

MHI is Qualcomm's protocol for talking to a modem over **PCIe**. Instead of USB endpoints, host and device share **ring buffers in host memory** and signal each other with **doorbells** (MMIO writes) and **MSI interrupts**. Data and control are split into numbered **channels**: QMI control, IP data (`IP_HW0`), DIAG, firmware download (SAHARA), and so on.

## 2. Why it exists

PCIe gives far more bandwidth and lower latency than USB for 5G modems in laptops (M.2 cards) and in SoCs. MHI is how Linux (`drivers/bus/mhi`) and Android devices drive those modems.

## 3. How it works (conceptual)

### Linux stack

```
 user:   ModemManager/libqmi            ip/netd
           | open/read/write              | netdev
 /dev/wwan0qmi0                        mhi_hwip0 / wwan0
           ^                              ^
   mhi_wwan_ctrl (WWAN port)          mhi_net (netdev)
           \______ MHI bus (drivers/bus/mhi/host) ______/
                     channels: QMI(14/15), IP_HW0(100/101), ...
                              |
   mhi_pci_generic (PCI driver: BAR0 MMIO, MSI vectors, firmware via SAHARA/BHI)
                              |
                        PCIe M.2 modem
```

### Rings and doorbells

```
host RAM:  context arrays (channel ctx, event ctx, command ctx)
           transfer ring (TRE = {buffer ptr, len, flags})   per channel
           event ring    (completion events)                per interrupt vector
host -> device:  write TREs, then ring the channel DOORBELL (MMIO in BAR0)
device -> host:  DMA the data, write an event to the event ring, send MSI
host ISR:        walk the event ring -> complete the buffers -> ring the event doorbell
```

### State machine

```
RESET -> READY -> M0 (mission mode, channels run) <-> M1/M2/M3 (low-power)
  SYS_ERR (firmware crash) -> host collects a RAM dump, resets
EE (execution environment): PBL -> SBL -> AMSS (mission)
```

## 4. Where in this repo (what maps to what)

| real MHI piece | lab equivalent | real or modelled |
|---|---|---|
| `/dev/wwan0qmi0` char device (mhi_wwan_ctrl) | `/dev/fake_qmi0` (`kernel/fake_mhi_modem/fake_mhi_modem.c`) | real char-device code, modelled modem |
| MHI states RESET/READY/M0/SYS_ERR | `enum fqmi_state` (`kernel/include/fake_qmi_uapi.h`) | names only, no real transitions |
| `mhi_pci_generic` probe/BAR/MSI | `edu_pci.ko` (`kernel/edu_pci/edu_pci.c:141`) | real PCI driver, different device |
| transfer/event rings, doorbells | none | **not implemented** |
| `mhi_net` netdev | dummy `wwan0` (`tests/e2e_daemon.sh`) | stand-in |
| QMUX bytes on the QMI channel | `lib/qmi` | real format (libqmi-checked) |

## 5. How it is verified

The parts that exist are verified as described in LEARN-Linux-kernel-modules-and-char-devices and LEARN-PCIe-drivers (`MLAB_KERNEL=1 ./run-tests.sh`). **Nothing about MHI rings is verified, because none exist.**

## 6. Real vs simulated vs blocked

**Blocked:** QEMU has no MHI endpoint device, and there is no M.2 modem hardware. The upstream `mhi_ep` (endpoint) stack and `pci-epf-mhi` would need an endpoint-capable SoC or a dedicated QEMU model.

## 7. Interview Q&A

1. **What does MHI replace?** The USB transport (cdc-wdm and qmi_wwan) for PCIe modems.
2. **What is a doorbell?** An MMIO register write that tells the device "new TREs are ready on channel N" (or that an event ring was consumed).
3. **Why do the rings live in host memory?** So the device can DMA directly into host memory, without the CPU copying into device memory.
4. **How does user space see the QMI channel?** As `/dev/wwan0qmi0` through the WWAN subsystem. The bytes are the same QMUX frames as over USB.
5. **What happens on SYS_ERR?** The host gets an error state change, optionally collects a RAM dump over the BHIe/SAHARA path, then resets the device and powers it back up through READY to M0.
6. **What are execution environments?** The modem boot stages (PBL, SBL, AMSS). Channels become available in specific EEs.
7. **QMUX over MHI vs QRTR?** On newer platforms QMI runs over the QRTR router (`AF_QIPCRTR`) on an MHI "IPCR" channel instead of QMUX.

## 8. Try it yourself

1. Read `drivers/bus/mhi/host/pci_generic.c` and find where it calls `pci_alloc_irq_vectors` and `mhi_register_controller`. Compare with `edu_probe`.
2. Extend `fake_mhi_modem.ko` with a "transfer ring": a kmalloc'd array of TRE structs and a doorbell ioctl that processes them. This is still a model, but it has the right shape.
3. Read `drivers/net/wwan/mhi_wwan_ctrl.c` and list which fops it gets from the WWAN core, compared with what `fqmi_fops` implements.
