define i256 @foo() {
  %r1 = ashr i256 2, 18446744073709551617
  ret i256 %r1
}

; CHECK: ConcreteVal(poison=1, 256b