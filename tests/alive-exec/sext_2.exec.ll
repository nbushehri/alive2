define i32 @test() {
        %X = sext i1 true to i32  
        ret i32 %X
}

; CHECK: ConcreteVal(poison=0, 32b, 4294967295u, -1s)
