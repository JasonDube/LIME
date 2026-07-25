#!/bin/bash
# Launch TRELLIS.2 image-to-3D API server for LIME Editor integration (port 8083).
# Protocol-compatible with the Hunyuan3D server, so LIME's Hunyuan3DClient works
# unchanged — just point it at 8083.
# Usage: ./launch_trellis.sh            (1024_cascade, quality)
#        TRELLIS_PIPELINE_TYPE=512 ./launch_trellis.sh   (lighter/faster)
export TRELLIS_REMESH="${TRELLIS_REMESH:-0}"
export TRELLIS_TEX_SIZE="${TRELLIS_TEX_SIZE:-1024}"
source ~/miniconda3/etc/profile.d/conda.sh
conda activate trellis2
cd ~/Desktop/TRELLIS.2
export ATTN_BACKEND=xformers          # xformers backend (no flash-attn build needed)
export TRELLIS_PORT=8083
export TRELLIS_PIPELINE_TYPE="${TRELLIS_PIPELINE_TYPE:-512}"
export TRELLIS_REMESH="${TRELLIS_REMESH:-0}"
export TRELLIS_TEX_SIZE="${TRELLIS_TEX_SIZE:-1024}"
export PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True
python api_server.py
