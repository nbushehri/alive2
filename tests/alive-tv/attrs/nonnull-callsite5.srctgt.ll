define void @src(i8* %p) {
  call void @f(i8* undef)
  ret void
}

define void @tgt(i8* %p) {
  unreachable
}

declare void @f(i8* nonnull)

; ERROR: Source is more defined than target
; XFAIL: call with escaped locals
; SKIP-IDENTITY
