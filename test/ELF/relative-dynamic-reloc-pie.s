# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld -pie %t.o -o %t.pie
# RUN: llvm-readobj -r -dyn-symbols %t.pie | FileCheck %s

## Test that we create R_X86_64_RELATIVE relocations with -pie.
# CHECK:      Relocations [
# CHECK-NEXT:   Section ({{.*}}) .rela.dyn {
# CHECK-NEXT:     0x1001 R_X86_64_RELATIVE - 0x1001
# CHECK-NEXT:     0x1009 R_X86_64_RELATIVE - 0x1009
# CHECK-NEXT:     0x1011 R_X86_64_RELATIVE - 0x100A
# CHECK-NEXT:   }
# CHECK-NEXT: ]

.globl _start
_start:
nop

foo:
 .quad foo

.hidden bar
.global bar
bar:
 .quad bar
 .quad bar + 1
