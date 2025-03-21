; ModuleID = 'bpftrace'
source_filename = "bpftrace"
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "bpf-pc-linux"

%"struct map_t" = type { ptr, ptr, ptr, ptr }
%"struct map_t.0" = type { ptr, ptr }
%"struct map_t.1" = type { ptr, ptr, ptr, ptr }
%helper_error_t = type <{ i64, i64, i32 }>

@LICENSE = global [4 x i8] c"GPL\00", section "license"
@AT_x = dso_local global %"struct map_t" zeroinitializer, section ".maps", !dbg !0
@ringbuf = dso_local global %"struct map_t.0" zeroinitializer, section ".maps", !dbg !20
@event_loss_counter = dso_local global %"struct map_t.1" zeroinitializer, section ".maps", !dbg !34

; Function Attrs: nounwind
declare i64 @llvm.bpf.pseudo(i64 %0, i64 %1) #0

define i64 @kprobe_f_1(ptr %0) section "s_kprobe_f_1" !dbg !50 {
entry:
  %key9 = alloca i32, align 4
  %helper_error_t4 = alloca %helper_error_t, align 8
  %"@x_key1" = alloca i64, align 8
  %key = alloca i32, align 4
  %helper_error_t = alloca %helper_error_t, align 8
  %"@x_val" = alloca i64, align 8
  %"@x_key" = alloca i64, align 8
  call void @llvm.lifetime.start.p0(i64 -1, ptr %"@x_key")
  store i64 1, ptr %"@x_key", align 8
  call void @llvm.lifetime.start.p0(i64 -1, ptr %"@x_val")
  store i64 1, ptr %"@x_val", align 8
  %update_elem = call i64 inttoptr (i64 2 to ptr)(ptr @AT_x, ptr %"@x_key", ptr %"@x_val", i64 0)
  %1 = trunc i64 %update_elem to i32
  %2 = icmp sge i32 %1, 0
  br i1 %2, label %helper_merge, label %helper_failure

helper_failure:                                   ; preds = %entry
  call void @llvm.lifetime.start.p0(i64 -1, ptr %helper_error_t)
  %3 = getelementptr %helper_error_t, ptr %helper_error_t, i64 0, i32 0
  store i64 30006, ptr %3, align 8
  %4 = getelementptr %helper_error_t, ptr %helper_error_t, i64 0, i32 1
  store i64 0, ptr %4, align 8
  %5 = getelementptr %helper_error_t, ptr %helper_error_t, i64 0, i32 2
  store i32 %1, ptr %5, align 4
  %ringbuf_output = call i64 inttoptr (i64 130 to ptr)(ptr @ringbuf, ptr %helper_error_t, i64 20, i64 0)
  %ringbuf_loss = icmp slt i64 %ringbuf_output, 0
  br i1 %ringbuf_loss, label %event_loss_counter, label %counter_merge

helper_merge:                                     ; preds = %counter_merge, %entry
  call void @llvm.lifetime.end.p0(i64 -1, ptr %"@x_val")
  call void @llvm.lifetime.end.p0(i64 -1, ptr %"@x_key")
  call void @llvm.lifetime.start.p0(i64 -1, ptr %"@x_key1")
  store i64 1, ptr %"@x_key1", align 8
  %delete_elem = call i64 inttoptr (i64 3 to ptr)(ptr @AT_x, ptr %"@x_key1")
  %6 = trunc i64 %delete_elem to i32
  %7 = icmp sge i32 %6, 0
  br i1 %7, label %helper_merge3, label %helper_failure2

event_loss_counter:                               ; preds = %helper_failure
  call void @llvm.lifetime.start.p0(i64 -1, ptr %key)
  store i32 0, ptr %key, align 4
  %lookup_elem = call ptr inttoptr (i64 1 to ptr)(ptr @event_loss_counter, ptr %key)
  %map_lookup_cond = icmp ne ptr %lookup_elem, null
  br i1 %map_lookup_cond, label %lookup_success, label %lookup_failure

counter_merge:                                    ; preds = %lookup_merge, %helper_failure
  call void @llvm.lifetime.end.p0(i64 -1, ptr %helper_error_t)
  br label %helper_merge

lookup_success:                                   ; preds = %event_loss_counter
  %8 = atomicrmw add ptr %lookup_elem, i64 1 seq_cst, align 8
  br label %lookup_merge

lookup_failure:                                   ; preds = %event_loss_counter
  br label %lookup_merge

lookup_merge:                                     ; preds = %lookup_failure, %lookup_success
  call void @llvm.lifetime.end.p0(i64 -1, ptr %key)
  br label %counter_merge

helper_failure2:                                  ; preds = %helper_merge
  call void @llvm.lifetime.start.p0(i64 -1, ptr %helper_error_t4)
  %9 = getelementptr %helper_error_t, ptr %helper_error_t4, i64 0, i32 0
  store i64 30006, ptr %9, align 8
  %10 = getelementptr %helper_error_t, ptr %helper_error_t4, i64 0, i32 1
  store i64 1, ptr %10, align 8
  %11 = getelementptr %helper_error_t, ptr %helper_error_t4, i64 0, i32 2
  store i32 %6, ptr %11, align 4
  %ringbuf_output5 = call i64 inttoptr (i64 130 to ptr)(ptr @ringbuf, ptr %helper_error_t4, i64 20, i64 0)
  %ringbuf_loss8 = icmp slt i64 %ringbuf_output5, 0
  br i1 %ringbuf_loss8, label %event_loss_counter6, label %counter_merge7

helper_merge3:                                    ; preds = %counter_merge7, %helper_merge
  call void @llvm.lifetime.end.p0(i64 -1, ptr %"@x_key1")
  ret i64 0

event_loss_counter6:                              ; preds = %helper_failure2
  call void @llvm.lifetime.start.p0(i64 -1, ptr %key9)
  store i32 0, ptr %key9, align 4
  %lookup_elem10 = call ptr inttoptr (i64 1 to ptr)(ptr @event_loss_counter, ptr %key9)
  %map_lookup_cond14 = icmp ne ptr %lookup_elem10, null
  br i1 %map_lookup_cond14, label %lookup_success11, label %lookup_failure12

counter_merge7:                                   ; preds = %lookup_merge13, %helper_failure2
  call void @llvm.lifetime.end.p0(i64 -1, ptr %helper_error_t4)
  br label %helper_merge3

lookup_success11:                                 ; preds = %event_loss_counter6
  %12 = atomicrmw add ptr %lookup_elem10, i64 1 seq_cst, align 8
  br label %lookup_merge13

lookup_failure12:                                 ; preds = %event_loss_counter6
  br label %lookup_merge13

lookup_merge13:                                   ; preds = %lookup_failure12, %lookup_success11
  call void @llvm.lifetime.end.p0(i64 -1, ptr %key9)
  br label %counter_merge7
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg %0, ptr nocapture %1) #1

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg %0, ptr nocapture %1) #1

