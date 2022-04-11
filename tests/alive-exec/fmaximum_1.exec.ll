declare float  @llvm.maximum.f32(float  %Val0, float  %Val1l)
declare double @llvm.maximum.f64(double %Val0, double %Val1)

define float @foo(float %0) {
  %2 = fadd float %0, 2.0
  %3 = call float @llvm.maximum.f32(float %0, float %2)
  ret float %3
}

; CHECK: ConcreteVal(poison=0, 32b, 5F)

define float @foo2() {
  %1 = call float @llvm.maximum.f32(float +0.0, float -0.0)
  ret float %1
}

; CHECK: ConcreteVal(poison=0, 32b, 0F)

define float @foo3() {
  %1 = call float @llvm.maximum.f32(float -0.0, float -0.0)
  ret float %1
}

; CHECK: ConcreteVal(poison=0, 32b, -0F)

define double @bar(double %0) {
  %2 = fadd double %0, 4.0
  %3 = call double @llvm.maximum.f64(double %2, double %0)
  ret double %3
}

; CHECK: ConcreteVal(poison=0, 64b, 7F)
