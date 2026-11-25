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
