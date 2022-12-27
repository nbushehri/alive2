declare i8 @llvm.ctpop.i8(i8)

define i8 @test() {
        %res = call i8 @llvm.ctpop.i8(i8 32)
        ret i8 %res
}

; CHECK: ConcreteVal(poison=0, 8b, 1u, 1s)
