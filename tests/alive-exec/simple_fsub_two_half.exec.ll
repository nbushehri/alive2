define half @foo(half %0) {
  %2 = fsub half %0, 0xH4000 ;2.0
  ret half %2
}
; CHECK: ConcreteVal(poison=0, 16b, 1F)


define half @foo2(half %0) {
  %2 = fsub half %0, 0xH7C00 ;Inf
  ret half %2
}

; CHECK: ConcreteVal(poison=0, 16b, -InfF)