define i8 @test(i8 %x) {
        %a = or i8 %x, 224; 0xE0
        %b = shl nsw i8 %a, 2
        ret i8 %b
}

; CHECK: ConcreteVal(poison=0, 8b
