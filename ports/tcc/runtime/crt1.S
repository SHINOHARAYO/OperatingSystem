.text
.global _start
.type _start, %function
_start:
    bl main
    mov w19, w0
    bl terminal_flush
    mov w0, w19
    mov x8, #8
    svc #0
1:
    b 1b
