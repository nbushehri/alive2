define i32 @foo() {
  %r1 = lshr i32 16, 2 
  ret i32 %r1
}

; CHECK: ConcreteVal(poison=0, 32b, 4u, 4s)