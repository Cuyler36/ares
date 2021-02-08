auto VDP::Debugger::load(Node::Object parent) -> void {
  memory.vram = parent->append<Node::Debugger::Memory>("VDP VRAM");
  memory.vram->setSize(32_KiB << 1);
  memory.vram->setRead([&](u32 address) -> u8 {
    return self.vram.memory[n15(address >> 1)].byte(!(address & 1));
  });
  memory.vram->setWrite([&](u32 address, u8 data) -> void {
    self.vram.memory[n15(address >> 1)].byte(!(address & 1)) = data;
  });

  memory.vsram = parent->append<Node::Debugger::Memory>("VDP VSRAM");
  memory.vsram->setSize(40 << 1);
  memory.vsram->setRead([&](u32 address) -> u8 {
    if(address >= 40 << 1) return 0x00;
    return self.vsram.memory[address >> 1].byte(!(address & 1));
  });
  memory.vsram->setWrite([&](u32 address, u8 data) -> void {
    if(address >= 40 << 1) return;
    self.vsram.memory[address >> 1].byte(!(address & 1)) = data;
  });

  memory.cram = parent->append<Node::Debugger::Memory>("VDP CRAM");
  memory.cram->setSize(64 << 1);
  memory.cram->setRead([&](u32 address) -> u8 {
    return self.cram.memory[n6(address >> 1)].byte(!(address & 1));
  });
  memory.cram->setWrite([&](u32 address, u8 data) -> void {
    self.cram.memory[n6(address >> 1)].byte(!(address & 1)) = data;
  });
}

auto VDP::Debugger::unload() -> void {
  memory.vram.reset();
  memory.vsram.reset();
  memory.cram.reset();
}
