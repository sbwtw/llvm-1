//===- Module.h - Object File Module ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJECT_MODULE_H
#define LLVM_OBJECT_MODULE_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/Atom.h"
#include "llvm/Support/system_error.h"
#include <map>

namespace llvm {
namespace object {
class ObjectFile;

class Module {
  Module(const Module&); // = delete;
  Module &operator=(const Module&); // = delete;

  iplist<Atom> Atoms;
  typedef std::map<StringRef, Atom*> AtomMap_t;
  AtomMap_t AtomMap;
  OwningPtr<ObjectFile> Represents;

public:
  Module(OwningPtr<ObjectFile> &from, error_code &ec);
  ~Module();

  Atom *getOrCreateAtom(StringRef name);
};

} // end namespace llvm
} // end namespace object

#endif
