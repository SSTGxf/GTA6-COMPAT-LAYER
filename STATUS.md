
### Update (booting #203/#204 binary)

- The TLS wall (0x140004B5E) is cleared: guest TLS index is 10 at runtime, slot 10 non-null with [+8]=1 on the previously faulting thread.
- Current wall: guest function 0x1409df9d0 -> 0x1409dfc30 -> gate 0x14407e700 returns false, caller asserts at 0x1409DFC18.
- DStorage: 139/139 raw pages copied, 0 refused, deterministic pack attribution (data.rpf, id 0x30000705a4fca4f8 offset 0x1c000/16) identical in #203 and #204.
