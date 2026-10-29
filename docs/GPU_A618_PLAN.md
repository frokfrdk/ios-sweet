# Adreno 618 GPU acceleration plan (sunfish / Pixel 4a)

Status, 2026-10-25: **Stages 0 and 1 passed on the phone (kernel 0096, image G1b). Stage 2A ran on G2, G2b
and G2m (patches 0097–0099): the rail votes, PLL0 and the GX GDSC work, and the GMU sees GX powered. The
GCC counter shows the GPU core clock (gx_gfx3d) does not run at all, so no GX register has been read.
Per the 2A decision rule, the next path is 2B (the GMU powers GX). 2B is planned in detail in "Stage 2B"
below and waits for the owner's go-ahead; nothing of it is built.** Nothing is flashed: every test is a
`fastboot boot` image. The firmware is extracted. The hardware facts
come from the stock DTB and the downstream sunfish kernel. ios-sailfish's GPU work has been read. Every
stage below ends in a test you can see on the phone, and nothing advances until that test passes.

## 1. Goal and approach

Today CoreAnimation renders on the CPU (`CA::OGL::SWContext`). The H4 boot copies each 828x1792 frame
(5.9 MB) into a canvas buffer, and the MDP's QSEED3-Lite scaler upscales it to the panel. The goal is
the approach [ios-sailfish](https://github.com/joranv1/ios-sailfish) proved on the Pixel 1 (Adreno 530):

- **No Metal.** CoreAnimation's GLES renderer (`CA::OGL::GLESContext`) runs on **Mesa freedreno**.
- Mesa is built for iOS as `sweetgl.dylib` and injected into `backboardd` with `DYLD_INSERT_LIBRARIES`.
- An **EAGL shim** replaces the few `EAGLContext` methods CA uses, so its GL calls reach Mesa.
- A small **libdrm/msm emulation** turns Mesa's DRM ioctls into calls to an XNU user client
  (`SweetGPU`). That client offers buffer objects, IOSurface import, command submission and fences.
- The **kernel** powers the GPU, programs its SMMU, loads the CP microcode, has TrustZone load the zap
  shader, and feeds Mesa's command streams to the ring.

Mesa's freedreno already supports a618 (SC7180 Chromebooks, mainline Linux). The new work is therefore
almost all **kernel bring-up**. The a6xx generation is harder than ios-sailfish's a5xx for one reason:
the **GMU** (a Cortex-M3 power controller) and RPMh now sit between the CPU and the GPU's power rail.

## 2. What ios-sailfish did (studied 2026-10-25)

Kernel (`bsd/dev/sweet_gpu.c`, `iokit/Kernel/arm/SweetGPU.cpp`, `SweetSurface.cpp`). Patch numbers are theirs.

| Step | Their patches | What it does |
|---|---|---|
| Staged probe | 0119 | Boot-arg `sweetgpu=N`. Runs 40 s after boot, pausing 11 s between steps so the persistent log keeps the last good step. Order: GFX rail, CX GDSC + AHB, GX GDSC (IO clamp), clocks, then reads of VBIF_VERSION, RBBM_STATUS and ALWAYSON. |
| CP test | 0120 | CP microcode comes in as boot-payload blobs (`/chosen/memory-map` `GPU-PM4`/`GPU-PFP`). Then a 16 KB ring, `CP_ME_INIT`, and `CP_MEM_WRITE 0xc0ffee01` to a scratch page. |
| SMMU | 0121, 0123, 0155, 0156, 0161 | The firmware ignores sCR0.CLIENTPD, so the first CP fetch reset the phone. Fix: an identity map in CB0 (stage 1, 4K granule, 1 GB blocks over DRAM), SMR0 matching every stream, S2CR0 → CB0, then a readback check. Later patches make faults non-fatal, pad VMs and retry stage-2 reads. |
| Zap shader | 0130 | The GPU starts in secure mode: the ring writes nothing to normal memory, and `CP_SET_SECURE_MODE 0` hangs without a zap. The kernel loads the zap through SCM/SMCCC PAS (`INIT_IMAGE`, `MEM_SETUP`, segment copy, `AUTH_AND_RESET`) with PAS id 13. |
| Bring-up tests | 0126–0136, 0141–0149 | 2D blit, compute, blend, textured quad, benchmarks, clock and DDR votes. |
| GPU VA | 0137 | GPU VA 8–16 GB is mapped with 4 KB pages (L1[8..15] → L2 → L3) for scattered memory. The identity map below 8 GB stays for kernel buffers. |
| User client | 0138, 0150, 0151 | Selectors 0–3: map user memory, map IOSurface, unmap, quad. **10 bo new, 11 bo free, 12 bo import (IOSurface), 13 submit (IBs + bo list → seqno), 14 wait, 15 info.** A read-only fence page, hang detection (fence stuck 2 s → service off) and guard pages between mappings. One shared address space; their own notes call that acceptable only on a single-user bring-up device. |
| IOSurface | 0152–0162 | Row alignment and client-mapping fixes so Mesa can import CA's surfaces. |
| Present | 0164 | The GPU draws the swap surface into one of two 32 bpp panel buffers, and the MDP flips between them (`sweet_fbcon_flip`). Also 600 MHz. |
| Plumbing | 0104, 0163, 0139, 0166 | `sweetenv=proc:VAR=VAL` boot-arg (adds env vars at exec). An AMFI policy-syscall answer to dyld's op 0x5a (otherwise dyld ignores `DYLD_INSERT_LIBRARIES`). An `IOAcceleratorES` stub without `MetalPluginName`, which stops CA polling for it for 30 s while still choosing GLES. |

User space (`tools/sweetgl/`):

- **`mesa-setup.sh`**: meson for iOS arm64 (`ios-cross.txt`), gallium freedreno, GLES2/3, `-Dfreedreno-kmds=msm`,
  no LLVM, no window system, static libraries.
- **`mesa.patch`** (3 hunks):
  - `os_mmap` → `sweet_drm_bo_map`.
  - Build `src/freedreno/drm` on darwin.
  - An **a5xx-only** pitch-alignment fix for imported linear surfaces.
- **`sweet_drm.c`**: libdrm plus the msm ioctls (GET_PARAM, GEM_NEW/INFO/CPU_PREP/SUBMIT, WAIT_FENCE,
  SUBMITQUEUE_*) over the SweetGPU selectors. The fd is `/dev/null`.
- **`sweetgl.c`**: a minimal gallium frontend (modelled on Haiku's hgl), surfaceless, CA draws only into FBOs.
  - It retries for 2 minutes while the GPU service comes up.
  - It forces `nogmem`: rendering goes straight to memory, because GMEM tile resolves overran unpadded
    IOSurfaces.
  - It forces `nofp16`.
- **`eagl.c`** (77 KB), the EAGL shim:
  - It swizzles `initWithAPI:properties:`, `API`, `setParameter:to:`, `getMacroContextPrivate` (GLI dispatch
    `{rctx, disp[1036]}`, generated by `gen-dispatch.py`), `texImageIOSurface:…`,
    `attachImage:toCoreSurface:invertedRender:`, `+setCurrentContext:` and `+currentContext`.
  - IOSurfaces become textures through `glEGLImageTargetTexture2DOES`.
  - It emulates `GL_EXT_shader_framebuffer_fetch`. Without it, CA drops every blend-filter layer: CC toggles,
    App Library plates, Reduce Transparency backgrounds.
  - It also emulates `GL_TEXTURE_RECTANGLE` and copies surfaces whose rows the GPU cannot sample.
- **`build.sh`**: links everything into one ad-hoc-signed dylib, zero-padded to 64 MiB so the file on the phone
  keeps its size and can be rewritten in place.
- It is built with Xcode 15 on macOS (`xcrun`). Boot-args: `sweetgpu=8 sweetgpuclk=600 sweetddr=1804
  sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`.

## 3. What is different on the Pixel 4a

| | Pixel 1 (sailfish) | Pixel 4a (sunfish) |
|---|---|---|
| GPU | Adreno 530, chip 0x05030002 | **Adreno 618, chip 0x06010800**, GMEM 512 KiB |
| CP microcode | PM4 + PFP | **SQE** `a630_sqe.fw` (32136 B) |
| Power | Host votes GFX rail (PMI8994 SPMI), GDSCs, clocks | **GMU** (Cortex-M3, `a618_gmu.bin` 32 KiB legacy raw image) votes `gfx.lvl` through its own RSC/PDC to **RPMh**; the GX GDSC and GX clock are normally the GMU's job |
| Rails | SPMI regulator writes | RPMh ARC `gfx.lvl` (pm6150 s2), `cx.lvl` (pm6150l s2); apps RSC 0x18220000 (drv 2); cmd-db at 0xc3f000c |
| Zap | PAS 13 | PAS 13, `a615_zap.mdt/.b00-.b02`, carve-out **0x97715000** (0x2000) |
| SMMU | smmu-v2, no hypervisor | smmu-v2 at **0x5040000** behind Qualcomm's **hypervisor** (`qcom,hyp_secure_alloc`); SIDs: gfx3d_user 0, gfx3d_secure 2, GMU 4/5 |
| Quirks (DT) | – | `hfi-use-reg`, `secvid-set-once`, `no-nap`, highest-bank-bit 14, UBWC mode 2 |
| Present | CPU copy then GPU quad + MDP flip | H4 already scales on the MDP (VIG0 QSEED3-Lite); only the 5.9 MB copy per frame is left |
| Kernel already has | – | RPMh client (`sweet_rpmh.c`, cmd-db parser, VRM-only), persistent console log (0047), SweetSurface/IOMFB (0046), AMFI policy syscall (0092, answers op 0x60 only) |

## 4. Facts gathered

**Firmware** (from `vendor.img`, in `~/ios-pixel/gpu-fw/fw/`, **never committed**):

| File | Size / layout |
|---|---|
| `a630_sqe.fw` | 32136 B; dword 0 = 0, then the version |
| `a618_gmu.bin` | 32768 B, raw Cortex-M3 image: 16 KiB code for ITCM (vector table first), then 16 KiB of DTCM data (corrected in Stage 2B) |
| `a615_zap.mdt` (+ `.b00/.b01/.b02`) | 3 program headers: NULL, NULL (hash, vaddr 0x6000), LOAD (paddr 0x5000, filesz 0x7c0) |

**DTB** (`~/ios-pixel/factory/stock/sunfish-base.dts`):

- **kgsl-3d0** at 0x5000000 (0x40000):
  - Other regions: cx_dbgc 0x5061000, cx_misc 0x509e000, qfprom 0x780000. IRQ SPI 300.
  - Speed bin: qfprom +0x41a0, mask 0x1fe00000, shift 21.
  - Bin-0 frequencies: 825/800/650/565/430/355/267/180 MHz. Initial level 7 = 180 MHz.
- **GMU** at 0x506a000 (0x31000):
  - PDC cfg 0xb290000, PDC seq 0xb490000. IRQs: HFI SPI 0x130, GMU SPI 0x131.
  - GMU clock 200 MHz.
  - Register offsets in the Linux XML (and `xnu/gpu/regs/`) count in dwords from the GPU base 0x5000000,
    as kgsl does: GMU_CM3_ITCM_START 0x1b400 = 0x506d000. `RSCC_*` count from the RSCC block
    (0x506a000 + 0x23000), the `A6XX_PDC*` domains from their PDC blocks.
- **gpucc** at 0x5090000 (0x9000):
  - GDSCs: CX 0x509106c, GX 0x509100c.
  - From downstream `gpucc-sdmmagpie.c`:
    - PLL0 (Fabia) at +0x0: L 0x12, alpha 0xC000.
    - PLL1 at +0x100.
    - GMU RCG at +0x1120: 200 MHz = GPLL0 div / 1.5.
    - GX gfx3d RCG at +0x101c: 180–825 MHz from the CRC divider.
    - Branches: ahb +0x1078, cx_gmu +0x1098, cxo +0x109c, cx_snoc_dvm +0x108c, cx_apb +0x1088,
      cx_gfx3d +0x10a4, gx_gfx3d +0x1054, gx_cxo +0x1060, gx_gmu +0x1064.
- **gcc** (`gcc-sdmmagpie.c`): gpu_cfg_ahb 0x71004, gpu_memnoc_gfx 0x7100c, gpu_snoc_dvm_gfx 0x71018,
  ddrss_gpu_axi 0x4452c, gpu_gpll0 / gpll0_div = enable bits 15 / 16 of 0x52004.

**Read on the phone** (Stage 0, boot G0, 2026-10-25):

- Speed bin: QFPROM 0x7841a0 reads 0x12468410 from EL1 without a fault, so **bin 146**. Its DTS table is
  700/650/565/430/355/267/180 MHz (levels 0–6, bus votes 11/10/9/8/7/6/4), initial level 6 = 180 MHz,
  ca-target level 4. Bin 0's 825 MHz table does not apply to this phone.
- cmd-db ARC level tables (hlvl index → RPMh vlvl):

  | Resource | addr | vlvl per hlvl |
  |---|---|---|
  | `gfx.lvl` | 0x30050 | 0 16 64 128 192 256 320 384 416 |
  | `cx.lvl` | 0x30000 | 0 16 64 128 192 256 384 |
  | `mx.lvl` | 0x30010 | 0 16 192 320 384 |
  | `lcx.lvl` / `lmx.lvl` | 0x30030 / 0x30040 | 0 16 64 128 256 384 / 0 16 128 256 384 |
  | `ddr.lvl` / `xo.lvl` | 0x30070 / 0x30090 | 0 32 128 160 / 0 32 80 128 |

  `ebi.lvl` and `mmcx.lvl` exist but have no level table. A second ARC header lists 10 `*.mol` entries
  (addr 0, one level each; `gfx.mol` 64, `mx.mol` 320).
- cmd-db BCMs (40): `MC0` 0x50000 (unit 1770000, width 16, vcd 0), `SH0` 0x5003c (unit 2090000, width 32,
  vcd 1), `ACV` 0x5000c (vcd 11). VRMs (64): `vrm.gfx` = smpa2 0x40600, `vrm.cx` = smpc2 0x42700,
  `vrm.mx` = smpa3 0x40000, which matches the rails in section 3.
- The four firmware blobs reach the kernel through `/chosen/memory-map` at the addresses and FNV-1a
  hashes mkpayload printed.

**Memory:**

- XNU's RAM is 0xb0000000+0x4d000000, or 0x100000000+0xfcc00000 in high-memory mode.
- An identity map of 0x80000000–0x1_ffff_ffff covers it, plus the splash carve-out (0x9c000000, 23 MB)
  and the zap carve-out.

## 5. Licence (decide before Stage 7)

ios-sailfish has **no LICENSE file**, so by default nothing in it may be copied. Upstream ios-sweet is
MIT, Mesa is MIT, and the freedreno register XML in Mesa is MIT. Mainline Linux is GPL-2.0: use it as a
reference for hardware sequences, and write our own code. The options:

1. **Ask the author (joranv1) to add a licence**, ideally MIT like upstream. This matters most for
   `eagl.c` / `sweet_drm.c` / `gen-dispatch.py`, the parts that are hardest to redo. The owner would make that
   contact; I will not post anything on the owner's behalf.
2. **Clean-room.** Use their write-up only as a map of *what* is needed, and write our own kernel and
   user-space code. The kernel side has to be rewritten for a6xx anyway. The EAGL shim holds a lot of
   reverse-engineered CoreAnimation behaviour that would have to be found again.

Stages 0–6 need nothing from their code, so the licence question blocks only Stage 7.

**Decided 2026-10-28 (owner): option 2, everything is our own code.** Rules and allowed sources are
in `docs/STAGE7_LICENCE.md`.

## 6. Stages

Each stage is a kernel patch in `xnu/patches/` built by the GitHub Actions workflow, booted with
`fastboot boot` only, so nothing persists. Boot-arg `sweetgpu=N` picks how far to go. N=0 or no boot-arg means
the GPU is untouched, so the everyday H4 behaviour stays the default.

### Stage 0: groundwork (no GPU register is touched)

1. **Firmware delivery.**
   - Add `--blob NAME=path` to `tools/mkpayload.py`. The blobs go after the ramdisk, and each gets a
     `/chosen/memory-map` property (`GPU-SQE`, `GPU-GMU`, `GPU-ZAP-MDT`, `GPU-ZAP-B02`…, 16 bytes = pa, len),
     as in their 0120.
   - `tools/sunfish/mkboot.sh` reads them from `~/ios-pixel/gpu-fw/fw/` with `GPUFW=1`. Boot images with blobs
     stay out of git.
2. **Probe framework.**
   - Add a `bsd/dev/sweet_gpu.c` skeleton: a thread starts about 40 s after boot when `sweetgpu=N`.
   - Each step logs, then sleeps 2 s before any access that could hang, so the 0047 log holds the last
     good step. It flushes every `ufslogsecs` seconds (default 10), so GPU test boots pass `ufslogsecs=1`.
   - Check that `tools/sweet-log-pull.sh` recovers the log after a deliberate hang.
3. **Register header.** Generate `a6xx` / `a6xx_gmu` / `adreno_pm4` offsets from Mesa's
   `src/freedreno/registers/adreno/*.xml` (MIT) with a small script. No hand-typed offsets.
4. **cmd-db dump** (read-only):
   - Extend `sweet_rpmh.c` to list ARC (slave 3) and BCM (slave 5) entries with their aux data:
     `gfx.lvl`, `cx.lvl`, `mx.lvl` level tables, and the `MC0` / `SH0` / `ACV` BCMs.
   - PDC programming (Stage 2B) and any direct votes (Stage 2A) take their addresses from this, not from
     constants.
5. **Speed bin:** read qfprom +0x41a0 and log the bin.

✅ Pass when a boot with `sweetgpu=1` logs the blob addresses, the cmd-db table and the speed bin, then
reaches the home screen as H4 does.

**Result (2026-10-25, commit dab0e56, kernel 0094, image G0):** passed. All four blobs checked out, the
cmd-db and speed-bin results are in section 4, and the home screen came up as with H4.

**Hang test (image G0H = G0 + `sweetgpuhang=1`):** passed for log recovery. The log was pulled after a
forced power-off, as `tools/sweet-log-pull.sh` does it, and held the `next: deliberate hang` line. One
core spinning with interrupts off did **not** freeze iOS: the phone kept responding for over 2 minutes, and
3686 more lines (12 of them CPUFREQ status) reached the log after the hang line. With `wdt=-1` nothing
notices a stuck core. For later stages this means:

- A stuck register access may leave the phone running. A `next:` line with no result line after it marks
  the hang, so every risky step must log its result.
- A stall of the whole SoC was not exercised. The 2 s pause plus `ufslogsecs=1` (0047 ends each flush with
  a UFS cache sync) puts the `next:` line on disk before the access, so it should still be there.

### Stage 1: CX side and the GMU block (reads only)

1. GCC: enable `gpu_cfg_ahb`, `ddrss_gpu_axi`, `gpu_memnoc_gfx`, `gpu_snoc_dvm_gfx`, and the
   `gpu_gpll0` / `gpll0_div` branches.
2. gpucc:
   - Turn the CX GDSC on (0x509106c, poll PWR_ON).
   - Set the GMU RCG to 200 MHz.
   - Enable cx_gmu, cxo, ahb, cx_snoc_dvm and cx_apb.
3. Read CX-domain registers only: GMU `CM3_FW_INIT_RESULT`, GMU always-on counter, `GPU_CC` IDs,
   `cx_misc`. **Reading a GX register now would hang the bus**, so the probe must not touch any.

✅ Pass when the log shows sane GMU/CX values and the phone keeps running.

**Built (patches 0095 + 0096, `sweetgpu=2`).** It runs after stage 0, in this order. Each step has its own
`next:` pause and logs before/after values:

1. If `gcc_gpu_cfg_ahb` (critical in Linux) is off, turn it on. gpucc is unreachable without it.
2. CX GDSC, following gdsc-regulator.c:
   - Clear HW_CONTROL and SW_OVERRIDE.
   - Set CLK_DIS_WAIT to 8.
   - Clear SW_COLLAPSE.
   - Poll PWR_ON at the hw-ctrl address 0x5091540 for up to 1 ms.

   It comes first, as in upstream `a6xx_gmu_resume()` (power domain before clocks). It is the GMU's
   `vddcx-supply` in the stock DT.
3. GCC: set the `gpu_gpll0` and `gpu_gpll0_div` votes (0x52004 bits 15 and 16). Then turn on
   `ddrss_gpu_axi` (0x4452c), `gpu_memnoc_gfx` (0x7100c) and `gpu_snoc_dvm_gfx` (0x71018) and wait for
   CLK_OFF to clear.
4. GMU RCG (0x5091120): CFG = source 6 (GPLL0 div, 300 MHz), divider 1.5 → 200 MHz. Then UPDATE.
5. Set wake/sleep 0xf on cx_gmu, as gpucc probe does. Then turn on ahb, cxo, cx_gmu, cx_snoc_dvm and
   cx_apb.
6. Reads:
   - GMU CM3/HFI/SPTPRAC/RPMh-state/AO/fence registers.
   - The always-on counter twice, 1 ms apart (expect ~19200 ticks).
   - The first 4 ITCM words.
   - cx_misc SYSTEM_CACHE_CNTL_0/1.

GMU offsets come from `sweet_gpu_regs.h`, a filtered copy made by
`gen-adreno-regs.py --only … --single sweet_gpu_regs.h`. They count from 0x5000000, as upstream
`gmu_read()` does; the probe subtracts 0x6a000 for the GMU mapping. GCC and gpucc offsets come from
gcc-/gpucc-sdmmagpie.c.

GX is never written. Its GDSCR, clamp register and PLLs are only read, through gpucc. Everything stays on
afterwards, because Stage 2 builds on it.

**G1 result (0095, GCC before the GDSC), 2026-10-25 05:10.** The phone booted and kept working. The stage
stopped cleanly at the GCC step.

Before:

| Register | Value | Meaning |
|---|---|---|
| `gpu_cfg_ahb` | 0x00000001 | on |
| `ddrss_gpu_axi` | 0x00000000 | off |
| `memnoc_gfx` | 0x80000220 | off |
| `snoc_dvm_gfx` | 0x80000001 | enabled but off |
| GCC vote | 0x01241d7b | |
| `gpu_misc` | 0x00010000 | |
| `cx_gdscr` | 0x00222001 | collapsed |
| `hw_ctrl` | 0x00005ff4 | PWR_ON clear |
| `gx_gdscr` | 0x00222001 | |
| `gx_domain` | 0x1 | clamped |
| GMU RCG | 0x80000000 / 0 | XO, root off |
| `cx_gmu` | 0x80004220 | |
| `ahb`, `cxo` | | on |
| `cx_snoc_dvm`, `cx_apb` | | off |
| PLL0/PLL1 mode and L | 0 | |

Steps:
- The votes took: 0x01241d7b → 0x01259d7b.
- `ddrss_gpu_axi` came on.
- `memnoc_gfx` (0x80000221) and `snoc_dvm_gfx` stayed at CLK_OFF, because they feed the collapsed CX domain.

Patch 0096 turns the CX GDSC on first.

**G1b result (0096): ✅ Stage 1 PASSED, 2026-10-25 05:29.** The owner saw the home screen, and the phone stayed
stable. The log has "stage 1 done", then 16,554 more lines (3,982 of them CPU-frequency and touch heartbeats),
with no panic or fault. Every step logged its result.

- **CX GDSC:** gdscr 0x00222001 → 0xf8228000, hw_ctrl 0x00005ff4 → 0x80001fe0 (PWR_ON).
  - Powering the domain alone took gpucc `ahb` and `cxo` from 0x80000001 to on.
  - It also turned `snoc_dvm_gfx` on, since its enable bit was already set at boot.
- **GCC:** votes 0x01241d7b → 0x01259d7b. `ddrss_gpu_axi`, `memnoc_gfx` (0x00000221) and `snoc_dvm_gfx` are on.
- **GMU RCG:** CFG 0 → 0x602 (200 MHz), UPDATE cleared. CMD reads 0 (root on) once `cx_gmu` runs.
- **gpucc:** `cx_gmu` 0x80004ff0 → 0x00004ff1 on. `ahb` and `cxo` on.
- **Unexpected:** the `cx_apb` and `cx_snoc_dvm` enable bits read back 0 after the write (0x80000000 and
  0x00000000). Neither is on the GPU path, so this blocks nothing:
  - The kgsl GPU node uses `core_clk rbbmtimer_clk mem_clk mem_iface_clk gmu_clk`.
  - The GMU node uses `gmu_clk cxo_clk axi_clk memnoc_clk`.
  - gpucc clock 0x08 (`gpu_apb_clk`) is used only by CoreSight funnel@6943000 and tpdm@6940000.
  - No node uses cx_snoc_dvm.
- **GMU reads:**

  | Register | Value | Meaning |
  |---|---|---|
  | `CM3_SYSRESET` | 1 | CM3 held in reset |
  | `BOOT_CONFIG` | 0 | |
  | `FW_BUSY` | 0 | |
  | `FW_INIT_RESULT` | 0 | |
  | `HFI_CTRL_STATUS` | 0 | |
  | `HFI_VERSION_INFO` | 0 | |
  | `SPTPRAC_PWR_CLK_STATUS` | 0xd4 | SPTP GDSC off, SP clock off, GX HM GDSC off, GX HM clock off |
  | `CX_GMU_RPMH_POWER_STATE` | 0xf | no firmware has run |
  | `AO_GPU_CX_BUSY_STATUS` | 0x30000000 | bits 28–29 are not named in the XML |
  | `AO_AHB_FENCE_CTRL` | 0 | Stage 2A step 2 is already the case |
  | `AHB_FENCE_STATUS` | 0 | |

- **Always-on counter:** 153803027 at the first read, then +21696 across `delay(1000)`.
  153803027 / 19.2 MHz = 8.01 s, which is the four 2 s pauses since the CX GDSC came on. So the counter
  starts at 0 when CX powers up and counts XO at 19.2 MHz. `delay(1000)` overshoots by about 13%.
- **ITCM:** `2060e473 06510925 e66e14c9 cd804228`. This is uninitialised SRAM, not firmware; the firmware
  would start with sp 0x43f80, reset 0x9. It reads without a hang.
- **cx_misc:** `SYSTEM_CACHE_CNTL_0/1` = 0/0.
- **GX:** `gx_gdscr` 0x00222001 (collapsed), `gx_domain` 0x1 (clamped), PLL0/PLL1 0. All untouched.

### Stage 2: GX power (decision gate)

**2A: GMU-less, host-driven (try first).** Every step reuses what we have and is observable on its own.
This mirrors how their 0119 powered the a530. Mainline's "GMU wrapper" a6xx parts (a610/a619_holi) show
that the GPU core runs with the host providing GX power. The steps:

1. Vote `gfx.lvl` to the 180 MHz corner (lowest active level) through the apps RSC. This generalises
   `sweet_rpmh.c` from VRM to ARC. `mx.lvl` must stay ≥ that level; read it first.
2. Clear the GMU AHB fence (`GMU_AO_AHB_FENCE_CTRL = 0`).
3. GX GDSC on (0x509100c): un-clamp IO, poll.
4. SPTP RAC on (`GMU_GX_SPTPRAC_POWER_CONTROL`, poll `SPTPRAC_PWR_CLK_STATUS`). This is what the GMU
   firmware would do.
5. Clocks:
   - PLL0 with the downstream config.
   - CRC divider, then the gfx3d RCG to 180 MHz.
   - Enable gx_gfx3d, gx_cxo, cx_gfx3d and gx_gmu.
6. Read `RBBM_STATUS`, the GBIF/VBIF version and the RBBM always-on counter twice. The counter must
   advance.

Risks:
- RPMh may ignore an apps-DRV vote on `gfx.lvl`. Check this with the ARC status read-back.
- The PLL may not lock. Check this with the lock bit.
- Levels are abstract corners, so a wrong vote cannot over-volt the rail. The worst case is a hang and
  reboot, which `fastboot boot` makes harmless.

**Built (patch 0097, `sweetgpu=3`).** It runs after stage 1 passes. The order follows the downstream drivers
(gdsc-regulator.c, clk-alpha-pll.c, clk-rcg2.c) instead of the list above. Each step logs a "next:" line
first, and the stage stops at the first failure:

| Step | What it writes | Check |
|---|---|---|
| 1. `mx.lvl` vote | one ACTIVE TCS command, data = hlvl | TCS completion |
| 2. `gfx.lvl` vote | the same | TCS completion |
| 3. PLL0 | L 0x12, FRAC 0xc000 (360 MHz), CAL_L 0x3f, the downstream config/user/test values, even post-divider 2; out of reset, RUN, then the outputs | lock bit within 200 µs |
| 4. gfx3d RCG (0x509101c) | ROOT_EN, then CFG = crc_div / 1 (180 MHz) and UPDATE | ROOT_OFF clears; UPDATE clears |
| 5. GX GDSC (0x509100c) | BCR 0x5091008 pulse, GMEM reset pulse and unclamp in 0x5091508, clear SW_COLLAPSE | PWR_ON within 500 µs; on timeout it collapses and clamps again |
| 6. GX branches | gx_cxo, gx_gmu, gx_gfx3d, cx_gfx3d enable bits | CLK_OFF clears (gx_cxo, gx_gmu) |
| 7. SPTP | `GMU_GX_SPTPRAC_POWER_CONTROL` = 0x778000 | status & 0x38 == 0x28 |
| 8. GX reads | none | only if the GMU's GX head-switch and clock bits read on |

How it differs from the plan:
- Both votes ask for vlvl ≥ 64 (LOW_SVS), mapped to an hlvl the way rpmhpd does it. 64 is what
  gdsc-regulator.c votes on the GX GDSC's parent. 180 MHz needs MIN_SVS (48), and gfx.lvl has no level
  below 64. So gfx gets hlvl 2 (vlvl 64), and mx gets hlvl 2 (vlvl 192, its lowest active level). mx is
  voted first so that it stays ≥ gfx. `sweet_rpmh_arc_vote` accepts only these two resources and vlvl ≤ 64.
- There is no ARC status read-back from the apps side. The GX GDSC reaching PWR_ON, and the GMU's
  `GX_HM_GDSC_POWER_OFF` bit clearing, are the evidence that the rail came up.
- The fence is not written: it already reads 0 (G1b).
- PLL0 runs at 360 MHz with the even divider at 2. This is the downstream config's own L and alpha, and it
  sits inside both Fabia VCO ranges. Linux would reach 180 MHz either this way or with the PLL at 180 MHz.
- No GBIF/VBIF version read: a618 has a GBIF, and the register XML has no version register for it
  (`VBIF_VERSION` belongs to a630's VBIF). The reads are `RBBM_STATUS`, `STATUS1-3`, `RBBM_CLOCK_CNTL` and
  the CP always-on counter twice, next to the GMU counter.
- The gx_* branch registers are read only after the GX GDSC is on.
- Everything stays on afterwards. The votes are ACTIVE only; iOS never enters the system sleep that would
  drop them.

**Result on G2 (kernel 0097, booted 10:17:04).** The owner saw the home screen and a stable phone. The log
(flush #206, 6537 lines) has no panic or fault, and 4394 lines follow the stage's last line. Stage 1 matched
G1b. Observed in 2A:
- `mx.lvl` vlvl 64 → hlvl 2 (vlvl 192) and `gfx.lvl` vlvl 64 → hlvl 2 (vlvl 64). Both went through TCS0
  with cmd-status 0x10101 (issued and completed). This shows RPMh accepted the votes, not that the rail
  came up.
- PLL0 locked: mode 0 → 0xd0800005 (LOCK_DET, ACTIVE, RESET_N, OUTCTRL), opmode RUN, USER_CTL 0x107. L
  0x12, CAL_L 0x3f and FRAC 0xc000 read back.
- The gfx3d RCG did not turn on: CMD 0x80000000 → 0x80000002. ROOT_EN stuck, but ROOT_OFF stayed set for
  500 µs, with CFG still 0 (XO). The stage stopped there, so the GX GDSC (gdscr 0x00222001, domain 0x1 =
  clamped) and SPTP (0xd4) were not touched.

Downstream does not treat this as fatal. The sunfish GX GDSC node (sdmmagpie.dtsi) has
`qcom,force-enable-root-clk` on `GPU_CC_GX_GFX3D_CLK_SRC`, `parent-supply = <&VDD_GFX_LEVEL>` and
`qcom,reset-aon-logic`. gdsc_enable then:
1. sets the parent to LOW_SVS;
2. calls clk_prepare_enable on the root, which goes through clk_rcg2_set_force_enable. If the root stays
   off, that function only prints a "rcg didn't turn on" warning and returns 0;
3. runs the BCR/GMEM/unclamp/un-collapse sequence.

So downstream never waits for this root before the GDSC. The RCG is a `gx_` clock with `vdd_gx`, and GX was
still clamped. Our guess is that the root, or its status, needs the GX side unclamped. That is not verified.

**Patch 0098 (image G2b).** It follows downstream: the root is forced on, and a root that stays off is
logged and the stage goes on. Then the GX GDSC (unchanged, reverted on timeout). With GX unclamped, the
root is checked again, and the switch to 180 MHz (crc_div) happens only once the root runs. The branches
and SPTP run either way. The GX reads also need the gfx3d root at 180 MHz, on top of the GMU's GX
power/clock bits.

**Result on G2b (kernel 0098, booted 10:38:34).** The owner saw the home screen and a stable phone. The log
(5759 lines) has no panic or fault, and 3417 lines follow the stage's last line. Stage 1, both votes and
PLL0 matched G2. Observed in 2A:
- Before the GDSC the gfx3d root stayed off again (CMD 0x80000000 → 0x80000002, CFG 0 = XO). The stage went
  on, as planned.
- **The GX GDSC came on:** gdscr 0x00222001 → 0xf8222000 (PWR_ON), domain 0x1 → 0 (unclamped). So the apps
  DRV `gfx.lvl` vote does bring up the GX rail, which was the first open question of 2A.
- The GMU's SPTPRAC_PWR_CLK_STATUS went 0xd4 → 0x94. Bit 6 (GX_HM_GDSC_POWER_OFF) cleared, so the GMU sees
  the GX head switch on. Bit 7 (GX_HM_CLK_OFF) stayed set.
- With GX unclamped the root still read off (CMD 0x80000002, CFG 0). 0098 only updates CFG once the root
  runs, so it never tried 180 MHz.
- Branches: gx_cxo on, and gx_gmu 0x80000000 → 0x00000001 (on). gx_gfx3d (0x80004220 → 0x80004221) and
  cx_gfx3d (0x80000000) still show CLK_OFF. Neither has a halt check here, and downstream marks gx_gfx3d
  BRANCH_HALT_SKIP.
- SPTPRAC enable: 0x94 → 0x94, not on. The check wants 0x28 (SPTP on, SP clock on, GMU_UP). Our guess, not
  verified: GMU_UP (bit 5) cannot come up while the CM3 is held in reset, so this check may never pass on 2A.
- The stage stopped before any GX read, as designed.

The open question is whether the gfx3d RCG really does not run, or only its status says so. Possible
causes, none verified:
1. The RCG needs a CFG update after GX power-up. Downstream's set_rate always does one inside the force
   enable, whether or not the root came on.
2. XO does not reach the GX side of the RCG.
3. The GX status bits are not reliable (downstream skips the gx_gfx3d halt check).
4. The GMU firmware normally owns this clock, which would point to 2B.

**Patch 0099 (image G2m).** sweet_gpu.c only. After the GX branches it uses the GCC frequency counter
(debugcc-sdmmagpie.c, clk-debug.c): gpucc debug mux → GCC debug mux 0x144 → a counter gated by XO/4.
- **First measurement.** It measures cx_gmu, gx_gfx3d, cx_gfx3d, gx_cxo and gx_gmu. Stage 1 set cx_gmu to
  200 MHz, so cx_gmu checks the method.
- **Update.** It then sets gfx3d to crc_div divider 1 (180 MHz) with an UPDATE even if the root reads off,
  as clk_rcg2_set_rate does.
- **Second measurement.** It measures cx_gmu, gx_gfx3d and cx_gfx3d again.
- **Gate for the GX reads.** All of these must hold:
  - the update must take;
  - gx_gfx3d must run at 100–250 MHz on the counter. The counter is trusted only when cx_gmu reads
    150–250 MHz; otherwise the root status bit decides;
  - the GMU GX bits must be on, as before.
- **Safety.** The counter uses bounded polls (1 ms, then 50 ms). It only touches the GCC and gpucc debug
  registers, and it turns their enables off again.

How to read G2m:
- cx_gmu far from 200 MHz: the counter setup is wrong; fix that first.
- gx_gfx3d near 19.2 MHz before the update, or near 180 MHz after it: the RCG runs and its status bit
  lies (cause 3), or the update started it (cause 1).
- gx_gfx3d 0 before and after: the RCG really does not run (cause 2 or 4).
- If gx_gfx3d runs but GX_HM_CLK_OFF stays set, the stage still stops before the reads. That bit is the
  next thing to study.

**Result on G2m (kernel 0099, booted 11:05:50).** The owner reported a working phone. The log (flush #133,
5179 lines) has no panic or fault, and 2710 lines follow the stage's last line. Everything up to the GX
branches matched G2b. The counter measured:

| clock | after the GX branches (CFG = XO) | after the 180 MHz update |
|---|---|---|
| cx_gmu | 200001 kHz | 199999 kHz |
| gx_gfx3d | 0 (not running) | 0 (not running) |
| cx_gfx3d | 0 (not running) | 0 (not running) |
| gx_cxo | 19201 kHz | not measured |
| gx_gmu | 199999 kHz | not measured |

- The counter method works. cx_gmu reads the 200 MHz that stage 1 set, and gx_cxo reads XO.
- Clocks on the powered GX side can run: gx_gmu runs at 200 MHz and gx_cxo at 19.2 MHz.
- The gfx3d RCG took the update (CFG 0 → 0x101, UPDATE cleared), but its root still read off. gx_gfx3d
  and cx_gfx3d count nothing, both from XO before the update and from crc_div after it. So the status bit
  was right: **the GPU core clock does not run when the host drives it.**
- SPTPRAC stayed 0x94, and the stage stopped before any GX read, as designed.

This rules out causes 1 and 3 above. Cause 2 is unlikely, because gx_cxo, which comes from the same XO,
runs. That leaves cause 4: something the host cannot see holds the gfx3d root off, most likely the GMU.
Supporting facts from downstream:
- The GMU node in sdmmagpie-gpu.dtsi lists only cx_gmu, cxo, the AXI and the memnoc clocks; gfx3d is not
  among them. Only the kgsl GPU node names gfx3d (as core_clk).
- No DT node enables gpu_cc_crc_ahb_clk or the ACD clocks. They are left to the GMU firmware or to the
  hardware defaults.
- Mainline Linux runs the a618 in sc7180 with the GMU firmware setting the GPU frequency. As far as we
  know it has no host gfx3d clock, and its GX GDSC enable is a no-op left to the GMU. This is from memory
  and was not re-checked in this session.

2A ends here, per its decision rule: gx_gfx3d never ran. The next path is 2B. A last cheap 2A try would be
turning on crc_ahb and the ACD clocks, but nothing in downstream does that, so it is a guess.

### Stage 2B: the GMU powers GX (detailed plan, nothing built yet)

Planned 2026-10-25, after 2A hit its decision rule. The owner asked to see this plan before any building.

**Idea.** On this GPU the GMU firmware owns GX power and the GPU core clock. 2B runs Qualcomm's own
`a618_gmu.bin` the way the sunfish kernel's kgsl does: give it memory, load it, start it, then ask it for
GX through its mailbox registers and HFI messages. An OOB ("out of band") request is one of those mailbox
bits: the host sets a request bit and the GMU answers with an ack bit.

**Sources, read in this session:**
- The sunfish downstream kgsl: android.googlesource.com `kernel/msm`, branch
  `android-msm-sunfish-4.14-android13-qpr3`, `drivers/gpu/msm/`: `adreno_a6xx_gmu.c`, `kgsl_gmu.c/h`,
  `kgsl_hfi.c/h`, `kgsl_gmu_core.h`, `a6xx_reg.h`, `adreno.h`, `adreno-gpulist.h`.
- Mainline Linux as a cross-check: `kernel/common` android-mainline, `drm/msm/adreno/a6xx_gmu.c` and
  `a6xx_hfi.c`.
- The ARM SMMUv2 architecture spec for the SMMU registers.
- Our cmd-db dump (Stage 0) and the stock DTB.

Both kernels are GPL-2.0. We take register offsets, values and order from them, which are hardware facts,
and write our own code (§5).

**What downstream does for a618:**
- kgsl puts a618 in its "a615 family" (`adreno_is_a615_family`: A615, A616, A618). That family uses the
  legacy GMU interface:
  - The firmware is a raw image written straight into the GMU's TCMs, with no header.
  - An OOB request sets bit `req+16` and waits for bit `req+24`.
  - HFI is version 1, with 4 queues at the legacy indices.
- The a618 entry in gpulist: features 64BIT, RPMH, PREEMPTION, GPMU, CONTENT_PROTECTION, IFPC,
  IOCOHERENT. Firmware `a618_gmu.bin`, expected GMU firmware version 1.8.
- The DT quirk `qcom,gpu-quirk-hfi-use-reg` changes two things. The GX rail comes up through mailbox
  registers and an OOB before HFI starts. And the last HFI message is TEST.
- The cold-boot order (`a6xx_gmu_fw_start`):
  1. TCM retention, then the RSCC/PDC microcode (first boot only).
  2. The firmware into the TCMs, then the boot registers and the power config.
  3. CM3 out of reset; wait for 0xBABEFACE.
  4. GX rail on (OOB), SPTPRAC on, HFI init, the HFI messages.
  5. Later, OOB_GPU.
- Mainline does the same for a618, with two differences:
  - It does an RSCC "start" handshake before every firmware start, cold boots included. Downstream does
    it only on warm boots.
  - It sends a fixed one-level "off" bandwidth table for a618; its comment says the 618 GMU does no bus
    scaling.

**Firmware layout (checked in this session).** The 32 KiB image has two halves:
- The first 16 KiB is Cortex-M3 code. Its vector table gives SP 0x00043F80 and reset 0x9.
- The second 16 KiB is initial DTCM data: 204 non-zero bytes, all below +0x664.

In the host window, ITCM (dword 0x1B400, 16 KiB) is directly followed by DTCM (0x1C400, 16 KiB). One
32 KiB write from ITCM_START therefore fills both, and both kernels load it that way. §4's "fits the
32 KiB ITCM" was wrong and is corrected there. The first DTCM word is 0x10110000. With kgsl's version
macros that reads as 1.17, but whether it is a version word is a guess until FW_VER answers.

**Boot-args:**
- `sweetgpu=4` selects 2B.
  - Stage 1 runs unchanged: CX clocks, CX GDSC, GMU clock at 200 MHz.
  - None of the 2A writes happen: no apps-DRV `mx.lvl`/`gfx.lvl` vote, no host GX GDSC, no host gfx3d RCG.
- The new `sweetgmu=N` (0–6) stops after step 2B.N. One kernel can then go exactly as far as the owner
  approves, and each further step needs only new boot-args.

**New code (patch 0100):**
- `sweet_gmu.c`: RSCC/PDC, firmware load, CM3 start, OOB, HFI. About 700 lines.
- `sweet_smmu.c`: the GPU SMMU survey, one context bank and its page tables. About 300 lines. Stage 3
  reuses it for the GPU's own context.
- `sweet_gpu.c`: the `sweetgpu=4` flow and the GX checks, reusing 0099's frequency counter.
- `mkboot.sh`: with GPUFW=1, it also adds a 1 MiB zero blob `gmu-mem` through `mkpayload.py --blob`. The
  blob holds no vendor data.

#### 2B.0 Read-only survey (`sweetgmu=0`)

Nothing is written beyond stage 1. Every value goes to the log.
- **GPU SMMU** at 0x5040000 (needs the CX GDSC and `gcc_gpu_memnoc_gfx`, both on after stage 1):
  - global: IDR0/1/2/7, sCR0, sACR, sGFSR, every SMR and S2CR;
  - per bank (8 banks): CBAR/CBA2R, then SCTLR, TCR, TTBR0, MAIR0, FSR.
- **RSCC** (GMU block +0x23000): RSC_STATUS0, PDC_SLAVE_ID, HIDDEN_TCS_CMD0..2, the PDC start and match
  registers, SEQ_BUSY, SEQ_MEM_0..4; also GMU_RSCC_CONTROL_REQ/ACK.
- **PDC**: in the cfg block (0xb290000), TCS0–TCS3, ENABLE_PDC and SEQ_START_ADDR; in the seq block
  (0xb490000), words 0–4.
- **GMU**: CM3_SYSRESET, FW_INIT_RESULT, BOOT_CONFIG, HFI_CTRL_STATUS, HFI_VERSION_INFO,
  GMU2HOST_INTR_INFO/MASK, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, SPTPRAC_PWR_CLK_STATUS, GBIF_HALT,
  and the first ITCM and DTCM words.
- **The `gmu-mem` blob**: its PA, and a check that it reads zero.

It answers these questions:
- Is the SMMU on (sCR0.CLIENTPD)?
- Which stream-match groups and banks are in use? kgsl's `qcom,gpu-offset = <0x48000>` says the GPU
  itself uses bank 0, and TrustZone may hold a bank for the secure stream 2.
- Did the bootloader leave RSCC/PDC programmed?
- Can EL1 read the PDC at all?

✅ Pass when every read returns without a fault and the tables are in the log.

**Result on G3a (2026-10-25, kernel 0100, `sweetgpu=4 sweetgmu=0`): PASSED.** All reads returned, no panic,
the phone stayed usable, and the log ran ~2900 lines past "stage 2B done at 2B.0". Observed:
- **SMMU:** IDR0 0x48016605, IDR1 0x20008005, IDR2 0x00007511, sCR0 0x00200626, sGFSR 0. So: **5** stream-match
  groups (not 8 banks as planned), 3 SID bits, **5** context banks of 4 KiB from +0x8000, OAS 36 bits, stage 1
  and 4K tables supported. The SMMU is **on** (CLIENTPD 0) and unmatched streams fault (USFCFG 1).
- **All 5 SMRs are free and every S2CR is "fault"; all 5 banks are zero.** Nothing (bootloader, hypervisor,
  TrustZone) left a GPU context, so 2B.1's attach will take bank 4 and SMR 0. CB0 stays free for Stage 3.
- **GMU:** CM3 in reset (SYSRESET 1), FW_INIT_RESULT 0, HFI all zero, CM3_CFG 0x52, RPMH_POWER_STATE 0xf,
  SPTPRAC 0xd4. The TCMs hold non-zero words (ITCM 2060a47b…, DTCM 98aef028…), so they are not cleared at reset.
- **RSCC:** PDC_SLAVE_ID 1, PDC_SEQ_START 0x4520, MATCH_LO/HI 0x4510/0x4514: already the values 2B.2 writes.
  HIDDEN_TCS_CMD0/1/2_ADDR read 0x82204514/0x82204510/0x82204520 with DATA 0 (2B.2 writes them to 0, as both
  kernels do; whether these are reset values is a guess). SEQ_MEM_0..4 and SEQ_BUSY are 0.
- **PDC: EL1 reads work** (risk 2 is gone). ENABLE_PDC 0, SEQ_START 0, seq words 0, TCS0–3 all zero: not
  programmed.
- **gmu-mem:** pa 0x1024f0000, 1 MiB, all zero; the layout needs 0x89000.

**Result on G3b (2026-10-25 15:06, `sweetgmu=3`): stopped at 2B.1, phone fine.** The 0100 code did:
- uncached mapping of `gmu-mem` and the page tables for all six regions: OK (6 tables, exactly as sized);
- bank 4 (CBA2R, CBAR, TCR2, TCR, MAIR0, TTBR0, SCTLR): every register read back as written;
- S2CR0 = 0x4 (translate, bank 4): read back as written;
- **SMR0 = 0x80010004 (valid, id 4, mask 1): read back 0x00000000.** Attach undone; nothing after 2B.1 ran
  (no RSCC, PDC, TCM or CM3 writes).
- In the undo, S2CR0 was written back to 0x000200ff (fault) but **still read 0x4**.

What this suggests (guesses until tested): a writable S2CR whose "fault" write is ignored is the Qualcomm
hypervisor behaviour mainline Linux works around in `arm-smmu-qcom.c` (on some firmware, S2CR writes of type
fault are ignored). So the stream-mapping registers are probably hypervisor-emulated here, and the dropped SMR
is the emulation refusing our value. The stock DT gives the GMU two single stream IDs (`gmu_user` SID 4,
`gmu_kernel` SID 5, `#iommu-cells = <1>`, so mask 0); kgsl never writes a masked SMR. Next try (patch 0101):
one SMR per SID with mask 0, plus a probe that writes an SMR without the valid bit and reads it back, to tell
"every SMR write is dropped" from "this value is refused". If SMRs cannot be written at all, the fallback is to
reuse whatever the hypervisor allows (to be investigated from the downstream arm-smmu driver).

**Result on G3d (2026-10-25 16:23, patch 0101, `sweetgmu=3`): the attach worked; a false fault check stopped 2B.**
- SMR0 = 0x80000004 and SMR1 = 0x80000005 (valid, mask 0) both **read back as written**; S2CR0/1 = 0x4 (bank 4).
  So masked SMRs are refused but single-ID ones are accepted (observed); the probe did not run.
- Bank 4 read back SCTLR 0x1027, TCR 0x802020, TCR2 0x38011, TTBR0 0x00040001024f0000; sCR0 unchanged.
- The fault check after attach saw sGFSR 0 and **bank 4 FSR 0x00000400**, and 2B stopped. That value is FSR.FORMAT
  (bits 10:9 = 0b10, AArch64 descriptor format), a read-only field, not a fault: `sweet_smmu_faults` returns the
  raw FSR instead of masking the fault bits (TF..UUT bits 1–8, SS bit 30, MULTI bit 31, mask 0xc00001fe). Bug in 0100.
- The stop path left the attach in place (SMRs valid, bank 4 enabled); GMU still in reset, so nothing uses it.
  No RSCC, PDC, TCM or CM3 writes happened. Phone booted to the UI as before.
- Next: mask the FSR fault bits in `sweet_smmu_faults` (patch 0102), then repeat with `sweetgmu=3`.

**Result on G3e (2026-10-25 16:41, patch 0102, `sweetgmu=3`): 2B.1 passed; 2B.2 stopped at the RSCC, phone fine.**
- 2B.1: same SMR/S2CR/bank values as G3d, FSR 0x400, no fault; attach accepted. GMU stays in reset.
- 2B.2: GENERAL_7 = 1 (TCM retention), RSC_STATUS0 = 0x01000000, PDC_SLAVE_ID = 1, all three HIDDEN_TCS_CMDn_DATA
  (CMD2_DATA now 0x80000000), OVERRIDE_START_ADDR, PDC_SEQ_START/MATCH_LO/MATCH_HI and SEQ_MEM 0..4 all read back
  as written.
- **HIDDEN_TCS_CMD0/1/2_ADDR: wrote 0, kept 0x82204514 / 0x82204510 / 0x82204520** (the values seen in the 2B.0
  survey). The stop rule ("every RSCC value must read back") then stopped 2B before any PDC write.
- Observation: the low halves of the preset addresses (0x4514, 0x4510, 0x4520) are the same values as
  PDC_MATCH_VALUE_HI/LO and PDC_SEQ_START_ADDR. Guess: these three ADDR registers are set by boot firmware and
  write-protected from EL1; mainline and downstream kgsl both write 0 to them and never read them back, so stock
  Android probably runs with the preset values too.
- Next (proposal): write them as kgsl does, but treat a kept firmware value as expected (log it, do not stop);
  every other RSCC value must still read back.

**Result on G3f (2026-10-25 17:09, patch 0103, `sweetgmu=3`): RSCC passed; 2B.2 stopped at the PDC, phone fine.**
- 2B.1 as on G3e. RSCC: the three HIDDEN_TCS_CMDn_ADDR logged "kept" (same preset values); everything else read back.
- PDC: sequencer words, TCS1/TCS3 enable (0x7), every CMD ADDR (0x30010 mx, 0x30000 cx, 0x30090 xo) and DATA
  (TCS1 1/0/0, TCS3 2/2/3) read back; ENABLE_PDC reads 0x80000001.
- **The six CMD MSGID writes (0x10108) read back 0x10100**: bits 16 (write) and 8 (response required) stuck, the low
  byte (8, the length field in the RPMh TCS msgid layout) reads 0. Before 2B.2 these registers read 0.
- The stop rule stopped 2B after the PDC block, so **the PDC was left enabled** with TCS1/TCS3 loaded; the GMU stays in
  reset, so nothing triggers it (guess). Phone booted to the UI as before.
- Guess: the PDC does not implement (or does not return) the msgid length bits; kgsl writes 0x10108 and never reads
  back. Next (proposal): for the six MSGID writes only, accept a readback that differs from the written value only in
  bits 7:0 (log it); every other PDC value must still read back.

**Result on G3g (2026-10-25 17:30, patch 0104, `sweetgmu=3`): 2B.1–2B.3 all done; the GMU firmware runs.**
- 2B.1 and the RSCC as on G3f. PDC: the six MSGID writes logged "length byte differs, accepted"; every other PDC value
  read back; "2B.2 done".
- 2B.3: 8192 firmware words into ITCM/DTCM, 0 differ on read-back. Boot registers read back except (non-fatal by design,
  "going on"): CM3_CFG wrote 0x4052 reads 0x52; SYS_BUS_CONFIG, ICACHE_CONFIG, DCACHE_CONFIG wrote 1 read 0. HFI_QTBL_ADDR
  0x60100000, AHB_FENCE_RANGE_0 0x80a008a0, PWR_COL_CP_MSG 0x60300000, RPMH_CTRL 0xf11 read back.
- **CM3 out of reset: FW_INIT_RESULT = 0xBABEFACE after 42 us.** FW_BUSY 0, GMU2HOST 0, AO_HOST_INT 0,
  RPMH_POWER_STATE 0xf, SPTPRAC 0xd4, HFI_STATUS 0. No SMMU fault before or after (sGFSR 0, bank 4 FSR only the
  0x400 format field). Dummy and log pages still zero (firmware had not written them when read).
- The GMU was left running (2B stops at `sweetgmu=3`). Phone booted to the UI as before; no visible change (expected:
  nothing on screen uses the GPU yet).
- Open questions (guesses): whether the four ignored boot registers matter (cache configs reading 0 may mean they
  are write-only or locked like the RSCC addresses). Next: 2B.4 (`sweetgmu=4`), the GX rail vote through the GMU.

**Result on G3h (2026-10-25 18:16, same 0104 kernel, `sweetgmu=4`): 2B.4 PASSED; the GX side is powered.**
- 2B.1–2B.3 as on G3g (0xBABEFACE after 43 us, same four non-fatal boot-register mismatches).
- Perf table from cmd-db (gfx.lvl 0x30050, mx.lvl 0x30010, cx.lvl 0x30000): all eight GPU votes and the GMU's
  two match the expected table exactly (perf 1 = 180 MHz, vote 0x00400202).
- Mailbox BOOT_SLUMBER_OPTION 0, GX_VOTE_IDX 2, MX_VOTE_IDX 2 read back.
- **OOB boot_slumber acked** (GMU2HOST_INTR_INFO 0x40000000). SPTPRAC status 0x28 (bit 3 on); it read 0x28 right
  before our SPTPRAC write too, so the GMU had already switched it on during boot_slumber (observation).
- **GX GDSC on:** gx_gdscr 0x00222001 → 0xf8222000, gx_domain 0x003c0000. No SMMU fault.
- Clocks (0099 counter, trusted: cx_gmu 199999 kHz): **gx_gfx3d runs at 9600 kHz**, cx_gfx3d 9600 kHz, gx_cxo 19200 kHz,
  gx_gmu 0. gfx3d RCG cmd/cfg 0x2/0x0 and GPU PLL0 mode 0x01000005 with L = 0: the gfx3d clock is still on its
  boot source (XO/2), not the GPU PLL. That answers 2A's open question: the GX branch needs the GX rail, which only
  the GMU vote provides.
- Pass rule met (OOB acked, SPTPRAC bit 3, GX GDSC on, gx_gfx3d counts). 180 MHz is expected after 2B.6's DCVS
  request, not here (guess). Phone booted to the UI as before; no visible change.
- Next: 2B.5 (`sweetgmu=5`), HFI init with the GMU (same kernel).

**Result on G3i (2026-10-25 18:29, same 0104 kernel, `sweetgmu=5`): HFI up, but the INIT ack was skipped.**
- 2B.1–2B.4 as on G3h (cx_gmu 200001 kHz, gx_gfx3d 9600 kHz).
- BW table addresses from cmd-db: MC0 0x50000, SH0 0x5003c, ACV 0x5000c, CN0 0x500a8 (as planned).
- **HFI start: HFI_CTRL_STATUS 0x1 (up), HFI_VERSION_INFO 0** (major 0, so HFI v1 as expected).
- INIT sent with header 0x00100500 (id 0, 5 dwords, type 0, seq 1). The GMU consumed it (command queue read 5 =
  write 5) and posted one message: **header 0x0002047e = id 126 (F2H ACK), 4 dwords, type 2**. Our matcher requires
  word 1 to equal our whole header, so it logged "skipped message 0x0002047e" and timed out ("NO ACK in 100 ms").
  Type and size passed, so word 1 differed from 0x00100500 (observation); the log does not show word 1 or the error
  word, so how it differs is unknown.
- Mainline `a6xx_hfi_wait_for_ack` matches an ack by id 126 (HFI_F2H_MSG_ACK) and the **sequence number** (bits 31:20)
  of word 1 only. Guess: this firmware rewrites another field of the returned header.
- Nothing after INIT was sent. GMU and GX left on. Phone booted to the UI as before.
- Next (proposal, patch 0105): match acks the mainline way (id 126 + seqnum of word 1), and log every word of each
  ack and each skipped message, so the returned header and error code are visible.
- Patch 0105 (owner approved): acks matched by id 126 + seqnum (bits 31:20 of word 1); every dword of an ack or a
  skipped message is logged, plus "ack returns header X (sent Y)" when word 1 differs. Commit 2703ca8,
  CI 40175446607 passed. Next image: G3j (0105 kernel,
  `sweetgmu=5`), booted only with the owner's OK.

**Result on G3j (2026-10-25 18:48, 0105 kernel, `sweetgmu=5`): 2B.5 PASSED, all five HFI messages acked with error 0.**
- Log flush #133, 5032 lines, no panic. 2B.1–2B.4 as on G3i.
- Every ack is 4 dwords: header id 126, then word 1 = **our header with the size byte (bits 15:8) cleared**
  (INIT 0x00100500 → 0x00100000, FW_VER 0x00200201 → 0x00200001, and so on), error 0, payload. The same length-byte
  clearing was seen in the PDC MSGID readback (0104). The ack's own sequence number counts up by 1 per ack.

| Message | Sent header | Ack word 1 | Error | Payload |
|---|---|---|---|---|
| INIT | 0x00100500 | 0x00100000 | 0 | 0x0e550e55 |
| FW_VER | 0x00200201 | 0x00200001 | 0 | **0x10110000 = firmware 1.17** |
| PERF_TBL (8 GPU, 2 GMU levels) | 0x00302b04 | 0x00300004 | 0 | 0x0e550e55 |
| BW_TBL | 0x0040a003 | 0x00400003 | 0 | 0x0e550e55 |
| TEST | 0x00500205 | 0x00500005 | 0 | 0x0e550e55 |

- 0x0e550e55 looks like a filler for "no payload" (guess).
- After HFI: FW_INIT_RESULT 0xbabeface, FW_BUSY 1, RPMH_POWER_STATE 0xf, SPTPRAC 0x28, HFI_STATUS 1.
  SMMU: no global fault, bank 4 FSR 0x400 (FORMAT only, no fault).
- Next: 2B.6 (`sweetgmu=6`, same 0105 kernel): OOB_GPU, the DCVS vote through registers, and the first GX register
  reads (RBBM_STATUS, the CP always-on counter). This is the first GX register access. Only with the owner's OK.

**Result on G3k (2026-10-25 19:07, 0105 kernel, `sweetgmu=6`): 2B.6 PASSED. Stage 2 (GX power) is complete.**
- Log flush #389, 6158 lines, no panic. 2B.1–2B.5 as on G3j. Owner: phone "same as before".
- OOB_GPU acked (GMU2HOST bit 24). DCVS perf index 1: the three mailbox writes read back, OOB_DCVS acked (bit 31),
  DCVS_RETURN 0.
- After DCVS the GMU has set up the GPU PLL (mode 0xd1000005, L 0x12) and moved gfx3d onto it. Measured:
  **gx_gfx3d 180001 kHz** and cx_gfx3d 180001 kHz (the 180 MHz perf level), cx_gmu 200002 kHz, gx_cxo 19201 kHz.
  gx_gmu reads 0 now (it read 200 MHz on G2m; not understood, noted only).
- GX GDSC 0xf8222000 (on), SPTPRAC 0x28. RPMH_POWER_STATE reads 0 after DCVS (0xf after HFI); meaning unknown.
- **First GX reads work: RBBM_STATUS 0x00000001** (only bit 0, CP_AHB_BUSY_CX_MASTER, which is probably our own
  AHB read; the GPU is otherwise idle), RBBM_INT_0_STATUS 0, GBIF_HALT 0.
- **CP always-on counter runs:** +21938 over the "1 ms" wait, against +21931 on the GMU counter in the same wait,
  so the two counters agree (the wait itself was about 1.14 ms at 19.2 MHz).
- One minute of monitoring at 10 s steps: FW_INIT_RESULT stays 0xbabeface, GX GDSC stays on, no SMMU fault
  (bank 4 FSR 0x400 = FORMAT only).
- Stage 2 pass rule met: the always-on counter advances, RBBM_STATUS reads, no SMMU fault, phone stable.
- Next: Stage 3 (the GPU's own SMMU context bank, stream 0). It needs a new patch, written only with the owner's OK.

**Stage 3 built (patch 0106, owner approved, commit 721eab3, CI 40178628271 passed).** `sweetgpu=5` with `sweetgmu=6`
runs all of 2B, then:
- One level-1 table (4 KiB) in GPU-GMU-MEM at +0x89000, just past the GMU's part, so mkboot is unchanged.
- 1 GiB blocks map 0x80000000..0x1_ffff_ffff 1:1 (attr 0 normal non-cacheable, read/write, AF, nG).
- SMMU bank 0 (kgsl's GPU bank) for stream 0, mask 0, through `sweet_smmu_attach` with a 36-bit input range
  (T0SZ 28; IDR2 says IAS 36). The GMU keeps its 32-bit bank 4. Every write is read back as in 2B.1.
- Then the bank's fault registers, RBBM_STATUS3 bit 24 (stalled on an SMMU fault), and both banks every 10 s for 30 s.
- Differences from the plan above: no "all streams" SMR (stream 0 only, as planned), no separate global register dump
  (2B.0's survey already logs them), and the 8–16 GB GPU VA window is left for stage 4.
- Pass: every register reads back, no fault on bank 0 or bank 4, RBBM_STATUS3 not stalled, phone stable. ATOS is off
  and the GPU sends no traffic yet, so the first real walk is stage 4's first CP fetch.
- Test image: G3l = 0106 kernel, `sweetgpu=5 sweetgmu=6`, only with the owner's OK.

**Result on G3l (2026-10-25 19:38): Stage 3 PASSED.**
- Log flush #135, 5158 lines, no panic. Survey: IAS 36, OAS 36 bits. 2B done at 2B.6 as on G3k.
- Table at 0x102579000 (GPU-GMU-MEM +0x89000); 0x80000000..0x1ffffffff mapped 1:1 in 1 GiB blocks.
- Stream 0 → SMR2 0x80000000 (stuck), S2CR 0x00000000 (translate, bank 0). Bank 0 read back:
  SCTLR 0x00001027, TCR 0x0080201c (T0SZ 28), TCR2 0x00038011, TTBR0 0x0000000102579000. sCR0 unchanged 0x00200626.
  So the hypervisor lets EL1 program bank 0 and a stream-0 SMR, as it did for bank 4 (observation).
- Bank 0 FSR 0x400 (FORMAT only), sGFSR 0; RBBM_STATUS 0x1, RBBM_STATUS3 0 (not stalled).
- 30 s monitor: bank 0 and bank 4 clean, GMU alive (0xbabeface).
- Not tested, as planned: an actual walk (ATOS off, no GPU traffic). Stage 4's first CP fetch is that test.
- Next: Stage 4 (CP first light: SQE load, hardware init list, ring, CP_ME_INIT). New patch only with the owner's OK.

#### 2B.1 GMU memory and an SMMU context for streams 4–5 (`sweetgmu=1`)

The GMU reaches memory only through the GPU SMMU, as stream 5 (`gmu_kernel`) and stream 4 (`gmu_user`).
kgsl maps all GMU memory through stream 5. The DT's kgsl SMMU node has no `qcom,skip-init`, so the stock
Android kernel programs this SMMU itself, from EL1 (an inference from the DT).

**Memory.** The 1 MiB `gmu-mem` blob, page-aligned in kernel RAM like the firmware blobs.
- It is cleaned and invalidated once through the kernel's cacheable mapping.
- From then on it is used only through an uncached `ml_io_map_wcomb` mapping, with 32-bit stores.
- Cache maintenance alone would not be safe: the GMU writes HFI indices into the same cache lines as ours.

GMU addresses, from downstream's `gmu_vma` table and allocation order:

| GMU VA | Size | Use |
|---|---|---|
| 0x00000000 | 16 KiB | ITCM, inside the GMU, not mapped |
| 0x00004000 | 240 KiB | ICACHE backing |
| 0x00040000 | 16 KiB | DTCM, inside the GMU, not mapped |
| 0x00044000 | 240 KiB | DCACHE backing |
| 0x60000000 | 4 KiB | dummy page; downstream allocates it first, on purpose |
| 0x60100000 | 20 KiB | HFI queue table and 4 queues |
| 0x60200000 | 16 KiB | dump buffer (INIT's `dbg_buffer`) |
| 0x60300000 | 4 KiB | log (`PWR_COL_CP_MSG`) |

Each uncached region starts on its own 1 MiB, as downstream allocates them. With 24 KiB of page tables this
uses about 550 KiB of the blob.

**SMMU programming**, in this order. Register names are from the SMMUv2 spec. Context banks start at
+0x8000, 4 KiB each; kgsl's `gpu-offset` (bank 0 at 0x5048000) confirms that layout.
1. Pick a free bank (not bank 0) and a free stream-match group, from 2B.0.
2. Set up the bank:
   - SCTLR = 0 first.
   - CBA2R: AArch64 format. CBAR: stage 1 with stage 2 bypass.
   - TCR: 4 KiB granule, 32-bit input range (T0SZ 32, walks start at level 1, the 3-level tables the DT
     asks for), non-cacheable walks, TTBR1 walks off. Output size from IDR2.
   - MAIR0 attribute 0 = Normal non-cacheable. TTBR0 = the level-1 table.
   - TLBIALL and TLBSYNC.
   - Then SCTLR: translation on, fault reporting on, faults terminate.
3. Stream match: SMR = valid, ID 4, mask 1, so it matches 4 and 5. S2CR = translate to that bank.
4. Only if 2B.0 found the SMMU off: clear CLIENTPD. Unmatched streams keep bypassing, as now.
5. Global TLB invalidate and sync.
6. Read back every register written. Any mismatch stops 2B and undoes step 3.

**Page tables.**
- 1 level-1 table and 2 level-2 tables (0–1 GiB, 1–2 GiB).
- 3 level-3 tables: 0x0–0x1FFFFF, 0x60000000–0x601FFFFF, 0x60200000–0x603FFFFF.
- Leaf entries: access flag set, read/write at any privilege, attribute 0.

**Not written:** `attach-impl-defs`, the 12 Qualcomm tuning registers in the DT. Mainline does not write
them either; they are the fallback if the GMU later shows SMMU errors.

ATOS is disabled on this SMMU, so the CPU cannot test a translation. The real test is in 2B.3: the bank's
FSR must stay 0 while the firmware starts.

✅ Pass when every write reads back, and sGFSR and the bank's FSR are 0.

#### 2B.2 RSCC and PDC microcode (`sweetgmu=2`)

This is downstream's `_load_gmu_rpmh_ucode`, run once per boot, after GMU_GENERAL_7 (0x1F9CC) = 1 (TCM
retention), which downstream writes first. The RSCC sequences and the PDC TCS commands run only when the
GMU collapses or wakes the GPU power domain. We keep the GPU active, so they should not run during the
test. They are written anyway, because the firmware expects them.

RSCC (dword offsets from 0x5000000; the block starts at 0x23400):

| Register | Offset | Value |
|---|---|---|
| RSC_STATUS0_DRV0 | 0x23404 | 0x01000000 (bit 24: SDE clock gating off) |
| PDC_SLAVE_ID_DRV0 | 0x2340B | 1 |
| HIDDEN_TCS_CMD0 ADDR / DATA | 0x2340D / 0x2340E | 0 / 0 |
| HIDDEN_TCS_CMD1 ADDR / DATA | 0x2340F / 0x23410 | 0 / 0 |
| HIDDEN_TCS_CMD2 ADDR / DATA | 0x23411 / 0x23412 | 0 / 0x80000000 |
| OVERRIDE_START_ADDR | 0x23500 | 0 |
| PDC_SEQ_START_ADDR | 0x23408 | 0x4520 |
| PDC_MATCH_VALUE_LO / HI | 0x23409 / 0x2340A | 0x4510 / 0x4514 |
| SEQ_MEM_0..4_DRV0 | 0x23580.. | 0xA7A506A0 0xA1E6A6E7 0xA2E081E1 0xE9A982E2 0x0020E8A8 |

PDC seq block (0xb490000), words 0–4: 0xFEBEA1E1 0xA5A4A3A2 0x8382A6E0 0xBCE3E284 0x002081FC.

PDC cfg block (0xb290000, dword offsets): two TCS banks, each with 3 ARC commands (msgid 0x10108).
- The command addresses come from cmd-db at run time. On this phone they are mx.lvl 0x30010, cx.lvl
  0x30000 and xo.lvl 0x30090; mainline also uses 0x30090 for a618.
- The data are hlvl indices into our cmd-db tables (§4).

| Bank | CONTROL / ENABLE / WAIT | mx.lvl | cx.lvl | xo.lvl |
|---|---|---|---|---|
| TCS1 (0x1572) | 0 / 7 / 0 | 1 (vlvl 16) | 0 (off) | 0 (off) |
| TCS3 (0x15D6) | 0 / 7 / 0 | 2 (vlvl 192) | 2 (vlvl 64; the a618 value) | 3 (vlvl 128) |

Commands start at bank + 3 and are 4 dwords apart. Then PDC_GPU_SEQ_START_ADDR (0x1148) = 0 and
PDC_GPU_ENABLE_PDC (0x1140) = 0x80000001. Every write is read back.

These are votes from the GPU's own RSC. The AOP combines them with the other masters' votes, so a wrong
GPU vote cannot pull MX or CX below what the rest of the SoC asks for.

✅ Pass when every value reads back and the phone keeps running.

#### 2B.3 Firmware in, CM3 start (`sweetgmu=3`)

1. Fill the HFI queue table (layout in 2B.5). The boot registers point at it, so it must be ready before
   the CM3 starts.
2. Write the 8192 words of `a618_gmu.bin` from CM3_ITCM_START (0x1B400). Read them all back and compare
   (FNV-1a of the read-back against the blob's).
3. Boot registers:

   | Register | Offset | Value |
   |---|---|---|
   | CM3_FW_INIT_RESULT | 0x1F81C | 0 |
   | CM3_BOOT_CONFIG | 0x1F801 | 2 (TCM_BOOT) |
   | HFI_QTBL_ADDR | 0x1F985 | 0x60100000 |
   | HFI_QTBL_INFO | 0x1F984 | 1 |
   | AHB_FENCE_RANGE_0 | 0x23B11 | 0x80A008A0 (downstream's mask) |
   | CM3_CFG | 0x1F82D | 0x4052 |
   | HFI_SFR_ADDR | 0x1F982 | 0x06018000 (chip id 0x06010800, repacked) |
   | PWR_COL_CP_MSG | 0x1F900 | 0x60300000 (the log page; size field 0 = 1 page) |
   | SYS_BUS_CONFIG, ICACHE_CONFIG, DCACHE_CONFIG | 0x1F40F, 0x1F400, 0x1F401 | 1 each |
   | PWR_COL_INTER_FRAME_CTRL | 0x1F8C0 | 0x09C40400 |
   | RPMH_CTRL | 0x1F8E8 | OR 0xF11 (interface, LLC, DDR, MX, CX and GFX votes on) |

   Idle level "active": no IFPC, SPTP or NAP bits. The DT has `qcom,no-nap`, and the a618 has no SPTP_PC
   feature. IFPC stays off for bring-up, so the GPU never collapses behind our back.
4. CX_GMU_WFI_CONFIG (0x1F802) = 0. GBIF_HALT only if 2B.0 found it non-zero.
5. CM3_SYSRESET (0x1F800) = 0. Poll CM3_FW_INIT_RESULT for 0xBABEFACE, for up to 100 ms.
6. Log FW_INIT_RESULT, CM3_FW_BUSY, GMU2HOST_INTR_INFO, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, the
   bank's FSR/FAR/FSYNR0, sGFSR, and the first words of the dummy and log pages.

On a timeout: CM3_SYSRESET = 1, log everything, stop.
- If the bank shows a fault, its FAR names the GMU address that was missing.
- If there is no fault and no 0xBABEFACE, the next try adds mainline's RSCC start handshake before step 5:
  RSCC_CONTROL_REQ bit 1, wait for the ACK bit, wait for SEQ_BUSY 0, then REQ = 0.

✅ Pass when 0xBABEFACE arrives within 100 ms and the bank's FSR is 0.

#### 2B.4 GX rail through the GMU (`sweetgmu=4`)

The perf table is computed at run time from the DT power levels, downstream's gfx3d voltage table and
cmd-db, as `rpmh_arc_votes_init` does. The vote word is vlvl << 16 | mx index << 8 | gfx index.
Expected values:

| Index | GPU | Corner (vlvl) | gfx.lvl index (vlvl) | mx.lvl index (vlvl) | Vote |
|---|---|---|---|---|---|
| 0 | off | — | 0 | 0 | 0x00000000 |
| 1 | 180 MHz | MIN_SVS 48 | 2 (64) | 2 (192) | 0x00400202 |
| 2 | 267 MHz | LOW_SVS 64 | 2 (64) | 2 (192) | 0x00400202 |
| 3 | 355 MHz | SVS 128 | 3 (128) | 2 (192) | 0x00800203 |
| 4 | 430 MHz | SVS_L1 192 | 4 (192) | 2 (192) | 0x00C00204 |
| 5 | 565 MHz | NOM 256 | 5 (256) | 3 (320) | 0x01000305 |
| 6 | 650 MHz | NOM_L1 320 | 6 (320) | 3 (320) | 0x01400306 |
| 7 | 700 MHz | TURBO 384 | 7 (384) | 4 (384) | 0x01800407 |

- This is the bin-146 list reversed, with the DT's 0 Hz level 7 as index 0, the way kgsl builds it.
- The GMU's own table: index 0 off, index 1 200 MHz. 200 MHz needs MIN_SVS, which gives cx.lvl index 2
  (64) and mx index 2: vote 0x00400202.

Then downstream's `a6xx_gmu_gfx_rail_on`, for the initial level 6 (180 MHz, perf index 1):
1. Mailbox words in DTCM: BOOT_SLUMBER_OPTION (0x1CBF8) = 0, GX_VOTE_IDX (0x1CBF9) = 2,
   MX_VOTE_IDX (0x1CBFA) = 2.
2. OOB boot_slumber: HOST2GMU_INTR_SET (0x1F994) = bit 22. Poll GMU2HOST_INTR_INFO (0x1F992) for bit 30,
   up to 100 ms. Then GMU2HOST_INTR_CLR (0x1F991) = bit 30.
3. SPTPRAC: GX_SPTPRAC_POWER_CONTROL (0x1A881) = 0x00778000. Poll SPTPRAC_PWR_CLK_STATUS (0x1F8D0) for
   bit 3, up to 10 ms.
4. Reads only: the GX GDSC, GX_DOMAIN_MISC, the gfx3d RCG CMD/CFG, SPTPRAC status. Then measure
   gx_gfx3d with 0099's counter. This answers the question 2A could not.

✅ Pass when the OOB is acked, SPTPRAC bit 3 is set, the GX GDSC is on and gx_gfx3d counts. Any non-zero
rate passes; 180 MHz is the expected one.

#### 2B.5 HFI (`sweetgmu=5`)

Queue table at GMU 0x60100000 (downstream's `hfi_init`):
- Header: version 0, size 54 dwords, first queue header at dword 6, 12 dwords per header, 4 queues, all 4
  active.
- Queues of 4 KiB (1024 dwords) from +0xE0, the table size rounded up to 16 bytes:

  | Header | Queue | GMU address | Index / priority (type word) | Status |
  |---|---|---|---|---|
  | 0 | command | 0x601000E0 | 0 / 10 (0x0A00) | on |
  | 1 | message | 0x601010E0 | 4 / 10 (0x0A04) | on |
  | 2 | debug | 0x601020E0 | 5 / 40 (0x2805) | on |
  | 3 | DSP | 0x601030E0 | 1 / 20 (0x1401) | off |

- The legacy indices 4, 5 and 1 apply to the a615 family. In every header: rx_wm 1, tx_wm 1, rx_req 1,
  the rest 0.

**Start.** HFI_CTRL_INIT (0x1F986) = 1. Poll HFI_CTRL_STATUS (0x1F980) bit 0, up to 100 ms. Log
HFI_VERSION_INFO (0x1F981); its major (bits 31:28) should be below 2, which kgsl treats as HFI v1.

**Sending a message:**
1. Write it at the command queue's write index, wrapping at 1024. Update write_index, then a barrier.
2. HOST2GMU_INTR_SET = 1.
3. Poll GMU2HOST_INTR_INFO bit 0 (message queue), up to 100 ms. Both kernels poll this raw status
   register, so the interrupt mask stays as found.
4. Clear that bit, then read the message queue.

The header word is id | dwords << 8 | type << 16 | sequence << 20. An ack has type 2. Its word 1 is our
header (the sequence must match), and word 2 is the error code (0 is OK).

**The messages:**
1. INIT (id 0, 5 dwords): seg_id 0, dbg_buffer 0x60200000, size 0x4000, boot_state 1 (cold).
2. FW_VER (id 1, 2 dwords): we support 1.8 (0x10080000). Word 3 of the reply is the firmware version.
   Log it; the major must be 1 and the minor at least 8.
3. PERF_TBL v1 (id 4, 43 dwords): 8 GPU levels and 2 GMU levels, the votes above, frequencies in kHz.
4. BW_TBL (id 3, 160 dwords with the fixed array sizes): one "off" level, the shape mainline sends for
   a618.
   - DDR: 3 commands, to MC0 0x50000, SH0 0x5003c and ACV 0x5000c. Data 0x40000000 each, wait mask 1.
   - CNOC: 1 command, to CN0. Data 0x40000000 (level 0) and 0x60000001 (level 1), wait mask 1.
   - All addresses come from cmd-db by name. **Mainline's CNOC address 0x5007c is SN15 on this SoC; CN0
     here is 0x500a8.** Values copied from sc7180 would be wrong.
5. TEST (id 5, 2 dwords). It also tells the GMU that no more HFI messages follow.

Because the a618 GMU does no bus scaling, the GPU's DDR bandwidth still rides on the CPU's and the
display's DDR votes. If bandwidth becomes the limit, an apps-side BCM vote is Stage 9 work.

✅ Pass when HFI_CTRL_STATUS bit 0 sets and all five messages are acked with error 0.

#### 2B.6 OOB_GPU and the first GX reads (`sweetgmu=6`, the whole of 2B)

1. OOB_GPU: HOST2GMU_INTR_SET = bit 16. Poll for bit 24, then clear it.
2. Frequency through the registers (downstream's `a6xx_gmu_dcvs_nohfi`):
   - DCVS_ACK_OPTION (0x1CBFC) = 0.
   - DCVS_PERF_SETTING (0x1CBFD) = 0x30000001: perf index 1, option "at least".
   - DCVS_BW_SETTING (0x1CBFE) = 0x000FF0FF: no change.
   - OOB_DCVS: set bit 23, wait for bit 31. Read DCVS_RETURN (0x1CBFF), then clear bit 31.
3. Measure gx_gfx3d again.
4. The first GX reads, only if the GX GDSC is on and gx_gfx3d runs: RBBM_STATUS (0x210),
   CP_ALWAYS_ON_COUNTER_LO/HI (0x980/0x981) twice, 1 ms apart, and RBBM_INT_0_STATUS (0x201).
5. Leave the GMU running with OOB_GPU still set, so GX stays up. For one minute, log every 10 s:
   FW_INIT_RESULT, AO_HOST_INTERRUPT_STATUS, RPMH_POWER_STATE, the GX GDSC and the bank's FSR.

✅ Pass (Stage 2 as a whole) when the always-on counter advances, RBBM_STATUS reads, no SMMU fault is
latched, and the phone stays stable.

#### Test boots

One kernel (0100) and three images that differ only in `sweetgmu`. Each boot needs the owner's OK, as
before. Image names are checked against `shim/out` first (G2c was already taken).

| Image | `sweetgmu` | Writes | Purpose |
|---|---|---|---|
| G3a | 0 | none beyond stage 1 | survey: SMMU, RSCC, PDC, GMU state |
| G3b | 3 | SMMU, RSCC/PDC, the TCMs, CM3 start | the firmware boots |
| G3c | 6 | also the OOBs, HFI and the GX reads | GX powered by the GMU |

G3b and G3c may need fixes in between. Those are new patches and CI runs, shown before anything boots.

#### Risks for 2B, most uncertain first

1. **The hypervisor and the SMMU global registers.** Writes might be trapped or dropped. The stock kernel
   programs this SMMU from EL1 on the same firmware, so it is probably allowed. The read-back in 2B.1
   shows it.
2. **PDC access from EL1.** The PDC is an always-on block. Downstream maps it from EL1, so reads probably
   work. If not, the 2B.0 read faults and the test boot panics; the log survives.
3. **The CM3 does not boot.** The likely causes, in order: a missing mapping (the bank's FSR/FAR names
   it), the RSCC start handshake (mainline's order), a firmware version mismatch.
4. **The boot_slumber ack never comes.** Either the GMU cannot vote gfx through its RSC (RSCC/PDC wrong),
   or the vote index is wrong. RPMH_POWER_STATE and the AO interrupt status tell more.
5. **HFI messages are rejected.** Most likely the bandwidth table. We copy mainline's a618 shape with this
   SoC's cmd-db addresses.
6. **A GMU left running.** It reaches only the memory we mapped, and its votes go through RPMh. The worst
   case is extra power use or a reboot. The everyday H4 boot never sets `sweetgpu`.

2B leaves these for later: the GPU's own SMMU context (stream 0, Stage 3), the SQE (Stage 4), interrupts
(everything polls), and power collapse (IFPC and slumber, Stage 9).

### Stage 3: GPU SMMU

- The SMMU code, the register survey and the first context bank (the GMU's, streams 4–5) come earlier,
  in 2B.0–2B.1 (`sweet_smmu.c`). Stage 3 adds the GPU's own bank.
- Same idea as their 0121:
  - CB0 stage-1 AArch64, 4K granule, T0SZ 28.
  - 1 GB identity blocks for 0x80000000–0x1_ffff_ffff.
  - MAIR normal non-cacheable.
  - An SMR for stream 0 only, S2CR → CB0. **Not "all streams"** as in their 0121: streams 4–5 already
    match the GMU's bank, and two matching SMRs would be a stream-match conflict.
  - Then TLBIALL, sync and read back.
- Read IDR0/IDR1 first, and log every global register before and after.
- Sunfish-specific risk: **the hypervisor may trap or refuse SMR/S2CR writes**, or stage 2 may block
  parts of DRAM. The readback is the test. If it fails, use the context bank and SMR layout the bootloader
  or hypervisor already set up for SID 0 (dump it first), and only write CB0's TTBR/TCR/SCTLR.
- Fault IRQs are not wired up. Poll the fault status register (FSR) and log it in every probe step.
- Add the 8–16 GB GPU VA window (L1[8..15] → L2 → L3, 4 KB pages, one guard page after each mapping), as
  in their 0137/0150.

✅ Pass when the readback matches and no fault is latched.

### Stage 4: CP first light (passed on G4c, patches 0107–0108)

Planned 2026-10-25, after G3l passed Stage 3. The owner asked to see this plan before any code.

**Idea.** Start the command processor (CP) the way mainline's a6xx `hw_init` does, minus everything that
is not needed to prove the CP runs. The SQE microcode goes into memory the GPU reaches through Stage 3's
bank, then come the hardware init list, a ring buffer, `CP_ME_INIT`, and packets whose only effect is a
CP register the CPU can read back. Stage 4 makes **no GPU writes to DRAM** by design: the ring has
`NO_UPDATE` set and no packet writes memory. Memory writes are Stage 5's test, because the GPU is still
in secure mode (their 0130 finding).

**Sources, read in this session:**
- Mainline Linux 6.18.54 (the WSL kernel tree): `drm/msm/adreno/a6xx_gpu.c` (`hw_init`,
  `a6xx_cp_init`, `a6xx_set_ubwc_config`, `a6xx_ucode_check_version`, `a6xx_idle`), `a6xx_catalog.c`
  (the `qcom,sm7150` a618 entry), `adreno_gpu.h` (PKT4/PKT7), `soc/qcom/ubwc_config.c` (`sm7150_data`).
- Our generated headers `xnu/gpu/regs/a6xx.xml.h`, `adreno_common.xml.h`, `adreno_pm4.xml.h` for the
  offsets.
- The downstream sunfish kgsl is **not** in `src/sunfish-kernel` (no `drivers/gpu/msm`), so mainline is
  the only reference for the init list. Values are hardware facts from GPL code. We write our own code (§5).
- The firmware file itself: `a630_sqe.fw` is 32136 B; dwords 0–3 are `0x00000000 0x016ee19a 0x01001ee1
  0x01190001`.

**What mainline does for this GPU** (the `qcom,sm7150` entry, chip id 0x06010800):
- SQE `a630_sqe.fw`, GMEM 512 KiB, `hwcg = a615_hwcg`, `protect = a630_protect`,
  `prim_fifo_threshold = 0x00180000`. Its zap is `a615_zap`, which matches our blobs.
- `a6xx_has_gbif()` is true (everything except a630), so the GBIF halts are cleared. The QoS write still
  takes the VBIF branch, because a618 is not in the a610/a640/a650 family list.
- UBWC from `sm7150_data`: UBWC 2.0, level 2 and 3 swizzle on, highest bank bit 14. So hbb = 1
  (hbb_lo 1, hbb_hi 0), `ubwc_mode` 0 (it is set only for UBWC 1.0), amsbc 0, and macrotile 0.
- The SQE check: the low 12 bits of the first word after dword 0 must be ≥ 0x190. Ours is 0x19a, so
  mainline would accept it and would use `WHERE_AM_I`. We use neither the RPTR shadow nor `WHERE_AM_I`.
- Order: OOB_GPU set → GBIF un-halt → SECVID TSB → 64-bit address modes → HWCG → QoS → UCHE → ROQ / mem
  pool / prim fifo → AHB, perf counters → UBWC → hang detect → UCHE_CLIENT_PF → GMU busy counters →
  CP_PROTECT → INT_0_MASK → SQE base → ring base/cntl → `CP_SQE_CNTL = 1` → `CP_ME_INIT` + idle → zap,
  then `CP_SET_SECURE_MODE 0` → clear OOB_GPU and OOB_BOOT_SLUMBER.

**What we already have from 2B/3:**
- OOB_GPU and OOB_BOOT_SLUMBER are set and never cleared, so the GMU keeps GX on. Stage 4 keeps it that way.
- Bank 0 (SID 0): 1 GiB identity blocks over 0x80000000–0x1_ffff_ffff, MAIR attribute 0 = Normal
  non-cacheable. The CPU maps GPU-GMU-MEM write-combined, so a `dsb sy` before each doorbell is enough and
  no cache maintenance is needed. The bank's SCTLR has CFRE but not CFCFG or HUPCF: a fault terminates
  the access instead of stalling it, and the FSR latches it.
- The `GPU-SQE` blob has been in the image since 0094.

**Boot-args:**
- `sweetgpu=6` selects Stage 4. It needs `sweetgmu=6`, and Stage 3 must pass in the same boot.
  `sweetgpu=5` stays exactly as on G3l.
- The new `sweetcp=N` (0–6, default 0) stops after step 4.N, like `sweetgmu`.

**New code (patch 0107):**
- `sweet_cp.c` (new, about 400 lines): steps 4.0–4.6, the PM4 header helpers (mainline's parity formula),
  the "CP dump" below.
- `sweet_gmu.c`: `sweet_gmu_spare()` becomes a page-aligned bump allocator. The first call still returns
  +0x89000, so Stage 3's table does not move.
- `sweet_gpu.c`: the `sweetgpu=6` hook after `gpu_stage3()`, which must return success. The "stages above
  N not built" message moves to `gpu_level > 6`.
- No `mkboot.sh` change: everything fits in the existing 1 MiB GPU-GMU-MEM blob.

**Memory layout** (GPU-GMU-MEM offsets; PAs as on G3l, where the blob started at 0x1024f0000; IOVA = PA
through the identity map):

| Offset | Size | PA on G3l | Use |
|---|---|---|---|
| +0x89000 | 4 KiB | 0x102579000 | Stage 3 level-1 table (unchanged) |
| +0x8a000 | 32 KiB | 0x10257a000 | SQE: the file without dword 0 (32132 B), rest zero |
| +0x92000 | 16 KiB | 0x102582000 | ring 0 (4096 dwords) |
| +0x96000 | 4 KiB | 0x102586000 | scratch page, filled with 0x5ee7dead; a tripwire in Stage 4, the marker target in Stage 5 |
| +0x97000 | | | end (the blob is 0x100000) |

**The "CP dump"** (logged after every step from 4.2 on, and on any stop):
RBBM_STATUS 0x210, STATUS1 0x211, STATUS2 0x212, STATUS3 0x213, RBBM_INT_0_STATUS 0x201,
CP_RB_RPTR 0x806, CP_RB_WPTR 0x807, CP_SQE_CNTL 0x808, CP_HW_FAULT 0x821, CP_INTERRUPT_STATUS 0x823,
CP_PROTECT_STATUS 0x824, CP_STATUS_1 0x825, CP_IB1_BASE 0x928/0x929, CP_IB1_REM_SIZE 0x92a,
CP_ROQ_AVAIL_RB 0x948, CP_SCRATCH_REG(0–7) 0x883–0x88a, CP_ALWAYS_ON_COUNTER 0x980/0x981, bank 0
FSR/FAR/FSYNR0, the GX GDSC, and the GMU's AO_HOST_INTERRUPT_STATUS. When a step from 4.3 on fails, the dump
is followed by `CP_SQE_CNTL = 0` (halt the SQE) and the stage stops. Nothing else is undone.

Every poll is bounded (1 s unless stated) and spins with 10 µs delays. Each register write is
announced with `gpu_pause()` as in 2B, then read back and logged with its expected value.

#### 4.0 Survey and memory (`sweetcp=0`)

No GPU register is written. CPU writes go only to our own blob.
1. Read and log the reset values of every register 4.1 and 4.2 will write. Also log
   RBBM_SECVID_TRUST_CNTL 0xf400 (read only, **never written**: the `secvid-set-once` quirk),
   RBBM_CLOCK_CNTL 0xae, CP_PROTECT_CNTL 0x84f, CP_PROTECT_REG(0–31) 0x850–0x86f, RBBM_INT_0_MASK 0x38,
   GBIF_HALT_ACK 0x3c46, RBBM_GBIF_HALT_ACK 0x17, and the CP dump.
2. Check the SQE blob: length 32136, dword 0 = 0, and (dword 1 & 0xfff) ≥ 0x190. A failed version check is
   logged but does not stop the stage, since nothing depends on `WHERE_AM_I` yet.
3. Allocate the three areas above. Copy the SQE without dword 0 and read every word back. Zero the ring,
   poison the scratch page, then `dsb sy`.

✅ Pass when the reads complete, the copy compares equal, and no SMMU fault is latched.

#### 4.1 Hardware init list (`sweetcp=1`)

Mainline's `hw_init` values for this GPU, in mainline's order. "Skip" rows are left at reset and logged
only.

| # | Register | Offset | Value | Note |
|---|---|---|---|---|
| 1 | GBIF_HALT, RBBM_GBIF_HALT | 0x3c45, 0x16 | 0, 0 | then read each back; log both ACKs |
| 2 | RBBM_SECVID_TSB_CNTL | 0xf803 | 0 | |
| 3 | RBBM_SECVID_TSB_TRUSTED_BASE, _SIZE | 0xf800/1, 0xf802 | 0, 0 | no trusted range |
| 4 | *_ADDR_MODE_CNTL × 12 | CP 0x842, VSC 0xc01, GRAS 0x8601, RB 0x8e05, PC 0x9e01, HLSQ 0xbe05, VFD 0xa601, VPC 0x9601, UCHE 0xe00, SP 0xae01, TPL1 0xb601, RBBM_SECVID_TSB 0xf810 | 1 each | 64-bit addressing |
| – | HWCG (`a615_hwcg`, ~60 writes plus GMU CGC) | | **skip** | clock gating is power, not function; Stage 9 |
| 5 | RBBM_VBIF_CLIENT_QOS_CNTL | 0x10 | 0x3 | |
| 6 | RBBM_PERFCTR_GPU_BUSY_MASKED | 0x50b | 0xffffffff | |
| 7 | UCHE_WRITE_RANGE_MAX | 0xe05/6 | 0x0001ffff_ffffffc0 | trap base + 0xfc0 |
| 8 | UCHE_TRAP_BASE, UCHE_WRITE_THRU_BASE | 0xe09/a, 0xe07/8 | 0x0001ffff_fffff000 | mainline's a6xx `uche_trap_base` |
| 9 | UCHE_GMEM_RANGE_MIN, _MAX | 0xe0b/c, 0xe0d/e | 0x00100000, 0x0017ffff | 512 KiB GMEM at 1 MiB |
| 10 | UCHE_FILTER_CNTL, UCHE_CACHE_WAYS | 0xe18, 0xe17 | 0x804, 0x4 | |
| 11 | CP_ROQ_THRESHOLDS_2, _1 | 0x8c2, 0x8c1 | 0x010000c0, 0x8040362c | |
| 12 | CP_MEM_POOL_SIZE | 0x8c3 | 128 | |
| 13 | PC_DBG_ECO_CNTL | 0x9e00 | 0x00180000 | prim fifo threshold |
| 14 | CP_AHB_CNTL | 0x98d | 0x1 | AHB default slave response = error |
| 15 | RBBM_PERFCTR_CNTL; CP_PERFCTR_CP_SEL(0) | 0x500; 0x8d0 | 1; 0 | 0 = `PERF_CP_ALWAYS_COUNT` (`a6xx_perfcntrs.xml`) |
| 16 | RB_NC_MODE_CNTL, TPL1_NC_MODE_CNTL, SP_NC_MODE_CNTL | 0x8e08, 0xb604, 0xae02 | 0x2 each | UBWC: hbb_lo 1, everything else 0 |
| 17 | UCHE_MODE_CNTL; RBBM_NC_MODE_CNTL | 0xe01; 0x534 | 0x00200000; 0 | hbb_lo << 21; macrotile 0 |
| 18 | RBBM_INTERFACE_HANG_INT_CNTL | 0x1f | 0x401fffff | hang detect on |
| 19 | UCHE_CLIENT_PF | 0xe19 | 0x81 | |
| – | GMU busy and power counters | | **skip** | Stage 9 |
| – | CP_PROTECT_CNTL and the 32 `a630_protect` entries | 0x84f, 0x850+ | **skip** | Stage 6 turns it on (see note) |
| – | RBBM_INT_0_MASK | 0x38 | **stays 0** | no handler yet; mainline's 0x03d0e242 waits for Stage 6 |

Note on CP_PROTECT: the scratch registers 0x883–0x88a sit just past mainline's `NORDWR(0x800, 0x82)` span,
so the 4.5 test keeps working once protection is on.

Never written in Stage 4: RBBM_SECVID_TRUST_CNTL, RBBM_SW_RESET_CMD, anything in the GMU's own range
except what 2B already does.

✅ Pass when every row reads back its value. A row that reads back differently is logged, as in 2B.2's RSCC
rule. It stops the stage only for rows 1–4 (halt, secure and address modes), which the CP needs.

#### 4.2 SQE and ring registers, CP still stopped (`sweetcp=2`)

| Register | Offset | Value |
|---|---|---|
| CP_SQE_INSTR_BASE | 0x830/0x831 | SQE IOVA (0x10257a000 on G3l's layout) |
| CP_RB_BASE | 0x800/0x801 | ring IOVA |
| CP_RB_CNTL | 0x802 | 0x0800020b: BUFSZ 11 (16 KiB / 8), BLKSZ 2 (32 B / 8) << 8, NO_UPDATE (bit 27) |
| CP_RB_WPTR | 0x807 | 0 |

CP_RB_RPTR_ADDR is not written, because NO_UPDATE means the CP never writes an RPTR shadow.

✅ Pass when all four read back, RPTR reads 0, and the CP dump shows nothing new.

#### 4.3 Start the SQE on an empty ring (`sweetcp=3`)

1. `CP_SQE_CNTL = 1`. **This is the GPU's first memory fetch through the SMMU**: the SQE reads its
   microcode from the SQE IOVA.
2. Dump at 1 ms, 100 ms and 1 s.

✅ Pass when bank 0's FSR stays 0, CP_HW_FAULT stays 0, RPTR stays 0 (the ring is empty), and RBBM_STATUS
settles to idle: `RBBM_STATUS & ~1 == 0`, mainline's check, where bit 0 is our own AHB access.

#### 4.4 `CP_ME_INIT` (`sweetcp=4`)

Ring dwords 0–8: `0x70c80008` (PKT7 opcode 0x48, count 8), then `0x2f, 0x3, 0x20000000, 0, 0, 0, 0, 0`,
the mainline a6xx payload. Then `dsb sy` and `CP_RB_WPTR = 9`.

✅ Pass when, within 1 s, RPTR = 9, RBBM_STATUS is idle and CP_HW_FAULT is 0. Log the time it took. This
is mainline's `a6xx_idle` test without the GMU-idle part.

#### 4.5 Register markers and ring wrap (`sweetcp=5`)

1. One marker: `0x48088a01` (PKT4 CP_SCRATCH_REG(7) 0x88a, count 1), then `0x5ee70001`. Update WPTR, then
   poll until CP_SCRATCH_REG(7) reads 0x5ee70001 and RPTR equals WPTR.
2. A wrap test: 80 batches. Each batch is 64 markers (128 dwords) with values 0x5ee71000 + n. After each
   batch, wait for idle and check the scratch register holds the batch's last value. 80 × 128 dwords is
   2.5 times round the 4096-dword ring. Because ME_INIT left an odd 9-dword offset, some packets straddle
   the end of the ring, as they will in real use.
3. Check that the scratch page still holds 0x5ee7dead everywhere and that the ring area outside what we
   wrote is still zero (no stray GPU writes).

✅ Pass when every batch lands, RPTR and WPTR agree after each batch (WPTR wraps modulo 4096), and both
tripwires are intact.

#### 4.6 Hold (`sweetcp=6`, the whole of Stage 4)

For 30 s, every 10 s: one marker (0x5ee72000 + n), the CP dump, the GX GDSC and the GMU state. Then leave
the SQE running and idle, with OOB_GPU still held. Rebooting clears it, as with Stage 3.

✅ Pass (Stage 4 as a whole) when all markers land, no SMMU fault, CP_HW_FAULT or CP_PROTECT_STATUS is
latched, RBBM_STATUS stays idle between markers, and the phone stays stable.

The draft's "SQE version register reads back" criterion is dropped: a6xx has no such register (the version
lives in the firmware file, checked in 4.0). The CP_SQE_INSTR_BASE readback in 4.2 replaces it.

#### Test boots

One kernel (0107) and three images. All use `sweetgpu=6 sweetgmu=6` and the 828xr ADT, and differ only in
`sweetcp`. `shim/out` has no G4* names yet. Each boot needs the owner's OK.

| Image | `sweetcp` | Writes beyond G3l | Purpose |
|---|---|---|---|
| G4a | 2 | the init list and the ring registers; CP not started | the writes stick |
| G4b | 4 | also `CP_SQE_CNTL = 1` and ME_INIT | the CP runs (first GPU fetch) |
| G4c | 6 | also the markers, the wrap test and the hold | Stage 4 complete |

Fixes found on G4a or G4b are new patches and CI runs, shown before anything boots.

#### Risks for Stage 4, most uncertain first

1. **The SQE fetch faults or reads garbage.** Bank 0 has not done a real walk yet (ATOS is off). The
   FSR/FAR name the address. Other suspects: dword 0 not skipped, or the write-combined copy not drained
   before `CP_SQE_CNTL` (the 4.0 readback plus `dsb sy` cover that).
2. **Secure mode blocks the ring fetch.** Mainline runs ME_INIT *before* leaving secure mode, so the CP
   should fetch non-secure memory while secure. If RPTR stays 0 with no fault, this is the first guess.
   The answer would be the zap (Stage 5) before ME_INIT, not writing SECVID_TRUST_CNTL.
3. **A SECVID TSB write is refused or raises an external abort.** The readback shows a refusal. An abort
   panics the test boot, and the persistent log survives (as on G0H).
4. **Hang detect that we cannot see.** INT_0_MASK stays 0, and we do not know yet whether INT_0_STATUS
   latches masked causes. The dumps tell. If it does not latch, hangs show only as RPTR ≠ WPTR plus
   RBBM_STATUS busy, which is enough for Stage 4.
5. **GX collapses under us.** OOB_GPU is held as in 2B.6, so the GMU should not collapse GX. A CPU read of a
   collapsed GX would stall one core. G0H showed that does not freeze iOS, and the log survives.
6. **The skipped HWCG.** Without it the clocks run ungated: more power, nothing functional. Mainline turns
   it on before the CP starts. If the SQE misbehaves only without it, Stage 4 gets an HWCG step.

Stage 4 leaves these for later: memory writes and leaving secure mode (Stage 5), CP_PROTECT, interrupts,
indirect buffers and fences (Stage 6), HWCG and power collapse (Stage 9).

### Stage 5: zap shader and leaving secure mode (passed on G5c, patches 0109–0110)

Planned 2026-10-25, after G4c passed Stage 4. The owner asked to see this plan before any code.

**Idea.** Do what mainline's a6xx `hw_init` does right after `CP_ME_INIT`: have TrustZone load and
authenticate the zap shader (PAS id 13), then send `CP_SET_SECURE_MODE 0` on the ring. Then test the GPU's
first writes to DRAM with `CP_MEM_WRITE` to the scratch page. Stage 4 left the CP running in secure mode,
where (by their 0130 finding) the ring writes nothing to normal memory. This is the first stage that calls
TrustZone for something other than PSCI.

**Sources, read in this session:**
- Mainline Linux 6.18.54 (the WSL kernel tree): `firmware/qcom/qcom_scm.c` (`qcom_scm_pas_init_image`,
  `_mem_setup`, `_auth_and_reset`, `_pas_supported`, `__qcom_scm_is_call_available`, `__get_convention`),
  `qcom_scm-smc.c` (`__scm_smc_call`, the INTERRUPTED, EBUSY and WAITQ loops), `qcom_scm.h` (ids, error
  codes, `QCOM_SCM_ARGS`), `soc/qcom/mdt_loader.c` (`qcom_mdt_read_metadata`, `qcom_mdt_pas_init`,
  `__qcom_mdt_load`), `drm/msm/adreno/adreno_gpu.c` (`zap_shader_load_mdt`) and `a6xx_gpu.c`
  (`a6xx_zap_shader_init` and the zap block of `hw_init`), and `adreno_pm4.xml` (opcodes).
- The firmware files, parsed in WSL (below).
- Our PSCI `smc #0` helper from patch 0065, which already works on this phone (`CPU_ON`, 8 cores).

**What mainline does:**
1. `qcom_mdt_pas_init`: build the metadata (ELF header segment plus hash segment), copy it into a
   physically contiguous, 4 KiB aligned, **non-cacheable** buffer ("to avoid XPU violations"), then call
   `PAS_INIT_IMAGE(13, pa)`. If any loadable segment has the relocatable flag (bit 27), also call
   `PAS_MEM_SETUP(13, carve-out pa, align_4k(max p_paddr + p_memsz) - min p_paddr)`.
2. `__qcom_mdt_load`: copy each loadable segment to `carve-out + (p_paddr - min p_paddr)` through a
   write-combined mapping, zero-fill `p_memsz - p_filesz`.
3. `PAS_AUTH_AND_RESET(13)`. `-EOPNOTSUPP` means "no zap on this target"; we do not take that path.
4. On the ring: `CP_SET_SECURE_MODE` with payload 0, flush, and `a6xx_idle`. With no zap (`-ENODEV`) mainline
   writes `RBBM_SECVID_TRUST_CNTL = 0` instead. Our `secvid-set-once` quirk forbids that, and this
   plan never does it.
5. The zap is loaded once per boot (`static bool loaded`). Later power-ups skip it. Power collapse is Stage 9.

**The SCM calls** (SMCCC, SIP owner, 64-bit, from `qcom_scm.h`). The function id is
`type << 31 | 1 << 30 | 2 << 24 | svc << 8 | cmd`. Mainline uses fast calls (type 1) for the atomic
convention probe and standard calls (type 0) for the rest. x1 = arginfo
(`nargs | type_i << (4 + 2i)`, VAL 0, RO 1, RW 2), x2..x5 = args. The results come back in x0 (status) and
x1 (`result[0]`).

| Call | x0 (function id) | x1 arginfo | x2.. | Wanted result |
|---|---|---|---|---|
| convention probe = `IS_CALL_AVAIL` (svc 6, cmd 1), fast | 0xc2000601 | 0x1 | 0x02000601 | x0 0, x1 1 |
| `IS_CALL_AVAIL` for PIL cmds 1, 2, 5, 7 | 0x42000601 | 0x1 | 0x02000201 / 202 / 205 / 207 | x0 0, x1 1 each |
| `PAS_IS_SUPPORTED` (svc 2, cmd 7) | 0x42000207 | 0x1 | 13 | x0 0, x1 1 |
| `PAS_INIT_IMAGE` (cmd 1) | 0x42000201 | 0x82 (VAL, RW) | 13, metadata pa | x0 0, x1 0 |
| `PAS_MEM_SETUP` (cmd 2) | 0x42000202 | 0x3 | 13, 0x97715000, 0x1000 | x0 0, x1 0 |
| `PAS_AUTH_AND_RESET` (cmd 5) | 0x42000205 | 0x1 | 13 | x0 0, x1 0 |

Never called: `PAS_SHUTDOWN` (cmd 6) and `SET_REMOTE_STATE`. A half-done PAS session is cleared by the
reboot that ends every test boot.

How x0 is handled, as in `qcom_scm-smc.c`:
- **1, INTERRUPTED:** call again with x0 = 1, the same x1–x5, and the x6 the call returned (Qualcomm's
  "A6 quirk": x6 carries the session). Up to 1000 times.
- **-12, EBUSY:** sleep 30 ms and repeat the original call, up to 20 times (mainline's numbers).
- **2, WAITQ_SLEEP:** needs the wait-queue service, which we do not implement. Log it and stop.
- **-1 / -2 / -3 / -4 / -5:** ERROR / EINVAL_ARG / EINVAL_ADDR / EOPNOTSUPP / ENOMEM. Log the name and stop.
- x0–x7 are logged for every call, together with the retry counts and the time taken.

The SMC is issued from the GPU thread with interrupts on (as for PSCI). The helper is a new
`sweet_scm_call()` modelled on our 0065 PSCI helper: x0–x7 in, x0–x3 and x6 out, x8–x17 clobbered.

**The firmware** (parsed in WSL from `~/ios-pixel/gpu-fw/fw/`):
- `a615_zap.mdt` is ELF32 (class 1, machine 164 = QDSP6 header convention), 6860 B, entry 0x5000,
  3 program headers:

| # | p_type | p_offset | p_paddr | p_filesz | p_memsz | p_flags | Meaning |
|---|---|---|---|---|---|---|---|
| 0 | NULL | 0 | 0 | 0x94 | 0 | 0x07000000 | the ELF header segment (MDT type 7) |
| 1 | NULL | 0x1000 | 0x6000 | 0x1a38 | 0x2000 | 0x02200000 | hash segment (MDT type 2) |
| 2 | LOAD | 0x3000 | 0x5000 | 0x7c0 | 0x7c0 | 0x08000007 | the code; **relocatable** (bit 27) |

- So the metadata is 0x94 + 0x1a38 = 6860 B. Because the hash follows the header in the file, the
  metadata is **the whole `.mdt` file**, which is exactly our `GPU-ZAP-MDT` blob. `.b00` and `.b01` are its
  two halves (checked byte for byte).
- The `GPU-ZAP-B02` blob (1984 B = 0x7c0) is segment 2.
- min p_paddr = 0x5000. max = align_4k(0x5000 + 0x7c0) = 0x6000. **`MEM_SETUP` size = 0x1000**: the
  old draft's 0x2000 was the carve-out size, not mainline's formula. Segment 2 goes to carve-out offset 0.
- The carve-out 0x97715000 (0x2000, from the Pixel DT) lies below XNU's RAM (0x1_0000_0000+ in
  high-memory mode), so XNU never allocates it. It is inside Stage 3's identity map.

**What we already have from Stages 2B–4:** OOB_GPU held, the SQE running and idle after ME_INIT and 4.5's
markers, `cp_emit` / `cp_kick` / `cp_wait_idle` / `cp_marker` / `cp_dump` / `cp_clean`, and the scratch page
filled with 0x5ee7dead. Both blobs have been in every image since 0094.

**Boot-args:**
- `sweetgpu=7` selects Stage 5. It needs `sweetgmu=6`, and Stage 4 must pass in the same boot with
  `sweetcp=5` or more, because the zap comes after ME_INIT as in mainline. The test images use `sweetcp=5`,
  which skips 4.6's 30 s hold. `sweetgpu=6` stays exactly as on G4c.
- The new `sweetzap=N` (0–6, default 0) stops after step 5.N.

**New code (patch 0109):**
- `sweet_zap.c` (new, about 350 lines): `sweet_scm_call()` with the retry rules, the MDT checks, steps
  5.0–5.6, and the "SCM log" line per call.
- `sweet_cp.c`: export the ring helpers (`emit`, `kick`, `wait_idle`, `marker`, `dump`, `clean`, `halt`) and
  the scratch page va/pa through `sweet_gpu_internal.h`. Nothing in stage 4's behaviour changes.
- `sweet_gpu.c`: parse `sweetzap`; after `sweet_cp_run()` succeeds with `gpu_level >= 7`, call
  `sweet_zap_run(gpu_zap_stop, MDT blob, B02 blob)`. The "stages above N not built" message moves to
  `gpu_level > 7`.
- No `mkboot.sh` change: both zap blobs are already in the image, and the metadata copy fits in GPU-GMU-MEM.

**Memory** (GPU-GMU-MEM offsets after Stage 4's areas; PAs as on G4c, where the tripwire/scratch page was
0x10258a000):

| Offset | Size | PA on G4c layout | Use |
|---|---|---|---|
| (stage 4) | 4 KiB | 0x10258a000 | scratch page: 0x5ee7dead, now the `CP_MEM_WRITE` target |
| next | 8 KiB | 0x10258b000 | metadata copy (6860 B, rest zero): contiguous, 4 KiB aligned, mapped non-cacheable like mainline's `dma_alloc_coherent` buffer |
| carve-out | 4 KiB used of 8 | 0x97715000 | segment 2, written through a device mapping (`ml_io_map`) |

After `PAS_INIT_IMAGE` the CPU never touches the metadata copy again. After `PAS_AUTH_AND_RESET` it never
touches the carve-out again, because TrustZone locks both with the XPU, and a CPU access would abort.

**Register writes in Stage 5:** none by the CPU except the doorbell `CP_RB_WPTR`, and `CP_SQE_CNTL = 0`
when a step fails after 5.1. Everything else goes through TrustZone or the ring. **Never** written:
`RBBM_SECVID_TRUST_CNTL`. It is also never *read* after 5.3 (see risk 5).

**The "zap dump"** = stage 4's CP dump, the bank 0 FSR, CP_HW_FAULT, CP_PROTECT_STATUS and words 0–3 and
1023 of the scratch page. It is logged after every step and on any stop.

#### 5.0 Survey: SCM queries and the files (`sweetzap=0`)

Read-only SMCs, the first SIP calls ours has made:
1. The convention probe, then `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7, then `PAS_IS_SUPPORTED(13)`.
2. Check the MDT blob: ELF magic, class 1, e_phnum 3, the three headers as in the table above, segment 0
   filesz + hash filesz == blob length. Compute min/max/relocate the way `qcom_mdt_pas_init` does, and
   log the `MEM_SETUP` size it gives (want 0x1000).
3. Check the B02 blob: length == segment 2's p_filesz (0x7c0), and p_memsz ≤ 0x2000 (fits the carve-out).
4. Log RBBM_SECVID_TRUST_CNTL (G4 read 0x1) for the last time.
5. Map the carve-out and read its first and last words (it should be ordinary memory: a reboot resets the
   XPU). Allocate the metadata area, copy the MDT, read it back.

✅ Pass when every SMC returns x0 0 with x1 1, the file checks hold, and the copies compare equal. A
`PAS_IS_SUPPORTED` of 0 stops the stage. We would then need a different plan, not a fallback.

*(G5a hit exactly that stop: `PAS_IS_SUPPORTED(13)` = 0. "5.0 revised" just below replaces this rule; patch
0110.)*

#### 5.0 revised after G5a (approved; patch 0110)

**What the stock sunfish kernel does** (android-msm-sunfish-4.14, read for hardware facts only, no code copied):
- The DT (`sdmmagpie-gpu.dtsi`) has `pil_gpu: qcom,kgsl-hyp` with compatible `qcom,pil-tz-generic`, `qcom,pas-id =
  <13>` and `qcom,firmware-name = "a615_zap"`. So PAS 13 and our files are the right ones. The node has no clocks,
  regulators or `qcom,mem-protect-id`.
- `sdmmagpie.dtsi` reserves `pil_gpu_mem: gpu_region@97715000` (`removed-dma-pool`, `no-map`, size 0x2000). I
  did not find which node links to it; the files were cut off.
- kgsl loads the zap shader with `subsystem_get("a615_zap")` in `a6xx_microcode_load`. This runs inside
  `a6xx_rb_start`, after the SQE base is written and **before** `CP_SQE_CNTL = 1`, so the SQE is still halted. The
  zap is skipped when `mmu.secured` is false.
- `subsys-pil-tz.c` **never calls `PAS_IS_SUPPORTED`**. Its command list has only INIT_IMAGE (1), MEM_SETUP (2),
  AUTH_AND_RESET (5) and SHUTDOWN. Mainline `adreno_gpu.c` and `mdt_loader.c` (v6.6) do not call
  `qcom_scm_pas_supported` either. So no known GPU zap loader ever asks this question.
- The SMC calls match ours:
  - INIT_IMAGE: arginfo `SCM_ARGS(2, VAL, RW)`, args (13, metadata pa);
  - MEM_SETUP: arginfo `SCM_ARGS(3)`, args (13, start, size);
  - AUTH_AND_RESET: arginfo `SCM_ARGS(1)`, arg 13.
- The metadata is the whole `.mdt`. Stock copies it into a `dma_alloc_attrs` buffer marked
  `DMA_ATTR_STRONGLY_ORDERED`, with the mask set to the full `dma_addr_t` width (no 4 GiB limit). It frees the
  buffer straight after INIT_IMAGE. There is no hyp_assign of the metadata.
- The region is assigned to a subsystem VMID only when the node has `qcom,mem-protect-id`; `kgsl-hyp` has none.
  The relocatable MEM_SETUP size is the 4 KiB-aligned span, 0x1000, as we have it.
- **Bandwidth vote:** before INIT_IMAGE and before AUTH_AND_RESET, stock calls `scm_pas_enable_bw()`. That is an
  msm-bus vote for `MSM_BUS_MASTER_CRYPTO_CORE_0`, presumably so TZ's hash checks have crypto-to-DDR bandwidth.
  Stock fails INIT_IMAGE if it cannot vote. **We have no such vote.**
- The secure-mode exit is `adreno_set_unsecured_mode`. With a zap handle it emits `cp_secure_mode(0)` on the ring,
  as our 5.4 does. Without a zap handle (only when `mmu.secured` is false) it writes `RBBM_SECVID_TRUST_CNTL = 0`
  directly. The DT sets `qcom,gpu-quirk-secvid-set-once`, which only stops the TSB base/size writes from
  repeating.

**Guess (not proven):** `PAS_IS_SUPPORTED` answers from a table that leaves out PAS 13, or the hypervisor answers
it. Since neither loader asks, a 0 says nothing about whether INIT_IMAGE will accept PAS 13. Only 5.2 can show
that.

**Revised 5.0 (`sweetzap=0`), read-only like before:**
1. The probe and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7, as before.
2. `PAS_IS_SUPPORTED` becomes **log only**, for 13 and, as a baseline, PAS 1 (ADSP), 4 (modem), 9 (video) and
   18 (CDSP). These are mainline's ids; I have not checked them against this phone's DT. If those all read 0 as
   well, the call simply means nothing here. If they read 1 and only 13 reads 0, it is a warning to carry into
   5.2, not a stop.
3. The MDT/B02 checks and the `SECVID_TRUST_CNTL` read, unchanged.
4. Steps 4–5 from above, which G5a never reached: map the carve-out (0x97715000, 0x2000) and read words, then copy
   the metadata and read it back.

✅ Pass when: every SMC returns x0 0; `IS_CALL_AVAIL` gives x1 1 for cmds 1, 2 and 5 (cmd 7 is log only); the file
checks hold; and the copies compare equal. `PAS_IS_SUPPORTED` values are logged and never stop the stage.

**Changes for 5.2–5.3 (same patch, for the later boots):**
- No crypto bandwidth vote is added yet. Display, CPU and GPU traffic already keep DDR busy, so I guess the crypto
  engine still reaches memory, only maybe slower. If 5.2 or 5.3 returns -1, or takes over 1 s, the next plan adds an
  RPMh BCM vote for the crypto master. We already have the cmd-db lookups from stage 0.
- Order: we keep mainline's order (zap after CP init, with the SQE running after 4.5). Stock instead loads it with
  the SQE halted. If AUTH_AND_RESET fails only with a running SQE, a later variant can halt it first with
  `sweet_cp_halt`, call AUTH, then restart.
- Keep `SECVID_TRUST_CNTL` never written. The direct write is stock's no-zap path only, and on this phone the
  register is probably locked (guess).

**Images:** G5a2 = patch 0110, `sweetzap=0`, otherwise the same as G5a. After it passes: G5b (`sweetzap=3`, through
AUTH_AND_RESET), then G5c (6).

#### 5.1 Secure-mode baseline (`sweetzap=1`)

While the GPU is still in secure mode, put one `CP_MEM_WRITE` on the ring: header `0x703d8003` (PKT7 opcode
0x3d, count 3), scratch page pa low, high, value `0xc0ffee00` to word 0. Then a register marker
(0x5ee75001). Wait idle (1 s).

✅ Pass when the CP goes idle, RPTR = WPTR, the marker lands, and no fault is latched. Word 0 is **logged
either way**: 0x5ee7dead confirms their finding (writes are dropped in secure mode), and 0xc0ffee00 means
it does not apply here. Either answer is useful, and only a hang or a fault stops the stage. The test shows
that 5.5's marker can tell the two states apart.

#### 5.2 `PAS_INIT_IMAGE` and `PAS_MEM_SETUP` (`sweetzap=2`)

1. `dsb sy`, then `PAS_INIT_IMAGE(13, metadata pa)`. TrustZone checks the signature on the hash segment
   here.
2. `PAS_MEM_SETUP(13, 0x97715000, 0x1000)`, only if 5.0 found the relocatable flag (it did).

✅ Pass when both return x0 0 and x1 0. On EINVAL_ADDR from INIT_IMAGE, the likely cause is risk 1.

#### 5.3 Load and `PAS_AUTH_AND_RESET` (`sweetzap=3`)

1. Copy B02 into the carve-out at offset 0 as 496 32-bit words (device memory: no unaligned access, no
   cache). Read every word back. Leave the rest of the page as it was (p_memsz = p_filesz, so there is no
   zero-fill). `dsb sy`.
2. `PAS_AUTH_AND_RESET(13)`. Log the time taken.
3. Zap dump.

✅ Pass when the readback matches, AUTH returns x0 0 and x1 0, and nothing new is latched in the CP or
SMMU. The GPU is still in secure mode at this point.

#### 5.4 Leave secure mode (`sweetzap=4`)

Ring: `0x70e60001` (PKT7 `CP_SET_SECURE_MODE` 0x66, count 1), `0x00000000`, then a register marker
(0x5ee75004). Kick, then wait idle, 1 s (mainline's `a6xx_idle`).

✅ Pass when the CP is idle with RPTR = WPTR, the marker lands, and CP_HW_FAULT, CP_PROTECT_STATUS and
bank 0's FSR stay 0. Mainline's comment says this packet hangs the CP without a zap, so a hang here
points at 5.3 even if AUTH returned 0.

#### 5.5 First GPU writes to DRAM (`sweetzap=5`)

1. `CP_MEM_WRITE 0xc0ffee01` to word 0 of the scratch page. Wait idle, `dsb sy`, read the word from the CPU
   (the mapping is non-cacheable, so no invalidate is needed).
2. Then in one submission: `0xc0ffee02` to word 1023 (the end of the page) and 16 writes of
   `0xc0ff1000 + i` to words 16–31.
3. Compare the whole page: the written words hold their values, and every other word is still 0x5ee7dead
   (word 0 may hold 5.1's value only if 5.1 found writes allowed).

✅ Pass when every write lands, nothing else in the page changed, and no fault is latched. **This is the
GPU's first visible write to DRAM**, and it is what fences and Stage 6 build on.

#### 5.6 Hold (`sweetzap=6`, the whole of Stage 5)

For 30 s, every 10 s: `CP_MEM_WRITE 0xc0ff2000 + n` to word 2 and a register marker, then the zap dump, the
GX GDSC and the GMU state. Afterwards leave the SQE running and idle, outside secure mode, with OOB_GPU held.

✅ Pass (Stage 5 as a whole) when every write and marker lands, nothing is latched, RBBM_STATUS is idle
between them, and the phone stays stable.

#### Test boots

One kernel (0109) and three images. All use `sweetgpu=7 sweetgmu=6 sweetcp=5` and the 828xr ADT, and
differ only in `sweetzap`. `shim/out` has no G5* names yet. Each boot needs the owner's OK. Leave the phone
on about 5 minutes.

| Image | `sweetzap` | Beyond G4c | Purpose |
|---|---|---|---|
| G5a | 0 | read-only SMC queries, file checks, metadata copy | TrustZone answers, and supports PAS 13 |
| G5b | 3 | the secure baseline, INIT_IMAGE, MEM_SETUP, the load, AUTH_AND_RESET | TrustZone accepts our zap |
| G5c | 6 | `CP_SET_SECURE_MODE 0`, the DRAM writes, the hold | Stage 5 complete |

Fixes found on G5a or G5b are new patches and CI runs, shown before anything boots.

#### Risks for Stage 5, most uncertain first

1. **TrustZone refuses the metadata buffer.** Ours is above 4 GiB (0x1_0258_b000 on the G4c layout).
   Mainline's coherent buffer is usually below 4 GiB, and some TrustZone builds may want a 32-bit address.
   EINVAL_ADDR (-3) or an XPU error would say so. The fix would be a buffer below 4 GiB, outside XNU's RAM.
   Which range to use is the owner's decision, because Android-era carve-outs live there.
2. **The hypervisor filters SIP calls from our EL1.** PSCI passes through. The PIL calls should too, since
   Android's kgsl makes the same ones. An SMC that never returns stalls one core (G0H: iOS keeps running,
   and the log survives).
3. **Signature or anti-rollback failure in INIT_IMAGE / AUTH_AND_RESET.** The files come from the sunfish
   factory image's `vendor.img`, so the key is right. Nothing confirms that build matches the phone's
   TrustZone version, and an older zap could be refused for rollback. The x0/x1 codes are logged.
4. **CP_SET_SECURE_MODE hangs** even though AUTH passed. Dump, halt the SQE, and stop. The zap normally runs
   from the carve-out, so the first suspect is the load offset (MEM_SETUP base vs p_paddr).
5. **A read of a locked GPU register aborts.** After the zap, `RBBM_SECVID_TRUST_CNTL` may be protected, and
   `CP_AHB_CNTL = 1` (4.1) makes bad AHB accesses return an error, which can become a CPU abort. So 0xf400
   is not read after 5.3.
6. **The 5.1 baseline hangs or faults the CP.** That is unlikely, given their finding that the write is
   just dropped. If it happens, 5.1 becomes optional in a follow-up patch, and G5b is re-run without it.
7. **WAITQ_SLEEP (2) from TrustZone.** Newer firmware uses it, and this 2019 firmware probably does not. We
   stop rather than implement it.

Stage 5 leaves these for later: CP_PROTECT, interrupts, indirect buffers, `CACHE_FLUSH_TS` fences (Stage 6),
and reloading the zap after power collapse (Stage 9).

### Stage 6: kernel GPU service and user client (0111 booted to 6.4; 0111b written, not booted)

Planned 2026-10-26, after G5c passed Stage 5. The owner asked to see this plan before any code.

**Where patch 0111 differs from the plan below** (written 2026-10-26; these override the step text):
- `CP_WAIT_REG_MEM` takes **6** payload dwords (count 6), not 5: function/poll, addr lo, addr hi, ref,
  mask, delay.
- The 6.4 copy IB is 4096 × 6 dwords = **96 KiB**, not 24 KiB. The self-test IBs therefore live in window
  BOs (made through the C API), not in the 4 KiB slots of the IB pool. The pool (16 × 4 KiB) holds only
  the 6.3 and 6.5 test IBs.
- Kernel BOs are `kmem_alloc_contig` memory kept **cacheable** for the CPU and cleaned/invalidated to PoC
  (`flush_dcache64`) before submit and after the fence (`sweet_gsvc_bo_sync`). This replaces the uncached
  alias, because each `ml_io_map` alias would leak kernel VA when a BO is freed.
- The gx_gfx3d clock counter is static in `sweet_gpu.c`, so `sweet_gpu_gfx3d_khz()` is exported from
  there, not from `sweet_gmu.c`. `sweet_gmu_dcvs(index, &ret)` stays in `sweet_gmu.c`.
- 6.5 waits on scratch word **11** and writes word **12**, because 6.3 already uses words 8 and 10.
- In 6.2, if `INT_0_STATUS` bit 20 does not latch and clear, the step logs it as a finding (risk 4) and
  goes on. It is not a stop.

**Patch 0111b (2026-10-27, after G6c)** changes 6.5 and the kill path:
- G6c showed that the hardware `RBBM_HANG_DETECT` (`INT_0_STATUS` bit 23, `HANG_INT_CNTL`
  (1<<30)|0x1fffff) fires during the deliberate `CP_WAIT_REG_MEM`, long before the 2 s software rule. In
  0111 it was fatal, so 6.5 killed the service.
- In 6.5's test mode only, bit 23 is now expected: the poller logs its latency from the submit, clears it
  and counts repeats. It is not added to the error bits. Outside test mode it stays fatal.
- 6.5 drains a leftover bit 23 after the release, before it leaves test mode. The poller reads the test
  flag before the status, so that drain cannot race into a kill.
- `gs_kill` now halts the SQE at once (`sweet_cp_halt_now`, no 2 s `gpu_pause`) and dumps after that.

**Idea.** Turn the CP that Stage 5 left running (outside secure mode, writes reaching DRAM) into a small
service that other code can use. First add what mainline's `hw_init` and `a6xx_submit` do and we skipped:
- register protection (CP_PROTECT);
- the interrupt mask;
- indirect buffers (IBs);
- `CACHE_FLUSH_TS` fences;
- a GPU address window for buffers that are not physically contiguous.

Then:
- a kernel self-test (clear and copy, checked by the CPU);
- a hang detector;
- the clock steps;
- a "service ready" state with a C API.

The user client (selectors 10–15) is a thin IOKit wrapper over that API. It comes last, in its own patch,
and it is the only step that needs a userspace binary.

**Sources, read in this session** (hardware facts only; no code copied):
- Mainline Linux 6.18.54 (the WSL kernel tree), from `drm/msm/adreno`:
  - `a6xx_gpu.c`: `a6xx_submit`, `a6xx_set_cp_protect`, the `hw_init` order (protect at line 1367,
    `CP_SQE_CNTL = 1` at line 1464), `A6XX_INT_MASK`, `a6xx_irq`, `a6xx_fault_detect_irq` and the
    `RBBM_INTERFACE_HANG_INT_CNTL` values;
  - `a6xx_catalog.c`: `a630_protect_regs`, which a618 uses, and gmem 512 KiB;
  - `a6xx_gpu.h`: the `A6XX_PROTECT_*` encodings;
  - `msm_gpu.h`: hangcheck 500 ms × 3 retries.
- `adreno_pm4.xml` (`~/ios-pixel/ref/adreno-xml`): the `CP_EVENT_WRITE` (EVENT 7:0, TIMESTAMP bit 30,
  IRQ bit 31), `CP_MEM_TO_MEM`, `CP_WAIT_REG_MEM` and `CP_INDIRECT_BUFFER` (IB_SIZE 19:0, in dwords)
  layouts, and the event numbers.
- Our `xnu/gpu/regs` headers for every offset below.
- Our own patches:
  - 0100 (the 2B.6 DCVS mailbox: `GMU_DCVS_PERF_SETTING` 0x1cbfd, `_BW_SETTING` 0x1cbfe,
    `_RETURN` 0x1cbff, `OOB_DCVS` 7);
  - 0106 (the CB0 page table);
  - 0107 (the ring helpers);
  - 0046 (the IOKit user-client pattern: `externalMethod`, `createMappingInTask`);
  - 0017 (the GIC: only the virtual timer and the IPI SGI are enabled).
- Their design, for selector numbers only:
  - 10–15 as below;
  - a GPU VA window at 8–16 GiB with 4 KiB pages (their 0137);
  - fences in a page written by `CACHE_FLUSH_TS`.

  Their repo has no licence, so we keep only these numbers and shapes, so that a later sweetgl port maps
  1:1.

**What we already have from Stages 2B–5:**
- OOB_GPU held; GX on at 180 MHz (DCVS perf index 1).
- The SQE running and idle outside secure mode.
- `cp_emit` / `cp_kick` / `cp_wait_idle` / `cp_marker` / `cp_dump` / `cp_clean`.
- The scratch page.
- CB0's identity map: 0x8000_0000–0x1_ffff_ffff, 1 GiB blocks, Normal non-cacheable, IOVA = PA.
- The ring has NO_UPDATE, so RPTR is read from `CP_RB_RPTR`.

**No interrupts in Stage 6 (decided by the hardware state, not a guess).** The GIC enables only the
virtual timer and the IPI SGI (0017). The GPU's SPI 300 (INTID 332) is never enabled, so nothing reaches
the CPU. Stage 6 therefore *polls*:
- The fence page, every 1 ms while work is outstanding.
- `RBBM_INT_0_STATUS`, on the same tick. With `INT_0_MASK` set, the GPU still latches status bits. We
  read them and clear them with `RBBM_INT_CLEAR_CMD`, exactly as `a6xx_irq` does.

Routing SPI 332 through the GIC is left for later (Stage 9 or its own stage), because it changes shared
interrupt code. 6.0 and 6.2 only *read* the GIC distributor bits for INTID 332, as evidence for that later
work.

**Boot-args:**
- `sweetgpu=8` selects Stage 6. It needs `sweetgmu=6 sweetcp=5`, and Stage 5 to pass to 5.5 in the same
  boot. The test images use `sweetzap=5`, which skips 5.6's 30 s hold. `sweetgpu=7` stays exactly as on G5c.
- The new `sweetsvc=N` (0–7, default 0) stops after step 6.N.

**New code:**
- **Patch 0111: kernel only, steps 6.0–6.7.**
  - `sweet_gsvc.c` (new, about 800 lines):
    - the protect and interrupt setup;
    - the submit path (ring lock, the preamble, IBs, the fence);
    - the fence/status poller thread;
    - the hang detector;
    - the VA window (map, unmap, TLB invalidate);
    - the BO table;
    - the self-test;
    - the clock steps;
    - the C API: `sweet_gsvc_bo_new`, `_bo_free`, `_bo_import_pages`, `_submit`, `_wait`, `_info`.
  - `sweet_gmu.c`: export the DCVS vote (perf index in, `DCVS_RETURN` out) and the 0099 clock counter.
  - `sweet_smmu.c`: export CB0's level-1 table pa/va and a `cb0_tlbiall()` helper (`TLBIALL`, `TLBSYNC`,
    then poll `TLBSTATUS`).
  - `sweet_gpu.c`: parse `sweetsvc`. After `sweet_zap_run()` succeeds with `gpu_level >= 8`, call
    `sweet_gsvc_run(stop)`.
- **Patch 0112: the user client (6U).**
  - An IOKit `SweetGPU` service and `SweetGPUUserClient`, about 300 lines of C++ next to 0046, which
    only marshal arguments into the C API.
  - A small static test tool, `sweetgpu-test`, built in CI and ad-hoc signed like `tools/mkinit.py`.
  - 0112 was planned to be written only after 6.7 passes; it was written early at the owner's request
    (2026-10-28, see 6U).

**Memory** (GPU-GMU-MEM is 1 MiB; offsets continue after Stage 5's metadata copy; PAs as on G5c, where
GMU-MEM started at 0x1024f8000 and the metadata copy was 0x10258f000):

| Offset | Size | PA on G5c layout | Use |
|---|---|---|---|
| +0x99000 | 4 KiB | 0x102591000 | fence page: word 0 = kernel ring fence, words 1–3 = last submit/retire/error counters (CPU-written), rest 0 |
| +0x9a000 | 64 KiB | 0x102592000 | kernel IB pool: 16 × 4 KiB IBs for the self-tests (IOVA = PA) |
| +0xaa000 | 4 KiB | 0x1025a2000 | window level-2 table: GPU VA 0x2_0000_0000–0x2_3fff_ffff (512 × 2 MiB) |
| +0xab000 | 128 KiB | 0x1025a3000 | 32 level-3 tables, so **64 MiB of BO space** in Stage 6 (more later by moving the tables out of GMU-MEM) |

- That uses 812 KiB of the 1 MiB. The GMU-MEM mapping is already uncached (Stage 3), so the tables and the
  fence page need no cache maintenance, only `dsb sy` before the GPU uses them.
- **The window.** Level-1 entry 8 of CB0's table (VA 8 GiB; entries 2–7 are the identity blocks) points at
  the level-2 table. Each used 2 MiB slot points at a level-3 table with 4 KiB pages: AP read/write, AF,
  nG, MAIR attribute 0 (Normal non-cacheable, as the identity map). Entry 8 is past DRAM (6 GiB ends at
  0x1_ffff_ffff), so nothing there is in use today. 6.0 reads it and wants it 0.
- **BOs** are allocated from the window with a simple first-fit bitmap over 16384 pages. A BO's GPU VA
  never changes while it lives. After any unmap: `cb0_tlbiall()` with the CP idle or past the BO's last
  fence. After a map, the same (cheap, and it rules out stale walk-cache entries).
- **BO memory** comes from XNU:
  - the 0111 self-tests use `kmem_alloc_contig` (as UFS does in 0028), cleaned and invalidated to PoC
    once, then accessed only through an uncached alias;
  - 0112 uses `IOBufferMemoryDescriptor` mapped write-combined, and imports walk an IOSurface's
    descriptor page by page.

  The GPU side is non-cacheable, so CPU writes need only `dsb sy` before a submit, and CPU reads need
  only the fence.

**Register writes in Stage 6** (CPU, through the GX mapping):
- `CP_PROTECT_CNTL`, `CP_PROTECT_REG(0..31)`;
- `RBBM_INT_0_MASK`, `RBBM_INT_CLEAR_CMD`, `RBBM_INTERFACE_HANG_INT_CNTL`;
- the doorbell `CP_RB_WPTR`;
- the DCVS mailbox (6.6 only);
- CB0's `TLBIALL` / `TLBSYNC` in the SMMU;
- `CP_SQE_CNTL = 0` only when the hang detector turns the service off.

**Never** written: `RBBM_SECVID_TRUST_CNTL` (and never read, as in Stage 5), any GMU power register, and
the CB0 identity entries 2–7.

**The ring preamble per submit** (as `a6xx_submit`, without the stats counters). Every header word comes
from 0107's `pkt4()` / `pkt7()` with their parity bits, so none are quoted here:
1. `CP_EVENT_WRITE` (0x46, count 1): `PC_CCU_INVALIDATE_DEPTH` (0x18).
2. `CP_EVENT_WRITE` (0x46, count 1): `PC_CCU_INVALIDATE_COLOR` (0x19).
3. Per IB: `CP_INDIRECT_BUFFER` (0x3f, count 3): iova low, iova high, size in dwords.
4. PKT4 `CP_SCRATCH_REG(2)` (0x885) = seqno (mainline's "fence in scratch 2", useful in a dump).
5. `CP_EVENT_WRITE` (count 4): dword 0 = `CACHE_FLUSH_TS (4) | IRQ (bit 31)` = `0x80000004`, then fence
   pa low, high, seqno. Then the doorbell.

**The "svc dump"** is the zap dump plus:
- `RBBM_INT_0_STATUS`, `CP_IB1_BASE`/`REM_SIZE`, `CP_IB2_BASE`/`REM_SIZE`, `CP_SCRATCH_REG(2)`;
- fence word 0 and the last seqno sent;
- the GICD enable/pending bits for INTID 332.

It is logged after every step and on any stop.

#### 6.0 Survey and setup, no new GPU work (`sweetsvc=0`)

1. Read and log `CP_PROTECT_CNTL` and all 32 `CP_PROTECT_REG`s, `RBBM_INT_0_MASK`, `RBBM_INT_0_STATUS`,
   `RBBM_INTERFACE_HANG_INT_CNTL`, `CP_SCRATCH_REG(0..7)` and `CP_ALWAYS_ON_COUNTER`. These are new
   reads, so their values before we write them are the record.
2. Read GICD `ISENABLER10`/`ISPENDR10` bit 12 (INTID 332) through a read-only `ml_io_map` of the
   distributor. Want enabled 0.
3. Read CB0's level-1 entry 8. Want 0.
4. Zero the fence page, the IB pool and the window tables, then `dsb sy`.
5. Allocate one 16 KiB self-test BO (`kmem_alloc_contig`) and log its pa.

✅ Pass when every read completes without an abort, entry 8 is 0, INTID 332 is not enabled, and nothing
is latched in the CP or the SMMU.

#### 6.1 Register protection and the interrupt mask (`sweetsvc=1`)

With the CP idle (RPTR = WPTR, RBBM_STATUS idle):

1. `CP_PROTECT_CNTL = 0xb`: `ACCESS_PROT_EN` (bit 0), `ACCESS_FAULT_ON_VIOL_EN` (bit 1),
   `LAST_SPAN_INF_RANGE` (bit 3).
2. The 32 `a630_protect` entries below. a630 has count = count_max = 32, so entry 31 is both "the last"
   and the infinite one. Encoding: RDONLY = `len << 18 | reg`, NORDWR = the same | bit 31.
3. `RBBM_INTERFACE_HANG_INT_CNTL = 0x401fffff` (mainline's default branch, which a618 takes).
4. `RBBM_INT_0_MASK = 0x03d0e242` (mainline's `A6XX_INT_MASK`).
5. Read every register back.
6. Then on the ring: a register marker (0x5ee76001 to `CP_SCRATCH_REG(7)` = 0x88a, which is not
   protected), and `CP_MEM_WRITE 0xc0ff6001` to scratch-page word 4. Wait idle.

| # | Register | Kind | Base | Len | Value |
|---|---|---|---|---|---|
| 0 | 0x850 | RDONLY | 0x00000 | 0x04ff | 0x13fc0000 |
| 1 | 0x851 | RDONLY | 0x00501 | 0x0005 | 0x00140501 |
| 2 | 0x852 | RDONLY | 0x0050b | 0x02f4 | 0x0bd0050b |
| 3 | 0x853 | NORDWR | 0x0050e | 0x0000 | 0x8000050e |
| 4 | 0x854 | NORDWR | 0x00510 | 0x0000 | 0x80000510 |
| 5 | 0x855 | NORDWR | 0x00534 | 0x0000 | 0x80000534 |
| 6 | 0x856 | NORDWR | 0x00800 | 0x0082 | 0x82080800 |
| 7 | 0x857 | NORDWR | 0x008a0 | 0x0008 | 0x802008a0 |
| 8 | 0x858 | NORDWR | 0x008ab | 0x0024 | 0x809008ab |
| 9 | 0x859 | RDONLY | 0x008de | 0x00ae | 0x02b808de |
| 10 | 0x85a | NORDWR | 0x00900 | 0x004d | 0x81340900 |
| 11 | 0x85b | NORDWR | 0x0098d | 0x0272 | 0x89c8098d |
| 12 | 0x85c | NORDWR | 0x00e00 | 0x0001 | 0x80040e00 |
| 13 | 0x85d | NORDWR | 0x00e03 | 0x000c | 0x80300e03 |
| 14 | 0x85e | NORDWR | 0x03c00 | 0x00c3 | 0x830c3c00 |
| 15 | 0x85f | RDONLY | 0x03cc4 | 0x1fff | 0x7ffc3cc4 |
| 16 | 0x860 | NORDWR | 0x08630 | 0x01cf | 0x873c8630 |
| 17 | 0x861 | NORDWR | 0x08e00 | 0x0000 | 0x80008e00 |
| 18 | 0x862 | NORDWR | 0x08e08 | 0x0000 | 0x80008e08 |
| 19 | 0x863 | NORDWR | 0x08e50 | 0x001f | 0x807c8e50 |
| 20 | 0x864 | NORDWR | 0x09624 | 0x01db | 0x876c9624 |
| 21 | 0x865 | NORDWR | 0x09e70 | 0x0001 | 0x80049e70 |
| 22 | 0x866 | NORDWR | 0x09e78 | 0x0187 | 0x861c9e78 |
| 23 | 0x867 | NORDWR | 0x0a630 | 0x01cf | 0x873ca630 |
| 24 | 0x868 | NORDWR | 0x0ae02 | 0x0000 | 0x8000ae02 |
| 25 | 0x869 | NORDWR | 0x0ae50 | 0x032f | 0x8cbcae50 |
| 26 | 0x86a | NORDWR | 0x0b604 | 0x0000 | 0x8000b604 |
| 27 | 0x86b | NORDWR | 0x0be02 | 0x0001 | 0x8004be02 |
| 28 | 0x86c | NORDWR | 0x0be20 | 0x17df | 0xdf7cbe20 |
| 29 | 0x86d | NORDWR | 0x0f000 | 0x0bff | 0xaffcf000 |
| 30 | 0x86e | RDONLY | 0x0fc00 | 0x1fff | 0x7ffcfc00 |
| 31 | 0x86f | NORDWR | 0x11c00 | (infinite) | 0x80011c00 |

Notes:
- Entry 6 covers 0x800–0x882 (ring registers, `CP_PROTECT_*` itself). The scratch registers 0x883–0x88a
  are just past it, which is why our markers still work. CP_PROTECT only filters the *CP's* register
  accesses (ring and IB packets), not the CPU's AHB writes, so the doorbell is unaffected.
- `RBBM_INT_0_MASK` bits: CP_AHB_ERROR 1, ATB_ASYNCFIFO_OVERFLOW 6, CP_HW_ERROR 9, CP_IB2 13, CP_IB1 14,
  CP_RB 15, CP_CACHE_FLUSH_TS 20, ATB_BUS_OVERFLOW 22, HANG_DETECT 23, UCHE_OOB_ACCESS 24,
  UCHE_TRAP_INTR 25.
- Mainline writes all of this **before** `CP_SQE_CNTL = 1`. Ours writes it with the SQE running but idle
  (risk 1).
- No violation test in Stage 6: a fault-on-violation stops the CP, and we have no GPU reset yet.

✅ Pass when every register reads back as written, the marker and the write land, and `CP_HW_FAULT`,
`CP_PROTECT_STATUS` and `CP_INTERRUPT_STATUS` stay 0.

#### 6.2 Fences: `CACHE_FLUSH_TS` (`sweetsvc=2`)

1. Ring: `CP_SCRATCH_REG(2) = 1`, then the 4-dword `CACHE_FLUSH_TS` (with IRQ) writing 1 to fence word 0.
   Kick. Poll the fence word from the CPU every 10 µs, up to 1 s, and log the time.
2. Read `RBBM_INT_0_STATUS`. Want bit 20 (CACHE_FLUSH_TS), and perhaps bits 15 (CP_RB) and 14. Clear it
   with `RBBM_INT_CLEAR_CMD` = the value read, and read 0 back.
3. Log the GICD pending bit for INTID 332 (want 1: the line asserted at a disabled interrupt; harmless).
4. 64 fences back to back, seqnos 2–65, one kick. Want fence word 0 = 65, monotonic samples, and RPTR = WPTR.
5. Start the **poller**: a kernel thread that, every 1 ms while `last_sent != fence`, reads the fence word
   and `RBBM_INT_0_STATUS`.
   - It clears the latched bits.
   - It wakes waiters on each new fence.
   - It logs (rate-limited) any error bit: 1, 6, 9, 22, 23, 24, 25.

   With no work outstanding it sleeps until the next submit.

✅ Pass when every fence lands in order, the first within 1 ms, `INT_0_STATUS` latches and clears as
expected, and nothing else is latched.

#### 6.3 Indirect buffers and the GPU VA window (`sweetsvc=3`)

1. **Kernel IB, identity address.** IB 0 in the pool: `CP_MEM_WRITE 0xc0ff6301` to scratch word 5, and a
   PKT4 marker 0x5ee76301 to `CP_SCRATCH_REG(6)`. Submit it with the preamble and fence 66. Want the word,
   the marker and the fence.
2. **A long IB.** IB 1: 256 `CP_MEM_WRITE`s of `0xc0ff6400 + i` to scratch words 256–511, about 1 KiB of
   commands. Want all 256 words, and nothing else in the page changed.
3. **Two IBs in one submit**, then **16 submits queued** before one kick (ring space check: never let WPTR
   pass RPTR − 1).
4. **The window.**
   - Write CB0 level-1 entry 8 → the level-2 table, then `cb0_tlbiall()`.
   - Map 6.0's 16 KiB BO at GPU VA 0x2_0000_0000 (4 pages, possibly scattered).
   - IB: `CP_MEM_WRITE`s through the *window* VA to the first and last word of each page. The CPU reads
     them back through the BO's own mapping.
5. **Unmap and remap.**
   - Unmap page 2, `cb0_tlbiall()`, map a *different* page at the same VA, and write through it again.
   - Want the new page written and the old page untouched.
   - This proves the TLB invalidate. Without it, the write would land in the old page.

✅ Pass when every write and fence lands, the window writes land in the right physical pages,
`CP_IB1_REM_SIZE` reads 0 when idle, and the SMMU bank 0 FSR stays at the 0x400 baseline (no translation
fault).

#### 6.4 Self-test: clear and copy (`sweetsvc=4`)

Two 64×64 RGBA8 "surfaces" (16 KiB each) as window BOs A and B, both poisoned by the CPU with 0x5ee7dead.
1. **Clear.** One IB: a single `CP_MEM_WRITE` packet with 4096 data dwords of `0xff336699` into A.
2. **Copy.** One IB: 4096 `CP_MEM_TO_MEM` packets (count 5: flags 0, dst B+4i, src A+4i), 24 KiB of
   commands, with `WAIT_FOR_MEM_WRITES` (bit 30) on the first packet, so the copy waits for the clear.
3. **Pattern copy.** The CPU fills A with `(y << 16) | x`, then the GPU copies A to B again.
4. The CPU compares A and B after each step and counts bad pixels. It logs the time of each IB using
   `CP_ALWAYS_ON_COUNTER` (19.2 MHz) read before the kick and after the fence.

This uses the CP, not the 2D blitter. `CP_BLIT` and the `RB_2D_*` / `GRAS_2D_*` path need about 30
registers of blitter state and CCU flushes. That is real rendering work, and it moves to Stage 7, where
sweetgl's blitter code sets it up. The CP path still exercises everything the service owns: BOs, the
window, IBs, ordering and fences.

✅ Pass when all three checks report **0 bad pixels** and every fence lands.

#### 6.5 Hang detector (`sweetsvc=5`)

The rule:
- If `last_sent != fence` and neither the fence word nor `CP_RB_RPTR` / `CP_IB1_REM_SIZE` has changed for
  **2 s**, the GPU is hung.
- The detector then logs the svc dump, writes `CP_SQE_CNTL = 0`, marks the service **dead** until reboot,
  and fails every waiter (`kIOReturnNotResponding` in 0112).

2 s matches mainline's order of magnitude (500 ms × 3 progress retries ≈ 1.5 s). An error bit in
`INT_0_STATUS` (CP_HW_ERROR 9, HANG_DETECT 23, UCHE_OOB 24) also kills the service at once.

Test, without losing the CP for later steps:
1. Scratch word 8 = 0. Submit an IB with `CP_WAIT_REG_MEM` (count 5: FUNCTION = EQ (3), POLL = memory
   (bit 4), addr = scratch word 8, ref 1, mask 0xffffffff, delay 16), then `CP_MEM_WRITE 0xc0ff6501` to
   word 9, then the fence.
2. The CP now waits for the memory. The detector runs in **test mode** for this one submit. It must report
   "hang detected" between 2.0 and 2.1 s, then log "test mode: not stopping".
3. The CPU writes 1 to word 8 and `dsb sy`. The CP must resume: word 9 and the fence land within 10 ms.
4. Then a normal submit, to show that nothing stuck.

✅ Pass when the detector fires in the window, the CP resumes and finishes after the release, and no error
bit latches. Since 0111b, the hardware `RBBM_HANG_DETECT` (bit 23) must also fire during the wait; its
latency is logged, and it is not counted as an error bit in test mode. If the CP does *not* resume, the real rule runs (SQE halted, service dead), the step fails,
and the image's later steps are skipped.

#### 6.6 Clock steps (`sweetsvc=6`)

Through 2B.6's DCVS path (mailbox `GMU_DCVS_PERF_SETTING = 0x30000000 | index`, `DCVS_BW_SETTING`
0x000ff0ff = no change, `OOB_DCVS`, then `DCVS_RETURN`).

Perf indices follow the 2B.5 table order, 1 = 180 MHz (measured on G3k):
- 2 = 267, 3 = 355, 4 = 430, 5 = 565, 6 = 650, 7 = 700 MHz.
- This order is a guess until 6.6 measures it.

The GMU votes the matching `gfx.lvl` corner itself from the perf table (as it did for 180 MHz). We send
no ARC vote of our own.

1. Index 4. Measure `gx_gfx3d` with the 0099 counter (want 430 MHz ± 1 %). Run the 6.4 self-test and time
   it.
2. Index 5. Measure (want 565 MHz ± 1 %), then the self-test again.
3. Stay at 565 MHz for 6.7. 650 and 700 MHz are not tried in Stage 6: they need ACD, and their thermal
   behaviour is unknown.

The DDR bus vote stays "no change". A BW index is added (a separate, planned step) only if the copy is
*not* faster at 565 than at 430, which would point at memory bandwidth.

✅ Pass when `DCVS_RETURN` is 0 and the measured clock matches at both steps, the self-tests report 0 bad
pixels, and GX stays on. If 565 fails, fall back to index 4 and record it. That is still a pass for
Stage 6, with `max_freq` 430 MHz.

#### 6.7 Service ready and hold (`sweetsvc=7`, the whole kernel part of Stage 6)

1. Log `SWEET GSVC: service ready` with the info block:
   - chip_id 0x06010800, gmem_size 0x80000, gmem_base 0x100000;
   - cur/max freq;
   - the fence page pa, the window base and size.
2. For 60 s, every 10 s, run the 6.4 self-test **through the C API only** (`bo_new` ×2, `submit`, `wait`,
   `bo_free` ×2). This is the same path the user client takes. After each run, log the svc dump, GX GDSC,
   GMU state and the BO bitmap (want every page free again after each round).
3. Leave the service up and idle.

✅ Pass (Stage 6 kernel part) when "service ready" is logged, every self-test round reports 0 bad pixels,
no BO pages leak, nothing latches, and the phone stays stable.

#### 6U User client and userspace test (patch 0112, after 6.7 passes)

**Patch 0112 written early** (2026-10-28, at the owner's request while CI was blocked; not built in CI, not
booted). Where it differs from the text below:
- `SweetGPU` matches IOResources `SweetGPUReady`. `sweet_gsvc_run` publishes that only **after** 6.7's hold
  passed, not when the service becomes ready, because the 6.7 self-tests require an empty window.
- BO memory is an `IOBufferMemoryDescriptor` (write-combined) cleaned and invalidated to PoC page by page
  once, because the pages were zeroed through cacheable aliases. Then it is imported with
  `sweet_gsvc_bo_import_pages`. BO sizes round up to whole 16 KiB XNU pages.
- Imported IOSurfaces may be CPU-cacheable, so the client cleans and invalidates them before each of its
  submits and after each of its successful waits. Between submit and wait the CPU must not touch them.
- The info struct adds `abi` (1), `window_base` and `window_size` (56 bytes). Wait timeouts are capped at
  10 s. A BO whose free times out (the GPU may still use it) is leaked, never reused.
- `sweetgpu-test` (`tools/sweetgpu/sweetgpu-test.c`) is a normal dynamically linked iOS binary built with
  Xcode in `xnu.yml` and signed with `codesign -s -`. It is not a static mkinit-style binary. The iPhoneOS
  SDK has no IOKit headers, so it declares the IOKit and IOSurface calls it uses. Locally it was checked
  only against stub headers.
- Delivery to the phone is still the owner's choice (options below).

A `SweetGPU` IOService, matched by name and published only when the service is ready, with user client
`SweetGPUUserClient`. Selectors as in their design. Our argument layout:

| Sel | Name | In | Out | Notes |
|---|---|---|---|---|
| 10 | bo new | scalar [size, flags] | scalar [handle, gpu_va, user_va] | pages from `IOBufferMemoryDescriptor`, user map write-combined; size ≤ 16 MiB per BO |
| 11 | bo free | scalar [handle] | — | unmapped from the window only after the BO's last fence |
| 12 | bo import IOSurface | scalar [surface id] | scalar [handle, gpu_va, size] | via `SweetSurface` lookup (0046); pages walked from its descriptor |
| 13 | submit | struct {count ≤ 16; {handle, offset, size_dw} × count} | scalar [seqno] | each IB checked to lie inside its BO; the kernel builds the preamble and fence |
| 14 | wait | scalar [seqno, timeout_ms] | — | success, `kIOReturnTimeout`, or `kIOReturnNotResponding` once the service is dead |
| 15 | info | — | struct {chip_id, gmem_size, gmem_base, cur_khz, max_khz, fence_user_va, last_retired, flags} | the fence page mapped read-only into the task |

- Selectors 0–9 and above 15 return `kIOReturnUnsupported`, logged once each.
- Closing the client waits for its outstanding seqnos (2 s cap), then frees its BOs.

**Security, and why 6U is a debug interface only.** CB0 maps all DRAM 1:1, and user IBs run in CB0. So
any process that can submit can read and write *all physical memory* through the GPU. CP_PROTECT (6.1)
only protects GPU registers. So in Stage 6:
- the client opens only for uid 0;
- it opens only when booted with `sweetgpu=8`;
- every open is logged.

Per-process GPU page tables (`CP_SMMU_TABLE_UPDATE`, a user context without the identity map) are required
before any app may use the GPU. They are an open item for Stage 7/9, not a Stage 6 step.

**The test tool** (`sweetgpu-test`, static, ad-hoc signed):
- `info`;
- 6.4's clear/copy through selectors 10/13/14;
- an IOSurface import plus a clear of it;
- a bad submit (IB outside its BO), which must be refused with no GPU work;
- printing "0 bad pixels".

How it reaches the phone is the **owner's decision**, because it changes the iOS root on userdata.
Options:
- (a) a LaunchDaemon added by the root tweaks;
- (b) the tool in the boot payload's RAM disk, run before the root mounts.

✅ Pass when the tool prints the info block, 0 bad pixels for both the BO and the IOSurface, the bad submit
is refused, and the kernel log shows no error bits.

#### Test boots

One kernel (0111) for G6a–G6d, then 0112 for G6e. All use `sweetgpu=8 sweetgmu=6 sweetcp=5 sweetzap=5` and
the 828xr ADT, and differ only in `sweetsvc`. `shim/out` has no G6* names yet. Each boot needs the owner's
OK. Leave the phone on about 5 minutes, then pull the log as for G5c.

| Image | `sweetsvc` | Beyond G5c | Purpose |
|---|---|---|---|
| G6a | 1 | survey, buffers, CP_PROTECT, INT mask | protection set while the SQE runs; first look at INTID 332 |
| G6b | 3 | fences, IBs, the VA window, TLB invalidate | the submit path works end to end |
| G6c | 5 | self-test, hang detector | 0 bad pixels; detector fires and the CP survives |
| G6d | 7 | 430/565 MHz, service ready, 60 s hold | Stage 6 kernel part complete |
| G6e | 7 + 0112 | user client and the test tool | after the owner picks how the tool is delivered |

Steps 6.0, 6.2, 6.4 and 6.6 have no image of their own: the next image runs through them, and its log
shows each step's result. If an image stops early, the fix is a new patch and CI run, shown before anything
boots. A smaller `sweetsvc` value can then be used to isolate the step.

#### Risks for Stage 6, most uncertain first

1. **CP_PROTECT written while the SQE runs.**
   - Mainline only programs it with the SQE stopped (in `hw_init`, and again after a reset).
   - The CP may ignore the change, latch a protect fault on a later packet, or need CP_ME_INIT again.
   - Fallback: move the writes into Stage 4's `cp_init_regs` (before `CP_SQE_CNTL = 1`) in a follow-up
     patch. That changes the order of G4/G5, so they get re-run.
2. **The window's TLB invalidate.**
   - `TLBIALL`/`TLBSYNC` on CB0 are new SMMU writes. Stage 3 programmed CB0 freely, so the hypervisor
     probably allows them (guess).
   - If `TLBSTATUS` never clears, the window is still usable for map-once BOs. Unmap-and-reuse then waits
     for a later stage.
3. **The CP does not resume after `CP_WAIT_REG_MEM`** (6.5). Possible causes: the memory poll goes through
   the UCHE and caches the old value, or the delay field means something else. The service dies as designed
   and G6c ends there. A fix would be a short `delay`, or a register poll (`POLL` = register) on a scratch
   register.
4. **`RBBM_INT_0_STATUS` does not latch without a live interrupt line.** Then the fence poller still works.
   Only the error-bit check goes blind. We would rely on `CP_HW_FAULT`, `CP_INTERRUPT_STATUS` and the
   2 s rule instead, and record it.
5. **565 MHz needs a bus or ACD vote**, or `DCVS_RETURN` is non-zero. Stay at 430 MHz (the pass rule
   allows it), and plan the BCM vote separately.
6. **CPU/GPU coherency.**
   - The kernel BOs have a cacheable alias from `kmem_alloc_contig`. We clean and invalidate once, then
     use only the uncached alias.
   - A missed `dsb sy` or a stale line would show as bad pixels in 6.4, never as a hang. Pixels holding
     0x5ee7dead mean "GPU write lost"; others mean "CPU read stale".
7. **Throughput.** `CP_MEM_TO_MEM` moves one dword per 6-dword packet. 16 KiB per copy is a few
   milliseconds at most, so it is too small to matter. It only means the timing numbers measure the CP, not
   memory bandwidth.

Stage 6 leaves these for later:
- the GIC route for SPI 332 (real interrupts);
- per-process page tables (needed before untrusted users);
- the 2D blitter and shaders (Stage 7);
- a GPU reset path for a dead service (today: reboot);
- 650/700 MHz with ACD;
- BOs over 64 MiB in total;
- power collapse (Stage 9).

### Stage 7: sweetgl for a618 (needs the licence decision)

1. **Build.** Add a GitHub Actions macOS job next to the kernel build:
   - Mesa at a pinned release, freedreno only, built with the iphoneos SDK, cached between runs.
   - The artifact is `sweetgl.dylib`, zero-padded to 64 MiB.
2. **msm emulation for a6xx:**
   - GPU_ID 618 and CHIP_ID 0x06010800; check both against Mesa's `freedreno_devices.py`.
   - GMEM 512 KiB.
   - Answer the params a6xx asks for: highest bank bit 14, UBWC swizzle / macrotile, UCHE trap base,
     priorities, timestamp from the always-on counter.
   - Do not report `VA_START`, so Mesa uses kernel IOVAs.
   - Start with `FD_MESA_DEBUG=nogmem,noubwc` (linear only).
3. **mesa.patch.** The pitch-alignment hunk is `is_a5xx`-only. Check a6xx's rules for imported linear
   IOSurfaces (828 px × 4 = 3312 B, not 64-aligned unless our SweetSurface pads rows) and extend the hunk
   or pad surface rows.
4. **EAGL shim.**
   - It is mostly GPU-agnostic.
   - Mesa's a6xx driver should advertise framebuffer fetch (check `PIPE_CAP_FBFETCH` on the pinned
     release). Test whether CA's `gl_LastFragData` shaders compile natively before carrying over the
     texture-copy emulation.
5. **Kernel plumbing** (our own code):
   - `sweetenv=` boot-arg (exec-time env injection, several processes).
   - Answer dyld's AMFI op **0x5a** in our 0092 handler.
   - An `IOAcceleratorES` stub without `MetalPluginName`.
6. **Test order:**
   1. A standalone test binary (`SWEETGL_TEST=1`-style offscreen triangle, read back).
   2. `backboardd` with `DYLD_INSERT_LIBRARIES`.
   3. Check SpringBoard, CC toggles, App Library plates and blur.
7. **Deployment.** Putting `/usr/lib/sweetgl.dylib` on the root is a root edit: pull, journal replay,
   `fsck.hfsplus -fy`, edit, push. That is **destructive and needs the owner's OK** at that time. Later
   updates are same-size in-place block writes, like `dscpatch.ps1`.

✅ Pass when the home screen renders through Mesa, with `CA_*` debug showing the GLES context.

#### Stage 7 planned in detail (2026-10-28, option 2: all our own code; nothing built)

Facts from Mesa 26.2.5 (read locally, MIT):

- On darwin `system_has_kms_drm` is false, so Mesa sets `dep_libdrm = null_dep` and does not build
  `src/freedreno/drm`. We supply our own libdrm subset and a small meson change.
- libdrm calls freedreno makes: `drmCommandWriteRead` (13), `drmIoctl` (6), `drmCommandWrite` (6),
  `drmGetVersion`/`drmFreeVersion`, `drmGetCap`, `drmOpenWithType`, the Prime and Syncobj calls (fence
  fds; stub them as unsupported).
- msm ioctls: GET_PARAM, SET_PARAM, GEM_NEW, GEM_INFO (7 call sites), GEM_CPU_PREP, GEM_MADVISE,
  GEM_SUBMIT, WAIT_FENCE, SUBMITQUEUE_NEW/QUERY/CLOSE. Params asked: CHIP_ID, GPU_ID, GMEM_SIZE,
  GMEM_BASE, MAX_FREQ, TIMESTAMP, PRIORITIES, FAULTS, SUSPENDS, UCHE_TRAP_BASE, VA_SIZE, SYSPROF.
- **Framebuffer fetch** (which CA's blend-filter layers need) is enabled only when
  `fd_device_version() >= FD_VERSION_GMEM_BASE`, so our `drmGetVersion` must report an msm minor at
  least that high.
- `os_mmap` is a macro in `src/util/os_mman.h`; BO mapping goes through it.
- `src/gallium/frontends/hgl` (Haiku) is the smallest existing GL frontend and the model for ours.

Steps (each ends in something testable; 7.0, 7.4 and 7.6 need no phone and can run in parallel):

- **7.0 Mesa build job.** A macOS CI job (separate workflow, so kernel runs don't wait on it): Mesa
  26.2.5 at a pinned commit, our own meson cross file for iphoneos arm64, `-Dgallium-drivers=freedreno
  -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled
  -Dgles1=disabled -Dgles2=enabled -Dllvm=disabled -Dshader-cache=disabled -Dzstd=disabled
  -Dexpat=disabled -Dbuild-tests=false`, static, plus our patch (meson: build freedreno/drm on darwin
  against our libdrm subset; `os_mmap` → our map hook). Cache the build. ✅ when it links and
  `nm` shows the freedreno screen entry points. Exact option names are checked against
  `meson.options` when writing it.
- **7.1 `sweetdrm`** (our libdrm subset + msm emulation) over the SweetGPU selectors 10–16:
  GEM_NEW → bo new, GEM_INFO → iova and a map cookie, CPU_PREP / WAIT_FENCE → wait, GEM_SUBMIT →
  submit (a6xx submits are iova-based, so no relocations), SUBMITQUEUE_* → fake ids, MADVISE → ok,
  GET_PARAM from `info` (chip 0x06010800, GPU_ID 618, GMEM 0x80000 at 0x100000). TIMESTAMP needs the
  always-on counter: a new selector or a field on the fence page (kernel patch). ✅ a host-side unit
  test against a fake SweetGPU, then on the phone in 7.2.
- **7.2 Offscreen test binary.** Gallium directly (no GL frontend yet): create the screen through
  sweetdrm, clear and draw a triangle into a linear resource, read back, report through sel 16 as
  sweetgpu-test does. `FD_MESA_DEBUG=nogmem,noubwc` first. Deploying it is a root edit (owner OK).
  - **Result: PASSED on the phone (G6l, 2026-10-29 21:36 boot, root v6l, sweetgl run 40860677622).**
    Screen "FD618" by freedreno; clear: 0 of 4096 pixels wrong; triangle: 0 of 4096 wrong, 2016 red
    (want 2048 ± 64); "7.2 sweetgl-tri: pass"; close: last seqno 130 retired, 6 BOs freed, 0 leaked.
    Kernel checks and sweetgpu-test passed alongside.
  - Two fixes were needed. (1) Without a GL frontend nothing sets up Mesa's GLSL type cache, and the
    driver's built-in shaders crashed in `glsl_array_type` during `context_create`; the tool calls
    `glsl_type_singleton_init_or_ref()` itself. (2) launchd gave the job a 6 MB jetsam limit (active
    non-fatal, inactive fatal) and ignored `JetsamProperties` in launchd.plist; the footprint is 13 MB
    after `context_create`, so jetsam killed it (reason 7, per-process limit). The tool now raises its
    own limit to 2048 MB with `memorystatus_control` (root may) and logs footprint and limit.
  - Leftovers: the `JetsamProperties` key sweetgpu-root.py adds has no effect (harmless); the kernel
    warns that the SDK's `TASK_VM_INFO_COUNT` (90) is newer than the kernel's (89), which is harmless.
    7.3 needs the same two fixes in whatever process hosts the GL frontend.
- **7.3 GL frontend** modelled on hgl: a surfaceless GLES context, rendering only into FBOs, the GL
  entry points exported for the EAGL shim. ✅ the 7.2 test redone through GL calls.

  **7.3 PASSED on the device (G6m, booted 2026-10-29 23:59, root v6m; sweetgl run 40870346392, 47320f2).**
  The test's report lines in the kernel log: `GL_VERSION "OpenGL ES 3.2 Mesa 26.2.5"`, `GL_RENDERER "FD618"`,
  GLSL ES 3.20; FBO complete; clear 0 of 4096 pixels wrong; GLSL ES triangle 0 of 4096 wrong, 2016 red
  (as 7.2); "7.3 sweetgl-gltri: pass"; close: last seqno 131 retired, 0 leaked. Footprint 1 MB after the
  screen, 15 MB with a current context, 22 MB at the end. Kernel stages 0-6, sweetgpu-test and the 8a.2
  GPU copies (0 rows differ) all passed in the same boot, no panic.

  How it is built: `tools/sweetgl/frontend/`: `sweetgl.h`/`sweetgl.c`
  wrap the freedreno screen in a `pipe_frontend_screen` and create `API_OPENGLES2` st contexts with a
  `PIPE_FORMAT_NONE` visual (no `gl_config`), made current with `st_api_make_current(st, NULL, NULL)`,
  i.e. Mesa's incomplete framebuffer, so they draw only into FBOs. `build.sh` makes `libsweetgl.a`:
  the frontend, the renderonly stub (moved from `test/`) and Mesa's GLESv2 entry points
  (`es2api/libgles2_public.c` with the header from `gen_gldispatch_mapi.py glesv2`, which meson skips
  without EGL/GLX). `mesa/build.sh` now also builds `glapi` (shared-glapi `core.c`: the TLS dispatch
  and current-context pointers libmesa calls). `test/sweetgl-gltri.c` is 7.2's checks through GLES
  3.0: RGBA8 renderbuffer FBO, `glClear` + `glReadPixels`, then a GLSL ES 3.00 triangle; boot-arg
  `sweetglgltri=1`, job `com.sweet.gl-gltri`, `GALLIUM_THREAD=0` by default, and it raises its own
  jetsam limit as 7.2 had to. The GLSL type cache is now set up by Mesa's own `_mesa_initialize`.
  The link also needs Mesa's `glapi`, `glcpp` (static library) and `vtn` libraries, now in `mesa/build.sh`.
- **7.4 EAGL research** (local only; nothing from Apple goes into git): disassemble QuartzCore and
  OpenGLES from our root to list exactly what `CA::OGL::GLESContext` calls on `EAGLContext`, how it
  gets the GL dispatch, binds IOSurfaces and presents. Output: our own interface notes, which define
  7.5.
- **7.5 EAGL shim**, written from the 7.4 notes. ✅ when sweetgl-eagltri passes on the device.

  *Written; sweetgl run 40887402788 (04dd3e4) failed (freedreno_gmem.h needs u_inlines.h first); fixed in
  8ae857f, run 40888280162 passed. Root v6n written to userdata 2026-10-30; boot G6n the same day: **7.5 passed on the device**
  (clear, triangle with fence and IOSurface texture each 0 of 4096 pixels wrong through the EAGLContext
  shim; the unknown-slot stub logged and returned 0; 6 BOs freed, 0 leaked).*

  How it is built: `tools/sweetgl/eagl/`. `sweeteagl.m` (no ARC) does not link OpenGLES: once the
  `EAGLContext` class is in the process, `sweeteagl_install()` (also run from the dylib's constructor)
  sets our implementations on `initWithAPI:` / `initWithAPI:properties:` / `initWithAPI:sharegroup:`,
  `dealloc`, `+setCurrentContext:`, `+currentContext`, `API`, `getMacroContextPrivate`,
  `setParameter:to:` / `getParameter:to:`, `attachImage:toCoreSurface:invertedRender:`, both
  `texImageIOSurface:...plane:[invert:]`, and (logged, NO) `renderbufferStorage:fromDrawable:` and
  `presentRenderbuffer:`.
  - Each context is a sweetgl context (ES 3.2 for API 2 or 3; API 1 refused) and a calloc'd `_private`
    block with the EAGL_NOTES layout (`sweeteagl.h` asserts the offsets 0x20, 0x28, 0x2088, 0x40e8),
    stored through the `_private` ivar's runtime offset. The current context is TSD slot 30
    (`tpidrro_el0`), so OpenGLES's own gl* exports reach the same table.
  - `sweeteagl_slots.c` fills the table: the 62 slots CA uses call Mesa's public gl* (591 checks
    `glIsProgram`, 470 FinishObjectAPPLE is `glFinish`, the EXT markers do nothing). Every other slot,
    1024 included, is one of 1036 generated assembly stubs (`mov w0, #i; b`) that logs its slot once and
    returns 0.
  - IOSurfaces: `sweetgl_image_from_iosurface` (frontend) imports the surface with SweetGPU's import
    selector (`sweetdrm_import_iosurface`, which now takes the surface's CPU mapping so Mesa can map the
    BO) and wraps it with `resource_from_handle` as a linear resource at the surface's pitch. Mesa
    imports KMS handles with size 0, which fails the layout check, so `sweetgl_import.c` first enters
    the BO in freedreno's handle table with its real size. The frontend's `get_egl_image` /
    `validate_egl_image` hooks hand the image to `glEGLImageTargetTexture2DOES`, so the bound texture
    takes the pages with no copy. `invertedRender:` / `invert:` YES is logged and drawn upright.
  - Present: nothing to hook. CA swaps through IOMFB itself, after its own Flush/Finish/FenceSync calls.
  - `build.sh` makes `libsweeteagl.a` (for tests) and `libsweeteagl.dylib` (Mesa inside, exports only
    `_sweeteagl_install`, for 7.7); CI checks the export list and that no `gl*` is left undefined.
  - `test/sweetgl-eagltri.m` (boot-arg `sweetgleagltri=1`, job `com.sweet.gl-eagltri`) dlopens
    OpenGLES, installs the shim, `initWithAPI:3 properties:`, and from there calls GL only through the
    macro table, as CA does. It builds the display-buffer FBO from a 64x64 BGRA IOSurface
    (`attachImage:`), clears it blue, draws a red triangle (GLSL ES 1.00, client arrays, `Uniform4f`,
    FenceSync/ClientWaitSync), then samples a CPU-filled green IOSurface (`texImageIOSurface:`). Each
    result is checked in the target surface's own memory, and it confirms an unimplemented slot returns 0.
  - Open for 7.7: the shim opens SweetGPU on the first context and fails it if the device is not up
    yet; backboardd starts long before the kernel's 6.7 hold ends (260 s in G6m).
- **7.6 Kernel plumbing**: `sweetenv=` (exec-time env vars), AMFI op 0x5a in the 0092 handler, an
  `IOAcceleratorES` stub without `MetalPluginName`. ✅ passed on the device (G6o run 2, below).

  *All three are xnu patch 0114 (ef2fb60), in every kernel since run 40790303318 (out-0114, out-0116c).
  Observed: the AMFI 0x5a answer is already used, e.g. G6n: "SWEET AMFI: dyld policy in 0x0 out
  0x16d3dfa28 -> 0xfb (0)" for launchd (so the guessed {in, out pointer} layout is right).
  `sweetenv=` and `sweetaccel=1` were in no boot image before G6o.*

  The test, `sweetgl-eagldy` (d1b80b8): `sweetgl-eagltri.m` built with `-DSWEETEAGL_INSERTED`
  (`test/build-eagldy.sh`). It links OpenGLES.framework, as backboardd does, and neither Mesa nor the
  shim; CI checks that with otool and nm. Boot-args `sweetgleagldy=1 sweetaccel=1
  sweetenv=sweetgl-eagldy:DYLD_INSERT_LIBRARIES=/usr/lib/libsweeteagl.dylib`; `sweetgpu-root.py`
  installs the dylib to /usr/lib. It reports (through its own SweetGPU connection):
  - `7.6 env`: the variable is there, as the kernel added it;
  - `7.6 dyld`: dyld loaded the dylib and `sweeteagl_install` is visible;
  - `7.6 shim`: EAGLContext's `initWithAPI:` is already the dylib's when main runs, so the constructor
    installed the shim (the 7.7 path: nothing in backboardd calls the installer);
  - `7.6 accel`: an `IOAcceleratorES` service exists;
  - then the 7.5 GL checks, without the three direct Mesa calls (`glGetShaderiv`, `glGetProgramiv`,
    `glCheckFramebufferStatus`): the test has no Mesa of its own, and the pixel checks catch the same.
  - The boot-args come to 596 of the 607 bytes the loader passes (`BOOT_LINE_LENGTH` 608, cut silently).

  Risk for that boot (a guess): with `IOAcceleratorES` present, backboardd's CA stops waiting 30 s and
  tries Metal and then GLES at once. Without the shim, Apple's EAGLContext should fail and CA fall
  back to the CPU renderer as now, but that is not observed yet.

  *G6o (2026-10-30, root v6o): all four 7.6 lines came out right ("7.6 env:
  DYLD_INSERT_LIBRARIES=/usr/lib/libsweeteagl.dylib", "7.6 dyld: … loaded, sweeteagl_install found",
  "7.6 shim: … initWithAPI: from /usr/lib/libsweeteagl.dylib", "7.6 accel: IOAcceleratorES found"),
  then GL_VERSION through slot 117 and "7.5 clear: 0 of 4096 pixels wrong". The log stops there: the
  bootloader keys were pressed (log line 5701) before the triangle and texture checks, which in G6n
  came ~290 log lines after the clear. So 7.6 is not passed yet; nothing failed. Seen: "SWEET env:
  sweetgl-eagldy += DYLD_INSERT_LIBRARIES=…", and "SWEET ACCEL: IOAcceleratorES stand-in registered"
  only after the 6.7 hold (~260 s), long after backboardd started, so the risk above was not
  exercised (no backboardd crash; FB swaps and GPU copies as in G6n). Next: boot G6o again (same root)
  and wait at least 12 min.*

  ✅ **Passed on the device, G6o run 2 (2026-10-30 09:26, same image and root):** the four 7.6 lines
  as above, then clear, triangle (ClientWaitSync signalled) and texture (texImageIOSurface YES) each
  "0 of 4096 pixels wrong", "7.6 sweetgl-eagldy: pass, footprint 13 MB", "close by sweetgl-eagldy:
  last seqno 130 retired, 6 BOs freed, 0 leaked". No panic, no SMMU fault, 8a.1 PASSED. So a program
  that links only Apple's OpenGLES gets Mesa through the kernel's `DYLD_INSERT_LIBRARIES` and the
  dylib's constructor alone. The crashes in the log (driverkitd sig 9, SpringBoard sig 6 once, the
  Siri TTS helpers) and the "slot 1 … returns 0" stub report were all in G6n as well. Still open for
  7.7: `IOAcceleratorES` only appears after the 6.7 hold (~260 s), so backboardd has never yet seen it
  at start.
- **7.7 backboardd** with `DYLD_INSERT_LIBRARIES`, then SpringBoard, CC toggles, App Library, blur.

Risks to check early (guesses):

- **The GPU VA window is 64 MiB** (0x200000000 + 0x4000000). Three 6 MB CA surfaces plus textures and
  Mesa's own BOs may not fit, so the window probably needs to grow before 7.7.
- Checked: only *kernel* BOs are contiguous (`kmem_alloc_contig`, used by 6.x and 8a.1). The user
  client's bo new is an IOBufferMemoryDescriptor imported page by page (G6h: 24-page BO fine), so
  Mesa's BOs can be scattered. Still open: how many BOs and pages the window bookkeeping allows.
- CA's surfaces are 3328 bytes per row (64-aligned), which suits a6xx's linear pitch rule; other
  surfaces CA samples may not be.

### Stage 8: present path

- Today (H4) the CPU copies 5.9 MB per frame into the canvas buffer in the splash carve-out, and the MDP
  scales it. Two options:
  - **(a) GPU copy.** A kernel-built blit copies the swap surface into the canvas, removing the CPU copy.
    Small and safe.
  - **(b) Zero-copy.** Allocate the swap surfaces physically contiguous (below 4 GB, 64-byte-aligned
    rows) and point VIG0 straight at the surface backboardd just swapped. The CTL flush latches it at
    the next frame. This would help even the current CPU path. It needs three 5.9 MB contiguous
    buffers, which means either reclaiming the 10 MB panel buffer once VIG0 is switched, or allocating
    them early in boot.
- Start with (a) and look into (b) afterwards.

#### 8(a) planned in detail (2026-10-28, while CI 40672085967 queued; nothing built)

**The present path today (0093 + `sweetfb_mdpscale=1`, read from `SweetFramebuffer.cpp`).**
- CA swaps alternate between two surfaces (2 and 3 in the G6g log): 828x1792 BGRA, 3328 bytes per row (64-byte
  aligned).
- They are `IOBufferMemoryDescriptor`s, cacheable and **not physically contiguous**.
- `AppleCLCD::vsyncFired` runs `present()` at most once per dirty vsync. `present()` memcpys 1792 rows of 3312 bytes
  (5.93 MB) into the canvas at PA 0x9ca00000, mapped write-combined, then runs `dsb sy`. VIG0 fetches the canvas and
  QSEED upscales it to 1080x2340.

**What the gain can be (estimate, not measured).**
- E2c measured a 10 MB panel copy at 2.1 ms (about 5 GB/s), so this copy is likely about 1.2–1.5 ms per presented
  frame. That is under 10% of one core even at 60 presents/s.
- The surfaces are cacheable CPU memory, so before the GPU (not coherent here) can read one, the CPU must clean it to
  PoC (`dc cvac` over 5.9 MB, as `sweet_gsvc_bo_sync` does). That clean may cost a large part of the copy it
  replaces.
- So **8(a) saves little while CA renders on the CPU**. The UI's slowness is CA's CPU renderer (FBSTAT showed 1–6
  swaps/s in the E2c/G1c sessions; a guess that this still holds), which only Stage 7 fixes.
- 8(a) pays off once Stage 7 renders into the surfaces with the GPU: then nothing is in the CPU caches, no clean is
  needed, and the CPU does no per-frame work at all.
- (b) zero-copy would need physically contiguous swap surfaces; today's surfaces are page lists, so (b) stays later.

**Recommendation.** Do 8a.0 now (free: one boot-arg on the next test image). Build 8a.1 now if wanted: it is also
Stage 7 groundwork, because it proves the 2D engine. Do 8a.2–8a.3 together with Stage 7's first GPU-rendered frames.

**Copy engine.** CP_MEM_TO_MEM (6.4's copy) moves one word per 6-dword packet: about 35 MB of IB per frame, so it is
unusable. Use the a6xx **2D blitter** (`CP_BLIT`, `BLIT_OP_SCALE`), which needs no shader.
- The sequence, as Mesa turnip's `r2d_*` and freedreno's `fd6_blitter` (MIT) use it:
  - `CP_SET_MARKER` 2D-blit mode.
  - `RB_2D_BLIT_CNTL` / `GRAS_2D_BLIT_CNTL` (colour format and ifmt), `SP_2D_DST_FORMAT`.
  - Source: `SP_PS_2D_SRC_INFO`, `_SIZE`, base and `_PITCH`.
  - Destination: `RB_2D_DST_INFO`, base and `_PITCH`.
  - Source and destination rectangles: `GRAS_2D_SRC_TL/BR_X/Y`, `GRAS_2D_DST_TL/BR`.
  - `CP_BLIT`, then the CCU colour flush event and our `CACHE_FLUSH_TS` fence.
- Offsets, field layouts and the exact flush events must be taken from `a6xx.xml` at the pinned Mesa release and
  checked one by one. This list is from memory.
- The 2D pitches are in units of 64 bytes (to check), so the **canvas stride must become 3328** (`VIG0 SRC_YSTRIDE`;
  3328 × 1792 = 5.96 MB, still under the carve-out's end). The CPU path can use the same stride.
- Format: BGRA8 to BGRA8, same size, nearest filter, no swap. Our 4.1 init list may not set the CCU (`RB_CCU_CNTL`)
  the way the 2D path expects; 8a.1 checks that.

**Steps.**
- **8a.0 Baseline (no new code).** Add `sweetfbstat=1` to the next test boot (G6h), then read FBSTAT's `copy avg` for
  the MDP path. PORT_LOG has no FBSTAT result for the MDP path; H4c was built for it, but no reading was recorded. It decides whether 8(a) is worth more
  than a groundwork test.
- **8a.1 Blit self-test (`sweetblit=1`, after 6.7 like 6.4; `sweetgpu=8 sweetsvc=7`).**
  - Two window BOs of 3328 × 1792 bytes; the CPU fills the source with the 6.4 pattern; one `CP_BLIT` copies it;
    every pixel is checked.
  - Repeat at 64x64 and 828x1 to catch row and edge errors. Log the GPU time (submit to fence).
  - Pass: 0 bad pixels in all three; no SMMU fault, no `CP_HW_FAULT` / `CP_PROTECT_STATUS`, no hang latch; the
    service is still ready.
  - Expect ≤ 2 ms for 6 MB (guess: DDR-bound at 180–430 MHz).
- **8a.2 Shadow copy (`sweetfb_gpucopy=1`).** The CPU still presents. Each presented surface is imported once (by
  surface ID, freed when the surface is freed), and a canvas-sized window BO is the destination. Per present: clean
  the surface to PoC (timed), blit, wait (20 ms limit), then compare 16 sampled rows with the canvas the CPU wrote.
  - Log once per 5 s: clean, blit and wait times, and mismatches.
  - Pass: 0 mismatches over 2 minutes of UI use; times logged. Nothing on screen changes.
- **8a.3 GPU present (`sweetfb_gpucopy=2`).** Import the canvas carve-out once as a BO (`bo_import_pages` of
  0x9ca00000…). The blit writes it; the CPU memcpy is skipped.
  - Any error, wait timeout or `SWEET_GSVC_DEAD` falls back to the CPU copy for the rest of the boot, and logs why.
  - Before the GPU path starts, VIG0 is never pointed at a buffer the GPU writes. The switch stays as it is (the first
    frame is a CPU copy).
  - Pass: the UI looks as on H4; FBSTAT copy avg falls (CPU time = clean + submit); no fallback in 10 minutes.

**8a.1 written (patch 0113, 2026-10-28; not built on CI, not booted).**
- `sweet_gsvc.c` runs `gs_blit_step` after 6.7 and before the user client is published, when `sweetblit` > 0.
- Three copies, each with its own source, destination and 1-page IB BO: 64x64 (pitch 256), 828x1 and 828x1792 (pitch 3328).
  - The source is a hash pattern (every byte varies, so channel swaps show). The 16 bytes of row padding hold a
    poison that must survive.
  - Each line logs bad pixels, changed padding words, the submit-to-fence time, and the two CCU flush timestamp words.
  - An `ENOMEM` from the 6 MB contiguous BOs counts as skipped, not failed.
- The IB, in order:
  - Set up: CCU colour/depth flush (timestamped) and invalidate, `CP_SET_MARKER` RM6_BLIT2DSCALE, WFI, and
    `RB_CCU_CNTL` 0x08000000 (sysmem: colour at 64 KiB).
  - With `sweetblit=2` only: the a618 magic registers.
  - `RB/GRAS_A2D_BLT_CNTL` 0x00f03000, `RB_A2D_PIXEL_CNTL` 0, `SP_A2D_OUTPUT_INFO` 0xf180.
  - Source `TPL1_A2D_SRC_TEXTURE_*` (info 0x00500030) and destination `RB_A2D_DEST_BUFFER_*`.
  - The rectangles, then WFI (blit_wfi_quirk) and `CP_BLIT` op 3.
  - A CCU colour flush, then WFI.
- None of these registers is in a630_protect.
- Pass: 0 bad pixels and 0 changed padding in at least 2 of 3 copies with none failed; window pages 0 afterwards;
  `gs_clean`. A failure is logged and doesn't kill the service, unless the poller finds a fatal error.

**8a.1 result (2026-10-29).** G6i (`sweetblit=1`) failed with stale source data from the previous case at the
reused window VA. G6j (`sweetblit=5`, CACHE_INVALIDATE first) passed all 6 cases, the 828x1792 copy in 1011 us
submit-to-fence. So every kernel blit starts with CACHE_INVALIDATE.

**8a.2 written (patch 0116, 2026-10-29; built on CI, not booted).**
- `sweet_gsvc_blit(dst, src, pitch, w, h, &seq)`: the 8a.1 IB (always with CACHE_INVALIDATE, no magic registers)
  in a one-page IB BO made on first use. Calls are serialised: each one waits for the previous copy first.
- `sweetfb_gpucopy=1`, MDP path only. After `present()`'s CPU canvas copy, `gpuShadow()`:
  - waits until the service is READY (until then it counts the present as "skipped");
  - imports the surface's pages into the window once (the 3 most recently used stay imported and retained);
  - uses as the shadow a page-list `IOBufferMemoryDescriptor` (not a contiguous `bo_new`), imported the same way;
  - cleans the surface and the shadow to PoC by VA (`flush_dcache64`, timed), blits, and waits (20 ms limit);
  - invalidates the shadow, then compares 16 rows spread over the canvas with the canvas itself.
- Its time is kept out of FBSTAT's copy statistics. `sweetfbstat` is forced on, and every 5 s a line follows FBSTAT:
  `SWEET GPUCOPY: n copies/5s, clean avg/max, blit+wait avg/max, x of y sampled rows differ, imports, skipped`.
  The first copy logs both GPU addresses. The first 8 differing rows are logged with the first differing pixel.
- Any error (import, blit, a 20 ms timeout, a dead service, a surface whose pitch or offset is not 64-byte aligned)
  logs `SWEET GPUCOPY: off for the rest of this boot: <why> (<errno>)` and stops 8a.2. The screen is unaffected.
- Test boot G6k = G6j's args + `sweetfb_gpucopy=1`. Pass: GPUCOPY lines with 0 differing rows over 2 minutes of UI
  use, never "off"; the times are recorded for the 8a.3 decision.

**Test boots.** G6h carries 8a.0 (proposal: G6g's args plus `sweetfbstat=1`). After that: B1 = G6h args +
`sweetblit=1`. Later, with Stage 7: B2 (`sweetfb_gpucopy=1`) and B3 (`=2`). All are `fastboot boot` only, and no root
change is needed.

**Risks, most uncertain first.**
1. The 2D engine's setup on a618 (CCU, ifmt, UBWC off): if 8a.1 shows garbage, compare with turnip's register dump
   for a linear BGRA copy.
2. The clean-to-PoC cost may eat the gain while CA renders on the CPU (8a.2 measures it).
3. Tearing: the canvas is updated while VIG0 scans it. That is the same as today's CPU copy, so it is not new.
4. Importing 1456 pages per surface: the window (64 MiB, BO limit 16 MiB) fits 2 surfaces plus the canvas (about
   18 MB). Re-imports when CA reallocates surfaces must free the old ones (6U's leak count shows it).

### Stage 9: tuning

- Clock levels up to 650/825 MHz, if thermals allow.
- DDR bandwidth votes (BCM through the apps RSC).
- GMEM rendering, once surfaces are padded.
- UBWC for CA's private textures.
- fp16 (sailfish had to turn it off).

## 7. Safety and test protocol

- Kernels are tried with `fastboot boot` only: nothing is written to the phone.
- The only writes are the Stage 7 root edit and later dylib updates. Each needs the owner's OK at that time.
- A GPU hang or a bad SMMU setup at worst freezes or resets the phone. Hold power to reboot, then pull the
  log in TWRP. H4 without `sweetgpu` stays the safe everyday boot.
- No raw voltage writes: rails are RPMh corner votes, and clocks come from downstream tables.
- Vendor firmware, boot images that contain it, and device logs stay out of git.

## 8. Risks, most uncertain first

1. **GX power through the GMU (2B).** 2A showed the host cannot start the GPU core clock. 2B adds the
   SMMU, RSCC/PDC, the firmware start and HFI at once; its own risk list is at the end of Stage 2B.
2. **The hypervisor and the GPU SMMU.** Trapped global registers, or stage-2 holes. This now comes up in
   2B.1, before Stage 3.
3. **TrustZone PAS for the zap on Pixel firmware.** It may need the metadata in specific memory (below 4 GiB);
   see Stage 5's risk list.
4. **sweetgl's a5xx assumptions on a6xx.** Pitch rules, params, nogmem/nofp16 workarounds.
5. **Licence** of the sweetgl code (section 5).
6. **Build cost.** Mesa on GitHub macOS runners; mitigated by caching.

## 9. Next concrete step

Stages 0–3 passed on the phone. The last results: G3k (Stage 2 complete, gx_gfx3d at 180 MHz on the GPU
PLL) and G3l (Stage 3, the GPU's SMMU bank for SID 0 reads back with no fault). The history is in the
stage sections above.

Stage 4 (CP first light) is now planned in detail ("Stage 4" above): survey, the init list, SQE and ring
registers, SQE start, ME_INIT, register markers with a ring-wrap test, and a 30 s hold, under `sweetgpu=6
sweetcp=N`. Patch 0107 builds it (`sweet_cp.c`, the bump allocator in `sweet_gmu.c`, the `sweetgpu=6`
hook; commit 5f906bf, CI 40181166244 passed, artifact `xnu-kernel`).

Result on G4a (0107, `sweetcp=2`, second boot; the first was switched off before stage 3): 2B and stage 3
passed again, 4.0 passed (SQE at 0x10257e000, ring 0x102586000, tripwire 0x10258a000, all read back; CP
registers and CP_PROTECT 0, SECVID_TRUST_CNTL 1, GBIF_HALT 0, GBIF_HALT_ACK 0x3). 4.1 stopped at its second row:
RBBM_GBIF_HALT (0x16) reads 0xdeafbead before and after writing 0, as do 0x17 and RBBM_NC_MODE_CNTL (0x534).
No SMMU or CP fault; the SQE was never started. Guess, not proven: those registers do not exist on a618.
Patch 0108 (commit 2e6dacd, CI 40183251805 passed) makes that row logged only; GBIF_HALT stays required.
Leave the phone on about 4 minutes: stage 4 starts about 2.5 minutes after boot.

Result on G4d (0108, `sweetcp=2`): stage 4 done at 4.2. Every required 4.1 row read back. Logged rows that
differ: RBBM_GBIF_HALT and RBBM_NC_MODE_CNTL (0xdeafbead), RBBM_PERFCTR_GPU_BUSY_MASKED (0x002f7fff),
UCHE_GMEM_RANGE_MAX_LO (0x0017f000: the low 12 bits are not kept), CP_ROQ_THRESHOLDS_2 (0) and UCHE_CLIENT_PF
(0x1). 4.2's SQE and ring registers all read back, RPTR 0, RBBM_STATUS 0x1, no CP or SMMU fault. GBIF_HALT_ACK
still reads 0x3.

Result on G4b (0108, `sweetcp=4`): stage 4 done at 4.4, the CP runs. 4.3: with CP_SQE_CNTL=1 on the empty
ring, RPTR stays 0 and the GPU idle at 1 ms, 100 ms and 1 s, CP_HW_FAULT and CP_PROTECT_STATUS 0, and no fault on
SMMU bank 0 or 4, so the SQE's microcode fetch through bank 0 worked. 4.4: ME_INIT was consumed, RPTR 9 and idle
after 10 us, no fault. The SQE stays running.

Result on G4c (0108, `sweetcp=6`): **stage 4 passed.** 4.5: the first marker landed (SCRATCH_REG7 0x5ee70001,
RPTR 11), then all 80 batches of 64 (each idle within 10 us), two ring wraps, last value 0x5ee723ff, RPTR = WPTR
2059. The ring past ME_INIT was still zero beforehand and the tripwire page was unchanged after. 4.6: markers
0x5ee72001..3 at 10, 20 and 30 s, RBBM_STATUS 0x1, CP_HW_FAULT and CP_PROTECT_STATUS 0, no SMMU fault, GMU
and GX GDSC steady.

Stage 5 (zap shader, leaving secure mode, first DRAM writes) is now planned in detail ("Stage 5" above):
read-only SCM queries, a secure-mode baseline write, PAS INIT_IMAGE / MEM_SETUP (size 0x1000) / load /
AUTH_AND_RESET, `CP_SET_SECURE_MODE 0`, `CP_MEM_WRITE` to the scratch page, and a 30 s hold, under `sweetgpu=7
sweetcp=5 sweetzap=N`, images G5a/G5b/G5c. Patch 0109 builds it (new `sweet_zap.c`, ring helpers exported from
`sweet_cp.c`, the `sweetgpu=7` hook; commit 8e908dd, CI 40194604743 passed, artifact `xnu-kernel`). In 5.5 the 16 writes to words 16–31 carry
`0xc0ff1000 + word index`.

**G5a result (booted once, `sweetzap=0`): Stage 5 stopped at 5.0, as the pass rule says, because `PAS_IS_SUPPORTED(13)`
returned x0 0, x1 0.** Seen in the log:
- Stages 0–4 passed again. Stage 4 reached 4.5 ("stage 4 done at 4.5"). The scratch page held its poison: 0 of 1024
  words differed.
- The SMC64 probe (fast call, 15 µs) and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7 all returned x0 0, x1 1. None was
  interrupted or busy.
- `PAS_IS_SUPPORTED(13)` returned x0 0 (the call itself succeeded) and x1 0 ("not supported"). It took 188 µs; the
  `IS_CALL_AVAIL` calls took 4–5 µs.
- `RBBM_SECVID_TRUST_CNTL` read 0x00000001.
- The MDT checks all held: ELF32, machine 164, 3 program headers, metadata = the whole 6860-byte file, segment 2 at
  carve-out + 0 (0x7c0 bytes, relocatable), MEM_SETUP size 0x1000, B02 1984 bytes.
- The carve-out read and the metadata copy did not run; the stage stopped first.
- No fault and no panic. The phone kept running to the end of the log.

Not known (guesses):
- Whether TZ would still accept `INIT_IMAGE` for PAS 13. The not-supported answer may only mean that this
  `IS_SUPPORTED` table leaves the GPU out. Mainline's zap loader does not call `PAS_IS_SUPPORTED` (from memory, not
  checked).
- Whether the hypervisor answered instead of TZ. The longer time suggests the call did more work than the others, but
  proves nothing.

Per this plan, 5.0 = 0 means a new plan, not a fallback. Research of the stock sunfish kernel, done with the
owner's OK, found:
- PAS 13 and `a615_zap` are right.
- The stock loader and mainline never call `PAS_IS_SUPPORTED`.
- There is no hyp_assign for the zap.
- Stock votes crypto-core bus bandwidth before INIT_IMAGE and AUTH_AND_RESET.

That gives "5.0 revised" (in Stage 5 above): `PAS_IS_SUPPORTED` becomes log only, with PAS 1/4/9/18 as a
baseline. The owner approved it.

Patch 0110 (commit 5b29b1f) changes only `zap_survey` in `sweet_zap.c`:
- `IS_CALL_AVAIL` must give 1 for cmds 1, 2 and 5; cmd 7 is logged only.
- `PAS_IS_SUPPORTED` is asked for 13, 1, 4, 9 and 18. Each answer is logged as "PAS n reads ...", and the SMC must
  still return x0 0.
- The `sup` term is gone from the pass rule.

The local clang check was clean, and the series 0094–0110 applies on 0093. CI run 40197408369 passed in 9m01s (artifact `xnu-kernel`). Nothing
is booted. G5a2 is packed from out-0110:
- The boot-args are identical to G5a's.
- The blob addresses are the same as G5a's.
- The 828xr ADT and all firmware are in the payload.

**G5a2 result (booted once, `sweetzap=0`): revised 5.0 PASSED ("stage 5 done at 5.0").** Seen in the log:
- Stages 0–4 passed again (4 done at 4.5). The scratch page was untouched: 0 of 1024 words differed.
- The probe and `IS_CALL_AVAIL` for PIL cmds 1, 2, 5 and 7 all gave x0 0, x1 1.
- `PAS_IS_SUPPORTED` gave x0 0 and x1 0 for **every** id asked: 13, 1, 4, 9 and 18. 13 took 188 µs; the others
  took 30–37 µs. Those other subsystems certainly run on this phone, so this call means nothing on this TZ.
  G5a's stop was a false alarm.
- `SECVID_TRUST_CNTL` read 0x1. The MDT and B02 checks were the same as G5a's.
- The carve-out at 0x97715000 mapped and read without a fault. The first and last words are both 0xafafafaf, a fill
  pattern (presumably left by the bootloader or TZ; not known).
- The metadata copy went to pa 0x10258f000, as guessed. All 6860 bytes were copied, the first word is 0x464c457f
  (ELF magic), and 0 words differed.
- SMMU bank 0 FSR stayed 0x400, the same as on every earlier boot. No panic.

Open risk for 5.2: the metadata pa is above 4 GiB. Stock sets no 32-bit mask, but nothing proves TZ accepts it. If it
does not, INIT_IMAGE should fail with an error (for example -3 EINVAL_ADDR) rather than hang (guess). Next, with the
owner's OK: pack G5b (`sweetzap=3`: 5.1 baseline, 5.2 INIT_IMAGE + MEM_SETUP, 5.3 load + AUTH_AND_RESET).
G5b is packed from out-0110:
- Its boot-args differ from G5a2's only in `sweetzap=3`.
- The blob addresses are the same as G5a2's.
- The 828xr ADT and all firmware are in the payload.

**G5b result (booted 2026-10-26 09:36, `sweetzap=3`): 5.0–5.3 PASSED; the zap shader authenticated ("stage 5 done
at 5.3").** Seen in the log:
- **5.0:** as on G5a2.
- **5.1:** the secure-mode `CP_MEM_WRITE` **landed**: word 0 = 0xc0ffee00, so writes are *not* dropped in secure
  mode on this setup. The marker 0x5ee75001 landed and RPTR = WPTR = 2065. No fault; the other 1023 words were
  untouched.
- **5.2:** `PAS_INIT_IMAGE(13, 0x10258f000)` returned x0 0, x1 0 after **3 INTERRUPTED resumes**, in 17.1 ms. So
  the resume path works, and metadata above 4 GiB is accepted (risk 1 did not happen). `PAS_MEM_SETUP(13,
  0x97715000, 0x1000)` returned 0 in 254 µs.
- **5.3:** all 496 words of segment 2 were written into the carve-out (first word 0x016ff021), and 0 read back
  differently. `PAS_AUTH_AND_RESET(13)` returned x0 0, x1 0 in 1.58 ms.
- **After 5.3**, the CP dump is unchanged: RBBM_STATUS 0x1, HW_FAULT 0, PROTECT_STATUS 0. SMMU bank 0/4 FSR 0x400
  as always. The GMU is the same as before. No panic.
- No crypto bandwidth vote was needed (at least for this one load).

**What this changes for 5.4–5.5 (observation plus a guess):** since a secure-mode write already lands, a landed
0xc0ffee01 in 5.5 cannot by itself prove that 5.4 left secure mode. The proof for 5.4 is instead:
- `CP_SET_SECURE_MODE 0` does not hang the CP (mainline says it hangs without a zap), and its marker lands;
- 5.5's writes land with no fault.

Whether the write path differs in secure mode (for example, which SMMU context or "secure" attribute the GPU uses)
is not known. Next, with the owner's OK: pack G5c (`sweetzap=6`) from out-0110. No code change is needed; the 5.5
pass rule already checks the words.

G5c is packed from out-0110:
- Its boot-args differ from G5b's only in `sweetzap=6`.
- The blob addresses are the same as before.
- The 828xr ADT and all firmware are in the payload.

**G5c result (booted 2026-10-26 09:48, `sweetzap=6`): STAGE 5 PASSED ("stage 5 done at 5.6").** Seen in the log:
- **5.0–5.3** passed again:
  - The 5.1 secure-mode write landed again.
  - INIT_IMAGE returned 0 after 2 INTERRUPTED resumes, in 16.3 ms.
  - MEM_SETUP returned 0 in 257 µs.
  - The B02 load had 0 mismatches.
  - AUTH_AND_RESET returned 0 in 1.61 ms.
- **5.4:** `CP_SET_SECURE_MODE 0` did **not** hang. The marker 0x5ee75004 landed, RPTR = WPTR = 2069, and
  HW_FAULT, PROTECT_STATUS and INT_0_STATUS stayed 0. SMMU bank 0/4 FSR stayed 0x400, as always.
- **5.5:**
  - Word 0 = 0xc0ffee01, idle after 10 µs.
  - One submission of 17 writes (word 1023 = 0xc0ffee02, words 16–31) was idle after 10 µs.
  - The CPU compared the whole page: 0 of 1024 words differ.
- **5.6:**
  - At 10/20/30 s, word 2 = 0xc0ff2001/2/3 and the markers 0x5ee75601/2/3 landed.
  - Each check found 0 of 1024 words differ, RBBM_STATUS 0x1, no fault, and the GMU steady.
  - RPTR = WPTR = 2159 at the end.
- The phone kept running after the hold: userspace went on launching daemons. No panic.

So the GPU now runs outside secure mode with the zap loaded, and its CP writes to DRAM through SMMU bank 0 are
CPU-visible. Stages 0–5 are done.

Not known: whether anything differs between secure and non-secure writes, since both landed. Next (to plan, with
the owner's OK): Stage 6.

**Stage 6 (2026-10-27): 0111 passes 6.0–6.4 on the phone; 0111b written for 6.5, not booted.**
- G6a passed 6.0–6.1, G6b 6.0–6.3, G6c 6.0–6.4. Fence 1 lands in 23 µs; bit 20 latches and clears; the
  window at 8 GiB, IBs and remap work; the self-test has 0 bad pixels (copy 3.3 ms).
- G6c stopped at 6.5: the hardware hang detector (bit 23) fired during the deliberate wait and was fatal.
  The CP did resume after the release. 0111b makes bit 23 expected in 6.5's test mode (see the top of
  "Stage 6").
- **G6c2 (2026-10-28, kernel 0111b + 0112, CI 40643167242, sweetsvc=5): 6.0–6.5 PASSED.** The 6.4
  self-test again had 0 bad pixels. In 6.5 the hardware RBBM_HANG_DETECT latched after 186 ms (one latch,
  RBBM_STATUS 0x00800005). It was logged and cleared, not fatal. The software detector fired after 2011 ms,
  inside the 2000–2100 ms window. After the release, fence 90 landed in 23 µs and the word read back as
  expected. There were no SMMU or CP faults, and iOS kept running. SweetGPUReady was not published, as
  required at sweetsvc=5. Next is G6d (sweetsvc=7).
- **G6d (2026-10-28, same kernel, sweetsvc=7): 6.0–6.7 PASSED; the kernel part of Stage 6 is complete.**
  - 6.6 clock steps: perf 4 measured 430,000 kHz and perf 5 measured 565,002 kHz. Both were within 1 %,
    and DCVS_RETURN was 0 both times. So the perf-index order guessed from 2B.5 is right for 4 and 5.
  - 6.6 self-tests: 0 bad pixels at both clocks. The 64×64 copy took 3.30 ms at 180 MHz, 2.18 ms at
    430 MHz and 2.15 ms at 565 MHz. The copy is barely faster at 565 than at 430, which by the 6.6 rule
    points at memory bandwidth (the DDR vote is still "no change"). A BW-index step is the planned
    follow-up. This is a guess until it is measured.
  - "service ready": chip_id 0x06010800, gmem 0x80000 at 0x100000, cur 565002 / max 565000 kHz, window
    0x200000000 + 64 MiB.
  - 6.7 hold: 6 rounds through the C API, each with 0 bad pixels and 0 window pages in use afterwards.
    There were no SMMU faults, no CP HW_FAULT and no hang latches outside 6.5's test, and GX stayed on.
  - Then "published SweetGPUReady", and "SweetGPU registered": SweetGPU::start from 0112 ran on the phone.
    iOS kept booting (fairplayd, bluetoothd) and the touch driver kept running.
  - G6e (2026-10-28; 0112, root v6g with sweetgpu-test as a launchd job, sweetgputest=1): **froze**.
    - The log shows 6.0–6.7 passing as in G6d. It ends at "SWEET GPUUC: SweetGPU registered", with nothing
      after it: no "open by", no panic, and none of the ~5 s FTS lines.
    - Reading (guess): the hang is in the tool's first user-client calls within ~5 s of registration. The
      1 s log flush loses the last second, so the exact step is unknown.
    - Next: 0112b. The user client logs, then pauses 2 s before each step (as gpu_pause in 0094).
  - G6f (0112b, + sweetgpuucslow=1 sweetiolog=sweetgpu-test): **froze in bo new**.
    - Open, initWithTask and info (fence map) all succeeded. The fence map logged XNU's "physical with
      managed page" warning.
    - The log ends at "next: bo new 16384 bytes: IOBufferMemoryDescriptor write-combined". The hang is in
      IOBMD inTaskWithOptions with kIOMapWriteCombineCache.
    - Next: 0112c makes user BOs cacheable (no WC) and cleans/invalidates them to PoC around submit/wait,
      like imports and the kernel's own BOs.
  - G6g (0112c, same args as G6f): **no freeze, the full sweetgpu-test session ran**.
    - All 6 bo new calls, the IOSurface import, 4 submit+wait (seqno 116–119) and all frees returned 0.
    - Both bad submits were refused (kIOReturnBadArgument).
    - Close: "last seqno 119 retired, 0 leaked". No GPU/SMMU errors, and iOS kept running.
    - Still open: the tool's pixel checks (PASS/FAIL) go to /dev/console, which isn't in the persistent
      log. 6U's pass rule needs that verdict logged.
  - (Before G6e) Next: G6e (sweetgpu-test through the user client). It waits on the owner's choice of how the tool gets
    onto the phone.
- While bit 20 is latched, GICD shows INTID 332 pending. So the GPU line reaches the GIC; only the enable
  is missing.

The detailed plan is under
"Stage 6" above, with 0111's deviations listed at its top. In short:
- patch 0111 (kernel only) for steps 6.0–6.7, with the new boot-arg `sweetsvc`;
- images G6a (1), G6b (3), G6c (5) and G6d (7);
- then patch 0112 (the user client, selectors 10–15, and a test tool), whose delivery to the phone is the
  owner's choice.

Interrupts stay polled, because the GIC never enables INTID 332 (SPI 300).
