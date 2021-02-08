//Super Pierrot
//(not working)

struct SuperPierrot : Interface {
  using Interface::Interface;
  Memory::Readable<n8> rom;

  auto load(Markup::Node document) -> void override {
    auto board = document["game/board"];
    Interface::load(rom, board["memory(type=ROM,content=Program)"]);
  }

  auto save(Markup::Node document) -> void override {
  }

  auto unload() -> void override {
  }

  auto read(n16 address, n8 data) -> n8 override {
    if(address >= 0x4000 && address <= 0x7fff) data = rom.read(bank[0] << 14 | (n14)address);
    if(address >= 0x8000 && address <= 0xbfff) data = rom.read(bank[1] << 14 | (n14)address);
    return data;
  }

  auto write(n16 address, n8 data) -> void override {
    if(address >= 0x4000 && address <= 0x4fff) bank[0] = data;
    if(address >= 0x6000 && address <= 0x6fff) bank[0] = data;
    if(address >= 0x8000 && address <= 0x8fff) bank[0] = data;
    if(address >= 0xa000 && address <= 0xafff) bank[0] = data;

    if(address >= 0x5000 && address <= 0x5fff) bank[1] = data;
    if(address >= 0x7000 && address <= 0x7fff) bank[1] = data;
    if(address >= 0x9000 && address <= 0x9fff) bank[1] = data;
    if(address >= 0xa000 && address <= 0xafff) bank[1] = data;
  }

  auto power() -> void override {
    bank[0] = 0;
    bank[1] = 0;
  }

  auto serialize(serializer& s) -> void override {
    s(bank);
  }

  n8 bank[2];
};
