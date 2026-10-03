; Sketch - a tiny VMU game written for web-vmu, so there is something to run that is ours to give away.
; Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
;
;   D-pad  move the pen (it draws as it goes)
;   A      wipe the screen
;   B      save the picture to the memory card (it comes back the next time the game starts)
;   MODE   leave the game
;
; Build:  python3 tools/vmuasm.py demo/sketch.s -o web/demo/sketch.vms --header esp32/src/demo_game.h
; It uses the screen, the buttons, the buzzer, the clock interrupt and the flash-write service,
; which is everything a VMU game normally touches.

; ---- variables (RAM bank 1, which is where a game starts out)
x      = $10        ; pen position, 0-47
y      = $11        ; 0-31
tmp    = $12
count  = $13
beep   = $14        ; frames of tone left
bheld  = $15        ; B was already down last frame
r0     = $00        ; the @R0 and @R2 pointer registers
r2     = $02
fmbank = $7d        ; flash address for the firmware's write service ...
fmhigh = $7e
fmlow  = $7f
fmdata = $80        ; ... and its 128 bytes of data

SAVEAREA   = $600       ; the game's last block is its save area: two 128-byte pages, one per screen half

; ---- interrupt vectors
  .org $00
  jmpf start
  .org $03
  reti
  .org $0b
  reti
  .org $13
  reti
  .org $1b
  jmp clocktick     ; base timer, twice a second
  .org $23
  reti
  .org $2b
  reti
  .org $33
  reti
  .org $3b
  reti
  .org $43
  reti
  .org $4b
  clr1 p3int,1
  reti

; ---- firmware services: flip to the system ROM and jump; it comes back to the instruction after the jmpf
  .org $100
fm_write:
  not1 ext,0
  jmpf fm_write
  ret

  .org $130
clocktick:
  push ie
  clr1 ie,7
  not1 ext,0
  jmpf clocktick
  pop ie
  reti

  .org $1f0
goodbye:
  not1 ext,0
  jmpf goodbye

; ---- file header: what the VMU and the console show for this game
  .org $200
  .text 16 "Sketch"
  .text 32 "Sketch, a test game for web-vmu"
  .text 16 "web-vmu"
  .byte 1,0         ; one icon
  .byte 10,0        ; animation speed
  .byte 0,0         ; no eyecatch picture
  .byte 0,0         ; checksum (not used for games)
  .byte 0,0,0,0     ; data size (not used for games)
  .org $260         ; 16 colours, 4 bits each of alpha, red, green, blue
  .byte $00,$00, $ee,$fe, $45,$f4, $b8,$f9, $32,$f2, $c3,$ff
  .org $280
  .icon "00000000000000000000000000000000"
  .icon "00000111111111111111111111100000"
  .icon "00001111111111111111111111110000"
  .icon "00001111111111111111111111110000"
  .icon "00001112222222222222222221110000"
  .icon "00001112333333333333333321110000"
  .icon "00001112333333333333333321110000"
  .icon "00001112333333333333334321110000"
  .icon "00001112333334433333343321110000"
  .icon "00001112333343343333433321110000"
  .icon "00001112333433334334333321110000"
  .icon "00001112334333333443333321110000"
  .icon "00001112343333333333333321110000"
  .icon "00001112333333333333333321110000"
  .icon "00001112333333333333333321110000"
  .icon "00001112222222222222222221110000"
  .icon "00001111111111111111111111110000"
  .icon "00001111111111111111111111110000"
  .icon "00001111111111122122111111110000"
  .icon "00001111111111111111111111110000"
  .icon "00001111122211111111111111110000"
  .icon "00001111122211111111115551110000"
  .icon "00001112222222111111115551110000"
  .icon "00001112222222111111115551110000"
  .icon "00001112222222111155511111110000"
  .icon "00001111122211111155511111110000"
  .icon "00001111122211111155511111110000"
  .icon "00001111111111111111111111110000"
  .icon "00001111111111111111111111110000"
  .icon "00001111111111111111111111110000"
  .icon "00000111111111111111111111100000"
  .icon "00000000000000000000000000000000"

