
define i32 @foo(i32 %0) #0 {
  %2 = add nsw i32 %0, 2
  ret i32 %2
}

; CHECK: ConcreteVal(poison=0, 32b, 5u, 5s)