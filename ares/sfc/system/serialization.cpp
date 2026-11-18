static const string SerializerVersion = "v147";

auto System::serialize(bool synchronize) -> serializer {
  // A streamed bus capture needs socket acknowledgements to finish a suspended
  // transfer. The guarded save cannot dispatch those commands safely; reject the
  // save without changing the recording. Native writers drain in captureBusReady().
  if(synchronize && nall::GDB::server.capture.busActive() && !nall::GDB::server.captureFile.owns()) {
    return {};
  }
  if(synchronize) scheduler.enter(Scheduler::Mode::Synchronize);
  serializer s;

  u32  signature = SerializerSignature;
  char version[16] = {};
  char description[512] = {};
  memory::copy(&version, (const char*)SerializerVersion, SerializerVersion.size());

  // The same loader can carry different download media. Reject incompatible
  // states before power() changes the running console or any flash bytes.
  string media = cartridge.pak ? cartridge.pak->attribute("btDownloadSha256") : string{};
  if(media) {
    memory::copy(description, "BTv1:", 5);
    memory::copy(description + 5, media.data(), 64);
    // Zero preserves compatibility with ordinary BTv1 download states.
    description[69] = cartridge.pak->attribute("btScenario").natural();
  }

  s(signature);
  s(synchronize);
  s(version);
  s(description);
  s(ppu.accurate);

  serialize(s, synchronize);
  return s;
}

auto System::unserialize(serializer& s) -> bool {
  u32  signature = 0;
  bool synchronize = true;
  char version[16] = {};
  char description[512] = {};
  bool ppuAccurate = false;

  s(signature);
  s(synchronize);
  s(version);
  s(description);
  s(ppuAccurate);

  if(signature != SerializerSignature) return false;
  if(string{version} != SerializerVersion) return false;
  if(ppuAccurate != ppu.accurate) return false;
  string media = cartridge.pak ? cartridge.pak->attribute("btDownloadSha256") : string{};
  bool btState = memory::compare(description, "BTv1:", 5) == 0;
  if(btState != (bool)media) return false;
  if(btState) {
    if(memory::compare(description + 5, media.data(), 64) != 0) return false;
    if((u8)description[69] != cartridge.pak->attribute("btScenario").natural()) return false;
  }

  nall::GDB::server.capture.fail("state-load");
  if(synchronize) power(/* reset = */ false);
  serialize(s, synchronize);
  // Keep the acknowledged stop at the restored boundary.
  // Otherwise stepping from a loaded breakpoint compares against the PC from before the load.
  if(synchronize && nall::GDB::server.isHalted()) {
    nall::GDB::server.reportPC(cpu.r.pc.d, !cpu.r.wai && !cpu.r.stp);
  }
  return true;
}

auto System::serialize(serializer& s, bool synchronize) -> void {
  // Restoring a state starts a new emulated timeline, including unsynchronized rewind states.
  if(s.reading() && nativeVideo.discontinuity) nativeVideo.discontinuity();
  scheduler.setSynchronize(synchronize);

  s(random);
  s(cartridge);
  s(cpu);
  s(smp);
  s(ppu);
  s(dsp);

  if(cartridge.has.ICD) s(icd);
  if(cartridge.has.MCC) s(mcc);
  if(cartridge.has.DIP) s(dip);
  if(cartridge.has.Competition) s(competition);
  if(cartridge.has.SA1) s(sa1);
  if(cartridge.has.SuperFX) s(superfx);
  if(cartridge.has.ARMDSP) s(armdsp);
  if(cartridge.has.HitachiDSP) s(hitachidsp);
  if(cartridge.has.NECDSP) s(necdsp);
  if(cartridge.has.EpsonRTC) s(epsonrtc);
  if(cartridge.has.SharpRTC) s(sharprtc);
  if(cartridge.has.SPC7110) s(spc7110);
  if(cartridge.has.SDD1) s(sdd1);
  if(cartridge.has.OBC1) s(obc1);
  if(cartridge.has.MSU1) s(msu1);

  if(cartridge.has.BSMemorySlot) s(bsmemory);
  if(cartridge.has.SufamiTurboSlotA) s(sufamiturboA);
  if(cartridge.has.SufamiTurboSlotB) s(sufamiturboB);

  s(controllerPort1);
  s(controllerPort2);
  s(expansionPort);
}
