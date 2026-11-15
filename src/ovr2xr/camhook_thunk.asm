; Assembly thunk for the CCamera copy hook.
;
; The C++ hook it replaces followed the x64 ABI exactly, and that was not enough.
; The ABI says RAX, RCX, RDX, R8-R11 and XMM0-XMM5 are the caller's problem, so
; a compiled function is free to destroy them - but the function we patched is a
; small unrolled copy that touches only RAX, and its callers were compiled
; alongside it. Whatever the compiler concluded about which registers survive a
; call to it, our C++ hook did not honour, and the result was the engine reading
; values it had left in XMM registers and finding ours: walls and props vanished,
; and eventually it crashed.
;
; That happened in OBSERVE mode, writing nothing to any camera - which is what
; ruled out the levelling maths and pointed here. A hook grafted into the middle
; of someone else's register allocation has to preserve everything, not just
; what the ABI requires.
;
; So this saves every volatile register around the callback, including the XMM
; set, and hands back RAX exactly as the original returned it.

.code

EXTERN dlvr_OnCameraCopied : PROC
EXTERN dlvr_CamCopyTrampoline : QWORD

; void dlvr_CamCopyThunk(void* dst /*rcx*/, const void* src /*rdx*/)
;
; Reached by a jump from the patched function entry, so the stack is exactly as
; the original saw it: a return address on top, RSP congruent to 8 mod 16.
; Subtracting 0D8h (also 8 mod 16) leaves RSP 16-aligned for the calls below.
dlvr_CamCopyThunk PROC
        sub     rsp, 0D8h

        mov     [rsp+20h], rcx          ; the arguments, for the callback later
        mov     [rsp+28h], rdx

        call    qword ptr [dlvr_CamCopyTrampoline]   ; the engine's own copy

        mov     [rsp+30h], rax          ; its return value must survive us
        mov     [rsp+38h], r8
        mov     [rsp+40h], r9
        mov     [rsp+48h], r10
        mov     [rsp+50h], r11
        movups  [rsp+60h], xmm0
        movups  [rsp+70h], xmm1
        movups  [rsp+80h], xmm2
        movups  [rsp+90h], xmm3
        movups  [rsp+0A0h], xmm4
        movups  [rsp+0B0h], xmm5

        mov     rcx, [rsp+20h]
        mov     rdx, [rsp+28h]
        call    dlvr_OnCameraCopied

        movups  xmm5, [rsp+0B0h]
        movups  xmm4, [rsp+0A0h]
        movups  xmm3, [rsp+90h]
        movups  xmm2, [rsp+80h]
        movups  xmm1, [rsp+70h]
        movups  xmm0, [rsp+60h]
        mov     r11, [rsp+50h]
        mov     r10, [rsp+48h]
        mov     r9,  [rsp+40h]
        mov     r8,  [rsp+38h]
        mov     rax, [rsp+30h]

        ; RCX and RDX too, and this is not belt-and-braces - the caller needs
        ; them. At engine+0x3F020:
        ;
        ;     call 0x3E9C0                 ; the function we hook
        ;     mov  r8d, [rdx+230h]         ; reads RDX after the call
        ;     mov  [rcx+230h], r8d         ; writes through RCX after the call
        ;
        ; The ABI says both are the caller's to lose, but this caller was
        ; compiled knowing the callee only touches RAX, so it keeps its pointers
        ; there across the call. Leaving them holding whatever the callback
        ; returned faulted at 0x3F033 every time delta mode wrote anything.
        ;
        ; Observe mode survived by luck: with nothing to do, the callback
        ; happened to leave its first argument - the destination pointer - still
        ; in RCX, which is exactly what the caller expected to find.
        mov     rcx, [rsp+20h]
        mov     rdx, [rsp+28h]

        add     rsp, 0D8h
        ret
dlvr_CamCopyThunk ENDP

END
