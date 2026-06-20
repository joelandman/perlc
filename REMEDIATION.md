# REMEDIATION.md — Critical Fixes Needed

All issues have been fixed:

1. ~~**Fix `NK::EvalBlock` LLVM codegen crash**~~ — FIXED (commit a701cd1)
   - Added check for `return` inside eval blocks in main function
   - Calls `perl_die` instead of emitting `ret ptr` (which violates LLVM's type requirements)

2. ~~**Fix `require` caching bug**~~ — FIXED (already working, verified)
   - Runtime `require` correctly registers named subs in the dispatch table

3. ~~**Fix compound `-=` on shared scalars**~~ — FIXED (commit db7ba77)
   - Added `get_or_install_mutex` call in `perl_atomic_add` to ensure mutex is installed

4. ~~**Add `*=` `/=` `%=` atomic RMW for shared scalars**~~ — FIXED (commit db7ba77)
   - Added `perl_atomic_rmw` runtime function with mutex protection
   - Codegen calls `perl_atomic_rmw` for `*=`, `/=`, `%=` on shared scalars

5. ~~**Fix closure + range with captured variable**~~ — FIXED (commit 776b963)
   - Fixed list return bug: added `perl_array_push_list_or_scalar` to unwrap `PERL_LIST_RESULT` tags
   - Fixed `perl_call_code_ref` to use caller's wantarray context
