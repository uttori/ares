auto Cartridge::serialize(serializer& s) -> void {
  s(ram);
  if(btGameCart.active()) s(btGameCart);
}
