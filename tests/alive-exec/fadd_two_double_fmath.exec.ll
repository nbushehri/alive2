define double @foo(double %0) #0 {
  %2 = fadd nnan double %0, 2.0
  ret double %2
}

; CHECK: ConcreteVal(poison=0, 64b, 5F)

define double @foo2(double %0) #0 {
  %2 = fadd ninf double %0, 2.0
  ret double %2
}

; CHECK: ConcreteVal(poison=0, 64b, 5F)

define double @foo3(double %0) #0 {
  %2 = fadd ninf double %0, 0x7FF8000000000000;nan
  ret double %2
}
; CHECK: ConcreteVal(poison=0, 64b, NaNF)

define double @foo4(double %0) #0 {
  %2 = fadd nnan double %0, 0x7FF8000000000000;nan
  ret double %2
}
; CHECK: ConcreteVal(poison=1, 64b

define double @bar() #0 {
  %1 = fadd double 2.0, 0x7FF0000000000000; inf rhs
  ret double %1
}
; CHECK: ConcreteVal(poison=0, 64b, +InfF)

define double @bar2() #0 {
  %1 = fadd ninf double 2.0, 0x7FF0000000000000; inf rhs
  ret double %1 ;  
}
; CHECK: ConcreteVal(poison=1, 64b