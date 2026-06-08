import os

print("[1] Analizando src/GPU3D.cpp para la Fase 4...")
with open("src/GPU3D.cpp", "r") as f:
    lines = f.readlines()

start_idx = -1
end_idx = -1

for i, line in enumerate(lines):
    if line.startswith("void GPU3D::CmdFIFOWrite"):
        start_idx = i
    elif line.startswith("s32 GPU3D::CyclesToRunFor"):
        end_idx = i
        break

if start_idx == -1 or end_idx == -1:
    print("[ERROR] No se pudo encontrar el bloque de comandos FIFO.")
    exit(1)

print("[2] Extrayendo comandos FIFO...")
cmd_code = "".join(lines[start_idx:end_idx])

# Buscamos y extraemos CmdNumParams del archivo original
cmdnum_start = -1
cmdnum_end = -1
for i, line in enumerate(lines):
    if line.startswith("const u8 CmdNumParams[256]"):
        cmdnum_start = i
    elif cmdnum_start != -1 and line.startswith("};"):
        cmdnum_end = i + 1
        break

if cmdnum_start == -1 or cmdnum_end == -1:
    print("[ERROR] No se encontró CmdNumParams.")
    exit(1)

cmdnum_code = "".join(lines[cmdnum_start:cmdnum_end])

print("[3] Modificando GPU3D.cpp central...")
# Eliminamos tanto el bloque de comandos como CmdNumParams
new_gpu3d_lines = lines[:min(start_idx, cmdnum_start)] + lines[max(end_idx, cmdnum_end):]

# Añadimos la declaración extern donde estaba CmdNumParams
# (esto es aproximado, pero funcional)
with open("src/GPU3D.cpp", "w") as f:
    f.writelines(new_gpu3d_lines)

# Insertamos la declaración extern al principio del namespace
with open("src/GPU3D.cpp", "r") as f:
    content = f.read()

content = content.replace(
    "namespace melonDS\n{",
    "namespace melonDS\n{\nusing Platform::Log;\nusing Platform::LogLevel;\n\nextern const u8 CmdNumParams[256];\n"
)

with open("src/GPU3D.cpp", "w") as f:
    f.write(content)

print("[4] Creando src/gpu3d/GPU3D_Cmd.cpp...")
cmd_file_content = """#include <stdio.h>
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

void MatrixLoadIdentity(s32* m);
void MatrixLoad4x4(s32* m, s32* s);
void MatrixLoad4x3(s32* m, s32* s);
void MatrixMult4x4(s32* m, s32* s);
void MatrixMult4x3(s32* m, s32* s);
void MatrixMult3x3(s32* m, s32* s);
void MatrixScale(s32* m, s32* s);
void MatrixTranslate(s32* m, s32* s);

""" + cmdnum_code + """

""" + cmd_code + """
} // namespace melonDS
"""

with open("src/gpu3d/GPU3D_Cmd.cpp", "w") as f:
    f.write(cmd_file_content)

print("[5] Actualizando CMakeLists.txt...")
with open("src/CMakeLists.txt", "r") as f:
    cmake = f.read()

if "gpu3d/GPU3D_Cmd.cpp" not in cmake:
    cmake = cmake.replace("    gpu3d/GPU3D_Geometry.cpp", "    gpu3d/GPU3D_Cmd.cpp\n    gpu3d/GPU3D_Geometry.cpp")
    with open("src/CMakeLists.txt", "w") as f:
        f.write(cmake)

print("[EXITO] Fase 4 completada.")
