auto SM83::operand() -> n8 {
  return read(PC++);
}

auto SM83::operands() -> n16 {
  n16    data = read(PC++) << 0;
  return data | read(PC++) << 8;
}

auto SM83::load(n16 address) -> n16 {
  n16    data = read(address++) << 0;
  return data | read(address++) << 8;
}

auto SM83::store(n16 address, n16 data) -> void {
  write(address++, data >> 0);
  write(address++, data >> 8);
}

auto SM83::pop() -> n16 {
  n16    data = read(SP++) << 0;
  return data | read(SP++) << 8;
}

auto SM83::push(n16 data) -> void {
  write(--SP, data >> 8);
  write(--SP, data >> 0);
}
