# Upstream provenance

`uxn2-upstream.c` is an unmodified snapshot of the official Hundred Rabbits
Uxn2 source used as the porting reference.

- Repository: `https://git.sr.ht/~rabbits/uxn2`
- Commit: `8794ea557cdf282968d3e0fc93714840a42b245c`
- Commit date: 2026-08-18
- Subject: `Wrap IMM opcodes`
- License: `UXN2-LICENSE` (MIT)

The RP2350 adaptation is in `../src/uxn_runtime.c`. The original snapshot is
kept beside it so changes in VM and device behavior can be audited.

The platform dependencies and their pinned revisions are listed in the root
README. Their license files are retained in `pico_shared/`, `pico_lib/` and
`tusb_xinput/`.
