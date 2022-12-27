define i32 @foo(i32 %0) #0 {
  %2 = add nsw i32 %0, poison
  ret i32 %2
}

; CHECK: ConcreteVal(poison=1, 32b