declare i8 @llvm.ctpop.i8(i8)

define i1 @test(i8 %x) {
        %a = shl i8 %x, 2
        %b = call i8 @llvm.ctpop.i8(i8 %a)
        %c = icmp sle i8 %b, 6
        ret i1 %c
}

; CHECK: ConcreteVal(poison=0, 1b, 1u, -1s)