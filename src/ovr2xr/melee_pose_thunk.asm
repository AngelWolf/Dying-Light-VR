; Assembly thunk for the melee POSE observation hook at gamedll+0xDC1C90.
;
; WHAT THIS FUNCTION IS. sub_180DC1C90 answers "where was the weapon at time t"
; once per sub-step of the melee sweep, and INV-129 established that every
; vertex of the swept quad chain except the seed comes from it - it reads the
; named animation helpers "AttackHelper"/"HandHelper" through
; IModelObject::GetAnimBind, NOT the live element. INV-130 established that its
; out-parameter is a 12-float row-major mtx34 with the translation at indices
; 3/7/11, and that the parameter is IN/OUT: the caller seeds a matrix and this
; function composes the helper offset onto it.
;
; Jay chose interception point (a) - substituting this function - so this hook
; is the OBSERVATION step before anything substitutes anything. IT WRITES
; NOTHING. It reads four arguments and returns.
;
; WHY IT TAIL-JUMPS, and this is copied from melee_thunk.asm because the reason
; is identical and it is not a style choice. The stolen prologue is
;       mov  rax, rsp
;       mov  [rax+20h], rbx
;       mov  [rax+18h], r8
;       mov  [rax+10h], rdx
; - the standard MSVC home-register spill, which writes into the CALLER'S shadow
; space using RSP. Reached through a CALL, RSP would be 8 lower and all three
; spills would land one slot off, silently. A tail JMP hands the trampoline
; exactly the RSP the original function was entered with.
;
; WHY WE OBSERVE ON ENTRY AND NOT ON RETURN. A tail jump never regains control,
; and making it a call to regain control is exactly the trap above. The output
; is not lost by this: the caller writes that sub-step's start straight into the
; hit record at +0x00, which recordprobe's [QUAD] line already logs every frame.
; So entry gives the INPUT matrix, the time and the anim id, and the existing
; probe gives the OUTPUT - both sides, with no fragile stack arithmetic.
;
; WHAT IT PRESERVES: everything the ABI calls volatile, for the reason
; camhook_thunk.asm records - a caller compiled alongside its callee keeps live
; values in volatile registers anyway, and this hook is grafted into the middle
; of someone else's register allocation.
;
; ARGUMENTS AT ENTRY (MS x64), and the fifth one is the reason for the offsets
; below. sub_180DC1C90(a1 rcx, a2 rdx, a3 r8, a4 r9, a5 xmm at [rsp+28h]):
;   rdx = a2, the IN/OUT mtx34          -> our arg 1
;   r8  = a3, the anim-id pointer       -> our arg 2
;   r9  = a4                            -> our arg 3
;   [rsp+28h] = a5, the TIME as float   -> our arg 4, in xmm3
; Reached by a JMP, so [rsp] is the return address and the fifth argument sits
; at [rsp+28h]. After `sub rsp, 0D8h` it is at [rsp+0D8h+28h] = [rsp+100h].

.code

EXTERN dlvr_MeleePoseObserve : PROC
EXTERN dlvr_MeleePoseTrampoline : QWORD

dlvr_MeleePoseThunk PROC
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

        movss   xmm3, dword ptr [rsp+100h]   ; a5, the time - read BEFORE we clobber
        mov     r8,  r9                      ; arg3 = a4
        mov     rcx, rdx                     ; arg1 = a2, the mtx34
        mov     rdx, [rsp+38h]               ; arg2 = a3, the anim-id pointer
        call    dlvr_MeleePoseObserve

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
        ; is what the stolen prologue's shadow-space spills require.
        jmp     qword ptr [dlvr_MeleePoseTrampoline]
dlvr_MeleePoseThunk ENDP

END
