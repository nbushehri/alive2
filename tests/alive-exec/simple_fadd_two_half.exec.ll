define half @foo(half %0) {
  %2 = fadd half %0, 0xH4000
  ret half %2
}

; CHECK: ConcreteVal(poison=0, 16b, 5F)