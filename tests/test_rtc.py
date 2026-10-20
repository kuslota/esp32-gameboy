#!/usr/bin/env python3
"""MBC3 clock cartridges: register access, wall-clock and emulated ticking, save footer and checkpoints."""
import ctypes
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
C=ctypes
FOOTER=48
with tempfile.TemporaryDirectory(prefix='gb-rtc-') as tmp:
 lib=Path(tmp)/'core.so'
 subprocess.run(['cc','-O1','-shared','-fPIC','-Wno-unknown-attributes','-Wno-unused-label',
                 '-I'+str(root/'src/gb_core'),str(root/'src/gb_core/gb_emulator.c'),
                 str(root/'src/gb_core/vendor/minigb_apu/minigb_apu.c'),'-DMINIGB_APU_AUDIO_FORMAT_S16SYS',
                 '-o',str(lib)],check=True)
 core=C.CDLL(str(lib))
 u8p=C.POINTER(C.c_uint8)
 for name,args,res in [('gb_emulator_init',[u8p,C.c_size_t],C.c_bool),('gb_emulator_frame',[C.c_bool],C.c_bool),
                       ('gb_emulator_validate_rom',[u8p,C.c_size_t],C.c_bool),
                       ('gb_emulator_rom_save_size',[u8p,C.c_size_t],C.c_size_t),
                       ('gb_emulator_rom_save_accepted',[u8p,C.c_size_t,C.c_size_t],C.c_bool),
                       ('gb_emulator_save_size',[],C.c_size_t),('gb_emulator_save_dirty',[],C.c_bool),('gb_emulator_save_clean',[],None),
                       ('gb_emulator_save_copy',[u8p,C.c_size_t],C.c_bool),('gb_emulator_save_load',[u8p,C.c_size_t],C.c_bool),
                       ('gb_emulator_state_size',[],C.c_size_t),('gb_emulator_state_copy',[u8p,C.c_size_t],C.c_bool),
                       ('gb_emulator_state_load',[u8p,C.c_size_t],C.c_bool)]:
  f=getattr(core,name);f.argtypes=args;f.restype=res
 CLOCK=C.CFUNCTYPE(C.c_bool,C.POINTER(C.c_int64))
 now=[None]
 def clock(out):
  if now[0] is None:return False
  out[0]=now[0];return True
 clock_cb=CLOCK(clock);core.gb_emulator_set_clock.argtypes=[CLOCK];core.gb_emulator_set_clock(clock_cb)

 def cart(mapper,ram_code,program=None):
  raw=bytearray((root/'tests/fixtures/eink_demo.gb').read_bytes())
  raw[0x147]=mapper;raw[0x149]=ram_code
  if program:raw[0x150:0x150+len(program)]=program
  raw[0x14d]=(-sum(raw[0x134:0x14d])-25)&0xff
  return (C.c_uint8*len(raw)).from_buffer_copy(raw),len(raw)
 def buf(data):return (C.c_uint8*len(data)).from_buffer_copy(bytes(data))
 def save():
  n=core.gb_emulator_save_size();out=(C.c_uint8*n)();assert core.gb_emulator_save_copy(out,n);return bytes(out)
 def regs(data):
  f=data[-FOOTER:];w=lambda i:int.from_bytes(f[4*i:4*i+4],'little')
  return [w(i) for i in range(5)],[w(5+i) for i in range(5)],int.from_bytes(f[40:48],'little')
 def footer(current,latched,wall,width=8):
  out=b''.join(v.to_bytes(4,'little') for v in current+latched)
  return out+wall.to_bytes(width,'little')

 # Mapper 0x10 (Pokemon Gold/Silver) and clock-only 0x0F are accepted; sizes include the footer.
 rom,size=cart(0x10,3)
 assert core.gb_emulator_validate_rom(rom,size) and core.gb_emulator_rom_save_size(rom,size)==32768+FOOTER
 for n,ok in [(32768,True),(32768+44,True),(32768+48,True),(32767,False),(32768+47,False),(0,False)]:
  assert core.gb_emulator_rom_save_accepted(rom,size,n)==ok,n
 clock_only,clock_size=cart(0x0f,0)
 assert core.gb_emulator_validate_rom(clock_only,clock_size) and core.gb_emulator_rom_save_size(clock_only,clock_size)==FOOTER
 for mapper,ram in [(0x0f,2),(0x10,0)]:
  bad,bad_size=cart(mapper,ram);assert not core.gb_emulator_validate_rom(bad,bad_size)
 plain,plain_size=cart(0x13,3);assert core.gb_emulator_rom_save_size(plain,plain_size)==32768

 # The game writes an out-of-range seconds value (masked to 0x3F), latches, reads it back, and reads
 # unmapped bank 0x0D (open bus FF). Both results land in SRAM bank 0.
 program=bytes([0xf3,0x3e,0x0a,0xea,0x00,0x00, 0x3e,0x08,0xea,0x00,0x40, 0x3e,0xff,0xea,0x00,0xa0,
                0x3e,0x00,0xea,0x00,0x60, 0x3e,0x01,0xea,0x00,0x60, 0xfa,0x00,0xa0,0x47,
                0x3e,0x0d,0xea,0x00,0x40, 0xfa,0x00,0xa0,0x4f, 0x3e,0x00,0xea,0x00,0x40,
                0x78,0xea,0x00,0xa0, 0x79,0xea,0x01,0xa0, 0x18,0xfe])
 rom,size=cart(0x10,3,program)
 now[0]=1000
 assert core.gb_emulator_init(rom,size) and core.gb_emulator_save_size()==32768+FOOTER
 for _ in range(3):assert core.gb_emulator_frame(True)
 data=save();current,latched,wall=regs(data)
 assert data[0]==0x3f and data[1]==0xff,(data[0],data[1])
 assert current[0]==0x3f and latched[0]==0x3f and wall==1000 and core.gb_emulator_save_dirty()

 # Wall clock: registers follow elapsed seconds across frames, pauses and the halt bit.
 rom,size=cart(0x10,3)
 now[0]=5000;assert core.gb_emulator_init(rom,size) and core.gb_emulator_frame(True)
 now[0]=5000+3725;assert core.gb_emulator_frame(True)
 current,_,wall=regs(save());assert current==[5,2,1,0,0] and wall==8725,current
 now[0]=8725-50;assert core.gb_emulator_frame(True)       # clock went backwards: re-anchor, no change
 now[0]=8725-50+10;assert core.gb_emulator_frame(True)
 assert regs(save())[0]==[15,2,1,0,0]
 # Day 511 23:59:59 rolls to day 0 with carry; carry stays set on later catch-ups.
 data=bytearray(save());data[-FOOTER:]=footer([59,59,23,255,1],[0]*5,9000)
 assert core.gb_emulator_save_load(buf(data),len(data))
 now[0]=9001;assert core.gb_emulator_frame(True)
 assert regs(save())[0]==[0,0,0,0,0x80]
 now[0]=9061;assert core.gb_emulator_frame(True)
 assert regs(save())[0]==[0,1,0,0,0x80]
 # Halt bit: time does not advance, and resuming does not jump.
 data=bytearray(save());data[-FOOTER:]=footer([1,2,3,4,0x40],[0]*5,9100)
 assert core.gb_emulator_save_load(buf(data),len(data))
 now[0]=9500;assert core.gb_emulator_frame(True)
 assert regs(save())[0]==[1,2,3,4,0x40]

 # Power-off catch-up from a saved footer, including the 44-byte (32-bit time) and RAM-only forms.
 ram=bytes(range(256))*128
 assert core.gb_emulator_init(rom,size)
 assert core.gb_emulator_save_load(buf(ram+footer([10,0,0,0,0],[0]*5,20000)),32768+48)
 now[0]=20000+86400+60;assert core.gb_emulator_frame(True)
 data=save();assert data[:32768]==ram and regs(data)[0]==[10,1,0,1,0]
 assert core.gb_emulator_init(rom,size)
 assert core.gb_emulator_save_load(buf(ram+footer([0,0,0,0,0],[0]*5,30000,4)),32768+44)
 now[0]=30030;assert core.gb_emulator_frame(True);assert regs(save())[0]==[30,0,0,0,0]
 assert core.gb_emulator_init(rom,size) and core.gb_emulator_save_load(buf(ram),32768)
 now[0]=40000;assert core.gb_emulator_frame(True)
 data=save();assert data[:32768]==ram and regs(data)[0]==[0,0,0,0,0] and regs(data)[2]==40000
 assert not core.gb_emulator_save_load(buf(ram[:-1]),32767)
 # Catch-up happens before the first frame runs: a game that latches and reads seconds at boot
 # sees the elapsed time, not the stale saved value.
 program=bytes([0xf3,0x3e,0x0a,0xea,0x00,0x00, 0x3e,0x08,0xea,0x00,0x40, 0x3e,0x00,0xea,0x00,0x60,
                0x3e,0x01,0xea,0x00,0x60, 0xfa,0x00,0xa0,0x47, 0x3e,0x00,0xea,0x00,0x40,
                0x78,0xea,0x00,0xa0, 0x18,0xfe])
 boot,boot_size=cart(0x10,3,program)
 assert core.gb_emulator_init(boot,boot_size)
 assert core.gb_emulator_save_load(buf(bytes(32768)+footer([10,0,0,0,0],[0]*5,45000)),32768+48)
 now[0]=45020;assert core.gb_emulator_frame(True)
 data=save();assert data[0]==30 and regs(data)[1][0]==30,(data[0],regs(data))

 # Without a clock the cartridge clock advances with emulated time (59.7 frames per second).
 now[0]=None
 # Each emulated second marks the save dirty, since the footer is the only record of it.
 assert core.gb_emulator_init(rom,size)
 for _ in range(59):assert core.gb_emulator_frame(True)
 assert regs(save())[0][0]==0 and regs(save())[2]==0 and not core.gb_emulator_save_dirty()
 assert core.gb_emulator_frame(True);assert regs(save())[0][0]==1 and core.gb_emulator_save_dirty()
 core.gb_emulator_save_clean()
 for _ in range(59):assert core.gb_emulator_frame(True)
 assert not core.gb_emulator_save_dirty()
 assert core.gb_emulator_frame(True);assert regs(save())[0][0]==2 and core.gb_emulator_save_dirty()

 # Checkpoints carry the clock anchor: resuming later catches up from the checkpoint time.
 now[0]=50000
 assert core.gb_emulator_init(plain,plain_size);plain_state=core.gb_emulator_state_size()
 assert core.gb_emulator_init(rom,size) and core.gb_emulator_frame(True)
 n=core.gb_emulator_state_size();assert n==plain_state+16
 state=(C.c_uint8*n)();assert core.gb_emulator_state_copy(state,n)
 now[0]=50000+125;assert core.gb_emulator_frame(True);assert regs(save())[0][:2]==[5,2]
 assert core.gb_emulator_init(rom,size)
 now[0]=50000+600;assert core.gb_emulator_state_load(state,n) and core.gb_emulator_frame(True)
 assert regs(save())[0][:2]==[0,10]
 assert not core.gb_emulator_state_load(state,n-16)

 # Clock-only cartridge: no RAM, so RAM reads are open bus and the save is just the footer.
 rom,size=cart(0x0f,0,bytes([0xf3,0x3e,0x0a,0xea,0x00,0x00,0xfa,0x00,0xa0,0xea,0x00,0xc0,0x18,0xfe]))
 now[0]=60000;assert core.gb_emulator_init(rom,size) and core.gb_emulator_save_size()==FOOTER
 for _ in range(2):assert core.gb_emulator_frame(True)
 assert len(save())==FOOTER and regs(save())[2]==60000
print('PASS: MBC3 clock mappers, masked/latched register access, open bus, wall-clock and emulated ticking, boot-time catch-up, fallback persistence, carry/halt, 48/44/0-byte footers, checkpoint catch-up, clock-only carts')
