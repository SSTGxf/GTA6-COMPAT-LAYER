; live slice 0x144fc2100..0x144fc2400 via arena-slice (768 bytes)
; Decode rule: the slice base is mid-stream (byte 0 is 0xe9, a jmp whose rel32 target
; 0x1229c4bed is not guest code), so the listing walks FUNCTION BOUNDARIES: each run of
; 0xCC padding starts a new function. Every instruction below is decoded from such a
; boundary, and the walk from 0x144fc21f0 covers the faulting address 0x144fc21f5.

; ---- function boundary 0x144fc2100 ----
0x144fc2100: e9e82aa0dd         jmp      0x1229c4bed

; ---- function boundary 0x144fc2180 ----
0x144fc2180: 89c8               mov      eax, ecx
0x144fc2182: 488b0d8fcba304     mov      rcx, qword ptr [rip + 0x4a3cb8f]
0x144fc2189: 486bc058           imul     rax, rax, 0x58
0x144fc218d: 488b440128         mov      rax, qword ptr [rcx + rax + 0x28]
0x144fc2192: 488d0d27000000     lea      rcx, [rip + 0x27]
0x144fc2199: 4839c8             cmp      rax, rcx
0x144fc219c: 7508               jne      0x144fc21a6
0x144fc219e: 4889d1             mov      rcx, rdx
0x144fc21a1: e91a000000         jmp      0x144fc21c0

; ---- function boundary 0x144fc21d0 ----
0x144fc21d0: 4883ec28           sub      rsp, 0x28
0x144fc21d4: e817000000         call     0x144fc21f0
0x144fc21d9: 8d04c0             lea      eax, [rax + rax*8]   <== return address pushed by the call at 0x144fc21d4
0x144fc21dc: 89c1               mov      ecx, eax
0x144fc21de: c1e90b             shr      ecx, 0xb
0x144fc21e1: 31c1               xor      ecx, eax
0x144fc21e3: 89c8               mov      eax, ecx
0x144fc21e5: c1e00f             shl      eax, 0xf
0x144fc21e8: 01c8               add      eax, ecx
0x144fc21ea: 4883c428           add      rsp, 0x28
0x144fc21ee: c3                 ret      

; ---- function boundary 0x144fc21f0 ----
0x144fc21f0: 4885c9             test     rcx, rcx
0x144fc21f3: 7477               je       0x144fc226c
0x144fc21f5: 440fb601           movzx    r8d, byte ptr [rcx]   <== FAULT: reads [rcx] = 0x1e954ab78c5
0x144fc21f9: 89d0               mov      eax, edx
0x144fc21fb: 4183f822           cmp      r8d, 0x22
0x144fc21ff: 743b               je       0x144fc223c
0x144fc2201: 4585c0             test     r8d, r8d
0x144fc2204: 7435               je       0x144fc223b
0x144fc2206: 48ffc1             inc      rcx
0x144fc2209: 488d15a0015f01     lea      rdx, [rip + 0x15f01a0]
0x144fc2210: 450fb6c0           movzx    r8d, r8b
0x144fc2214: 450fb60410         movzx    r8d, byte ptr [r8 + rdx]
0x144fc2219: 4101c0             add      r8d, eax
0x144fc221c: 4589c1             mov      r9d, r8d
0x144fc221f: 41c1e10a           shl      r9d, 0xa
0x144fc2223: 4501c1             add      r9d, r8d
0x144fc2226: 440fb601           movzx    r8d, byte ptr [rcx]
0x144fc222a: 48ffc1             inc      rcx
0x144fc222d: 4489c8             mov      eax, r9d
0x144fc2230: c1e806             shr      eax, 6
0x144fc2233: 4431c8             xor      eax, r9d
0x144fc2236: 4584c0             test     r8b, r8b
0x144fc2239: 75d5               jne      0x144fc2210
0x144fc223b: c3                 ret      

; ---- function boundary 0x144fc2270 ----
0x144fc2270: 4885c9             test     rcx, rcx
0x144fc2273: 7407               je       0x144fc227c
0x144fc2275: 31d2               xor      edx, edx
0x144fc2277: e904000000         jmp      0x144fc2280

