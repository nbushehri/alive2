declare i4 @llvm.sadd.sat.i4(i4 %a, i4 %b)
declare i8 @llvm.sadd.sat.i8(i8 %a, i8 %b)
declare i16 @llvm.sadd.sat.i16(i16 %a, i16 %b)
declare i32 @llvm.sadd.sat.i32(i32 %a, i32 %b)
declare i64 @llvm.sadd.sat.i64(i64 %a, i64 %b)
declare <4 x i32> @llvm.sadd.sat.v4i32(<4 x i32> %a, <4 x i32> %b)


define i4 @foo4_1() {
    %b = call i4 @llvm.sadd.sat.i4(i4 1, i4 2)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 3u, 3s)

define i4 @foo4_2() {
    %b = call i4 @llvm.sadd.sat.i4(i4 5, i4 6)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 7u, 7s)

define i4 @foo4_3() {
    %b = call i4 @llvm.sadd.sat.i4(i4 -4, i4 2)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 14u, -2s)

define i4 @foo4_4() {
    %b = call i4 @llvm.sadd.sat.i4(i4 -4, i4 -5)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 8u, -8s)

define i32 @foo32(i32 %x) {
    %b = call i32 @llvm.sadd.sat.i32(i32 %x, i32 4)
    ret i32 %b
}

; CHECK: ConcreteVal(poison=0, 32b, 7u, 7s)