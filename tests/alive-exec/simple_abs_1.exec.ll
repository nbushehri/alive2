
declare i8 @llvm.abs.i8(i8, i1)

define i8 @foo(i8 %0) {
  %2 = call i8 @llvm.abs.i8(i8 %0, i1 0)
  ret i8 %2
}

; CHECK: ConcreteVal(poison=0, 8b, 3u, 3s)
