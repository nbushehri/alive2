define i8 @test(i8 %x) {
        %a = or i8 %x, 128; 0x80
        %b = shl nsw i8 %a, 2
        ret i8 %b
}

; CHECK: ConcreteVal(poison=1, 8b