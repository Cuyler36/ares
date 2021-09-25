auto CPU::Recompiler::pool(u32 address) -> Pool* {
  auto& pool = pools[address >> 8 & 0x1fffff];
  if(!pool) pool = (Pool*)allocator.acquire(sizeof(Pool));
  return pool;
}

auto CPU::Recompiler::block(u32 address) -> Block* {
  if(auto block = pool(address)->blocks[address >> 2 & 0x3f]) return block;
  auto block = emit(address);
  return pool(address)->blocks[address >> 2 & 0x3f] = block;
}

auto CPU::Recompiler::emit(u32 address) -> Block* {
  if(unlikely(allocator.available() < 1_MiB)) {
    print("CPU allocator flush\n");
    allocator.release(bump_allocator::zero_fill);
    reset();
  }

  auto block = (Block*)allocator.acquire(sizeof(Block));
  block->code = allocator.acquire();
  bind({block->code, allocator.available()});
  push(rbx);
  push(rbp);
  push(r13);
  if constexpr(ABI::Windows) {
    push(rsi);
    push(rdi);
    sub(rsp, imm8(0x40));
  }
  mov(rbx, imm64(&self.ipu.r[0] + 16));
  mov(rbp, imm64(&self));
  mov(r13, imm64(&self.fpu.r[0] + 16));

  jmp(imm8(ABI::Windows ? 11 : 5));

  u32 epilogue = size();

  if constexpr(ABI::Windows) {
    add(rsp, imm8(0x40));
    pop(rdi);
    pop(rsi);
  }
  pop(r13);
  pop(rbp);
  pop(rbx);
  ret();

  bool hasBranched = 0;
  while(true) {
    u32 instruction = bus.read<Word>(address);
    bool branched = emitEXECUTE(instruction);
    if(unlikely(instruction == 0x1000'ffff)) {
      //accelerate idle loops
      mov(rax, mem64(&self.clock));
      add(rax, imm8(64));
      mov(mem64(&self.clock), rax);
    }
    call(&CPU::instructionEpilogue);
    address += 4;
    if(hasBranched || (address & 0xfc) == 0) break;  //block boundary
    hasBranched = branched;
    test(rax, rax);
    jnz(imm32(epilogue - size() - 6));
  }
  jmp(imm32(epilogue - size() - 5));

  allocator.reserve(size());
//print(hex(PC, 8L), " ", instructions, " ", size(), "\n");
  return block;
}

#define Sa  (instruction >>  6 & 31)
#define Rdn (instruction >> 11 & 31)
#define Rtn (instruction >> 16 & 31)
#define Rsn (instruction >> 21 & 31)
#define Fdn (instruction >>  6 & 31)
#define Fsn (instruction >> 11 & 31)
#define Ftn (instruction >> 16 & 31)
#define Rd  dis8(rbx, (Rdn - 16) * 8)
#define Rt  dis8(rbx, (Rtn - 16) * 8)
#define Rs  dis8(rbx, (Rsn - 16) * 8)
#define Fd  dis8(r13, (Fdn - 16) * 8)
#define Fs  dis8(r13, (Fsn - 16) * 8)
#define Ft  dis8(r13, (Ftn - 16) * 8)
#define i16 s16(instruction)
#define n16 u16(instruction)
#define n26 u32(instruction & 0x03ff'ffff)

auto CPU::Recompiler::emitEXECUTE(u32 instruction) -> bool {
  switch(instruction >> 26) {

  //SPECIAL
  case 0x00: {
    return emitSPECIAL(instruction);
  }

  //REGIMM
  case 0x01: {
    return emitREGIMM(instruction);
  }

  //J n26
  case 0x02: {
    mov(esi, imm32(n26));
    call(&CPU::J);
    return 1;
  }

  //JAL n26
  case 0x03: {
    mov(esi, imm32(n26));
    call(&CPU::JAL);
    return 1;
  }

  //BEQ Rs,Rt,i16
  case 0x04: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    mov(ecx, imm32(i16));
    call(&CPU::BEQ);
    return 1;
  }

  //BNE Rs,Rt,i16
  case 0x05: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    mov(ecx, imm32(i16));
    call(&CPU::BNE);
    return 1;
  }

  //BLEZ Rs,i16
  case 0x06: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLEZ);
    return 1;
  }

  //BGTZ Rs,i16
  case 0x07: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGTZ);
    return 1;
  }

  //ADDI Rt,Rs,i16
  case 0x08: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::ADDI);
    return 0;
  }

  //ADDIU Rt,Rs,i16
  case 0x09: {
    mov(esi, Rs);
    add(esi, imm32(i16));
    movsxd(rsi, esi);
    mov(Rt, rsi);
    return 0;
  }

  //SLTI Rt,Rs,i16
  case 0x0a: {
    mov(rsi, Rs);
    mov(edx, imm32(i16));
    movsxd(rdx, edx);
    cmp(rsi, rdx);
    setl(al);
    movzx(eax, al);
    mov(Rt, rax);
    return 0;
  }

  //SLTIU Rt,Rs,i16
  case 0x0b: {
    mov(rsi, Rs);
    mov(edx, imm32(i16));
    movsxd(rdx, edx);
    cmp(rsi, rdx);
    setb(al);
    movzx(eax, al);
    mov(Rt, rax);
    return 0;
  }

  //ANDI Rt,Rs,n16
  case 0x0c: {
    mov(rsi, Rs);
    and(rsi, imm32(n16));
    mov(Rt, rsi);
    return 0;
  }

  //ORI Rt,Rs,n16
  case 0x0d: {
    mov(rsi, Rs);
    or(rsi, imm32(n16));
    mov(Rt, rsi);
    return 0;
  }

  //XORI Rt,Rs,n16
  case 0x0e: {
    mov(rsi, Rs);
    xor(rsi, imm32(n16));
    mov(Rt, rsi);
    return 0;
  }

  //LUI Rt,n16
  case 0x0f: {
    mov(esi, imm32(n16 << 16));
    movsxd(rsi, esi);
    mov(Rt, rsi);
    return 0;
  }

  //SCC
  case 0x10: {
    return emitSCC(instruction);
  }

  //FPU
  case 0x11: {
    return emitFPU(instruction);
  }

  //COP2
  case 0x12: {
    call(&CPU::COP2);
    return 1;
  }

  //COP3
  case 0x13: {
    call(&CPU::COP3);
    return 1;
  }

  //BEQL Rs,Rt,i16
  case 0x14: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    mov(ecx, imm32(i16));
    call(&CPU::BEQL);
    return 1;
  }

  //BNEL Rs,Rt,i16
  case 0x15: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    mov(ecx, imm32(i16));
    call(&CPU::BNEL);
    return 1;
  }

  //BLEZL Rs,i16
  case 0x16: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLEZL);
    return 1;
  }

  //BGTZL Rs,i16
  case 0x17: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGTZL);
    return 1;
  }

  //DADDI Rt,Rs,i16
  case 0x18: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::DADDI);
    return 0;
  }

  //DADDIU Rt,Rs,i16
  case 0x19: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::DADDIU);
    return 0;
  }

  //LDL Rt,Rs,i16
  case 0x1a: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LDL);
    return 0;
  }

  //LDR Rt,Rs,i16
  case 0x1b: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LDR);
    return 0;
  }

  //INVALID
  case 0x1c ... 0x1f: {
    call(&CPU::INVALID);
    return 1;
  }

  //LB Rt,Rs,i16
  case 0x20: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LB);
    return 0;
  }

  //LH Rt,Rs,i16
  case 0x21: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LH);
    return 0;
  }

  //LWL Rt,Rs,i16
  case 0x22: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LWL);
    return 0;
  }

  //LW Rt,Rs,i16
  case 0x23: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LW);
    return 0;
  }

  //LBU Rt,Rs,i16
  case 0x24: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LBU);
    return 0;
  }

  //LHU Rt,Rs,i16
  case 0x25: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LHU);
    return 0;
  }

  //LWR Rt,Rs,i16
  case 0x26: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LWR);
    return 0;
  }

  //LWU Rt,Rs,i16
  case 0x27: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LWU);
    return 0;
  }

  //SB Rt,Rs,i16
  case 0x28: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SB);
    return 0;
  }

  //SH Rt,Rs,i16
  case 0x29: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SH);
    return 0;
  }

  //SWL Rt,Rs,i16
  case 0x2a: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SWL);
    return 0;
  }

  //SW Rt,Rs,i16
  case 0x2b: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SW);
    return 0;
  }

  //SDL Rt,Rs,i16
  case 0x2c: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SDL);
    return 0;
  }

  //SDR Rt,Rs,i16
  case 0x2d: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SDR);
    return 0;
  }

  //SWR Rt,Rs,i16
  case 0x2e: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SWR);
    return 0;
  }

  //CACHE op(offset),base
  case 0x2f: {
    mov(esi, imm32(instruction >> 16 & 31));
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::CACHE);
    return 0;
  }

  //LL Rt,Rs,i16
  case 0x30: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LL);
    return 0;
  }

  //LWC1 Ft,Rs,i16
  case 0x31: {
    mov(esi, imm32(Ftn));
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LWC1);
    return 0;
  }

  //LWC2
  case 0x32: {
    call(&CPU::COP2);
    return 1;
  }

  //LWC3
  case 0x33: {
    call(&CPU::COP3);
    return 1;
  }

  //LLD Rt,Rs,i16
  case 0x34: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LLD);
    return 0;
  }

  //LDC1 Ft,Rs,i16
  case 0x35: {
    mov(esi, imm32(Ftn));
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LDC1);
    return 0;
  }

  //LDC2
  case 0x36: {
    call(&CPU::COP2);
    return 1;
  }

  //LD Rt,Rs,i16
  case 0x37: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::LD);
    return 0;
  }

  //SC Rt,Rs,i16
  case 0x38: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SC);
    return 0;
  }

  //SWC1 Ft,Rs,i16
  case 0x39: {
    mov(esi, imm32(Ftn));
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SWC1);
    return 0;
  }

  //SWC2
  case 0x3a: {
    call(&CPU::COP2);
    return 1;
  }

  //SWC3
  case 0x3b: {
    call(&CPU::COP3);
    return 1;
  }

  //SCD Rt,Rs,i16
  case 0x3c: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SCD);
    return 0;
  }

  //SDC1 Ft,Rs,i16
  case 0x3d: {
    mov(esi, imm32(Ftn));
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SDC1);
    return 0;
  }

  //SDC2
  case 0x3e: {
    call(&CPU::COP2);
    return 1;
  }

  //SD Rt,Rs,i16
  case 0x3f: {
    lea(rsi, Rt);
    lea(rdx, Rs);
    mov(ecx, imm32(i16));
    call(&CPU::SD);
    return 0;
  }

  }

  return 0;
}

