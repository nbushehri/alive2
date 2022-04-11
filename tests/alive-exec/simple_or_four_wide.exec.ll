
define i96 @foo(i96 %0) #0 {
  %2 = or i96 %0, 4
  ret i96 %2
}

; CHECK: ConcreteVal(poison=0, 96b, 7u, 7s)