# DLSS 5 DLL Loader Fix - Plan

## Objective
Fix the broken DLSS 5 DLL loader/wrapper implementation to use NVIDIA's legitimate signed components.

## Requirements
- Preserve all existing user changes, Git state, and working DLSS 5 functionality
- Keep `dlss5=1` configuration behavior in `settings.ini`
- Use only NVIDIA-provided, signed components and supported interfaces
- No proxy DLLs, DLL replacement, or interception mechanisms
- DLL loader should only target the Windows build
- Remove dead/broken implementation if audit confirms it's unnecessary
- Repair CMake build configuration and dependency setup
- Comprehensive error handling for DLSS 5 init/shutdown
- Produce final report covering root cause, architecture, files removed/modified, build/test results, dependencies, and outstanding issues

## Execution Approach
- [x] Inline execution (not subagent)
- [ ] Track plan in this file

## Audit Findings
- Broken wrapper files: `src/wrapper/ngx_wrapper.c`, `src/wrapper/nvngx_wrapper.c`, `src/wrapper/build.rs`, `src/wrapper/Cargo.toml`
- Broken loader script: `loader_dls.py`
- These should be removed as they're broken by design

## Work Items
- [x] Remove broken wrapper files (ngx_wrapper.c, nvngx_wrapper.c, build.rs, Cargo.toml)
- [x] Remove broken loader script (loader_dls.py)
- [ ] Implement changes to validate DLL signatures or restrict search path to load only NVIDIA-signed components
- [ ] Run build and tests to verify everything still works
- [ ] Write comprehensive audit report covering root cause, architecture, files removed/modified, build/test results, dependencies, and outstanding issues

## Notes
- The actual DLSS integration uses NVIDIA's proper NGX API through runtime code (ngx_vk.cpp)
- The wrapper DLL approach was broken by design and should be removed
- The loader must validate DLL signatures or restrict search path to ensure only NVIDIA-signed components are loaded