; ---- the game
  .org $480
start:
  clr1 ie,7
  mov #$81,ocr      ; run from the fast RC oscillator
  mov #$09,mcr      ; screen refresh on
  mov #$80,vccr     ; screen on
  clr1 p3int,0      ; no button interrupt: the buttons are polled
  clr1 p1,7         ; buzzer pin ready
  mov #$ff,p3       ; pull-ups on the button pins
  mov #0,t1cnt
  mov #0,beep
  mov #0,bheld
  set1 ie,7

  mov #0,xbnk       ; bring back the saved picture
  mov #<SAVEAREA,trl
  call loadpage
  mov #1,xbnk
  mov #<SAVEAREA+$80,trl
  call loadpage
  mov #24,x
  mov #16,y

main:
  call plot
  call wait

  ld beep           ; stop the tone when its time is up
  bz .quiet
  dec beep
  ld beep
  bnz .quiet
  mov #0,t1cnt
.quiet:

  bp p3,6,.stay     ; MODE: back to the VMU's own menu
  jmp goodbye
.stay:
  bp p3,0,.notup
  ld y
  bz .notup
  dec y
.notup:
  bp p3,1,.notdown
  ld y
  sub #31
  bz .notdown
  inc y
.notdown:
  bp p3,2,.notleft
  ld x
  bz .notleft
  dec x
.notleft:
  bp p3,3,.notright
  ld x
  sub #47
  bz .notright
  inc x
.notright:
  bp p3,4,.nota
  call wipe
  mov #$a0,t1lr     ; a low note
  mov #$d0,t1lc
  mov #$50,t1cnt
  mov #3,beep
.nota:
  bp p3,5,.bup
  ld bheld
  bnz .again
  mov #1,bheld
  call save
  mov #$e0,t1lr     ; a high note
  mov #$f0,t1lc
  mov #$50,t1cnt
  mov #6,beep
  br .again
.bup:
  mov #0,bheld
.again:
  jmp main

; set the pixel at (x, y)
plot:
  mov #0,xbnk       ; rows 0-15 live in display bank 0, rows 16-31 in bank 1
  ld y
  and #$10
  bz .top
  mov #1,xbnk
.top:
  ld y              ; each pair of rows takes 16 bytes: 6 + 6 used, 4 spare
  and #$0e
  rol
  rol
  rol
  st tmp
  bn y,0,.even
  add #6
  st tmp
.even:
  ld x
  ror
  ror
  ror
  and #$1f
  add tmp
  add #$80
  st r2
  mov #<bits,trl
  mov #>bits,trh
  ld x
  and #7
  ldc
  or @R2
  st @R2
  ret
bits:
  .byte $80,$40,$20,$10,$08,$04,$02,$01

; about a twenty-fifth of a second
wait:
  mov #12,count
.outer:
  mov #0,tmp
.inner:
  dbnz tmp,.inner
  dbnz count,.outer
  ret

; clear both halves of the screen
wipe:
  mov #0,xbnk
  call .half
  mov #1,xbnk
.half:
  mov #$80,r2
.next:
  mov #0,@R2
  inc r2
  ld r2
  bnz .next
  ret

; copy 128 bytes from the save area (address low byte already in TRL) to the current display bank
loadpage:
  mov #>SAVEAREA,trh
  mov #$80,r2
  mov #0,tmp
.next:
  ld tmp
  ldc
  st @R2
  inc tmp
  inc r2
  ld r2
  bnz .next
  ret

; write both halves of the screen to the save area
save:
  clr1 ie,7
  mov #0,xbnk
  mov #<SAVEAREA,fmlow
  call .page
  mov #1,xbnk
  mov #<SAVEAREA+$80,fmlow
  call .page
  set1 ie,7
  ret
.page:
  mov #0,fmbank
  mov #>SAVEAREA,fmhigh
  mov #$80,r2
  mov #fmdata,r0
.next:
  ld @R2
  st @R0
  inc r0
  inc r2
  ld r2
  bnz .next
  call fm_write
  ret

  .pad $800         ; four blocks: code and header in the first three, the save area in the last
