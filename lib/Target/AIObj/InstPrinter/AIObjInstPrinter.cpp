//===-- AIObjInstPrinter.cpp - Convert AIObj MCInst to assembly syntax ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class prints a AIObj MCInst to a ai.obj file.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asm-printer"
#include "AIObjInstPrinter.h"
#include "MCTargetDesc/AIObjMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define GET_INSTRUCTION_NAME
#include "AIObjGenAsmWriter.inc"

AIObjInstPrinter::AIObjInstPrinter(const MCAsmInfo &MAI,
                               const MCSubtargetInfo &STI) :
  MCInstPrinter(MAI) {
  // Initialize the set of available features.
  setAvailableFeatures(STI.getFeatureBits());
}

StringRef AIObjInstPrinter::getOpcodeName(unsigned Opcode) const {
  return getInstructionName(Opcode);
}

void AIObjInstPrinter::printRegName(raw_ostream &OS, unsigned RegNo) const {
  // Do nothing, we have no registers.
}

void AIObjInstPrinter::printInst(const MCInst *MI, raw_ostream &O,
                               StringRef Annot) {
  switch (MI->getOpcode()) {
  default:
    printInstruction(MI, O);
    break;
  case AIObj::FUNCTION_CALL_VOID:
    O << "\tfunc_call " << MI->getOperand(0).getImm();
    break;
  case AIObj::FUNCTION_CALL:
    O << "\tfunc_call " << MI->getOperand(1).getImm();
  }
  printAnnotation(O, Annot);
}

void AIObjInstPrinter::printCall(const MCInst *MI, raw_ostream &O) {
  O << "\tfunc_call ";
  O << MI->getOperand(1).getImm();
}

void AIObjInstPrinter::printOperand( const MCInst *MI
                                   , unsigned OpNo
                                   , raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  assert(Op.isImm() && "AIObj only supports immidiate operands!");
  O << Op.getImm();
}
