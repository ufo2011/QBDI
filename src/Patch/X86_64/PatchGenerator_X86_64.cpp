/*
 * This file is part of QBDI.
 *
 * Copyright 2017 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vector>
#include <memory>

#include "X86InstrInfo.h"

#include "Patch/Patch.h"
#include "Patch/X86_64/InstInfo_X86_64.h"
#include "Patch/X86_64/Layer2_X86_64.h"
#include "Patch/X86_64/PatchGenerator_X86_64.h"
#include "Patch/X86_64/RelocatableInst_X86_64.h"
#include "Utility/LogSys.h"


namespace QBDI {

RelocatableInst::SharedPtrVec GetOperand::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    if(inst->getOperand(op).isReg()) {
        return {NoReloc(movrr(temp_manager->getRegForTemp(temp), inst->getOperand(op).getReg()))};
    }
    else if(inst->getOperand(op).isImm()) {
        return {Mov(temp_manager->getRegForTemp(temp), Constant(inst->getOperand(op).getImm()))};
    }
    else {
        LogError("GetOperand::generate", "Invalid operand type for GetOperand()");
        return {};
    }
}

RelocatableInst::SharedPtrVec GetConstant::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    return {Mov(temp_manager->getRegForTemp(temp), cst)};
}

RelocatableInst::SharedPtrVec GetPCOffset::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    switch (type) {
        case ConstantType:
            return{Mov(temp_manager->getRegForTemp(temp), Constant(address + instSize + cst))};
        case OperandType:
            // FIXME: Implement for register operand
            return {Mov(
                temp_manager->getRegForTemp(temp),
                Constant(address + instSize + inst->getOperand(op).getImm()))
            };
    }
}

RelocatableInst::SharedPtrVec GetReadAddress::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    // Check if this instruction does indeed read something
    unsigned size = getReadSize(inst);
    if(size > 0) {
        unsigned FormDesc = TSFlags & llvm::X86II::FormMask;
        // If it is a stack read, return RSP value
        if(isStackRead(inst)) {
            if (inst->getOpcode() == llvm::X86::LEAVE || inst->getOpcode() == llvm::X86::LEAVE64) {
                return {Mov(temp_manager->getRegForTemp(temp), Reg(REG_BP))};
            } else {
                return {Mov(temp_manager->getRegForTemp(temp), Reg(REG_SP))};
            }
        // specific case for instruction type cmpsb and movsb
        } else if (FormDesc == llvm::X86II::RawFrmDstSrc ||
                   FormDesc == llvm::X86II::RawFrmDst ||
                   FormDesc == llvm::X86II::RawFrmSrc) {
            RequireAction("GetReadAddress::generate", index < 2 && "Wrong index", abort());
            unsigned int reg;
            if (FormDesc == llvm::X86II::RawFrmSrc ||
                (FormDesc == llvm::X86II::RawFrmDstSrc && index == 0)) {
                // (R|E)SI
                reg = Reg(4);
                Require("GetReadAddress::generate", reg == llvm::X86::RSI || reg == llvm::X86::ESI);
            } else {
                // (R|E)DI
                reg = Reg(5);
                Require("GetReadAddress::generate", reg == llvm::X86::RDI || reg == llvm::X86::EDI);
            }
            return {NoReloc(movrr(temp_manager->getRegForTemp(temp), reg))};
        // Else replace the instruction with a LEA on the same address
        } else {
            // Scan for LLVM X86 address encoding
            for(unsigned i = 0; i + 4 <= inst->getNumOperands(); i++) {
                if(inst->getOperand(i + 0).isReg() && inst->getOperand(i + 1).isImm() &&
                   inst->getOperand(i + 2).isReg() && inst->getOperand(i + 3).isImm() &&
                   inst->getOperand(i + 4).isReg()) {
                    // If it uses PC as a base register, substitute PC
                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        return {
                            Mov(
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                Constant(address + instSize)
                            ),
                            NoReloc(lea(
                                temp_manager->getRegForTemp(temp),
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                inst->getOperand(i + 1).getImm(),
                                inst->getOperand(i + 2).getReg(),
                                inst->getOperand(i + 3).getImm(),
                                inst->getOperand(i + 4).getReg())
                            )
                        };
                    }
                    else {
                        return {
                            NoReloc(lea(
                                temp_manager->getRegForTemp(temp),
                                inst->getOperand(i + 0).getReg(),
                                inst->getOperand(i + 1).getImm(),
                                inst->getOperand(i + 2).getReg(),
                                inst->getOperand(i + 3).getImm(),
                                inst->getOperand(i + 4).getReg()
                            ))
                        };
                    }
                }
            }
            RequireAction("GetReadAddress::generate", false && "No memory address found in the instruction", abort());
        }
    }
    RequireAction("GetReadAddress::generate", false && "Called on an instruction which does not make read access", abort());
}

RelocatableInst::SharedPtrVec GetWriteAddress::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    // Check if this instruction does indeed read something
    unsigned size = getWriteSize(inst);
    if (size > 0) {
        unsigned FormDesc = TSFlags & llvm::X86II::FormMask;
        // If it is a stack read, return RSP value
        if(isStackWrite(inst)) {
            if (inst->getOpcode() == llvm::X86::ENTER) {
                return {Mov(temp_manager->getRegForTemp(temp), Reg(REG_BP))};
            } else {
                return {Mov(temp_manager->getRegForTemp(temp), Reg(REG_SP))};
            }
        } else if (FormDesc == llvm::X86II::RawFrmDstSrc ||
                   FormDesc == llvm::X86II::RawFrmDst ||
                   FormDesc == llvm::X86II::RawFrmSrc) {
            unsigned int reg;
            if (FormDesc == llvm::X86II::RawFrmSrc) {
                // (R|E)SI
                reg = Reg(4);
                Require("GetWriteAddress::generate", reg == llvm::X86::RSI || reg == llvm::X86::ESI);
            } else {
                // (R|E)DI
                reg = Reg(5);
                Require("GetWriteAddress::generate", reg == llvm::X86::RDI || reg == llvm::X86::EDI);
            }

            // if the instruction has a rep prefix, the memory address is get twice,
            // one before the instruction, one after. The computation of the real
            // memory access is done by the VM.
            if (hasREPPrefix(*inst))
                return {NoReloc(movrr(temp_manager->getRegForTemp(temp), reg))};

            unsigned shiftImm = 10;
            if(size == 8) {
                // if DF bit is set, shift 0x400 to 0X10 (8 * 2)
                shiftImm = 10 - 4;
            }
            else if(size == 4) {
                shiftImm = 10 - 3;
            }
            else if(size == 2) {
                shiftImm = 10 - 2;
            }
            else if(size == 1) {
                shiftImm = 10 - 1;
            }
            // The address in the register after the instruction is changed.
            // if DF == 0, reg == accessAddress + size
            // if DF == 1, reg == accessAddress - size
            return {
                Xchg(Reg(REG_SP), Offset(offsetof(Context, hostState.sp))),
                Pushf(),
                // load flag
                NoReloc(movrm(temp_manager->getRegForTemp(temp), Reg(REG_SP), 1, 0, 0, 0)),
                NoReloc(andri(temp_manager->getRegForTemp(temp), 0x0400)),
                // if DF is set, transform tmp to size * 2
                NoReloc(shrri(temp_manager->getRegForTemp(temp), shiftImm)),
                // compute the real address with reg + tmp - size
                // if DF == 0, get reg - size
                // if DF == 1, get reg + size
                NoReloc(lea(temp_manager->getRegForTemp(temp), reg, 1, temp_manager->getRegForTemp(temp), -size, 0)),
                // restore eflags (that may have been changed by the 'shift' and the 'and')
                Popf(),
                Xchg(Reg(REG_SP), Offset(offsetof(Context, hostState.sp))),
            };
        // Else replace the instruction with a LEA on the same address
        } else {
            // Scan for LLVM X86 address encoding
            for(unsigned i = 0; i + 4 <= inst->getNumOperands(); i++) {
                if(inst->getOperand(i + 0).isReg() && inst->getOperand(i + 1).isImm() &&
                   inst->getOperand(i + 2).isReg() && inst->getOperand(i + 3).isImm() &&
                   inst->getOperand(i + 4).isReg()) {
                    // If it uses PC as a base register, substitute PC
                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        return {
                            Mov(
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                Constant(address + instSize)
                            ),
                            NoReloc(lea(
                                temp_manager->getRegForTemp(temp),
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                inst->getOperand(i + 1).getImm(),
                                inst->getOperand(i + 2).getReg(),
                                inst->getOperand(i + 3).getImm(),
                                inst->getOperand(i + 4).getReg())
                            )
                        };
                    }
                    else {
                        return {
                            NoReloc(lea(
                                temp_manager->getRegForTemp(temp),
                                inst->getOperand(i + 0).getReg(),
                                inst->getOperand(i + 1).getImm(),
                                inst->getOperand(i + 2).getReg(),
                                inst->getOperand(i + 3).getImm(),
                                inst->getOperand(i + 4).getReg()
                            ))
                        };
                    }
                }
            }
            RequireAction("GetWriteAddress::generate", false && "No memory address found in the instruction", abort());
        }
    }
    RequireAction("GetWriteAddress::generate", false && "Called on an instruction which does not make write access", abort());
}

RelocatableInst::SharedPtrVec GetReadValue::generate(const llvm::MCInst* inst,
     rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    unsigned size = getReadSize(inst);
    if(size > 0) {
        unsigned dst = temp_manager->getRegForTemp(temp);
        unsigned FormDesc = TSFlags & llvm::X86II::FormMask;
        if(size < 8) {
            dst = temp_manager->getSizedSubReg(dst, 4);
        } else if (size > sizeof(rword)) {
            // read value size greater than 8 bytes isn't implemented
            return {NoReloc(movri(dst, 0))};
        }
        if(isStackRead(inst)) {
            llvm::MCInst readinst;
            unsigned int stack_register = REG_SP;
            if (inst->getOpcode() == llvm::X86::LEAVE || inst->getOpcode() == llvm::X86::LEAVE64) {
                stack_register = REG_BP;
            }
            if(size == 8) {
                readinst = mov64rm(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 4) {
                readinst = mov32rm(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 2) {
                readinst = mov32rm16(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 1) {
                readinst = mov32rm8(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            return {NoReloc(std::move(readinst))};
        } else if (FormDesc == llvm::X86II::RawFrmDstSrc ||
                   FormDesc == llvm::X86II::RawFrmDst ||
                   FormDesc == llvm::X86II::RawFrmSrc) {
            llvm::MCInst readinst;
            RequireAction("GetReadValue::generate", index < 2 && "Wrong index", abort());
            unsigned int reg;
            if (FormDesc == llvm::X86II::RawFrmSrc ||
                (FormDesc == llvm::X86II::RawFrmDstSrc && index == 0)) {
                // (R|E)SI
                reg = Reg(4);
                Require("GetReadValue::generate", reg == llvm::X86::RSI || reg == llvm::X86::ESI);
            } else {
                // (R|E)DI
                reg = Reg(5);
                Require("GetReadValue::generate", reg == llvm::X86::RDI || reg == llvm::X86::EDI);
            }
            if(size == 8) {
                readinst = {mov64rm(dst, reg, 1, 0, 0, 0)};
            }
            else if(size == 4) {
                readinst = {mov32rm(dst, reg, 1, 0, 0, 0)};
            }
            else if(size == 2) {
                readinst = {mov32rm16(dst, reg, 1, 0, 0, 0)};
            }
            else if(size == 1) {
                readinst = {mov32rm8(dst, reg, 1, 0, 0, 0)};
            }
            return {NoReloc(std::move(readinst))};
        } else {
            for(unsigned i = 0; i + 4 <= inst->getNumOperands(); i++) {
                if(inst->getOperand(i + 0).isReg() && inst->getOperand(i + 1).isImm() &&
                   inst->getOperand(i + 2).isReg() && inst->getOperand(i + 3).isImm() &&
                   inst->getOperand(i + 4).isReg()) {
                    llvm::MCInst readinst;

                    unsigned base = inst->getOperand(i + 0).getReg();
                    rword scale = inst->getOperand(i + 1).getImm();
                    unsigned offset = inst->getOperand(i + 2).getReg();
                    rword displacement = inst->getOperand(i + 3).getImm();
                    unsigned seg = inst->getOperand(i + 4).getReg();

                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        base = temp_manager->getRegForTemp(0xFFFFFFFF);
                    }

                    if(size == 8) {
                        readinst = mov64rm(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 4) {
                        readinst = mov32rm(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 2) {
                        readinst = mov32rm16(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 1) {
                        readinst = mov32rm8(dst, base, scale, offset, displacement, seg);
                    }

                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        return {
                            Mov(
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                Constant(address + instSize)
                            ),
                            NoReloc(std::move(readinst))
                        };
                    }
                    else {
                        return {
                            NoReloc(std::move(readinst))
                        };
                    }
                }
            }
            RequireAction("GetReadValue::generate", false && "No memory address found in the instruction", abort());
        }
    }
    RequireAction("GetReadValue::generate", false && "Called on an instruction which does not make read access", abort());
}

RelocatableInst::SharedPtrVec GetWriteValue::generate(const llvm::MCInst* inst,
     rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    unsigned size = getWriteSize(inst);
    if (size > 0) {
        unsigned dst = temp_manager->getRegForTemp(temp);
        unsigned FormDesc = TSFlags & llvm::X86II::FormMask;
        if(size < 8) {
            dst = temp_manager->getSizedSubReg(dst, 4);
        } else if (size > sizeof(rword)) {
            // write value size greater than 8 bytes isn't implemented
            return {NoReloc(movri(dst, 0))};
        }

        if(isStackWrite(inst)) {
            llvm::MCInst readinst;
            unsigned int stack_register = REG_SP;
            if (inst->getOpcode() == llvm::X86::ENTER) {
                stack_register = REG_BP;
            }
            if(size == 8) {
                readinst = mov64rm(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 4) {
                readinst = mov32rm(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 2) {
                readinst = mov32rm16(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            else if(size == 1) {
                readinst = mov32rm8(dst, Reg(stack_register), 1, 0, 0, 0);
            }
            return {NoReloc(std::move(readinst))};
        } else if (FormDesc == llvm::X86II::RawFrmDstSrc ||
                   FormDesc == llvm::X86II::RawFrmDst ||
                   FormDesc == llvm::X86II::RawFrmSrc) {
            // the computation of the address need to have DF,
            // As the address is already in temp, just dereference it.
            if(size == 8) {
                return {NoReloc(mov64rm(dst, temp_manager->getRegForTemp(temp), 1, 0, 0, 0))};
            }
            else if(size == 4) {
                return {NoReloc(mov32rm(dst, temp_manager->getRegForTemp(temp), 1, 0, 0, 0))};
            }
            else if(size == 2) {
                return {NoReloc(mov32rm16(dst, temp_manager->getRegForTemp(temp), 1, 0, 0, 0))};
            }
            else if(size == 1) {
                return {NoReloc(mov32rm8(dst, temp_manager->getRegForTemp(temp), 1, 0, 0, 0))};
            }
        } else {
            for(unsigned i = 0; i + 4 <= inst->getNumOperands(); i++) {
                if(inst->getOperand(i + 0).isReg() && inst->getOperand(i + 1).isImm() &&
                   inst->getOperand(i + 2).isReg() && inst->getOperand(i + 3).isImm() &&
                   inst->getOperand(i + 4).isReg()) {
                    llvm::MCInst readinst;

                    unsigned base = inst->getOperand(i + 0).getReg();
                    rword scale = inst->getOperand(i + 1).getImm();
                    unsigned offset = inst->getOperand(i + 2).getReg();
                    rword displacement = inst->getOperand(i + 3).getImm();
                    unsigned seg = inst->getOperand(i + 4).getReg();

                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        base = temp_manager->getRegForTemp(0xFFFFFFFF);
                    }

                    if(size == 8) {
                        readinst = mov64rm(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 4) {
                        readinst = mov32rm(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 2) {
                        readinst = mov32rm16(dst, base, scale, offset, displacement, seg);
                    }
                    else if(size == 1) {
                        readinst = mov32rm8(dst, base, scale, offset, displacement, seg);
                    }

                    if(inst->getOperand(i + 0).getReg() == Reg(REG_PC)) {
                        return {
                            Mov(
                                temp_manager->getRegForTemp(0xFFFFFFFF),
                                Constant(address + instSize)
                            ),
                            NoReloc(std::move(readinst))
                        };
                    }
                    else {
                        return {
                            NoReloc(std::move(readinst))
                        };
                    }
                }
            }
            RequireAction("GetWriteValue::generate", false && "No memory address found in the instruction", abort());
        }
    }
    RequireAction("GetWriteValue::generate", false && "Called on an instruction which does not make write access", abort());
}

RelocatableInst::SharedPtrVec GetInstId::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    return {InstId(movri(temp_manager->getRegForTemp(temp), 0), 1)};
}

RelocatableInst::SharedPtrVec WriteTemp::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    switch (type) {
        case OffsetType:
            return {Mov(offset, temp_manager->getRegForTemp(temp))};
        case ShadowType:
            return {TaggedShadowx86(
                movmr(0, 0, 0, 0, 0, temp_manager->getRegForTemp(temp)),
                0,
                shadow.getTag(),
                7
            )};
    }
}

RelocatableInst::SharedPtrVec SaveReg::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    return {Mov(offset, reg)};
}

RelocatableInst::SharedPtrVec LoadReg::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    return {Mov(reg, offset)};
}

RelocatableInst::SharedPtrVec JmpEpilogue::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {

    return {EpilogueRel(jmp(0), 0, -1)};
}

RelocatableInst::SharedPtrVec SimulateCall::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    RelocatableInst::SharedPtrVec patch;

    append(patch, WriteTemp(temp, Offset(Reg(REG_PC))).generate(inst, address, instSize, temp_manager, nullptr));
    append(patch, GetPCOffset(temp, Constant(0)).generate(inst, address, instSize, temp_manager, nullptr));
    patch.push_back(Pushr(temp_manager->getRegForTemp(temp)));

    return {patch};
}

RelocatableInst::SharedPtrVec SimulateRet::generate(const llvm::MCInst* inst,
    rword address, rword instSize, TempManager *temp_manager, const Patch *toMerge) const {
    RelocatableInst::SharedPtrVec patch;

    patch.push_back(Popr(temp_manager->getRegForTemp(temp)));
    if(inst->getNumOperands() == 1 && inst->getOperand(0).isImm()) {
        patch.push_back(Add(Reg(REG_SP), Constant(inst->getOperand(0).getImm())));
    }
    append(patch, WriteTemp(temp, Offset(Reg(REG_PC))).generate(inst, address, instSize, temp_manager, nullptr));

    return {patch};
}


}
