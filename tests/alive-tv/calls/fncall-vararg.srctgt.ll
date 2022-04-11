; ERROR: Source is more defined than target

target triple = "x86_64-unknown-linux-gnu"
declare void @g(i8*, ...)

define void @src() {
  tail call void (i8*, ...) @g(i8* null, i32 0)
  ret void
}

define void @tgt() {
  tail call void (i8*, ...) @g(i8* null)
  ret void
}
; XFAIL: call with escaped locals
; SKIP-IDENTITY
