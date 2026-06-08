import os

print("[1] Analizando src/GPU3D.cpp...")
with open("src/GPU3D.cpp", "r") as f:
    lines = f.readlines()

start_idx = -1
for i, line in enumerate(lines):
    if line.startswith("void GPU3D::WriteToGXFIFO"):
        start_idx = i
        break

if start_idx == -1:
    print("[ERROR] No se encontró WriteToGXFIFO.")
    exit(1)

end_idx = len(lines) - 1
while end_idx > start_idx and not lines[end_idx].startswith("}"):
    end_idx -= 1

io_code = "".join(lines[start_idx:end_idx])

print("[2] Extrayendo IO...")
new_gpu3d_lines = lines[:start_idx] + lines[end_idx:]

# ⚠️ NO TOQUES CmdNumParams AQUÍ. fase4.py se encargará de moverlo.

with open("src/GPU3D.cpp", "w") as f:
    f.writelines(new_gpu3d_lines)

print("[3] Creando src/gpu3d/GPU3D_IO.cpp...")
io_file_content = """#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "NDS.h"
#include "GPU.h"
#include "FIFO.h"
#include "GPU3D_Soft.h"
#include "Platform.h"
#include "GPU3D.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

extern const u8 CmdNumParams[256];

""" + io_code + """
} // namespace melonDS
"""
os.makedirs("src/gpu3d", exist_ok=True)
with open("src/gpu3d/GPU3D_IO.cpp", "w") as f:
    f.write(io_file_content)

print("[4] Actualizando CMakeLists.txt...")
with open("src/CMakeLists.txt", "r") as f:
    cmake = f.read()

if "gpu3d/GPU3D_IO.cpp" not in cmake:
    cmake = cmake.replace("    gpu3d/GPU3D_Math.cpp", "    gpu3d/GPU3D_Math.cpp\n    gpu3d/GPU3D_IO.cpp")
    with open("src/CMakeLists.txt", "w") as f:
        f.write(cmake)

print("[EXITO] Fase 2 completada.")
