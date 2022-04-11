
define i32 @foo(i32 %0) #0 {
  %2 = and i32 %0, 4
  ret i32 %2
}

; CHECK: ConcreteVal(poison=0, 32b, 0u, 0s)
