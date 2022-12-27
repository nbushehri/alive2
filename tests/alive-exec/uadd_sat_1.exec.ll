declare i4 @llvm.uadd.sat.i4(i4 %a, i4 %b)
declare i8 @llvm.uadd.sat.i8(i8 %a, i8 %b)
declare i16 @llvm.uadd.sat.i16(i16 %a, i16 %b)
declare i32 @llvm.uadd.sat.i32(i32 %a, i32 %b)
declare i64 @llvm.uadd.sat.i64(i64 %a, i64 %b)
declare <4 x i32> @llvm.uadd.sat.v4i32(<4 x i32> %a, <4 x i32> %b)


define i4 @foo4_1() {
    %b = call i4 @llvm.uadd.sat.i4(i4 1, i4 2)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 3u, 3s)

define i4 @foo4_2() {
    %b = call i4 @llvm.uadd.sat.i4(i4 5, i4 6)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 11u, -5s)

define i4 @foo4_3() {
    %b = call i4 @llvm.uadd.sat.i4(i4 8, i4 8)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=0, 4b, 15u, -1s)

define i4 @foo4_4() {
    %b = call i4 @llvm.uadd.sat.i4(i4 8, i4 poison)
    ret i4 %b
}

; CHECK: ConcreteVal(poison=1, 4b

define i32 @foo32(i32 %x) {
    %b = call i32 @llvm.uadd.sat.i32(i32 %x, i32 4)
    ret i32 %b
}

; CHECK: ConcreteVal(poison=0, 32b, 7u, 7s)