# Project: Nested Virtualization for OpenBSD VMM

This project aims to implement nested virtualization support for Intel VT-x in the OpenBSD VMM, following OpenBSD's development principles of simplicity, correctness, and consistency.

## Development Plan

A detailed, phased development plan is available in this repository:

- **[NESTED_VIRTUALIZATION_PLAN.md](./NESTED_VIRTUALIZATION_PLAN.md)**

## Commit Strategy

Work will be committed and pushed after the completion of each task outlined in the development plan. This ensures a clean, atomic commit history where each commit represents a single, logical change, and that changes are regularly synchronized with the remote repository.

## Quick Build Reference

To build and install a new kernel with the latest changes:

1.  **Update Source:** `cd /usr/src && cvs -q up -Pd`
2.  **Build Tools:** `make -C /usr/src obj`
3.  **Build & Install Kernel:** `make -C /usr/src/sys/arch/amd64/compile/GENERIC.MP && doas make -C /usr/src/sys/arch/amd64/compile/GENERIC.MP install`
4.  **Reboot**

*For more details, see the full plan or [OpenBSD FAQ 5](https://www.openbsd.org/faq/faq5.html#Bld).*