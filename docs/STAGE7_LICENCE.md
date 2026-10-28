# Stage 7: the licence decision (brief, 2026-10-28)

The owner decides this. It blocks only Stage 7 (Mesa running CoreAnimation's GLES renderer); Stages 0–6
and the kernel side of 8(a) need nothing from ios-sailfish. This is not legal advice.

## The facts

- **ios-sailfish has no LICENSE file.** By default that means all rights reserved: its code may be read
  but not copied, adapted or redistributed. Ideas, facts and interfaces are not covered (for example,
  "CA calls these six EAGL methods", "dyld asks AMFI op 0x5a"); the code that expresses them is.
- **Most of Stage 7 is Mesa, and Mesa is MIT.** The GL driver, the a618 support and the register XML
  can be used freely with the notice kept. Upstream ios-sweet is MIT too.
- **Our fork is on GitHub.** If it is public, whatever lands in it is published (redistribution is
  exactly what a missing licence forbids).
- Section 2 of `GPU_A618_PLAN.md` was written from reading their repository. Strictly, that rules out a
  textbook clean room (where the reader and the writer are different people). What is still clear is
  that no code of theirs may be copied in; their write-up can serve as a map of *what* is needed.

## What Stage 7 needs, and how much of it touches their code

| Part | Size (guess) | Without their code |
|---|---|---|
| Mesa build job (iphoneos SDK, freedreno only) | small | Easy: meson cross file + CI job. Mesa docs cover it. |
| libdrm / msm ioctl emulation over our SweetGPU selectors | ~1–2k lines | Easy to medium. Defined by what Mesa calls (MIT) and by our own user client (ours, already tested on G6h). |
| Gallium frontend (surfaceless, FBO-only) | ~1k lines | Medium. Haiku's hgl frontend in Mesa (MIT) is the model they also used. |
| Kernel plumbing: `sweetenv=`, AMFI op 0x5a, `IOAcceleratorES` stub | small | Ours anyway (it lives in our XNU patches). |
| **EAGL shim** (they have 77 KB) | large | **Hard.** It encodes reverse-engineered CoreAnimation/EAGL behaviour: the GLI dispatch table layout, the IOSurface texture paths, framebuffer-fetch emulation. Redoing it means disassembling iOS 16's QuartzCore/OpenGLES from our own root, locally, and testing on the phone. |

## Options

1. **Ask joranv1 to add a licence** (ideally MIT, like upstream). The owner makes the contact; I can
   draft the message for the owner to send, and I will not post anything. Upside: the EAGL shim and
   `gen-dispatch.py` become usable, saving the hardest part. Downside: unknown wait, or a no.
2. **Write everything ourselves.** No dependency on anyone. Costs the most time, almost all of it in the
   EAGL shim.
3. **Both at once (recommended).** Start now on the parts that never needed their code: the Mesa build
   job, the msm emulation, the frontend and the kernel plumbing. They are needed either way. In
   parallel the owner asks for a licence. When the EAGL shim is reached, use theirs if a licence has
   arrived, or write our own if not. Nothing is lost while waiting.

## What I need from the owner

- Pick 1, 2 or 3.
- For 1 or 3: whether you want a draft of the message to joranv1 (you send it yourself).
