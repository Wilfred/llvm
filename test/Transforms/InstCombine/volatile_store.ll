; NOTE: Assertions have been autogenerated by update_test_checks.py
; RUN: opt < %s -instcombine -S | FileCheck %s

@x = weak global i32 0

define void @self_assign_1() {
; CHECK-LABEL: @self_assign_1(
; CHECK:         [[TMP:%.*]] = load volatile i32, i32* @x, align 4
; CHECK-NEXT:    store volatile i32 [[TMP]], i32* @x, align 4
; CHECK-NEXT:    br label %return
; CHECK:       return:
; CHECK-NEXT:    ret void
;
entry:
	%tmp = load volatile i32, i32* @x
	store volatile i32 %tmp, i32* @x
	br label %return

return:
	ret void
}
