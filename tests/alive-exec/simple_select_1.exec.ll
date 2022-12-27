
define i8 @foo(i8 %0) {
  %2 = select i1 true, i8 42, i8 %0  
  ret i8 %2
}

; CHECK: ConcreteVal(poison=0, 8b, 42u, 42s)
