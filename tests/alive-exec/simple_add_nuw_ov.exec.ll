
define i8 @foo(i8 %0) #0 {
  %2 = add nuw i8 %0, 255
  ret i8 %2
}

; CHECK: ConcreteVal(poison=1, 8b