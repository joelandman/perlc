#pragma once

/* Thin wrapper for LLVM initialization.
   This header deliberately contains ZERO <llvm/...> includes.
   The actual implementation lives in llvm_support.cpp which is
   allowed to see the full LLVM headers.
*/
#ifdef __cplusplus
extern "C" {
#endif

void perlc_llvm_early_init(int *argc, char ***argv);

#ifdef __cplusplus
}
#endif
