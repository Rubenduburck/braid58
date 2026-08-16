# Source provenance

| Kernel | Candidate source/body SHA-256 |
|---|---|
| Encode32 AVX2 | `443de7c4efbd263998501426a442d19c64a03f57fe3659d293d5cb4540416c7b` |
| Decode32 AVX2 | `9ae75869bed812fd2b706a74b6ff614c9da3519ba91ab1a18494df5a7a988915` |
| Encode32 AVX-512 | `6bff0c42dd2c49ee9d0bf99f1f80d4647fbe3035a77f2b24989d809c51245328` |
| Decode32 AVX-512 | `2b741645fe2ac7b23964a0a5ce38ca12b508bc1d74a6afa54d0aaad4b1fc24d9` |
| Encode64 AVX2 | `64a879d0b6f4d084fb5a28c420ac017792c591c8142adc75e45ce587e786170a` |
| Decode64 AVX2 | `13dca80e4411d8111142fda5ad7bda1c06e61a8dc4a913df9fb049142536105a` |
| Encode64 AVX-512 | `4967d40db868823771561890f4a9bcab5ac8c19a94600347b881c3f3330a152c` |
| Decode64 AVX-512 | `bd495d8d36603e76e38b5a8928468c06b24d5d8c7be2e79807dea1bb8988a295` |
| Encode32 AVX2 x2/x3 source | `880b3ba85474303458467c6c4b3427b1596f775fdd645e87012938a0b8d436c3` |
| Encode64 AVX2 x2/x3 source | `1212890491b80772e66206550945e93bc762da22ff98f5ba4a42eb9aafbb2c07` |
| Encode32 AVX-512 x2/x3 source | `326a48d4aafddc3134db3a6da65443679f2b853e7551aa3e45cde5d6f2a98f5b` |
| Encode32 AVX-512 batch constants | `d83011c8aa6bffb735a8a97cdac24e3ea05a04f4962179b257925ab639c7c390` |

Encode64 AVX-512 originated in
`Braid58-AVX512-encode64-ZMM-B5-candidate.zip`, SHA-256
`eb7d144734f1faf7dd300dd26d025c940e10f2d7d9fbd149a0da5be90503b23c`.

Package integration changes:

- internal symbol names;
- public C and Rust wrappers;
- compile-time target selection;
- C integer constants for intrinsic immediate operands;
- build, test, audit, and benchmark integration.

Arithmetic kernel operations and tables are unchanged except where documented
in the source.

No Base58 Turbo, Firedancer, or five8 algorithm source is included.
`bench/turbo-public-bridge.patch` contains C ABI wrappers around Base58
Turbo's public API. `bench/turbo-criterion.patch` adds Braid58 entries,
deterministic benchmark inputs, and backend-specific plot selection to Turbo's
benchmark files; it does not modify Turbo's library source. AVX2 and AVX-512
are linked and measured in separate executables.

The project is licensed under the MIT license.
