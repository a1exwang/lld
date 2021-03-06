// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t.o
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %p/Inputs/comdat.s -o %t2.o
// RUN: ld.lld -shared %t.o %t.o %t2.o -o %t
// RUN: llvm-objdump -d %t | FileCheck %s
// RUN: llvm-readobj -s -t %t | FileCheck --check-prefix=READ %s
// REQUIRES: x86

        .section	.text2,"axG",@progbits,foo,comdat,unique,0
foo:
        nop

// CHECK: Disassembly of section .text2:
// CHECK-NEXT: foo:
// CHECK-NEXT:   1000: {{.*}}  nop
// CHECK-NOT: nop

        .section bar, "ax"
        call foo

// CHECK: Disassembly of section bar:
// CHECK-NEXT: bar:
// 0x1000 - 0x1001 - 5 = -6
// 0      - 0x1006 - 5 = -4107
// CHECK-NEXT:   1001:	{{.*}}  callq  -6
// CHECK-NEXT:   1006:	{{.*}}  callq  -4107

        .section .text3,"axG",@progbits,zed,comdat,unique,0


// READ:      Name: .text2
// READ-NEXT: Type: SHT_PROGBITS
// READ-NEXT: Flags [
// READ-NEXT:   SHF_ALLOC
// READ-NEXT:   SHF_EXECINSTR
// READ-NEXT: ]

// READ:      Name: .text3
// READ-NEXT: Type: SHT_PROGBITS
// READ-NEXT: Flags [
// READ-NEXT:   SHF_ALLOC
// READ-NEXT:   SHF_EXECINSTR
// READ-NEXT: ]

// READ:      Symbols [
// READ-NEXT:   Symbol {
// READ-NEXT:     Name:  (0)
// READ-NEXT:     Value: 0x0
// READ-NEXT:     Size: 0
// READ-NEXT:     Binding: Local
// READ-NEXT:     Type: None
// READ-NEXT:     Other: 0
// READ-NEXT:     Section: Undefined
// READ-NEXT:   }
// READ-NEXT:   Symbol {
// READ-NEXT:     Name: foo
// READ-NEXT:     Value
// READ-NEXT:     Size: 0
// READ-NEXT:     Binding: Local
// READ-NEXT:     Type: None
// READ-NEXT:     Other: 0
// READ-NEXT:     Section: .text
// READ-NEXT:   }
// READ-NEXT:   Symbol {
// READ-NEXT:     Name: _DYNAMIC
// READ-NEXT:     Value: 0x2000
// READ-NEXT:     Size: 0
// READ-NEXT:     Binding: Local
// READ-NEXT:     Type: None
// READ-NEXT:     Other [ (0x2)
// READ-NEXT:       STV_HIDDEN
// READ-NEXT:     ]
// READ-NEXT:     Section: .dynamic
// READ-NEXT:   }
// READ-NEXT:   Symbol {
// READ-NEXT:     Name: abc
// READ-NEXT:     Value: 0x0
// READ-NEXT:     Size: 0
// READ-NEXT:     Binding: Global
// READ-NEXT:     Type: None
// READ-NEXT:     Other: 0
// READ-NEXT:     Section: Undefined
// READ-NEXT:   }
// READ-NEXT: ]
