// SPDX-License-Identifier: GPL-3.0-or-later
// Known fault instructions and a known callee-saved register marker.
.text
.global tt_fault_write, tt_fault_write_site
.type tt_fault_write, @function
tt_fault_write:
    push %r12
    movabs $0x5454505346351234, %r12
tt_fault_write_site:
    mov %esi, (%rdi)
    pop %r12
    ret
.size tt_fault_write, .-tt_fault_write

.global tt_fault_read, tt_fault_read_site, tt_fault_read_resume
.type tt_fault_read, @function
tt_fault_read:
    push %r12
    movabs $0x5454505346351234, %r12
tt_fault_read_site:
    mov (%rdi), %eax
tt_fault_read_resume:
    pop %r12
    ret
.size tt_fault_read, .-tt_fault_read

.global tt_fault_read_xmm, tt_fault_xmm_site, tt_fault_xmm_resume
.type tt_fault_read_xmm, @function
tt_fault_read_xmm:
    push %r12
    movabs $0x5454505346351234, %r12
    movq %r12, %xmm0
    movq %r12, %xmm15
tt_fault_xmm_site:
    mov (%rdi), %eax
tt_fault_xmm_resume:
    movdqu %xmm0, (%rsi)
    movdqu %xmm15, 16(%rsi)
    pop %r12
    ret
.size tt_fault_read_xmm, .-tt_fault_read_xmm
.section .note.GNU-stack,"",@progbits
