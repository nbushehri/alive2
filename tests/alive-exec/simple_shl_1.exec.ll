define i8 @test(i8 %x) {
        %a = shl i8 %x, 2
        ret i8 %a
}

; CHECK: ConcreteVal(poison=0, 8b, 12u, 12s)