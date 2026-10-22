# ERNativeUI API v1.3 snapshot provenance

These files are immutable copies of the public SDK released as
`ERNativeUI-v1.3.0`. This release freezes the complete 384-byte API 1.3
table.

| Property | Value |
|---|---|
| Git tag | `ERNativeUI-v1.3.0` |
| Commit | `8d342cc0dd1cd0d0e8b01aa263dddcc185306b64` |
| `erui.h` Git blob | `e1d291e4f2c493928041de7cb13fb7564d209667` |
| `ERNativeUI.hpp` Git blob | `dd7530cdded55b7efc540de4d249ec1e266e2ecf` |
| `erui.h` SHA-256 | `07cc9cf6516bbd39d0f5d15ab25074584568b1930273f52218babc06232bcc18` |
| `ERNativeUI.hpp` SHA-256 | `d519effcc252494fd16ad0a054156b98e076acf4c73de8933de6d9fac15e741f` |

The Git identities can be independently verified with:

```text
git rev-list -n 1 ERNativeUI-v1.3.0
git rev-parse ERNativeUI-v1.3.0:include/ernativeui/erui.h
git rev-parse ERNativeUI-v1.3.0:include/ernativeui/ERNativeUI.hpp
```

Do not edit these headers after they are committed. A compatibility failure
must be fixed in the current host or treated as a new major API contract.
