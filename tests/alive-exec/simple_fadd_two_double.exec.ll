define double @foo(double %0) #0 {
  %2 = fadd double %0, 2.0
  ret double %2
}

; CHECK: ConcreteVal(poison=0, 64b, 5F)