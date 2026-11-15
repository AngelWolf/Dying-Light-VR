; Assembly thunk for the melee element hook at gamedll+0xC4D920.
;
; WHY AN INLINE HOOK AT ALL. The element poke has always ridden a DR1 execution
; breakpoint, and INV-120 measured what that costs: six of six in-play crashes
; on engine+0x8CD6F8 had the trap armed, THREE of them with the write off
; entirely, and no session with diag_melee_elem_probe=0 has ever produced that
; face. U-27 holds the causal question open - there is no mechanism - so this
; exists to SETTLE it by removing the debug register while keeping the write.
; D-57 named the shape: "a different interception mechanism, not a different
; setting."
;
; WHY IT TAIL-JUMPS INSTEAD OF CALLING THE TRAMPOLINE, and this is not a style
; choice. The stolen prologue is
;       mov  rax, rsp
;       mov  [rax+20h], r9
;       mov  [rax+18h], r8
;       mov  [rax+8],  rcx
; - the standard MSVC home-register spill, which writes the incoming arguments
; into the CALLER'S shadow space using RSP. If we reached it through a CALL,
; RSP would be 8 lower and all three spills would land in the wrong place, one
; slot off, silently. A tail JMP hands the trampoline exactly the RSP the
; original function was entered with.
;
; WHAT IT PRESERVES, and why more than the ABI demands: camhook_thunk.asm's
; comment records the lesson in full. The ABI says RAX, RCX, RDX, R8-R11 and
; XMM0-XMM5 belong to the caller - but a caller compiled alongside its callee
; keeps live values in them anyway, and our hook is grafted into the middle of
; someone else's register allocation. That cost walls and props vanishing, then
; a crash, in OBSERVE mode. So: everything.

.code

EXTERN dlvr_MeleeElemPoke : PROC
EXTERN dlvr_MeleeElemTrampoline : QWORD

; void dlvr_MeleeElemThunk(...)
;
; Reached by a jump from the patched function entry, so the stack is exactly as
; the original saw it: a return address on top, RSP congruent to 8 mod 16.
; Subtracting 0D8h (also 8 mod 16) leaves RSP 16-aligned for the call below.
;
; R8 is the contact record - the third argument, [R8+8] being the element the
; poke rewrites (INV-65, U-24). It is read BEFORE anything can clobber it.
dlvr_MeleeElemThunk PROC
        sub     rsp, 0D8h

        mov     [rsp+20h], rax
        mov     [rsp+28h], rcx
        mov     [rsp+30h], rdx
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

        mov     rcx, r8                 ; pbase = the contact record
        call    dlvr_MeleeElemPoke

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
        mov     rdx, [rsp+30h]
        mov     rcx, [rsp+28h]
        mov     rax, [rsp+20h]

        add     rsp, 0D8h

        ; RSP is now exactly what the original function was entered with, which
        ; is what the stolen prologue's shadow-space spills require. The
        ; trampoline runs those four instructions and jumps to target+15; the
        ; original function then returns straight to the game's caller, with no
        ; frame of ours left on the stack.
        jmp     qword ptr [dlvr_MeleeElemTrampoline]
dlvr_MeleeElemThunk ENDP

END
