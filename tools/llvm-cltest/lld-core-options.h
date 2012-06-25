//===-- lld-core-options.h Option parser for lld-core ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OPTION_LLD_CORE_H
#define LLVM_OPTION_LLD_CORE_H

#include "Option.h"

namespace llvm {
namespace option {

enum LLDCoreOptionKind {
};

// This is needed to determine which tool a given argument is from by allowing
// the linker to assign unique ids.
extern const ToolInfo LLDCoreToolInfo;

struct LLDCoreTool {
  LLDCoreTool(int Argc, const char * const *Argv);

  ArgumentList getArgList() const { return CLP.getArgList(); };

  CommandLineParser CLP;
};

} // end namespace llvm
} // end namespace option

#endif
