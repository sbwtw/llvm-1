//===-- llvm-cltest.cpp - Command line test -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"

#include "clang-driver.h"
#include "link-options.h"
#include "lld.h"
#include "lld-core-options.h"

#include <map>
#include <string>

using namespace llvm;
using namespace option;

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  ClangDriverTool clang(argc - 1, argv + 1);
  LinkTool link(clang.getArgList());
  for (auto arg : link.getArgList()) {
    arg->dump();
    errs() << " ";
  }
  errs() << "\n";
}
