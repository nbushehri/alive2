define float @foo(float %0) #0 {
  %2 = fsub float %0, 2.0
  ret float %2
}

; CHECK: ConcreteVal(poison=0, 32b, 1F)

define float @bar() #0 {
  %1 = fsub float 0.0, 0x7FF8000000000000;nan
  ret float %1
}
; CHECK: ConcreteVal(poison=0, 32b, NaNF)

define float @bar2(float %0) #0 {
  %2 = fsub float %0, 0x7FF8000000000000;nan
  ret float %2
}
; CHECK: ConcreteVal(poison=0, 32b, NaNF)

define float @baz() #0 {
  %1 = fsub float 0.0, 0x7FF0000000000000;+inf
  ret float %1
}
; CHECK: ConcreteVal(poison=0, 32b, -InfF)

define float @baz2(float %0) #0 {
  %2 = fsub float %0, 0x7FF0000000000000;+inf
  ret float %2
}
; CHECK: ConcreteVal(poison=0, 32b, -InfF)