auto CPU::Recompiler::emitSPECIAL(u32 instruction) -> bool {
  switch(instruction & 0x3f) {

  //SLL Rd,Rt,Sa
  case 0x00: {
    mov(esi, Rt);
    shl(esi, imm8(Sa));
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //INVALID
  case 0x01: {
    call(&CPU::INVALID);
    return 1;
  }

  //SRL Rd,Rt,Sa
  case 0x02: {
    mov(esi, Rt);
    shr(esi, imm8(Sa));
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //SRA Rd,Rt,Sa
  case 0x03: {
    mov(esi, Rt);
    sar(esi, imm8(Sa));
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //SLLV Rd,Rt,Rs
  case 0x04: {
    mov(esi, Rt);
    mov(cl, Rs);
    and(cl, imm8(31));
    shl(esi, cl);
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //INVALID
  case 0x05: {
    call(&CPU::INVALID);
    return 1;
  }

  //SRLV Rd,Rt,RS
  case 0x06: {
    mov(esi, Rt);
    mov(cl, Rs);
    and(cl, imm8(31));
    shr(esi, cl);
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //SRAV Rd,Rt,Rs
  case 0x07: {
    mov(esi, Rt);
    mov(cl, Rs);
    and(cl, imm8(31));
    sar(esi, cl);
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //JR Rs
  case 0x08: {
    lea(rsi, Rs);
    call(&CPU::JR);
    return 1;
  }

  //JALR Rd,Rs
  case 0x09: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    call(&CPU::JALR);
    return 1;
  }

  //INVALID
  case 0x0a ... 0x0b: {
    call(&CPU::INVALID);
    return 1;
  }

  //SYSCALL
  case 0x0c: {
    call(&CPU::SYSCALL);
    return 1;
  }

  //BREAK
  case 0x0d: {
    call(&CPU::BREAK);
    return 1;
  }

  //INVALID
  case 0x0e: {
    call(&CPU::INVALID);
    return 1;
  }

  //SYNC
  case 0x0f: {
    call(&CPU::SYNC);
    return 0;
  }

  //MFHI Rd
  case 0x10: {
    mov(rax, mem64(&cpu.ipu.hi));
    mov(Rd, rax);
    return 0;
  }

  //MTHI Rs
  case 0x11: {
    mov(rax, Rs);
    mov(mem64(&cpu.ipu.hi), rax);
    return 0;
  }

  //MFLO Rd
  case 0x12: {
    mov(rax, mem64(&cpu.ipu.lo));
    mov(Rd, rax);
    return 0;
  }

  //MTLO Rs
  case 0x13: {
    mov(rax, Rs);
    mov(mem64(&cpu.ipu.lo), rax);
    return 0;
  }

  //DSLLV Rd,Rt,Rs
  case 0x14: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    lea(rcx, Rs);
    call(&CPU::DSLLV);
    return 0;
  }

  //INVALID
  case 0x15: {
    call(&CPU::INVALID);
    return 1;
  }

  //DSRLV Rd,Rt,Rs
  case 0x16: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    lea(rcx, Rs);
    call(&CPU::DSRLV);
    return 0;
  }

  //DSRAV Rd,Rt,Rs
  case 0x17: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    lea(rcx, Rs);
    call(&CPU::DSRAV);
    return 0;
  }

  //MULT Rs,Rt
  case 0x18: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::MULT);
    return 0;
  }

  //MULTU Rs,Rt
  case 0x19: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::MULTU);
    return 0;
  }

  //DIV Rs,Rt
  case 0x1a: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DIV);
    return 0;
  }

  //DIVU Rs,Rt
  case 0x1b: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DIVU);
    return 0;
  }

  //DMULT Rs,Rt
  case 0x1c: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DMULT);
    return 0;
  }

  //DMULTU Rs,Rt
  case 0x1d: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DMULTU);
    return 0;
  }

  //DDIV Rs,Rt
  case 0x1e: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DDIV);
    return 0;
  }

  //DDIVU Rs,Rt
  case 0x1f: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::DDIVU);
    return 0;
  }

  //ADD Rd,Rs,Rt
  case 0x20: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::ADD);
    return 0;
  }

  //ADDU Rd,Rs,Rt
  case 0x21: {
    mov(esi, Rs);
    add(esi, Rt);
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //SUB Rd,Rs,Rt
  case 0x22: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::SUB);
    return 0;
  }

  //SUBU Rd,Rs,Rt
  case 0x23: {
    mov(esi, Rs);
    sub(esi, Rt);
    movsxd(rsi, esi);
    mov(Rd, rsi);
    return 0;
  }

  //AND Rd,Rs,Rt
  case 0x24: {
    mov(rsi, Rs);
    and(rsi, Rt);
    mov(Rd, rsi);
    return 0;
  }

  //OR Rd,Rs,Rt
  case 0x25: {
    mov(rsi, Rs);
    or(rsi, Rt);
    mov(Rd, rsi);
    return 0;
  }

  //XOR Rd,Rs,Rt
  case 0x26: {
    mov(rsi, Rs);
    xor(rsi, Rt);
    mov(Rd, rsi);
    return 0;
  }

  //NOR Rd,Rs,Rt
  case 0x27: {
    mov(rsi, Rs);
    or(rsi, Rt);
    not(rsi);
    mov(Rd, rsi);
    return 0;
  }

  //INVALID
  case 0x28 ... 0x29: {
    call(&CPU::INVALID);
    return 1;
  }

  //SLT Rd,Rs,Rt
  case 0x2a: {
    mov(rsi, Rs);
    cmp(rsi, Rt);
    setl(al);
    movzx(eax, al);
    mov(Rd, rax);
    return 0;
  }

  //SLTU Rd,Rs,Rt
  case 0x2b: {
    mov(rsi, Rs);
    cmp(rsi, Rt);
    setb(al);
    movzx(eax, al);
    mov(Rd, rax);
    return 0;
  }

  //DADD Rd,Rs,Rt
  case 0x2c: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::DADD);
    return 0;
  }

  //DADDU Rd,Rs,Rt
  case 0x2d: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::DADDU);
    return 0;
  }

  //DSUB Rd,Rs,Rt
  case 0x2e: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::DSUB);
    return 0;
  }

  //DSUBU Rd,Rs,Rt
  case 0x2f: {
    lea(rsi, Rd);
    lea(rdx, Rs);
    lea(rcx, Rt);
    call(&CPU::DSUBU);
    return 0;
  }

  //TGE Rs,Rt
  case 0x30: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TGE);
    return 0;
  }

  //TGEU Rs,Rt
  case 0x31: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TGEU);
    return 0;
  }

  //TLT Rs,Rt
  case 0x32: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TLT);
    return 0;
  }

  //TLTU Rs,Rt
  case 0x33: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TLTU);
    return 0;
  }

  //TEQ Rs,Rt
  case 0x34: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TEQ);
    return 0;
  }

  //INVALID
  case 0x35: {
    call(&CPU::INVALID);
    return 1;
  }

  //TNE Rs,Rt
  case 0x36: {
    lea(rsi, Rs);
    lea(rdx, Rt);
    call(&CPU::TNE);
    return 0;
  }

  //INVALID
  case 0x37: {
    call(&CPU::INVALID);
    return 1;
  }

  //DSLL Rd,Rt,Sa
  case 0x38: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa));
    call(&CPU::DSLL);
    return 0;
  }

  //INVALID
  case 0x39: {
    call(&CPU::INVALID);
    return 1;
  }

  //DSRL Rd,Rt,Sa
  case 0x3a: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa));
    call(&CPU::DSRL);
    return 0;
  }

  //DSRA Rd,Rt,Sa
  case 0x3b: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa));
    call(&CPU::DSRA);
    return 0;
  }

  //DSLL32 Rd,Rt,Sa
  case 0x3c: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa+32));
    call(&CPU::DSLL);
    return 0;
  }

  //INVALID
  case 0x3d: {
    call(&CPU::INVALID);
    return 1;
  }

  //DSRL32 Rd,Rt,Sa
  case 0x3e: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa+32));
    call(&CPU::DSRL);
    return 0;
  }

  //DSRA32 Rd,Rt,Sa
  case 0x3f: {
    lea(rsi, Rd);
    lea(rdx, Rt);
    mov(ecx, imm32(Sa+32));
    call(&CPU::DSRA);
    return 0;
  }

  }

  return 0;
}

