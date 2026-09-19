# Third-party notices

MelonDMA is licensed under the **Apache License 2.0** ([LICENSE](LICENSE)).
This file documents where the code's knowledge comes from, and what is
deliberately kept out of this repository.

> **History.** Earlier revisions of this file described MelonDMA as a port of
> GPL-2.0-only mlx5 driver code and relicensed the project to GPL-2.0 on that
> basis. That description was inaccurate. MelonDMA is an original DriverKit
> implementation: its structure, logic and userspace layer are written from
> scratch, and the hardware/protocol facts it encodes come from public
> documentation and from dual-licensed header definitions — not from copied
> GPL source. The project is therefore relicensed to Apache-2.0, which better
> reflects its origin and lets downstream users adopt it without the copyleft
> obligation.

## Where the knowledge comes from

### Mellanox Adapters Programmer's Reference Manual (PRM)

The register layouts, command semantics, WQE/CQE encodings and doorbell
conventions are implemented from the PRM. The PRM itself is a proprietary
document: it is **not** distributed here, and no extracted text or images from
it are committed. Only the *facts* it describes (offsets, bit widths, opcodes,
command sequences) are encoded in the driver's own words.

### InfiniBand Architecture specification

The RoCEv2 / IB transport behaviour (QP state machine, work-request semantics,
completion rules) is implemented from the IBA specification. Like the PRM, the
spec PDF is proprietary and is not distributed here; only the protocol facts
are encoded in the code.

### Mellanox dual-licensed headers (GPL-2.0 OR OpenIB-BSD)

Register/bitfield constants that are transcribed verbatim (notably
`src/dext/Sources/hw/mlx5_bits.h` and the register decode headers) come from
Mellanox's `include/linux/mlx5/mlx5_ifc.h` and related headers, which Mellanox
licenses under a **choice** of GPL-2.0 or the **OpenIB.org BSD license**. This
project uses them under the **OpenIB-BSD** grant, which is Apache-2.0
compatible.

> Copyright (c) 2013-2015, Mellanox Technologies, Ltd. All rights reserved.

### Linux mlx5 driver / MLNX_OFED — behavioural reference only

The Linux `drivers/infiniband/hw/mlx5/`, `drivers/net/ethernet/mellanox/mlx5/core/`
and the MLNX_OFED drops are consulted as a *behavioural reference*: to confirm
the correct sequencing of firmware commands and to understand what a correct
driver should observe at each step. No source is copied from them. The upstream
rdma-core userspace library serves the same purpose for the verbs API shape.

## What is deliberately *not* in this repository

- **Mellanox PRM** and **InfiniBand Architecture specification** PDFs —
  proprietary, local reference only, never committed.
- **Apple XNU/DriverKit kernel sources** (`IOService.cpp`, `IOUserClient.cpp`,
  `IOUserServer.cpp`) — APSL-2.0, consulted locally while researching
  driver-matching internals; no code is transcribed from them and the files are
  excluded from git (see `.gitignore`).
- **Prebuilt binaries and build artifacts** — excluded from git (see `.gitignore`).
- **Donor/reference project checkouts** (`donors/`, `archive/`) — local-only,
  kept out of this repository.

## Provenance discipline going forward

- Files that transcribe structures/constants from `mlx5_ifc.h` or the PRM note
  the source in a comment rather than presenting magic numbers.
- No proprietary document text is committed to this repository.
- If a future contribution would change the licensing basis, the project
  license changes visibly with it — not silently.
