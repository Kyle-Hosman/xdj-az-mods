/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated per firmware build. Do not edit by hand. 413 EP147 addresses for azmod.
 *
 * Code names an EP147 address by its v1.30 value (EP_1b93a28). ep147_addr_init(build)
 * fills ep147_addr[] for the running build, which common/ep147build.h
 * identified by md5; until then, and on an unknown build, every entry is 0.
 * v2.00 values are hand-resolved where the automatic re-anchor was ambiguous. */
#ifndef EP147ADDR_H
#define EP147ADDR_H
#include "../common/ep147build.h"

enum {
    EPI_424d40,
    EPI_5acb58,
    EPI_6742d8,
    EPI_6ac3c0,
    EPI_6b7958,
    EPI_708a10,
    EPI_7a01e0,
    EPI_9d1ae8,
    EPI_9d4ac0,
    EPI_a77770,
    EPI_a77a20,
    EPI_aa9a48,
    EPI_aabb78,
    EPI_c85838,
    EPI_d34790,
    EPI_d85a08,
    EPI_e33830,
    EPI_11755a0,
    EPI_1176c88,
    EPI_11881d8,
    EPI_1224270,
    EPI_12246a0,
    EPI_12439e8,
    EPI_1243a58,
    EPI_1243b38,
    EPI_1243ba8,
    EPI_1243c18,
    EPI_1243c88,
    EPI_1243cf8,
    EPI_1243d68,
    EPI_1243dd8,
    EPI_1243e48,
    EPI_1243eb8,
    EPI_1243f28,
    EPI_1243f98,
    EPI_1244008,
    EPI_12441d8,
    EPI_12671d0,
    EPI_126eab0,
    EPI_1272830,
    EPI_1348c48,
    EPI_1349130,
    EPI_1453e80,
    EPI_14553d8,
    EPI_14574f8,
    EPI_1457b4c,
    EPI_14754e8,
    EPI_14a3e10,
    EPI_1891d10,
    EPI_18d8d18,
    EPI_195cc98,
    EPI_1960f70,
    EPI_1987038,
    EPI_19ab418,
    EPI_19ab6d8,
    EPI_19ac3c8,
    EPI_19ac568,
    EPI_19bcf68,
    EPI_19bcf80,
    EPI_19d6760,
    EPI_19d7b60,
    EPI_19dd430,
    EPI_1a29670,
    EPI_1a29830,
    EPI_1a79ea0,
    EPI_1ac7848,
    EPI_1ac7998,
    EPI_1ac7e50,
    EPI_1ac9268,
    EPI_1ac9cb0,
    EPI_1aca09c,
    EPI_1aca178,
    EPI_1acc7b0,
    EPI_1acc820,
    EPI_1acd240,
    EPI_1ace798,
    EPI_1ad0d40,
    EPI_1afde70,
    EPI_1b57640,
    EPI_1b59b98,
    EPI_1b5bab0,
    EPI_1b5bab8,
    EPI_1b5bb90,
    EPI_1b8f950,
    EPI_1b93a28,
    EPI_1b948d0,
    EPI_1b949b8,
    EPI_1b95e80,
    EPI_1b97d98,
    EPI_1ba2e28,
    EPI_1bad940,
    EPI_1bb99f0,
    EPI_1bc41b0,
    EPI_1bc5668,
    EPI_1bcfb78,
    EPI_1bcff88,
    EPI_1bd03f0,
    EPI_1bd0600,
    EPI_1bd06b8,
    EPI_1bdb7e0,
    EPI_1bdcf98,
    EPI_1c192a0,
    EPI_1c1fad0,
    EPI_1c296b8,
    EPI_1c296d8,
    EPI_1c348c8,
    EPI_1c6e420,
    EPI_1c6e788,
    EPI_1c70fd0,
    EPI_1c71030,
    EPI_1c71728,
    EPI_1ca31a8,
    EPI_1ca4650,
    EPI_1d5ef18,
    EPI_1d6bc60,
    EPI_1d6bff0,
    EPI_1d6f280,
    EPI_1d76f18,
    EPI_1d77468,
    EPI_1d82b08,
    EPI_1d9fb68,
    EPI_1da09b0,
    EPI_1ddd8d8,
    EPI_1dde9e8,
    EPI_1de0148,
    EPI_1de01d8,
    EPI_1de1020,
    EPI_1de1550,
    EPI_1de4ee0,
    EPI_1de4fb8,
    EPI_1de50e0,
    EPI_1de5120,
    EPI_1de51f0,
    EPI_1de52c0,
    EPI_1de5d90,
    EPI_1de5e30,
    EPI_1de5ea0,
    EPI_1de7b20,
    EPI_1dea048,
    EPI_1deb250,
    EPI_1df3ae0,
    EPI_1df4908,
    EPI_1df57c8,
    EPI_1df6010,
    EPI_1df6240,
    EPI_1df69f8,
    EPI_1dfb328,
    EPI_1dfc710,
    EPI_1dfed18,
    EPI_1dff558,
    EPI_1dffe10,
    EPI_1e00dd0,
    EPI_1e02d18,
    EPI_1e06450,
    EPI_1e071a0,
    EPI_1e27d28,
    EPI_1e461f8,
    EPI_1e46af0,
    EPI_1e678f0,
    EPI_1eaacd8,
    EPI_1ec495c,
    EPI_1f05520,
    EPI_1f25838,
    EPI_1f3fb18,
    EPI_1f40998,
    EPI_1f44418,
    EPI_1f45660,
    EPI_1f4a3f0,
    EPI_1f4a590,
    EPI_1f4ac90,
    EPI_1f5da20,
    EPI_1fa7a28,
    EPI_1fefa78,
    EPI_1ff1250,
    EPI_1ffbc60,
    EPI_201281c,
    EPI_2085768,
    EPI_2086748,
    EPI_211c0e8,
    EPI_211c840,
    EPI_211c918,
    EPI_212f988,
    EPI_212fb94,
    EPI_218a6d8,
    EPI_218a890,
    EPI_21b6560,
    EPI_21c81c0,
    EPI_21e1090,
    EPI_21e3440,
    EPI_21e4550,
    EPI_21e48b0,
    EPI_21e6de0,
    EPI_21e7540,
    EPI_21e8590,
    EPI_21ea450,
    EPI_21ea670,
    EPI_2224f60,
    EPI_22fb7d0,
    EPI_22fc600,
    EPI_230b460,
    EPI_230d790,
    EPI_230e3e0,
    EPI_230f530,
    EPI_230f5c0,
    EPI_2333ce0,
    EPI_233c340,
    EPI_2398900,
    EPI_239cda0,
    EPI_23a2e60,
    EPI_23a45f0,
    EPI_23a54e0,
    EPI_23aee40,
    EPI_23aeef0,
    EPI_23aef40,
    EPI_23af140,
    EPI_23af590,
    EPI_23af680,
    EPI_23af6c0,
    EPI_23afeb0,
    EPI_23b04e0,
    EPI_23b12b0,
    EPI_23b1410,
    EPI_23b1440,
    EPI_23b4be0,
    EPI_23de290,
    EPI_23eb200,
    EPI_23ed7c0,
    EPI_23ee2a0,
    EPI_23f1370,
    EPI_23fde20,
    EPI_23fe810,
    EPI_2401dd0,
    EPI_2402ff0,
    EPI_242d0a0,
    EPI_2437090,
    EPI_243d1d0,
    EPI_243d200,
    EPI_2459540,
    EPI_2459640,
    EPI_24597e0,
    EPI_2465c70,
    EPI_2471200,
    EPI_2482f30,
    EPI_2485140,
    EPI_2485d40,
    EPI_2488570,
    EPI_248a250,
    EPI_248a8f0,
    EPI_248ab00,
    EPI_248bfc0,
    EPI_2491ed0,
    EPI_24924f0,
    EPI_249a4b0,
    EPI_249d6b0,
    EPI_249daf0,
    EPI_249efe0,
    EPI_249f200,
    EPI_24a0890,
    EPI_24f5bc0,
    EPI_24f6b34,
    EPI_24fa6f0,
    EPI_24fac14,
    EPI_24fac4c,
    EPI_24fb350,
    EPI_25c0aa0,
    EPI_25e0550,
    EPI_25e9ba0,
    EPI_26a3f60,
    EPI_26a4838,
    EPI_26cf590,
    EPI_26e1b68,
    EPI_27d18b8,
    EPI_2847138,
    EPI_284ab30,
    EPI_284b090,
    EPI_284b2b8,
    EPI_2854578,
    EPI_2857330,
    EPI_285f078,
    EPI_28729f8,
    EPI_2874158,
    EPI_2896b90,
    EPI_29672e0,
    EPI_2967378,
    EPI_29674c8,
    EPI_2967678,
    EPI_29676a8,
    EPI_29676d0,
    EPI_29676f0,
    EPI_2968548,
    EPI_29c2b00,
    EPI_29c3b38,
    EPI_29c5028,
    EPI_29c98c0,
    EPI_29d4830,
    EPI_29d67a0,
    EPI_29da0e8,
    EPI_29daee8,
    EPI_29db4c8,
    EPI_29dbec8,
    EPI_29e0f18,
    EPI_29e2858,
    EPI_29e9540,
    EPI_29e9790,
    EPI_2a19970,
    EPI_2a92ae0,
    EPI_2aa6448,
    EPI_2abc588,
    EPI_2b37908,
    EPI_2b44be8,
    EPI_2b44c50,
    EPI_2b63770,
    EPI_2b668b8,
    EPI_2b66d98,
    EPI_2b67408,
    EPI_2b67a78,
    EPI_2b6f208,
    EPI_2b6f460,
    EPI_2b72f00,
    EPI_2b72f50,
    EPI_2b72fa0,
    EPI_2b74558,
    EPI_2b747e0,
    EPI_2b79058,
    EPI_2b79158,
    EPI_2b799c0,
    EPI_2b7d8d0,
    EPI_2b7da80,
    EPI_2b7f408,
    EPI_2b7f588,
    EPI_2b7f6d8,
    EPI_2b88870,
    EPI_2b88a48,
    EPI_2b9eb88,
    EPI_2bcc810,
    EPI_2bcdec0,
    EPI_2c29228,
    EPI_2c51010,
    EPI_2c510a0,
    EPI_2c52ff0,
    EPI_2c5ad08,
    EPI_2c5bb40,
    EPI_2c667f8,
    EPI_2cd4b28,
    EPI_2ce30d0,
    EPI_2ce7c50,
    EPI_2cedcf8,
    EPI_2cee580,
    EPI_2cfe0f0,
    EPI_2cfe5c0,
    EPI_2d0b700,
    EPI_2d1b678,
    EPI_2d1dbe8,
    EPI_2d21548,
    EPI_2d37158,
    EPI_2d37408,
    EPI_2d387d0,
    EPI_2d38a80,
    EPI_2d3ca00,
    EPI_2d3cc28,
    EPI_2d3d9b8,
    EPI_2d3dbe0,
    EPI_2d6c498,
    EPI_2d6cac0,
    EPI_2d72b48,
    EPI_2d72cb0,
    EPI_2d74bb0,
    EPI_2d7a800,
    EPI_2d824e0,
    EPI_2d84b60,
    EPI_2d8d300,
    EPI_2d8d4d8,
    EPI_2d8d7a0,
    EPI_2d8da20,
    EPI_2d8f570,
    EPI_2d8f880,
    EPI_2d95250,
    EPI_2d994e8,
    EPI_2da1c28,
    EPI_2dae640,
    EPI_2dc1820,
    EPI_2dc7888,
    EPI_2dcc3a0,
    EPI_2dcc760,
    EPI_2dccc70,
    EPI_2dede68,
    EPI_2defe68,
    EPI_2df55e8,
    EPI_2df7ff8,
    EPI_2dfbde0,
    EPI_2dfbe08,
    EPI_2e15028,
    EPI_2e15058,
    EPI_2e41538,
    EPI_2e509c8,
    EPI_2e5fe90,
    EPI_2e600a0,
    EPI_2e605b8,
    EPI_2e67bb0,
    EPI_3141a38,
    EPI_3142bc0,
    EPI_3142d48,
    EPI_314cae0,
    EPI_31592e0,
    EPI_315e560,
    EPI_3480128,
    EPI_3b25a60,
    EPI_3b32488,
    EPI_3b35728,
    EPI_3b35f40,
    EPI_3b35fb8,
    EPI_3b8dcb8,
    EPI_3b8e9f8,
    EPI_N
};
static const uint32_t ep147_addr_v130[EPI_N] = {
    0x0424d40,
    0x05acb58,
    0x06742d8,
    0x06ac3c0,
    0x06b7958,
    0x0708a10,
    0x07a01e0,
    0x09d1ae8,
    0x09d4ac0,
    0x0a77770,
    0x0a77a20,
    0x0aa9a48,
    0x0aabb78,
    0x0c85838,
    0x0d34790,
    0x0d85a08,
    0x0e33830,
    0x11755a0,
    0x1176c88,
    0x11881d8,
    0x1224270,
    0x12246a0,
    0x12439e8,
    0x1243a58,
    0x1243b38,
    0x1243ba8,
    0x1243c18,
    0x1243c88,
    0x1243cf8,
    0x1243d68,
    0x1243dd8,
    0x1243e48,
    0x1243eb8,
    0x1243f28,
    0x1243f98,
    0x1244008,
    0x12441d8,
    0x12671d0,
    0x126eab0,
    0x1272830,
    0x1348c48,
    0x1349130,
    0x1453e80,
    0x14553d8,
    0x14574f8,
    0x1457b4c,
    0x14754e8,
    0x14a3e10,
    0x1891d10,
    0x18d8d18,
    0x195cc98,
    0x1960f70,
    0x1987038,
    0x19ab418,
    0x19ab6d8,
    0x19ac3c8,
    0x19ac568,
    0x19bcf68,
    0x19bcf80,
    0x19d6760,
    0x19d7b60,
    0x19dd430,
    0x1a29670,
    0x1a29830,
    0x1a79ea0,
    0x1ac7848,
    0x1ac7998,
    0x1ac7e50,
    0x1ac9268,
    0x1ac9cb0,
    0x1aca09c,
    0x1aca178,
    0x1acc7b0,
    0x1acc820,
    0x1acd240,
    0x1ace798,
    0x1ad0d40,
    0x1afde70,
    0x1b57640,
    0x1b59b98,
    0x1b5bab0,
    0x1b5bab8,
    0x1b5bb90,
    0x1b8f950,
    0x1b93a28,
    0x1b948d0,
    0x1b949b8,
    0x1b95e80,
    0x1b97d98,
    0x1ba2e28,
    0x1bad940,
    0x1bb99f0,
    0x1bc41b0,
    0x1bc5668,
    0x1bcfb78,
    0x1bcff88,
    0x1bd03f0,
    0x1bd0600,
    0x1bd06b8,
    0x1bdb7e0,
    0x1bdcf98,
    0x1c192a0,
    0x1c1fad0,
    0x1c296b8,
    0x1c296d8,
    0x1c348c8,
    0x1c6e420,
    0x1c6e788,
    0x1c70fd0,
    0x1c71030,
    0x1c71728,
    0x1ca31a8,
    0x1ca4650,
    0x1d5ef18,
    0x1d6bc60,
    0x1d6bff0,
    0x1d6f280,
    0x1d76f18,
    0x1d77468,
    0x1d82b08,
    0x1d9fb68,
    0x1da09b0,
    0x1ddd8d8,
    0x1dde9e8,
    0x1de0148,
    0x1de01d8,
    0x1de1020,
    0x1de1550,
    0x1de4ee0,
    0x1de4fb8,
    0x1de50e0,
    0x1de5120,
    0x1de51f0,
    0x1de52c0,
    0x1de5d90,
    0x1de5e30,
    0x1de5ea0,
    0x1de7b20,
    0x1dea048,
    0x1deb250,
    0x1df3ae0,
    0x1df4908,
    0x1df57c8,
    0x1df6010,
    0x1df6240,
    0x1df69f8,
    0x1dfb328,
    0x1dfc710,
    0x1dfed18,
    0x1dff558,
    0x1dffe10,
    0x1e00dd0,
    0x1e02d18,
    0x1e06450,
    0x1e071a0,
    0x1e27d28,
    0x1e461f8,
    0x1e46af0,
    0x1e678f0,
    0x1eaacd8,
    0x1ec495c,
    0x1f05520,
    0x1f25838,
    0x1f3fb18,
    0x1f40998,
    0x1f44418,
    0x1f45660,
    0x1f4a3f0,
    0x1f4a590,
    0x1f4ac90,
    0x1f5da20,
    0x1fa7a28,
    0x1fefa78,
    0x1ff1250,
    0x1ffbc60,
    0x201281c,
    0x2085768,
    0x2086748,
    0x211c0e8,
    0x211c840,
    0x211c918,
    0x212f988,
    0x212fb94,
    0x218a6d8,
    0x218a890,
    0x21b6560,
    0x21c81c0,
    0x21e1090,
    0x21e3440,
    0x21e4550,
    0x21e48b0,
    0x21e6de0,
    0x21e7540,
    0x21e8590,
    0x21ea450,
    0x21ea670,
    0x2224f60,
    0x22fb7d0,
    0x22fc600,
    0x230b460,
    0x230d790,
    0x230e3e0,
    0x230f530,
    0x230f5c0,
    0x2333ce0,
    0x233c340,
    0x2398900,
    0x239cda0,
    0x23a2e60,
    0x23a45f0,
    0x23a54e0,
    0x23aee40,
    0x23aeef0,
    0x23aef40,
    0x23af140,
    0x23af590,
    0x23af680,
    0x23af6c0,
    0x23afeb0,
    0x23b04e0,
    0x23b12b0,
    0x23b1410,
    0x23b1440,
    0x23b4be0,
    0x23de290,
    0x23eb200,
    0x23ed7c0,
    0x23ee2a0,
    0x23f1370,
    0x23fde20,
    0x23fe810,
    0x2401dd0,
    0x2402ff0,
    0x242d0a0,
    0x2437090,
    0x243d1d0,
    0x243d200,
    0x2459540,
    0x2459640,
    0x24597e0,
    0x2465c70,
    0x2471200,
    0x2482f30,
    0x2485140,
    0x2485d40,
    0x2488570,
    0x248a250,
    0x248a8f0,
    0x248ab00,
    0x248bfc0,
    0x2491ed0,
    0x24924f0,
    0x249a4b0,
    0x249d6b0,
    0x249daf0,
    0x249efe0,
    0x249f200,
    0x24a0890,
    0x24f5bc0,
    0x24f6b34,
    0x24fa6f0,
    0x24fac14,
    0x24fac4c,
    0x24fb350,
    0x25c0aa0,
    0x25e0550,
    0x25e9ba0,
    0x26a3f60,
    0x26a4838,
    0x26cf590,
    0x26e1b68,
    0x27d18b8,
    0x2847138,
    0x284ab30,
    0x284b090,
    0x284b2b8,
    0x2854578,
    0x2857330,
    0x285f078,
    0x28729f8,
    0x2874158,
    0x2896b90,
    0x29672e0,
    0x2967378,
    0x29674c8,
    0x2967678,
    0x29676a8,
    0x29676d0,
    0x29676f0,
    0x2968548,
    0x29c2b00,
    0x29c3b38,
    0x29c5028,
    0x29c98c0,
    0x29d4830,
    0x29d67a0,
    0x29da0e8,
    0x29daee8,
    0x29db4c8,
    0x29dbec8,
    0x29e0f18,
    0x29e2858,
    0x29e9540,
    0x29e9790,
    0x2a19970,
    0x2a92ae0,
    0x2aa6448,
    0x2abc588,
    0x2b37908,
    0x2b44be8,
    0x2b44c50,
    0x2b63770,
    0x2b668b8,
    0x2b66d98,
    0x2b67408,
    0x2b67a78,
    0x2b6f208,
    0x2b6f460,
    0x2b72f00,
    0x2b72f50,
    0x2b72fa0,
    0x2b74558,
    0x2b747e0,
    0x2b79058,
    0x2b79158,
    0x2b799c0,
    0x2b7d8d0,
    0x2b7da80,
    0x2b7f408,
    0x2b7f588,
    0x2b7f6d8,
    0x2b88870,
    0x2b88a48,
    0x2b9eb88,
    0x2bcc810,
    0x2bcdec0,
    0x2c29228,
    0x2c51010,
    0x2c510a0,
    0x2c52ff0,
    0x2c5ad08,
    0x2c5bb40,
    0x2c667f8,
    0x2cd4b28,
    0x2ce30d0,
    0x2ce7c50,
    0x2cedcf8,
    0x2cee580,
    0x2cfe0f0,
    0x2cfe5c0,
    0x2d0b700,
    0x2d1b678,
    0x2d1dbe8,
    0x2d21548,
    0x2d37158,
    0x2d37408,
    0x2d387d0,
    0x2d38a80,
    0x2d3ca00,
    0x2d3cc28,
    0x2d3d9b8,
    0x2d3dbe0,
    0x2d6c498,
    0x2d6cac0,
    0x2d72b48,
    0x2d72cb0,
    0x2d74bb0,
    0x2d7a800,
    0x2d824e0,
    0x2d84b60,
    0x2d8d300,
    0x2d8d4d8,
    0x2d8d7a0,
    0x2d8da20,
    0x2d8f570,
    0x2d8f880,
    0x2d95250,
    0x2d994e8,
    0x2da1c28,
    0x2dae640,
    0x2dc1820,
    0x2dc7888,
    0x2dcc3a0,
    0x2dcc760,
    0x2dccc70,
    0x2dede68,
    0x2defe68,
    0x2df55e8,
    0x2df7ff8,
    0x2dfbde0,
    0x2dfbe08,
    0x2e15028,
    0x2e15058,
    0x2e41538,
    0x2e509c8,
    0x2e5fe90,
    0x2e600a0,
    0x2e605b8,
    0x2e67bb0,
    0x3141a38,
    0x3142bc0,
    0x3142d48,
    0x314cae0,
    0x31592e0,
    0x315e560,
    0x3480128,
    0x3b25a60,
    0x3b32488,
    0x3b35728,
    0x3b35f40,
    0x3b35fb8,
    0x3b8dcb8,
    0x3b8e9f8,
};
static const uint32_t ep147_addr_v200[EPI_N] = {
    0x0426570,   /* hand: operator new: the new(0x230) for the sub column button in the TrackListHeader ctor */
    0x05d8a70,   /* code w=12 */
    0x06b5d88,   /* code w=12 | xref 6/6 agree */
    0x06ede50,   /* code w=12 | xref 1/1 agree */
    0x06f93e8,   /* code w=12 | xref 6/6 agree */
    0x074a460,   /* code w=12 | ptr 1 agree */
    0x07e27c8,   /* hand: the only regObject instance whose adrp+add+add chain materialises the __PRETTY_FUNCT */
    0x0a17120,   /* ptr 1 agree */
    0x0a1a0f8,   /* xref 1/1 agree */
    0x0abda68,   /* code w=12 | ptr 1 agree */
    0x0abdd18,   /* code w=12 | xref 1/1 agree */
    0x0aefcd0,   /* code w=12 | xref 1/1 agree | ptr 1 agree */
    0x0af1ec0,   /* code w=12 | xref 1/1 agree | ptr 1 agree */
    0x0d00f08,   /* code w=12 | ptr 1 agree */
    0x0dbe648,   /* code w=12 | xref 4/6 agree */
    0x0e10800,   /* code w=12 | xref 2/2 agree */
    0x0ebd910,   /* code w=12 | xref 1/1 agree */
    0x122f860,   /* hand: FolderEntryDigger: re-read one analysed folder from disk (digger, folderID) (masked- */
    0x1230f48,   /* code w=12 | xref 1/1 agree */
    0x12424a8,   /* code w=96 | ptr 1 agree */
    0x12df7e8,   /* code w=12 | xref 3/3 agree */
    0x12dfc18,   /* code w=12 | ptr 1 agree */
    0x12ff178,   /* code w=96 | xref 3/3 agree */
    0x12ff1e8,   /* code w=96 | xref 4/4 agree */
    0x12ff2c8,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff338,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff3a8,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff418,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff488,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff4f8,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff568,   /* hand: RTTI: the function whose lambda manager returns the type_info of ListCacheCollector: */
    0x12ff5d8,   /* xref 1/1 agree */
    0x12ff648,   /* code w=96 */
    0x12ff6b8,   /* code w=96 | xref 1/1 agree */
    0x12ff728,   /* code w=96 | xref 1/1 agree */
    0x12ff798,   /* hand: ListCacheCollector::removeFolderListCache(u32) (masked-signature match, unique) */
    0x12ff968,   /* code w=12 | xref 6/6 agree */
    0x1322908,   /* code w=12 | xref 1/1 agree */
    0x132a1f0,   /* code w=12 | xref 1/1 agree */
    0x132df70,   /* code w=12 | xref 1/1 agree */
    0x1403f98,   /* hand: NetworkManager::onLinkMemberRemoved (masked-signature match, unique) */
    0x1404480,   /* hand: NetworkManager::onLinkMemberAdded (masked-signature match, unique) */
    0x150f568,   /* code w=12 | xref 2/3 agree */
    0x1510ac0,   /* code w=12 | xref 2/2 agree */
    0x1512bc0,   /* code w=12 */
    0x1513214,   /* code w=12 */
    0x1530af0,   /* code w=12 back=8 | xref 4/4 agree */
    0x155f500,   /* ptr 1 agree */
    0x19ef578,   /* code w=96 | ptr 2 agree */
    0x1a384a8,   /* code w=12 | ptr 1 agree */
    0x1abfdd0,   /* code w=12 back=8 | xref 6/6 agree */
    0x1ac4f40,   /* code w=12 | xref 6/6 agree */
    0x1aeb1a0,   /* code w=12 | ptr 1 agree */
    0x1b0f618,   /* code w=12 | xref 1/1 agree | ptr 4 agree */
    0x1b0f8d8,   /* code w=12 back=32 | xref 1/1 agree | ptr 4 agree */
    0x1b106c0,   /* code w=12 | xref 1/1 agree */
    0x1b10860,   /* code w=12 | xref 2/2 agree | ptr 3 agree */
    0x1b21130,   /* code w=12 | ptr 1 agree */
    0x1b21148,   /* code w=12 */
    0x1b3aaa0,   /* code w=24 | ptr 1 agree */
    0x1b3bea0,   /* code w=12 | xref 6/6 agree */
    0x1b3fa60,   /* code w=12 */
    0x1b8d998,   /* code w=12 | ptr 1 agree */
    0x1b8db58,   /* code w=12 | ptr 1 agree */
    0x1bdf2f8,   /* hand: gui::source_select::DevicelessLoginWidgetController timer tick (crash guard): 2.00 v */
    0x1c2ded8,   /* code w=12 | xref 1/1 agree */
    0x1c2e028,   /* code w=12 | xref 1/1 agree */
    0x1c2e4e0,   /* code w=12 | xref 1/1 agree */
    0x1c2f8f8,   /* code w=12 | xref 1/1 agree */
    0x1c30340,   /* code w=12 | xref 3/4 agree */
    0x1c3072c,   /* code w=12 */
    0x1c30808,   /* code w=12 | xref 6/6 agree */
    0x1c32e40,   /* code w=12 | xref 1/1 agree */
    0x1c32eb0,   /* code w=12 | xref 1/1 agree */
    0x1c338d0,   /* code w=12 | xref 2/2 agree */
    0x1c34e48,   /* code w=12 | xref 4/4 agree */
    0x1c373f0,   /* code w=12 */
    0x1c67308,   /* code w=24 | ptr 1 agree */
    0x1cc0c58,   /* code w=24 | ptr 1 agree */
    0x1cc31b0,   /* code w=12 | xref 1/1 agree */
    0x1cc50c8,   /* code w=12 | ptr 1 agree */
    0x1cc50d0,   /* code w=12 | xref 6/6 agree */
    0x1cc51a8,   /* code w=12 | ptr 1 agree */
    0x1cf8f50,   /* code w=12 | xref 2/2 agree */
    0x1cfd028,   /* code w=12 | xref 1/1 agree */
    0x1cfded0,   /* code w=48 | ptr 1 agree */
    0x1cfdfb8,   /* code w=48 | ptr 1 agree */
    0x1cff480,   /* code w=12 | ptr 1 agree */
    0x1d01398,   /* code w=48 back=8 | xref 1/1 agree */
    0x1d0c428,   /* code w=12 | xref 1/3 agree */
    0x1d16f40,   /* code w=12 | ptr 1 agree */
    0x1d22f70,   /* hand: the sub column button's image (juce::Image sret) from the resources: the TrackListHe */
    0x1d2d598,   /* hand: the image resources getter the TrackListHeader ctor calls before the button's image */
    0x1d2ea98,   /* code w=12 */
    0x1d38fa8,   /* code w=12 | xref 2/2 agree */
    0x1d393b8,   /* hand: gui::browse::TrackListHeader ctor(this, mode): stores BC_HDR_VT and the mode at +0x1 */
    0x1d39858,   /* hand: TrackListHeader: add a listener to [this+0x138] (only in mode 0: ldr w2,[x0,#0x134]; */
    0x1d39a68,   /* hand: TrackListHeader: places its sub column button at the right of the sub column (getInd */
    0x1d39b20,   /* hand: gui::browse::TrackListHeader column layout(header, row height): the only function th */
    0x1d447f0,   /* ptr 1 agree */
    0x1d466f8,   /* hand: TrackListWidget vtable slot +0x58 in both builds (neighbouring slots +0x40..+0x68 un */
    0x1d82a20,   /* code w=12 | xref 2/2 agree */
    0x1d89250,   /* code w=12 | xref 2/2 agree */
    0x1d931d0,   /* hand: SubCategorySelectPopupWidget: ldr x0,[x0,#0x58] then a tail call to the TableListBox */
    0x1d931f0,   /* hand: SubCategorySelectPopupWidget: appends a juce::String to the label array at +0x60 (gr */
    0x1d9e3f0,   /* code w=12 | ptr 1 agree */
    0x1dd7f60,   /* hand: gui::status_area::browse::FunctionButtonWidgetController ctor: called by the StatusA */
    0x1dd82c8,   /* hand: PREVIEW set on gui::status_area::browse::FunctionButtonWidgetController: the Browse  */
    0x1ddab20,   /* xref 1/1 agree */
    0x1ddab80,   /* code w=12 | xref 2/3 agree */
    0x1ddb278,   /* code w=12 | xref 3/5 agree */
    0x1e0cfe0,   /* code w=12 | xref 1/1 agree */
    0x1e0e488,   /* code w=12 */
    0x1ec91d8,   /* code w=12 | xref 6/6 agree */
    0x1ed5f28,   /* hand: instruction fingerprint mov x0,#0x1dc; movk x0,#0x207,lsl#32 unique in both builds ( */
    0x1ed6308,   /* hand: SimpleWaveform2DeckWidget Label setText(this+Label, text, 0): 1.30 slot 13 = ldr x0, */
    0x1ed9d50,   /* hand: SimpleWaveform4DeckWidget: same as HT_SW_ORIG2 (2.00 slot 14; slot 13 0x1ed9d10 is t */
    0x1ee21c8,   /* hand: gui::waveform::KeyWidget: writes the tray KEY Label (+0x140) with juce::Label::setTe */
    0x1ee2718,   /* hand: KeyWidget: isEnabled() of its KEY row arrow (+0x148; ldr x0,[x0,#0x148]; b Component */
    0x1eedca0,   /* code w=12 | xref 4/4 agree */
    0x1f0ad70,   /* hand: TrackInfo2DeckWidget title setText: 1.30 slot 14 = ldr x0,[x0,#360]; mov w2,#0; b 0x */
    0x1f0c3a0,   /* hand: TrackInfo4DeckWidget: same as HT_ORIG2 (2.00 slot 15; slot 14 0x1f0c380 is the new s */
    0x1f49c08,   /* code w=12 | xref 1/1 agree */
    0x1f4ad18,   /* code w=12 | xref 3/3 agree */
    0x1f4c428,   /* code w=12 | xref 1/1 agree */
    0x1f4c4b8,   /* code w=12 | xref 1/1 agree */
    0x1f4d300,   /* code w=12 | xref 1/1 agree */
    0x1f4d830,   /* code w=12 | ptr 1 agree */
    0x1f511c0,   /* code w=12 | xref 3/3 agree */
    0x1f51298,   /* code w=12 | xref 3/3 agree */
    0x1f513c0,   /* code w=12 */
    0x1f51400,   /* code w=96 */
    0x1f514d0,   /* code w=96 | xref 1/1 agree */
    0x1f515a0,   /* code w=12 | xref 1/1 agree */
    0x1f52070,   /* code w=12 | xref 1/1 agree */
    0x1f52110,   /* code w=12 */
    0x1f52180,   /* code w=12 | xref 1/1 agree */
    0x1f53e00,   /* code w=12 | xref 1/1 agree */
    0x1f56328,   /* code w=12 | xref 1/1 agree */
    0x1f57530,   /* code w=12 */
    0x1f5fdc0,   /* code w=12 | xref 1/1 agree */
    0x1f60be8,   /* code w=24 | xref 2/2 agree */
    0x1f61aa8,   /* code w=96 | xref 1/1 agree */
    0x1f622f0,   /* code w=12 | xref 6/6 agree */
    0x1f62520,   /* code w=12 | xref 6/6 agree */
    0x1f62cd8,   /* code w=12 | xref 1/1 agree */
    0x1f675c8,   /* code w=12 | xref 1/1 agree */
    0x1f689b0,   /* code w=12 | xref 1/1 agree */
    0x1f6afb8,   /* code w=12 */
    0x1f6b808,   /* code w=12 | xref 1/1 agree */
    0x1f6c0b8,   /* code w=12 back=32 | xref 1/3 agree */
    0x1f6d048,   /* code w=12 | xref 1/3 agree */
    0x1f6ef50,   /* code w=12 | ptr 1 agree */
    0x1f72658,   /* hand: the sub column button's ctor (btn, &name, &image, &image, &image, 1, 1, 0.3f, 0.5f): */
    0x1f733a8,   /* code w=12 | ptr 4 agree */
    0x1f93f08,   /* hand: vtable slot 2 (timerCallback) of N3gui11player_info11LocalPlayerE: v2.00 vtable 0x31 */
    0x1fb25f8,   /* code w=12 | xref 6/6 agree */
    0x1fb2ef0,   /* code w=12 | xref 6/6 agree */
    0x1fd3fa8,   /* code w=12 | xref 2/2 agree */
    0x20173f0,   /* code w=12 back=8 | xref 4/6 agree */
    0x2030354,   /* hand: return address of the indirect call `ldr x1,[x0]; ldr x1,[x1,#0x70]; blr x1` inside  */
    0x2071608,   /* code w=24 back=8 | ptr 1 agree */
    0x2091660,   /* hand: gui::browser_logic::TrackListDisplayFormatEventFacade ctor: stores BC_FC_VT (1.30 0x */
    0x20ac490,   /* hand: gui::disp_data::BrowseDispData ctor: stores B2_BDD_VT and builds its sub objects, th */
    0x20ad318,   /* code w=12 | xref 1/1 agree */
    0x20b0ca8,   /* hand: the only BrowseLineData-vtable referrer between its independently relocated neighbou */
    0x20b1d78,   /* xref 2/2 agree */
    0x20b6768,   /* code w=12 | xref 1/1 agree */
    0x20b6908,   /* code w=12 | xref 6/6 agree */
    0x20b7008,   /* code w=12 | xref 5/6 agree */
    0x20c9d90,   /* hand: callers 1/1 agree; normalized disassembly 0.96 identical over 400 instructions */
    0x2115140,   /* code w=12 | xref 6/6 agree */
    0x215d140,   /* hand: masked code signature (12 words, unique); the only caller on both builds is PlayerIn */
    0x215e9e0,   /* hand: masked code signature (12 words, unique) and its only caller (1.30 0x1ff34e0 -> 2.00 */
    0x2169498,   /* hand: masked code signature (12 words, unique) and its only caller, TrackInfoUpdater::upda */
    0x21800c8,   /* hand: the return address of the only call to the gui::FontManager ctor (v1.30 0x1f4ac90, v */
    0x21f2df8,   /* code w=12 | ptr 1 agree */
    0x21f3dd8,   /* code w=12 | ptr 1 agree */
    0x2289588,   /* code w=12 | xref 1/1 agree */
    0x2289ce0,   /* code w=12 | xref 1/1 agree */
    0x2289db8,   /* code w=12 | xref 1/1 agree */
    0x229ce28,   /* code w=12 */
    0x229d034,   /* code w=12 */
    0x22f7b78,   /* hand: TrackInfoQueue vtable slot +0x20 on both builds (1.30 vt 0x2e509c8, 2.00 vt 0x3203ee */
    0x22f7d30,   /* hand: TrackInfoQueue vtable slot +0x58 on both builds (1.30 0x2e50a20, 2.00 0x3203f40); bo */
    0x2323a00,   /* code w=12 | xref 1/1 agree */
    0x2335660,   /* code w=12 | ptr 4 agree */
    0x234e530,   /* code w=12 | ptr 4 agree */
    0x23508e0,   /* code w=12 | xref 6/6 agree */
    0x23519f0,   /* code w=12 | xref 4/4 agree */
    0x2351d50,   /* code w=12 | xref 6/6 agree */
    0x2354280,   /* code w=12 | xref 6/6 agree */
    0x23549e0,   /* code w=12 | xref 1/1 agree */
    0x2355a30,   /* code w=12 | xref 6/6 agree */
    0x23578f0,   /* code w=12 | ptr 1 agree */
    0x2357b10,   /* code w=12 | ptr 1 agree */
    0x2392400,   /* code w=12 | xref 2/2 agree */
    0x2468c70,   /* code w=12 | xref 6/6 agree | ptr 1 agree */
    0x2469aa0,   /* code w=12 | xref 6/6 agree */
    0x2478900,   /* code w=12 | xref 4/6 agree */
    0x247ac30,   /* code w=12 | xref 6/6 agree */
    0x247b880,   /* code w=48 | xref 5/6 agree */
    0x247c9d0,   /* code w=24 | xref 6/6 agree */
    0x247ca60,   /* code w=12 | xref 6/6 agree */
    0x24a1180,   /* code w=96 | xref 6/6 agree */
    0x24a97e0,   /* hand: juce::String(CharPointer_UTF8) (this, &utf8): the call the sub item formatter makes  */
    0x2505da0,   /* code w=12 | xref 6/6 agree */
    0x250a240,   /* code w=12 | xref 6/6 agree */
    0x2510300,   /* code w=48 | xref 6/6 agree */
    0x2511a90,   /* code w=24 | xref 5/6 agree */
    0x2512980,   /* hand: juce::Colour::withMultipliedAlpha(float), result through x8: drawLabel, right after  */
    0x251c2e0,   /* code w=24 | xref 6/6 agree */
    0x251c390,   /* code w=12 | xref 6/6 agree */
    0x251c3e0,   /* code w=12 | xref 6/6 agree */
    0x251c5e0,   /* code w=12 | xref 2/6 agree */
    0x251ca30,   /* hand: juce::Graphics::setFont(const Font &): drawLabel, x1 = the Label + 0x178 (its font)  */
    0x251cb20,   /* code w=12 | xref 3/6 agree */
    0x251cb60,   /* code w=12 | xref 6/6 agree */
    0x251d350,   /* code w=12 | xref 6/6 agree */
    0x251d980,   /* code w=12 | xref 4/6 agree */
    0x251e750,   /* hand: juce::Image(): the two empty images the TrackListHeader ctor passes to the button ct */
    0x251e8b0,   /* code w=24 | xref 6/6 agree */
    0x251e8e0,   /* code w=48 | xref 6/6 agree */
    0x2522080,   /* code w=12 | xref 5/5 agree */
    0x254b730,   /* code w=12 | xref 4/6 agree */
    0x25586a0,   /* hand: juce::Graphics::drawFittedText(String, Rectangle<int>, Justification, maxLines, minS */
    0x255ac60,   /* code w=12 | xref 6/6 agree */
    0x255b740,   /* code w=12 | xref 4/6 agree */
    0x255e810,   /* code w=12 | ptr 2 agree */
    0x256b2c0,   /* code w=12 | ptr 2 agree */
    0x256bcb0,   /* hand: SoftwareRenderer vtable (1.30 0x3142bc0) slot +0xc0, fillPath: the slot value read l */
    0x256f270,   /* code w=12 | ptr 2 agree */
    0x2570490,   /* code w=12 | ptr 2 agree */
    0x259a540,   /* code w=24 | xref 6/6 agree | ptr 4 agree */
    0x25a4530,   /* hand: juce::Component::isEnabled: drawLabel (1.30 0x2475408 / 2.00 0x25e270c) picks alpha  */
    0x25aa670,   /* code w=12 | xref 4/6 agree */
    0x25aa6a0,   /* code w=12 | xref 4/4 agree */
    0x25c69e0,   /* hand: juce::TableHeaderComponent::getIndexOfColumnId(id, onlyVisible): called by the heade */
    0x25c6ae0,   /* hand: juce::TableHeaderComponent::getColumnPosition(index) (Rectangle<int> sret): the head */
    0x25c6c80,   /* hand: juce::TableHeaderComponent::isColumnVisible(id): the call BC_LAYOUT makes for each c */
    0x25d3110,   /* code w=12 | xref 6/6 agree */
    0x25de6a0,   /* hand: juce::Component::findColour(id), result through x8: the call after mov w1,#0x281 in  */
    0x25f03d0,   /* hand: juce::Component::getComponentAt(const Point<int> &): the only self-recursive functio */
    0x25f25e0,   /* hand: juce::Component::addChildComponent body (parent, child, zOrder): addAndMakeVisible t */
    0x25f31e0,   /* code w=12 | xref 6/6 agree */
    0x25f5a10,   /* code w=24 | xref 6/6 agree */
    0x25f76f0,   /* code w=12 | xref 3/3 agree */
    0x25f7d90,   /* hand: juce::TableHeaderComponent::setColumnVisible(id, bool): identical bodies (search [th */
    0x25f7fa0,   /* code w=12 | xref 6/6 agree */
    0x25f9460,   /* hand: juce::TableHeaderComponent::columnClicked(this, columnId, mods): a sortable column,  */
    0x25ff370,   /* hand: RTTI: slot 0 (complete-object destructor) of the primary vtable whose type_info name */
    0x25ff990,   /* code w=12 | xref 1/6 agree */
    0x2607950,   /* hand: juce::Component::removeChildComponent(Component *): addAndMakeVisible (1.30 0x249d6b */
    0x260ab50,   /* code w=12 | xref 6/6 agree */
    0x260af90,   /* code w=12 | xref 6/6 agree | ptr 4 agree */
    0x260c480,   /* code w=12 | xref 6/6 agree */
    0x260c6a0,   /* hand: juce::Component::setSize(w, h): setSize(0x50, 0x3c) on the new sub column button in  */
    0x260dd30,   /* hand: juce::Component::setBounds(const Rectangle<int> &): the header's button placement ca */
    0x2663060,   /* code w=12 | ptr 1 agree */
    0x2663fd4,   /* code w=12 */
    0x2667b90,   /* code w=12 | xref 2/2 agree */
    0x26680b4,   /* code w=12 */
    0x26680ec,   /* code w=12 */
    0x26687f0,   /* code w=12 | ptr 1 agree */
    0x28fa0c0,   /* hand: section headers: .rodata sh_addr (v1.30 value reproduces azmod's literal) */
    0x2919bb0,   /* vtable N12midi_adapter14ReceiveManagerE +0x0 | xref 2/2 agree */
    0x2923200,   /* vtable N23connection_with_hostapp15IPcAppliSetMode4ModeE +0x0 | xref 5/6 agree */
    0x29de458,   /* vtable N7usecase12library_edit28PlaylistRenewalNoticeHandlerE +0x0 */
    0x29ded30,   /* vtable N7usecase12library_edit13TaglistEditorE +0x0 | xref 5/6 agree */
    0x2a0a000,   /* vtable N7usecase5mixer7ChannelE +0x0 | xref 6/6 agree */
    0x2a1c5d8,   /* vtable N7usecase11phase_meter11LocalPlayerE +0x0 */
    0x2b1e4d8,   /* vtable N23connection_with_hostapp14AlphaThetaCertE +0x0 | xref 2/2 agree */
    0x2b991d0,   /* hand: music_library::FolderEntryDigger vtable (vtable relocation, RTTI) */
    0x2b9cbe0,   /* vtable N13music_library21SqlitePlaylistUpdaterE +0x0 | xref 3/3 agree */
    0x2b9d140,   /* hand: RTTI: the vtable address point of N13music_library15SqliteSortEntryE; slot +0x10 rea */
    0x2b9d368,   /* vtable N13music_library14SqliteDataBaseE +0x0 | xref 4/4 agree */
    0x2ba6768,   /* vtable N13music_library14TrackListCacheE +0x0 | xref 3/3 agree */
    0x2ba9520,   /* vtable N13music_library18InformationUpdaterE +0x0 | xref 3/3 agree */
    0x2bb1268,   /* vtable N13music_library18MusicLibraryServerE +0x0 | xref 1/2 agree */
    0x2bc4c38,   /* hand: network::NetworkManager vtable (onLinkMemberAdded +0x48, onLinkMemberRemoved +0x50)  */
    0x2bc63a0,   /* vtable N12player_basis10PlayerCoreE +0x0 | xref 2/2 agree */
    0x2be9658,   /* vtable N9dj_player18VinylSpeedAdjustorE +0x0 | xref 1/1 agree */
    0x2cd9588,   /* hand: RTTI: the vtable address point of N3gui9disp_data19SubCategoryDispListE; slots 2..4  */
    0x2cd9620,   /* vtable N3gui9disp_data14BrowseLineDataE +0x0 */
    0x2cd9780,   /* vtable N3gui18ToggleImagesButtonE +0x0 | xref 5/6 agree */
    0x2cd9930,   /* vtable N3gui18ToggleImagesButtonE +0x0 | xref 5/6 agree */
    0x2cd9960,   /* vtable N3gui18ToggleImagesButtonE +0x0 | xref 4/6 agree */
    0x2cd9988,   /* vtable N3gui16GuiObjectManagerE +0x0 | xref 1/2 agree */
    0x2cd99a8,   /* vtable N2ui14UiLayerObjectsE +0x0 | xref 3/3 agree */
    0x2cda800,   /* vtable 16EP147Application +0x0 | xref 2/2 agree */
    0x2d3ad00,   /* vtable St23_Sp_counted_ptr_inplaceIN3gui7caution7browser17DuplicatedCautionESaIS3_ELN9__gn */
    0x2d3bd78,   /* vtable St23_Sp_counted_ptr_inplaceIN3gui7caution6simple18PlayerErrorCautionESaIS3_ELN9__gn */
    0x2d3d268,   /* vtable N3gui9disp_data10BrowseMenuE +0x0 | xref 1/2 agree */
    0x2d41b00,   /* vtable N3gui24DisplayRefreshCycleTimerE +0x0 | xref 2/2 agree */
    0x2d4ca88,   /* vtable N3gui6browse14BrowseListViewE +0x0 | xref 2/2 agree */
    0x2d4ea68,   /* vtable N3gui6browse10BrowseViewE +0x0 | xref 6/6 agree */
    0x2d523c8,   /* vtable N3gui6browse12PlaylistViewE +0x0 | xref 6/6 agree */
    0x2d531e8,   /* vtable N3gui6browse10SearchViewE +0x0 | xref 2/2 agree */
    0x2d537e8,   /* xref 2/2 agree */
    0x2d541e8,   /* vtable N3gui6browse11TagListViewE +0x0 | xref 6/6 agree */
    0x2d59258,   /* vtable N3gui8shortcut12ShortcutViewE +0x0 | xref 2/2 agree */
    0x2d5ab98,   /* vtable N3gui13source_select16SourceSelectViewE +0x0 | xref 2/2 agree */
    0x2d61890,   /* vtable N3gui15ImagesComponentE +0x0 | xref 6/6 agree */
    0x2d61ae0,   /* vtable N3gui8waveform12WaveformViewE +0x0 | xref 2/2 agree */
    0x2d91cc0,   /* vtable of the streaming-login widget controller +0x0 | xref 6/6 agree */
    0x2e0b900,   /* vtable N3gui13source_select16SourceTableModelE +0x0 | xref 3/3 agree */
    0x2e20228,   /* vtable N3gui8shortcut26VinylSpeedAdjustTimeWidgetE +0x0 | xref 2/2 agree */
    0x2e36388,   /* vtable N3gui11device_icon30WaveformLoadedDeviceIconWidgetE +0x0 | xref 2/2 agree */
    0x2eb1778,   /* vtable N3gui16BrowseMenuWidgetE +0x0 | xref 2/2 agree */
    0x2ebea58,   /* vtable N3gui26BrowseMenuWidgetControllerE +0x0 | xref 2/2 agree */
    0x2ebeac0,   /* vtable N3gui26BrowseMenuWidgetControllerE +0x0 | xref 2/2 agree */
    0x2edd5e0,   /* vtable N3gui17detailed_waveform8LoopIconE +0x0 | xref 6/6 agree | ptr 1 agree */
    0x2ee0728,   /* vtable N3gui17detailed_waveform21PlayPositionBarWidgetE +0x0 | xref 6/6 agree */
    0x2ee0c08,   /* vtable N3gui8waveform22DetailedWaveformWidgetIN14appnd_trk_info21DetailedWaveform_BlueENS0 */
    0x2ee1278,   /* vtable N3gui8waveform22DetailedWaveformWidgetIN14appnd_trk_info20DetailedWaveform_RGBENS0_ */
    0x2ee18e8,   /* vtable N3gui8waveform22DetailedWaveformWidgetIN14appnd_trk_info22DetailedWaveform_3BandENS */
    0x2ee9078,   /* vtable N3gui17detailed_waveform8own_deck14WaveformWidgetE +0x0 | xref 2/2 agree | ptr 1 ag */
    0x2ee92d0,   /* vtable N3gui17detailed_waveform8own_deck14WaveformWidgetE +0x0 | xref 2/2 agree | ptr 1 ag */
    0x2eecd70,   /* vtable N3gui8waveform28DetailedWaveformDataProviderIN14appnd_trk_info21DetailedWaveform_Bl */
    0x2eecdc0,   /* vtable N3gui8waveform28DetailedWaveformDataProviderIN14appnd_trk_info20DetailedWaveform_RG */
    0x2eece10,   /* vtable N3gui8waveform35DetailedWaveformLayeredDataProviderIN14appnd_trk_info22DetailedWave */
    0x2eee3c8,   /* vtable N3gui27DetailedWaveformFieldWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x2eee650,   /* vtable N3gui27DetailedWaveformFieldWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x2ef2ec8,   /* vtable N3gui6browse19HierarchyListWidgetE +0x0 | xref 6/6 agree | ptr 1 agree */
    0x2ef2fc8,   /* vtable N3gui6browse19HierarchyListWidgetE +0x0 | xref 6/6 agree */
    0x2ef3830,   /* vtable N3gui23IconHoldableScrollLabelE +0x0 | xref 2/2 agree */
    0x2ef7800,   /* hand: RTTI: the primary vtable address point of N3gui6browse15TrackListHeaderE, stored by  */
    0x2ef79b0,   /* hand: the 8 bytes the TrackListHeader ctor loads (ldr d0) and stores at button+0x1c0 */
    0x2ef9338,   /* vtable N3gui6browse15TrackListWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x2ef94c0,   /* vtable N3gui6browse15TrackListWidgetE +0x0 | xref 4/4 agree */
    0x2ef9618,   /* vtable N3gui6browse15TrackListWidgetE +0x0 | xref 4/4 agree */
    0x2f027a0,   /* hand: RTTI: the primary vtable (offset-to-top 0) of N3gui11performance14KeyShiftWidgetE (t */
    0x2f02978,   /* hand: RTTI: the same vtable group, the entry with offset-to-top -112: the KeyShiftWidget j */
    0x2f18ab8,   /* vtable N3gui31OverviewWaveform1200PointWidgetE +0x0 | xref 6/6 agree */
    0x2f77aa0,   /* hand: RTTI: the vtable of N3gui6browse28SubCategorySelectPopupWidgetE with offset-to-top - */
    0x2f79150,   /* vtable N3gui11ScrollLabelE +0x0 | xref 6/6 agree */
    0x2fd44b8,   /* vtable N3gui22SoftwareKeyboardWidgetE +0x0 | xref 2/2 agree */
    0x2ffc2a0,   /* hand: the vtable address point BC_FB_CTOR stores in both builds (typeinfo N3gui11status_ar */
    0x2ffc330,   /* vtable N3gui11status_area6browse20BrowseFunctionWidgetE +0x0 | xref 2/2 agree */
    0x2ffe280,   /* vtable N3gui11status_area6browse26StatusAreaWidgetControllerE +0x0 | xref 2/2 agree */
    0x3006a58,   /* vtable N3gui11status_area8waveform16StatusAreaWidgetE +0x0 | xref 2/2 agree */
    0x3007890,   /* vtable N3gui11status_area8waveform27WaveformSettingButtonWidgetE +0x0 | xref 2/2 agree */
    0x3012548,   /* vtable N3gui11status_area21RecordingStatusWidgetE +0x0 | xref 2/2 agree */
    0x3084a68,   /* hand: RTTI: the only vtable whose type_info names N3gui14LinkInfoWidgetE (typeinfo 0x3084a */
    0x3093008,   /* vtable N3gui8waveform25SimpleWaveform2DeckWidgetE +0x0 */
    0x3098688,   /* vtable N3gui8waveform25SimpleWaveform4DeckWidgetE +0x0 */
    0x309f030,   /* hand: RTTI: the primary vtable (offset-to-top 0) of N3gui8waveform9KeyWidgetE; its ctor 1. */
    0x309f8c0,   /* vtable N3gui8waveform24DeckInfo2DeckFieldWidgetE +0x0 | xref 2/2 agree */
    0x30af430,   /* hand: RTTI: the primary vtable of N3gui8waveform19DeckInfo2DeckWidgetE (ctor 1.30 0x1d7bd8 */
    0x30af900,   /* hand: RTTI: the primary vtable of N3gui8waveform19DeckInfo4DeckWidgetE (ctor 1.30 0x1d82b0 */
    0x30bca40,   /* vtable N3gui8waveform19DeckInfoFieldWidgetE +0x0 | xref 1/2 agree */
    0x30cc9b8,   /* vtable N3gui8waveform17DeckNo2DeckWidgetE +0x0 | xref 4/6 agree */
    0x30cef28,   /* vtable N3gui8waveform17DeckNo4DeckWidgetE +0x0 | xref 4/6 agree */
    0x30d2888,   /* vtable N3gui8waveform11OnAirWidgetE +0x0 | xref 3/3 agree */
    0x30e84b0,   /* vtable N3gui8waveform20TrackInfo2DeckWidgetE +0x0 */
    0x30e8778,   /* hand: RTTI TrackInfo2DeckWidget: its 6th (juce::Component base) vtable; the other five kee */
    0x30ea650,   /* vtable N3gui8waveform20TrackInfo4DeckWidgetE +0x0 */
    0x30ea918,   /* hand: RTTI TrackInfo4DeckWidget: same as HT_CVT2 (6th vtable, ott -368 -> -472) */
    0x30ef370,   /* vtable N3gui8waveform21PlayerInfo2DeckWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x30ef598,   /* vtable N3gui8waveform21PlayerInfo2DeckWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x30f0328,   /* vtable N3gui8waveform21PlayerInfo4DeckWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x30f0550,   /* vtable N3gui8waveform21PlayerInfo4DeckWidgetE +0x0 | xref 2/2 agree | ptr 1 agree */
    0x311ee28,   /* vtable N3gui8waveform17BeatFxParamWidgetE +0x0 | xref 2/2 agree */
    0x311f450,   /* vtable N3gui8waveform18EffectReverbWidgetE +0x0 | xref 2/2 agree */
    0x31254d8,   /* vtable N3gui8waveform15XPadParamWidgetE +0x0 | xref 2/2 agree */
    0x3125640,   /* vtable N3gui8waveform15XPadParamWidgetE +0x168 | xref 2/2 agree */
    0x3127540,   /* vtable N3gui8waveform10XPadWidgetE +0x0 | xref 2/2 agree */
    0x312d190,   /* vtable N3gui8waveform8FxWidgetE +0x0 | xref 2/2 agree */
    0x3134e70,   /* hand: content: identical 622-byte PNG (md5 20d75b6995f0); 2 copies in both builds, this is */
    0x31374f0,   /* vtable N3gui8waveform12PadCueWidgetE +0x0 | xref 2/2 agree */
    0x313fc90,   /* vtable N3gui8waveform19PadModeStatusWidgetE +0x0 | xref 2/2 agree */
    0x313fe68,   /* vtable N3gui8waveform17PadSlipLoopWidgetE +0x0 | xref 2/3 agree */
    0x3140130,   /* vtable N3gui8waveform17PadSlipLoopWidgetE +0x2c8 */
    0x31403b0,   /* vtable N3gui8waveform17PadSlipLoopWidgetE +0x548 */
    0x3141f00,   /* ptr 1 agree */
    0x3142210,   /* vtable N3gui8waveform15PadStatusWidgetE +0x0 | xref 2/2 agree */
    0x3147be0,   /* ptr 1 agree */
    0x314be78,   /* vtable N3gui19PreviewableWaveformE +0x0 | xref 2/3 agree */
    0x31545b8,   /* vtable N3gui11player_info11LocalPlayerE +0x0 | xref 2/2 agree */
    0x3161130,   /* vtable N3gui13browser_logic32MusicLibraryCommunicationAdaptor14IReplyListenerE +0x8 */
    0x3174628,   /* vtable N3gui13browser_logic15MenuEventFacadeE +0x0 | xref 2/5 agree */
    0x317a690,   /* hand: RTTI: the vtable address point of N3gui13browser_logic33TrackListDisplayFormatEventF */
    0x317f388,   /* vtable N3gui9disp_data14BrowseDispDataE +0x0 | xref 2/3 agree */
    0x317f748,   /* vtable N3gui9disp_data21BrowseDispDataCreatorE +0x0 | xref 1/1 agree */
    0x317fc58,   /* vtable N3gui11FontManagerE +0x0 | xref 5/5 agree */
    0x31a1278,   /* hand: RTTI: the vtable whose type_info names St23_Sp_counted_ptr_inplaceIN3gui8BpmStateESa */
    0x31a3270,   /* hand: RTTI: the only vtable whose type_info names St23_Sp_counted_ptr_inplaceIN3gui15Playe */
    0x31a89f0,   /* hand: RTTI: the vtable address point of N3gui11sub_display14PlayerInfoDataE (offset-to-top */
    0x31ab400,   /* hand: RTTI: the primary vtable whose type_info names N3gui11sub_display20CurrentHotCueColl */
    0x31af1e0,   /* vtable N3gui7caution7browser14CautionManagerE +0x0 | xref 1/1 agree */
    0x31af208,   /* vtable N3gui7caution6simple14CautionManagerE +0x0 | xref 1/1 agree */
    0x31c8448,   /* vtable N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices23DeckOperationPadHandlerINS2_1 */
    0x31c8478,   /* vtable N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices23DeckOperationPadHandlerINS2_1 */
    0x31f4a58,   /* vtable N14device_adapter22MainDisplayUpdateTimerE +0x0 | xref 6/6 agree */
    0x3203ee8,   /* hand: RTTI: the vtable whose type_info names N14device_adapter11subcpu_comm9deck_ucom14Tra */
    0x32133a0,   /* vtable N4meow16TouchableListBox3RowE +0x0 | xref 6/6 agree */
    0x32135b0,   /* vtable N4meow16TouchableListBox12ListViewportE +0x0 | xref 6/6 agree */
    0x3213ac8,   /* vtable N4meow21TouchableTableListBox7RowCompE +0x0 | xref 6/6 agree */
    0x321b0c0,   /* vtable N4meow12TimerTaskBox4ImplE +0x0 */
    0x34f4f18,   /* vtable N4juce5Timer11TimerThread17CallTimersMessageE +0x0 | xref 1/1 agree */
    0x34f60a0,   /* vtable N4juce32LowLevelGraphicsSoftwareRendererE +0x0 | xref 2/2 agree */
    0x34f6228,   /* vtable N4juce17SoftwarePixelDataE +0x0 | xref 6/6 agree */
    0x34fffc0,   /* vtable N4juce28StandardCachedComponentImageE +0x0 | xref 3/3 agree */
    0x350c7c0,   /* vtable N4juce5LabelE +0x0 | xref 2/2 agree */
    0x3511a40,   /* vtable N4juce18LinuxComponentPeer19LinuxRepaintManagerE +0x0 | xref 3/3 agree */
    0x384b430,   /* hand: section headers: .rodata sh_addr+sh_size (v1.30 value reproduces azmod's literal) */
    0x3f84a88,   /* hand: dynamic relocations: the .rela.plt R_AARCH64_JUMP_SLOT r_offset for symbol XNextEven */
    0x3f92028,   /* hand: gui::detailed_waveform::DispStateHolder static zoom (u16 num, u16 den; .data 01 00 0 */
    0x3f953b8,   /* xref 2/2 agree */
    0x3f95bd0,   /* xref 2/2 agree */
    0x3f95c48,   /* xref 2/2 agree */
    0x3ff5f50,   /* xref 6/6 agree */
    0x3ff6c90,   /* hand: the juce::String the TrackListWidget button listener's click slot (vtable +0x10: v1. */
};
static uintptr_t ep147_addr[EPI_N];

