define i8 @test(i8 %x) {
        %a = and i8 %x, 15; 0x0F
        %b = shl nuw i8 %a, 4
        ret i8 %b
}

; CHECK: ConcreteVal(poison=0, 8b
