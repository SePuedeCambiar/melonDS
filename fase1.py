import os
import subprocess

print("[1] Restaurando archivos limpios de fábrica...")
subprocess.run(["git", "checkout", "origin/master", "--", "src/GPU3D.cpp", "src/CMakeLists.txt"])

os.makedirs("src/gpu3d", exist_ok=True)

with open("src/GPU3D.cpp", "r") as f:
    lines = f.readlines()

start_idx = -1
end_idx = -1
for i, line in enumerate(lines):
    # Buscamos la DEFINICIÓN matemática, no la declaración superior
    if line.startswith("void MatrixLoadIdentity(s32* m)"):
        if i + 1 < len(lines) and "{" in lines[i+1]:
            start_idx = i
    elif line.startswith("void GPU3D::UpdateClipMatrix()"):
        end_idx = i
        break

if start_idx == -1 or end_idx == -1:
    print("[ERROR] No se encontraron las funciones matemáticas.")
    exit(1)

print("[2] Extrayendo código matemático...")
math_code = "".join(lines[start_idx:end_idx])

# Inyectamos las firmas para que el archivo original no pierda la referencia
declarations = (
    "void MatrixLoadIdentity(s32* m);\n"
    "void MatrixLoad4x4(s32* m, s32* s);\n"
    "void MatrixLoad4x3(s32* m, s32* s);\n"
    "void MatrixMult4x4(s32* m, s32* s);\n"
    "void MatrixMult4x3(s32* m, s32* s);\n"
    "void MatrixMult3x3(s32* m, s32* s);\n"
    "void MatrixScale(s32* m, s32* s);\n"
    "void MatrixTranslate(s32* m, s32* s);\n\n"
)

new_gpu3d = lines[:start_idx] + [declarations] + lines[end_idx:]

with open("src/GPU3D.cpp", "w") as f:
    f.writelines(new_gpu3d)

print("[3] Creando src/gpu3d/GPU3D_Math.cpp...")
math_file_content = """#include <stdio.h>
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

""" + math_code + """
} // namespace melonDS
"""
with open("src/gpu3d/GPU3D_Math.cpp", "w") as f:
    f.write(math_file_content)

print("[4] Actualizando CMakeLists.txt...")
with open("src/CMakeLists.txt", "r") as f:
    cmake = f.read()

cmake = cmake.replace("    GPU3D.cpp", "    GPU3D.cpp\n    gpu3d/GPU3D_Math.cpp")
cmake = cmake.replace('INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"', 'PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"')

with open("src/CMakeLists.txt", "w") as f:
    f.write(cmake)

print("[EXITO] Fase 1 completada. ¡Listo para compilar!")
