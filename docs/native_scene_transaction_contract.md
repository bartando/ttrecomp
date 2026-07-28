# Native scene private transaction contract

`tabletennis_native_scene_transaction` is the first end-to-end execution of the
Skate 3-style scene path. It consumes the immutable title-owned frame scene,
builds the exact original-ordinal composition plan, and records the currently
proven native families into one RGBA8/D32 four-sample offscreen pass.

This stage is deliberately observer-only:

- `tabletennis_native_scene_transaction_observer=true` arms the complete
  capture graph and schedules the output callback during gameplay.
- Resource allocation and uploads finish before the offscreen pass opens.
- PS328 venue, 14D venue, C6 crowd, and CA9 player draws are recorded one at a
  time in strictly increasing title draw ordinal.
- Delayed C6 backend proof is cloned onto the exact older immutable title
  snapshot whose ordered block matched. A callback can never mark the newer
  title frame currently being captured as proven.
- Any plan, preparation, or record failure rejects the entire transaction.
- A successful transaction is also discarded. The private target is never
  resolved into the presenter and no guest draw or guest frame is suppressed.

The log states `private_target=true resolved=false guest_suppressed=false` so a
successful draw-record proof cannot be mistaken for native serving. Serving is
a later gate and requires complete MAIN/HUD/post coverage plus stable output
and performance validation.
