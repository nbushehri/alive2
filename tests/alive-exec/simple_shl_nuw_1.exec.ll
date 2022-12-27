define i8 @test(i8 %x) {
        %a = or i8 %x, -96; 0xA0
        %b = shl nuw i8 %a, 2
        ret i8 %b
}

; CHECK: ConcreteVal(poison=1, 8b
