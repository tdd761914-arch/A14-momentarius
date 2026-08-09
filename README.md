# A14-momentarius

A12/A13 PPL Bypass by me and [Clarity](https://github.com/TheRealClarity)

This fork adds fail-closed A14/T8101 support for the iOS 18.5 AGX RTKit
firmware layout:

- `CPUFAMILY_ARM_FIRESTORM_ICESTORM` detection;
- semantic discovery of the `TTBR1_EL1` resume path;
- runtime `BPTP` patchbay lookup;
- validation of the unused executable range at `text+0xa80..0x1000`;
- full mutation: the A13 TTBR1-swap engine (`momentarius_build_ppl_write`) is
  reused with the A14 layout (code cave `text+0xa80`, decoder `text+0x25f0`,
  resume hook `text+0x2528`).

The A14 path no longer stops after the probe: it builds the fake page tables,
installs the shellcode and loads `new_ttbr1` during the next GFX resume.  Note
that the runtime writeability of AGX `__TEXT` on T8101 (`ascwrap-v4`) is not
statically proven; see `docs/A13_A14_OFFSETS_RU.md`.