/* 1 = every address this library uses exists in `build`; 0 = stay inert. */
static int ep147_addr_init(int build){
    const uint32_t *t = build == EPB_V130 ? ep147_addr_v130 : build == EPB_V200 ? ep147_addr_v200 : 0;
    if(!t) return 0;
    for(int i = 0; i < EPI_N; i++){ if(!t[i]) return 0; }
    for(int i = 0; i < EPI_N; i++) ep147_addr[i] = t[i];
    return 1;
}

#define EP_424d40 ((uintptr_t)ep147_addr[EPI_424d40])
#define EP_5acb58 ((uintptr_t)ep147_addr[EPI_5acb58])
#define EP_6742d8 ((uintptr_t)ep147_addr[EPI_6742d8])
#define EP_6ac3c0 ((uintptr_t)ep147_addr[EPI_6ac3c0])
#define EP_6b7958 ((uintptr_t)ep147_addr[EPI_6b7958])
#define EP_708a10 ((uintptr_t)ep147_addr[EPI_708a10])
#define EP_7a01e0 ((uintptr_t)ep147_addr[EPI_7a01e0])
#define EP_9d1ae8 ((uintptr_t)ep147_addr[EPI_9d1ae8])
#define EP_9d4ac0 ((uintptr_t)ep147_addr[EPI_9d4ac0])
#define EP_a77770 ((uintptr_t)ep147_addr[EPI_a77770])
#define EP_a77a20 ((uintptr_t)ep147_addr[EPI_a77a20])
#define EP_aa9a48 ((uintptr_t)ep147_addr[EPI_aa9a48])
#define EP_aabb78 ((uintptr_t)ep147_addr[EPI_aabb78])
#define EP_c85838 ((uintptr_t)ep147_addr[EPI_c85838])
#define EP_d34790 ((uintptr_t)ep147_addr[EPI_d34790])
#define EP_d85a08 ((uintptr_t)ep147_addr[EPI_d85a08])
#define EP_e33830 ((uintptr_t)ep147_addr[EPI_e33830])
#define EP_11755a0 ((uintptr_t)ep147_addr[EPI_11755a0])
#define EP_1176c88 ((uintptr_t)ep147_addr[EPI_1176c88])
#define EP_11881d8 ((uintptr_t)ep147_addr[EPI_11881d8])
#define EP_1224270 ((uintptr_t)ep147_addr[EPI_1224270])
#define EP_12246a0 ((uintptr_t)ep147_addr[EPI_12246a0])
#define EP_12439e8 ((uintptr_t)ep147_addr[EPI_12439e8])
#define EP_1243a58 ((uintptr_t)ep147_addr[EPI_1243a58])
#define EP_1243b38 ((uintptr_t)ep147_addr[EPI_1243b38])
#define EP_1243ba8 ((uintptr_t)ep147_addr[EPI_1243ba8])
#define EP_1243c18 ((uintptr_t)ep147_addr[EPI_1243c18])
#define EP_1243c88 ((uintptr_t)ep147_addr[EPI_1243c88])
#define EP_1243cf8 ((uintptr_t)ep147_addr[EPI_1243cf8])
#define EP_1243d68 ((uintptr_t)ep147_addr[EPI_1243d68])
#define EP_1243dd8 ((uintptr_t)ep147_addr[EPI_1243dd8])
#define EP_1243e48 ((uintptr_t)ep147_addr[EPI_1243e48])
#define EP_1243eb8 ((uintptr_t)ep147_addr[EPI_1243eb8])
#define EP_1243f28 ((uintptr_t)ep147_addr[EPI_1243f28])
#define EP_1243f98 ((uintptr_t)ep147_addr[EPI_1243f98])
#define EP_1244008 ((uintptr_t)ep147_addr[EPI_1244008])
#define EP_12441d8 ((uintptr_t)ep147_addr[EPI_12441d8])
#define EP_12671d0 ((uintptr_t)ep147_addr[EPI_12671d0])
#define EP_126eab0 ((uintptr_t)ep147_addr[EPI_126eab0])
#define EP_1272830 ((uintptr_t)ep147_addr[EPI_1272830])
#define EP_1348c48 ((uintptr_t)ep147_addr[EPI_1348c48])
#define EP_1349130 ((uintptr_t)ep147_addr[EPI_1349130])
#define EP_1453e80 ((uintptr_t)ep147_addr[EPI_1453e80])
#define EP_14553d8 ((uintptr_t)ep147_addr[EPI_14553d8])
#define EP_14574f8 ((uintptr_t)ep147_addr[EPI_14574f8])
#define EP_1457b4c ((uintptr_t)ep147_addr[EPI_1457b4c])
#define EP_14754e8 ((uintptr_t)ep147_addr[EPI_14754e8])
#define EP_14a3e10 ((uintptr_t)ep147_addr[EPI_14a3e10])
#define EP_1891d10 ((uintptr_t)ep147_addr[EPI_1891d10])
#define EP_18d8d18 ((uintptr_t)ep147_addr[EPI_18d8d18])
#define EP_195cc98 ((uintptr_t)ep147_addr[EPI_195cc98])
#define EP_1960f70 ((uintptr_t)ep147_addr[EPI_1960f70])
#define EP_1987038 ((uintptr_t)ep147_addr[EPI_1987038])
#define EP_19ab418 ((uintptr_t)ep147_addr[EPI_19ab418])
#define EP_19ab6d8 ((uintptr_t)ep147_addr[EPI_19ab6d8])
#define EP_19ac3c8 ((uintptr_t)ep147_addr[EPI_19ac3c8])
#define EP_19ac568 ((uintptr_t)ep147_addr[EPI_19ac568])
#define EP_19bcf68 ((uintptr_t)ep147_addr[EPI_19bcf68])
#define EP_19bcf80 ((uintptr_t)ep147_addr[EPI_19bcf80])
#define EP_19d6760 ((uintptr_t)ep147_addr[EPI_19d6760])
#define EP_19d7b60 ((uintptr_t)ep147_addr[EPI_19d7b60])
#define EP_19dd430 ((uintptr_t)ep147_addr[EPI_19dd430])
#define EP_1a29670 ((uintptr_t)ep147_addr[EPI_1a29670])
#define EP_1a29830 ((uintptr_t)ep147_addr[EPI_1a29830])
#define EP_1a79ea0 ((uintptr_t)ep147_addr[EPI_1a79ea0])
#define EP_1ac7848 ((uintptr_t)ep147_addr[EPI_1ac7848])
#define EP_1ac7998 ((uintptr_t)ep147_addr[EPI_1ac7998])
#define EP_1ac7e50 ((uintptr_t)ep147_addr[EPI_1ac7e50])
#define EP_1ac9268 ((uintptr_t)ep147_addr[EPI_1ac9268])
#define EP_1ac9cb0 ((uintptr_t)ep147_addr[EPI_1ac9cb0])
#define EP_1aca09c ((uintptr_t)ep147_addr[EPI_1aca09c])
#define EP_1aca178 ((uintptr_t)ep147_addr[EPI_1aca178])
#define EP_1acc7b0 ((uintptr_t)ep147_addr[EPI_1acc7b0])
#define EP_1acc820 ((uintptr_t)ep147_addr[EPI_1acc820])
#define EP_1acd240 ((uintptr_t)ep147_addr[EPI_1acd240])
#define EP_1ace798 ((uintptr_t)ep147_addr[EPI_1ace798])
#define EP_1ad0d40 ((uintptr_t)ep147_addr[EPI_1ad0d40])
#define EP_1afde70 ((uintptr_t)ep147_addr[EPI_1afde70])
#define EP_1b57640 ((uintptr_t)ep147_addr[EPI_1b57640])
#define EP_1b59b98 ((uintptr_t)ep147_addr[EPI_1b59b98])
#define EP_1b5bab0 ((uintptr_t)ep147_addr[EPI_1b5bab0])
#define EP_1b5bab8 ((uintptr_t)ep147_addr[EPI_1b5bab8])
#define EP_1b5bb90 ((uintptr_t)ep147_addr[EPI_1b5bb90])
#define EP_1b8f950 ((uintptr_t)ep147_addr[EPI_1b8f950])
#define EP_1b93a28 ((uintptr_t)ep147_addr[EPI_1b93a28])
#define EP_1b948d0 ((uintptr_t)ep147_addr[EPI_1b948d0])
#define EP_1b949b8 ((uintptr_t)ep147_addr[EPI_1b949b8])
#define EP_1b95e80 ((uintptr_t)ep147_addr[EPI_1b95e80])
#define EP_1b97d98 ((uintptr_t)ep147_addr[EPI_1b97d98])
#define EP_1ba2e28 ((uintptr_t)ep147_addr[EPI_1ba2e28])
#define EP_1bad940 ((uintptr_t)ep147_addr[EPI_1bad940])
#define EP_1bb99f0 ((uintptr_t)ep147_addr[EPI_1bb99f0])
#define EP_1bc41b0 ((uintptr_t)ep147_addr[EPI_1bc41b0])
#define EP_1bc5668 ((uintptr_t)ep147_addr[EPI_1bc5668])
#define EP_1bcfb78 ((uintptr_t)ep147_addr[EPI_1bcfb78])
#define EP_1bcff88 ((uintptr_t)ep147_addr[EPI_1bcff88])
#define EP_1bd03f0 ((uintptr_t)ep147_addr[EPI_1bd03f0])
#define EP_1bd0600 ((uintptr_t)ep147_addr[EPI_1bd0600])
#define EP_1bd06b8 ((uintptr_t)ep147_addr[EPI_1bd06b8])
#define EP_1bdb7e0 ((uintptr_t)ep147_addr[EPI_1bdb7e0])
#define EP_1bdcf98 ((uintptr_t)ep147_addr[EPI_1bdcf98])
#define EP_1c192a0 ((uintptr_t)ep147_addr[EPI_1c192a0])
#define EP_1c1fad0 ((uintptr_t)ep147_addr[EPI_1c1fad0])
#define EP_1c296b8 ((uintptr_t)ep147_addr[EPI_1c296b8])
#define EP_1c296d8 ((uintptr_t)ep147_addr[EPI_1c296d8])
#define EP_1c348c8 ((uintptr_t)ep147_addr[EPI_1c348c8])
#define EP_1c6e420 ((uintptr_t)ep147_addr[EPI_1c6e420])
#define EP_1c6e788 ((uintptr_t)ep147_addr[EPI_1c6e788])
#define EP_1c70fd0 ((uintptr_t)ep147_addr[EPI_1c70fd0])
#define EP_1c71030 ((uintptr_t)ep147_addr[EPI_1c71030])
#define EP_1c71728 ((uintptr_t)ep147_addr[EPI_1c71728])
#define EP_1ca31a8 ((uintptr_t)ep147_addr[EPI_1ca31a8])
#define EP_1ca4650 ((uintptr_t)ep147_addr[EPI_1ca4650])
#define EP_1d5ef18 ((uintptr_t)ep147_addr[EPI_1d5ef18])
#define EP_1d6bc60 ((uintptr_t)ep147_addr[EPI_1d6bc60])
#define EP_1d6bff0 ((uintptr_t)ep147_addr[EPI_1d6bff0])
#define EP_1d6f280 ((uintptr_t)ep147_addr[EPI_1d6f280])
#define EP_1d76f18 ((uintptr_t)ep147_addr[EPI_1d76f18])
#define EP_1d77468 ((uintptr_t)ep147_addr[EPI_1d77468])
#define EP_1d82b08 ((uintptr_t)ep147_addr[EPI_1d82b08])
#define EP_1d9fb68 ((uintptr_t)ep147_addr[EPI_1d9fb68])
#define EP_1da09b0 ((uintptr_t)ep147_addr[EPI_1da09b0])
#define EP_1ddd8d8 ((uintptr_t)ep147_addr[EPI_1ddd8d8])
#define EP_1dde9e8 ((uintptr_t)ep147_addr[EPI_1dde9e8])
#define EP_1de0148 ((uintptr_t)ep147_addr[EPI_1de0148])
#define EP_1de01d8 ((uintptr_t)ep147_addr[EPI_1de01d8])
#define EP_1de1020 ((uintptr_t)ep147_addr[EPI_1de1020])
#define EP_1de1550 ((uintptr_t)ep147_addr[EPI_1de1550])
#define EP_1de4ee0 ((uintptr_t)ep147_addr[EPI_1de4ee0])
#define EP_1de4fb8 ((uintptr_t)ep147_addr[EPI_1de4fb8])
#define EP_1de50e0 ((uintptr_t)ep147_addr[EPI_1de50e0])
#define EP_1de5120 ((uintptr_t)ep147_addr[EPI_1de5120])
#define EP_1de51f0 ((uintptr_t)ep147_addr[EPI_1de51f0])
#define EP_1de52c0 ((uintptr_t)ep147_addr[EPI_1de52c0])
#define EP_1de5d90 ((uintptr_t)ep147_addr[EPI_1de5d90])
#define EP_1de5e30 ((uintptr_t)ep147_addr[EPI_1de5e30])
#define EP_1de5ea0 ((uintptr_t)ep147_addr[EPI_1de5ea0])
#define EP_1de7b20 ((uintptr_t)ep147_addr[EPI_1de7b20])
#define EP_1dea048 ((uintptr_t)ep147_addr[EPI_1dea048])
#define EP_1deb250 ((uintptr_t)ep147_addr[EPI_1deb250])
#define EP_1df3ae0 ((uintptr_t)ep147_addr[EPI_1df3ae0])
#define EP_1df4908 ((uintptr_t)ep147_addr[EPI_1df4908])
#define EP_1df57c8 ((uintptr_t)ep147_addr[EPI_1df57c8])
#define EP_1df6010 ((uintptr_t)ep147_addr[EPI_1df6010])
#define EP_1df6240 ((uintptr_t)ep147_addr[EPI_1df6240])
#define EP_1df69f8 ((uintptr_t)ep147_addr[EPI_1df69f8])
#define EP_1dfb328 ((uintptr_t)ep147_addr[EPI_1dfb328])
#define EP_1dfc710 ((uintptr_t)ep147_addr[EPI_1dfc710])
#define EP_1dfed18 ((uintptr_t)ep147_addr[EPI_1dfed18])
#define EP_1dff558 ((uintptr_t)ep147_addr[EPI_1dff558])
#define EP_1dffe10 ((uintptr_t)ep147_addr[EPI_1dffe10])
#define EP_1e00dd0 ((uintptr_t)ep147_addr[EPI_1e00dd0])
#define EP_1e02d18 ((uintptr_t)ep147_addr[EPI_1e02d18])
#define EP_1e06450 ((uintptr_t)ep147_addr[EPI_1e06450])
#define EP_1e071a0 ((uintptr_t)ep147_addr[EPI_1e071a0])
#define EP_1e27d28 ((uintptr_t)ep147_addr[EPI_1e27d28])
#define EP_1e461f8 ((uintptr_t)ep147_addr[EPI_1e461f8])
#define EP_1e46af0 ((uintptr_t)ep147_addr[EPI_1e46af0])
#define EP_1e678f0 ((uintptr_t)ep147_addr[EPI_1e678f0])
#define EP_1eaacd8 ((uintptr_t)ep147_addr[EPI_1eaacd8])
#define EP_1ec495c ((uintptr_t)ep147_addr[EPI_1ec495c])
#define EP_1f05520 ((uintptr_t)ep147_addr[EPI_1f05520])
#define EP_1f25838 ((uintptr_t)ep147_addr[EPI_1f25838])
#define EP_1f3fb18 ((uintptr_t)ep147_addr[EPI_1f3fb18])
#define EP_1f40998 ((uintptr_t)ep147_addr[EPI_1f40998])
#define EP_1f44418 ((uintptr_t)ep147_addr[EPI_1f44418])
#define EP_1f45660 ((uintptr_t)ep147_addr[EPI_1f45660])
#define EP_1f4a3f0 ((uintptr_t)ep147_addr[EPI_1f4a3f0])
#define EP_1f4a590 ((uintptr_t)ep147_addr[EPI_1f4a590])
#define EP_1f4ac90 ((uintptr_t)ep147_addr[EPI_1f4ac90])
#define EP_1f5da20 ((uintptr_t)ep147_addr[EPI_1f5da20])
#define EP_1fa7a28 ((uintptr_t)ep147_addr[EPI_1fa7a28])
#define EP_1fefa78 ((uintptr_t)ep147_addr[EPI_1fefa78])
#define EP_1ff1250 ((uintptr_t)ep147_addr[EPI_1ff1250])
#define EP_1ffbc60 ((uintptr_t)ep147_addr[EPI_1ffbc60])
#define EP_201281c ((uintptr_t)ep147_addr[EPI_201281c])
#define EP_2085768 ((uintptr_t)ep147_addr[EPI_2085768])
#define EP_2086748 ((uintptr_t)ep147_addr[EPI_2086748])
#define EP_211c0e8 ((uintptr_t)ep147_addr[EPI_211c0e8])
#define EP_211c840 ((uintptr_t)ep147_addr[EPI_211c840])
#define EP_211c918 ((uintptr_t)ep147_addr[EPI_211c918])
#define EP_212f988 ((uintptr_t)ep147_addr[EPI_212f988])
#define EP_212fb94 ((uintptr_t)ep147_addr[EPI_212fb94])
#define EP_218a6d8 ((uintptr_t)ep147_addr[EPI_218a6d8])
#define EP_218a890 ((uintptr_t)ep147_addr[EPI_218a890])
#define EP_21b6560 ((uintptr_t)ep147_addr[EPI_21b6560])
#define EP_21c81c0 ((uintptr_t)ep147_addr[EPI_21c81c0])
#define EP_21e1090 ((uintptr_t)ep147_addr[EPI_21e1090])
#define EP_21e3440 ((uintptr_t)ep147_addr[EPI_21e3440])
#define EP_21e4550 ((uintptr_t)ep147_addr[EPI_21e4550])
#define EP_21e48b0 ((uintptr_t)ep147_addr[EPI_21e48b0])
#define EP_21e6de0 ((uintptr_t)ep147_addr[EPI_21e6de0])
#define EP_21e7540 ((uintptr_t)ep147_addr[EPI_21e7540])
#define EP_21e8590 ((uintptr_t)ep147_addr[EPI_21e8590])
#define EP_21ea450 ((uintptr_t)ep147_addr[EPI_21ea450])
#define EP_21ea670 ((uintptr_t)ep147_addr[EPI_21ea670])
#define EP_2224f60 ((uintptr_t)ep147_addr[EPI_2224f60])
#define EP_22fb7d0 ((uintptr_t)ep147_addr[EPI_22fb7d0])
#define EP_22fc600 ((uintptr_t)ep147_addr[EPI_22fc600])
#define EP_230b460 ((uintptr_t)ep147_addr[EPI_230b460])
#define EP_230d790 ((uintptr_t)ep147_addr[EPI_230d790])
#define EP_230e3e0 ((uintptr_t)ep147_addr[EPI_230e3e0])
#define EP_230f530 ((uintptr_t)ep147_addr[EPI_230f530])
#define EP_230f5c0 ((uintptr_t)ep147_addr[EPI_230f5c0])
#define EP_2333ce0 ((uintptr_t)ep147_addr[EPI_2333ce0])
#define EP_233c340 ((uintptr_t)ep147_addr[EPI_233c340])
#define EP_2398900 ((uintptr_t)ep147_addr[EPI_2398900])
#define EP_239cda0 ((uintptr_t)ep147_addr[EPI_239cda0])
#define EP_23a2e60 ((uintptr_t)ep147_addr[EPI_23a2e60])
#define EP_23a45f0 ((uintptr_t)ep147_addr[EPI_23a45f0])
#define EP_23a54e0 ((uintptr_t)ep147_addr[EPI_23a54e0])
#define EP_23aee40 ((uintptr_t)ep147_addr[EPI_23aee40])
#define EP_23aeef0 ((uintptr_t)ep147_addr[EPI_23aeef0])
#define EP_23aef40 ((uintptr_t)ep147_addr[EPI_23aef40])
#define EP_23af140 ((uintptr_t)ep147_addr[EPI_23af140])
#define EP_23af590 ((uintptr_t)ep147_addr[EPI_23af590])
#define EP_23af680 ((uintptr_t)ep147_addr[EPI_23af680])
#define EP_23af6c0 ((uintptr_t)ep147_addr[EPI_23af6c0])
#define EP_23afeb0 ((uintptr_t)ep147_addr[EPI_23afeb0])
#define EP_23b04e0 ((uintptr_t)ep147_addr[EPI_23b04e0])
#define EP_23b12b0 ((uintptr_t)ep147_addr[EPI_23b12b0])
#define EP_23b1410 ((uintptr_t)ep147_addr[EPI_23b1410])
#define EP_23b1440 ((uintptr_t)ep147_addr[EPI_23b1440])
#define EP_23b4be0 ((uintptr_t)ep147_addr[EPI_23b4be0])
#define EP_23de290 ((uintptr_t)ep147_addr[EPI_23de290])
#define EP_23eb200 ((uintptr_t)ep147_addr[EPI_23eb200])
#define EP_23ed7c0 ((uintptr_t)ep147_addr[EPI_23ed7c0])
#define EP_23ee2a0 ((uintptr_t)ep147_addr[EPI_23ee2a0])
#define EP_23f1370 ((uintptr_t)ep147_addr[EPI_23f1370])
#define EP_23fde20 ((uintptr_t)ep147_addr[EPI_23fde20])
#define EP_23fe810 ((uintptr_t)ep147_addr[EPI_23fe810])
#define EP_2401dd0 ((uintptr_t)ep147_addr[EPI_2401dd0])
#define EP_2402ff0 ((uintptr_t)ep147_addr[EPI_2402ff0])
#define EP_242d0a0 ((uintptr_t)ep147_addr[EPI_242d0a0])
#define EP_2437090 ((uintptr_t)ep147_addr[EPI_2437090])
#define EP_243d1d0 ((uintptr_t)ep147_addr[EPI_243d1d0])
#define EP_243d200 ((uintptr_t)ep147_addr[EPI_243d200])
#define EP_2459540 ((uintptr_t)ep147_addr[EPI_2459540])
#define EP_2459640 ((uintptr_t)ep147_addr[EPI_2459640])
#define EP_24597e0 ((uintptr_t)ep147_addr[EPI_24597e0])
#define EP_2465c70 ((uintptr_t)ep147_addr[EPI_2465c70])
#define EP_2471200 ((uintptr_t)ep147_addr[EPI_2471200])
#define EP_2482f30 ((uintptr_t)ep147_addr[EPI_2482f30])
#define EP_2485140 ((uintptr_t)ep147_addr[EPI_2485140])
#define EP_2485d40 ((uintptr_t)ep147_addr[EPI_2485d40])
#define EP_2488570 ((uintptr_t)ep147_addr[EPI_2488570])
#define EP_248a250 ((uintptr_t)ep147_addr[EPI_248a250])
#define EP_248a8f0 ((uintptr_t)ep147_addr[EPI_248a8f0])
#define EP_248ab00 ((uintptr_t)ep147_addr[EPI_248ab00])
#define EP_248bfc0 ((uintptr_t)ep147_addr[EPI_248bfc0])
#define EP_2491ed0 ((uintptr_t)ep147_addr[EPI_2491ed0])
#define EP_24924f0 ((uintptr_t)ep147_addr[EPI_24924f0])
#define EP_249a4b0 ((uintptr_t)ep147_addr[EPI_249a4b0])
#define EP_249d6b0 ((uintptr_t)ep147_addr[EPI_249d6b0])
#define EP_249daf0 ((uintptr_t)ep147_addr[EPI_249daf0])
#define EP_249efe0 ((uintptr_t)ep147_addr[EPI_249efe0])
#define EP_249f200 ((uintptr_t)ep147_addr[EPI_249f200])
#define EP_24a0890 ((uintptr_t)ep147_addr[EPI_24a0890])
#define EP_24f5bc0 ((uintptr_t)ep147_addr[EPI_24f5bc0])
#define EP_24f6b34 ((uintptr_t)ep147_addr[EPI_24f6b34])
#define EP_24fa6f0 ((uintptr_t)ep147_addr[EPI_24fa6f0])
#define EP_24fac14 ((uintptr_t)ep147_addr[EPI_24fac14])
#define EP_24fac4c ((uintptr_t)ep147_addr[EPI_24fac4c])
#define EP_24fb350 ((uintptr_t)ep147_addr[EPI_24fb350])
#define EP_25c0aa0 ((uintptr_t)ep147_addr[EPI_25c0aa0])
#define EP_25e0550 ((uintptr_t)ep147_addr[EPI_25e0550])
#define EP_25e9ba0 ((uintptr_t)ep147_addr[EPI_25e9ba0])
#define EP_26a3f60 ((uintptr_t)ep147_addr[EPI_26a3f60])
#define EP_26a4838 ((uintptr_t)ep147_addr[EPI_26a4838])
#define EP_26cf590 ((uintptr_t)ep147_addr[EPI_26cf590])
#define EP_26e1b68 ((uintptr_t)ep147_addr[EPI_26e1b68])
#define EP_27d18b8 ((uintptr_t)ep147_addr[EPI_27d18b8])
#define EP_2847138 ((uintptr_t)ep147_addr[EPI_2847138])
#define EP_284ab30 ((uintptr_t)ep147_addr[EPI_284ab30])
#define EP_284b090 ((uintptr_t)ep147_addr[EPI_284b090])
#define EP_284b2b8 ((uintptr_t)ep147_addr[EPI_284b2b8])
#define EP_2854578 ((uintptr_t)ep147_addr[EPI_2854578])
#define EP_2857330 ((uintptr_t)ep147_addr[EPI_2857330])
#define EP_285f078 ((uintptr_t)ep147_addr[EPI_285f078])
#define EP_28729f8 ((uintptr_t)ep147_addr[EPI_28729f8])
#define EP_2874158 ((uintptr_t)ep147_addr[EPI_2874158])
#define EP_2896b90 ((uintptr_t)ep147_addr[EPI_2896b90])
#define EP_29672e0 ((uintptr_t)ep147_addr[EPI_29672e0])
#define EP_2967378 ((uintptr_t)ep147_addr[EPI_2967378])
#define EP_29674c8 ((uintptr_t)ep147_addr[EPI_29674c8])
#define EP_2967678 ((uintptr_t)ep147_addr[EPI_2967678])
#define EP_29676a8 ((uintptr_t)ep147_addr[EPI_29676a8])
#define EP_29676d0 ((uintptr_t)ep147_addr[EPI_29676d0])
#define EP_29676f0 ((uintptr_t)ep147_addr[EPI_29676f0])
#define EP_2968548 ((uintptr_t)ep147_addr[EPI_2968548])
#define EP_29c2b00 ((uintptr_t)ep147_addr[EPI_29c2b00])
#define EP_29c3b38 ((uintptr_t)ep147_addr[EPI_29c3b38])
#define EP_29c5028 ((uintptr_t)ep147_addr[EPI_29c5028])
#define EP_29c98c0 ((uintptr_t)ep147_addr[EPI_29c98c0])
#define EP_29d4830 ((uintptr_t)ep147_addr[EPI_29d4830])
#define EP_29d67a0 ((uintptr_t)ep147_addr[EPI_29d67a0])
#define EP_29da0e8 ((uintptr_t)ep147_addr[EPI_29da0e8])
#define EP_29daee8 ((uintptr_t)ep147_addr[EPI_29daee8])
#define EP_29db4c8 ((uintptr_t)ep147_addr[EPI_29db4c8])
#define EP_29dbec8 ((uintptr_t)ep147_addr[EPI_29dbec8])
#define EP_29e0f18 ((uintptr_t)ep147_addr[EPI_29e0f18])
#define EP_29e2858 ((uintptr_t)ep147_addr[EPI_29e2858])
#define EP_29e9540 ((uintptr_t)ep147_addr[EPI_29e9540])
#define EP_29e9790 ((uintptr_t)ep147_addr[EPI_29e9790])
#define EP_2a19970 ((uintptr_t)ep147_addr[EPI_2a19970])
#define EP_2a92ae0 ((uintptr_t)ep147_addr[EPI_2a92ae0])
#define EP_2aa6448 ((uintptr_t)ep147_addr[EPI_2aa6448])
#define EP_2abc588 ((uintptr_t)ep147_addr[EPI_2abc588])
#define EP_2b37908 ((uintptr_t)ep147_addr[EPI_2b37908])
#define EP_2b44be8 ((uintptr_t)ep147_addr[EPI_2b44be8])
#define EP_2b44c50 ((uintptr_t)ep147_addr[EPI_2b44c50])
#define EP_2b63770 ((uintptr_t)ep147_addr[EPI_2b63770])
#define EP_2b668b8 ((uintptr_t)ep147_addr[EPI_2b668b8])
#define EP_2b66d98 ((uintptr_t)ep147_addr[EPI_2b66d98])
#define EP_2b67408 ((uintptr_t)ep147_addr[EPI_2b67408])
#define EP_2b67a78 ((uintptr_t)ep147_addr[EPI_2b67a78])
#define EP_2b6f208 ((uintptr_t)ep147_addr[EPI_2b6f208])
#define EP_2b6f460 ((uintptr_t)ep147_addr[EPI_2b6f460])
#define EP_2b72f00 ((uintptr_t)ep147_addr[EPI_2b72f00])
#define EP_2b72f50 ((uintptr_t)ep147_addr[EPI_2b72f50])
#define EP_2b72fa0 ((uintptr_t)ep147_addr[EPI_2b72fa0])
#define EP_2b74558 ((uintptr_t)ep147_addr[EPI_2b74558])
#define EP_2b747e0 ((uintptr_t)ep147_addr[EPI_2b747e0])
#define EP_2b79058 ((uintptr_t)ep147_addr[EPI_2b79058])
#define EP_2b79158 ((uintptr_t)ep147_addr[EPI_2b79158])
#define EP_2b799c0 ((uintptr_t)ep147_addr[EPI_2b799c0])
#define EP_2b7d8d0 ((uintptr_t)ep147_addr[EPI_2b7d8d0])
#define EP_2b7da80 ((uintptr_t)ep147_addr[EPI_2b7da80])
#define EP_2b7f408 ((uintptr_t)ep147_addr[EPI_2b7f408])
#define EP_2b7f588 ((uintptr_t)ep147_addr[EPI_2b7f588])
#define EP_2b7f6d8 ((uintptr_t)ep147_addr[EPI_2b7f6d8])
#define EP_2b88870 ((uintptr_t)ep147_addr[EPI_2b88870])
#define EP_2b88a48 ((uintptr_t)ep147_addr[EPI_2b88a48])
#define EP_2b9eb88 ((uintptr_t)ep147_addr[EPI_2b9eb88])
#define EP_2bcc810 ((uintptr_t)ep147_addr[EPI_2bcc810])
#define EP_2bcdec0 ((uintptr_t)ep147_addr[EPI_2bcdec0])
#define EP_2c29228 ((uintptr_t)ep147_addr[EPI_2c29228])
#define EP_2c51010 ((uintptr_t)ep147_addr[EPI_2c51010])
#define EP_2c510a0 ((uintptr_t)ep147_addr[EPI_2c510a0])
#define EP_2c52ff0 ((uintptr_t)ep147_addr[EPI_2c52ff0])
#define EP_2c5ad08 ((uintptr_t)ep147_addr[EPI_2c5ad08])
#define EP_2c5bb40 ((uintptr_t)ep147_addr[EPI_2c5bb40])
#define EP_2c667f8 ((uintptr_t)ep147_addr[EPI_2c667f8])
#define EP_2cd4b28 ((uintptr_t)ep147_addr[EPI_2cd4b28])
#define EP_2ce30d0 ((uintptr_t)ep147_addr[EPI_2ce30d0])
#define EP_2ce7c50 ((uintptr_t)ep147_addr[EPI_2ce7c50])
#define EP_2cedcf8 ((uintptr_t)ep147_addr[EPI_2cedcf8])
#define EP_2cee580 ((uintptr_t)ep147_addr[EPI_2cee580])
#define EP_2cfe0f0 ((uintptr_t)ep147_addr[EPI_2cfe0f0])
#define EP_2cfe5c0 ((uintptr_t)ep147_addr[EPI_2cfe5c0])
#define EP_2d0b700 ((uintptr_t)ep147_addr[EPI_2d0b700])
#define EP_2d1b678 ((uintptr_t)ep147_addr[EPI_2d1b678])
#define EP_2d1dbe8 ((uintptr_t)ep147_addr[EPI_2d1dbe8])
#define EP_2d21548 ((uintptr_t)ep147_addr[EPI_2d21548])
#define EP_2d37158 ((uintptr_t)ep147_addr[EPI_2d37158])
#define EP_2d37408 ((uintptr_t)ep147_addr[EPI_2d37408])
#define EP_2d387d0 ((uintptr_t)ep147_addr[EPI_2d387d0])
#define EP_2d38a80 ((uintptr_t)ep147_addr[EPI_2d38a80])
#define EP_2d3ca00 ((uintptr_t)ep147_addr[EPI_2d3ca00])
#define EP_2d3cc28 ((uintptr_t)ep147_addr[EPI_2d3cc28])
#define EP_2d3d9b8 ((uintptr_t)ep147_addr[EPI_2d3d9b8])
#define EP_2d3dbe0 ((uintptr_t)ep147_addr[EPI_2d3dbe0])
#define EP_2d6c498 ((uintptr_t)ep147_addr[EPI_2d6c498])
#define EP_2d6cac0 ((uintptr_t)ep147_addr[EPI_2d6cac0])
#define EP_2d72b48 ((uintptr_t)ep147_addr[EPI_2d72b48])
#define EP_2d72cb0 ((uintptr_t)ep147_addr[EPI_2d72cb0])
#define EP_2d74bb0 ((uintptr_t)ep147_addr[EPI_2d74bb0])
#define EP_2d7a800 ((uintptr_t)ep147_addr[EPI_2d7a800])
#define EP_2d824e0 ((uintptr_t)ep147_addr[EPI_2d824e0])
#define EP_2d84b60 ((uintptr_t)ep147_addr[EPI_2d84b60])
#define EP_2d8d300 ((uintptr_t)ep147_addr[EPI_2d8d300])
#define EP_2d8d4d8 ((uintptr_t)ep147_addr[EPI_2d8d4d8])
#define EP_2d8d7a0 ((uintptr_t)ep147_addr[EPI_2d8d7a0])
#define EP_2d8da20 ((uintptr_t)ep147_addr[EPI_2d8da20])
#define EP_2d8f570 ((uintptr_t)ep147_addr[EPI_2d8f570])
#define EP_2d8f880 ((uintptr_t)ep147_addr[EPI_2d8f880])
#define EP_2d95250 ((uintptr_t)ep147_addr[EPI_2d95250])
#define EP_2d994e8 ((uintptr_t)ep147_addr[EPI_2d994e8])
#define EP_2da1c28 ((uintptr_t)ep147_addr[EPI_2da1c28])
#define EP_2dae640 ((uintptr_t)ep147_addr[EPI_2dae640])
#define EP_2dc1820 ((uintptr_t)ep147_addr[EPI_2dc1820])
#define EP_2dc7888 ((uintptr_t)ep147_addr[EPI_2dc7888])
#define EP_2dcc3a0 ((uintptr_t)ep147_addr[EPI_2dcc3a0])
#define EP_2dcc760 ((uintptr_t)ep147_addr[EPI_2dcc760])
#define EP_2dccc70 ((uintptr_t)ep147_addr[EPI_2dccc70])
#define EP_2dede68 ((uintptr_t)ep147_addr[EPI_2dede68])
#define EP_2defe68 ((uintptr_t)ep147_addr[EPI_2defe68])
#define EP_2df55e8 ((uintptr_t)ep147_addr[EPI_2df55e8])
#define EP_2df7ff8 ((uintptr_t)ep147_addr[EPI_2df7ff8])
#define EP_2dfbde0 ((uintptr_t)ep147_addr[EPI_2dfbde0])
#define EP_2dfbe08 ((uintptr_t)ep147_addr[EPI_2dfbe08])
#define EP_2e15028 ((uintptr_t)ep147_addr[EPI_2e15028])
#define EP_2e15058 ((uintptr_t)ep147_addr[EPI_2e15058])
#define EP_2e41538 ((uintptr_t)ep147_addr[EPI_2e41538])
#define EP_2e509c8 ((uintptr_t)ep147_addr[EPI_2e509c8])
#define EP_2e5fe90 ((uintptr_t)ep147_addr[EPI_2e5fe90])
#define EP_2e600a0 ((uintptr_t)ep147_addr[EPI_2e600a0])
#define EP_2e605b8 ((uintptr_t)ep147_addr[EPI_2e605b8])
#define EP_2e67bb0 ((uintptr_t)ep147_addr[EPI_2e67bb0])
#define EP_3141a38 ((uintptr_t)ep147_addr[EPI_3141a38])
#define EP_3142bc0 ((uintptr_t)ep147_addr[EPI_3142bc0])
#define EP_3142d48 ((uintptr_t)ep147_addr[EPI_3142d48])
#define EP_314cae0 ((uintptr_t)ep147_addr[EPI_314cae0])
#define EP_31592e0 ((uintptr_t)ep147_addr[EPI_31592e0])
#define EP_315e560 ((uintptr_t)ep147_addr[EPI_315e560])
#define EP_3480128 ((uintptr_t)ep147_addr[EPI_3480128])
#define EP_3b25a60 ((uintptr_t)ep147_addr[EPI_3b25a60])
#define EP_3b32488 ((uintptr_t)ep147_addr[EPI_3b32488])
#define EP_3b35728 ((uintptr_t)ep147_addr[EPI_3b35728])
#define EP_3b35f40 ((uintptr_t)ep147_addr[EPI_3b35f40])
#define EP_3b35fb8 ((uintptr_t)ep147_addr[EPI_3b35fb8])
#define EP_3b8dcb8 ((uintptr_t)ep147_addr[EPI_3b8dcb8])
#define EP_3b8e9f8 ((uintptr_t)ep147_addr[EPI_3b8e9f8])
#endif
