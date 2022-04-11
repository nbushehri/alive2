declare float @llvm.fabs.f32(float  %Val)
declare double @llvm.fabs.f64(double %Val)

define float @foo(float %0) {
  %2 = fneg float %0
  %3 = call float @llvm.fabs.f32(float %2)
  ret float %3
}

; CHECK: ConcreteVal(poison=0, 32b, 3F)


define double @bar(double %0) {
  %2 = fneg double %0
  %3 = call double @llvm.fabs.f64(double %2)
  ret double %3
}

; CHECK: ConcreteVal(poison=0, 64b, 3F)

