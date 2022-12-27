declare i8 @llvm.ctpop.i8(i8)

define i8 @test() {
        %res = call i8 @llvm.ctpop.i8(i8 255)
        ret i8 %res
}

; CHECK: ConcreteVal(poison=0, 8b, 8u, 8s)