auto CPU::Recompiler::emitREGIMM(u32 instruction) -> bool {
  switch(instruction >> 16 & 0x1f) {

  //BLTZ Rs,i16
  case 0x00: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLTZ);
    return 0;
  }

  //BGEZ Rs,i16
  case 0x01: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGEZ);
    return 0;
  }

  //BLTZL Rs,i16
  case 0x02: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLTZL);
    return 0;
  }

  //BGEZL Rs,i16
  case 0x03: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGEZL);
    return 0;
  }

  //INVALID
  case 0x04 ... 0x07: {
    call(&CPU::INVALID);
    return 1;
  }

  //TGEI Rs,i16
  case 0x08: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TGEI);
    return 0;
  }

  //TGEIU Rs,i16
  case 0x09: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TGEIU);
    return 0;
  }

  //TLTI Rs,i16
  case 0x0a: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TLTI);
    return 0;
  }

  //TLTIU Rs,i16
  case 0x0b: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TLTIU);
    return 0;
  }

  //TEQI Rs,i16
  case 0x0c: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TEQI);
    return 0;
  }

  //INVALID
  case 0x0d: {
    call(&CPU::INVALID);
    return 1;
  }

  //TNEI Rs,i16
  case 0x0e: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::TNEI);
    return 0;
  }

  //INVALID
  case 0x0f: {
    call(&CPU::INVALID);
    return 1;
  }

  //BLTZAL Rs,i16
  case 0x10: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLTZAL);
    return 0;
  }

  //BGEZAL Rs,i16
  case 0x11: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGEZAL);
    return 0;
  }

  //BLTZALL Rs,i16
  case 0x12: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BLTZALL);
    return 0;
  }

  //BGEZALL Rs,i16
  case 0x13: {
    lea(rsi, Rs);
    mov(edx, imm32(i16));
    call(&CPU::BGEZALL);
    return 0;
  }

  //INVALID
  case 0x14 ... 0x1f: {
    call(&CPU::INVALID);
    return 1;
  }

  }

  return 0;
}

