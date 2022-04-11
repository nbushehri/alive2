define dso_local i32 @sum(i32 %0) #0 {
  br label %2

2:                                                ; preds = %4, %1
  %.01 = phi i32 [ 0, %1 ], [ %5, %4 ]
  %.0 = phi i32 [ 1, %1 ], [ %6, %4 ]
  %3 = icmp slt i32 %.0, %0
  br i1 %3, label %4, label %7

4:                                                ; preds = %2
  %5 = add nsw i32 %.01, %.0
  %6 = add nsw i32 %.0, 1
  br label %2

7:                                                ; preds = %2
  ret i32 %.01
}

; CHECK: ConcreteVal(poison=0, 32b, 3u, 3s)
