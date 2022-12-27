define i8 @foo() {
  %r1 = lshr i8 4, 3 
  ret i8 %r1
}

; CHECK: ConcreteVal(poison=0, 8b, 0u, 0s)
