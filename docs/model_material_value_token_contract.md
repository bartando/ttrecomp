# Generic model/material value-token contract

`sub_820F19C8` is the title's `grmModelGeom` submission loop. The native hook
observes it before original execution and publishes bounded, value-only tokens;
it never suppresses or serves a draw.

The recompiled body establishes the selection chain:

- `model+0x16`: geometry count
- `model+0x0C`: `uint16_t` material indices
- `shader_group+0x08`: shader pointer array
- `0x82606350`: optional global shader override
- `shader+0x04`: flags whose bits 5..9 select the render category; a global
  override with bit 0 set selects category zero
- `shader+0x00`: shader vtable
- `shader_vtable+0x0C`: selected draw slot target

Every retained token is
`{model, geometry_index, shader, shader_vtable, shader_slot_target,
render_category, lod}`. Guest reads use the streaming-fault recovery layer,
the observer scans at most 256 geometries per submission, and a frame retains
at most 2048 tokens. No guest pointer is dereferenced after publication.

Enable aggregate coverage telemetry with:

```text
--tabletennis_native_model_material_tokens_log_interval=60
```

The report is one line per interval and includes submission, geometry,
category-selection, unique-identity, slot-target, read-failure, and bounded
drop counts.
