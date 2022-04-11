define i32 @foo() {
  %r1 = lshr i32 1, 32 
  ret i32 %r1
}

; CHECK: ConcreteVal(poison=1, 32b