auto CPU::Recompiler::emitSCC(u32 instruction) -> bool {
  switch(instruction >> 21 & 0x1f) {

  //MFC0 Rt,Rd
  case 0x00: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::MFC0);
    return 0;
  }

  //DMFC0 Rt,Rd
  case 0x01: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::DMFC0);
    return 0;
  }

  //INVALID
  case 0x02 ... 0x03: {
    call(&CPU::INVALID);
    return 1;
  }

  //MTC0 Rt,Rd
  case 0x04: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::MTC0);
    return 0;
  }

  //DMTC0 Rt,Rd
  case 0x05: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::DMTC0);
    return 0;
  }

  //INVALID
  case 0x06 ... 0x0f: {
    call(&CPU::INVALID);
    return 1;
  }

  }

  switch(instruction & 0x3f) {

  //TLBR
  case 0x01: {
    call(&CPU::TLBR);
    return 0;
  }

  //TLBWI
  case 0x02: {
    call(&CPU::TLBWI);
    return 0;
  }

  //TLBWR
  case 0x06: {
    call(&CPU::TLBWR);
    return 0;
  }

  //TLBP
  case 0x08: {
    call(&CPU::TLBP);
    return 0;
  }

  //ERET
  case 0x18: {
    call(&CPU::ERET);
    return 1;
  }

  }

  return 0;
}

