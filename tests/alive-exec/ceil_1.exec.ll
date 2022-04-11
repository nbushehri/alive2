declare float @llvm.ceil.f32(float  %Val)
declare double @llvm.ceil.f64(double %Val)

define float @foo(float %0) {
  %2 = fneg float %0
  %3 = fadd nnan float %2, 1.25
  %4 = call float @llvm.ceil.f32(float %3)
  ret float %4
}

; CHECK: ConcreteVal(poison=0, 32b, -1F)


define float @foo1() {
  %1 = call float @llvm.ceil.f32(float -1.75)
  ret float %1
}

; CHECK: ConcreteVal(poison=0, 32b, -1F)

define double @bar(double %0) {
  %2 = fneg double %0
  %3 = call double @llvm.ceil.f64(double %2)
  ret double %3
}

; CHECK: ConcreteVal(poison=0, 64b, -3F)

