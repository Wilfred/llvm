========================================
 Kaleidoscope: Compiling to Object Code
========================================

.. contents::
   :local:

Chapter 5 Introduction
======================

Welcome to Chapter 5 of the "`Implementing a language with LLVM
<index.html>`_" tutorial. This chapter describes how to compile our
language down to object code files.

Choosing a target
=================

LLVM has native support for cross-compilation. You can compile to the
architecture of your current machine, or just as easily compile for
other architectures. In this tutorial, we'll target the current
machine.

To specify the architecture that you want to target, we use a string
called a "target triple". This takes the form
``<arch><sub>-<vendor>-<sys>-<abi>`` (see the `cross compilation docs
<http://clang.llvm.org/docs/CrossCompilation.html#target-triple>`_).

As an example, we can see what clang thinks is our current target
triple:

::

    clang --version | grep Target
    Target: x86_64-unknown-linux-gnu

Fortunately, we don't need to hard-code a target triple to target the
current machine. LLVM provides ``sys::getDefaultTargetTriple``, which
returns the target triple of the current machine.

Data layout
===========

todo

Target Machine
==============

We can also tell LLVM that we're happy using a specific feature (such as SSE) or
even a specific CPU (such as sandylake).

We can use llc to see all the extra features and CPUs within an
architecture that LLVM knows about. For example, let's look at x86:

::

    $ llvm-as < /dev/null | llc -march=x86 -mattr=help
    Available CPUs for this target:

      amdfam10      - Select the amdfam10 processor.
      athlon        - Select the athlon processor.
      athlon-4      - Select the athlon-4 processor.
      ...

    Available features for this target:

      16bit-mode            - 16-bit mode (i8086).
      32bit-mode            - 32-bit mode (80386).
      3dnow                 - Enable 3DNow! instructions.
      3dnowa                - Enable 3DNow! Athlon instructions.
      ...

In our example, we'll use the generic CPU without any additional
features.
    
        let mut target = null_mut();
        let mut err_msg_ptr = null_mut();
        unsafe {
            LLVMGetTargetFromTriple(target_triple, &mut target, &mut err_msg_ptr);
            if target.is_null() {
                // LLVM couldn't find a target triple with this name,
                // so it should have given us an error message.
                assert!(!err_msg_ptr.is_null());

                let err_msg_cstr = CStr::from_ptr(err_msg_ptr as *const _);
                let err_msg = str::from_utf8(err_msg_cstr.to_bytes()).unwrap();
                return Err(err_msg.to_owned());
            }
        }

        // TODO: do these strings live long enough?
        // cpu is documented: http://llvm.org/docs/CommandGuide/llc.html#cmdoption-mcpu
        let cpu = CString::new("generic").unwrap();
        // features are documented: http://llvm.org/docs/CommandGuide/llc.html#cmdoption-mattr
        let features = CString::new("").unwrap();

        let target_machine;
        unsafe {
            target_machine =
                LLVMCreateTargetMachine(target,
                                        target_triple,
                                        cpu.as_ptr() as *const _,
                                        features.as_ptr() as *const _,
                                        LLVMCodeGenOptLevel::LLVMCodeGenLevelAggressive,
                                        LLVMRelocMode::LLVMRelocDefault,
                                        LLVMCodeModel::LLVMCodeModelDefault);
        }


Initialize Targets
==================

        LLVM_InitializeAllTargetInfos();
        LLVM_InitializeAllTargets();
        LLVM_InitializeAllTargetMCs();
        LLVM_InitializeAllAsmParsers();
        LLVM_InitializeAllAsmPrinters();

Emit Object Code
================


        let mut obj_error = module.new_mut_string_ptr("Writing object file failed.");
        let result = LLVMTargetMachineEmitToFile(target_machine.tm,
                                                 module.module,
                                                 module.new_string_ptr(path) as *mut i8,
                                                 LLVMCodeGenFileType::LLVMObjectFile,
                                                 &mut obj_error);
