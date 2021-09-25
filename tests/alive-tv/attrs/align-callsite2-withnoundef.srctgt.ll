define void @src(i8* %p) {
  call void @g(i8* align(4) %p)
  load i8, i8* %p
  ret void
}

define void @tgt(i8* %p) {
  call void @g(i8* align(4) %p)
  load i8, i8* %p, align 4
  ret void
}

declare void @g(i8* noundef)
; XFAIL: call with escaped locals
; SKIP-IDENTITY
