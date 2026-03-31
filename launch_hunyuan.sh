#!/bin/bash
# Launch Hunyuan3D-2 API server for LIME Editor integration
# Texture pipeline is loaded on-demand per generation (fits in 12GB VRAM)
# Usage: ./launch_hunyuan.sh          (full model)
#        ./launch_hunyuan.sh --low     (mini model + CPU offload)
cd ~/Desktop/hunyuan3d2/Hunyuan3D-2
source .venv/bin/activate

if [ "$1" = "--low" ]; then
    python api_server.py --model_path tencent/Hunyuan3D-2mini --subfolder hunyuan3d-dit-v2-mini-turbo --port 8081 --enable_tex --low_vram
else
    python api_server.py --model_path tencent/Hunyuan3D-2 --subfolder hunyuan3d-dit-v2-0-turbo --port 8081 --enable_tex
fi
