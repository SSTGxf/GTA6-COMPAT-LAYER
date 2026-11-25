; image fh2test/forza_durango_release_final.exe size=58204160 imagebase=0x140000000 size_of_image_end=0x3a17800
; section table (name, VA, VSize, RawPtr, RawSize):
;   .text     VA=0x00001000 VSize=0x01db300f RawPtr=0x00000600 RawSize=0x01db3200
;   BINK      VA=0x01db5000 VSize=0x000193b6 RawPtr=0x01db3800 RawSize=0x00019400
;   RADCODE   VA=0x01dcf000 VSize=0x000006d6 RawPtr=0x01dccc00 RawSize=0x00000800
;   .rdata    VA=0x01dd0000 VSize=0x00e76f20 RawPtr=0x01dcd400 RawSize=0x00e77000
;   .data     VA=0x02c47000 VSize=0x00af2568 RawPtr=0x02c44400 RawSize=0x00864a00
;   .pdata    VA=0x0373a000 VSize=0x001f2e28 RawPtr=0x034a8e00 RawSize=0x001f3000
;   minATL    VA=0x0392d000 VSize=0x00000020 RawPtr=0x0369be00 RawSize=0x00000200
;   .tls      VA=0x0392e000 VSize=0x00000151 RawPtr=0x0369c000 RawSize=0x00000200
;   _RDATA    VA=0x0392f000 VSize=0x00002ff0 RawPtr=0x0369c200 RawSize=0x00003000
;   BINKDATA  VA=0x03932000 VSize=0x00000130 RawPtr=0x0369f200 RawSize=0x00000200
;   BINKCONS  VA=0x03933000 VSize=0x00002000 RawPtr=0x0369f400 RawSize=0x00002000
;   RADDATA   VA=0x03935000 VSize=0x0000000c RawPtr=0x036a1400 RawSize=0x00000200
;   .rsrc     VA=0x03936000 VSize=0x000001e0 RawPtr=0x036a1600 RawSize=0x00000200
;   .reloc    VA=0x03937000 VSize=0x000e07b0 RawPtr=0x036a1800 RawSize=0x000e0800
;
; RVA for the gate 0x14407e700 is 0x407e700 if the module base is 0x140000000.
; 0x407e700 > last section end 0x3a17800, so that RVA lies OUTSIDE this image.
; Therefore the guest gate 0x14407e700 does not live in this PE; it is mapped at
; a base 0x144000000 that is 0x4000000 above the PE image base and beyond its size,
; i.e. in a separately materialised region (allocated/JIT/shim), not in the file.

