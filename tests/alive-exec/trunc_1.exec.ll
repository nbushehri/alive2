define i32 @test(i64 %Val) {
        %tmp.3 = trunc i64 %Val to i32          
        ret i32 %tmp.3
}

; CHECK: ConcreteVal(poison=0, 32b, 3u, 3s)