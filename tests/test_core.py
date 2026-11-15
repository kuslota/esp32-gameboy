#!/usr/bin/env python3
"""Run the real DMG core/ROM and verify output, timing math and dirty conversion."""
import ctypes
import hashlib
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
subprocess.run(['python3',str(root/'tests/tools/build_demo_rom.py'),'--check'],check=True)
with tempfile.TemporaryDirectory(prefix='gb-core-') as tmp:
 p=Path(tmp);lib=p/'core.so'
 subprocess.run(['cc','-O3','-shared','-fPIC','-Wno-unknown-attributes','-Wno-unused-label',
                 '-I'+str(root/'src/gb_core'),
                 str(root/'src/gb_core/gb_emulator.c'),str(root/'src/gb_core/gb_video.c'),
                 str(root/'src/gb_core/vendor/minigb_apu/minigb_apu.c'),'-DMINIGB_APU_AUDIO_FORMAT_S16SYS',
                 '-o',str(lib)],check=True)
 core=ctypes.CDLL(str(lib))
 core.gb_emulator_init.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t];core.gb_emulator_init.restype=ctypes.c_bool
 core.gb_emulator_frame.argtypes=[ctypes.c_bool];core.gb_emulator_frame.restype=ctypes.c_bool
 core.gb_emulator_pixels.restype=ctypes.POINTER(ctypes.c_uint8)
 core.gb_emulator_demo_counter.restype=ctypes.c_uint8
 class Diagnostics(ctypes.Structure):
  _fields_=[('error',ctypes.c_int),('error_argument',ctypes.c_uint16),('error_pc',ctypes.c_uint16),
            ('ignored_io_reads',ctypes.c_uint32),('ignored_io_writes',ctypes.c_uint32),
            ('first_ignored_io_address',ctypes.c_uint16),('first_ignored_io_pc',ctypes.c_uint16)]
 core.gb_emulator_diagnostics.restype=Diagnostics
 raw=(root/'tests/fixtures/eink_demo.gb').read_bytes();rom=(ctypes.c_uint8*len(raw)).from_buffer_copy(raw)
 assert core.gb_emulator_init(rom,len(raw))
 frames=[];counters=[]
 for n in range(720):
  assert core.gb_emulator_frame(False)
  if n%60==59:
   frames.append(bytes(core.gb_emulator_pixels()[:5760]));counters.append(core.gb_emulator_demo_counter())
 print('ROM VBlank counters every 60 calls:',counters)
 for old,new in zip(counters,counters[1:]):assert (new-old)%256==60, (old,new)
 assert len(set(frames))>8
 shades={((b>>shift)&3) for b in frames[-1] for shift in (0,2,4,6)};assert shades==set(range(4))
 expected=frames[-1];counter=counters[-1]
 assert core.gb_emulator_init(rom,len(raw))
 for n in range(720):assert core.gb_emulator_frame(n%3!=2)
 assert bytes(core.gb_emulator_pixels()[:5760])==expected and core.gb_emulator_demo_counter()==counter
 # Frame output suppression does not change emulated CPU/PPU progress.
 for size in (0,100,32767):assert not core.gb_emulator_init(rom,size)
 for at,value in [(0x147,1),(0x148,9),(0x149,255),(0x143,0x80),(0x14d,raw[0x14d]^1)]:
  bad=bytearray(raw);bad[at]=value;data=(ctypes.c_uint8*len(bad)).from_buffer_copy(bad)
  assert not core.gb_emulator_init(data,len(bad))
 # Invalid opcode inside valid cartridge stops instead of trapping the frontend.
 bad=bytearray(raw);bad[0x150]=0xd3;data=(ctypes.c_uint8*len(bad)).from_buffer_copy(bad)
 assert core.gb_emulator_init(data,len(bad)) and not core.gb_emulator_frame(False)
 assert core.gb_emulator_error()==2 and core.gb_emulator_diagnostics().error_argument==0xd3
 assert core.gb_emulator_diagnostics().error_pc==0x151
 assert not core.gb_emulator_frame(False)
 # Unused DMG I/O must read FF whatever was written, probed with real LDH writes/reads.
 # A mismatch executes D3, so it cannot pass silently.
 holes=[0x7f,0x03,*range(0x08,0x0f),0x4e,*range(0x60,0x68),*range(0x6d,0x70),0x71,*range(0x78,0x7f)]
 probe=bytearray([0xf3]) # DI
 for port in holes:
  for value in (0x00,0x55,0xaa):
   probe.extend([0x3e,value,0xe0,port,0xf0,port,0xfe,0xff,0x28,0x01,0xd3])
 # Ordinary IO and HRAM still retain writable values.
 for port,value in [(0x06,0x5a),(0x42,0x17),(0x43,0x39),(0x80,0xa5),(0xfe,0x81)]:
  probe.extend([0x3e,value,0xe0,port,0xf0,port,0xfe,value,0x28,0x01,0xd3])
 probe.extend([0xc3,0x50,0x01]) # JP demo entry
 unused=bytearray(raw);unused[0x100:0x104]=bytes([0,0xc3,0,0x60])
 unused[0x6000:0x6000+len(probe)]=probe
 data=(ctypes.c_uint8*len(unused)).from_buffer_copy(unused)
 assert core.gb_emulator_init(data,len(unused))
 for n in range(120):assert core.gb_emulator_frame(False), (core.gb_emulator_error(),hex(core.gb_emulator_diagnostics().error_argument))
 diagnostic=core.gb_emulator_diagnostics()
 assert diagnostic.error==0 and diagnostic.ignored_io_reads==diagnostic.ignored_io_writes==3*len(holes)
 assert diagnostic.first_ignored_io_address==0xff7f and diagnostic.first_ignored_io_pc==0x6005
 assert core.gb_emulator_demo_counter()>100
 # Reload resets the diagnostic; the unmodified demo has no unused-I/O notices.
 assert core.gb_emulator_init(rom,len(raw)) and core.gb_emulator_frame(False)
 # Save states include running CPU/RAM, even for a cartridge without a battery.
 core.gb_emulator_state_size.restype=ctypes.c_size_t
 core.gb_emulator_state_copy.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
 core.gb_emulator_state_copy.restype=ctypes.c_bool
 core.gb_emulator_state_load.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
 core.gb_emulator_state_load.restype=ctypes.c_bool
 state_size=core.gb_emulator_state_size();state=(ctypes.c_uint8*state_size)()
 assert state_size>0 and core.gb_emulator_state_copy(state,state_size)
 saved_counter=core.gb_emulator_demo_counter()
 saved_pixels=bytes(core.gb_emulator_pixels()[:5760])
 for n in range(11):assert core.gb_emulator_frame(True)
 assert core.gb_emulator_demo_counter()!=saved_counter
 assert not core.gb_emulator_state_load(state,state_size-1)
 broken=(ctypes.c_uint8*state_size).from_buffer_copy(state);broken[0]^=0xff
 before=core.gb_emulator_demo_counter()
 assert not core.gb_emulator_state_load(broken,state_size)
 assert core.gb_emulator_demo_counter()==before
 assert core.gb_emulator_state_load(state,state_size)
 assert core.gb_emulator_demo_counter()==saved_counter
 assert bytes(core.gb_emulator_pixels()[:5760])==saved_pixels
 # A ROM program writes real sound registers and generates PCM regardless of
 # whether the frontend drains the previous audio block.
 sound=bytearray(raw)
 program=bytes([0x3e,0x80,0xe0,0x26,0x3e,0x77,0xe0,0x24,0x3e,0xff,0xe0,0x25,
                0x3e,0x80,0xe0,0x11,0x3e,0xf3,0xe0,0x12,0x3e,0x00,0xe0,0x13,
                0x3e,0xc3,0xe0,0x14,0x18,0xfe])
 sound[0x150:0x150+len(program)]=program
 sound_rom=(ctypes.c_uint8*len(sound)).from_buffer_copy(sound)
 assert core.gb_emulator_init(sound_rom,len(sound))
 core.gb_emulator_audio_samples.argtypes=[ctypes.POINTER(ctypes.c_int16),ctypes.c_size_t]
 core.gb_emulator_audio_samples.restype=ctypes.c_size_t
 pcm=(ctypes.c_int16*524)()
 lengths=[]
 for n in range(100):
  assert core.gb_emulator_frame(n%2==0)
  count=core.gb_emulator_audio_samples(pcm,524)
  lengths.append(count)
  if 2<=n<10:assert any(pcm[i] for i in range(count))
 assert set(lengths)=={523,524}
 assert not core.gb_emulator_audio_samples(pcm,524)
 # Battery RAM is part of the full state, distinct from its SRAM-only save.
 battery=bytearray(raw);battery[0x147]=3;battery[0x149]=2
 battery[0x14d]=(-sum(battery[0x134:0x14d])-25)&0xff
 battery_program=bytes([0x3e,0x0a,0xea,0x00,0x00,0x3e,0x5a,0xea,0x00,0xa0,
                        0x18,0xfe])
 battery[0x150:0x150+len(battery_program)]=battery_program
 battery_rom=(ctypes.c_uint8*len(battery)).from_buffer_copy(battery)
 assert core.gb_emulator_init(battery_rom,len(battery))
 assert core.gb_emulator_frame(True)
 core.gb_emulator_save_size.restype=ctypes.c_size_t
 core.gb_emulator_save_copy.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
 core.gb_emulator_save_copy.restype=ctypes.c_bool
 core.gb_emulator_save_load.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
 core.gb_emulator_save_load.restype=ctypes.c_bool
 ram_size=core.gb_emulator_save_size();ram=(ctypes.c_uint8*ram_size)()
 assert ram_size==8192 and core.gb_emulator_save_copy(ram,ram_size) and ram[0]==0x5a
 battery_state_size=core.gb_emulator_state_size()
 battery_state=(ctypes.c_uint8*battery_state_size)()
 assert core.gb_emulator_state_copy(battery_state,battery_state_size)
 replacement=(ctypes.c_uint8*ram_size)();replacement[0]=0x33
 assert core.gb_emulator_save_load(replacement,ram_size)
 assert core.gb_emulator_state_load(battery_state,battery_state_size)
 assert core.gb_emulator_save_copy(ram,ram_size) and ram[0]==0x5a
 core.gb_emulator_save_dirty.restype=ctypes.c_bool
 assert core.gb_emulator_save_dirty()
 assert core.gb_emulator_init(rom,len(raw)) and core.gb_emulator_frame(False)
 diagnostic=core.gb_emulator_diagnostics()
 assert not any((diagnostic.error,diagnostic.error_argument,diagnostic.error_pc,
                 diagnostic.ignored_io_reads,diagnostic.ignored_io_writes,diagnostic.first_ignored_io_address))
 class Video(ctypes.Structure):_fields_=[('previous',ctypes.c_uint8*5760),('valid',ctypes.c_bool)]
 core.gb_video_convert.argtypes=[ctypes.POINTER(Video),ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_uint8)]
 core.gb_video_convert.restype=ctypes.c_uint
 for shade,black_per_tile in [(0,0),(1,5),(2,11),(3,16)]:
  state=Video();packed=(ctypes.c_uint8*5760)(*([shade*0x55]*5760))
  guarded=(ctypes.c_uint8*5002)(*([0xa5]*5002));panel=ctypes.cast(ctypes.byref(guarded,1),ctypes.POINTER(ctypes.c_uint8))
  assert core.gb_video_convert(ctypes.byref(state),packed,panel)==144
  assert guarded[0]==guarded[-1]==0xa5
  black=0
  for y in range(200):
   for x in range(200):
    mask=0x80>>(x%8);pixel=bool(panel[y*25+x//8]&mask)
    if 20<=x<180 and 28<=y<172:black+=not pixel
    else:assert pixel==bool(0xa5&mask)
  assert black==1440*black_per_tile
  assert core.gb_video_convert(ctypes.byref(state),packed,panel)==0
  packed[70*40]^=0xff
  assert core.gb_video_convert(ctypes.byref(state),packed,panel)==1
 print('Final native-frame SHA256:',hashlib.sha256(expected).hexdigest())
 # Compile exact clock helper independently of the RTOS.
 (p/'clock.c').write_text('#include "gb_video.h"\n#include <assert.h>\nint main(void){assert(gb_frame_time_us(4194304)==70224000000ULL);assert(gb_frame_time_us(720)==12054748);for(unsigned n=1;n<10000;++n){unsigned long long d=gb_frame_time_us(n)-gb_frame_time_us(n-1);assert(d==16742 || d==16743);}return 0;}\n')
 subprocess.run(['cc','-I'+str(root/'src/gb_core'),str(p/'clock.c'),'-o',str(p/'clock')],check=True)
 subprocess.run([str(p/'clock')],check=True)
print('PASS: ROM execution, APU PCM/cadence, state roundtrip with LCD and battery RAM, invalid state guards, unused-I/O compatibility, diagnostics, fatal opcode guard, render skipping, video and rational clock')
