define i16 @test() {
        %X = sext i8 -1 to i16  
        ret i16 %X
}

; CHECK: ConcreteVal(poison=0, 16b, 65535u, -1s)
