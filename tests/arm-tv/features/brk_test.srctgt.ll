; TEST-ARGS: -backend-tv --disable-undef-input --disable-poison-input

define i32 @usub(i32 %0, i32 %1) local_unnamed_addr {
  %3 = tail call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %0, i32 %1)
  %4 = extractvalue { i32, i1 } %3, 1
  br i1 %4, label %5, label %6

5:                                                ; preds = %2
  tail call void @llvm.trap()
  unreachable

6:                                                ; preds = %2
  %7 = extractvalue { i32, i1 } %3, 0
  ret i32 %7
}

; Function Attrs: cold noreturn nounwind
declare void @llvm.trap() #0

; Function Attrs: nocallback nofree nosync nounwind readnone speculatable willreturn
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32) #1

attributes #0 = { cold noreturn nounwind }
attributes #1 = { nocallback nofree nosync nounwind readnone speculatable willreturn }
