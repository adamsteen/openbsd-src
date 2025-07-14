# OpenBSD Nested Virtualization Development Plan

## 1. Philosophy and Guiding Principles

This document outlines the development plan for introducing nested virtualization support into the OpenBSD Virtual Machine Monitor (VMM) for Intel VT-x. The implementation will strictly adhere to the following principles:

- **Simplicity:** The design will be the simplest possible implementation that is correct and secure. We will avoid complexity unless it is proven to be absolutely necessary.
- **Consistency:** All new code will follow the existing style, structure, and conventions of the OpenBSD kernel and the VMM subsystem. `style(9)` is the canonical reference.
- **Hardware-First:** The implementation will leverage hardware features like VMCS shadowing directly, minimizing complex software emulation.
- **Incremental Development:** The work is broken into distinct, testable phases to ensure a stable and methodical implementation.

## 2. References

This project relies on the official Intel® 64 and IA-32 Architectures Software Developer's Manuals as the canonical source of truth for VMX architecture and behavior.

- **Intel® 64 and IA-32 Architectures Software Developer’s Manual (Combined Volumes):** [https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  - **Volume 3C:** System Programming Guide, Part 3. This volume is the most critical, as it details the Virtual Machine Extensions (VMX). Key chapters include:
    - **Chapter 24:** VMX Support for Virtualization of the VMX Architecture
    - **Chapter 27:** Nested VMX Transitions

## 3. Hardware Requirements

The minimum required hardware feature set for nested virtualization support is the **"Modern Baseline"** for Intel VT-x. This ensures a simple, correct, and performant implementation. The required features are:

1.  **Intel VT-x:** The basic virtualization extensions.
2.  **Extended Page Tables (EPT):** For hardware-accelerated memory virtualization.
3.  **"VMCS Shadowing":** To efficiently handle L1 hypervisor `VMREAD`/`VMWRITE` operations.
4.  **"Unrestricted Guest":** To allow modern L1 guest hypervisors to run without complex workarounds.

Requiring this baseline aligns with the project's goal of a clean and maintainable implementation.

## 4. Phase 1: Foundational Primitives

This phase focuses on detecting hardware support and making the minimal necessary changes to core data structures.

### Task 1.1: Detect Nested Virtualization Capability

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Action:** In the VMX feature discovery code, check for the "VMCS shadowing" bit in the `IA32_VMX_PROCBASED_CTLS2` MSR.
- **Details:**
    - This capability is a prerequisite for efficient nested virtualization.
    - If the feature is supported, set a global flag, e.g., `vmm_nested_support = 1;`.
    - If not supported, the flag remains `0`, and all subsequent nested virtualization features will be disabled.
- **Reference:** SDM Vol. 3C, Appendix A.3.3

### Task 1.2: Minimal `struct vcpu` Modification

- **File:** `sys/dev/vmm/vmm.h`
- **Action:** Add fields to `struct vcpu` to manage the nested state.
- **Details:**
    - `uint64_t vc_shadow_vmcs_pa;`: Physical address of the shadow VMCS.
    - `struct vmcs *vc_shadow_vmcs;`: Virtual address of the shadow VMCS.
    - `int vc_is_nested;`: A flag to indicate that the vCPU is operating as an L1 hypervisor.

### Task 1.3: Enable VMX in the Guest

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Function:** `vcpu_reset_regs_vmx()`
- **Action:** Allow a guest to use VMX instructions by setting the appropriate VM-execution control.
- **Details:**
    - When setting up the VM-execution controls, if nested virtualization is enabled for the VM, set the **"virtual-machine extensions" bit** in the **secondary processor-based VM-execution controls**.
    - This will be conditional on a new flag in `struct vm_create_params`.
- **Reference:** SDM Vol. 3C, Chapter 24.6.2

### Task 1.4: Set VMCS Link Pointer

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Function:** `vcpu_reset_regs_vmx()`
- **Action:** Set the VMCS link pointer as required for nested transitions.
- **Details:**
    - Add a `vmwrite(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFF);` during VMCS initialization. This is a mandatory hardware requirement for all nested transitions.
- **Reference:** SDM Vol. 3C, Chapter 24.8.2

## 4. Phase 2: Handling L1 Hypervisor Operations

This phase implements the core logic for managing an L1 guest that is acting as a hypervisor by trapping and redirecting its VMX operations to a shadow VMCS. This entire phase is guided by the principles of **VMCS Shadowing**.

- **Reference:** SDM Vol. 3C, Chapter 24.10

### Task 2.1: Handle `VMXON` Exit

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Function:** `vmx_handle_exit()`
- **Action:** Add a case for `EXIT_REASON_VMXON`.
- **Details:**
    - Allocate a page for the shadow VMCS.
    - Store its physical and virtual addresses in the `struct vcpu`.
    - Initialize the shadow VMCS revision ID.
    - Set `vcpu->vc_is_nested = 1;`.
    - Advance the guest's instruction pointer (`RIP`) past the `VMXON` instruction.

### Task 2.2: Handle Shadow VMCS Operations

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Function:** `vmx_handle_exit()`
- **Action:** Add handlers for `VMPTRLD`, `VMCLEAR`, `VMREAD`, and `VMWRITE` exits.
- **Details:**
    - These handlers will operate on the L1 guest's *shadow VMCS* (`vcpu->vc_shadow_vmcs`), not the hardware VMCS.
    - `VMPTRLD`: Set the L1's concept of the "current" VMCS (a new field in `struct vcpu` may be needed).
    - `VMCLEAR`: Clear the L1's "current" VMCS pointer.
    - `VMREAD`/`VMWRITE`: Read or write data from/to the shadow VMCS page.

## 5. Phase 3: L2 Guest Execution

This phase implements the logic to run an L2 guest and handle its exits, a process known as a **Nested VMX Transition**.

- **Reference:** SDM Vol. 3C, Chapter 27

### Task 3.1: Handle `VMLAUNCH`/`VMRESUME` from L1

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Function:** `vmx_handle_exit()`
- **Action:** Add handlers for `EXIT_REASON_VMLAUNCH` and `EXIT_REASON_VMRESUME`.
- **Details:**
    - These handlers will be the entry point for running the L2 guest. They will call a new function, `nested_vmx_run()`.

### Task 3.2: Implement `nested_vmx_run()`

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Action:** Create the `nested_vmx_run()` function.
- **Details:**
    - This function performs the "VMCS merge" by copying necessary fields from the L1's shadow VMCS into the L0's hardware VMCS.
    - It then saves the L1 guest's state and uses the existing `vmx_enter_guest` assembly routine to execute the L2 guest.

### Task 3.3: Handle L2 Guest Exits

- **File:** `sys/arch/amd64/amd64/vmm_machdep.c`
- **Action:** Create a `nested_vmx_handle_exit()` function.
- **Details:**
    - The main `vmx_handle_exit()` will check `vcpu->vc_is_nested` and dispatch to this new function if an exit occurs during L2 execution.
    - This handler will read the exit information from the hardware VMCS.
    - It will then "reflect" the exit to the L1 hypervisor by writing the exit details into the L1's shadow VMCS.
    - Finally, it will restore the L1 guest's state and resume it, making it appear as if the L2 guest's exit happened directly in the L1.
- **Future Compatibility Note:** This exit-reflection mechanism serves as the general-purpose fallback path. The architecture is designed to be compatible with future optimizations like Virtualization Exceptions (`#VE`), which would allow the hardware to bypass this L0 handler for certain performance-critical exits and deliver them directly to the L1 guest.

## 6. Phase 4: Integration and Cleanup

### Task 4.1: User-level Control

- **File:** `sys/dev/vmm/vmm.h`
- **Action:** Add a flag to `struct vm_create_params` to allow `vmctl(8)` to enable nested virtualization on a per-VM basis.

### Task 4.2: Resource Management

- **File:** `sys/dev/vmm/vmm.c`
- **Function:** `vm_teardown()`
- **Action:** Ensure that the shadow VMCS page is properly deallocated when a VM is terminated.

## 7. Building and Testing

All development will be done against OpenBSD-current. The following steps, based on the OpenBSD FAQ and the `release(8)` man page, should be used to build and install a new kernel with the VMM changes.

1.  **Update the Source Tree:** Ensure your `/usr/src` is up to date with the latest from `-current`.
    ```
    cd /usr/src
    cvs -q up -Pd
    ```

2.  **Build the Kernel:**
    - **Build Tools:** `make -C /usr/src obj`
    - **Configure:** `config GENERIC.MP` (or your custom kernel config)
    - **Compile:** `make -C /usr/src/sys/arch/amd64/compile/GENERIC.MP`

3.  **Install the Kernel:**
    - `doas make -C /usr/src/sys/arch/amd64/compile/GENERIC.MP install`

4.  **Reboot:** Reboot the system to load the new kernel.

- **Reference:**
    - [OpenBSD FAQ 5: Building the System](https://www.openbsd.org/faq/faq5.html#Bld)
    - `release(8)` man page: [https://man.openbsd.org/release](https://man.openbsd.org/release)

### Task 4.3: Testing

- **Action:** Develop a regression test.
- **Details:**
    - Create a simple userland program that creates an L1 guest.
    - The L1 guest will enable VMX and attempt to launch a minimal L2 guest.
    - The test will verify that the L2 guest can execute a simple instruction (e.g., `HLT`) and that the exit is correctly reflected to the L1 guest.
