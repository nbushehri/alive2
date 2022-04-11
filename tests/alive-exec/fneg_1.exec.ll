define double @foo(double %0) #0 {
  %2 = fneg double %0
  ret double %2
}

; CHECK: ConcreteVal(poison=0, 64b, -3F)

define float @foo2(float %0) #0 {
  %2 = fneg float %0
  ret float %2
}

; CHECK: ConcreteVal(poison=0, 64b, -3F)