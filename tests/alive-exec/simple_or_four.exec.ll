
define i64 @foo(i64 %0) #0 {
  %2 = or i64 %0, 4
  ret i64 %2
}

; CHECK: ConcreteVal(poison=0, 64b, 7u, 7s)