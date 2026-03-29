# Device Drivers

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

This page primarily describes the planned userspace driver model.
The current kernel still relies on a small in-kernel bootstrap driver set.

Device drivers run as userspace servers.
They interact with hardware exclusively through [[Object Model|kernel objects]] -- the kernel mediates all hardware access and drivers hold only the handles they need.

## Hardware Access
- **MMIO** -- Device register spaces mapped as page-backed objects.
  Drivers receive a handle.
  [[Object Transaction Programs|Transaction programs]] could enable fast-path register writes without IPC.
- **DMA** -- Kernel allocates and pins DMA-safe pages, provides physical addresses to the driver.
- **IOMMU** -- Kernel programs the IOMMU; the mapping is itself a kernel object.

## Interrupts
Hardware interrupts are [[Object Model|kernel objects]] with handles.
When an interrupt fires, the kernel signals the interrupt object and the driver server's wait unblocks.
The server handles the interrupt and signals back to unmask.
See [[Interrupt Model#Planned Architecture]].

## Isolation
Revoking a misbehaving driver means closing its handles.
Every hardware resource (interrupts, MMIO, DMA buffers, IOMMU mappings) is a handle-bearing object with specific rights.
A crashed driver loses all access immediately, and the system can restart it cleanly.
See [[Server Lifecycle]].

## Current System
The kernel currently includes two in-kernel drivers for bootstrap purposes:
- **UART serial** -- used for logging and the [[Testing|test harness]]
- **PIT timer** -- kernel time base