; ---- live slice dumped from boot #269 (build_ga261) ----
; base arithmetic: region base 0x144000000 + offset 0x7e700 = 0x14407e700
; bytes=512 (out/gate_14407e700.bin), gate-slice log line: 'gate-slice wrote ... base=0x144000000+0x7e700 bytes=512'
0x14407e700: 4157                   push r15
0x14407e702: 4156                   push r14
0x14407e704: 4155                   push r13
0x14407e706: 4154                   push r12
0x14407e708: 56                     push rsi
0x14407e709: 57                     push rdi
0x14407e70a: 55                     push rbp
0x14407e70b: 53                     push rbx
0x14407e70c: 4883ec68               sub rsp, 0x68
0x14407e710: 4c8bac24d8000000       mov r13, qword ptr [rsp + 0xd8]
0x14407e718: 4889ce                 mov rsi, rcx
0x14407e71b: 4d85ed                 test r13, r13
0x14407e71e: 7408                   je 0x14407e728
0x14407e720: 41c7450000000000       mov dword ptr [r13], 0
0x14407e728: 4584c9                 test r9b, r9b
0x14407e72b: 7404                   je 0x14407e731
0x14407e72d: 48895610               mov qword ptr [rsi + 0x10], rdx
0x14407e731: 4885d2                 test rdx, rdx
0x14407e734: 0f95c0                 setne al
0x14407e737: 4585c0                 test r8d, r8d
0x14407e73a: 0f95c1                 setne cl
0x14407e73d: 84c8                   test al, cl
0x14407e73f: 7516                   jne 0x14407e757
0x14407e741: 4d85ed                 test r13, r13
0x14407e744: 0f846a040000           je 0x14407ebb4
0x14407e74a: 41c7450002000000       mov dword ptr [r13], 2
0x14407e752: e95d040000             jmp 0x14407ebb4
0x14407e757: 440f38f07204           movbe r14d, dword ptr [rdx + 4]
0x14407e75d: 488bac24d0000000       mov rbp, qword ptr [rsp + 0xd0]
0x14407e765: 4539c6                 cmp r14d, r8d
0x14407e768: 7615                   jbe 0x14407e77f
0x14407e76a: 4531d2                 xor r10d, r10d
0x14407e76d: 4531db                 xor r11d, r11d
0x14407e770: 31c0                   xor eax, eax
0x14407e772: 4531e4                 xor r12d, r12d
0x14407e775: 31ff                   xor edi, edi
0x14407e777: 4531f6                 xor r14d, r14d
0x14407e77a: e91b040000             jmp 0x14407eb9a
0x14407e77f: 0f38f00a               movbe ecx, dword ptr [rdx]
0x14407e783: 4489c5                 mov ebp, r8d
0x14407e786: 4a8d1c32               lea rbx, [rdx + r14]
0x14407e78a: 4801d5                 add rbp, rdx
0x14407e78d: 81f947435350           cmp ecx, 0x50534347
0x14407e793: 7f4a                   jg 0x14407e7df
0x14407e795: 81f94253524c           cmp ecx, 0x4c525342
0x14407e79b: 0f8fb5000000           jg 0x14407e856
0x14407e7a1: 81f94c4e454c           cmp ecx, 0x4c454e4c
0x14407e7a7: 0f8f30010000           jg 0x14407e8dd
0x14407e7ad: 81f9534b4843           cmp ecx, 0x43484b53
0x14407e7b3: 0f847a020000           je 0x14407ea33
0x14407e7b9: 81f944495547           cmp ecx, 0x47554944
0x14407e7bf: 0f85ff090000           jne 0x14407f1c4
0x14407e7c5: 488d4208               lea rax, [rdx + 8]
0x14407e7c9: 418d4ef8               lea ecx, [r14 - 8]
0x14407e7cd: 48898680000000         mov qword ptr [rsi + 0x80], rax
0x14407e7d4: 898e88000000           mov dword ptr [rsi + 0x88], ecx
0x14407e7da: e93a030000             jmp 0x14407eb19
0x14407e7df: 4531d2                 xor r10d, r10d
0x14407e7e2: 4531ff                 xor r15d, r15d
0x14407e7e5: 81f94d465453           cmp ecx, 0x5354464d
0x14407e7eb: 0f8fa3000000           jg 0x14407e894
0x14407e7f1: 81f94d495350           cmp ecx, 0x5053494d
0x14407e7f7: 0f8f12010000           jg 0x14407e90f
0x14407e7fd: 81f948435350           cmp ecx, 0x50534348
0x14407e803: 0f844e020000           je 0x14407ea57
0x14407e809: 41b900000000           mov r9d, 0
0x14407e80f: b800000000             mov eax, 0
0x14407e814: 41bc00000000           mov r12d, 0
0x14407e81a: bf00000000             mov edi, 0
0x14407e81f: 4c894c2428             mov qword ptr [rsp + 0x28], r9
0x14407e824: 41b900000000           mov r9d, 0
0x14407e82a: 4c894c2430             mov qword ptr [rsp + 0x30], r9
0x14407e82f: 41b900000000           mov r9d, 0
0x14407e835: 4c894c2440             mov qword ptr [rsp + 0x40], r9
0x14407e83a: 41b900000000           mov r9d, 0
0x14407e840: 4c894c2438             mov qword ptr [rsp + 0x38], r9
0x14407e845: 81f947495350           cmp ecx, 0x50534947
0x14407e84b: 0f84f9020000           je 0x14407eb4a
0x14407e851: e96e090000             jmp 0x14407f1c4
0x14407e856: 81f9524e534c           cmp ecx, 0x4c534e52
0x14407e85c: 0f8f06010000           jg 0x14407e968
0x14407e862: 81f94353524c           cmp ecx, 0x4c525343
0x14407e868: 0f8425020000           je 0x14407ea93
0x14407e86e: 81f94843534c           cmp ecx, 0x4c534348
0x14407e874: 0f854a090000           jne 0x14407f1c4
0x14407e87a: 488d4208               lea rax, [rdx + 8]
0x14407e87e: 418d4ef8               lea ecx, [r14 - 8]
0x14407e882: 488986b0000000         mov qword ptr [rsi + 0xb0], rax
0x14407e889: 898eb8000000           mov dword ptr [rsi + 0xb8], ecx
0x14407e88f: e985020000             jmp 0x14407eb19
0x14407e894: 81f944525453           cmp ecx, 0x53545244
0x14407e89a: 0f8f14010000           jg 0x14407e9b4
0x14407e8a0: 81f94e465453           cmp ecx, 0x5354464e
0x14407e8a6: 0f84fe010000           je 0x14407eaaa
0x14407e8ac: 81f9534e5453           cmp ecx, 0x53544e53
0x14407e8b2: 0f850c090000           jne 0x14407f1c4
0x14407e8b8: 488b8424d0000000       mov rax, qword ptr [rsp + 0xd0]
0x14407e8c0: f60002                 test byte ptr [rax], 2
0x14407e8c3: 0f8550020000           jne 0x14407eb19
0x14407e8c9: 488d4208               lea rax, [rdx + 8]
0x14407e8cd: 418d4ef8               lea ecx, [r14 - 8]
0x14407e8d1: 48894670               mov qword ptr [rsi + 0x70], rax
0x14407e8d5: 894e78                 mov dword ptr [rsi + 0x78], ecx
0x14407e8d8: e93c020000             jmp 0x14407eb19
0x14407e8dd: 81f94d4e454c           cmp ecx, 0x4c454e4d
0x14407e8e3: 0f84df010000           je 0x14407eac8
0x14407e8e9: 81f94449474c           cmp ecx, 0x4c474944
0x14407e8ef: 0f85cf080000           jne 0x14407f1c4
0x14407e8f5: 488d4208               lea rax, [rdx + 8]
0x14407e8f9: 418d4ef8               lea ecx, [r14 - 8]

