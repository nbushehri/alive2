define float @foo() #0 {
  %1 = frem float 4.0, 3.0
  ret float %1
}

; CHECK: ConcreteVal(poison=0, 32b, 1F)

define float @foo2() #0 {
  %1 = frem float 3.0, 2.0
  ret float %1
}

; CHECK: ConcreteVal(poison=0, 32b, 1F)


define float @foo3(float %0) #0 {
  %2 = frem ninf float %0, 2.0
  ret float %2
}

; CHECK: ConcreteVal(poison=0, 32b, 1F)