auto CPU::Recompiler::emitFPU(u32 instruction) -> bool {
  switch(instruction >> 21 & 0x1f) {

  //MFC1 Rt,Fs
  case 0x00: {
    lea(rsi, Rt);
    mov(edx, imm32(Fsn));
    call(&CPU::MFC1);
    return 0;
  }

  //DMFC1 Rt,Fs
  case 0x01: {
    lea(rsi, Rt);
    mov(edx, imm32(Fsn));
    call(&CPU::DMFC1);
    return 0;
  }

  //CFC1 Rt,Rd
  case 0x02: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::CFC1);
    return 0;
  }

  //INVALID
  case 0x03: {
    call(&CPU::INVALID);
    return 1;
  }

  //MTC1 Rt,Fs
  case 0x04: {
    lea(rsi, Rt);
    mov(edx, imm32(Fsn));
    call(&CPU::MTC1);
    return 0;
  }

  //DMTC1 Rt,Fs
  case 0x05: {
    lea(rsi, Rt);
    mov(edx, imm32(Fsn));
    call(&CPU::DMTC1);
    return 0;
  }

  //CTC1 Rt,Rd
  case 0x06: {
    lea(rsi, Rt);
    mov(edx, imm32(Rdn));
    call(&CPU::CTC1);
    return 0;
  }

  //INVALID
  case 0x07: {
    call(&CPU::INVALID);
    return 1;
  }

  //BC1 offset
  case 0x08: {
    mov(esi, imm32(instruction >> 16 & 1));
    mov(edx, imm32(instruction >> 17 & 1));
    mov(ecx, imm32(i16));
    call(&CPU::BC1);
    return 1;
  }

  //INVALID
  case 0x09 ... 0x0f: {
    call(&CPU::INVALID);
    return 1;
  }

  }

  if((instruction >> 21 & 31) == 16)
  switch(instruction & 0x3f) {

  //FADD.S Fd,Fs,Ft
  case 0x00: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FADD_S);
    return 0;
  }

  //FSUB.S Fd,Fs,Ft
  case 0x01: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FSUB_S);
    return 0;
  }

  //FMUL.S Fd,Fs,Ft
  case 0x02: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FMUL_S);
    return 0;
  }

  //FDIV.S Fd,Fs,Ft
  case 0x03: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FDIV_S);
    return 0;
  }

  //FSQRT.S Fd,Fs
  case 0x04: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FSQRT_S);
    return 0;
  }

  //FABS.S Fd,Fs
  case 0x05: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FABS_S);
    return 0;
  }

  //FMOV.S Fd,Fs
  case 0x06: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FMOV_S);
    return 0;
  }

  //FNEG.S Fd,Fs
  case 0x07: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FNEG_S);
    return 0;
  }

  //FROUND.L.S Fd,Fs
  case 0x08: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FROUND_L_S);
    return 0;
  }

  //FTRUNC.L.S Fd,Fs
  case 0x09: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FTRUNC_L_S);
    return 0;
  }

  //FCEIL.L.S Fd,Fs
  case 0x0a: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCEIL_L_S);
    return 0;
  }

  //FFLOOR.L.S Fd,Fs
  case 0x0b: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FFLOOR_L_S);
    return 0;
  }

  //FROUND.W.S Fd,Fs
  case 0x0c: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FROUND_W_S);
    return 0;
  }

  //FTRUNC.W.S Fd,Fs
  case 0x0d: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FTRUNC_W_S);
    return 0;
  }

  //FCEIL.W.S Fd,Fs
  case 0x0e: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCEIL_W_S);
    return 0;
  }

  //FFLOOR.W.S Fd,Fs
  case 0x0f: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FFLOOR_W_S);
    return 0;
  }

  //FCVT.D.S Fd,Fs
  case 0x21: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_D_S);
    return 0;
  }

  //FCVT.W.S Fd,Fs
  case 0x24: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_W_S);
    return 0;
  }

  //FCVT.L.S Fd,Fs
  case 0x25: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_L_S);
    return 0;
  }

  //FC.F.S Fs,Ft
  case 0x30: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_F_S);
    return 0;
  }

  //FC.UN.S Fs,Ft
  case 0x31: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_UN_S);
    return 0;
  }

  //FC.EQ.S Fs,Ft
  case 0x32: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_EQ_S);
    return 0;
  }

  //FC.UEQ.S Fs,Ft
  case 0x33: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_UEQ_S);
    return 0;
  }

  //FC.OLT.S Fs,Ft
  case 0x34: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_OLT_S);
    return 0;
  }

  //FC.ULT.S Fs,Ft
  case 0x35: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_ULT_S);
    return 0;
  }

  //FC.OLE.S Fs,Ft
  case 0x36: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_OLE_S);
    return 0;
  }

  //FC.ULE.S Fs,Ft
  case 0x37: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_ULE_S);
    return 0;
  }

  //FC.SF.S Fs,Ft
  case 0x38: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_SF_S);
    return 0;
  }

  //FC.NGLE.S Fs,Ft
  case 0x39: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGLE_S);
    return 0;
  }

  //FC.SEQ.S Fs,Ft
  case 0x3a: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_SEQ_S);
    return 0;
  }

  //FC.NGL.S Fs,Ft
  case 0x3b: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGL_S);
    return 0;
  }

  //FC.LT.S Fs,Ft
  case 0x3c: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_LT_S);
    return 0;
  }

  //FC.NGE.S Fs,Ft
  case 0x3d: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGE_S);
    return 0;
  }

  //FC.LE.S Fs,Ft
  case 0x3e: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_LE_S);
    return 0;
  }

  //FC.NGT.S Fs,Ft
  case 0x3f: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGT_S);
    return 0;
  }

  }

  if((instruction >> 21 & 31) == 17)
  switch(instruction & 0x3f) {

  //FADD.D Fd,Fs,Ft
  case 0x00: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FADD_D);
    return 0;
  }

  //FSUB.D Fd,Fs,Ft
  case 0x01: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FSUB_D);
    return 0;
  }

  //FMUL.D Fd,Fs,Ft
  case 0x02: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FMUL_D);
    return 0;
  }

  //FDIV.D Fd,Fs,Ft
  case 0x03: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    mov(ecx, imm32(Ftn));
    call(&CPU::FDIV_D);
    return 0;
  }

  //FSQRT.D Fd,Fs
  case 0x04: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FSQRT_D);
    return 0;
  }

  //FABS.D Fd,Fs
  case 0x05: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FABS_D);
    return 0;
  }

  //FMOV.D Fd,Fs
  case 0x06: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FMOV_D);
    return 0;
  }

  //FNEG.D Fd,Fs
  case 0x07: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FNEG_D);
    return 0;
  }

  //FROUND.L.D Fd,Fs
  case 0x08: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FROUND_L_D);
    return 0;
  }

  //FTRUNC.L.D Fd,Fs
  case 0x09: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FTRUNC_L_D);
    return 0;
  }

  //FCEIL.L.D Fd,Fs
  case 0x0a: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCEIL_L_D);
    return 0;
  }

  //FFLOOR.L.D Fd,Fs
  case 0x0b: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FFLOOR_L_D);
    return 0;
  }

  //FROUND.W.D Fd,Fs
  case 0x0c: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FROUND_W_D);
    return 0;
  }

  //FTRUNC.W.D Fd,Fs
  case 0x0d: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FTRUNC_W_D);
    return 0;
  }

  //FCEIL.W.D Fd,Fs
  case 0x0e: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCEIL_W_D);
    return 0;
  }

  //FFLOOR.W.D Fd,Fs
  case 0x0f: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FFLOOR_W_D);
    return 0;
  }

  //FCVT.S.D Fd,Fs
  case 0x20: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_S_D);
    return 0;
  }

  //FCVT.W.D Fd,Fs
  case 0x24: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_W_D);
    return 0;
  }

  //FCVT.L.D Fd,Fs
  case 0x25: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_L_D);
    return 0;
  }

  //FC.F.D Fs,Ft
  case 0x30: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_F_D);
    return 0;
  }

  //FC.UN.D Fs,Ft
  case 0x31: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_UN_D);
    return 0;
  }

  //FC.EQ.D Fs,Ft
  case 0x32: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_EQ_D);
    return 0;
  }

  //FC.UEQ.D Fs,Ft
  case 0x33: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_UEQ_D);
    return 0;
  }

  //FC.OLT.D Fs,Ft
  case 0x34: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_OLT_D);
    return 0;
  }

  //FC.ULT.D Fs,Ft
  case 0x35: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_ULT_D);
    return 0;
  }

  //FC.OLE.D Fs,Ft
  case 0x36: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_OLE_D);
    return 0;
  }

  //FC.ULE.D Fs,Ft
  case 0x37: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_ULE_D);
    return 0;
  }

  //FC.SF.D Fs,Ft
  case 0x38: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_SF_D);
    return 0;
  }

  //FC.NGLE.D Fs,Ft
  case 0x39: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGLE_D);
    return 0;
  }

  //FC.SEQ.D Fs,Ft
  case 0x3a: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_SEQ_D);
    return 0;
  }

  //FC.NGL.D Fs,Ft
  case 0x3b: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGL_D);
    return 0;
  }

  //FC.LT.D Fs,Ft
  case 0x3c: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_LT_D);
    return 0;
  }

  //FC.NGE.D Fs,Ft
  case 0x3d: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGE_D);
    return 0;
  }

  //FC.LE.D Fs,Ft
  case 0x3e: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_LE_D);
    return 0;
  }

  //FC.NGT.D Fs,Ft
  case 0x3f: {
    mov(esi, imm32(Fsn));
    mov(edx, imm32(Ftn));
    call(&CPU::FC_NGT_D);
    return 0;
  }

  }

  if((instruction >> 21 & 31) == 20)
  switch(instruction & 0x3f) {

  //FCVT.S.W Fd,Fs
  case 0x20: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_S_W);
    return 0;
  }

  //FCVT.D.W Fd,Fs
  case 0x21: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_D_W);
    return 0;
  }

  }

  if((instruction >> 21 & 31) == 21)
  switch(instruction & 0x3f) {

  //FCVT.S.L
  case 0x20: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_S_L);
    return 0;
  }

  //FCVT.D.L
  case 0x21: {
    mov(esi, imm32(Fdn));
    mov(edx, imm32(Fsn));
    call(&CPU::FCVT_D_L);
    return 0;
  }

  }

  return 0;
}

#undef Sa
#undef Rdn
#undef Rtn
#undef Rsn
#undef Fdn
#undef Fsn
#undef Ftn
#undef Rd
#undef Rt
#undef Rs
#undef Fd
#undef Fs
#undef Ft
#undef i16
#undef n16
#undef n26

template<typename V, typename... P>
auto CPU::Recompiler::call(V (CPU::*function)(P...)) -> void {
  static_assert(sizeof...(P) <= 5);
  if constexpr(ABI::SystemV) {
    mov(rdi, rbp);
  }
  if constexpr(ABI::Windows) {
    if constexpr(sizeof...(P) >= 5) mov(dis8(rsp, 0x28), r9);
    if constexpr(sizeof...(P) >= 4) mov(dis8(rsp, 0x20), r8);
    if constexpr(sizeof...(P) >= 3) mov(r9, rcx);
    if constexpr(sizeof...(P) >= 2) mov(r8, rdx);
    if constexpr(sizeof...(P) >= 1) mov(rdx, rsi);
    mov(rcx, rbp);
  }
  call(imm64{function}, rax);
}
