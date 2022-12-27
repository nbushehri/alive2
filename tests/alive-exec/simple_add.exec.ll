
define dso_local i32 @foo(i32 %0, i32 %1) #0 {
  %3 = add nsw i32 %0, %1
  ret i32 %3
}

; CHECK: ConcreteVal(poison=0, 32b, 6u, 6s)
