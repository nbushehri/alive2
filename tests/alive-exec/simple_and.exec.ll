define i32 @foo(i32 %0, i32 %1) #0 {
  %3 = and i32 %0, %1
  ret i32 %3
}

; CHECK: ConcreteVal(poison=0, 32b, 3u, 3s)

define i32 @foo2(i32 %0) #0 {
  %2 = and i32 %0, 1
  ret i32 %2
}

; CHECK: ConcreteVal(poison=0, 32b, 1u, 1s)