; ==== full slice decode (512 bytes) ====
; region base 0x144000000 + offset 0x7e700 = slice[0] @ 0x14407e700
; annotations: cmp/test/jcc that can steer the false return
0x14407e700: 4157                     push r15
0x14407e702: 4156                     push r14
0x14407e704: 4155                     push r13
0x14407e706: 4154                     push r12
0x14407e708: 56                       push rsi
0x14407e709: 57                       push rdi
0x14407e70a: 55                       push rbp
0x14407e70b: 53                       push rbx
0x14407e70c: 4883ec68                 sub rsp, 0x68
0x14407e710: 4c8bac24d8000000         mov r13, qword ptr [rsp + 0xd8]
0x14407e718: 4889ce                   mov rsi, rcx
0x14407e71b: 4d85ed                   test r13, r13
0x14407e71e: 7408                     je 0x14407e728
0x14407e720: 41c7450000000000         mov dword ptr [r13], 0
0x14407e728: 4584c9                   test r9b, r9b
0x14407e72b: 7404                     je 0x14407e731
0x14407e72d: 48895610                 mov qword ptr [rsi + 0x10], rdx
0x14407e731: 4885d2                   test rdx, rdx
0x14407e734: 0f95c0                   setne al
0x14407e737: 4585c0                   test r8d, r8d
0x14407e73a: 0f95c1                   setne cl
0x14407e73d: 84c8                     test al, cl
0x14407e73f: 7516                     jne 0x14407e757
0x14407e741: 4d85ed                   test r13, r13
0x14407e744: 0f846a040000             je 0x14407ebb4
0x14407e74a: 41c7450002000000         mov dword ptr [r13], 2
0x14407e752: e95d040000               jmp 0x14407ebb4
0x14407e757: 440f38f07204             movbe r14d, dword ptr [rdx + 4]
0x14407e75d: 488bac24d0000000         mov rbp, qword ptr [rsp + 0xd0]
0x14407e765: 4539c6                   cmp r14d, r8d
0x14407e768: 7615                     jbe 0x14407e77f
0x14407e76a: 4531d2                   xor r10d, r10d
0x14407e76d: 4531db                   xor r11d, r11d
0x14407e770: 31c0                     xor eax, eax
0x14407e772: 4531e4                   xor r12d, r12d
0x14407e775: 31ff                     xor edi, edi
0x14407e777: 4531f6                   xor r14d, r14d
0x14407e77a: e91b040000               jmp 0x14407eb9a
0x14407e77f: 0f38f00a                 movbe ecx, dword ptr [rdx]
0x14407e783: 4489c5                   mov ebp, r8d
0x14407e786: 4a8d1c32                 lea rbx, [rdx + r14]
0x14407e78a: 4801d5                   add rbp, rdx
0x14407e78d: 81f947435350             cmp ecx, 0x50534347
0x14407e793: 7f4a                     jg 0x14407e7df
0x14407e795: 81f94253524c             cmp ecx, 0x4c525342
0x14407e79b: 0f8fb5000000             jg 0x14407e856
0x14407e7a1: 81f94c4e454c             cmp ecx, 0x4c454e4c
0x14407e7a7: 0f8f30010000             jg 0x14407e8dd
0x14407e7ad: 81f9534b4843             cmp ecx, 0x43484b53
0x14407e7b3: 0f847a020000             je 0x14407ea33
0x14407e7b9: 81f944495547             cmp ecx, 0x47554944
0x14407e7bf: 0f85ff090000             jne 0x14407f1c4
0x14407e7c5: 488d4208                 lea rax, [rdx + 8]
0x14407e7c9: 418d4ef8                 lea ecx, [r14 - 8]
0x14407e7cd: 48898680000000           mov qword ptr [rsi + 0x80], rax
0x14407e7d4: 898e88000000             mov dword ptr [rsi + 0x88], ecx
0x14407e7da: e93a030000               jmp 0x14407eb19
0x14407e7df: 4531d2                   xor r10d, r10d
0x14407e7e2: 4531ff                   xor r15d, r15d
0x14407e7e5: 81f94d465453             cmp ecx, 0x5354464d
0x14407e7eb: 0f8fa3000000             jg 0x14407e894
0x14407e7f1: 81f94d495350             cmp ecx, 0x5053494d
0x14407e7f7: 0f8f12010000             jg 0x14407e90f
0x14407e7fd: 81f948435350             cmp ecx, 0x50534348
0x14407e803: 0f844e020000             je 0x14407ea57
0x14407e809: 41b900000000             mov r9d, 0
0x14407e80f: b800000000               mov eax, 0
0x14407e814: 41bc00000000             mov r12d, 0
0x14407e81a: bf00000000               mov edi, 0
0x14407e81f: 4c894c2428               mov qword ptr [rsp + 0x28], r9
0x14407e824: 41b900000000             mov r9d, 0
0x14407e82a: 4c894c2430               mov qword ptr [rsp + 0x30], r9
0x14407e82f: 41b900000000             mov r9d, 0
0x14407e835: 4c894c2440               mov qword ptr [rsp + 0x40], r9
0x14407e83a: 41b900000000             mov r9d, 0
0x14407e840: 4c894c2438               mov qword ptr [rsp + 0x38], r9
0x14407e845: 81f947495350             cmp ecx, 0x50534947
0x14407e84b: 0f84f9020000             je 0x14407eb4a
0x14407e851: e96e090000               jmp 0x14407f1c4
0x14407e856: 81f9524e534c             cmp ecx, 0x4c534e52
0x14407e85c: 0f8f06010000             jg 0x14407e968
0x14407e862: 81f94353524c             cmp ecx, 0x4c525343
0x14407e868: 0f8425020000             je 0x14407ea93
0x14407e86e: 81f94843534c             cmp ecx, 0x4c534348
0x14407e874: 0f854a090000             jne 0x14407f1c4
0x14407e87a: 488d4208                 lea rax, [rdx + 8]
0x14407e87e: 418d4ef8                 lea ecx, [r14 - 8]
0x14407e882: 488986b0000000           mov qword ptr [rsi + 0xb0], rax
0x14407e889: 898eb8000000             mov dword ptr [rsi + 0xb8], ecx
0x14407e88f: e985020000               jmp 0x14407eb19
0x14407e894: 81f944525453             cmp ecx, 0x53545244
0x14407e89a: 0f8f14010000             jg 0x14407e9b4
0x14407e8a0: 81f94e465453             cmp ecx, 0x5354464e
0x14407e8a6: 0f84fe010000             je 0x14407eaaa
0x14407e8ac: 81f9534e5453             cmp ecx, 0x53544e53
0x14407e8b2: 0f850c090000             jne 0x14407f1c4
0x14407e8b8: 488b8424d0000000         mov rax, qword ptr [rsp + 0xd0]
0x14407e8c0: f60002                   test byte ptr [rax], 2
0x14407e8c3: 0f8550020000             jne 0x14407eb19
0x14407e8c9: 488d4208                 lea rax, [rdx + 8]
0x14407e8cd: 418d4ef8                 lea ecx, [r14 - 8]
0x14407e8d1: 48894670                 mov qword ptr [rsi + 0x70], rax
0x14407e8d5: 894e78                   mov dword ptr [rsi + 0x78], ecx
0x14407e8d8: e93c020000               jmp 0x14407eb19
0x14407e8dd: 81f94d4e454c             cmp ecx, 0x4c454e4d
0x14407e8e3: 0f84df010000             je 0x14407eac8
0x14407e8e9: 81f94449474c             cmp ecx, 0x4c474944
0x14407e8ef: 0f85cf080000             jne 0x14407f1c4
0x14407e8f5: 488d4208                 lea rax, [rdx + 8]
0x14407e8f9: 418d4ef8                 lea ecx, [r14 - 8]
; ==== compares/branches only ====
; 0x14407e71b: 4d85ed                   test r13, r13
; 0x14407e71e: 7408                     je 0x14407e728
; 0x14407e728: 4584c9                   test r9b, r9b
; 0x14407e72b: 7404                     je 0x14407e731
; 0x14407e731: 4885d2                   test rdx, rdx
; 0x14407e734: 0f95c0                   setne al
; 0x14407e737: 4585c0                   test r8d, r8d
; 0x14407e73a: 0f95c1                   setne cl
; 0x14407e73d: 84c8                     test al, cl
; 0x14407e73f: 7516                     jne 0x14407e757
; 0x14407e741: 4d85ed                   test r13, r13
; 0x14407e744: 0f846a040000             je 0x14407ebb4
; 0x14407e765: 4539c6                   cmp r14d, r8d
; 0x14407e768: 7615                     jbe 0x14407e77f
; 0x14407e78d: 81f947435350             cmp ecx, 0x50534347
; 0x14407e793: 7f4a                     jg 0x14407e7df
; 0x14407e795: 81f94253524c             cmp ecx, 0x4c525342
; 0x14407e79b: 0f8fb5000000             jg 0x14407e856
; 0x14407e7a1: 81f94c4e454c             cmp ecx, 0x4c454e4c
; 0x14407e7a7: 0f8f30010000             jg 0x14407e8dd
; 0x14407e7ad: 81f9534b4843             cmp ecx, 0x43484b53
; 0x14407e7b3: 0f847a020000             je 0x14407ea33
; 0x14407e7b9: 81f944495547             cmp ecx, 0x47554944
; 0x14407e7bf: 0f85ff090000             jne 0x14407f1c4
; 0x14407e7e5: 81f94d465453             cmp ecx, 0x5354464d
; 0x14407e7eb: 0f8fa3000000             jg 0x14407e894
; 0x14407e7f1: 81f94d495350             cmp ecx, 0x5053494d
; 0x14407e7f7: 0f8f12010000             jg 0x14407e90f
; 0x14407e7fd: 81f948435350             cmp ecx, 0x50534348
; 0x14407e803: 0f844e020000             je 0x14407ea57
; 0x14407e845: 81f947495350             cmp ecx, 0x50534947
; 0x14407e84b: 0f84f9020000             je 0x14407eb4a
; 0x14407e856: 81f9524e534c             cmp ecx, 0x4c534e52
; 0x14407e85c: 0f8f06010000             jg 0x14407e968
; 0x14407e862: 81f94353524c             cmp ecx, 0x4c525343
; 0x14407e868: 0f8425020000             je 0x14407ea93
; 0x14407e86e: 81f94843534c             cmp ecx, 0x4c534348
; 0x14407e874: 0f854a090000             jne 0x14407f1c4
; 0x14407e894: 81f944525453             cmp ecx, 0x53545244
; 0x14407e89a: 0f8f14010000             jg 0x14407e9b4
; 0x14407e8a0: 81f94e465453             cmp ecx, 0x5354464e
; 0x14407e8a6: 0f84fe010000             je 0x14407eaaa
; 0x14407e8ac: 81f9534e5453             cmp ecx, 0x53544e53
; 0x14407e8b2: 0f850c090000             jne 0x14407f1c4
; 0x14407e8c0: f60002                   test byte ptr [rax], 2
; 0x14407e8c3: 0f8550020000             jne 0x14407eb19
; 0x14407e8dd: 81f94d4e454c             cmp ecx, 0x4c454e4d
; 0x14407e8e3: 0f84df010000             je 0x14407eac8
; 0x14407e8e9: 81f94449474c             cmp ecx, 0x4c474944
; 0x14407e8ef: 0f85cf080000             jne 0x14407f1c4