attributes #0 = { nounwind }
attributes #1 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.dbg.cu = !{!47}
!llvm.module.flags = !{!49}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "AT_x", linkageName: "global", scope: !2, file: !2, type: !3, isLocal: false, isDefinition: true)
!2 = !DIFile(filename: "bpftrace.bpf.o", directory: ".")
!3 = !DICompositeType(tag: DW_TAG_structure_type, scope: !2, file: !2, size: 256, elements: !4)
!4 = !{!5, !11, !16, !19}
!5 = !DIDerivedType(tag: DW_TAG_member, name: "type", scope: !2, file: !2, baseType: !6, size: 64)
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!7 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 32, elements: !9)
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !{!10}
!10 = !DISubrange(count: 1, lowerBound: 0)
!11 = !DIDerivedType(tag: DW_TAG_member, name: "max_entries", scope: !2, file: !2, baseType: !12, size: 64, offset: 64)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !13, size: 64)
!13 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 131072, elements: !14)
!14 = !{!15}
!15 = !DISubrange(count: 4096, lowerBound: 0)
!16 = !DIDerivedType(tag: DW_TAG_member, name: "key", scope: !2, file: !2, baseType: !17, size: 64, offset: 128)
!17 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !18, size: 64)
!18 = !DIBasicType(name: "int64", size: 64, encoding: DW_ATE_signed)
!19 = !DIDerivedType(tag: DW_TAG_member, name: "value", scope: !2, file: !2, baseType: !17, size: 64, offset: 192)
!20 = !DIGlobalVariableExpression(var: !21, expr: !DIExpression())
!21 = distinct !DIGlobalVariable(name: "ringbuf", linkageName: "global", scope: !2, file: !2, type: !22, isLocal: false, isDefinition: true)
!22 = !DICompositeType(tag: DW_TAG_structure_type, scope: !2, file: !2, size: 128, elements: !23)
!23 = !{!24, !29}
!24 = !DIDerivedType(tag: DW_TAG_member, name: "type", scope: !2, file: !2, baseType: !25, size: 64)
!25 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !26, size: 64)
!26 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 864, elements: !27)
!27 = !{!28}
!28 = !DISubrange(count: 27, lowerBound: 0)
!29 = !DIDerivedType(tag: DW_TAG_member, name: "max_entries", scope: !2, file: !2, baseType: !30, size: 64, offset: 64)
!30 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !31, size: 64)
!31 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 8388608, elements: !32)
!32 = !{!33}
!33 = !DISubrange(count: 262144, lowerBound: 0)
!34 = !DIGlobalVariableExpression(var: !35, expr: !DIExpression())
!35 = distinct !DIGlobalVariable(name: "event_loss_counter", linkageName: "global", scope: !2, file: !2, type: !36, isLocal: false, isDefinition: true)
!36 = !DICompositeType(tag: DW_TAG_structure_type, scope: !2, file: !2, size: 256, elements: !37)
!37 = !{!38, !43, !44, !19}
!38 = !DIDerivedType(tag: DW_TAG_member, name: "type", scope: !2, file: !2, baseType: !39, size: 64)
!39 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !40, size: 64)
!40 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 64, elements: !41)
!41 = !{!42}
!42 = !DISubrange(count: 2, lowerBound: 0)
!43 = !DIDerivedType(tag: DW_TAG_member, name: "max_entries", scope: !2, file: !2, baseType: !6, size: 64, offset: 64)
!44 = !DIDerivedType(tag: DW_TAG_member, name: "key", scope: !2, file: !2, baseType: !45, size: 64, offset: 128)
!45 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !46, size: 64)
!46 = !DIBasicType(name: "int32", size: 32, encoding: DW_ATE_signed)
!47 = distinct !DICompileUnit(language: DW_LANG_C, file: !2, producer: "bpftrace", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly, globals: !48)
!48 = !{!0, !20, !34}
!49 = !{i32 2, !"Debug Info Version", i32 3}
!50 = distinct !DISubprogram(name: "kprobe_f_1", linkageName: "kprobe_f_1", scope: !2, file: !2, type: !51, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !47, retainedNodes: !55)
!51 = !DISubroutineType(types: !52)
!52 = !{!18, !53}
!53 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !54, size: 64)
!54 = !DIBasicType(name: "int8", size: 8, encoding: DW_ATE_signed)
!55 = !{!56}
!56 = !DILocalVariable(name: "ctx", arg: 1, scope: !50, file: !2, type: !53)
