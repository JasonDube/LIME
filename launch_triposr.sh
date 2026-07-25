#!/bin/bash
# Launch TripoSR image-to-3D API server for LIME (port 8084).
# Fast lane: single image -> vertex-colored GLB in seconds. Protocol-compatible
# with the Hunyuan3D client, so LIME's Hunyuan3DClient works unchanged.
source ~/miniconda3/etc/profile.d/conda.sh
conda activate triposr
cd ~/Desktop/triposr
export TRIPOSR_PORT=8084
export PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True
python api_server.py
