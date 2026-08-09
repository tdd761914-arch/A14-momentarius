# A14-momentarius

A12/A13 PPL Bypass by me and [Clarity](https://github.com/TheRealClarity)

This fork adds fail-closed A14/T8101 research support for the iOS 18.5
AGX RTKit firmware layout:

- `CPUFAMILY_ARM_FIRESTORM_ICESTORM` detection;
- semantic discovery of the `TTBR1_EL1` resume path;
- runtime `BPTP` patchbay lookup;
- validation of the unused executable range at `text+0xa80..0x1000`.

The A14 path is probe-only: it reports the discovered runtime layout and stops
before modifying or executing AGX firmware. A successful probe is returned as
`MOMENTARIUS_A14_PROBE_ONLY`.
