//go:build !noasm && arm64

#include "textflag.h"

// Shared tail mask table for all kernels

DATA tail_mask_table+0x00(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x08(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x10(SB)/1, $0xff
DATA tail_mask_table+0x11(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x19(SB)/4, $0x00000000
DATA tail_mask_table+0x1d(SB)/2, $0x0000
DATA tail_mask_table+0x1f(SB)/1, $0x00
DATA tail_mask_table+0x20(SB)/1, $0xff
DATA tail_mask_table+0x21(SB)/1, $0xff
DATA tail_mask_table+0x22(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x2a(SB)/4, $0x00000000
DATA tail_mask_table+0x2e(SB)/2, $0x0000
DATA tail_mask_table+0x30(SB)/1, $0xff
DATA tail_mask_table+0x31(SB)/1, $0xff
DATA tail_mask_table+0x32(SB)/1, $0xff
DATA tail_mask_table+0x33(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x3b(SB)/4, $0x00000000
DATA tail_mask_table+0x3f(SB)/1, $0x00
DATA tail_mask_table+0x40(SB)/1, $0xff
DATA tail_mask_table+0x41(SB)/1, $0xff
DATA tail_mask_table+0x42(SB)/1, $0xff
DATA tail_mask_table+0x43(SB)/1, $0xff
DATA tail_mask_table+0x44(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x4c(SB)/4, $0x00000000
DATA tail_mask_table+0x50(SB)/1, $0xff
DATA tail_mask_table+0x51(SB)/1, $0xff
DATA tail_mask_table+0x52(SB)/1, $0xff
DATA tail_mask_table+0x53(SB)/1, $0xff
DATA tail_mask_table+0x54(SB)/1, $0xff
DATA tail_mask_table+0x55(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x5d(SB)/2, $0x0000
DATA tail_mask_table+0x5f(SB)/1, $0x00
DATA tail_mask_table+0x60(SB)/1, $0xff
DATA tail_mask_table+0x61(SB)/1, $0xff
DATA tail_mask_table+0x62(SB)/1, $0xff
DATA tail_mask_table+0x63(SB)/1, $0xff
DATA tail_mask_table+0x64(SB)/1, $0xff
DATA tail_mask_table+0x65(SB)/1, $0xff
DATA tail_mask_table+0x66(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x6e(SB)/2, $0x0000
DATA tail_mask_table+0x70(SB)/1, $0xff
DATA tail_mask_table+0x71(SB)/1, $0xff
DATA tail_mask_table+0x72(SB)/1, $0xff
DATA tail_mask_table+0x73(SB)/1, $0xff
DATA tail_mask_table+0x74(SB)/1, $0xff
DATA tail_mask_table+0x75(SB)/1, $0xff
DATA tail_mask_table+0x76(SB)/1, $0xff
DATA tail_mask_table+0x77(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x7f(SB)/1, $0x00
DATA tail_mask_table+0x80(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0x88(SB)/8, $0x0000000000000000
DATA tail_mask_table+0x90(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0x98(SB)/8, $0x00000000000000ff
DATA tail_mask_table+0xa0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xa8(SB)/8, $0x000000000000ffff
DATA tail_mask_table+0xb0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xb8(SB)/8, $0x0000000000ffffff
DATA tail_mask_table+0xc0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xc8(SB)/8, $0x00000000ffffffff
DATA tail_mask_table+0xd0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xd8(SB)/8, $0x000000ffffffffff
DATA tail_mask_table+0xe0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xe8(SB)/8, $0x0000ffffffffffff
DATA tail_mask_table+0xf0(SB)/8, $0xffffffffffffffff
DATA tail_mask_table+0xf8(SB)/8, $0x00ffffffffffffff
GLOBL tail_mask_table(SB), (RODATA|NOPTR), $256
