; ModuleID = 'build/vec_add.ll'
source_filename = "kernel.c"
target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: readwrite)
define protected amdgpu_kernel void @vec_add(ptr noundef readonly captures(none) %0, ptr noundef readonly captures(none) %1, ptr noundef writeonly captures(none) %2, i32 noundef %3) local_unnamed_addr #0 {
  %5 = addrspacecast ptr %2 to ptr addrspace(1)
  %6 = addrspacecast ptr %1 to ptr addrspace(1)
  %7 = addrspacecast ptr %0 to ptr addrspace(1)
  %8 = tail call i32 @llvm.amdgcn.workitem.id.x()
  %9 = icmp slt i32 %8, %3
  br i1 %9, label %10, label %18

10:                                               ; preds = %4
  %11 = zext nneg i32 %8 to i64
  %12 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %7, i64 %11
  %13 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %6, i64 %11
  %14 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %5, i64 %11
  %15 = load float, ptr addrspace(1) %12, align 4, !tbaa !7, !amdgpu.noclobber !9
  %16 = load float, ptr addrspace(1) %13, align 4, !tbaa !7, !amdgpu.noclobber !9
  %17 = fadd float %15, %16
  store float %17, ptr addrspace(1) %14, align 4, !tbaa !7
  br label %18

18:                                               ; preds = %10, %4
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare noundef range(i32 0, 1024) i32 @llvm.amdgcn.workitem.id.x() #1

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: readwrite) "amdgpu-agpr-alloc"="0" "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx942" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.module.flags = !{!0, !1}
!llvm.ident = !{!2}
!llvm.errno.tbaa = !{!3}

!0 = !{i32 1, !"amdhsa_code_object_version", i32 600}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{!"AMD clang version 23.0.0git (https://github.com/ROCm/llvm-project.git 5c9bfa94a37c59923dee3c55942566db7904b659+PATCHED:440716f8b87be9d8e20ed910e10e5b6d14d57cf6)"}
!3 = !{!4, !4, i64 0}
!4 = !{!"int", !5, i64 0}
!5 = !{!"omnipotent char", !6, i64 0}
!6 = !{!"Simple C/C++ TBAA"}
!7 = !{!8, !8, i64 0}
!8 = !{!"float", !5, i64 0}
!9 = !{}
