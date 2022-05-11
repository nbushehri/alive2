declare i8 @f(i8* %p) readonly argmemonly

define i8 @src() {
  %p = alloca i8
  store i8 1, i8* %p
  %r = call i8 @f(i8* %p)
  ret i8 %r
}

define i8 @tgt() {
  %p = alloca i8
  store i8 2, i8* %p
  %r = call i8 @f(i8* %p)
  ret i8 %r
}

; SKIP-IDENTITY
; ERROR: Value mismatch
; XFAIL: argmemonly
