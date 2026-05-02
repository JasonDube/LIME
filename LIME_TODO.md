# LIME — backlog

Living scratch list of things we know we want to come back to. Not a roadmap;
just so good ideas don't fall on the floor.

## Skinning / animation quality

- [ ] **Corrective blend shapes ("shape keys")**. The big one. On top of LBS or
      DQS, author per-character shape deltas that fire when a bone hits a given
      angle (e.g. `elbow_bent_90`). Inside of the elbow folds, bicep bulges,
      knee cap pops out — the stuff hand-authored AAA rigs do that pure linear
      skinning can't. Implementation sketch:
        - Per character, list of `{bone_index, axis, angle, vertex_deltas[]}`
        - Each frame, compute the bone's current rotation around the named axis,
          map to a 0..1 weight via a falloff curve, add `weight * vertex_deltas`
          to the LBS/DQS output.
        - UI: "record corrective at current pose" — sculpts the desired shape
          while the bone is held at angle X, stores the delta from where LBS/DQS
          puts it.
        - Export to GLB as morph targets driven by an animation channel.

- [ ] **Helper / muscle bones**. Procedural extra bones positioned algorithmically
      (e.g. shoulder twist bone that rotates half as much as the shoulder) to
      catch the volume LBS loses. Cheaper than blend shapes, no per-character
      authoring.

- [ ] **DQS support in eden renderer**. We can preview DQS in LIME today, but
      the GLB export goes to eden's LBS skinning shader. Adding a DQS variant of
      `skinned_model.vert` would close the preview/runtime gap.

- [ ] **Smooth-rim subdivision around joints**. Even with DQS, more verts at the
      bend = a smoother curve. Could add an "elbow ring" mesh op that inserts a
      few loop cuts around a selected bone's region.

## Rigging UX

- [ ] **Rigging-panel layout pass** — buttons have been added one at a time over
      many sessions and the panel is getting noisy. Group into named ImGui
      child windows / collapsing headers so each section reads like a tool:
        - *Skeleton* (Add Bone, Insert Bone, bone list, Delete Bone, Save/Load
          Skeleton)
        - *Bind* (Set Bind Pose, Auto Weights, Max Reach, Use DQS, Clear Weights)
        - *Visualize* (Show Skeleton, Show Bone Names, Weight Heatmap, Weight Paint
          + radius/strength)
        - *Export* (Export Skinned GLB)
      Reduce vertical scroll, and stop mixing authoring buttons with viz toggles.

- [ ] **Mirror weights** across an axis. Paint one side of a symmetrical mesh
      and have the other side update.
- [ ] **Smooth weights** brush — average a vertex's weights with its neighbors.
- [ ] **Weight by bone visibility**. Toggle which bones can be picked up by
      Auto Weights so a sub-rig can be re-weighted in isolation.

## Animation authoring

- [x] ~~Rotation tracks in keyframes~~ — done; Set Key now snapshots
      `m_boneWorldRotations` alongside positions, playback slerps both.
- [ ] **Rotation channels in skinned-GLB export**. Now that LIME tracks
      bone rotations through animation, the export should write them as
      glTF `rotation` channels instead of just `translation`. Eden's
      `AnimationPlayer` already loads `target_path = "rotation"`, so the
      runtime side just works once the export does.
- [ ] **Onion skin / ghosting** — show the previous and next keyframe poses
      faintly behind the current pose for timing reference.
- [ ] **Editable curves** — drag the segment between keys in the timeline to
      change the interpolation (ease in/out instead of linear).

## Modeling

- [ ] **Selection hiding** — toggle to hide the currently-selected verts/edges/faces
      from rendering (and from picking) so a complex mesh can be carved up region
      by region without visual clutter. Reverse: "Show All" or "Show Hidden".
      The `m_ctx.hiddenFaces` set already exists — wire it to a hotkey + button
      and add hidden-edge/vertex parallels.

## Retopology

- [ ] **Mid-retop save/resume** — verify whether the manual retopology job survives
      a save+reopen cycle. If not, persist the in-progress retop state (live
      surface ref, placed verts, current edge loops, manifold flags) into the
      `.lime` file under a side block so a long retop session can be split
      across days. Right now we think you'd lose the work — confirm and fix.

## Misc

- [ ] **Save/load animations + bind pose in `.lime`**. Currently in-memory only;
      reopen the file and the rig is back to bind without keys.
- [ ] **Multi-character GLB export** with shared skeletons — for clones.
