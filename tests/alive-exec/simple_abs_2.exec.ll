declare i8 @llvm.abs.i8(i8, i1)

define i8 @foo(i8 %0) {
  %2 = call i8 @llvm.abs.i8(i8 -128, i1 1)
  ret i8 %2
}

; CHECK: ConcreteVal(poison=1, 8b