; ---- function boundary 0x144fc2280 ----
0x144fc2280: 0fb601             movzx    eax, byte ptr [rcx]
0x144fc2283: 84c0               test     al, al
0x144fc2285: 742b               je       0x144fc22b2
0x144fc2287: 48ffc1             inc      rcx
0x144fc228a: 660f1f440000       nop      word ptr [rax + rax]
0x144fc2290: 0fbec0             movsx    eax, al
0x144fc2293: 01d0               add      eax, edx
0x144fc2295: 4189c0             mov      r8d, eax
0x144fc2298: 41c1e00a           shl      r8d, 0xa
0x144fc229c: 4101c0             add      r8d, eax
0x144fc229f: 0fb601             movzx    eax, byte ptr [rcx]
0x144fc22a2: 48ffc1             inc      rcx
0x144fc22a5: 4489c2             mov      edx, r8d
0x144fc22a8: c1ea06             shr      edx, 6
0x144fc22ab: 4431c2             xor      edx, r8d
0x144fc22ae: 84c0               test     al, al
0x144fc22b0: 75de               jne      0x144fc2290
0x144fc22b2: 8d04d2             lea      eax, [rdx + rdx*8]
0x144fc22b5: 89c1               mov      ecx, eax
0x144fc22b7: c1e90b             shr      ecx, 0xb
0x144fc22ba: 31c1               xor      ecx, eax
0x144fc22bc: 89c8               mov      eax, ecx
0x144fc22be: c1e00f             shl      eax, 0xf
0x144fc22c1: 01c8               add      eax, ecx
0x144fc22c3: c3                 ret      

; ---- function boundary 0x144fc22d0 ----
0x144fc22d0: 8b899c000000       mov      ecx, dword ptr [rcx + 0x9c]
0x144fc22d6: e905000000         jmp      0x144fc22e0

; ---- function boundary 0x144fc22e0 ----
0x144fc22e0: 89c8               mov      eax, ecx
0x144fc22e2: 2500003e00         and      eax, 0x3e0000
0x144fc22e7: 3d00000800         cmp      eax, 0x80000
0x144fc22ec: 751e               jne      0x144fc230c
0x144fc22ee: 81e1ffff0100       and      ecx, 0x1ffff
0x144fc22f4: 0faf0d29bdbf05     imul     ecx, dword ptr [rip + 0x5bfbd29]
0x144fc22fb: 488b052ebdbf05     mov      rax, qword ptr [rip + 0x5bfbd2e]
0x144fc2302: 83c108             add      ecx, 8
0x144fc2305: 66833c48ff         cmp      word ptr [rax + rcx*2], -1
0x144fc230a: 7503               jne      0x144fc230f
0x144fc230c: 31c0               xor      eax, eax
0x144fc230e: c3                 ret      

; ---- function boundary 0x144fc2330 ----
0x144fc2330: 8b899c000000       mov      ecx, dword ptr [rcx + 0x9c]
0x144fc2336: e905000000         jmp      0x144fc2340

; ---- function boundary 0x144fc2340 ----
0x144fc2340: 89c8               mov      eax, ecx
0x144fc2342: 2500003e00         and      eax, 0x3e0000
0x144fc2347: 3d00000800         cmp      eax, 0x80000
0x144fc234c: 751e               jne      0x144fc236c
0x144fc234e: 81e1ffff0100       and      ecx, 0x1ffff
0x144fc2354: 0faf0dc9bcbf05     imul     ecx, dword ptr [rip + 0x5bfbcc9]
0x144fc235b: 488b05cebcbf05     mov      rax, qword ptr [rip + 0x5bfbcce]
0x144fc2362: 83c10a             add      ecx, 0xa
0x144fc2365: 66833c48ff         cmp      word ptr [rax + rcx*2], -1
0x144fc236a: 7503               jne      0x144fc236f
0x144fc236c: 31c0               xor      eax, eax
0x144fc236e: c3                 ret      

; ---- function boundary 0x144fc2390 ----
0x144fc2390: 80b9d000000000     cmp      byte ptr [rcx + 0xd0], 0
0x144fc2397: 0f8413a31400       je       0x14510c6b0
0x144fc239d: 31c0               xor      eax, eax
0x144fc239f: c3                 ret      

