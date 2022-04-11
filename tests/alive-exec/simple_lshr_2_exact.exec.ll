define i32 @foo() {
  %r1 = lshr exact i32 18, 2 
  ret i32 %r1
}

; CHECK: ConcreteVal(poison=1, 32b