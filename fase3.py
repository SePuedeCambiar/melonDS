import os

print("[1] Analizando src/GPU3D.cpp...")
with open("src/GPU3D.cpp", "r") as f:
    lines = f.readlines()

start_idx = -1
end_idx = -1

for i, line in enumerate(lines):
    if line.startswith("template<int comp, s32 plane"):
        start_idx = i
    elif line.startswith("void GPU3D::CmdFIFOWrite"):
        end_idx = i
        break

if start_idx == -1 or end_idx == -1:
    print("[ERROR] No se pudo encontrar el bloque geométrico.")
    exit(1)

print("[2] Extrayendo geometría y clipping de forma quirúrgica...")
geom_code = "".join(lines[start_idx:end_idx])

print("[3] Modificando GPU3D.cpp central...")
new_gpu3d_lines = lines[:start_idx] + lines[end_idx:]

with open("src/GPU3D.cpp", "w") as f:
    f.writelines(new_gpu3d_lines)

print("[4] Creando src/gpu3d/GPU3D_Geometry.cpp...")
geom_file_content = """#include <stdio.h>
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

""" + geom_code + """
} // namespace melonDS
"""
with open("src/gpu3d/GPU3D_Geometry.cpp", "w") as f:
    f.write(geom_file_content)

print("[5] Actualizando CMakeLists.txt...")
with open("src/CMakeLists.txt", "r") as f:
    cmake = f.read()

if "gpu3d/GPU3D_Geometry.cpp" not in cmake:
    cmake = cmake.replace("    gpu3d/GPU3D_IO.cpp", "    gpu3d/GPU3D_Geometry.cpp\n    gpu3d/GPU3D_IO.cpp")
    with open("src/CMakeLists.txt", "w") as f:
        f.write(cmake)

print("[EXITO] Fase 3 completada. ¡Listo para compilar!")
