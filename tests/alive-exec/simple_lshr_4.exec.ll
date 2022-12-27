define i8 @foo() {
  %r1 = lshr i8 -2, 1 
  ret i8 %r1
}

; CHECK: ConcreteVal(poison=0, 8b, 127u, 127s)